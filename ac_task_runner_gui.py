#!/usr/bin/env python3
# ac_task_runner_gui.py — ac --task 任务的 GUI runner
# =====================================================================
# 2026-09-10 编写 — 按用户需求 "ac --task xxx 没界面, 最好都显示界面"
#
# 行为:
#   启动 PyQt6 窗口, 后台线程跑 `ac --task <name> --cli`, 实时解析 stdout
#   提取 sub-task 进度, 显示在窗口上.
#
# 布局 (1400x800, 深色):
#   ┌────────────────────────────────────────────────────────────┐
#   │ 顶部: task 名 + 状态 (Running/Done/Fail) + 进度条           │
#   ├──────────────────┬─────────────────────────────────────────┤
#   │ 左侧: sub-task    │ 右侧: 实时 log (颜色高亮 sub-task 边界) │
#   │ 列表 (✓/✗/⟳      │                                         │
#   │  [idx/N] rc 耗时) │                                         │
#   │  点击切换到那行  │                                         │
#   ├──────────────────┴─────────────────────────────────────────┤
#   │ 底部: 状态栏 + 按钮 (Stop / Copy / Open log dir / Close)   │
#   └────────────────────────────────────────────────────────────┘
#
# 用法:
#   python3 ac_task_runner_gui.py --task rebuild-v96r
#   python3 ac_task_runner_gui.py --task view-log
#   python3 ac_task_runner_gui.py --task xxx --no-stop   # 跑完不弹通知
#   python3 ac_task_runner_gui.py --task xxx --no-gui    # CLI fallback
# =====================================================================

import argparse
import os
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

# 防止无 PyQt6 环境崩
try:
    from PyQt6.QtCore import Qt, QThread, pyqtSignal, QTimer
    from PyQt6.QtGui import (
        QColor, QFont, QTextCursor, QIcon, QAction, QKeySequence,
    )
    from PyQt6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QSplitter,
        QListWidget, QListWidgetItem, QPlainTextEdit, QStatusBar, QPushButton,
        QLabel, QProgressBar, QToolBar, QFileDialog, QMessageBox,
        QGroupBox, QFrame, QSizePolicy,
    )
    HAS_PYQT6 = True
except ImportError:
    HAS_PYQT6 = False

TASK_LOG_ROOT = Path("/tmp/ac_task_logs")

# ac stdout 解析正则
RE_TASK_START = re.compile(r"🚀 task=(\S+) \| sub-task 总数: (\d+)")
RE_SUB_START = re.compile(r"▶▶▶ sub-task \[(\d+)/(\d+)\]  cmd: (.*)")
RE_SUB_DONE = re.compile(r"([✓✗]) sub-task \[(\d+)/(\d+)\] 完成 rc=(-?\d+)  \((\d+\.?\d*)s\)")
RE_SUB_FAIL = re.compile(r"✗✗✗ sub-task \[(\d+)/(\d+)\] 失败, 停止后续 sub-task")
RE_TASK_DONE = re.compile(r"([✓✗]) task=(\S+) 完成 rc=(-?\d+)  \((\d+)/(\d+) sub-task\)")


def find_ac_cmd() -> str:
    """找 ac 命令路径 (PATH 优先)"""
    from shutil import which
    a = which("ac")
    if a:
        return a
    return "/home/bv/.local/bin/ac"


