# -*- coding: utf-8 -*-
"""
图形化编译工具（Compile Tool）
================================
一个基于 PySide6 的编译工具，封装任意 C/C++ 编译命令：

  - 实时显示编译输出（识别 error / warning / 文件位置）
  - 总耗时统计（毫秒级）
  - 进度条（按编译步骤 / 文件计数）
  - 保存日志到 .log 和 .json 文件
  - 命令行模式（headless）供 AI 直接调用
  - 项目预设 + JSON 配置文件

用法：
    python compile_tool.py                       # 启动 GUI
    python compile_tool.py gui                   # 启动 GUI（同上）
    python compile_tool.py run --cmd "g++ ..."   # CLI 单次执行
    python compile_tool.py run --preset <name>   # 用预设执行
    python compile_tool.py presets               # 列出所有预设
    python compile_tool.py add-preset <name> ... # 添加预设
"""

import sys
import os
import re
import json
import time
import shlex
import signal
import argparse
import subprocess
from pathlib import Path
from typing import Dict, List, Optional, Callable
from dataclasses import dataclass, field, asdict
from datetime import datetime

# ===== 常量 =====
APP_DIR = Path(__file__).parent
CONFIG_FILE = APP_DIR / "compile_presets.json"
LOG_DIR = APP_DIR / "build_logs"

# 暗色系（与 step_tracker 风格统一）
COLOR_BG = "#1e1e2e"
COLOR_PANEL = "#2a2a3e"
COLOR_ACCENT = "#0d9488"
COLOR_TEXT = "#e0e0e0"
COLOR_ERROR = "#ef4444"
COLOR_WARNING = "#f59e0b"
COLOR_SUCCESS = "#10b981"
COLOR_INFO = "#94a3b8"


# ===== 数据模型 =====
@dataclass
class CompileMessage:
    """一条编译消息（错误/警告/信息）。"""
    kind: str          # error / warning / info / raw
    file: str = ""     # 出处文件
    line: int = 0      # 行号
    column: int = 0    # 列号
    text: str = ""     # 消息内容
    raw: str = ""      # 原始行
    time_ms: int = 0   # 相对开始时间（毫秒）

    def to_dict(self) -> Dict:
        return asdict(self)


@dataclass
class CompileResult:
    """编译的最终结果。"""
    success: bool
    exit_code: int
    duration_ms: int
    total_lines: int
    errors: int
    warnings: int
    files_seen: List[str]
    messages: List[CompileMessage]
    command: str
    cwd: str
    log_file: Optional[str] = None
    json_file: Optional[str] = None
    started_at: str = ""
    finished_at: str = ""


# ===== 输出解析器 =====
class OutputParser:
    """解析编译器的输出，识别 error/warning/文件位置等。

    支持常见的编译输出格式：
      - GCC / Clang:  file.cpp:10:5: error: ...
      - MSVC:         file.cpp(10): error C2065: ...
      - CMake:        -- Configuring done / -- Build files have been written
      - 通用:         [12/34] Building CXX object ...
    """

    # GCC/Clang 风格: file:line:col: severity: message
    GCC_RE = re.compile(
        r"^(?P<file>[^:\n]+?):(?P<line>\d+):(?P<col>\d+):\s*"
        r"(?:\x1b\[[0-9;]*m)?(?P<kind>fatal error|error|warning|note|info)(?:\x1b\[[0-9;]*m)?:\s*(?P<text>.*)$"
    )
    # MSVC 风格: file(line[,col]) : severity Cxxxx: message
    # 或: file(line) : error C2065: ...
    MSVC_RE = re.compile(
        r"^(?P<file>[^(\n]+)\((?P<line>\d+)(?:,(?P<col>\d+))?\)\s*:\s*"
        r"(?P<kind>fatal error|error|warning|note|message)\s+(?P<text>.*)$"
    )
    # CMake 进度: [12/34] Building CXX object ...
    CMAKE_PROGRESS_RE = re.compile(
        r"^\[(?P<current>\d+)/(?P<total>\d+)\]\s+(?P<text>.*)$"
    )
    # 简单行内 error/warning 检测（兜底）
    SIMPLE_RE = re.compile(
        r"\b(error|warning|fatal error)\b", re.IGNORECASE
    )

    def __init__(self):
        """初始化解析器。"""
        self.files_seen: List[str] = []
        self.errors = 0
        self.warnings = 0
        self.current_step = 0
        self.total_steps = 0

    def parse(self, line: str, elapsed_ms: int) -> CompileMessage:
        """解析一行输出，返回 CompileMessage。"""
        raw = line.rstrip("\r\n")
        # 1. 移除 ANSI 转义（颜色码等）
        clean = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", raw)

        # 2. GCC / Clang 风格
        m = self.GCC_RE.match(clean)
        if m:
            kind = m.group("kind").strip()
            msg_kind = self._map_kind(kind)
            self._bump(msg_kind, m.group("file"))
            return CompileMessage(
                kind=msg_kind,
                file=m.group("file"),
                line=int(m.group("line")),
                column=int(m.group("col")),
                text=m.group("text").strip(),
                raw=raw,
                time_ms=elapsed_ms,
            )

        # 3. MSVC 风格
        m = self.MSVC_RE.match(clean)
        if m:
            kind = m.group("kind").strip()
            msg_kind = self._map_kind(kind)
            self._bump(msg_kind, m.group("file"))
            return CompileMessage(
                kind=msg_kind,
                file=m.group("file").strip(),
                line=int(m.group("line")),
                column=int(m.group("col") or 0),
                text=m.group("text").strip(),
                raw=raw,
                time_ms=elapsed_ms,
            )

        # 4. CMake 进度行
        m = self.CMAKE_PROGRESS_RE.match(clean)
        if m:
            cur, total = int(m.group("current")), int(m.group("total"))
            self.current_step = cur
            self.total_steps = total
            return CompileMessage(
                kind="info",
                text=f"[{cur}/{total}] {m.group('text').strip()}",
                raw=raw,
                time_ms=elapsed_ms,
            )

        # 5. 兜底：粗略判断
        if self.SIMPLE_RE.search(clean):
            if "error" in clean.lower():
                self.errors += 1
                return CompileMessage(kind="error", text=clean, raw=raw, time_ms=elapsed_ms)
            if "warning" in clean.lower():
                self.warnings += 1
                return CompileMessage(kind="warning", text=clean, raw=raw, time_ms=elapsed_ms)

        # 6. 普通信息
        return CompileMessage(kind="info", text=clean, raw=raw, time_ms=elapsed_ms)

    @staticmethod
    def _map_kind(kind: str) -> str:
        """把编译器关键字映射到统一分类。"""
        k = kind.lower()
        if "error" in k:
            return "error"
        if "warning" in k:
            return "warning"
        if "note" in k:
            return "note"
        return "info"

    def _bump(self, kind: str, file_path: str) -> None:
        """统计错误/警告数量 + 记录文件。"""
        if kind == "error":
            self.errors += 1
        elif kind == "warning":
            self.warnings += 1
        if file_path and file_path not in self.files_seen:
            # 只保留文件名（避免长绝对路径）
            short = os.path.basename(file_path)
            if short:
                self.files_seen.append(short)


