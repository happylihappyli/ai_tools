# -*- coding: utf-8 -*-
import sys
import os
import re
import json
import time
import shlex
import signal
import argparse
import subprocess
import glob
import datetime
from pathlib import Path
from typing import Dict, List, Optional, Callable, Tuple
from dataclasses import dataclass, field, asdict
from datetime import datetime

# 确保脚本目录在 sys.path 中，以便正确导入同目录模块
APP_DIR = Path(__file__).parent.resolve()
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

# ===== tracker_rec (事件记录) =====
try:
    import tracker_rec  # 同目录: /home/bv/code/ai_tools/tracker_rec.py
    _TRACKER_OK = True
except ImportError:
    tracker_rec = None  # type: ignore
    _TRACKER_OK = False


def _track(event_type: str, title: str, desc: str = "", node_id: Optional[str] = None) -> None:
    """记录一条事件到 step_data.json。CLI/GUI 通用, 失败静默 (不阻塞主流程)."""
    if not _TRACKER_OK:
        return
    try:
        tracker_rec.add_event(event_type, title, desc, node_id)
    except Exception:
        pass


# ===== 常量 =====
APP_DIR = Path(__file__).parent
DEFAULT_CONFIG_FILE = APP_DIR / "compile_presets.json"
DEFAULT_LOG_DIR = APP_DIR / "build_logs"


def _resolve_config_file() -> Path:
    """解析预设文件路径: env COMPILE_PRESETS_FILE > 默认。"""
    env = os.environ.get("COMPILE_PRESETS_FILE")
    if env:
        return Path(env).expanduser().resolve()
    return DEFAULT_CONFIG_FILE


def _resolve_log_dir() -> Path:
    """解析日志目录: env COMPILE_LOG_DIR > 默认。"""
    env = os.environ.get("COMPILE_LOG_DIR")
    if env:
        return Path(env).expanduser().resolve()
    return DEFAULT_LOG_DIR


# 模块加载时解析一次, 之后 set_config_file() / set_log_dir() 可改
CONFIG_FILE = _resolve_config_file()
LOG_DIR = _resolve_log_dir()


def set_config_file(path) -> None:
    """运行时切换预设文件路径。"""
    global CONFIG_FILE
    CONFIG_FILE = Path(path).expanduser().resolve()


def set_log_dir(path) -> None:
    """运行时切换日志目录。"""
    global LOG_DIR
    LOG_DIR = Path(path).expanduser().resolve()

# 暗色系（与 step_tracker 风格统一）
COLOR_BG = "#1e1e2e"
COLOR_PANEL = "#2a2a3e"
COLOR_ACCENT = "#0d9488"
COLOR_TEXT = "#e0e0e0"
COLOR_ERROR = "#ff6b6b"        # 亮红 (深底背景高对比)
COLOR_WARNING = "#fbbf24"      # 亮黄 (深底背景高对比)
COLOR_SUCCESS = "#34d399"      # 亮绿
COLOR_INFO = "#cbd5e1"         # 亮灰
COLOR_NOTE = "#60a5fa"         # 亮蓝