def notify(title, body, urgent=False):
    """桌面通知 (notify-send)"""
    for cmd in [
        ["notify-send", "-u", "critical" if urgent else "normal", title, body],
        ["kdialog", "--passivepopup", f"{title}: {body}", "5"],
    ]:
        try:
            subprocess.run(cmd, timeout=2, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue


# -------- 后台 QThread: 跑 ac --task, 解析 stdout --------
class AcRunner(QThread):
    """在后台线程跑 `ac --task <name> --cli`, 实时 emit 输出行和 sub-task 事件"""
    line_received = pyqtSignal(str)          # 每行 stdout
    task_started = pyqtSignal(str, int)      # (task_name, total_sub)
    sub_started = pyqtSignal(int, int, str, str)  # (idx, total, cmd, log_path)
    sub_done = pyqtSignal(int, int, int, float)   # (idx, total, rc, dt_sec)
    sub_failed = pyqtSignal(int, int)        # (idx, total) 失败停止信号
    task_finished = pyqtSignal(int)          # 进程 rc
    proc_error = pyqtSignal(str)             # 启动失败

    def __init__(self, task_name: str, ac_args: list = None, cwd: str = None):
        super().__init__()
        self.task_name = task_name
        self.ac_args = ac_args or []
        self.cwd = cwd
        self.proc = None
        self._stop_flag = False
        # 内部状态
        self._sub_total = 0
        self._sub_idx = 0
        # 缓存: 上次匹配的 sub-task, 等待下一行 "   log: ..." 补充 log_path 再 emit
        self._pending_sub = None  # {'idx','total','cmd'} 等下行的 log:

    def run(self):
        ac_cmd = find_ac_cmd()
        args = [ac_cmd, "--task", self.task_name, "--cli", "--offscreen"] + self.ac_args
        # 2026-09-10 整合: 嵌套调用 ac, 避免 ac 内部又弹 GUI 窗口
        env = os.environ.copy()
        env["AC_INNER_CALL"] = "1"
        # 找 ai_build.json 看 cwd
        cwd = self.cwd
        if cwd is None:
            # 默认 ac 自己会读 cwd 的 ai_build.json, 不强求
            cwd = "/home/bv/code/godot_ui_linux"
        try:
            self.proc = subprocess.Popen(
                args,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1, cwd=cwd, env=env,
            )
        except Exception as e:
            self.proc_error.emit(f"启动 ac 失败: {e}")
            self.task_finished.emit(127)
            return

        try:
            assert self.proc.stdout is not None
            for line in self.proc.stdout:
                if self._stop_flag:
                    break
                # 处理 \r (进度条覆盖)
                if "\r" in line and not line.endswith("\n"):
                    # 进度行 (scons/j8 的 [N/M] 进度), 替换而非追加
                    self.line_received.emit("[PROGRESS] " + line.rstrip("\r\n"))
                    continue
                self.line_received.emit(line.rstrip("\n"))
                # 解析事件
                m = RE_TASK_START.search(line)
                if m:
                    self._sub_total = int(m.group(2))
                    self.task_started.emit(m.group(1), self._sub_total)
                    continue
                m = RE_SUB_START.search(line)
                if m:
                    idx = int(m.group(1))
                    total = int(m.group(2))
                    cmd = m.group(3)
                    # 2026-09-10 改: cmd 是 stdout 截断的 80 字符版本, 缓存 _pending_sub
                    # 等下面两行 (log: + cmd_full:) 补充完整信息再 emit
                    self._pending_sub = {"idx": idx, "total": total, "cmd": cmd, "log_path": "", "cmd_full": ""}
                    self._sub_idx = idx
                    continue
                # 匹配 "   log: /tmp/..." 行 → 缓存 log_path
                m_log = re.search(r"^\s*log:\s*(\S+)", line)
                if m_log and self._pending_sub:
                    self._pending_sub["log_path"] = m_log.group(1)
                    continue
                # 2026-09-10 新增: 匹配 "   📋 cmd_full: <完整>" 行 → 拿完整 cmd
                m_cmd_full = re.search(r"^\s*📋\s*cmd_full:\s*(.+)$", line)
                if m_cmd_full and self._pending_sub:
                    self._pending_sub["cmd_full"] = m_cmd_full.group(1).strip()
                    # 三行都齐了 (log + cmd_full) → emit sub_started
                    p = self._pending_sub
                    final_cmd = p["cmd_full"] if p["cmd_full"] else p["cmd"]
                    self.sub_started.emit(p["idx"], p["total"], final_cmd, p["log_path"])
                    self._pending_sub = None
                    continue
                m = RE_SUB_DONE.search(line)
                if m:
                    sym = m.group(1)
                    idx = int(m.group(2))
                    total = int(m.group(3))
                    rc = int(m.group(4))
                    dt = float(m.group(5))
                    self.sub_done.emit(idx, total, rc, dt)
                    continue
                m = RE_SUB_FAIL.search(line)
                if m:
                    idx = int(m.group(1))
                    total = int(m.group(2))
                    self.sub_failed.emit(idx, total)
                    continue
            rc = self.proc.wait(timeout=5)
        except Exception as e:
            rc = 1
            self.line_received.emit(f"[ERROR] {e}")
        self.task_finished.emit(rc)

    def stop(self):
        """用户点停止按钮: 杀 ac 进程"""
        self._stop_flag = True
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.terminate()
                time.sleep(0.5)
                if self.proc.poll() is None:
                    self.proc.kill()
            except Exception:
                pass


# -------- 主窗口 --------
class RunnerWindow(QMainWindow):
    def __init__(self, task_name: str, ac_args: list = None):
        super().__init__()
        self.task_name = task_name
        self.ac_args = ac_args or []
        self._sub_states = {}  # idx -> {rc, dt, cmd, status}
        self._total = 0
        self._task_rc = None
        self._task_start_ts = time.time()
        self._build_ui()
        self._launch_runner()

    def _build_ui(self):
        self.setWindowTitle(f"ac task runner — {self.task_name}")
        self.resize(1400, 800)
        self.setStyleSheet(self._dark_style())

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(6)

        # 顶部: 任务标题 + 状态 + 进度条
        header = QWidget()
        hl = QHBoxLayout(header)
        hl.setContentsMargins(0, 0, 0, 0)
        self.lbl_title = QLabel(f"🚀 {self.task_name}")
        self.lbl_title.setFont(QFont("monospace", 16, QFont.Weight.Bold))
        self.lbl_title.setStyleSheet("color: #88ccff;")
        hl.addWidget(self.lbl_title)
        hl.addStretch()
        self.lbl_status = QLabel("⏳ 启动中…")
        self.lbl_status.setFont(QFont("monospace", 12, QFont.Weight.Bold))
        self.lbl_status.setStyleSheet(
            "background-color: #444; color: #fff; padding: 4px 14px; "
            "border-radius: 4px; border: 1px solid #888;"
        )
        hl.addWidget(self.lbl_status)
        root.addWidget(header)

        self.progress = QProgressBar()
        self.progress.setRange(0, 1)
        self.progress.setValue(0)
        self.progress.setFormat("等待启动…")
        self.progress.setStyleSheet(
            "QProgressBar { border: 1px solid #444; border-radius: 3px; "
            "background-color: #1a1a1a; color: #fff; height: 18px; text-align: center; }"
            "QProgressBar::chunk { background-color: #4682b4; }"
        )
        root.addWidget(self.progress)

        # 2026-09-10 加: "当前运行" 区域 (大字显示正在执行的命令, 避免误以为卡住)
        current_box = QFrame()
        current_box.setFrameShape(QFrame.Shape.StyledPanel)
        current_box.setStyleSheet(
            "QFrame { background-color: #1a2a3a; border: 2px solid #4682b4; "
            "border-radius: 4px; padding: 4px; }"
        )
        cb_layout = QVBoxLayout(current_box)
        cb_layout.setContentsMargins(8, 4, 8, 4)
        cb_layout.setSpacing(2)
        # 顶行: 标题 + sub-task 编号
        cur_top = QHBoxLayout()
        cur_title = QLabel("⚡ 当前执行")
        cur_title.setFont(QFont("sans-serif", 11, QFont.Weight.Bold))
        cur_title.setStyleSheet("color: #88ccff;")
        cur_top.addWidget(cur_title)
        self.lbl_current_idx = QLabel("—")
        self.lbl_current_idx.setFont(QFont("monospace", 12, QFont.Weight.Bold))
        self.lbl_current_idx.setStyleSheet(
            "background-color: #4682b4; color: #fff; padding: 2px 8px; "
            "border-radius: 3px;"
        )
        cur_top.addWidget(self.lbl_current_idx)
        self.lbl_current_dt = QLabel("")
        self.lbl_current_dt.setFont(QFont("monospace", 10))
        self.lbl_current_dt.setStyleSheet("color: #aaa;")
        cur_top.addWidget(self.lbl_current_dt)
        cur_top.addStretch()
        cb_layout.addLayout(cur_top)
        # 当前命令 (等宽字体, 自动换行, 高度 50px, 2-3 行)
        self.txt_current_cmd = QPlainTextEdit()
        self.txt_current_cmd.setReadOnly(True)
        self.txt_current_cmd.setFont(QFont("monospace", 10))
        self.txt_current_cmd.setMaximumHeight(70)
        self.txt_current_cmd.setLineWrapMode(QPlainTextEdit.LineWrapMode.WidgetWidth)
        self.txt_current_cmd.setStyleSheet(
            "QPlainTextEdit { background-color: #0a1828; color: #ffeeaa; "
            "border: 1px solid #2a4a6a; padding: 4px; }"
        )
        self.txt_current_cmd.setPlaceholderText("(等待 sub-task 开始)")
        cb_layout.addWidget(self.txt_current_cmd)
        root.addWidget(current_box)

        # 中间 split: 左侧 sub-task 列表 + 右侧 log
        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.setSizes([400, 1000])

        # --- 左侧 sub-task 列表 ---
        left = QWidget()
        ll = QVBoxLayout(left)
        ll.setContentsMargins(0, 0, 0, 0)
        ll.addWidget(QLabel("📋 Sub-task 进度"))
        self.sub_list = QListWidget()
        self.sub_list.setFont(QFont("monospace", 10))
        self.sub_list.setStyleSheet(
            "QListWidget { background-color: #1a1a1a; }"
            "QListWidget::item { padding: 4px; border-bottom: 1px solid #2a2a2a; }"
            "QListWidget::item:selected { background-color: #4682b4; }"
        )
        self.sub_list.itemClicked.connect(lambda *_: self._on_sub_list_clicked())
        self.sub_list.currentItemChanged.connect(lambda *_: self._on_sub_list_clicked())
        ll.addWidget(self.sub_list)
        splitter.addWidget(left)

        # --- 右侧 log ---
        right = QWidget()
        rl = QVBoxLayout(right)
        rl.setContentsMargins(0, 0, 0, 0)
        log_header = QHBoxLayout()
        self.lbl_log_summary = QLabel("📄 实时 log (stdout, 0 行)")
        self.lbl_log_summary.setStyleSheet("color: #88ccff; font-weight: bold;")
        log_header.addWidget(self.lbl_log_summary)
        log_header.addStretch()
        rl.addLayout(log_header)
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setFont(QFont("monospace", 10))
        self.log_view.setStyleSheet(
            "QPlainTextEdit { background-color: #0a0a0a; color: #cccccc; border: 1px solid #333; }"
        )
        self.log_view.setMaximumBlockCount(10000)  # 限制最大行数, 防止卡 UI
        rl.addWidget(self.log_view)
        splitter.addWidget(right)
        root.addWidget(splitter, stretch=1)

        # 底部详情行 (2026-09-10 加: 显示选中 sub-task 的完整命令 + log 路径)
        detail_box = QGroupBox("📜 选中 sub-task 详情")
        detail_layout = QVBoxLayout(detail_box)
        detail_layout.setContentsMargins(6, 4, 6, 4)
        detail_layout.setSpacing(2)
        # 顶行: sub-task 编号 + rc + 耗时
        detail_top = QHBoxLayout()
        self.lbl_detail_meta = QLabel("(未选中 sub-task)")
        self.lbl_detail_meta.setFont(QFont("monospace", 10, QFont.Weight.Bold))
        self.lbl_detail_meta.setStyleSheet("color: #88ccff;")
        detail_top.addWidget(self.lbl_detail_meta)
        detail_top.addStretch()
        self.btn_open_sub_log = QPushButton("📂 打开 sub-task log")
        self.btn_open_sub_log.setEnabled(False)
        self.btn_open_sub_log.clicked.connect(self._on_open_sub_log)
        detail_top.addWidget(self.btn_open_sub_log)
        self.btn_copy_sub_cmd = QPushButton("📋 复制完整命令")
        self.btn_copy_sub_cmd.setEnabled(False)
        self.btn_copy_sub_cmd.clicked.connect(self._on_copy_sub_cmd)
        detail_top.addWidget(self.btn_copy_sub_cmd)
        detail_layout.addLayout(detail_top)
        # 命令区: 完整命令, 等宽字体 (2026-09-10 改: 不限高度, 长命令完整显示 + 滚动)
        self.txt_detail_cmd = QPlainTextEdit()
        self.txt_detail_cmd.setReadOnly(True)
        self.txt_detail_cmd.setFont(QFont("monospace", 9))
        self.txt_detail_cmd.setMinimumHeight(80)        # 至少 4 行
        self.txt_detail_cmd.setMaximumHeight(280)       # 最多 ~15 行, 不让窗口被撑爆
        self.txt_detail_cmd.setLineWrapMode(QPlainTextEdit.LineWrapMode.WidgetWidth)  # 长行自动换行
        self.txt_detail_cmd.setPlaceholderText(
            "(鼠标点左侧 sub-task 列表, 这里显示完整命令)\n"
            "  ↕ 滚动查看完整命令 / Ctrl+A 全选 / Ctrl+C 复制"
        )
        self.txt_detail_cmd.setStyleSheet(
            "QPlainTextEdit { background-color: #0a0a0a; color: #aaffaa; "
            "border: 1px solid #333; padding: 4px; }"
        )
        # 垂直滚动条永远显示 (用户知道可以滚动看完整命令)
        self.txt_detail_cmd.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOn
        )
        detail_layout.addWidget(self.txt_detail_cmd)
        root.addWidget(detail_box)

        # 底部按钮
        btn_row = QHBoxLayout()
        self.btn_stop = QPushButton("⏹ Stop (杀 ac)")
        self.btn_stop.setStyleSheet(
            "QPushButton { background-color: #aa4444; color: #fff; padding: 6px 14px; "
            "font-weight: bold; border-radius: 3px; }"
            "QPushButton:disabled { background-color: #444; color: #888; }"
        )
        self.btn_stop.clicked.connect(self._on_stop_clicked)
        btn_row.addWidget(self.btn_stop)
        self.btn_copy = QPushButton("📋 复制 log")
        self.btn_copy.clicked.connect(self._on_copy)
        btn_row.addWidget(self.btn_copy)
        self.btn_open_dir = QPushButton("📂 打开 log 目录")
        self.btn_open_dir.clicked.connect(self._on_open_dir)
        btn_row.addWidget(self.btn_open_dir)
        btn_row.addStretch()
        self.lbl_elapsed = QLabel("⏱ 00:00")
        self.lbl_elapsed.setFont(QFont("monospace", 11))
        self.lbl_elapsed.setStyleSheet("color: #aaa;")
        btn_row.addWidget(self.lbl_elapsed)
        root.addLayout(btn_row)

        # 菜单栏 / 工具栏
        self._build_menu()

        # 状态栏
        self.status = QStatusBar()
        self.setStatusBar(self.status)
        self.status.showMessage(f"启动 ac --task {self.task_name}…", 3000)

        # 启动 elapsed timer
        self._elapsed_timer = QTimer(self)
        self._elapsed_timer.timeout.connect(self._update_elapsed)
        self._elapsed_timer.start(1000)

    def _build_menu(self):
        mb = self.menuBar()
        m_run = mb.addMenu("运行")
        a_stop = QAction("⏹ Stop", self)
        a_stop.setShortcut(QKeySequence("Ctrl+X"))
        a_stop.triggered.connect(self._on_stop_clicked)
        m_run.addAction(a_stop)
        m_run.addSeparator()
        a_quit = QAction("关闭", self)
        a_quit.setShortcut(QKeySequence("Ctrl+Q"))
        a_quit.triggered.connect(self.close)
        m_run.addAction(a_quit)
        m_log = mb.addMenu("Log")
        a_copy = QAction("📋 复制 log", self)
        a_copy.setShortcut(QKeySequence("Ctrl+L"))
        a_copy.triggered.connect(self._on_copy)
        m_log.addAction(a_copy)
        a_open = QAction("📂 打开 log 目录", self)
        a_open.setShortcut(QKeySequence("Ctrl+O"))
        a_open.triggered.connect(self._on_open_dir)
        m_log.addAction(a_open)

    def _dark_style(self) -> str:
        return """
        QMainWindow, QWidget { background-color: #2b2b2b; color: #ddd; }
        QMenuBar { background-color: #1a1a1a; }
        QMenuBar::item:selected { background-color: #4682b4; }
        QMenu { background-color: #2b2b2b; border: 1px solid #444; }
        QMenu::item:selected { background-color: #4682b4; }
        QStatusBar { background-color: #1a1a1a; color: #888; }
        QLabel { color: #ddd; }
        QGroupBox { border: 1px solid #444; margin-top: 8px; padding-top: 6px; }
        QPushButton { background-color: #3a3a3a; color: #ddd; padding: 4px 10px;
                      border: 1px solid #555; border-radius: 3px; }
        QPushButton:hover { background-color: #4a4a4a; }
        """

    def _launch_runner(self):
        self.runner = AcRunner(self.task_name, self.ac_args)
        self.runner.line_received.connect(self._on_line)
        self.runner.task_started.connect(self._on_task_started)
        self.runner.sub_started.connect(self._on_sub_started)
        self.runner.sub_done.connect(self._on_sub_done)
        self.runner.sub_failed.connect(self._on_sub_failed)
        self.runner.task_finished.connect(self._on_task_finished)
        self.runner.proc_error.connect(self._on_proc_error)
        self.runner.start()

    def _on_line(self, line: str):
        # 进度行 (scons 的 \r 覆盖) 单独处理
        if line.startswith("[PROGRESS] "):
            # 提取 scons 进度 [\d+/\d+] 显示在状态栏
            import re as _re
            m = _re.search(r"\[(\d+)/(\d+)\]", line)
            if m:
                self.status.showMessage(f"⏳ scons {line[11:].strip()[:80]}", 1500)
            return
        # 普通行, 写到 log
        # 颜色标记
        if "sub-task" in line and ("▶▶▶" in line or "✓" in line[:3] or "✗" in line[:3]):
            color = "#88ccff" if "▶▶▶" in line else ("#88ff88" if "✓" in line[:3] else "#ff8888")
            self.log_view.appendHtml(
                f'<span style="color:{color};">{self._esc(line)}</span>'
            )
        elif "🚀" in line or "===" in line:
            self.log_view.appendHtml(
                f'<span style="color:#ffaa44;">{self._esc(line)}</span>'
            )
        else:
            self.log_view.appendPlainText(line)
        # 自动滚到底
        sb = self.log_view.verticalScrollBar()
        sb.setValue(sb.maximum())
        # 更新 log 摘要
        n = self.log_view.blockCount()
        self.lbl_log_summary.setText(f"📄 实时 log (stdout, {n} 行)")

    def _esc(self, s: str) -> str:
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

    def _on_task_started(self, task_name: str, total: int):
        self._total = total
        self._task_start_ts = time.time()
        self.progress.setRange(0, max(total, 1))
        self.progress.setValue(0)
        self.progress.setFormat(f"0 / {total}")
        self.lbl_status.setText("⏳ Running")
        self.lbl_status.setStyleSheet(
            "background-color: #4488aa; color: #fff; padding: 4px 14px; "
            "border-radius: 4px; border: 1px solid #888; font-weight: bold;"
        )
        # 预填 sub-task 占位
        self.sub_list.clear()
        for i in range(1, total + 1):
            self._sub_states[i] = {"rc": None, "dt": 0, "cmd": "", "status": "pending"}
            item = QListWidgetItem(f"  ⏳ [{i:>2}/{total}] 等待…")
            item.setForeground(QColor("#888888"))
            self.sub_list.addItem(item)
        self.status.showMessage(f"🚀 task={task_name} | {total} sub-task", 3000)

    def _on_sub_started(self, idx: int, total: int, cmd: str, log_path: str):
        self._sub_states[idx] = {
            "rc": None, "dt": 0, "cmd": cmd, "log_path": log_path, "status": "running"
        }
        if idx - 1 < self.sub_list.count():
            cmd_short = cmd[:55] + ("…" if len(cmd) > 55 else "")
            item = self.sub_list.item(idx - 1)
            item.setText(f"  ⟳ [{idx:>2}/{total}] {cmd_short}")
            item.setForeground(QColor("#88ccff"))
            # tooltip 显示完整命令
            log_line = f"\n📄 log: {log_path}" if log_path else ""
            item.setToolTip(
                f"sub-task [{idx}/{total}]  跑中…\n"
                f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
                f"$ {cmd}{log_line}"
            )
            # 自动滚到当前
            self.sub_list.setCurrentRow(idx - 1)
            # 立即更新详情区
            self._show_sub_detail(idx)
            # 2026-09-10 加: 顶部"当前执行"区域大字显示, 避免误以为卡住
            self.lbl_current_idx.setText(f"[{idx}/{total}]")
            self.lbl_current_idx.setStyleSheet(
                "background-color: #ff8800; color: #fff; padding: 2px 8px; "
                "border-radius: 3px;"
            )
            self.txt_current_cmd.setPlainText(cmd)
            self._current_cmd_start_ts = time.time()
            self.lbl_current_dt.setText("⏱ 0s")
            # 启动当前命令计时器 (每 0.5s 刷新一次)
            if not hasattr(self, "_current_dt_timer") or not self._current_dt_timer.isActive():
                from PyQt6.QtCore import QTimer as _QT
                if not hasattr(self, "_current_dt_timer"):
                    self._current_dt_timer = _QT(self)
                    self._current_dt_timer.timeout.connect(self._update_current_dt)
                self._current_dt_timer.start(500)

    def _on_sub_done(self, idx: int, total: int, rc: int, dt: float):
        if idx not in self._sub_states:
            return
        self._sub_states[idx]["rc"] = rc
        self._sub_states[idx]["dt"] = dt
        self._sub_states[idx]["status"] = "ok" if rc == 0 else "fail"
        if idx - 1 < self.sub_list.count():
            sym = "✓" if rc == 0 else "✗"
            color = "#88ff88" if rc == 0 else "#ff8888"
            cmd = self._sub_states[idx]["cmd"]
            cmd_short = cmd[:50] + ("…" if len(cmd) > 50 else "")
            item = self.sub_list.item(idx - 1)
            item.setText(
                f"  {sym} [{idx:>2}/{total}] rc={rc} {dt:.1f}s  {cmd_short}"
            )
            item.setForeground(QColor(color))
            # 2026-09-10 加: hover tooltip 显示完整命令 + log 路径
            log_path = self._sub_states[idx].get("log_path", "")
            log_line = f"\n📄 log: {log_path}" if log_path else ""
            item.setToolTip(
                f"sub-task [{idx}/{total}]  rc={rc}  {dt:.2f}s\n"
                f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
                f"$ {cmd}{log_line}"
            )
        # 进度
        done = sum(1 for s in self._sub_states.values() if s["status"] in ("ok", "fail"))
        self.progress.setValue(done)
        self.progress.setFormat(f"{done} / {total}")
        # 2026-09-10 加: 当前命令完成 → "⚡ 当前执行" 区显示完成状态
        # 如果还有下一个 sub-task, 它自己的 sub_started 会覆盖; 这里先标绿
        self.lbl_current_idx.setStyleSheet(
            "background-color: #44aa44; color: #fff; padding: 2px 8px; "
            "border-radius: 3px;"
        )
        self.lbl_current_dt.setText(f"⏱ {dt:.1f}s ✓")
        # 停当前命令计时器 (下一个 sub_started 时会重启)
        if hasattr(self, "_current_dt_timer") and self._current_dt_timer.isActive():
            # 不一定停, 因为下一个 sub-task 可能马上启动; 让下一个 sub_started 决定
            # 但如果已经到最后一个, 关闭
            if done >= total:
                self._current_dt_timer.stop()
                self.lbl_current_idx.setText("[DONE]")
                self.lbl_current_idx.setStyleSheet(
                    "background-color: #228822; color: #fff; padding: 2px 8px; "
                    "border-radius: 3px;"
                )

    def _update_current_dt(self):
        """每 0.5s 刷新一次当前命令的耗时, 避免误以为卡住"""
        if not hasattr(self, "_current_cmd_start_ts"):
            return
        dt = time.time() - self._current_cmd_start_ts
        if dt < 60:
            self.lbl_current_dt.setText(f"⏱ {dt:.1f}s")
        else:
            m, s = divmod(int(dt), 60)
            self.lbl_current_dt.setText(f"⏱ {m}m{s:02d}s")

    def _on_sub_failed(self, idx: int, total: int):
        # 在状态栏显示失败
        self.status.showMessage(f"✗ sub-task [{idx}/{total}] 失败, ac 已停", 5000)

    def _on_task_finished(self, rc: int):
        self._task_rc = rc
        self._elapsed_timer.stop()
        elapsed = time.time() - self._task_start_ts
        m, s = divmod(int(elapsed), 60)
        self.lbl_elapsed.setText(f"⏱ {m:02d}:{s:02d}")
        if rc == 0:
            self.lbl_status.setText("✓ Done")
            self.lbl_status.setStyleSheet(
                "background-color: #44aa44; color: #fff; padding: 4px 14px; "
                "border-radius: 4px; border: 1px solid #888; font-weight: bold;"
            )
            self.progress.setValue(self._total)
            self.progress.setFormat(f"✓ {self._total} / {self._total}")
            self.status.showMessage(
                f"✓ {self.task_name} 完成 (rc=0)  log: {TASK_LOG_ROOT / self.task_name}/latest.log",
                10000,
            )
            notify(f"✓ ac: {self.task_name}", f"成功, 耗时 {m:02d}:{s:02d}")
        else:
            self.lbl_status.setText(f"✗ Fail (rc={rc})")
            self.lbl_status.setStyleSheet(
                "background-color: #aa4444; color: #fff; padding: 4px 14px; "
                "border-radius: 4px; border: 1px solid #888; font-weight: bold;"
            )
            self.status.showMessage(
                f"✗ {self.task_name} 失败 (rc={rc})  log: {TASK_LOG_ROOT / self.task_name}/latest.log",
                10000,
            )
            notify(
                f"✗ ac: {self.task_name} [FAIL]",
                f"rc={rc}  耗时 {m:02d}:{s:02d}  log: {TASK_LOG_ROOT / self.task_name}/latest.log",
                urgent=True,
            )
        self.btn_stop.setEnabled(False)
        self.btn_stop.setText("✓ 已完成")

    def _on_proc_error(self, msg: str):
        self.lbl_status.setText("✗ 启动失败")
        self.lbl_status.setStyleSheet(
            "background-color: #aa4444; color: #fff; padding: 4px 14px; "
            "border-radius: 4px; border: 1px solid #888; font-weight: bold;"
        )
        self.log_view.appendPlainText(f"[ERROR] {msg}")
        self.btn_stop.setEnabled(False)

    def _on_stop_clicked(self):
        if self.runner.isRunning():
            self.status.showMessage("⏹ 正在杀 ac 进程…", 3000)
            self.runner.stop()

    def _on_copy(self):
        QApplication.clipboard().setText(self.log_view.toPlainText())
        self.status.showMessage("✓ log 已复制到剪贴板", 3000)

    def _on_open_dir(self):
        d = TASK_LOG_ROOT / self.task_name
        if not d.is_dir():
            d.mkdir(parents=True, exist_ok=True)
        # 优先用 xdg-open, 失败再 nautilus/dolphin
        for cmd in [
            ["xdg-open", str(d)],
            ["nautilus", str(d)],
            ["dolphin", str(d)],
            ["thunar", str(d)],
        ]:
            try:
                subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.status.showMessage(f"📂 打开 {d}", 3000)
                return
            except FileNotFoundError:
                continue
        self.status.showMessage(f"⚠ 找不到文件管理器, log 目录: {d}", 5000)

    def _show_sub_detail(self, idx: int):
        """更新底部详情区, 显示选中 sub-task 的完整命令 + log 路径"""
        if idx not in self._sub_states:
            return
        s = self._sub_states[idx]
        rc = s.get("rc")
        dt = s.get("dt", 0)
        cmd = s.get("cmd", "")
        log_path = s.get("log_path", "")
        status = s.get("status", "pending")
        # meta 行
        sym = {"ok": "✓", "fail": "✗", "running": "⟳", "pending": "⏳"}.get(status, "·")
        if rc is None and status != "running":
            meta = f"{sym} sub-task [{idx}/{self._total}]  状态: {status}  耗时: --"
        else:
            rc_str = str(rc) if rc is not None else "..."
            meta = (f"{sym} sub-task [{idx}/{self._total}]  "
                    f"rc={rc_str}  耗时: {dt:.2f}s  状态: {status}")
        if log_path:
            meta += f"  |  log: {log_path}"
        self.lbl_detail_meta.setText(meta)
        # 命令区 (完整, 多行) — 自动选中文本, 让用户立即知道有完整命令可复制
        self.txt_detail_cmd.setPlainText(cmd if cmd else "(空命令)")
        if cmd:
            # 全选 (让用户立刻知道可以 Ctrl+C 复制完整内容)
            cursor = self.txt_detail_cmd.textCursor()
            cursor.select(QTextCursor.SelectionType.Document)
            self.txt_detail_cmd.setTextCursor(cursor)
            # 取消焦点高亮, 但保持选择状态
            self.txt_detail_cmd.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        # 启用按钮
        self.btn_copy_sub_cmd.setEnabled(bool(cmd))
        self.btn_open_sub_log.setEnabled(bool(log_path) and Path(log_path).exists())

    def _on_sub_list_clicked(self):
        """sub-task 列表点击 → 更新详情区"""
        item = self.sub_list.currentItem()
        if item is None:
            return
        idx = self.sub_list.currentRow() + 1  # 0-based → 1-based
        self._show_sub_detail(idx)

    def _on_open_sub_log(self):
        """打开当前选中 sub-task 的 log 文件"""
        item = self.sub_list.currentItem()
        if item is None:
            return
        idx = self.sub_list.currentRow() + 1
        if idx not in self._sub_states:
            return
        log_path = self._sub_states[idx].get("log_path", "")
        if not log_path or not Path(log_path).exists():
            self.status.showMessage(f"⚠ log 不存在: {log_path}", 3000)
            return
        for cmd in [
            ["xdg-open", log_path],
            ["gedit", log_path],
            ["kate", log_path],
            ["code", log_path],
        ]:
            try:
                subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.status.showMessage(f"📂 打开 sub-task log: {log_path}", 3000)
                return
            except FileNotFoundError:
                continue
        self.status.showMessage(f"⚠ 找不到文本编辑器, log: {log_path}", 5000)

    def _on_copy_sub_cmd(self):
        """复制当前选中 sub-task 的完整命令"""
        item = self.sub_list.currentItem()
        if item is None:
            return
        idx = self.sub_list.currentRow() + 1
        if idx not in self._sub_states:
            return
        cmd = self._sub_states[idx].get("cmd", "")
        if cmd:
            QApplication.clipboard().setText(cmd)
            self.status.showMessage(f"✓ 完整命令已复制 ({len(cmd)} 字符)", 3000)

    def _update_elapsed(self):
        elapsed = time.time() - self._task_start_ts
        m, s = divmod(int(elapsed), 60)
        self.lbl_elapsed.setText(f"⏱ {m:02d}:{s:02d}")

    def closeEvent(self, ev):
        if self.runner.isRunning():
            r = QMessageBox.question(
                self, "ac 还在跑",
                f"ac --task {self.task_name} 还在跑, 确定要杀 + 关闭吗?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            )
            if r != QMessageBox.StandardButton.Yes:
                ev.ignore()
                return
            self.runner.stop()
            self.runner.wait(3000)
        ev.accept()


# -------- CLI fallback (无 PyQt6) --------
def run_cli(task_name: str, ac_args: list = None):
    """无 PyQt6 时, 直接调 `ac --task xxx --cli --offscreen`"""
    ac_cmd = find_ac_cmd()
    args = [ac_cmd, "--task", task_name, "--cli", "--offscreen"] + (ac_args or [])
    print(f"[CLI fallback] 跑: {' '.join(args)}")
    print(f"(无 PyQt6, 装上: pip install PyQt6 后用 GUI 跑)")
    rc = subprocess.call(args)
    sys.exit(rc)


def main():
    p = argparse.ArgumentParser(
        description="ac --task 任务的 GUI runner (PyQt6)",
        epilog="例: python3 ac_task_runner_gui.py --task rebuild-v96r",
    )
    p.add_argument("--task", required=True, help="ai_build.json 里的 task name")
    p.add_argument("--ac-arg", action="append", default=[],
                   help="透传给 ac 的额外参数 (可多次)")
    p.add_argument("--no-gui", action="store_true",
                   help="CLI fallback (无 GUI, 直接调 ac --task --cli)")
    args = p.parse_args()

    if not HAS_PYQT6 or args.no_gui:
        run_cli(args.task, args.ac_arg)
        return

    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    win = RunnerWindow(args.task, args.ac_arg)
    win.show()
    win._keep_alive = (app, win)  # 防 GC
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