# ===== 编译执行器 =====
class CompileRunner:
    """执行编译命令并实时回调输出。

    用法：
        runner = CompileRunner(["g++", "-c", "main.cpp", "-o", "main.o"], cwd="/path")
        runner.on_message = lambda msg: ...
        runner.on_finish = lambda result: ...
        runner.start()
    """

    def __init__(self, cmd: List[str], cwd: str = "", env: Optional[Dict[str, str]] = None):
        """初始化：cmd 必须是列表（已 shlex 拆分）。"""
        if isinstance(cmd, str):
            cmd = shlex.split(cmd)
        self.cmd = cmd
        self.cwd = cwd or os.getcwd()
        self.env = env
        self.parser = OutputParser()
        self.process: Optional[subprocess.Popen] = None
        self._start_time: float = 0.0
        self._stopped = False
        self.messages: List[CompileMessage] = []
        self._all_raw: List[str] = []  # 完整原始输出

        # 回调（外部设置）
        self.on_message: Optional[Callable[[CompileMessage], None]] = None
        self.on_finish: Optional[Callable[[CompileResult], None]] = None
        self.on_progress: Optional[Callable[[int, int], None]] = None  # cur, total

    def start(self) -> None:
        """启动编译（后台线程读输出 + 等待结束）。"""
        import threading
        self._start_time = time.time()
        self._stopped = False
        self.messages.clear()
        self._all_raw.clear()

        try:
            # Windows 下 CREATE_NEW_PROCESS_GROUP 便于终止
            kwargs = {
                "cwd": self.cwd,
                "stdout": subprocess.PIPE,
                "stderr": subprocess.STDOUT,  # 合并
                "bufsize": 1,  # 行缓冲
                "universal_newlines": True,  # 文本模式
                "encoding": "utf-8",
                "errors": "replace",
            }
            if os.name == "nt":
                kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
            if self.env is not None:
                kwargs["env"] = self.env

            self.process = subprocess.Popen(self.cmd, **kwargs)
        except FileNotFoundError as e:
            msg = CompileMessage(kind="error", text=f"无法启动命令: {e}", raw=str(e),
                                 time_ms=0)
            self.messages.append(msg)
            if self.on_message:
                self.on_message(msg)
            self._finish(-1)
            return
        except Exception as e:
            msg = CompileMessage(kind="error", text=f"启动失败: {e}", raw=str(e),
                                 time_ms=0)
            self.messages.append(msg)
            if self.on_message:
                self.on_message(msg)
            self._finish(-1)
            return

        # 启动读取线程
        t = threading.Thread(target=self._read_loop, daemon=True)
        t.start()
        # 启动等待线程
        t2 = threading.Thread(target=self._wait_loop, daemon=True)
        t2.start()

    def _read_loop(self) -> None:
        """持续读取子进程输出，逐行解析。"""
        if not self.process or not self.process.stdout:
            return
        try:
            for line in self.process.stdout:
                if self._stopped:
                    break
                self._all_raw.append(line)
                elapsed_ms = int((time.time() - self._start_time) * 1000)
                msg = self.parser.parse(line, elapsed_ms)
                self.messages.append(msg)
                if self.on_message:
                    try:
                        self.on_message(msg)
                    except Exception:
                        pass
                # 进度变化
                if self.parser.total_steps > 0:
                    if self.on_progress:
                        try:
                            self.on_progress(self.parser.current_step, self.parser.total_steps)
                        except Exception:
                            pass
        except Exception as e:
            err = CompileMessage(kind="error", text=f"读取输出失败: {e}",
                                 raw=str(e), time_ms=int((time.time() - self._start_time) * 1000))
            self.messages.append(err)
            if self.on_message:
                self.on_message(err)

    def _wait_loop(self) -> None:
        """等待子进程结束并触发 finish 回调。"""
        if not self.process:
            return
        try:
            self.process.wait()
        except Exception:
            pass
        # 等读取线程结束
        time.sleep(0.1)
        self._finish(self.process.returncode if self.process else -1)

    def _finish(self, exit_code: int) -> None:
        """结束处理。"""
        self._stopped = True
        duration_ms = int((time.time() - self._start_time) * 1000)
        result = CompileResult(
            success=(exit_code == 0),
            exit_code=exit_code,
            duration_ms=duration_ms,
            total_lines=len(self.messages),
            errors=self.parser.errors,
            warnings=self.parser.warnings,
            files_seen=list(self.parser.files_seen),
            messages=list(self.messages),
            command=" ".join(self.cmd) if isinstance(self.cmd, list) else str(self.cmd),
            cwd=self.cwd,
            started_at=datetime.fromtimestamp(self._start_time).isoformat(timespec="seconds"),
            finished_at=datetime.now().isoformat(timespec="seconds"),
        )
        if self.on_finish:
            try:
                self.on_finish(result)
            except Exception:
                pass

    def stop(self) -> None:
        """主动停止编译。"""
        self._stopped = True
        if self.process and self.process.poll() is None:
            try:
                if os.name == "nt":
                    # Windows: 用 CTRL_BREAK 优雅终止，兜底 kill
                    self.process.send_signal(signal.CTRL_BREAK_EVENT)
                else:
                    self.process.terminate()
            except Exception:
                try:
                    self.process.kill()
                except Exception:
                    pass


