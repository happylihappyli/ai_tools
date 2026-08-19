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
import glob
import datetime
from pathlib import Path
from typing import Dict, List, Optional, Callable, Tuple
from dataclasses import dataclass, field, asdict
from datetime import datetime

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


# ===== 编译执行器 =====
def parse_env_prefixes(cmd_input) -> Tuple[Dict[str, str], List[str]]:
    """从命令字符串/列表中提取 `VAR=value` 前缀 (shell 风格)。

    例如:
        "GUS=1 scons -j2"
    -> ({"GUS": "1"}, ["scons", "-j2"])

    支持重复 / 多个 VAR:
        "A=1 B=2 gcc main.c"
    -> ({"A": "1", "B": "2"}, ["gcc", "main.c"])

    不会误判: `=value` 单独不行 (无变量名), `gcc -DFOO=1` 里的 = 在参数中不会被切
    (因为 shlex.split 已经把 -DFOO=1 作为一个 token)
    """
    if isinstance(cmd_input, str):
        tokens = shlex.split(cmd_input)
    else:
        tokens = list(cmd_input)
    env: Dict[str, str] = {}
    i = 0
    while i < len(tokens):
        t = tokens[i]
        # 形如 VAR=value 且 VAR 是合法标识符
        if "=" in t and re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", t):
            k, v = t.split("=", 1)
            env[k] = v
            i += 1
        else:
            break
    return env, tokens[i:]


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
        # 把外层进度回调桥接到 parser（让 scons/cmake 阶段变化也能触发进度）
        if self.on_progress:
            self.parser.on_progress = self.on_progress

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

    def __init__(self, config_file: Optional[Path] = None):
        """加载或初始化配置文件。config_file 为 None 时用全局 CONFIG_FILE。"""
        if config_file is None:
            config_file = CONFIG_FILE
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
        # 兼容: presets 可能是 list (新格式) 也可能是 dict (老格式)
        self._normalize_presets()

    def _normalize_presets(self) -> None:
        """把 presets 统一成 dict[name] = preset 结构。"""
        presets = self.data.get("presets")
        if isinstance(presets, list):
            # list → dict
            d = {}
            for p in presets:
                if isinstance(p, dict) and "name" in p:
                    d[p["name"]] = p
            self.data["presets"] = d
        elif not isinstance(presets, dict):
            self.data["presets"] = {}

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
                "scons-build": {
                    "name": "scons-build",
                    "command": "scons -j4",
                    "cwd": ".",
                    "description": "示例：使用 SCons 构建（4 线程）",
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
def save_log(result: CompileResult, log_dir: Optional[Path] = None) -> tuple:
    """把编译结果保存到文件，返回 (log_path, json_path)。log_dir 为 None 用全局 LOG_DIR。"""
    if log_dir is None:
        log_dir = LOG_DIR
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


# ===== Godot 专用操作 helpers (GUI/CLI 共享) =====
def _cli_node_id() -> Optional[str]:
    """CLI: 读 step_data.json 的 current_node, 作为事件关联节点。"""
    if not _TRACKER_OK:
        return None
    try:
        d = tracker_rec._load()
        return d.get("current_node")
    except Exception:
        return None


def godot_resolve_extras(preset: Dict) -> Dict:
    """从预设读 godot extras, 兼容缺字段 (返回空 dict)."""
    return preset.get("extras", {}) if preset else {}


def godot_clean(src_dir: str) -> int:
    """Clean: 删 .sconsign5.dblite + bin/*.o, 返回删除数。"""
    removed = 0
    for pat in [".sconsign5.dblite", "bin/*.o"]:
        for p in glob.glob(str(Path(src_dir) / pat)):
            try:
                os.remove(p)
                removed += 1
            except Exception:
                pass
    return removed


def godot_deep_clean(src_dir: str) -> int:
    """Deep Clean: 跑 scons -c + 删 .sconsign5 + 删 bin/ binary, 返回删除数。"""
    # Step 1: scons -c
    try:
        subprocess.run(
            ["scons", "-c", "platform=linuxbsd", "target=editor",
             "module_cloud_viewer_native_enabled=yes", "-j2"],
            cwd=src_dir, capture_output=True, text=True, timeout=120
        )
    except Exception:
        pass
    # Step 2: 删 .sconsign5
    for p in [".sconsign5.dblite", ".scons_node_count"]:
        full = Path(src_dir) / p
        if full.exists():
            try:
                full.unlink()
            except Exception:
                pass
    # Step 3: 删 bin/ binary
    removed = 0
    bin_dir = Path(src_dir) / "bin"
    if bin_dir.exists():
        for pat in ["godot.linuxbsd.*", "*.so"]:
            for p in glob.glob(str(bin_dir / pat)):
                if os.path.isfile(p):
                    try:
                        os.remove(p)
                        removed += 1
                    except Exception:
                        pass
    return removed


def godot_touch(editor_cpp: str) -> bool:
    """Touch 一个文件 (强制 scons 重编)。返回是否成功。"""
    if not editor_cpp or not os.path.exists(editor_cpp):
        return False
    Path(editor_cpp).touch()
    return True


def godot_binary_contains_fix(binary: str, marker: str) -> str:
    """检查 binary 是否含 fix 字符串 (用 strings 命令)。
    返回: "✓ 含 <marker>" / "✗ 不含 <marker>" / "? strings 命令缺失" / "? binary 不存在"
    """
    if not binary or not os.path.exists(binary):
        return f"? binary 不存在: {binary}"
    try:
        r = subprocess.run(["strings", binary], capture_output=True, text=True, timeout=10)
        if marker in r.stdout:
            return f"✓ 含 {marker}"
        return f"✗ 不含 {marker}"
    except Exception:
        return "? strings 命令缺失"


def godot_diagnose(src_dir: str, binary: str, editor_cpp: str,
                   fix_marker: str = "v5.11_editor_fix") -> List[str]:
    """诊断 scons 状态 (不跑 scons, 文件系统级检查), 返回日志行列表。"""
    log: List[str] = []
    log.append("=== 🩺 scons 诊断 (文件系统检查) ===")
    # 1. sconsign5
    sconsign = Path(src_dir) / ".sconsign5.dblite"
    log.append("--- 1. sconsign5.dblite 状态 ---")
    if sconsign.exists():
        sz = sconsign.stat().st_size
        mt = datetime.fromtimestamp(sconsign.stat().st_mtime).strftime("%H:%M:%S")
        log.append(f"  ✓ 存在, 大小: {sz} 字节, mtime: {mt}")
    else:
        log.append(f"  ✗ 不存在: {sconsign}")
    # 2. .o
    log.append("--- 2. editor_data.cpp.o 状态 ---")
    o_files = list(Path(src_dir).rglob("editor_data.cpp.o"))
    if o_files:
        for o in o_files:
            sz = o.stat().st_size
            mt = datetime.fromtimestamp(o.stat().st_mtime).strftime("%H:%M:%S")
            log.append(f"  {o.relative_to(src_dir)} (大小: {sz}, mtime: {mt})")
    else:
        log.append("  ✗ 没找到任何 editor_data.cpp.o")
    # 3. binary
    log.append("--- 3. Binary 状态 ---")
    if binary and os.path.exists(binary):
        sz = os.path.getsize(binary) / 1024 / 1024
        mt = datetime.fromtimestamp(os.path.getmtime(binary)).strftime("%H:%M:%S")
        log.append(f"  {os.path.basename(binary)}")
        log.append(f"  大小: {sz:.1f} MB, mtime: {mt}")
    else:
        log.append(f"  ✗ 不存在: {binary}")
    # 4. source
    log.append("--- 4. editor_data.cpp 状态 ---")
    if editor_cpp and os.path.exists(editor_cpp):
        sz = os.path.getsize(editor_cpp)
        mt = datetime.fromtimestamp(os.path.getmtime(editor_cpp)).strftime("%H:%M:%S")
        log.append(f"  {editor_cpp}")
        log.append(f"  大小: {sz} 字节, mtime: {mt}")
    # 5. 决策表
    log.append("--- 5. 决策表 ---")
    src_mt = os.path.getmtime(editor_cpp) if os.path.exists(editor_cpp) else 0
    o_mt = o_files[0].stat().st_mtime if o_files else 0
    bin_mt = os.path.getmtime(binary) if binary and os.path.exists(binary) else 0
    log.append(f"  source mtime: {datetime.fromtimestamp(src_mt).strftime('%H:%M:%S') if src_mt else 'N/A'}")
    log.append(f"  .o mtime:     {datetime.fromtimestamp(o_mt).strftime('%H:%M:%S') if o_mt else 'N/A'}")
    log.append(f"  binary mtime: {datetime.fromtimestamp(bin_mt).strftime('%H:%M:%S') if bin_mt else 'N/A'}")
    if src_mt > o_mt:
        log.append("  ⚠️  source > .o → 应该重编")
    elif src_mt == o_mt:
        log.append("  ⚠️  source == .o → 可能跳 (你刚改过?)")
    else:
        log.append("  ✓  source < .o → .o 是新的")
    if o_mt > bin_mt:
        log.append("  ⚠️  .o > binary → 应该重链")
    elif o_mt == bin_mt:
        log.append("  ⚠️  .o == binary → 可能跳")
    else:
        log.append("  ✓  .o < binary → binary 是新的")
    # 6. strings 检查
    log.append(f"--- 6. binary 含 fix 字符串? ({fix_marker}) ---")
    fix_status = godot_binary_contains_fix(binary, fix_marker) if binary else None
    log.append(f"  {fix_status}")
    # 7. 物理机手动命令
    log.append("--- 7. 物理机手动跑 ---")
    log.append("  rm -f .sconsign5.dblite bin/godot.linuxbsd.editor.x86_64.cloud_ros2")
    log.append("  scons platform=linuxbsd target=editor module_cloud_viewer_native_enabled=yes -j2 2>&1 | tail -20")
    return log


def godot_launch(launch_script: str, render: str, mode: str) -> bool:
    """启动 Godot (detach), 返回是否成功。失败会写 stderr。"""
    if not launch_script or not os.path.exists(launch_script):
        print(f"✗ 找不到 launch_script: {launch_script}", file=sys.stderr)
        return False
    env = os.environ.copy()
    env["RENDER_MODE"] = render
    if mode == "editor":
        env["USE_EDITOR"] = "yes"
    try:
        subprocess.Popen(["/bin/bash", launch_script], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except Exception as e:
        print(f"✗ 启动失败: {e}", file=sys.stderr)
        return False


def godot_resolve_paths_from_preset(preset: Dict, src_dir: str = "") -> Dict:
    """把预设 + src_dir 解析成统一路径 dict (给 GUI/CLI 共享)。"""
    extras = godot_resolve_extras(preset)
    return {
        "src_dir": src_dir or preset.get("cwd", "."),
        "binary": extras.get("binary_path", ""),
        "editor_cpp": extras.get("editor_data_cpp", ""),
        "fix_marker": extras.get("fix_marker", "v5.11_editor_fix"),
        "launch_script": extras.get("launch_script", ""),
        "ui_script": extras.get("ui_script", ""),
        "default_render": extras.get("default_render", "VULKAN_WAYLAND"),
        "render_modes": extras.get("render_modes", ["VULKAN_WAYLAND"]),
    }


# ===== GUI 部分 =====
def run_gui() -> None:
    """启动图形界面 (PySide6/PyQt5/PySide2 多后端兼容)."""
    # 如果传入 --offscreen 则用 offscreen 平台（用于测试）
    # 必须在 argparse 之前移除，否则会报 unrecognized arguments
    if "--offscreen" in sys.argv:
        os.environ["QT_QPA_PLATFORM"] = "offscreen"
        sys.argv = [a for a in sys.argv if a != "--offscreen"]

    # 引入多后端兼容层
    try:
        from _qt_compat import (
            QT_BACKEND, APP_EXEC, gui_available,
            Qt, QTimer, QObject, Signal, QProcess, QFileSystemWatcher,
            QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
            QLabel, QLineEdit, QTextEdit, QPushButton, QListWidget, QListWidgetItem,
            QProgressBar, QComboBox, QFileDialog, QMessageBox, QSplitter,
            QStatusBar, QToolBar, QPlainTextEdit, QTabWidget, QGroupBox,
            QInputDialog, QColor, QFont, QAction, QTextCursor, QKeySequence, QShortcut,
            QMenu,
        )
    except ImportError:
        sys.stderr.write("✗ _qt_compat.py 不在同目录 (需要 /home/bv/code/ai_tools/_qt_compat.py)\n")
        sys.exit(1)
    if not gui_available():
        sys.stderr.write(
            "✗ 没找到任何 Qt 后端 (PySide6 / PyQt5 / PySide2)\n"
            "  安装: pip install --user PySide6\n"
        )
        sys.exit(1)
    sys.stderr.write(f"[compile_tool] using {QT_BACKEND}\n")

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

            # Detached 守护进程编译相关
            self.daemon_parser: Optional[OutputParser] = None
            self.daemon_log_path: Optional[Path] = None
            self.daemon_log_pos: int = 0
            self.daemon_watcher: Optional[QFileSystemWatcher] = None
            self.daemon_poll_timer: Optional[QTimer] = None
            self.daemon_start_ts: float = 0.0

            self.setWindowTitle("图形化编译工具 - Compile Tool")
            self.resize(1280, 800)
            self.setStyleSheet(self._qss())

            self._build_ui()
            self._load_presets_to_combo()
            self._apply_last_preset()

            # 2026-08-19: 启动 200ms 后检测是否有 build_daemon 在跑 (别的 GUI 启的)
            #   如果有 → 启用 ⏹ 停 Daemon 按钮, 让用户能停
            #   没有 → 保持 disabled
            QTimer.singleShot(200, self._detect_existing_daemon)

        def _detect_existing_daemon(self) -> None:
            """启动时检测: .ai_tools/build_daemon.pid 指向真 build_daemon 进程 → 启用 ⏹ 按钮。"""
            ai_dir = Path("/home/bv/code/godot_ui_linux/.ai_tools")
            pid_file = ai_dir / "build_daemon.pid"
            if not pid_file.exists():
                return
            try:
                old_pid = int(pid_file.read_text().strip())
                os.kill(old_pid, 0)  # 进程必须活
                with open(f"/proc/{old_pid}/cmdline", "rb") as _f:
                    cmdline = _f.read().decode("utf-8", errors="replace")
                if "build_daemon" not in cmdline:
                    return  # PID 复用, 不是真 daemon
            except (ProcessLookupError, ValueError, PermissionError, OSError, FileNotFoundError):
                return

            # 真 daemon 在跑 → 启用停 Daemon 按钮
            self.act_stop_detached.setEnabled(True)
            self.statusBar().showMessage(
                f"📡 检测到已运行 daemon (pid={old_pid}), 点 ⏹ 停 Daemon 可中断", 8000)

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
                /* 2026-08-19: QMessageBox 强制黑底白字 (避免系统 theme 让文字灰底灰字看不清) */
                QMessageBox {{
                    background: #000000; color: #ffffff;
                }}
                QMessageBox QLabel {{
                    color: #ffffff; background: transparent;
                }}
                QMessageBox QPushButton {{
                    background: {COLOR_ACCENT}; color: white;
                    border: none; padding: 6px 16px; border-radius: 4px;
                    min-width: 70px;
                }}
                QMessageBox QPushButton:hover {{ background: #14b8a6; }}
                QMessageBox QPushButton:default {{
                    background: {COLOR_ACCENT}; border: 2px solid #fbbf24;
                }}
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
            self.act_detached = QAction("🚀 Detached 编译", self)
            self.act_detached.setToolTip("Detached 守护进程跑 scons, 关 GUI 不杀编译, 抗 Trae sandbox SIGTERM")
            self.act_detached.triggered.connect(self.start_detached)
            tb.addAction(self.act_detached)
            self.act_stop_detached = QAction("⏹ 停 Daemon", self)
            self.act_stop_detached.triggered.connect(self.stop_detached)
            self.act_stop_detached.setEnabled(False)
            tb.addAction(self.act_stop_detached)
            tb.addSeparator()
            self.act_clear = QAction("🧹 清空", self)
            self.act_clear.triggered.connect(self.clear_messages)
            tb.addAction(self.act_clear)
            self.act_save = QAction("💾 保存日志", self)
            self.act_save.triggered.connect(self.save_log_manual)
            tb.addAction(self.act_save)
            tb.addSeparator()
            self.act_tracker = QAction("📊 任务追踪", self)
            self.act_tracker.setToolTip("打开 step_tracker.py GUI (节点流程图 + 实时事件)")
            self.act_tracker.triggered.connect(self.open_tracker)
            tb.addAction(self.act_tracker)
            self.act_godot_ui = QAction("🎛 Godot UI", self)
            self.act_godot_ui.setToolTip("打开 ui.py 主工具 (ros2 mock + 测试)")
            self.act_godot_ui.triggered.connect(self.open_godot_ui)
            tb.addAction(self.act_godot_ui)
            tb.addSeparator()
            tb.addAction("❓ 帮助", self.show_help)

            # 进度条
            self.progress = QProgressBar()
            self.progress.setMaximum(0)  # 0 = 忙碌（不确定）
            self.progress.setFormat("等待开始…")
            top_layout.addWidget(self.progress)

            # ===== Godot Extras 面板 (仅 godot 预设时显示) =====
            godot_group = QGroupBox("Godot 专用工具 (Deep Clean / Touch / 诊断 / 启动)")
            godot_layout = QVBoxLayout(godot_group)

            # 工具行 1: Clean / Deep Clean / Touch / 诊断
            tools_row1 = QHBoxLayout()
            self.btn_clean = QPushButton("🧹 Clean")
            self.btn_clean.setToolTip("删 .sconsign5.dblite + .o, 下次跑全量")
            self.btn_clean.clicked.connect(self.on_godot_clean)
            tools_row1.addWidget(self.btn_clean)

            self.btn_deep_clean = QPushButton("🧹 Deep Clean")
            self.btn_deep_clean.setStyleSheet(
                "QPushButton { background-color: #9C27B0; }"
                "QPushButton:hover { background-color: #BA68C8; }"
            )
            self.btn_deep_clean.setToolTip(
                "跑 scons -c + 删 .sconsign5.dblite + 删 bin/ binary. 下次全量重编"
            )
            self.btn_deep_clean.clicked.connect(self.on_godot_deep_clean)
            tools_row1.addWidget(self.btn_deep_clean)

            self.btn_touch = QPushButton("👆 Touch + 重编")
            self.btn_touch.setToolTip("只 touch editor_data.cpp, 强制 scons 重编这个文件 + 重链 binary")
            self.btn_touch.clicked.connect(self.on_godot_touch)
            tools_row1.addWidget(self.btn_touch)

            self.btn_diagnose = QPushButton("🩺 诊断 scons")
            self.btn_diagnose.setStyleSheet(
                "QPushButton { background-color: #FF5722; }"
                "QPushButton:hover { background-color: #FF7043; }"
            )
            self.btn_diagnose.setToolTip("看 sconsign5 / .o / binary 状态, 不跑 scons, 立刻出结果")
            self.btn_diagnose.clicked.connect(self.on_godot_diagnose)
            tools_row1.addWidget(self.btn_diagnose)
            tools_row1.addStretch(1)
            godot_layout.addLayout(tools_row1)

            # 工具行 2: 启动 (render + mode + 启动按钮 + ui.py 按钮)
            launch_row = QHBoxLayout()
            launch_row.addWidget(QLabel("Render:"))
            self.render_combo = QComboBox()
            self.render_combo.addItems([
                "VULKAN_WAYLAND", "VULKAN_NVIDIA", "VULKAN_SOFT",
                "GLES3_NVIDIA", "OPENGL3_NVIDIA", "HEADLESS"
            ])
            self.render_combo.setCurrentText("VULKAN_WAYLAND")
            launch_row.addWidget(self.render_combo)

            launch_row.addWidget(QLabel("Mode:"))
            self.mode_combo = QComboBox()
            self.mode_combo.addItems(["game (3D 渲染)", "editor (场景树/inspector)"])
            launch_row.addWidget(self.mode_combo)
            launch_row.addStretch(1)
            godot_layout.addLayout(launch_row)

            launch_btn_row = QHBoxLayout()
            self.btn_launch = QPushButton("🚀 启动 Godot")
            self.btn_launch.setStyleSheet(
                "QPushButton { background-color: #FF9800; color: white; "
                "padding: 12px 24px; font-size: 16px; font-weight: bold; }"
            )
            self.btn_launch.clicked.connect(self.on_godot_launch)
            launch_btn_row.addWidget(self.btn_launch)

            self.btn_launch_ui = QPushButton("🎛 启动 ui.py")
            self.btn_launch_ui.setStyleSheet(
                "QPushButton { background-color: #2196F3; color: white; "
                "padding: 12px 24px; font-size: 16px; }"
            )
            self.btn_launch_ui.clicked.connect(self.on_godot_launch_ui)
            launch_btn_row.addWidget(self.btn_launch_ui)
            launch_btn_row.addStretch(1)
            godot_layout.addLayout(launch_btn_row)

            self.godot_group = godot_group
            godot_group.setVisible(False)  # 默认隐藏, 选 godot 预设才显示
            top_layout.addWidget(godot_group)
            self.godot_extras = {}  # type: Dict

            # 主区域：分割（消息列表 + 详情 / 原始输出）
            splitter = QSplitter(Qt.Orientation.Vertical)

            # 消息列表
            self.msg_list = QListWidget()
            self.msg_list.setFont(QFont("Consolas", 10))
            self.msg_list.setStyleSheet(f"""
                QListWidget {{
                    background: {COLOR_BG};
                    color: {COLOR_TEXT};
                    border: 1px solid {COLOR_PANEL};
                    outline: none;
                }}
                QListWidget::item {{
                    padding: 3px 6px;
                    border-bottom: 1px solid #2a2a3e;
                }}
                QListWidget::item:selected {{
                    background: {COLOR_ACCENT};
                    color: #ffffff;
                }}
                QListWidget::item[kind="error"]   {{ color: {COLOR_ERROR}; }}
                QListWidget::item[kind="warning"] {{ color: {COLOR_WARNING}; }}
                QListWidget::item[kind="info"]    {{ color: {COLOR_INFO}; }}
                QListWidget::item[kind="note"]    {{ color: {COLOR_NOTE}; }}
                QListWidget::item[kind="success"] {{ color: {COLOR_SUCCESS}; }}
            """)
            self.msg_list.setSelectionMode(QListWidget.SelectionMode.ExtendedSelection)
            self.msg_list.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            self.msg_list.customContextMenuRequested.connect(self._on_msg_list_context_menu)
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
            # 消息列表的复制 / 全选 (在 msg_list 有焦点时生效)
            QShortcut(QKeySequence.StandardKey.Copy, self.msg_list, activated=self._copy_selected)
            QShortcut(QKeySequence.StandardKey.SelectAll, self.msg_list, activated=self.msg_list.selectAll)

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
            # 显示 / 隐藏 Godot extras 面板
            self._show_godot_panel(p.get("type") == "godot")
            if p.get("type") == "godot":
                self._populate_godot_extras(p.get("extras", {}))

        def _current_node_id(self) -> Optional[str]:
            """读 step_data.json 的 current_node, 作为事件关联节点。"""
            if not _TRACKER_OK:
                return None
            try:
                d = tracker_rec._load()
                return d.get("current_node")
            except Exception:
                return None

        def _show_godot_panel(self, show: bool) -> None:
            """显示/隐藏 Godot extras 面板 (Deep Clean / Touch / 诊断 / 启动)."""
            if hasattr(self, "godot_group"):
                self.godot_group.setVisible(show)

        def _populate_godot_extras(self, extras: Dict) -> None:
            """把 preset.extras 填到 Godot 面板的控件。"""
            self.godot_extras = extras
            # 启动模式
            if hasattr(self, "render_combo"):
                self.render_combo.clear()
                self.render_combo.addItems(extras.get("render_modes", ["VULKAN_WAYLAND"]))
                default = extras.get("default_render")
                if default:
                    idx = self.render_combo.findText(default)
                    if idx >= 0:
                        self.render_combo.setCurrentIndex(idx)

        # ===== Godot 专用操作 =====
        def _extras(self) -> Dict:
            """返回当前预设的 extras (空字典如果非 godot)。"""
            return getattr(self, "godot_extras", {})

        def _src_dir(self) -> str:
            """返回 cwd_edit 路径。"""
            return self.cwd_edit.text().strip() or os.getcwd()

        def on_godot_clean(self) -> None:
            """Clean: 删 .sconsign5.dblite + .o 文件。"""
            src = self._src_dir()
            reply = QMessageBox.question(
                self, "确认 Clean",
                f"删除 {src}/.sconsign5.dblite + .o 文件, 下次编译会全量重编。\n确认?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
            )
            if reply != QMessageBox.StandardButton.Yes:
                return
            removed = 0
            for pat in [".sconsign5.dblite", "bin/*.o"]:
                for p in glob.glob(str(Path(src) / pat)):
                    try:
                        os.remove(p)
                        removed += 1
                    except Exception:
                        pass
            self.statusBar().showMessage(f"🧹 Clean 完成, 删了 {removed} 个文件", 5000)
            _track("clean", "Clean (.sconsign5 + .o)",
                   desc=f"删 {removed} 个文件 in {src}",
                   node_id=self._current_node_id())

        def on_godot_deep_clean(self) -> None:
            """Deep Clean: 跑 scons -c + 删 .sconsign5 + 删 bin/ binary。"""
            src = self._src_dir()
            reply = QMessageBox.question(
                self, "深度 Clean (会清旧 binary)",
                f"在 {src} 执行:\n"
                f"  1) scons -c (清 .o + .d)\n"
                f"  2) 删 .sconsign5.dblite\n"
                f"  3) 删 bin/ 下的 binary\n\n"
                f"之后必须点 ▶ 开始编译 全量重编 (15-30 分钟)\n\n"
                f"确认执行?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
            )
            if reply != QMessageBox.StandardButton.Yes:
                return
            log_lines = []
            # Step 1: scons -c
            log_lines.append("=== Step 1: scons -c ===")
            try:
                r = subprocess.run(
                    ["scons", "-c", "platform=linuxbsd", "target=editor",
                     "module_cloud_viewer_native_enabled=yes", "-j2"],
                    cwd=src, capture_output=True, text=True, timeout=120
                )
                log_lines.append(f"exit: {r.returncode}")
                log_lines.append(r.stdout[-2000:] if r.stdout else "")
                log_lines.append(r.stderr[-1000:] if r.stderr else "")
            except Exception as e:
                log_lines.append(f"scons -c 失败: {e}")
            # Step 2: 删 .sconsign5.dblite
            log_lines.append("=== Step 2: 删 .sconsign5.dblite ===")
            for p in [".sconsign5.dblite", ".scons_node_count"]:
                full = Path(src) / p
                if full.exists():
                    try:
                        full.unlink()
                        log_lines.append(f"  删 {full}")
                    except Exception as e:
                        log_lines.append(f"  删 {full} 失败: {e}")
                else:
                    log_lines.append(f"  不存在: {full}")
            # Step 3: 删 bin/ binary
            log_lines.append("=== Step 3: 删 bin/ binary ===")
            bin_dir = Path(src) / "bin"
            removed = 0
            if bin_dir.exists():
                for pat in ["godot.linuxbsd.*", "*.so"]:
                    for p in glob.glob(str(bin_dir / pat)):
                        if os.path.isfile(p):
                            try:
                                os.remove(p)
                                log_lines.append(f"  删 {p}")
                                removed += 1
                            except Exception as e:
                                log_lines.append(f"  删 {p} 失败: {e}")
            log_lines.append(f"=== 完成: 删了 {removed} 个 binary ===")
            # 写日志
            for line in log_lines:
                self._on_message(CompileMessage(kind="info", text=line, raw=line, time_ms=0))
            self.statusBar().showMessage(
                f"🧹 Deep Clean 完成, 删了 {removed} 个 binary. 现在点 ▶ 全量重编", 5000)
            _track("deep_clean", f"Deep Clean 删 {removed} 个 binary",
                   desc=f"in {src}",
                   node_id=self._current_node_id())

        def on_godot_touch(self) -> None:
            """Touch editor_data.cpp + 自动重编。"""
            extras = self._extras()
            editor_cpp = extras.get("editor_data_cpp", "")
            if not editor_cpp or not os.path.exists(editor_cpp):
                QMessageBox.warning(self, "错误",
                                    f"找不到 editor_data.cpp:\n  {editor_cpp}\n请检查预设 extras.editor_data_cpp")
                return
            Path(editor_cpp).touch()
            mtime = os.path.getmtime(editor_cpp)
            ts = datetime.fromtimestamp(mtime).strftime("%H:%M:%S")
            self.statusBar().showMessage(f"👆 Touched {editor_cpp} @ {ts}, 启动编译...")
            _track("touch", f"Touch editor_data.cpp ({ts})",
                   desc=editor_cpp,
                   node_id=self._current_node_id())
            # 自动启编译
            self.start_compile()

        def on_godot_diagnose(self) -> None:
            """诊断: 检查 sconsign5 + .o + binary 状态, 不跑 scons。"""
            extras = self._extras()
            binary = extras.get("binary_path", "")
            editor_cpp = extras.get("editor_data_cpp", "")
            src = self._src_dir()
            log = []
            log.append("=== 🩺 scons 诊断 (文件系统检查) ===")
            # 1. sconsign5
            sconsign = Path(src) / ".sconsign5.dblite"
            log.append("--- 1. sconsign5.dblite 状态 ---")
            if sconsign.exists():
                sz = sconsign.stat().st_size
                mt = datetime.fromtimestamp(sconsign.stat().st_mtime).strftime("%H:%M:%S")
                log.append(f"  ✓ 存在, 大小: {sz} 字节, mtime: {mt}")
            else:
                log.append(f"  ✗ 不存在: {sconsign}")
            # 2. .o
            log.append("--- 2. editor_data.cpp.o 状态 ---")
            o_files = list(Path(src).rglob("editor_data.cpp.o"))
            if o_files:
                for o in o_files:
                    sz = o.stat().st_size
                    mt = datetime.fromtimestamp(o.stat().st_mtime).strftime("%H:%M:%S")
                    log.append(f"  {o.relative_to(src)} (大小: {sz}, mtime: {mt})")
            else:
                log.append("  ✗ 没找到任何 editor_data.cpp.o")
            # 3. binary
            log.append("--- 3. Binary 状态 ---")
            if binary and os.path.exists(binary):
                sz = os.path.getsize(binary) / 1024 / 1024
                mt = datetime.fromtimestamp(os.path.getmtime(binary)).strftime("%H:%M:%S")
                log.append(f"  {os.path.basename(binary)}")
                log.append(f"  大小: {sz:.1f} MB, mtime: {mt}")
            else:
                log.append(f"  ✗ 不存在: {binary}")
            # 4. source
            log.append("--- 4. editor_data.cpp 状态 ---")
            if editor_cpp and os.path.exists(editor_cpp):
                sz = os.path.getsize(editor_cpp)
                mt = datetime.fromtimestamp(os.path.getmtime(editor_cpp)).strftime("%H:%M:%S")
                log.append(f"  {editor_cpp}")
                log.append(f"  大小: {sz} 字节, mtime: {mt}")
            # 5. 决策表
            log.append("--- 5. 决策表 ---")
            src_mt = os.path.getmtime(editor_cpp) if os.path.exists(editor_cpp) else 0
            o_mt = o_files[0].stat().st_mtime if o_files else 0
            bin_mt = os.path.getmtime(binary) if binary and os.path.exists(binary) else 0
            log.append(f"  source mtime: {datetime.fromtimestamp(src_mt).strftime('%H:%M:%S') if src_mt else 'N/A'}")
            log.append(f"  .o mtime:     {datetime.fromtimestamp(o_mt).strftime('%H:%M:%S') if o_mt else 'N/A'}")
            log.append(f"  binary mtime: {datetime.fromtimestamp(bin_mt).strftime('%H:%M:%S') if bin_mt else 'N/A'}")
            if src_mt > o_mt:
                log.append("  ⚠️  source > .o → 应该重编")
            elif src_mt == o_mt:
                log.append("  ⚠️  source == .o → 可能跳 (你刚改过?)")
            else:
                log.append("  ✓  source < .o → .o 是新的")
            if o_mt > bin_mt:
                log.append("  ⚠️  .o > binary → 应该重链")
            elif o_mt == bin_mt:
                log.append("  ⚠️  .o == binary → 可能跳")
            else:
                log.append("  ✓  .o < binary → binary 是新的")
            # 6. strings 检查
            log.append("--- 6. binary 含 fix 字符串? ---")
            fix_status = self._binary_contains_fix(binary, extras.get("fix_marker", "v5.11_editor_fix")) if binary else None
            log.append(f"  {fix_status}")
            # 7. 物理机手动命令
            log.append("--- 7. 物理机手动跑 ---")
            log.append("  rm -f .sconsign5.dblite bin/godot.linuxbsd.editor.x86_64.cloud_ros2")
            log.append(f"  scons platform=linuxbsd target=editor module_cloud_viewer_native_enabled=yes -j2 2>&1 | tail -20")
            # 输出
            for line in log:
                self._on_message(CompileMessage(kind="info", text=line, raw=line, time_ms=0))
            # 弹窗 (短)
            QMessageBox.information(
                self, "诊断完成",
                f"  source mtime: {datetime.fromtimestamp(src_mt).strftime('%H:%M:%S') if src_mt else 'N/A'}\n"
                f"  .o mtime:     {datetime.fromtimestamp(o_mt).strftime('%H:%M:%S') if o_mt else 'N/A'}\n"
                f"  binary mtime: {datetime.fromtimestamp(bin_mt).strftime('%H:%M:%S') if bin_mt else 'N/A'}\n\n"
                f"看 log 区下面 '🩺 诊断' 完整输出"
            )
            _track("diagnose", "scons 诊断",
                   desc=f"src={src_mt:.0f} o={o_mt:.0f} bin={bin_mt:.0f} fix={fix_status}",
                   node_id=self._current_node_id())

        def _binary_contains_fix(self, binary: str, marker: str) -> Optional[str]:
            """检查 binary 是否含 fix 字符串 (用 strings 命令)。
            返回: "✓ 含 <marker>" / "✗ 不含 <marker>" / "? strings 命令缺失"
            """
            if not binary or not os.path.exists(binary):
                return f"? binary 不存在: {binary}"
            try:
                r = subprocess.run(["strings", binary], capture_output=True, text=True, timeout=10)
                if marker in r.stdout:
                    return f"✓ 含 {marker}"
                return f"✗ 不含 {marker}"
            except Exception:
                return "? strings 命令缺失"

        def _check_binary_for_editor_mode(self, mode: str) -> tuple:
            """editor 模式: 检查 binary 是否含 fix 字符串 + mtime。
            返回 (is_ok, message)。
            """
            if mode != "editor":
                return (True, "")
            extras = self._extras()
            binary = extras.get("binary_path", "")
            editor_cpp = extras.get("editor_data_cpp", "")
            if not binary or not os.path.exists(binary):
                return (False, f"Binary 不存在:\n  {binary}\n\n必须先编译!")
            if not editor_cpp or not os.path.exists(editor_cpp):
                return (False, f"找不到 editor_data.cpp:\n  {editor_cpp}")
            fix_status = self._binary_contains_fix(binary, extras.get("fix_marker", "v5.11_editor_fix"))
            if "✓" in fix_status:
                return (True, "")
            if "✗" in fix_status:
                return (False,
                    f"⚠️ Binary 不含 {extras.get('fix_marker', 'fix')}, 启 editor 可能 SIGSEGV!\n\n"
                    f"  fix 状态: {fix_status}\n\n"
                    f"必须先点 ▶ 开始编译 重编 binary, 再启 editor。\n"
                    f"或者临时选 [game] 模式。")
            return (True, f"(fix 字符串未确认: {fix_status})")

        def on_godot_launch(self) -> None:
            """启动 Godot (选 render + mode, 可能弹 fix 检查)。"""
            extras = self._extras()
            launch_script = extras.get("launch_script")
            if not launch_script or not os.path.exists(launch_script):
                QMessageBox.warning(self, "错误", f"找不到 launch_script:\n  {launch_script}")
                return
            render = self.render_combo.currentText()
            mode = "editor" if "editor" in self.mode_combo.currentText() else "game"
            # editor 模式检查
            is_ok, msg = self._check_binary_for_editor_mode(mode)
            if not is_ok:
                reply = QMessageBox.question(
                    self, "Binary 警告",
                    msg + "\n\n选 Yes 重编, No 强制启动, Cancel 取消",
                    QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No | QMessageBox.StandardButton.Cancel,
                    QMessageBox.StandardButton.Yes
                )
                if reply == QMessageBox.StandardButton.Yes:
                    self.start_compile()
                    return
                if reply == QMessageBox.StandardButton.Cancel:
                    return
                # No = 强制启动
            env = os.environ.copy()
            env["RENDER_MODE"] = render
            if mode == "editor":
                env["USE_EDITOR"] = "yes"
            cmd = ["/bin/bash", launch_script]
            self.statusBar().showMessage(f"🚀 启动 Godot ({render}, {mode})...")
            try:
                subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.statusBar().showMessage(f"✓ Godot 已启动 ({render}, {mode}, detach)", 5000)
                _track("launch", f"启动 Godot ({render}, {mode})",
                       desc=launch_script,
                       node_id=self._current_node_id())
            except Exception as e:
                QMessageBox.critical(self, "启动失败", str(e))
                _track("error", f"启动 Godot 失败",
                       desc=str(e),
                       node_id=self._current_node_id())

        def on_godot_launch_ui(self) -> None:
            """启动 ui.py (主 GUI 工具)。"""
            extras = self._extras()
            ui_script = extras.get("ui_script", "/home/bv/code/godot_ui_linux/ui.py")
            if not os.path.exists(ui_script):
                QMessageBox.warning(self, "错误", f"找不到 ui.py:\n  {ui_script}")
                return
            try:
                subprocess.Popen(
                    ["python3", ui_script],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
                )
                self.statusBar().showMessage(f"✓ ui.py 已启动", 3000)
                _track("launch", "启动 ui.py", desc=ui_script,
                       node_id=self._current_node_id())
            except Exception as e:
                QMessageBox.critical(self, "启动失败", str(e))

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
            cwd = self.cwd_edit.text().strip() or os.getcwd()

            # 拆分 shell 风格的 VAR=value 前缀
            extra_env, cmd = parse_env_prefixes(cmd_str)
            if not cmd:
                QMessageBox.warning(self, "错误",
                    f"命令解析后为空, 检查 VAR=value 前缀是否正确:\n{cmd_str}")
                return
            if extra_env:
                # 把 env 写回命令显示 (剥掉前缀, 等价)
                shown = " ".join(f"{k}={v}" for k, v in extra_env.items()) + " " + " ".join(cmd)
                self.cmd_edit.setText(shown)

            # ===== 立即的视觉反馈 (用户点完立刻看到东西) =====
            preset_name = self.preset_combo.currentText() if hasattr(self, "preset_combo") else ""
            self.clear_messages()
            # 1. 日志顶部插一行 "▶ 开始"
            start_time_str = time.strftime("%H:%M:%S")
            self._on_message(CompileMessage(
                kind="info",
                text=f"▶ 开始编译: {preset_name or cmd_str}",
                raw="",
                time_ms=0,
            ))
            if extra_env:
                env_str = ", ".join(f"{k}={v}" for k, v in extra_env.items())
                self._on_message(CompileMessage(
                    kind="info",
                    text=f"🔧 环境变量: {env_str}",
                    raw="",
                    time_ms=0,
                ))
            cwd_disp = cwd.replace(os.path.expanduser("~"), "~")
            self._on_message(CompileMessage(
                kind="info",
                text=f"📂 工作目录: {cwd_disp}",
                raw="",
                time_ms=0,
            ))
            # 2. 进度条归零
            self.progress.setRange(0, 100)
            self.progress.setValue(0)
            self.progress.setFormat("准备中… %p%")
            # 3. 按钮状态
            self.act_start.setEnabled(False)
            self.act_stop.setEnabled(True)
            # 4. 状态栏 + 窗口标题
            self.statusBar().showMessage(f"▶ 编译中: {cmd_str}  (cwd: {cwd_disp})")
            self.setWindowTitle(f"[编译中…] 步骤跟踪工具 - {preset_name or 'compile_tool'}")
            # 5. 计时
            self._start_ts = time.time()
            self._elapsed_timer = QTimer(self)
            self._elapsed_timer.timeout.connect(self._update_elapsed)
            self._elapsed_timer.start(100)  # 100ms 刷新

            # ===== 记录开始事件 =====
            _track("compile_start",
                   f"开始编译: {preset_name}" if preset_name else "开始编译",
                   desc=cmd_str,
                   node_id=self._current_node_id())

            # ===== 启动 runner (后台线程读输出) =====
            self.runner = CompileRunner(cmd, cwd=cwd, env=extra_env if extra_env else None)
            self.runner.on_message = self._on_message_thread
            self.runner.on_progress = self._on_progress_thread
            self.runner.on_finish = self._on_finish_thread
            self.runner.start()
            # 启动后: 把进度条设为"不确定"模式, 等第一条 scons 输出
            self.progress.setRange(0, 0)
            self.progress.setFormat("编译中… 等待 scons 输出…")

        def stop_compile(self) -> None:
            """停止编译。"""
            if self.runner:
                self.runner.stop()
                self.statusBar().showMessage("⏹ 已请求停止", 3000)
                _track("compile_stop", "手动停止编译",
                       desc=self.cmd_edit.text().strip()[:200],
                       node_id=self._current_node_id())

        # ============== Detached 守护进程编译 (抗 sandbox SIGTERM) ==============
        def start_detached(self) -> None:
            """启 detached daemon 跑当前预设命令, 关 GUI 不杀编译。"""
            # daemon 是 subprocess 启的 (line 1848+), 不需要 import build_daemon

            # 检查 daemon 是否已在跑 (2026-08-19: 双重检查 — os.kill 容易误判 PID 复用)
            ai_dir = Path("/home/bv/code/godot_ui_linux/.ai_tools")
            pid_file = ai_dir / "build_daemon.pid"
            if pid_file.exists():
                stale = True
                try:
                    old_pid = int(pid_file.read_text().strip())
                    # 1) 进程必须存活
                    os.kill(old_pid, 0)
                    # 2) cmdline 必须含 build_daemon (防 PID 复用成 scons)
                    try:
                        with open(f"/proc/{old_pid}/cmdline", "rb") as _f:
                            cmdline = _f.read().decode("utf-8", errors="replace")
                        if "build_daemon" in cmdline:
                            stale = False
                    except (FileNotFoundError, ProcessLookupError, PermissionError):
                        pass
                except (ProcessLookupError, ValueError, PermissionError, OSError):
                    pass
                if not stale:
                    QMessageBox.warning(self, "提示",
                        f"Daemon 已在运行 (pid={old_pid})\n先点 ⏹ 停 Daemon 再启新的")
                    return
                # stale: 清掉旧 pid 文件, 继续启新的
                try:
                    pid_file.unlink()
                    self.statusBar().showMessage(f"🧹 清掉 stale pid 文件, 继续启动", 3000)
                except Exception:
                    pass

            # 解析当前命令
            cmd_str = self.cmd_edit.text().strip()
            if not cmd_str:
                QMessageBox.warning(self, "提示", "请先选个 godot 预设 (会自动填命令)")
                return
            cwd = self.cwd_edit.text().strip() or os.getcwd()
            extra_env, cmd = parse_env_prefixes(cmd_str)
            if not cmd:
                QMessageBox.warning(self, "错误", f"命令解析为空:\n{cmd_str}")
                return

            # 准备 log 文件 (daemon 自己写, 这里只是占位)
            log_dir = ai_dir / "build_logs"
            log_dir.mkdir(parents=True, exist_ok=True)

            # 启 build_daemon (start_new_session=True 脱离父进程, 抗 sandbox SIGTERM)
            daemon_script = Path("/home/bv/code/godot_ui_linux/build_daemon.py")
            if not daemon_script.exists():
                QMessageBox.critical(self, "错误", f"找不到 build_daemon.py:\n{daemon_script}")
                return
            daemon_cmd = [
                "python3", str(daemon_script),
                "--cmd", " ".join(cmd),
                "--cwd", cwd,
                "--status-file", str(ai_dir / "build_status.json"),
            ]
            # daemon 会自己生成 log_path 并写入 status, 这里先用 None 占位, 启动后从 status file 读
            try:
                proc = subprocess.Popen(
                    daemon_cmd,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    stdin=subprocess.DEVNULL,
                    cwd="/home/bv/code/godot_ui_linux",
                    start_new_session=True,
                )
            except Exception as e:
                QMessageBox.critical(self, "错误", f"启 daemon 失败:\n{e}")
                return
            time.sleep(0.5)
            if proc.poll() is not None:
                # 启动秒退 — 尝试读 status
                try:
                    s = json.loads((ai_dir / "build_status.json").read_text())
                    QMessageBox.critical(self, "错误",
                        f"Daemon 秒退 (rc={proc.returncode}):\n状态: {s.get('status')}\n日志: {s.get('log_path')}")
                except Exception:
                    QMessageBox.critical(self, "错误", f"Daemon 秒退 (rc={proc.returncode})")
                return
            # 从 status file 读 log_path
            log_path = None
            try:
                _s = json.loads((ai_dir / "build_status.json").read_text())
                log_path = Path(_s.get("log_path", "")) if _s.get("log_path") else None
            except Exception:
                pass
            if log_path is None or not log_path.exists():
                QMessageBox.critical(self, "错误", f"Daemon 启动但 log 路径无效:\n{log_path}")
                return

            # 记录状态
            self.daemon_log_path = log_path
            self.daemon_log_pos = 0
            self.daemon_parser = OutputParser()
            self.daemon_start_ts = time.time()

            # UI 准备
            self.clear_messages()
            preset_name = self.preset_combo.currentText() if hasattr(self, "preset_combo") else ""
            self._on_message(CompileMessage(
                kind="info",
                text=f"🚀 Detached 编译已启动: {preset_name or cmd_str}  (daemon pid={proc.pid})",
                raw="", time_ms=0,
            ))
            self._on_message(CompileMessage(
                kind="info",
                text=f"📄 日志: {log_path}",
                raw="", time_ms=0,
            ))
            self.progress.setRange(0, 0)
            self.progress.setFormat("Detached 编译中… 等待 scons 输出…")
            self.act_start.setEnabled(False)
            self.act_detached.setEnabled(False)
            self.act_stop_detached.setEnabled(True)
            self.statusBar().showMessage(f"🚀 Detached pid={proc.pid}, 关 GUI 不杀编译")
            self.setWindowTitle(f"[Detached 编译中…] 图形化编译工具")

            # 启动计时
            self._start_ts = time.time()
            if self._elapsed_timer:
                self._elapsed_timer.stop()
            self._elapsed_timer = QTimer(self)
            self._elapsed_timer.timeout.connect(self._update_elapsed)
            self._elapsed_timer.start(100)

            # 记录事件
            _track("detached_start", f"启动 detached: {preset_name}" if preset_name else "启动 detached",
                   desc=f"daemon pid={proc.pid}\nlog={log_path}",
                   node_id=self._current_node_id())

            # 监控 log 文件 (QFileSystemWatcher + 1s 兜底)
            if self.daemon_watcher is None:
                self.daemon_watcher = QFileSystemWatcher(self)
                self.daemon_watcher.fileChanged.connect(self._on_daemon_log_changed)
            if str(log_path) not in self.daemon_watcher.files():
                self.daemon_watcher.addPath(str(log_path))
            if self.daemon_poll_timer is None:
                self.daemon_poll_timer = QTimer(self)
                self.daemon_poll_timer.timeout.connect(self._tail_daemon_log)
            self.daemon_poll_timer.start(1000)

        def _on_daemon_log_changed(self, path: str) -> None:
            """log 文件变化时立即 tail (QFileSystemWatcher)。"""
            if path not in self.daemon_watcher.files():
                self.daemon_watcher.addPath(path)  # editor-style 替换, 重新加
            self._tail_daemon_log()

        def _tail_daemon_log(self) -> None:
            """从上次位置 tail log, 解析每行通过 OutputParser。"""
            if not self.daemon_log_path or not self.daemon_parser:
                return
            try:
                if not self.daemon_log_path.exists():
                    return
                with open(self.daemon_log_path, "r", encoding="utf-8", errors="replace") as f:
                    f.seek(self.daemon_log_pos)
                    new_data = f.read()
                    self.daemon_log_pos = f.tell()
            except Exception:
                return
            if not new_data:
                return
            elapsed_ms = int((time.time() - self.daemon_start_ts) * 1000)
            for line in new_data.splitlines():
                if not line:
                    continue
                try:
                    msg = self.daemon_parser.parse(line, elapsed_ms)
                except Exception as e:
                    self._on_message(CompileMessage(
                        kind="warning", text=f"[parse err] {e}: {line[:200]}",
                        raw=line, time_ms=elapsed_ms,
                    ))
                    continue
                self._on_message(msg)
                # 进度同步
                if self.daemon_parser.total_steps > 0:
                    self.bus.progress_updated.emit(
                        self.daemon_parser.current_step,
                        self.daemon_parser.total_steps,
                    )
                # 检测终态 (daemon 写 final status)
                if "✗ 编译结束:" in line or "✓ 编译结束:" in line:
                    self._finalize_detached()

        def _finalize_detached(self) -> None:
            """读取 status file, 收尾 UI。"""
            ai_dir = Path("/home/bv/code/godot_ui_linux/.ai_tools")
            status_file = ai_dir / "build_status.json"
            result = None
            if status_file.exists():
                try:
                    s = json.loads(status_file.read_text())
                    status = s.get("status", "unknown")
                    exit_code = s.get("exit_code", -1)
                    errs = s.get("errors", 0)
                    warns = s.get("warnings", 0)
                    result = CompileResult(
                        success=(status == "success" and exit_code == 0),
                        exit_code=exit_code,
                        duration_ms=int((time.time() - self.daemon_start_ts) * 1000),
                        total_lines=self.daemon_parser.current_step if self.daemon_parser else 0,
                        errors=errs,
                        warnings=warns,
                        files_seen=[],
                        messages=list(self.current_messages),
                    )
                except Exception as e:
                    self._on_message(CompileMessage(
                        kind="warning", text=f"[finalize] 读 status 失败: {e}",
                        raw="", time_ms=0,
                    ))
            if result:
                self.current_result = result
                self.bus.finished.emit(result)
            # 停 poll timer
            if self.daemon_poll_timer:
                self.daemon_poll_timer.stop()
            # 按钮恢复
            self.act_start.setEnabled(True)
            self.act_detached.setEnabled(True)
            self.act_stop_detached.setEnabled(False)
            self.setWindowTitle("图形化编译工具 - Compile Tool")
            if self._elapsed_timer:
                self._elapsed_timer.stop()

        def stop_detached(self) -> None:
            """发 SIGTERM 给 daemon (杀子进程组, 优雅停止)。"""
            ai_dir = Path("/home/bv/code/godot_ui_linux/.ai_tools")
            pid_file = ai_dir / "build_daemon.pid"
            if not pid_file.exists():
                self.statusBar().showMessage("⚠ 没找到 daemon pid 文件", 3000)
                return
            try:
                pid = int(pid_file.read_text().strip())
            except Exception:
                self.statusBar().showMessage("⚠ pid 文件格式错", 3000)
                # 2026-08-19: 格式错也清掉, 免得下次启动卡 stale 检查
                try: pid_file.unlink()
                except Exception: pass
                return
            try:
                # 用 SIGTERM, daemon 内部会 killpg 子进程
                os.kill(pid, signal.SIGTERM)
                self.statusBar().showMessage(f"⏹ 已发 SIGTERM 给 daemon (pid={pid})", 3000)
                self._on_message(CompileMessage(
                    kind="warning", text=f"⏹ 已请求停止 daemon (pid={pid})",
                    raw="", time_ms=0,
                ))
                # 2026-08-19: 主动清 pid 文件, 防 PID 复用让下次 start_detached 误判
                # daemon 是 start_new_session, 不会被 GUI 拖死, 进程组会被 SIGTERM 杀掉
                try: pid_file.unlink()
                except Exception: pass
            except ProcessLookupError:
                self.statusBar().showMessage(f"⚠ pid={pid} 已不存在", 3000)
                # 2026-08-19: 已死, 清掉 pid 文件
                try: pid_file.unlink()
                except Exception: pass

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
                _track("save_log", "手动保存日志", desc=log,
                       node_id=self._current_node_id())
            except Exception as e:
                QMessageBox.critical(self, "错误", f"保存失败: {e}")
                _track("error", "保存日志失败", desc=str(e),
                       node_id=self._current_node_id())

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
            # 用 Qt.UserRole 存 kind, QSS 按 [kind="xxx"] 着色 (这样选中态能正确显示)
            item.setData(Qt.ItemDataRole.UserRole, msg.kind)
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
            # 进度条
            self.progress.setRange(0, 100)
            self.progress.setValue(100)
            ok = result.success
            dur_s = result.duration_ms / 1000.0
            self.progress.setFormat(
                f"{'✓ 成功' if ok else f'✗ 失败 (退出码 {result.exit_code})'} · "
                f"耗时 {dur_s:.1f}s · 错误 {result.errors} · 警告 {result.warnings}"
            )
            if ok:
                self.progress.setStyleSheet(f"QProgressBar::chunk {{ background: {COLOR_SUCCESS}; }}")
            else:
                self.progress.setStyleSheet(f"QProgressBar::chunk {{ background: {COLOR_ERROR}; }}")
            # 顶部插一条总结行
            preset_name = self.preset_combo.currentText() if hasattr(self, "preset_combo") else ""
            self._on_message(CompileMessage(
                kind="success" if ok else "error",
                text=f"{'✓' if ok else '✗'} 编译结束: {preset_name or '(无预设)'} · "
                     f"耗时 {dur_s:.1f}s · 错误 {result.errors} · 警告 {result.warnings}",
                raw="",
                time_ms=result.duration_ms,
            ))
            # 自动保存日志
            log_path = ""
            try:
                log, js = save_log(result)
                log_path = log
            except Exception as e:
                self.statusBar().showMessage(f"编译结束，但日志保存失败: {e}", 8000)
            # 状态栏
            msg = (
                f"{'✓' if ok else '✗'} 编译结束 · {preset_name or 'cmd'} · "
                f"耗时 {dur_s:.1f}s · 错误 {result.errors} · 警告 {result.warnings}"
            )
            self.statusBar().showMessage(msg, 0)  # 不自动消失, 用户能看到
            # 窗口标题
            self.setWindowTitle(
                f"{'✓' if ok else '✗'} 步骤跟踪工具 - {preset_name or 'compile_tool'} "
                f"({dur_s:.1f}s)"
            )
            # 弹通知 (QMessageBox 阻塞, 但加 setWindowState 确保可见)
            box = QMessageBox(self)
            box.setWindowTitle("编译结束")
            box.setIcon(QMessageBox.Icon.Information if ok else QMessageBox.Icon.Critical)
            box.setText(
                f"{'✓ 编译成功' if ok else f'✗ 编译失败 (退出码 {result.exit_code})'}\n\n"
                f"预设: {preset_name or '(无)'}\n"
                f"耗时: {dur_s:.1f}s\n"
                f"错误: {result.errors}  警告: {result.warnings}\n"
            )
            if log_path:
                box.setDetailedText(f"日志: {log_path}")
            box.setStandardButtons(QMessageBox.StandardButton.Ok)
            box.show()  # 非阻塞
            box.raise_()
            box.activateWindow()
            # 记录结束事件
            ev_type = "compile_finish" if ok else "compile_fail"
            _track(ev_type,
                   f"{'✓' if ok else '✗'} 编译结束 ({preset_name or 'cmd'})",
                   desc=f"耗时 {dur_s:.1f}s · 错误 {result.errors} · 警告 {result.warnings}",
                   node_id=self._current_node_id())

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

        def _on_msg_list_context_menu(self, pos) -> None:
            """消息列表右键菜单: 复制 / 复制全部 / 全选 / 跳到文件。"""
            menu = QMenu(self.msg_list)
            act_copy = menu.addAction("📋 复制选中 (Ctrl+C)")
            act_copy_all = menu.addAction("📋 复制全部消息")
            act_copy_raw = menu.addAction("📋 复制原始输出 (raw)")
            menu.addSeparator()
            act_select_all = menu.addAction("✓ 全选 (Ctrl+A)")
            menu.addSeparator()
            # 跳到文件 (只对有 file 的项)
            idx_at_pos = self.msg_list.indexAt(pos)
            if idx_at_pos.isValid():
                row = idx_at_pos.row()
                if 0 <= row < len(self.current_messages):
                    msg = self.current_messages[row]
                    if msg.file:
                        act_open = menu.addAction(f"📄 打开: {msg.file}:{msg.line}")
                        act_open.triggered.connect(lambda: self._open_file_at(msg))
            menu.addSeparator()
            act_clear = menu.addAction("🗑 清空列表")

            sel_count = len(self.msg_list.selectedItems())
            act_copy.setEnabled(sel_count > 0)
            act_select_all.setEnabled(self.msg_list.count() > 0)

            chosen = menu.exec(self.msg_list.mapToGlobal(pos))
            if chosen == act_copy:
                self._copy_selected()
            elif chosen == act_copy_all:
                self._copy_all_messages()
            elif chosen == act_copy_raw:
                self._copy_raw_output()
            elif chosen == act_select_all:
                self.msg_list.selectAll()
            elif chosen == act_clear:
                self.clear_messages()

        def _copy_selected(self) -> None:
            """复制选中行的纯文本 (按列表顺序)。"""
            items = self.msg_list.selectedItems()
            if not items:
                self.statusBar().showMessage("未选中任何行", 2000)
                return
            # 按行号排序
            items.sort(key=lambda it: self.msg_list.row(it))
            lines = [it.text() for it in items]
            QApplication.clipboard().setText("\n".join(lines))
            self.statusBar().showMessage(f"✓ 已复制 {len(lines)} 行到剪贴板", 3000)

        def _copy_all_messages(self) -> None:
            """复制整个消息列表 (按顺序,纯文本)。"""
            if self.msg_list.count() == 0:
                self.statusBar().showMessage("列表为空", 2000)
                return
            lines = [self.msg_list.item(i).text() for i in range(self.msg_list.count())]
            QApplication.clipboard().setText("\n".join(lines))
            self.statusBar().showMessage(f"✓ 已复制 {len(lines)} 行消息", 3000)

        def _copy_raw_output(self) -> None:
            """复制原始输出区(scons 的 stdout/stderr 完整日志)。"""
            text = self.raw_output.toPlainText()
            if not text:
                self.statusBar().showMessage("原始输出为空", 2000)
                return
            QApplication.clipboard().setText(text)
            self.statusBar().showMessage(f"✓ 已复制 {len(text)} 字符原始输出", 3000)

        def _open_file_at(self, msg) -> None:
            """在系统默认编辑器中打开消息指向的文件。"""
            import subprocess as _sp
            if not msg.file or not os.path.exists(msg.file):
                self.statusBar().showMessage(f"❌ 文件不存在: {msg.file}", 5000)
                return
            try:
                # 用 xdg-open (Linux) / open (Mac) / start (Windows)
                if sys.platform.startswith("linux"):
                    _sp.Popen(["xdg-open", msg.file])
                elif sys.platform == "darwin":
                    _sp.Popen(["open", msg.file])
                else:
                    os.startfile(msg.file)  # type: ignore
                self.statusBar().showMessage(f"📄 已打开: {msg.file}", 3000)
            except Exception as e:
                self.statusBar().showMessage(f"❌ 打开失败: {e}", 5000)

        def show_help(self) -> None:
            """显示帮助。"""
            QMessageBox.information(self, "用法",
                "快捷键 (msg_list 有焦点时):\n"
                "  Ctrl+R     开始编译\n"
                "  Ctrl+K     停止\n"
                "  Ctrl+L     清空\n"
                "  Ctrl+S     保存日志\n"
                "  Ctrl+C     复制选中行\n"
                "  Ctrl+A     全选\n"
                "  双击       复制 file:line:col\n"
                "  右键       菜单(复制/复制全部/复制 raw/全选/打开文件/清空)\n\n"
                "工具栏:\n"
                "  📊 任务追踪 - 打开 step_tracker.py (节点流程图 + 实时事件)\n"
                "  🎛 Godot UI - 打开 ui.py 主工具 (ros2 mock + 测试)\n\n"
                "日志自动保存到 build_logs/ 目录\n"
                "配置保存在 compile_presets.json\n"
                "事件记录到 step_data.json (可用 env TRACKER_DATA_FILE 切项目)\n\n"
                "CLI 用法:\n"
                "  python compile_tool.py run --cmd \"g++ main.cpp\"\n"
                "  python compile_tool.py run --preset hello-g++\n"
                "  python compile_tool.py presets")

        def open_tracker(self) -> None:
            """打开 step_tracker.py GUI (节点流程图 + 事件)。"""
            self._open_aux_script(
                APP_DIR / "step_tracker.py", "gui",
                label="📊 任务追踪",
                success_msg="📊 已启动任务追踪",
            )

        def open_godot_ui(self) -> None:
            """打开 ui.py (ros2 mock + 测试, 来自 godot extras.ui_script)。"""
            # 优先用 godot extras 里的 ui_script
            ui_script = ""
            try:
                extras = self._extras()
                ui_script = extras.get("ui_script", "")
            except Exception:
                pass
            if not ui_script or not os.path.exists(ui_script):
                QMessageBox.warning(self, "错误", f"找不到 ui.py:\n{ui_script or '(未配置)'}\n请选一个 godot 预设")
                return
            self._open_aux_script(
                Path(ui_script), None,
                label="🎛 Godot UI",
                success_msg="🎛 已启动 Godot UI",
            )

        # 跟踪已启动的辅助进程 (key=label, value=subprocess.Popen)
        _aux_procs: Dict[str, "_sp.Popen"] = {}

        def _open_aux_script(self, script_path: Path, sub_arg, label: str, success_msg: str) -> None:
            """通用: 启动辅助脚本 (step_tracker / ui.py), 隔离 tty, 监控存活。"""
            import subprocess as _sp
            if not script_path.exists():
                QMessageBox.warning(self, "错误", f"找不到脚本:\n{script_path}")
                return

            # 如果之前启动过且还活着, 不要重复启动
            old = self._aux_procs.get(label)
            if old is not None and old.poll() is None:
                self.statusBar().showMessage(
                    f"{label} 已在运行 (pid={old.pid}), 切到该窗口查看 (关闭后这里会自动更新)", 5000)
                return

            # 输出日志: 写到 COMPILE_LOG_DIR/aux_<safe>.log (避免污染 tty)
            log_dir = Path(os.environ.get("COMPILE_LOG_DIR", APP_DIR / "build_logs"))
            log_dir.mkdir(parents=True, exist_ok=True)
            # 用稳定 key 而非带 emoji 的 label (避免 unicode 路径问题)
            safe_key = {
                "📊 任务追踪": "step_tracker",
                "🎛 Godot UI": "godot_ui",
            }.get(label, "aux")
            log_path = log_dir / f"aux_{safe_key}.log"
            try:
                log_f = open(log_path, "ab", buffering=0)
            except Exception as e:
                QMessageBox.warning(self, "错误", f"无法创建日志文件: {e}")
                return

            try:
                # 关键: 隔离 stdin (防争 tty), stdout/stderr → log file (防污染 terminal)
                # start_new_session=True → 创建新进程组, 不受父进程 Ctrl+C/SIGHUP 影响
                cmd = [sys.executable, str(script_path)]
                if sub_arg:
                    cmd.append(sub_arg)
                env = os.environ.copy()
                proc = _sp.Popen(
                    cmd, env=env,
                    stdin=_sp.DEVNULL,
                    stdout=log_f, stderr=_sp.STDOUT,
                    start_new_session=True,
                    close_fds=True,
                )
            except Exception as e:
                log_f.close()
                QMessageBox.warning(self, "错误", f"启动失败: {e}")
                return

            self._aux_procs[label] = proc
            self.statusBar().showMessage(
                f"{success_msg} (pid={proc.pid}, 日志: {log_path.name})", 5000)
            _track("aux_open", f"启动辅助: {label}",
                   desc=f"pid={proc.pid} script={script_path.name} log={log_path}",
                   node_id=self._current_node_id())

            # 启动后 500ms 检查一次, 确认进程没秒崩
            def _check_startup():
                if proc.poll() is not None:
                    rc = proc.returncode
                    self._aux_procs.pop(label, None)
                    msg = f"❌ {label} 启动后秒退 (rc={rc}), 日志: {log_path}"
                    self.statusBar().showMessage(msg, 10000)
                    QMessageBox.warning(self, "启动失败",
                        f"{label} 启动后秒退 (rc={rc})\n\n查看日志:\n{log_path}")
                    _track("error", f"{label} 启动秒退",
                           desc=f"rc={rc} log={log_path}",
                           node_id=self._current_node_id())
            QTimer.singleShot(500, _check_startup)

            # 启动一个 2 秒间隔的存活监控 (只在标签页被跟踪时跑, 关掉就清)
            self._start_aux_monitor()

        def _start_aux_monitor(self) -> None:
            """启动/重启辅助进程存活监控 (2 秒间隔)。"""
            if getattr(self, "_aux_monitor_timer", None) is not None:
                return  # 已经在跑
            t = QTimer(self)
            t.setInterval(2000)
            def _tick():
                dead = []
                for label, p in list(self._aux_procs.items()):
                    if p.poll() is not None:
                        rc = p.returncode
                        dead.append((label, p.pid, rc))
                for label, pid, rc in dead:
                    self._aux_procs.pop(label, None)
                    self.statusBar().showMessage(
                        f"⏹ {label} 已退出 (pid={pid}, rc={rc})", 5000)
                    _track("aux_close", f"辅助退出: {label}",
                           desc=f"pid={pid} rc={rc}",
                           node_id=self._current_node_id())
                # 全部清空后停掉 timer
                if not self._aux_procs:
                    t.stop()
                    t.deleteLater()
                    self._aux_monitor_timer = None
            t.timeout.connect(_tick)
            self._aux_monitor_timer = t
            t.start()

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
                "note": COLOR_NOTE,
                "info": COLOR_INFO,
                "success": COLOR_SUCCESS,
            }.get(kind, COLOR_TEXT)

    # 需要 QInputDialog (从 _qt_compat 已导入, 此处仅引用)
    QInputDialog  # 防止未使用警告

    app = QApplication.instance() or QApplication(sys.argv)
    win = MainWindow()
    if os.environ.get("COMPILE_TOOL_TEST_WIN_REF"):
        # 测试模式: 把 win 引用挂到 module, 便于 offscreen 自测
        sys.modules[__name__]._test_win_ref = win
    win.show()
    sys.exit(getattr(app, APP_EXEC)())


def _test_detached_flow() -> int:
    """(offscreen) 测 detached 编译流程。

    测 3 件事:
      A. build_daemon.py 能启 + 写 status + 写 log
      B. OutputParser 能解析 daemon log 行 (scons 进度 + error)
      C. compile_tool GUI 加载 OK + act_detached 按钮存在

    不测真实 GUI 集成 (那个要人工测, 因为 MainWindow 嵌套在 run_gui() 里)。
    """
    import subprocess
    ai_dir = Path("/home/bv/code/godot_ui_linux/.ai_tools")
    (ai_dir / "build_status.json").unlink(missing_ok=True)
    (ai_dir / "build_daemon.pid").unlink(missing_ok=True)

    # ============== A. daemon 测 ==============
    print("=" * 60)
    print("A. build_daemon.py 测试 (echo-hello + sleep 0.3)")
    print("=" * 60)
    proc = subprocess.run(
        ["python3", "/home/bv/code/godot_ui_linux/build_daemon.py",
         "--cmd", "echo 'compile_tool test line 1'; echo 'line 2'; sleep 0.3; echo 'line 3'",
         "--cwd", "/tmp",
         "--status-file", str(ai_dir / "build_status.json")],
        capture_output=True, text=True, timeout=10,
    )
    print(f"  daemon rc={proc.returncode}")
    if not (ai_dir / "build_status.json").exists():
        print("  ❌ TEST FAIL: status 文件没生成")
        print(f"  stderr: {proc.stderr[:300]}")
        return 1
    status = json.loads((ai_dir / "build_status.json").read_text())
    log_path = Path(status.get("log_path", ""))
    print(f"  status={status.get('status')!r}  exit={status.get('exit_code')}")
    print(f"  log_path={log_path}")
    if status.get("status") != "success" or status.get("exit_code") != 0:
        print(f"  ❌ TEST FAIL: status 不是 success")
        return 1
    if not log_path.exists():
        print(f"  ❌ TEST FAIL: log 文件没生成: {log_path}")
        return 1
    print(f"  ✅ daemon 测通过: status=success exit=0, log={log_path.name}")

    # ============== B. OutputParser 测 ==============
    print()
    print("=" * 60)
    print("B. OutputParser 测 (解析 daemon log)")
    print("=" * 60)
    parser = OutputParser()
    log_lines = log_path.read_text(errors="replace").splitlines()
    print(f"  log 行数: {len(log_lines)}")
    for i, line in enumerate(log_lines[:5]):
        try:
            msg = parser.parse(line, i * 100)
            print(f"    [{i}] kind={msg.kind!r} text={msg.text[:60]!r}")
        except Exception as e:
            print(f"    [{i}] parse err: {e}")
    print(f"  parser 状态: errors={parser.errors} warnings={parser.warnings} steps={parser.current_step}/{parser.total_steps}")
    # 注: echo 输出不会触发 GCC/CMake 错误模式, 应该 errors=0 warnings=0
    if parser.errors != 0:
        print(f"  ⚠ parser 误报 errors (echo 不应该有 error): {parser.errors}")

    # 测 scons 进度行能否正确解析
    print()
    print("  测 scons 进度行 [N/M] 解析:")
    for fake_line in [
        "[1234/16599] [  7%] Compiling platform/linuxbsd/godot_linuxbsd.cpp ...",
        "[1500/16599] [  9%] Linking Static Library obj/scene/libmain.linuxbsd.editor.x86_64.a ...",
        "main/main.cpp:2004:22: warning: declaration of 'foo' shadows a global declaration [-Wshadow]",
        "/path/to/file.cpp:42:5: error: undefined reference to `bar()'",
    ]:
        msg = parser.parse(fake_line, 5000)
        print(f"    kind={msg.kind!r:10}  file={msg.file!r:30}  text={msg.text[:50]!r}")
    if parser.errors >= 1 and parser.warnings >= 1:
        print(f"  ✅ parser 测通过: errors={parser.errors} warnings={parser.warnings}")
    else:
        print(f"  ❌ TEST FAIL: parser 没识别 error/warning (errors={parser.errors} warnings={parser.warnings})")
        return 1

    # ============== C. compile_tool 模块 + 预设加载测 (offscreen) ==============
    print()
    print("=" * 60)
    print("C. compile_tool 模块 + 预设 + 按钮定义测 (offscreen)")
    print("=" * 60)
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    os.environ["COMPILE_TOOL_TEST_WIN_REF"] = "1"
    os.environ.setdefault("COMPILE_PRESETS_FILE", str(ai_dir / "compile_presets.json"))

    # 验证模块属性 (不实际启 GUI, 因为 Qt 必须主线程 + run_gui 内部 exec_ 阻塞)
    self_mod = sys.modules[__name__]
    print(f"  ✅ module 加载 OK: {self_mod.__file__.split('/')[-1]}")

    # 验证 PresetManager 能读预设
    pm = PresetManager(config_file=ai_dir / "compile_presets.json")
    presets = pm.list_presets()
    print(f"  预设数: {len(presets)}")
    has_godot = any(p["name"] == "godot-editor-build" for p in presets)
    if not has_godot:
        print(f"  ❌ TEST FAIL: 没找到 godot-editor-build 预设")
        return 1
    g_preset = next(p for p in presets if p["name"] == "godot-editor-build")
    print(f"  godot-editor-build: {g_preset['command'][:60]}...")
    if "extra_suffix=cloud_ros2" not in g_preset["command"]:
        print(f"  ❌ TEST FAIL: godot-editor-build 命令缺 extra_suffix=cloud_ros2")
        return 1
    print(f"  ✅ godot-editor-build 含 extra_suffix=cloud_ros2 (修复已生效)")

    # 验证 act_detached 字符串在源码里 (因为 MainWindow 在 run_gui() 内, 没法直接 import)
    src = (Path(self_mod.__file__)).read_text()
    if "🚀 Detached 编译" not in src:
        print(f"  ❌ TEST FAIL: 源码里没找到 '🚀 Detached 编译' 按钮定义")
        return 1
    if "def start_detached" not in src:
        print(f"  ❌ TEST FAIL: 源码里没找到 'def start_detached' 方法定义")
        return 1
    if "def _tail_daemon_log" not in src:
        print(f"  ❌ TEST FAIL: 源码里没找到 'def _tail_daemon_log' 方法定义")
        return 1
    if "def stop_detached" not in src:
        print(f"  ❌ TEST FAIL: 源码里没找到 'def stop_detached' 方法定义")
        return 1
    print(f"  ✅ 源码含 detached 按钮 + start_detached / _tail_daemon_log / stop_detached 方法")

    # 验证 parse_env_prefixes (start_detached 用到)
    extra_env, cmd = parse_env_prefixes("GUS_PROGRESS_FORCE=1 scons platform=linuxbsd -j2")
    if extra_env.get("GUS_PROGRESS_FORCE") != "1" or "scons" not in cmd:
        print(f"  ❌ TEST FAIL: parse_env_prefixes 解析错: env={extra_env} cmd={cmd}")
        return 1
    print(f"  ✅ parse_env_prefixes 正确: env={dict(extra_env)} cmd={cmd}")
    print()
    print("=" * 60)
    print("✅ 全部测试通过!")
    print("=" * 60)
    return 0


# ===== CLI 部分 =====
def run_cli(args: List[str]) -> int:
    """处理命令行调用。"""
    # 提前过滤 GUI 测试标志（避免 argparse 报 unknown arg）
    if "--offscreen" in args:
        os.environ["QT_QPA_PLATFORM"] = "offscreen"
        args = [a for a in args if a != "--offscreen"]

    parser = argparse.ArgumentParser(
        prog="compile_tool.py",
        description="图形化编译工具 CLI - 供 AI 或脚本调用",
    )
    parser.add_argument("--data-file", default=None,
                        help="step_data.json 路径 (env TRACKER_DATA_FILE 也可, 默认 /home/bv/code/ai_tools/step_data.json)")
    parser.add_argument("--config", default=None, dest="config_file",
                        help=f"预设文件 (env COMPILE_PRESETS_FILE 也可, 默认 {DEFAULT_CONFIG_FILE})")
    parser.add_argument("--log-dir", default=None, dest="log_dir",
                        help=f"日志目录 (env COMPILE_LOG_DIR 也可, 默认 {DEFAULT_LOG_DIR})")
    sub = parser.add_subparsers(dest="cmd")

    sub.add_parser("gui", help="启动图形界面")
    sub.add_parser("presets", help="列出所有预设")
    sub.add_parser("test-detached", help="(offscreen) 测 detached 编译流程: 启 echo-hello daemon, 验证状态同步, 自动退出")

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

    # ===== godot 子命令 (headless: clean / deep_clean / touch / diagnose / launch) =====
    p_godot = sub.add_parser("godot", help="Godot 专用操作 (clean/deep_clean/touch/diagnose/launch/launch-ui)")
    p_godot.add_argument("--preset", required=True, help="godot 预设名称 (e.g. godot-editor-build)")
    godot_sub = p_godot.add_subparsers(dest="godot_cmd", required=True)
    godot_sub.add_parser("clean", help="删 .sconsign5 + bin/*.o")
    godot_sub.add_parser("deep_clean", help="跑 scons -c + 删 .sconsign5 + 删 bin/ binary")
    godot_sub.add_parser("touch", help="touch editor_data.cpp 强制重编")
    godot_sub.add_parser("diagnose", help="检查 sconsign5/.o/binary 状态, strings 查 fix 字符串")
    p_godot_launch = godot_sub.add_parser("launch", help="启动 Godot (detach)")
    p_godot_launch.add_argument("--render", default="VULKAN_WAYLAND",
                                help="渲染器: VULKAN_WAYLAND / VULKAN_NVIDIA / VULKAN_SOFT / GLES3_NVIDIA / OPENGL3_NVIDIA / HEADLESS")
    p_godot_launch.add_argument("--mode", default="game", choices=["game", "editor"],
                                help="启动模式: game (3D 渲染) / editor (场景树/inspector)")
    godot_sub.add_parser("launch-ui", help="启动 ui.py 主 GUI 工具")

    parsed = parser.parse_args(args)

    # 全局: --data-file / --config / --log-dir 覆盖
    # (CLI > env > 默认)
    if parsed.data_file:
        if _TRACKER_OK:
            tracker_rec.set_data_file(parsed.data_file)
        # 同步设 env, 免得子进程读到不一致
        os.environ["TRACKER_DATA_FILE"] = str(Path(parsed.data_file).expanduser().resolve())
        print(f"📁 step_data: {tracker_rec.get_data_file() if _TRACKER_OK else parsed.data_file}",
              file=sys.stderr)
    if parsed.config_file:
        set_config_file(parsed.config_file)
        os.environ["COMPILE_PRESETS_FILE"] = str(CONFIG_FILE)
        print(f"📁 预设文件: {CONFIG_FILE}", file=sys.stderr)
    if parsed.log_dir:
        set_log_dir(parsed.log_dir)
        os.environ["COMPILE_LOG_DIR"] = str(LOG_DIR)
        print(f"📁 日志目录: {LOG_DIR}", file=sys.stderr)

    pm = PresetManager()

    if parsed.cmd in (None, "gui"):
        run_gui()
        return 0

    if parsed.cmd == "presets":
        presets = pm.list_presets()
        print(f"共 {len(presets)} 个预设:")
        for p in presets:
            mark = " ★" if p["name"] == pm.data.get("last_preset") else ""
            ptype = f" [{p.get('type', 'generic')}]" if p.get("type") and p.get("type") != "generic" else ""
            desc = f"  - {p['description']}" if p.get("description") else ""
            print(f"  • {p['name']}{mark}{ptype}{desc}")
            print(f"      {p['command']}")
        return 0

    if parsed.cmd == "test-detached":
        return _test_detached_flow()

    if parsed.cmd == "add-preset":
        pm.add_preset(parsed.name, parsed.command, parsed.cwd, parsed.desc)
        print(f"✓ 已保存预设 {parsed.name}")
        _track("info", f"添加预设 {parsed.name}",
               desc=parsed.command, node_id=_cli_node_id())
        return 0

    if parsed.cmd == "del-preset":
        if pm.delete_preset(parsed.name):
            print(f"🗑 已删除预设 {parsed.name}")
            _track("info", f"删除预设 {parsed.name}", node_id=_cli_node_id())
            return 0
        print(f"✗ 预设 {parsed.name} 不存在", file=sys.stderr)
        return 1

    if parsed.cmd == "godot":
        return _run_godot_cli(parsed, pm)

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
            # 拆分 shell 风格的 VAR=value 前缀
            extra_env, cmd = parse_env_prefixes(cmd_str)
        except ValueError as e:
            print(f"✗ 命令解析失败: {e}", file=sys.stderr)
            return 1
        if not cmd:
            print(f"✗ 命令解析后为空: {cmd_str}", file=sys.stderr)
            return 1
        if extra_env:
            env_str = ", ".join(f"{k}={v}" for k, v in extra_env.items())
            print(f"🔧 环境变量: {env_str}", file=sys.stderr)
        # 记录 last_preset（如果是预设）
        if parsed.preset:
            pm.data["last_preset"] = parsed.preset
            pm.save()

        # 记录开始事件
        _track("compile_start",
               f"开始编译: {parsed.preset}" if parsed.preset else "开始编译",
               desc=cmd_str, node_id=_cli_node_id())

        # 同步等待 CompileRunner 结束（CompileRunner 内部已用线程读输出）
        result_holder = {}

        runner = CompileRunner(cmd, cwd=cwd or os.getcwd(), env=extra_env if extra_env else None)
        if parsed.quiet:
            runner.on_message = lambda m: None
        else:
            runner.on_message = lambda m: print(m.raw, end="")
        runner.on_finish = lambda r: result_holder.__setitem__("r", r)
        runner.start()
        # 等待
        while "r" not in result_holder:
            time.sleep(0.05)
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
        print(f"\n{status} | 耗时 {r.duration_ms}ms | 退出码 {r.exit_code} | "
              f"错误 {r.errors} | 警告 {r.warnings} | 行数 {r.total_lines}")
        if r.files_seen:
            print(f"涉及文件: {', '.join(r.files_seen)}")
        # 记录结束事件
        ev_type = "compile_finish" if r.success else "compile_fail"
        _track(ev_type,
               f"{'✓' if r.success else '✗'} 编译结束 ({parsed.preset or 'cmd'})",
               desc=f"耗时 {r.duration_ms}ms · 错误 {r.errors} · 警告 {r.warnings}",
               node_id=_cli_node_id())
        return 0 if r.success else 1

    parser.print_help()
    return 1


def _run_godot_cli(parsed, pm: PresetManager) -> int:
    """处理 compile_tool.py godot <cmd> --preset X 的 headless 调用。"""
    preset = pm.get_preset(parsed.preset)
    if not preset:
        print(f"✗ 预设 {parsed.preset} 不存在", file=sys.stderr)
        return 1
    if preset.get("type") != "godot":
        print(f"⚠️  预设 {parsed.preset} 不是 godot 类型 (type={preset.get('type')}), 操作可能失败",
              file=sys.stderr)
    p = godot_resolve_paths_from_preset(preset)
    node_id = _cli_node_id()
    cmd = parsed.godot_cmd

    if cmd == "clean":
        removed = godot_clean(p["src_dir"])
        print(f"🧹 Clean 完成, 删了 {removed} 个文件 (in {p['src_dir']})")
        _track("clean", f"Clean (.sconsign5 + .o)",
               desc=f"删 {removed} 个文件 in {p['src_dir']}", node_id=node_id)
        return 0

    if cmd == "deep_clean":
        print(f"🧹 Deep Clean 开始 in {p['src_dir']} ...")
        removed = godot_deep_clean(p["src_dir"])
        print(f"🧹 Deep Clean 完成, 删了 {removed} 个 binary. 现在跑 ▶ 全量重编")
        _track("deep_clean", f"Deep Clean 删 {removed} 个 binary",
               desc=f"in {p['src_dir']}", node_id=node_id)
        return 0

    if cmd == "touch":
        if not godot_touch(p["editor_cpp"]):
            print(f"✗ 找不到 editor_data.cpp: {p['editor_cpp']}", file=sys.stderr)
            _track("error", "Touch 失败 (文件不存在)",
                   desc=p["editor_cpp"], node_id=node_id)
            return 1
        mtime = datetime.fromtimestamp(os.path.getmtime(p["editor_cpp"]))
        ts = mtime.strftime("%H:%M:%S")
        print(f"👆 Touched {p['editor_cpp']} @ {ts}")
        _track("touch", f"Touch editor_data.cpp ({ts})",
               desc=p["editor_cpp"], node_id=node_id)
        return 0

    if cmd == "diagnose":
        log = godot_diagnose(p["src_dir"], p["binary"], p["editor_cpp"], p["fix_marker"])
        for line in log:
            print(line)
        # 提取 fix 状态作为 desc
        fix_line = next((l for l in log if "含" in l and "?" not in l), "")
        _track("diagnose", "scons 诊断",
               desc=f"src={p['src_dir']} {fix_line.strip()}", node_id=node_id)
        return 0

    if cmd == "launch":
        render = parsed.render or p["default_render"]
        mode = parsed.mode
        # editor 模式检查
        if mode == "editor":
            fix_status = godot_binary_contains_fix(p["binary"], p["fix_marker"])
            if "✗" in fix_status:
                print(f"⚠️  Binary 不含 {p['fix_marker']}, 启 editor 可能 SIGSEGV!")
                print(f"  fix 状态: {fix_status}")
                print(f"  必须先编译, 或改用 --mode game")
                _track("error", "Launch 失败 (binary 不含 fix)",
                       desc=fix_status, node_id=node_id)
                return 1
        if godot_launch(p["launch_script"], render, mode):
            print(f"🚀 Godot 已启动 ({render}, {mode}, detach)")
            _track("launch", f"启动 Godot ({render}, {mode})",
                   desc=p["launch_script"], node_id=node_id)
            return 0
        _track("error", "启动 Godot 失败",
               desc=p["launch_script"], node_id=node_id)
        return 1

    if cmd == "launch-ui":
        ui = p["ui_script"]
        if not ui or not os.path.exists(ui):
            print(f"✗ 找不到 ui.py: {ui}", file=sys.stderr)
            _track("error", "启动 ui.py 失败 (文件不存在)",
                   desc=ui, node_id=node_id)
            return 1
        try:
            subprocess.Popen(["python3", ui],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            print(f"🎛 ui.py 已启动")
            _track("launch", "启动 ui.py", desc=ui, node_id=node_id)
            return 0
        except Exception as e:
            print(f"✗ 启动失败: {e}", file=sys.stderr)
            _track("error", "启动 ui.py 失败", desc=str(e), node_id=node_id)
            return 1

    print(f"✗ 未知 godot 子命令: {cmd}", file=sys.stderr)
    return 1


# ===== 入口 =====
if __name__ == "__main__":
    if len(sys.argv) <= 1:
        run_gui()
    else:
        sys.exit(run_cli(sys.argv[1:]))