# ===== C++ 专家工具 (针对 Godot UI / Standalone Skia 项目优化) =====
def cpp_check_env() -> List[str]:
    """诊断 C++ 编译环境 (g++, scons, pkg-config)。"""
    log = []
    log.append("=== 🛠 C++ 环境诊断 ===")
    # 1. Compiler
    try:
        r = subprocess.run(["g++", "--version"], capture_output=True, text=True)
        log.append(f"  Compiler: {r.stdout.splitlines()[0]}")
    except Exception:
        log.append("  ❌ g++ 未找到")
    # 2. SCons
    try:
        r = subprocess.run(["scons", "--version"], capture_output=True, text=True)
        log.append(f"  SCons: {r.stdout.splitlines()[0]}")
    except Exception:
        log.append("  ❌ scons 未找到")
    # 3. C++ Standard (检查是否支持 C++23)
    try:
        # 写一个简单的 C++23 特性测试 (std::expected 或 designated initializers)
        test_cpp = "/tmp/cpp23_test.cpp"
        with open(test_cpp, "w") as f:
            f.write("#include <expected>\nint main() { std::expected<int, int> e = 1; return 0; }")
        r = subprocess.run(["g++", "-std=c++23", test_cpp, "-o", "/tmp/cpp23_test"], capture_output=True)
        if r.returncode == 0:
            log.append("  ✓ C++23 支持: 正常 (std::expected 编译通过)")
        else:
            log.append("  ⚠ C++23 支持: 部分或不支持 (std::expected 编译失败)")
    except Exception:
        log.append("  ⚠ C++23 测试跳过")
    return log


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
    # SCons 阶段标签: scons: Reading SConscript files ... / Building targets ... / done ...
    SCONS_STAGE_RE = re.compile(
        r"^scons:\s*"
        r"(?P<phase>Reading SConscript files \.\.\."
        r"|done reading SConscript files\."
        r"|Building targets \.\.\."
        r"|done building targets\.)\s*$"
    )
    # SCons 错误: scons: *** [target] Error 1
    SCONS_ERR_RE = re.compile(
        r"^scons:\s*\*\*\*\s*\[(?P<target>[^\]]+)\]\s*(?P<rest>.*)$"
    )
    # 编译器命令行（SCons 非 -Q 模式 + Make 工具常见）
    # 匹配: g++ / gcc / clang++ / clang / cl / MSBuild / link / ld / ar / cmake / ninja
    # 注: 用 lookahead 替代 \b，因为 + 等符号不是 word char，\b 无法在 g++ 末尾匹配
    COMPILER_CMD_RE = re.compile(
        r"^(?P<cmd>g\+\+|gcc|clang\+\+?|cl(?:\.exe)?|MSBuild|link(?:\.exe)?|ld|ar"
        r"|cmake(?:\.exe)?|ninja(?:\.exe)?|make|gmake|cc|c\+\+)"
        r"(?=[\s\-/\\]|$)",
        re.IGNORECASE,
    )
    # SCons 进度行: "Compiling xxx.cpp", "Linking program -> bin/xxx", "Building xxx"
    # (这些是 scons 状态行, 路径里可能含 error/warning 等关键字, 不能再用 SIMPLE_RE 误判)
    SCONS_PROGRESS_RE = re.compile(
        r"^(?:Compiling|Linking|Building|Indexing|Generating|Checking|"
        r"Installing|Running|Reading|Spawning|Building\.\.\.|"
        r"libtool|ranlib|strip)\b",
        re.IGNORECASE,
    )
    # 简单行内 error/warning 检测（兜底）
    SIMPLE_RE = re.compile(
        r"\b(error|warning|fatal error)\b", re.IGNORECASE
    )

    # SCons 阶段 → 内部阶段权重（用于无 N/M 时的进度估算）
    _SCONS_PHASE_WEIGHT = {
        "reading": 0.10,
        "read_done": 0.20,
        "building": 0.20,  # building 起始，与 read_done 等权重
        "build_done": 1.00,
    }

    def __init__(self):
        """初始化解析器。"""
        self.files_seen: List[str] = []
        self.errors = 0
        self.warnings = 0
        self.current_step = 0
        self.total_steps = 0
        # SCons 状态：是否已进入 scons 阶段（避免误判普通 g++ 命令）
        self._scons_active = False
        # SCons 内部阶段："" / "reading" / "read_done" / "building" / "build_done"
        self._scons_phase = ""
        # SCons 子命令计数（building 阶段每行命令 +1）
        self._scons_substeps = 0
        # SCons 估算的子步骤总数（动态调整）
        self._scons_substeps_high = 0
        # 记录触发进度的回调
        self.on_progress: Optional[Callable[[int, int], None]] = None

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

        # 4.5 SCons 阶段标签
        m = self.SCONS_STAGE_RE.match(clean)
        if m:
            phase_raw = m.group("phase")
            if phase_raw.startswith("Reading"):
                self._scons_active = True
                self._scons_phase = "reading"
            elif phase_raw.startswith("done reading"):
                self._scons_phase = "read_done"
            elif phase_raw.startswith("Building"):
                self._scons_phase = "building"
            elif phase_raw.startswith("done building"):
                self._scons_phase = "build_done"
            self._update_scons_progress()
            return CompileMessage(
                kind="info",
                text=f"SCons {phase_raw}",
                raw=raw,
                time_ms=elapsed_ms,
            )

        # 4.6 SCons 错误: scons: *** [target] Error 1
        m = self.SCONS_ERR_RE.match(clean)
        if m:
            self.errors += 1
            self._scons_phase = "build_done"  # 失败即终止进度
            self._update_scons_progress()
            return CompileMessage(
                kind="error",
                text=clean,
                raw=raw,
                time_ms=elapsed_ms,
            )

        # 4.7 SCons 子命令（仅在 building 阶段计数）
        if self._scons_active and self._scons_phase == "building":
            if self.COMPILER_CMD_RE.match(clean):
                self._scons_substeps += 1
                # 自适应预估：当前计数的 1.3 倍作为预估上限
                self._scons_substeps_high = max(
                    self._scons_substeps_high,
                    int(self._scons_substeps * 1.3) + 1,
                )
                self._update_scons_progress()
                return CompileMessage(
                    kind="info",
                    text=f"→ {clean[:200]}{'...' if len(clean) > 200 else ''}",
                    raw=raw,
                    time_ms=elapsed_ms,
                )

        # 4.8 SCons 进度行 ("Compiling xxx.cpp", "Linking program -> bin/xxx" 等)
        # 必须在 SIMPLE_RE 兜底之前匹配, 否则路径里含 error/warning 关键字的行
        # (如 "Compiling core/error/error_list.cpp ...") 会被误判为 error。
        if self.SCONS_PROGRESS_RE.match(clean):
            # 也算子步骤, 让进度条有更平滑的爬升
            if self._scons_active and self._scons_phase == "building":
                self._scons_substeps += 1
                self._scons_substeps_high = max(
                    self._scons_substeps_high,
                    int(self._scons_substeps * 1.3) + 1,
                )
                self._update_scons_progress()
            return CompileMessage(
                kind="info",
                text=clean[:300],
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

    def _update_scons_progress(self) -> None:
        """根据 SCons 阶段 + 子步骤数估算进度，写入 current_step/total_steps。

        进度模型（base = 阶段权重，sub = building 内子步骤 0~0.8）：
          reading        → 10%
          read_done      → 20%
          building       → 20% (起)
          building + k   → 20% + k/N * 80%
          build_done     → 100%
        其中 N 是动态预估上限（取 max(当前计数*1.3+1, 上限)），保证进度只前进不回退。
        """
        phase = self._scons_phase
        if phase == "reading":
            cur, total = 1, 10
        elif phase == "read_done":
            cur, total = 2, 10
        elif phase == "building":
            sub_cur = self._scons_substeps
            sub_total = max(self._scons_substeps_high, 1)
            # base = 2, span = 8 (2..10)
            cur = 2 + int(sub_cur * 8 / sub_total)
            total = 10
        elif phase == "build_done":
            cur = total = 10
        else:
            return

        # 单调递增（不回退）
        if cur > self.current_step or total != self.total_steps:
            self.current_step = cur
            self.total_steps = total
            if self.on_progress:
                try:
                    self.on_progress(cur, total)
                except Exception:
                    pass


class CppOutputParser(OutputParser):
    """扩展解析器，针对 C++ 模板和现代 C++ 错误进行优化。"""
    
    # 匹配 template 实例化链
    TEMPLATE_RE = re.compile(r"^(?:in instantiation of|required from|recursively instantiated from)")
    
    def parse(self, line: str, elapsed_ms: int) -> CompileMessage:
        msg = super().parse(line, elapsed_ms)
        # 如果是模板相关的 info，升级为 note 或特定类型
        if msg.kind == "info" and self.TEMPLATE_RE.search(msg.raw):
            msg.kind = "note"
        return msg


# ===== 编译执行器 =====
class CompileExecutor:
    """负责运行编译命令并流式解析输出。"""

    def __init__(self, parser: OutputParser):
        self.parser = parser
        self.process: Optional[subprocess.Popen] = None
        self.is_running = False
        self.start_time = 0

    def run(self, cmd: str, cwd: str, on_msg: Callable[[CompileMessage], None]) -> CompileResult:
        """同步运行命令。"""
        self.is_running = True
        self.start_time = time.time()
        
        # 自动纠正 CWD
        effective_cwd = Path(cwd).expanduser().resolve()
        if cmd.strip().startswith("scons") and not (effective_cwd / "SConstruct").exists():
            candidates = ["godot-ui-standalone-skia", "godot-ui-standalone-direct2d", "source"]
            for cand in candidates:
                sub = effective_cwd / cand
                if sub.exists() and (sub / "SConstruct").exists():
                    effective_cwd = sub
                    on_msg(CompileMessage("note", text=f"💡 自动切换工作目录到: {effective_cwd}", raw=f"Note: Auto-switched CWD to {effective_cwd}"))
                    break
        
        cwd_str = str(effective_cwd)
        _track("compile_start", "编译开始", f"Command: {cmd} (CWD: {cwd_str})")

        # 合并 stderr 到 stdout，并使用 stdbuf 强制行缓冲
        try:
            env = os.environ.copy()
            env["PYTHONUNBUFFERED"] = "1"
            env["GUS_PROGRESS_FORCE"] = "1"
            
            # 增加 PTY 支持以欺骗编译器输出实时流
            import pty
            master, slave = pty.openpty()

            on_msg(CompileMessage("info", text=f"🛠 正在启动: {cmd}", raw=f"Starting: {cmd}"))

            self.process = subprocess.Popen(
                cmd,
                shell=True,
                cwd=cwd_str,
                stdout=slave,
                stderr=slave,
                text=True,
                bufsize=1,
                env=env,
                preexec_fn=os.setsid if os.name != "nt" else None
            )
            
            # 关闭从端，主端读取
            os.close(slave)
            self.master_fd = master
            
            if self.process.pid:
                on_msg(CompileMessage("info", text=f"✅ 进程已启动 (PID: {self.process.pid})", raw="Process started"))

        except Exception as e:
            self.is_running = False
            _track("compile_fail", "启动失败", str(e))
            on_msg(CompileMessage("error", text=f"❌ 启动失败: {str(e)}", raw=str(e)))
            return CompileResult(False, -1, 0, 0, 1, 0, [], [], cmd, cwd)

        messages = []
        try:
            import select
            while self.is_running:
                # 使用 select 防止阻塞，设置 0.1s 超时
                r, _, _ = select.select([self.master_fd], [], [], 0.1)
                if self.master_fd in r:
                    try:
                        data = os.read(self.master_fd, 4096).decode('utf-8', errors='replace')
                    except OSError:
                        break
                    
                    if not data:
                        if self.process.poll() is not None:
                            break
                        continue
                    
                    for line in data.splitlines():
                        elapsed = int((time.time() - self.start_time) * 1000)
                        msg = self.parser.parse(line, elapsed)
                        messages.append(msg)
                        on_msg(msg)
                else:
                    # 如果超时且进程已结束，则跳出
                    if self.process.poll() is not None:
                        break
                    
        except Exception as e:
            on_msg(CompileMessage("error", text=f"⚠ 读取输出时出错: {str(e)}", raw=str(e)))
        finally:
            if hasattr(self, 'master_fd'):
                try:
                    os.close(self.master_fd)
                except OSError:
                    pass

        exit_code = self.process.wait()
        duration = int((time.time() - self.start_time) * 1000)
        self.is_running = False
        
        result = CompileResult(
            success=(exit_code == 0),
            exit_code=exit_code,
            duration_ms=duration,
            total_lines=len(messages),
            errors=self.parser.errors,
            warnings=self.parser.warnings,
            files_seen=self.parser.files_seen,
            messages=messages,
            command=cmd,
            cwd=cwd
        )

        _track("compile_finish" if result.success else "compile_fail", 
               f"编译成功" if result.success else f"编译失败 (code {exit_code})", 
               f"耗时: {duration}ms, 错误: {result.errors}, 警告: {result.warnings}")

        return result

    def stop(self):
        """强行停止编译。"""
        if self.process and self.process.poll() is None:
            try:
                if os.name != "nt":
                    os.killpg(os.getpgid(self.process.pid), signal.SIGTERM)
                else:
                    self.process.terminate()
            except Exception:
                pass
        self.is_running = False


# ===== GUI (PySide6/PyQt5 兼容) =====
try:
    from _qt_compat import (
        gui_available, QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QLabel, QLineEdit, QPushButton, QTextEdit, QProgressBar, QComboBox,
        QListWidget, QListWidgetItem, QColor, QFont, Qt, QTimer, QFileDialog, QMessageBox,
        QSplitter, QFrame, QTreeWidget, QTreeWidgetItem, QTabWidget,
        QGraphicsView, QGraphicsScene, QGraphicsEllipseItem, QGraphicsPathItem,
        QGraphicsTextItem, QGraphicsItem, QPainterPath, QPen, QBrush, QPointF,
        APP_EXEC, QT_BACKEND, QObject, Signal, Slot, QPainter
    )
    _GUI_AVAILABLE = gui_available()
except ImportError:
    _GUI_AVAILABLE = False


class TaskNodeItem(QGraphicsEllipseItem):
    """图形化任务节点。"""
    def __init__(self, node_id, title, status, x, y):
        super().__init__(x - 25, y - 25, 50, 50)
        self.node_id = node_id
        
        # 颜色根据状态
        color = "#555"
        if status == "completed": color = COLOR_SUCCESS
        elif status == "in_progress": color = COLOR_ACCENT
        elif status == "failed": color = COLOR_ERROR
        
        self.setBrush(QBrush(QColor(color)))
        self.setPen(QPen(QColor("#fff"), 2))
        self.setFlag(QGraphicsItem.ItemIsSelectable)
        
        # 文字标签
        self.text = QGraphicsTextItem(title[:10], self)
        self.text.setDefaultTextColor(QColor("#fff"))
        self.text.setPos(x - 20, y + 25)

class TaskGraphView(QGraphicsView):
    """图形化展示任务关系图。"""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.scene = QGraphicsScene(self)
        self.setScene(self.scene)
        self.setRenderHint(QPainter.Antialiasing)
        self.setBackgroundBrush(QBrush(QColor(COLOR_BG)))
        self.setMinimumHeight(200)

    def update_graph(self, nodes, current_id):
        self.scene.clear()
        if not nodes: return
        
        node_map = {n["id"]: n for n in nodes}
        pos_map = {}
        
        # 简单的分层布局算法
        roots = []
        has_parent = set()
        for n in nodes:
            for nxt in n.get("next", []): has_parent.add(nxt)
        roots = [n for n in nodes if n["id"] not in has_parent]
        
        def layout_node(node_id, x, y, level):
            if node_id in pos_map: return
            pos_map[node_id] = (x, y)
            
            node = node_map.get(node_id)
            if not node: return
            
            item = TaskNodeItem(node_id, node.get("title", ""), node.get("status", ""), x, y)
            if node_id == current_id:
                item.setPen(QPen(QColor(COLOR_WARNING), 4))
            self.scene.addItem(item)
            
            next_nodes = node.get("next", [])
            for i, nxt_id in enumerate(next_nodes):
                nx = x + 150
                ny = y + (i - len(next_nodes)/2.0 + 0.5) * 100
                
                # 画线
                path = QGraphicsPathItem()
                pen = QPen(QColor("#666"), 2)
                from_x, from_y = x + 25, y
                to_x, to_y = nx - 25, ny
                line_path = QPainterPath()
                line_path.moveTo(from_x, from_y)
                line_path.cubicTo(from_x + 50, from_y, to_x - 50, to_y, to_x, to_y)
                path.setPath(line_path)
                path.setPen(pen)
                self.scene.addItem(path)
                
                layout_node(nxt_id, nx, ny, level + 1)

        for i, root in enumerate(roots[-5:]): # 最近5个根
            layout_node(root["id"], 50, 50 + i * 200, 0)
        
        self.setSceneRect(self.scene.itemsBoundingRect().adjusted(-50, -50, 50, 50))

class TaskMapWidget(QFrame):
    """显示任务关系链的侧边栏组件。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumWidth(250)
        self.setStyleSheet(f"""
            TaskMapWidget {{ 
                background: {COLOR_PANEL}; 
                border-right: 1px solid #333;
            }}
            QLabel {{ font-weight: bold; padding: 5px; color: {COLOR_ACCENT}; }}
            QTreeWidget {{ border: none; background: transparent; color: #aaa; }}
            QPushButton#btn_tool {{
                background: #3a3a5a; border: 1px solid #555; padding: 4px; font-size: 11px;
            }}
        """)
        
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        
        header = QHBoxLayout()
        header.addWidget(QLabel("🧭 驾驶舱"))
        
        btn_clear = QPushButton("🗑 清空")
        btn_clear.setObjectName("btn_tool")
        btn_clear.clicked.connect(self._on_clear_tasks)
        header.addWidget(btn_clear)

        btn_root = QPushButton("➕ 根计划")
        btn_root.setObjectName("btn_tool")
        btn_root.clicked.connect(lambda: self._on_add_plan(is_root=True))
        header.addWidget(btn_root)
        
        btn_sub = QPushButton("➕ 子任务")
        btn_sub.setObjectName("btn_tool")
        btn_sub.clicked.connect(lambda: self._on_add_plan(is_root=False))
        header.addWidget(btn_sub)
        
        layout.addLayout(header)
        
        self.tabs = QTabWidget()
        self.tabs.setStyleSheet("QTabBar::tab { background: #222; padding: 5px; } QTabBar::tab:selected { background: #333; }")
        
        # 图形视图 (默认)
        self.graph = TaskGraphView()
        self.tabs.addTab(self.graph, "关系图")
        
        # 树视图
        self.tree = QTreeWidget()
        self.tree.setHeaderHidden(True)
        self.tabs.addTab(self.tree, "列表")
        
        layout.addWidget(self.tabs)
        
        self.refresh_timer = QTimer(self)
        self.refresh_timer.timeout.connect(self.refresh_tasks)
        self.refresh_timer.start(3000)

    def _on_clear_tasks(self):
        """清空所有任务记录。"""
        if not _TRACKER_OK: return
        reply = QMessageBox.question(self, "确认清空", "确定要清空所有任务记录吗？这无法撤销。", 
                                     QMessageBox.Yes | QMessageBox.No)
        if reply == QMessageBox.Yes:
            # 清空 nodes 和 events
            data = tracker_rec._load()
            data["nodes"] = []
            data["events"] = []
            data["current_node"] = None
            tracker_rec._save(data)
            self.refresh_tasks()

    def _on_add_plan(self, is_root=False):
        """手动添加计划节点。"""
        title, ok = QInputDialog.getText(self, "添加任务", "任务标题:")
        if ok and title:
            node_id = f"plan_{int(time.time())}"
            parent_id = None if is_root else tracker_rec.get_current_node_id()
            tracker_rec.add_node(node_id, title=title, status="pending", parent_id=parent_id)
            self.refresh_tasks()

    def refresh_tasks(self):
        if not _TRACKER_OK: return
        nodes = tracker_rec.list_nodes()
        current_id = tracker_rec.get_current_node_id()
        
        # 更新树
        self.tree.clear()
        node_map = {n["id"]: n for n in nodes}
        has_parent = set()
        for n in nodes:
            for nxt_id in n.get("next", []): has_parent.add(nxt_id)
        roots = [n for n in nodes if n["id"] not in has_parent]
        
        def add_node_to_tree(node, parent_item=None):
            item = QTreeWidgetItem(parent_item or self.tree)
            node_id = node["id"]
            type_icon = "📂" if node_id.startswith("plan_") else "📜"
            status_icon = {"completed": "✅", "in_progress": "🚀", "failed": "❌"}.get(node.get("status"), "🔘")
            item.setText(0, f"{type_icon} {status_icon} {node.get('title', '')}")
            item.setData(0, Qt.UserRole, node_id)
            if node_id == current_id:
                item.setForeground(0, QColor(COLOR_ACCENT))
                self.tree.setCurrentItem(item)
            for nxt_id in node.get("next", []):
                if nxt_id in node_map: add_node_to_tree(node_map[nxt_id], item)
            item.setExpanded(True)

        for root in roots[-8:]: add_node_to_tree(root)
        
        # 更新图形
        self.graph.update_graph(nodes, current_id)


class WorkerSignals(QObject):
    msg_signal = Signal(object)
    progress_signal = Signal(int, int)
    finish_signal = Signal(object)
    error_signal = Signal(str)

class CompileGUI(QMainWindow):
    """编译工具的 GUI 界面。"""

    def __init__(self, preset_file: Path):
        super().__init__()
        self.preset_file = preset_file
        self.executor: Optional[CompileExecutor] = None
        self.presets: Dict = {}
        self.current_preset: Optional[str] = None
        
        self.signals = WorkerSignals()
        self.signals.msg_signal.connect(self._handle_msg_signal)
        self.signals.progress_signal.connect(self._handle_progress_signal)
        self.signals.finish_signal.connect(self._on_finish)
        self.signals.error_signal.connect(self._handle_error_signal)
        
        self.setWindowTitle("C++ 专家编译工具 (Godot/Skia)")
        self.resize(1000, 700)
        self.setStyleSheet(f"""
            QMainWindow {{ background: {COLOR_BG}; }}
            QWidget {{ background: {COLOR_BG}; color: {COLOR_TEXT}; }}
            QComboBox, QLineEdit, QTextEdit {{
                background: {COLOR_PANEL}; border: 1px solid #444; border-radius: 4px; padding: 4px;
            }}
            QPushButton {{
                background: {COLOR_PANEL}; border: 1px solid #555; border-radius: 4px; padding: 6px 12px;
            }}
            QPushButton:hover {{ background: {COLOR_ACCENT}; }}
            QProgressBar {{
                background: {COLOR_PANEL}; border: 1px solid #444; border-radius: 4px; text-align: center;
            }}
            QProgressBar::chunk {{ background: {COLOR_ACCENT}; }}
        """)

        self._build_ui()
        self.load_presets()
        
        # 诊断环境
        QTimer.singleShot(500, self._run_env_check)

    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        # 使用 QSplitter 实现可拖动分割
        self.splitter = QSplitter(Qt.Horizontal)
        self.splitter.setStyleSheet(f"""
            QSplitter::handle {{
                background: #333;
                width: 2px;
            }}
            QSplitter::handle:hover {{
                background: {COLOR_ACCENT};
            }}
        """)
        main_layout.addWidget(self.splitter)

        # 1. 左侧任务图侧边栏
        self.task_map = TaskMapWidget(self)
        self.splitter.addWidget(self.task_map)

        # 2. 右侧主面板
        right_panel = QWidget()
        right_layout = QVBoxLayout(right_panel)
        self.splitter.addWidget(right_panel)
        
        # 设置初始比例 (280px : 剩余)
        self.splitter.setSizes([280, 720])
        self.splitter.setStretchFactor(1, 1)

        # 顶部：预设 + 命令 (驾驶舱风格)
        dash_header = QFrame()
        dash_header.setStyleSheet(f"background: {COLOR_PANEL}; border-radius: 8px; margin-bottom: 5px;")
        dash_layout = QVBoxLayout(dash_header)
        
        # 第一行: 预设
        top_row = QHBoxLayout()
        top_row.addWidget(QLabel("🚀 预设:"))
        self.preset_combo = QComboBox()
        self.preset_combo.currentIndexChanged.connect(self._on_preset_changed)
        top_row.addWidget(self.preset_combo, 1)
        
        btn_reload = QPushButton("🔄")
        btn_reload.setFixedWidth(40)
        btn_reload.clicked.connect(self.load_presets)
        top_row.addWidget(btn_reload)
        dash_layout.addLayout(top_row)

        # 第二行: 命令
        cmd_row = QHBoxLayout()
        cmd_row.addWidget(QLabel("🛠 命令:"))
        self.cmd_edit = QLineEdit()
        cmd_row.addWidget(self.cmd_edit, 1)
        dash_layout.addLayout(cmd_row)

        # 第三行: 任务提示词
        task_row = QHBoxLayout()
        task_row.addWidget(QLabel("📝 任务:"))
        self.task_edit = QLineEdit()
        self.task_edit.setPlaceholderText("输入本次编译的任务目标...")
        task_row.addWidget(self.task_edit, 1)
        dash_layout.addLayout(task_row)

        right_layout.addWidget(dash_header)

        # 控制与进度 (横向栏)
        ctrl_bar = QHBoxLayout()
        self.btn_run = QPushButton("⚡ 执行编译")
        self.btn_run.setStyleSheet(f"""
            QPushButton {{ 
                background: {COLOR_ACCENT}; 
                font-weight: bold; 
                height: 35px;
                font-size: 14px;
            }}
            QPushButton:disabled {{ background: #444; }}
        """)
        self.btn_run.clicked.connect(self.start_compile)
        ctrl_bar.addWidget(self.btn_run, 2)

        self.btn_stop = QPushButton("🛑 停止")
        self.btn_stop.setEnabled(False)
        self.btn_stop.setFixedHeight(35)
        self.btn_stop.clicked.connect(self.stop_compile)
        ctrl_bar.addWidget(self.btn_stop, 1)
        
        right_layout.addLayout(ctrl_bar)

        # 实时进度条
        self.progress_container = QWidget()
        progress_layout = QVBoxLayout(self.progress_container)
        progress_layout.setContentsMargins(0, 5, 0, 5)
        progress_layout.setSpacing(2)

        self.progress_info = QLabel("就绪")
        self.progress_info.setStyleSheet("color: #aaa; font-size: 10px;")
        progress_layout.addWidget(self.progress_info)

        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        self.progress.setValue(0)
        self.progress.setFixedHeight(8)
        self.progress.setTextVisible(True)
        self.progress.setAlignment(Qt.AlignCenter)
        self.progress.setFormat("%p%")
        self.progress.setStyleSheet(f"""
            QProgressBar {{
                background: #1a1a2e;
                border: none;
                border-radius: 4px;
                color: transparent;
            }}
            QProgressBar::chunk {{
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 {COLOR_ACCENT}, stop:1 #2dd4bf);
                border-radius: 4px;
            }}
        """)
        progress_layout.addWidget(self.progress)
        right_layout.addWidget(self.progress_container)

        # 输出区域
        self.log_view = QTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setFont(QFont("Consolas", 10))
        self.log_view.setStyleSheet("border: 1px solid #333; border-radius: 4px;")
        right_layout.addWidget(self.log_view, 1)

        # 状态栏
        self.status_lbl = QLabel("Ready")
        self.status_lbl.setStyleSheet("color: #888; font-size: 11px;")
        right_layout.addWidget(self.status_lbl)

    def load_presets(self):
        if not self.preset_file.exists():
            return
        try:
            with open(self.preset_file, "r", encoding="utf-8") as f:
                data = json.load(f)
                self.presets = data.get("presets", {})
                last = data.get("last_preset")
                
                self.preset_combo.clear()
                for name in self.presets:
                    self.preset_combo.addItem(name)
                
                if last and last in self.presets:
                    idx = self.preset_combo.findText(last)
                    self.preset_combo.setCurrentIndex(idx)
        except Exception as e:
            self.log_view.append(f"❌ 加载预设失败: {e}")

    def _on_preset_changed(self, idx):
        name = self.preset_combo.currentText()
        if name in self.presets:
            p = self.presets[name]
            self.cmd_edit.setText(p.get("command", ""))
            self.current_preset = name

    def _run_env_check(self):
        logs = cpp_check_env()
        for line in logs:
            self.log_view.append(line)
        self.log_view.append("")

    def start_compile(self):
        cmd = self.cmd_edit.text().strip()
        if not cmd:
            return
        
        # 保护：防止重复启动
        if self.executor and self.executor.is_running:
            return
        
        # 在主线程中捕获任务提示词，确保线程安全
        task_title = self.task_edit.text().strip()

        self.btn_run.setEnabled(False)
        self.btn_stop.setEnabled(True)
        self.log_view.clear()
        self.log_view.append(f"🚀 准备执行: {cmd}")
        self.log_view.append(f"📂 工作目录: {self.presets.get(self.current_preset, {}).get('cwd', '.')}")
        self.progress.setValue(0)
        self.progress_info.setText("正在准备编译环境...")
        self.status_lbl.setText("编译中...")

        parser = CppOutputParser()
        parser.on_progress = self._update_progress
        self.executor = CompileExecutor(parser)

        # 开启线程执行
        import threading
        def run_task():
            try:
                # 异步记录任务
                self._on_compile_start_record(cmd, task_title)
                
                cwd = "."
                if self.current_preset and self.current_preset in self.presets:
                    cwd = self.presets[self.current_preset].get("cwd", ".")
                
                # 再次确认目录存在
                effective_cwd = Path(cwd).expanduser().resolve()
                if not effective_cwd.exists():
                    raise FileNotFoundError(f"工作目录不存在: {effective_cwd}")

                # 注意：使用信号或 invokeMethod 来更新 GUI 更安全。为了兼容性，这里使用 invokeMethod 的替代。
                # 但直接使用 QTimer.singleShot(0, ...) 在某些 PyQt5 版本上跨线程调用可能会失效！
                # 为确保输出，我们让 executor 也能处理
                result = self.executor.run(cmd, cwd, self._on_new_msg)
                
                # 回到主线程更新 UI
                self.signals.finish_signal.emit(result)
            except Exception as e:
                # 捕获线程内的所有异常并显示
                error_msg = f"\n❌ 内部错误: {str(e)}"
                print(error_msg)
                self.signals.error_signal.emit(error_msg)

        threading.Thread(target=run_task, daemon=True).start()

    def _on_compile_start_record(self, cmd, task_title):
        """在线程中记录任务，使用传入的参数避免访问 GUI 组件。"""
        if _TRACKER_OK and task_title:
            try:
                # 默认不再自动嵌套，除非用户显式选择了父节点
                # 这里我们保持逻辑：如果是从“➕ 子任务”按钮来的，会有 parent_id
                # 如果是直接点“执行编译”，我们根据当前选中的树节点来决定
                
                # 为了防止树太深，我们将默认逻辑改为：
                # 如果当前没有选中任何节点，或者是连续任务，则尝试作为兄弟节点
                current_id = tracker_rec.get_current_node_id()
                parent_id = None
                
                if current_id:
                    if current_id.startswith("task_"):
                        # 连续任务 -> 兄弟节点
                        parent_id = tracker_rec.find_parent_id(current_id)
                    else:
                        # 手动计划 -> 子节点
                        parent_id = current_id
                
                node_id = f"task_{int(time.time())}"
                tracker_rec.add_node(node_id, title=task_title, desc=f"Command: {cmd}", parent_id=parent_id)
                tracker_rec.set_current(node_id)
                QTimer.singleShot(0, lambda: self.log_view.append(f"📌 任务已入库: {task_title}"))
                QTimer.singleShot(0, self.task_map.refresh_tasks)
            except Exception as e:
                print(f"Error recording task: {e}")

    def _handle_error_signal(self, error_msg: str):
        self.log_view.append(error_msg)
        self.status_lbl.setText("执行出错")
        self.btn_run.setEnabled(True)

    def _handle_progress_signal(self, cur: int, total: int):
        if total > 0:
            val = int(cur * 100 / total)
            self.progress.setValue(val)

    def _handle_msg_signal(self, msg: CompileMessage):
        if msg.kind == "info" and len(msg.text) > 0:
            info = msg.text[:80] + ("..." if len(msg.text) > 80 else "")
            self.progress_info.setText(info)

        color = COLOR_TEXT
        if msg.kind == "error": color = COLOR_ERROR
        elif msg.kind == "warning": color = COLOR_WARNING
        elif msg.kind == "note": color = COLOR_NOTE
        
        self._append_log(msg.raw, color)

    def _on_new_msg(self, msg: CompileMessage):
        # 线程中发送信号
        self.signals.msg_signal.emit(msg)

    def _update_progress(self, cur, total):
        # 线程中发送信号
        self.signals.progress_signal.emit(cur, total)

    def _append_log(self, text, color):
        if not text:
            return
        self.log_view.setTextColor(QColor(color))
        self.log_view.append(text)
        # 强制刷新界面，确保日志即时显示
        QApplication.processEvents()
        # 自动滚动到底部
        self.log_view.verticalScrollBar().setValue(self.log_view.verticalScrollBar().maximum())

    def _on_finish(self, result: CompileResult):
        self.btn_run.setEnabled(True)
        self.btn_stop.setEnabled(False)
        self.progress_info.setText("编译结束")
        
        # 更新任务状态
        if _TRACKER_OK:
            current_id = tracker_rec.get_current_node_id()
            if current_id:
                if result.success:
                    tracker_rec.complete_action(current_id, desc=f"编译成功 (耗时: {result.duration_ms}ms)")
                else:
                    tracker_rec.update_node(current_id, status="failed", desc=f"编译失败 (错误数: {result.errors})")
            self.task_map.refresh_tasks()

        status = "成功" if result.success else "失败"
        self.status_lbl.setText(f"编译{status} (耗时: {result.duration_ms}ms)")
        
        if result.success:
            self.progress.setValue(100)
            QMessageBox.information(self, "编译完成", f"编译成功！\n耗时: {result.duration_ms}ms")
        else:
            QMessageBox.critical(self, "编译失败", f"编译失败，错误数: {result.errors}")

    def stop_compile(self):
        if self.executor:
            self.executor.stop()
            if _TRACKER_OK:
                current_id = tracker_rec.get_current_node_id()
                if current_id:
                    tracker_rec.update_node(current_id, status="failed", desc="编译已手动停止")
                self.task_map.refresh_tasks()
            
            self.log_view.append("\n🛑 编译已由用户停止。")
            self.status_lbl.setText("已停止")
            self.btn_run.setEnabled(True)
            self.btn_stop.setEnabled(False)


# ===== CLI & Main =====
def main():
    parser = argparse.ArgumentParser(description="C++ 专家编译工具")
    parser.add_argument("--preset", help="启动时选中的预设名称")
    parser.add_argument("--task", help="本次任务的提示词 (CLI 模式使用)")
    parser.add_argument("--cmd", help="在 GUI 中自动运行的命令，或 CLI 模式下运行的命令")
    parser.add_argument("--cli", action="store_true", help="使用无界面的纯命令行模式")
    parser.add_argument("--cwd", default=".", help="运行命令的工作目录")
    
    args = parser.parse_args()

    if args.cli and args.cmd:
        # CLI 模式
        parser_obj = CppOutputParser()
        executor = CompileExecutor(parser_obj)
        
        if args.task and _TRACKER_OK:
            current_id = tracker_rec.get_current_node_id()
            parent_id = None
            if current_id:
                if current_id.startswith("task_"):
                    parent_id = tracker_rec.find_parent_id(current_id)
                else:
                    parent_id = current_id
            
            node_id = f"task_{int(time.time())}"
            # 创建新节点并自动关联父节点
            tracker_rec.add_node(node_id, title=args.task, desc=f"CLI Command: {args.cmd}", parent_id=parent_id)
            tracker_rec.set_current(node_id)

        print(f"🚀 开始编译: {args.cmd}")
        result = executor.run(args.cmd, args.cwd, lambda m: print(m.raw))
        sys.exit(0 if result.success else 1)
    
    # GUI 模式
    if not _GUI_AVAILABLE:
        print("❌ 错误: 未检测到 Qt 后端，无法启动 GUI。请使用 --cmd 模式。")
        sys.exit(1)

    # 启用高 DPI 支持 (针对 Qt5，必须在 QApplication 之前)
    if hasattr(Qt, "AA_EnableHighDpiScaling"):
        try:
            QApplication.setAttribute(Qt.AA_EnableHighDpiScaling)
        except Exception:
            pass

    app = QApplication(sys.argv)

    gui = CompileGUI(CONFIG_FILE)
    if args.preset:
        idx = gui.preset_combo.findText(args.preset)
        if idx >= 0:
            gui.preset_combo.setCurrentIndex(idx)
    if args.task:
        gui.task_edit.setText(args.task)
    if args.cmd:
        gui.cmd_edit.setText(args.cmd)
    if args.cwd:
        # 临时插入一个包含 cwd 的预设，供 GUI 运行时读取
        gui.presets["__temp_cmd__"] = {"cwd": args.cwd, "command": args.cmd or ""}
        gui.current_preset = "__temp_cmd__"
        
    gui.show()
    gui.raise_()
    gui.activateWindow()
    if args.cmd:
        # 如果传入了 --cmd，自动开始编译
        QTimer.singleShot(500, gui.start_compile)

    sys.exit(getattr(app, APP_EXEC)())


if __name__ == "__main__":
    main()