# ===== 预设管理 =====
class PresetManager:
    """管理项目预设（JSON 文件）。"""

    def __init__(self, config_file: Path = CONFIG_FILE):
        """加载或初始化配置文件。"""
        self.config_file = config_file
        self.data: Dict = {}
        self.load()

    def load(self) -> None:
        """加载配置文件，不存在则创建默认。"""
        if self.config_file.exists():
            try:
                with open(self.config_file, "r", encoding="utf-8") as f:
                    self.data = json.load(f)
            except (json.JSONDecodeError, OSError):
                self.data = self._default()
        else:
            self.data = self._default()
            self.save()

    def save(self) -> None:
        """保存到文件。"""
        self.config_file.parent.mkdir(parents=True, exist_ok=True)
        with open(self.config_file, "w", encoding="utf-8") as f:
            json.dump(self.data, f, ensure_ascii=False, indent=2)

    @staticmethod
    def _default() -> Dict:
        """默认配置：包含几个示例预设。"""
        return {
            "presets": {
                "hello-g++": {
                    "name": "hello-g++",
                    "command": "g++ -std=c++17 -Wall -Wextra -O2 main.cpp -o main.exe",
                    "cwd": ".",
                    "description": "示例：用 g++ 编译 main.cpp",
                },
                "hello-cl": {
                    "name": "hello-cl",
                    "command": "cl /nologo /EHsc /O2 /W3 main.cpp /Fe:main.exe",
                    "cwd": ".",
                    "description": "示例：用 MSVC cl.exe 编译 main.cpp",
                },
                "cmake-build": {
                    "name": "cmake-build",
                    "command": "cmake --build build --config Release",
                    "cwd": ".",
                    "description": "示例：使用 CMake 构建 build 目录",
                },
            },
            "last_preset": "hello-g++",
        }

    def list_presets(self) -> List[Dict]:
        """返回所有预设列表。"""
        return list(self.data.get("presets", {}).values())

    def get_preset(self, name: str) -> Optional[Dict]:
        """按名称获取预设。"""
        return self.data.get("presets", {}).get(name)

    def add_preset(self, name: str, command: str, cwd: str = ".",
                   description: str = "") -> Dict:
        """添加 / 覆盖预设。"""
        if "presets" not in self.data:
            self.data["presets"] = {}
        preset = {
            "name": name,
            "command": command,
            "cwd": cwd,
            "description": description,
        }
        self.data["presets"][name] = preset
        self.save()
        return preset

    def delete_preset(self, name: str) -> bool:
        """删除预设。"""
        if name in self.data.get("presets", {}):
            del self.data["presets"][name]
            self.save()
            return True
        return False

    def get_last_preset(self) -> Optional[Dict]:
        """返回上次使用的预设。"""
        name = self.data.get("last_preset")
        if name:
            return self.get_preset(name)
        presets = self.list_presets()
        return presets[0] if presets else None


# ===== 日志保存 =====
def save_log(result: CompileResult, log_dir: Path = LOG_DIR) -> tuple:
    """把编译结果保存到文件，返回 (log_path, json_path)。"""
    log_dir.mkdir(parents=True, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    base = log_dir / f"build_{ts}"
    log_path = base.with_suffix(".log")
    json_path = base.with_suffix(".json")

    # 文本日志
    with open(log_path, "w", encoding="utf-8") as f:
        f.write(f"命令: {result.command}\n")
        f.write(f"工作目录: {result.cwd}\n")
        f.write(f"开始: {result.started_at}\n")
        f.write(f"结束: {result.finished_at}\n")
        f.write(f"耗时: {result.duration_ms} ms\n")
        f.write(f"退出码: {result.exit_code}\n")
        f.write(f"成功: {result.success}\n")
        f.write(f"错误数: {result.errors}\n")
        f.write(f"警告数: {result.warnings}\n")
        f.write(f"涉及文件: {', '.join(result.files_seen) or '（无）'}\n")
        f.write("=" * 80 + "\n")
        for msg in result.messages:
            f.write(msg.raw + "\n")

    # JSON 结构化
    payload = asdict(result)
    # 把 messages 转 dict
    payload["messages"] = [m if isinstance(m, dict) else asdict(m) for m in result.messages]
    payload["log_file"] = str(log_path)
    payload["json_file"] = str(json_path)
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)

    result.log_file = str(log_path)
    result.json_file = str(json_path)
    return str(log_path), str(json_path)


# ===== GUI 部分 =====
def run_gui() -> None:
    """启动 PySide6 图形界面。"""
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen") if "--offscreen" in sys.argv else None
    from PySide6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QLabel, QLineEdit, QTextEdit, QPushButton, QListWidget, QListWidgetItem,
        QProgressBar, QComboBox, QFileDialog, QMessageBox, QSplitter,
        QStatusBar, QToolBar, QPlainTextEdit, QTabWidget
    )
    from PySide6.QtCore import Qt, QTimer, Signal, QObject
    from PySide6.QtGui import QColor, QFont, QAction, QTextCursor, QKeySequence, QShortcut

    class SignalBus(QObject):
        """跨线程信号总线。"""
        message_received = Signal(object)
        progress_updated = Signal(int, int)
        finished = Signal(object)

    class MainWindow(QMainWindow):
        """主窗口。"""

        def __init__(self):
            super().__init__()
            self.presets = PresetManager()
            self.bus = SignalBus()
            self.runner: Optional[CompileRunner] = None
            self.current_messages: List[CompileMessage] = []
            self.current_result: Optional[CompileResult] = None
            self._start_ts: float = 0.0
            self._elapsed_timer: Optional[QTimer] = None

            self.setWindowTitle("图形化编译工具 - Compile Tool")
            self.resize(1280, 800)
            self.setStyleSheet(self._qss())

            self._build_ui()
            self._load_presets_to_combo()
            self._apply_last_preset()

        def _qss(self) -> str:
            """全局样式表。"""
            return f"""
                QMainWindow {{ background: {COLOR_BG}; }}
                QLabel {{ color: {COLOR_TEXT}; }}
                QLineEdit, QPlainTextEdit, QListWidget, QComboBox {{
                    background: {COLOR_PANEL}; color: {COLOR_TEXT};
                    border: 1px solid #444; border-radius: 4px; padding: 4px;
                }}
                QPushButton {{
                    background: {COLOR_ACCENT}; color: white; border: none;
                    padding: 6px 12px; border-radius: 4px;
                }}
                QPushButton:hover {{ background: #14b8a6; }}
                QPushButton:disabled {{ background: #555; color: #999; }}
                QProgressBar {{
                    background: {COLOR_PANEL}; border: 1px solid #444;
                    border-radius: 4px; text-align: center; color: {COLOR_TEXT};
                }}
                QProgressBar::chunk {{
                    background: {COLOR_ACCENT};
                }}
                QToolBar {{ background: {COLOR_PANEL}; border: none; spacing: 4px; }}
                QStatusBar {{ background: {COLOR_PANEL}; color: {COLOR_TEXT}; }}
                QTabWidget::pane {{ border: 1px solid #444; background: {COLOR_BG}; }}
                QTabBar::tab {{
                    background: {COLOR_PANEL}; color: {COLOR_TEXT};
                    padding: 6px 14px; border-radius: 4px 4px 0 0;
                }}
                QTabBar::tab:selected {{ background: {COLOR_ACCENT}; color: white; }}
            """

        def _build_ui(self) -> None:
            """构建界面。"""
            # 顶部：预设 + 命令
            top = QWidget()
            top_layout = QVBoxLayout(top)
            top_layout.setContentsMargins(8, 8, 8, 4)
            top_layout.setSpacing(4)

            # 第一行：预设 + 操作
            row1 = QHBoxLayout()
            row1.addWidget(QLabel("预设:"))
            self.preset_combo = QComboBox()
            self.preset_combo.setMinimumWidth(180)
            self.preset_combo.currentIndexChanged.connect(self._on_preset_changed)
            row1.addWidget(self.preset_combo)
            self.btn_new_preset = QPushButton("💾 保存为预设")
            self.btn_new_preset.clicked.connect(self._save_as_preset)
            row1.addWidget(self.btn_new_preset)
            self.btn_delete_preset = QPushButton("🗑 删除预设")
            self.btn_delete_preset.clicked.connect(self._delete_preset)
            row1.addWidget(self.btn_delete_preset)
            row1.addStretch(1)
            top_layout.addLayout(row1)

            # 第二行：命令
            row2 = QHBoxLayout()
            row2.addWidget(QLabel("命令:"))
            self.cmd_edit = QLineEdit()
            self.cmd_edit.setPlaceholderText("例如: g++ -std=c++17 main.cpp -o main.exe")
            self.cmd_edit.returnPressed.connect(self.start_compile)
            row2.addWidget(self.cmd_edit, 1)
            top_layout.addLayout(row2)

            # 第三行：工作目录
            row3 = QHBoxLayout()
            row3.addWidget(QLabel("工作目录:"))
            self.cwd_edit = QLineEdit()
            self.cwd_edit.setPlaceholderText("执行命令的工作目录，留空 = 当前目录")
            row3.addWidget(self.cwd_edit, 1)
            self.btn_browse = QPushButton("📂 浏览…")
            self.btn_browse.clicked.connect(self._browse_cwd)
            row3.addWidget(self.btn_browse)
            top_layout.addLayout(row3)

            # 工具栏
            tb = QToolBar()
            self.addToolBar(tb)
            self.act_start = QAction("▶ 开始编译", self)
            self.act_start.triggered.connect(self.start_compile)
            tb.addAction(self.act_start)
            self.act_stop = QAction("⏹ 停止", self)
            self.act_stop.triggered.connect(self.stop_compile)
            self.act_stop.setEnabled(False)
            tb.addAction(self.act_stop)
            tb.addSeparator()
            self.act_clear = QAction("🧹 清空", self)
            self.act_clear.triggered.connect(self.clear_messages)
            tb.addAction(self.act_clear)
            self.act_save = QAction("💾 保存日志", self)
            self.act_save.triggered.connect(self.save_log_manual)
            tb.addAction(self.act_save)
            tb.addSeparator()
            tb.addAction("❓ 帮助", self.show_help)

            # 进度条
            self.progress = QProgressBar()
            self.progress.setMaximum(0)  # 0 = 忙碌（不确定）
            self.progress.setFormat("等待开始…")
            top_layout.addWidget(self.progress)

            # 主区域：分割（消息列表 + 详情 / 原始输出）
            splitter = QSplitter(Qt.Orientation.Vertical)

            # 消息列表
            self.msg_list = QListWidget()
            self.msg_list.setFont(QFont("Consolas", 10))
            self.msg_list.itemDoubleClicked.connect(self._on_message_double_click)
            splitter.addWidget(self.msg_list)

            # 原始输出
            self.raw_output = QPlainTextEdit()
            self.raw_output.setReadOnly(True)
            self.raw_output.setFont(QFont("Consolas", 9))
            splitter.addWidget(self.raw_output)

            splitter.setSizes([500, 300])

            # 总布局
            central = QWidget()
            v = QVBoxLayout(central)
            v.setContentsMargins(0, 0, 0, 0)
            v.setSpacing(0)
            v.addWidget(top)
            v.addWidget(splitter, 1)
            self.setCentralWidget(central)

            # 状态栏
            self.setStatusBar(QStatusBar())
            self.lbl_status_time = QLabel("耗时: 0.0s")
            self.lbl_status_errors = QLabel("错误: 0")
            self.lbl_status_warnings = QLabel("警告: 0")
            self.lbl_status_lines = QLabel("行数: 0")
            self.statusBar().addPermanentWidget(self.lbl_status_lines)
            self.statusBar().addPermanentWidget(self.lbl_status_warnings)
            self.statusBar().addPermanentWidget(self.lbl_status_errors)
            self.statusBar().addPermanentWidget(self.lbl_status_time)
            self.statusBar().showMessage("就绪 · 配置命令后点击 ▶ 开始")

            # 信号连接
            self.bus.message_received.connect(self._on_message)
            self.bus.progress_updated.connect(self._on_progress)
            self.bus.finished.connect(self._on_finished)

            # 快捷键
            QShortcut(QKeySequence("Ctrl+R"), self, activated=self.start_compile)
            QShortcut(QKeySequence("Ctrl+S"), self, activated=self.save_log_manual)
            QShortcut(QKeySequence("Ctrl+L"), self, activated=self.clear_messages)
            QShortcut(QKeySequence("Ctrl+K"), self, activated=self.stop_compile)

        # ---- 预设 ----
        def _load_presets_to_combo(self) -> None:
            """把预设填到下拉框。"""
            self.preset_combo.blockSignals(True)
            self.preset_combo.clear()
            for p in self.presets.list_presets():
                self.preset_combo.addItem(p["name"], p)
            self.preset_combo.blockSignals(False)

        def _apply_last_preset(self) -> None:
            """应用上次使用的预设。"""
            last = self.presets.get_last_preset()
            if last:
                self._apply_preset(last)

        def _on_preset_changed(self, idx: int) -> None:
            """下拉框切换时回填。"""
            data = self.preset_combo.itemData(idx)
            if data:
                self._apply_preset(data)
                self.presets.data["last_preset"] = data["name"]
                self.presets.save()

        def _apply_preset(self, p: Dict) -> None:
            """把预设填到 UI。"""
            self.cmd_edit.setText(p.get("command", ""))
            self.cwd_edit.setText(p.get("cwd", ""))
            idx = self.preset_combo.findText(p.get("name", ""))
            if idx >= 0:
                self.preset_combo.blockSignals(True)
                self.preset_combo.setCurrentIndex(idx)
                self.preset_combo.blockSignals(False)

        def _save_as_preset(self) -> None:
            """把当前命令保存为新预设。"""
            name, ok = QInputDialog.getText(self, "保存预设", "预设名称:")
            if not ok or not name.strip():
                return
            self.presets.add_preset(
                name.strip(),
                self.cmd_edit.text(),
                self.cwd_edit.text() or ".",
                description="",
            )
            self._load_presets_to_combo()
            self.statusBar().showMessage(f"✓ 已保存预设 {name}", 3000)

        def _delete_preset(self) -> None:
            """删除当前预设。"""
            name = self.preset_combo.currentText()
            if not name:
                return
            ret = QMessageBox.question(self, "确认", f"删除预设 {name}？")
            if ret == QMessageBox.StandardButton.Yes:
                self.presets.delete_preset(name)
                self._load_presets_to_combo()
                self.statusBar().showMessage(f"已删除预设 {name}", 3000)

        def _browse_cwd(self) -> None:
            """选择工作目录。"""
            d = QFileDialog.getExistingDirectory(self, "选择工作目录", self.cwd_edit.text())
            if d:
                self.cwd_edit.setText(d)

        # ---- 编译控制 ----
        def start_compile(self) -> None:
            """开始编译。"""
            cmd_str = self.cmd_edit.text().strip()
            if not cmd_str:
                QMessageBox.warning(self, "提示", "请先输入编译命令")
                return
            self.clear_messages()
            cwd = self.cwd_edit.text().strip() or os.getcwd()
            try:
                cmd = shlex.split(cmd_str)
            except ValueError as e:
                QMessageBox.warning(self, "错误", f"命令解析失败: {e}")
                return

            self.runner = CompileRunner(cmd, cwd=cwd)
            self.runner.on_message = self._on_message_thread
            self.runner.on_progress = self._on_progress_thread
            self.runner.on_finish = self._on_finish_thread
            self.runner.start()

            self._start_ts = time.time()
            self._elapsed_timer = QTimer(self)
            self._elapsed_timer.timeout.connect(self._update_elapsed)
            self._elapsed_timer.start(100)  # 100ms 刷新

            self.act_start.setEnabled(False)
            self.act_stop.setEnabled(True)
            self.progress.setRange(0, 0)  # 忙碌
            self.progress.setFormat("编译中…")
            self.statusBar().showMessage(f"▶ 编译中: {cmd_str}")

        def stop_compile(self) -> None:
            """停止编译。"""
            if self.runner:
                self.runner.stop()
                self.statusBar().showMessage("⏹ 已请求停止", 3000)

        def clear_messages(self) -> None:
            """清空消息和输出。"""
            self.msg_list.clear()
            self.raw_output.clear()
            self.current_messages.clear()
            self.current_result = None
            self.lbl_status_errors.setText("错误: 0")
            self.lbl_status_warnings.setText("警告: 0")
            self.lbl_status_lines.setText("行数: 0")
            self.lbl_status_time.setText("耗时: 0.0s")
            self.progress.setValue(0)
            self.progress.setMaximum(0)
            self.progress.setFormat("等待开始…")

        def save_log_manual(self) -> None:
            """手动保存日志。"""
            if not self.current_result:
                QMessageBox.information(self, "提示", "当前没有可保存的编译结果")
                return
            try:
                log, js = save_log(self.current_result)
                self.statusBar().showMessage(f"✓ 日志已保存: {log}", 5000)
                QMessageBox.information(self, "保存成功", f"日志: {log}\nJSON: {js}")
            except Exception as e:
                QMessageBox.critical(self, "错误", f"保存失败: {e}")

        # ---- 跨线程回调（用 Signal 桥接到 UI 线程） ----
        def _on_message_thread(self, msg: CompileMessage) -> None:
            self.bus.message_received.emit(msg)

        def _on_progress_thread(self, cur: int, total: int) -> None:
            self.bus.progress_updated.emit(cur, total)

        def _on_finish_thread(self, result: CompileResult) -> None:
            self.bus.finished.emit(result)

        # ---- UI 线程槽 ----
        def _on_message(self, msg: CompileMessage) -> None:
            """收到一条新消息。"""
            self.current_messages.append(msg)
            item = QListWidgetItem(self._format_message(msg))
            color = self._color_for(msg.kind)
            item.setForeground(QColor(color))
            self.msg_list.addItem(item)
            # 滚动到底
            self.msg_list.scrollToBottom()
            # 原始输出
            self.raw_output.appendPlainText(msg.raw)
            # 状态栏
            self.lbl_status_lines.setText(f"行数: {len(self.current_messages)}")
            if msg.kind == "error":
                self.lbl_status_errors.setText(f"错误: {sum(1 for m in self.current_messages if m.kind == 'error')}")
            elif msg.kind == "warning":
                self.lbl_status_warnings.setText(f"警告: {sum(1 for m in self.current_messages if m.kind == 'warning')}")

        def _on_progress(self, cur: int, total: int) -> None:
            """进度更新。"""
            if total > 0:
                self.progress.setRange(0, total)
                self.progress.setValue(cur)
                self.progress.setFormat(f"编译中… {cur}/{total} ({cur*100//total}%)")

        def _on_finished(self, result: CompileResult) -> None:
            """编译结束。"""
            self.current_result = result
            self.act_start.setEnabled(True)
            self.act_stop.setEnabled(False)
            if self._elapsed_timer:
                self._elapsed_timer.stop()
            self._update_elapsed()
            self.progress.setRange(0, 100)
            self.progress.setValue(100 if result.success else 100)
            self.progress.setFormat("✓ 成功" if result.success else f"✗ 失败 (退出码 {result.exit_code})")
            if result.success:
                self.progress.setStyleSheet(f"QProgressBar::chunk {{ background: {COLOR_SUCCESS}; }}")
            else:
                self.progress.setStyleSheet(f"QProgressBar::chunk {{ background: {COLOR_ERROR}; }}")
            # 自动保存
            try:
                log, js = save_log(result)
                self.statusBar().showMessage(
                    f"{'✓' if result.success else '✗'} 编译结束 · 耗时 {result.duration_ms}ms · "
                    f"错误 {result.errors} · 警告 {result.warnings} · 日志: {log}", 8000
                )
            except Exception as e:
                self.statusBar().showMessage(f"编译结束，但日志保存失败: {e}", 8000)

        def _update_elapsed(self) -> None:
            """更新耗时显示。"""
            if self._start_ts > 0:
                sec = time.time() - self._start_ts
                self.lbl_status_time.setText(f"耗时: {sec:.1f}s")

        def _on_message_double_click(self, item: QListWidgetItem) -> None:
            """双击消息：尝试在文件管理器中打开或复制内容。"""
            idx = self.msg_list.row(item)
            if 0 <= idx < len(self.current_messages):
                msg = self.current_messages[idx]
                if msg.file:
                    self.statusBar().showMessage(
                        f"📄 {msg.file}:{msg.line}:{msg.column}  {msg.text}", 5000
                    )
                    QApplication.clipboard().setText(f"{msg.file}:{msg.line}:{msg.column}: {msg.text}")
                else:
                    QApplication.clipboard().setText(msg.text)

        def show_help(self) -> None:
            """显示帮助。"""
            QMessageBox.information(self, "用法",
                "快捷键:\n"
                "  Ctrl+R  开始编译\n"
                "  Ctrl+K  停止\n"
                "  Ctrl+L  清空\n"
                "  Ctrl+S  保存日志\n\n"
                "双击消息行：复制 file:line:col 到剪贴板\n\n"
                "日志自动保存到 build_logs/ 目录\n"
                "配置保存在 compile_presets.json\n\n"
                "CLI 用法:\n"
                "  python compile_tool.py run --cmd \"g++ main.cpp\"\n"
                "  python compile_tool.py run --preset hello-g++\n"
                "  python compile_tool.py presets")

        @staticmethod
        def _format_message(msg: CompileMessage) -> str:
            """格式化消息用于列表显示。"""
            prefix = {"error": "❌", "warning": "⚠", "note": "ℹ", "info": "  "}.get(msg.kind, "  ")
            time_s = msg.time_ms / 1000
            if msg.file:
                loc = f"{msg.file}:{msg.line}:{msg.column}" if msg.line else msg.file
                return f"{prefix} [{time_s:6.2f}s] {loc}  {msg.text}"
            return f"{prefix} [{time_s:6.2f}s] {msg.text}"

        @staticmethod
        def _color_for(kind: str) -> str:
            return {
                "error": COLOR_ERROR,
                "warning": COLOR_WARNING,
                "note": "#60a5fa",
                "info": COLOR_INFO,
            }.get(kind, COLOR_TEXT)

    # 需要 QInputDialog
    from PySide6.QtWidgets import QInputDialog
    QInputDialog  # 防止未使用警告

    app = QApplication.instance() or QApplication(sys.argv)
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


# ===== CLI 部分 =====
def run_cli(args: List[str]) -> int:
    """处理命令行调用。"""
    parser = argparse.ArgumentParser(
        prog="compile_tool.py",
        description="图形化编译工具 CLI - 供 AI 或脚本调用",
    )
    sub = parser.add_subparsers(dest="cmd")

    sub.add_parser("gui", help="启动图形界面")
    sub.add_parser("presets", help="列出所有预设")

    p_run = sub.add_parser("run", help="执行一次编译")
    p_run.add_argument("--cmd", dest="cmd_arg", help="编译命令（与 --preset 二选一）")
    p_run.add_argument("--preset", help="预设名称")
    p_run.add_argument("--cwd", default="", help="工作目录")
    p_run.add_argument("--no-save", action="store_true", help="不自动保存日志")
    p_run.add_argument("--quiet", action="store_true", help="只输出最后摘要")

    p_add = sub.add_parser("add-preset", help="添加 / 覆盖预设")
    p_add.add_argument("name", help="预设名称")
    p_add.add_argument("command", help="编译命令")
    p_add.add_argument("--cwd", default=".", help="工作目录")
    p_add.add_argument("--desc", default="", help="描述")

    p_del = sub.add_parser("del-preset", help="删除预设")
    p_del.add_argument("name", help="预设名称")

    parsed = parser.parse_args(args)
    pm = PresetManager()

    if parsed.cmd in (None, "gui"):
        run_gui()
        return 0

    if parsed.cmd == "presets":
        presets = pm.list_presets()
        print(f"共 {len(presets)} 个预设:")
        for p in presets:
            mark = " ★" if p["name"] == pm.data.get("last_preset") else ""
            desc = f"  - {p['description']}" if p.get("description") else ""
            print(f"  • {p['name']}{mark}{desc}")
            print(f"      {p['command']}")
        return 0

    if parsed.cmd == "add-preset":
        pm.add_preset(parsed.name, parsed.command, parsed.cwd, parsed.desc)
        print(f"✓ 已保存预设 {parsed.name}")
        return 0

    if parsed.cmd == "del-preset":
        if pm.delete_preset(parsed.name):
            print(f"🗑 已删除预设 {parsed.name}")
            return 0
        print(f"✗ 预设 {parsed.name} 不存在", file=sys.stderr)
        return 1

    if parsed.cmd == "run":
        # 确定命令
        cmd_str = ""
        cwd = parsed.cwd
        if parsed.preset:
            p = pm.get_preset(parsed.preset)
            if not p:
                print(f"✗ 预设 {parsed.preset} 不存在", file=sys.stderr)
                return 1
            cmd_str = p["command"]
            cwd = cwd or p.get("cwd", ".")
        if not cmd_str:
            cmd_str = parsed.cmd_arg or ""
        if not cmd_str:
            print("✗ 必须提供 --cmd 或 --preset", file=sys.stderr)
            return 1
        # 执行
        try:
            cmd = shlex.split(cmd_str)
        except ValueError as e:
            print(f"✗ 命令解析失败: {e}", file=sys.stderr)
            return 1
        # 记录 last_preset（如果是预设）
        if parsed.preset:
            pm.data["last_preset"] = parsed.preset
            pm.save()

        result_holder = {}
        def finish(r):
            result_holder["r"] = r
            # 在 finish 后停止主循环
            from PySide6.QtCore import QCoreApplication
            QCoreApplication.quit()

        from PySide6.QtCore import QCoreApplication, QTimer
        app = QCoreApplication.instance() or QCoreApplication(sys.argv)

        runner = CompileRunner(cmd, cwd=cwd or os.getcwd())
        runner.on_message = (lambda m: None) if parsed.quiet else (lambda m: print(m.raw))
        runner.on_finish = finish
        runner.start()

        # 等待结束（QCoreApplication 让信号能 dispatch，但 CompileRunner 用的是普通 callback）
        # 改用简单轮询
        while "r" not in result_holder:
            time.sleep(0.05)
            app.processEvents()

        r: CompileResult = result_holder["r"]
        # 保存
        if not parsed.no_save:
            try:
                log, js = save_log(r)
                if not parsed.quiet:
                    print(f"\n[日志] {log}\n[JSON] {js}", file=sys.stderr)
            except Exception as e:
                print(f"[警告] 日志保存失败: {e}", file=sys.stderr)

        # 摘要
        status = "✓ 成功" if r.success else "✗ 失败"
        print(f"{status} | 耗时 {r.duration_ms}ms | 退出码 {r.exit_code} | "
              f"错误 {r.errors} | 警告 {r.warnings} | 行数 {r.total_lines}")
        if r.files_seen:
            print(f"涉及文件: {', '.join(r.files_seen)}")
        return 0 if r.success else 1

    parser.print_help()
    return 1


# ===== 入口 =====
if __name__ == "__main__":
    if len(sys.argv) <= 1:
        run_gui()
    else:
        sys.exit(run_cli(sys.argv[1:]))
