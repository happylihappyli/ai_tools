#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ac_gui — AI 编译 Qt GUI 启动器 (集成 ac 工具链)
================================================================
启动后弹窗口,带菜单/工具栏/进度条/日志面板/状态栏。
默认启动后立即自动跑 auto 链 (0 点击, 按用户偏好)。

启动:
    ac_gui                            # 弹窗 + 自动跑 auto 链
    ac_gui --no-auto                  # 弹窗, 不自动跑
    ac_gui --task build-only          # 弹窗 + 跑指定 task
    ac_gui --project /path/to/proj    # 切到别的项目

集成:
    - 工具 → GitHub Token 管理  → 调 ac ght (独立子窗口)
    - 工具 → TTS 播报          → 调 ac tts
    - 工具 → 备份当前项目      → 调 ac bak
"""
import os
import sys
import json
import subprocess
import shutil
import time
from datetime import datetime
from pathlib import Path

# 解析项目目录
PROJECT_DIR = "/home/bv/code/godot_ui_linux/godot-ui-standalone-skia"
AC_DIR = "/home/bv/code/ai_tools"
AC_BIN = f"{AC_DIR}/ac"

# 解析 argv (简单, 不引 argparse 避免冲突)
AUTO_START = True
TASK_NAME = None
QT_PLATFORM = os.environ.get("QT_QPA_PLATFORM", "")  # 空=用系统默认 (Wayland/X11)
DOCTOR_MODE = False
SHOW_DOCTOR = False

# 解析 argv (简单, 不引 argparse 避免冲突)
for a in sys.argv[1:]:
    if a == "--no-auto":
        AUTO_START = False
    elif a == "--auto":
        AUTO_START = True
    elif a.startswith("--task="):
        TASK_NAME = a.split("=", 1)[1]
    elif a == "--task" and len(sys.argv) > sys.argv.index(a) + 1:
        TASK_NAME = sys.argv[sys.argv.index(a) + 1]
    elif a.startswith("--project="):
        PROJECT_DIR = a.split("=", 1)[1]
    elif a == "--project" and len(sys.argv) > sys.argv.index(a) + 1:
        PROJECT_DIR = sys.argv[sys.argv.index(a) + 1]
    elif a in ("--doctor",):
        DOCTOR_MODE = True
        AUTO_START = False
    elif a in ("-h", "--help"):
        print(__doc__)
        sys.exit(0)

# 加 ai_tools 到 sys.path (复用 _qt_compat)
sys.path.insert(0, AC_DIR)

# Qt 平台默认值: 优先用命令行 --platform, 再用 env, 最后按显示环境自动选
# - 本地有 DISPLAY/WAYLAND: 留空让 Qt 自动选 (xcb/wayland)
# - 沙箱/SSH/无显示: 用 offscreen (不然会崩)
_HAS_DISPLAY = bool(os.environ.get("DISPLAY")) or bool(os.environ.get("WAYLAND_DISPLAY"))
if not QT_PLATFORM and "QT_QPA_PLATFORM" not in os.environ:
    if not _HAS_DISPLAY:
        os.environ["QT_QPA_PLATFORM"] = "offscreen"
        QT_PLATFORM = "offscreen"

from _qt_compat import (
    QT_BACKEND, APP_EXEC, gui_available,
    Qt, QTimer, QProcess, QProcessEnvironment, QObject,
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QLineEdit, QPushButton, QTreeWidget, QTreeWidgetItem,
    QStatusBar, QMessageBox, QColor, QFont, QFrame,
    QSizePolicy, QGroupBox, QProgressBar, QPlainTextEdit, QTextCursor,
    QAction, QMenu, QToolBar, QDockWidget, QKeySequence, QDialog,
    QFileDialog, QInputDialog,
)

import github_token_gui as gt  # 复用 SetupHelpDialog / AboutDialog


# 颜色
COLOR_OK = "#2e7d32"
COLOR_ERR = "#c62828"
COLOR_WARN = "#ef6c00"
COLOR_INFO = "#1565c0"
COLOR_BG = "#fafafa"
COLOR_LABEL = "#424242"

APP_NAME = "ac GUI — AI 编译启动器"
APP_VERSION = "1.0.0"
APP_ORG = "ai_tools"


def load_ai_build():
    """加载 ai_build.json"""
    p = Path(PROJECT_DIR) / "ai_build.json"
    if p.exists():
        try:
            with open(p, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception as e:
            print(f"⚠ 读取 ai_build.json 失败: {e}", file=sys.stderr)
    return {}


def run_ac_cli(*args, cwd=None, capture=True):
    """调 ac CLI 子命令, 返回 (rc, stdout, stderr)"""
    if cwd is None:
        cwd = PROJECT_DIR
    try:
        result = subprocess.run(
            [AC_BIN] + list(args),
            cwd=cwd,
            capture_output=capture,
            text=True,
            timeout=300,
        )
        return result.returncode, (result.stdout or ""), (result.stderr or "")
    except subprocess.TimeoutExpired:
        return -1, "", "TIMEOUT (300s)"
    except Exception as e:
        return 1, "", f"EXCEPTION: {e}"


def tts_async(msg: str):
    """异步 tts 播报"""
    try:
        subprocess.Popen(
            [AC_BIN, "tts", msg],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
    except Exception:
        pass


class TaskRunner(QObject):
    """跑 ai_build.json 里的 task, 用 QProcess 实时输出"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.parent = parent
        self.proc = None
        self._task_name = ""
        self._cmd = ""
        self._start_time = 0

    def is_running(self):
        return self.proc is not None and self.proc.state() != QProcess.NotRunning

    def run(self, task_name: str, cmd: str):
        """异步跑 task"""
        if self.is_running():
            return False
        self._task_name = task_name
        self._cmd = cmd
        self._start_time = time.time()
        self.proc = QProcess(self.parent)
        self.proc.setProcessChannelMode(QProcess.MergedChannels)
        self.proc.setWorkingDirectory(PROJECT_DIR)
        # 透传环境
        env = QProcessEnvironment.systemEnvironment()
        # 沙箱环境保持 offscreen, 本地用默认
        if QT_PLATFORM:
            env.insert("QT_QPA_PLATFORM", QT_PLATFORM)
        self.proc.setProcessEnvironment(env)
        # 信号
        self.proc.readyReadStandardOutput.connect(self._on_output)
        self.proc.finished.connect(self._on_finished)
        self.proc.errorOccurred.connect(self._on_error)
        # 解析 cmd (用 shell 跑, 兼容 && 链)
        self.proc.start("/bin/sh", ["-c", cmd])
        return True

    def _on_output(self):
        if not self.proc:
            return
        data = bytes(self.proc.readAllStandardOutput()).decode("utf-8", errors="replace")
        for line in data.splitlines():
            self.parent.on_proc_output(self._task_name, line)

    def _on_finished(self, exit_code, exit_status):
        elapsed = time.time() - self._start_time
        self.parent.on_proc_finished(self._task_name, exit_code, elapsed)

    def _on_error(self, err):
        self.parent.on_proc_error(self._task_name, err)

    def stop(self):
        if self.is_running():
            self.proc.kill()
            self.proc.waitForFinished(2000)


class LogDockWidget(QWidget):
    """日志面板: 颜色编码时间戳行, 自动滚动"""

    def __init__(self, parent=None):
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(2)

        bar = QHBoxLayout()
        self._auto_scroll_chk = QPushButton("自动滚动: 开")
        self._auto_scroll_chk.setCheckable(True)
        self._auto_scroll_chk.setChecked(True)
        self._auto_scroll_chk.toggled.connect(self._on_auto_scroll_toggle)
        self._clear_btn = QPushButton("清空日志")
        self._clear_btn.clicked.connect(lambda: self._edit.clear())
        bar.addWidget(self._auto_scroll_chk)
        bar.addStretch(1)
        bar.addWidget(self._clear_btn)
        layout.addLayout(bar)

        self._edit = QPlainTextEdit()
        self._edit.setReadOnly(True)
        self._edit.setMaximumBlockCount(5000)
        font = QFont("monospace")
        font.setPointSize(10)
        self._edit.setFont(font)
        self._edit.setStyleSheet(
            "QPlainTextEdit { background: #1e1e1e; color: #e0e0e0; border: 1px solid #444; }"
        )
        layout.addWidget(self._edit, 1)

    def _on_auto_scroll_toggle(self, checked: bool):
        self._auto_scroll_chk.setText(f"自动滚动: {'开' if checked else '关'}")

    def append(self, level: str, msg: str, color_hint: str = ""):
        prefix = {"ok": "✓", "err": "✗", "warn": "⚠", "info": "•", "debug": "›", "task": "▶"}.get(level, "•")
        ts = datetime.now().strftime("%H:%M:%S")
        self._edit.appendPlainText(f"[{ts}] {prefix} {msg}")
        if self._auto_scroll_chk.isChecked():
            sb = self._edit.verticalScrollBar()
            sb.setValue(sb.maximum())


class AcMainWindow(QMainWindow):
    """ac 工具链主窗口: 菜单/工具栏/进度条/日志 Dock/状态栏"""

    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"{APP_NAME} — {Path(PROJECT_DIR).name}")
        self.resize(900, 640)

        self._config = load_ai_build()
        self._tasks = {t["name"]: t for t in self._config.get("tasks", [])}
        self._runner = TaskRunner(self)

        # ===== 中心 widget =====
        central = QWidget()
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(10)

        # 顶部: 项目信息
        info_frame = QFrame()
        info_frame.setFrameShape(QFrame.StyledPanel)
        info_frame.setStyleSheet(
            f"QFrame {{ background: {COLOR_BG}; border: 1px solid #e0e0e0; "
            f"border-radius: 6px; padding: 8px; }}"
        )
        info_layout = QVBoxLayout(info_frame)
        self._proj_label = QLabel(f"📁 {PROJECT_DIR}")
        big = QFont()
        big.setPointSize(13)
        big.setBold(True)
        self._proj_label.setFont(big)
        info_layout.addWidget(self._proj_label)

        self._auto_label = QLabel(
            f"auto 链: {', '.join(self._config.get('auto', ['build+deploy']))}"
        )
        self._auto_label.setStyleSheet(f"color: {COLOR_LABEL}; font-size: 11px;")
        info_layout.addWidget(self._auto_label)
        layout.addWidget(info_frame)

        # 任务列表
        task_group = QGroupBox(f"任务列表 ({len(self._tasks)} 个)")
        task_layout = QVBoxLayout(task_group)
        self._task_list = QTreeWidget()
        self._task_list.setHeaderLabels(["任务名", "说明", "命令"])
        self._task_list.setRootIsDecorated(False)
        self._task_list.setAlternatingRowColors(True)
        self._task_list.setColumnWidth(0, 140)
        self._task_list.setColumnWidth(1, 240)
        self._task_list.setColumnWidth(2, 320)
        for name, t in self._tasks.items():
            item = QTreeWidgetItem()
            item.setText(0, name)
            item.setText(1, t.get("description", ""))
            item.setText(2, t.get("cmd", ""))
            item.setData(0, Qt.UserRole, name)
            self._task_list.addTopLevelItem(item)
        self._task_list.itemDoubleClicked.connect(self.on_run_task_by_item)
        task_layout.addWidget(self._task_list)
        layout.addWidget(task_group, 1)

        # 进度条 + 当前任务
        prog_row = QHBoxLayout()
        self._prog_label = QLabel("当前: —")
        self._prog_label.setStyleSheet(f"color: {COLOR_LABEL};")
        self._prog_bar = QProgressBar()
        self._prog_bar.setRange(0, 0)  # 不确定模式 (跑的时候转)
        self._prog_bar.setVisible(False)
        prog_row.addWidget(self._prog_label, 1)
        prog_row.addWidget(self._prog_bar, 2)
        layout.addLayout(prog_row)

        # 控制按钮行
        ctrl_row = QHBoxLayout()
        self._btn_run = QPushButton("▶ 跑选中任务")
        self._btn_stop = QPushButton("■ 停止")
        self._btn_auto = QPushButton("⚡ 跑 Auto 链")
        self._btn_stop.setEnabled(False)
        ctrl_row.addWidget(self._btn_run)
        ctrl_row.addWidget(self._btn_stop)
        ctrl_row.addWidget(self._btn_auto)
        ctrl_row.addStretch(1)
        layout.addLayout(ctrl_row)

        # 状态栏
        self._statusbar = QStatusBar()
        self.setStatusBar(self._statusbar)
        self._sb_label = QLabel("就绪")
        self._sb_label.setStyleSheet(f"color: {COLOR_LABEL}; padding: 2px 8px;")
        self._statusbar.addWidget(self._sb_label, 1)
        self._sb_right = QLabel(f"Qt={QT_BACKEND}")
        self._sb_right.setStyleSheet(f"color: {COLOR_LABEL}; padding: 2px 8px;")
        self._statusbar.addPermanentWidget(self._sb_right)

        # ===== Action / Menu / Toolbar / Dock =====
        self._build_actions()
        self._build_menu()
        self._build_toolbar()
        self._build_log_dock()

        # 信号
        self._btn_run.clicked.connect(self.on_run_selected)
        self._btn_auto.clicked.connect(self.on_run_auto)
        self._btn_stop.clicked.connect(self.on_stop)

        self._log("info", f"{APP_NAME} v{APP_VERSION} 启动")
        self._log("info", f"项目: {PROJECT_DIR}")
        self._log("info", f"任务数: {len(self._tasks)}")
        self._log("info", f"Qt 后端: {QT_BACKEND}")

        # 自动启动
        if AUTO_START:
            QTimer.singleShot(300, self.on_run_auto)

    # ===== Action / Menu / Toolbar / Dock =====

    def _build_actions(self):
        self.act_run_selected = QAction("跑选中任务", self)
        self.act_run_selected.setShortcut(QKeySequence("F5"))
        self.act_run_selected.triggered.connect(self.on_run_selected)

        self.act_run_auto = QAction("跑 Auto 链", self)
        self.act_run_auto.setShortcut(QKeySequence("F6"))
        self.act_run_auto.triggered.connect(self.on_run_auto)

        self.act_stop = QAction("停止", self)
        self.act_stop.setShortcut(QKeySequence("F7"))
        self.act_stop.triggered.connect(self.on_stop)

        self.act_open_ght = QAction("GitHub Token 管理...", self)
        self.act_open_ght.setShortcut(QKeySequence("Ctrl+G"))
        self.act_open_ght.triggered.connect(self.on_open_ght)

        self.act_tts = QAction("TTS 播报...", self)
        self.act_tts.setShortcut(QKeySequence("Ctrl+T"))
        self.act_tts.triggered.connect(self.on_tts_prompt)

        self.act_bak = QAction("备份当前项目...", self)
        self.act_bak.setShortcut(QKeySequence("Ctrl+B"))
        self.act_bak.triggered.connect(self.on_bak_project)

        self.act_toggle_log = QAction("显示日志面板", self)
        self.act_toggle_log.setCheckable(True)
        self.act_toggle_log.setChecked(False)
        self.act_toggle_log.setShortcut(QKeySequence("Ctrl+L"))
        self.act_toggle_log.toggled.connect(self.on_toggle_log)

        self.act_help_setup = QAction("设置指南 (GitHub Token)...", self)
        self.act_help_setup.setShortcut(QKeySequence("F1"))
        self.act_help_setup.triggered.connect(self.on_help_setup)

        self.act_help_about = QAction("关于...", self)
        self.act_help_about.setShortcut(QKeySequence("Ctrl+,"))
        self.act_help_about.triggered.connect(self.on_about)

        self.act_quit = QAction("退出", self)
        self.act_quit.setShortcut(QKeySequence("Ctrl+Q"))
        self.act_quit.triggered.connect(self.close)

    def _build_menu(self):
        mb = self.menuBar()
        m_file = mb.addMenu("文件(&F)")
        m_file.addAction(self.act_bak)
        m_file.addSeparator()
        m_file.addAction(self.act_quit)

        m_run = mb.addMenu("运行(&R)")
        m_run.addAction(self.act_run_selected)
        m_run.addAction(self.act_run_auto)
        m_run.addAction(self.act_stop)

        m_tools = mb.addMenu("工具(&T)")
        m_tools.addAction(self.act_open_ght)
        m_tools.addAction(self.act_tts)
        m_tools.addSeparator()
        m_tools.addAction(self.act_bak)

        m_view = mb.addMenu("视图(&V)")
        m_view.addAction(self.act_toggle_log)

        m_help = mb.addMenu("帮助(&H)")
        m_help.addAction(self.act_help_setup)
        m_help.addSeparator()
        m_help.addAction(self.act_help_about)

    def _build_toolbar(self):
        tb = QToolBar("主工具栏", self)
        tb.setObjectName("AcMainToolBar")
        tb.setMovable(True)
        tb.setFloatable(True)
        tb.addAction(self.act_run_selected)
        tb.addAction(self.act_run_auto)
        tb.addAction(self.act_stop)
        tb.addSeparator()
        tb.addAction(self.act_open_ght)
        tb.addAction(self.act_tts)
        tb.addAction(self.act_bak)
        tb.addSeparator()
        tb.addAction(self.act_toggle_log)
        tb.addAction(self.act_help_setup)
        self.addToolBar(tb)

    def _build_log_dock(self):
        self._log_widget = LogDockWidget()
        self._log_dock = QDockWidget("操作日志", self)
        self._log_dock.setObjectName("AcLogDock")
        self._log_dock.setWidget(self._log_widget)
        self._log_dock.setAllowedAreas(
            Qt.LeftDockWidgetArea | Qt.RightDockWidgetArea
            | Qt.BottomDockWidgetArea | Qt.TopDockWidgetArea
        )
        self._log_dock.setFeatures(
            QDockWidget.DockWidgetMovable | QDockWidget.DockWidgetFloatable
            | QDockWidget.DockWidgetClosable
        )
        self.addDockWidget(Qt.BottomDockWidgetArea, self._log_dock)
        self._log_dock.hide()
        self._log_dock.visibilityChanged.connect(
            lambda v: self.act_toggle_log.setChecked(v)
        )

    # ===== Log 助手 =====

    def _log(self, level: str, msg: str):
        if hasattr(self, "_log_widget"):
            self._log_widget.append(level, msg)
        try:
            ts = datetime.now().strftime("%H:%M:%S")
            print(f"[{ts}] [{level.upper()}] {msg}", file=sys.stderr)
        except Exception:
            pass

    # ===== Task 运行 =====

    def on_run_selected(self):
        item = self._task_list.currentItem()
        if not item:
            self._log("warn", "先在任务列表里选一个任务 (双击或选中后按 F5)")
            return
        name = item.data(0, Qt.UserRole)
        self._run_task(name)

    def on_run_task_by_item(self, item, _col):
        name = item.data(0, Qt.UserRole)
        self._run_task(name)

    def on_run_auto(self):
        auto_list = self._config.get("auto", ["build+deploy"])
        self._log("task", f"跑 auto 链: {auto_list}")
        self._auto_queue = list(auto_list)
        self._auto_index = 0
        self._run_next_in_auto()

    def _run_next_in_auto(self):
        if self._auto_index >= len(self._auto_queue):
            self._log("ok", "auto 链全部完成 ✓")
            tts_async("ac 工具链, 全部完成")
            return
        name = self._auto_queue[self._auto_index]
        self._auto_index += 1
        self._log("info", f"[auto {self._auto_index}/{len(self._auto_queue)}] {name}")
        if name not in self._tasks:
            self._log("err", f"未知 task: {name}, 跳过")
            self._run_next_in_auto()
            return
        self._run_task(name, on_done=self._run_next_in_auto)

    def on_stop(self):
        if self._runner.is_running():
            self._log("warn", "停止当前 task")
            self._runner.stop()
        else:
            self._log("info", "没有 task 在跑")

    def _run_task(self, name: str, on_done=None):
        if name not in self._tasks:
            self._log("err", f"未知 task: {name}")
            return
        if self._runner.is_running():
            self._log("warn", "已有 task 在跑, 请先停止")
            return
        cmd = self._tasks[name]["cmd"]
        self._log("task", f"▶ 跑 task [{name}]: {cmd}")
        self._prog_label.setText(f"当前: {name}")
        self._prog_bar.setVisible(True)
        self._btn_run.setEnabled(False)
        self._btn_auto.setEnabled(False)
        self._btn_stop.setEnabled(True)
        self._current_on_done = on_done
        self._runner.run(name, cmd)

    def on_proc_output(self, task_name, line):
        if not line.strip():
            return
        # scons 输出 (含 error/warning/编译文件) 染色
        level = "info"
        ll = line.lower()
        if "error" in ll or "fatal" in ll or "failed" in ll:
            level = "err"
        elif "warning" in ll:
            level = "warn"
        elif "✓" in line or "[ok]" in ll:
            level = "ok"
        self._log(level, f"[{task_name}] {line}")

    def on_proc_finished(self, task_name, exit_code, elapsed):
        self._log("ok" if exit_code == 0 else "err",
                  f"[{task_name}] 退出码 {exit_code}  耗时 {elapsed:.1f}s")
        tts_async(f"task {task_name} {'成功' if exit_code == 0 else '失败'}")
        self._prog_label.setText(f"当前: — (上次: {task_name}, rc={exit_code}, {elapsed:.1f}s)")
        self._prog_bar.setVisible(False)
        self._btn_run.setEnabled(True)
        self._btn_auto.setEnabled(True)
        self._btn_stop.setEnabled(False)
        on_done = getattr(self, "_current_on_done", None)
        if on_done:
            self._current_on_done = None
            on_done()

    def on_proc_error(self, task_name, err):
        self._log("err", f"[{task_name}] QProcess 错误: {err}")

    # ===== 工具菜单槽函数 =====

    def on_open_ght(self):
        """调起 ac ght (独立子进程, 不阻塞本窗口)"""
        self._log("info", f"调起: ac ght (Qt={QT_PLATFORM or 'system'})")
        try:
            subprocess.Popen(
                [AC_BIN, "ght", "--no-check"],
                start_new_session=True,
                env={**os.environ, "QT_QPA_PLATFORM": QT_PLATFORM or os.environ.get("QT_QPA_PLATFORM", "xcb")},
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception as e:
            self._log("err", f"启动 ac ght 失败: {e}")

    def on_tts_prompt(self):
        """弹输入框让用户输入要播报的文字"""
        text, ok = QInputDialog.getText(self, "TTS 播报", "输入要播报的文字:")
        if ok and text.strip():
            tts_async(text.strip())
            self._log("ok", f"TTS 播报: {text.strip()[:40]}")

    def on_bak_project(self):
        """备份当前项目"""
        self._log("info", f"备份项目: {PROJECT_DIR}")
        rc, out, err = run_ac_cli("bak", PROJECT_DIR)
        if rc == 0:
            self._log("ok", f"备份成功: {out.strip()}")
            tts_async("备份完成")
        else:
            self._log("err", f"备份失败: {err.strip()}")
            tts_async("备份失败")

    def on_toggle_log(self, checked: bool):
        if checked:
            self._log_dock.show()
            self._log_dock.raise_()
        else:
            self._log_dock.hide()

    def on_help_setup(self):
        dlg = gt.SetupHelpDialog(self)
        dlg.exec()

    def on_about(self):
        QMessageBox.about(
            self, f"关于 — {APP_NAME}",
            f"<h3>{APP_NAME}</h3>"
            f"<p>版本: {APP_VERSION}</p>"
            f"<p>项目: {PROJECT_DIR}</p>"
            f"<p>任务数: {len(self._tasks)}</p>"
            f"<p>Qt 后端: {QT_BACKEND}</p>"
            f"<hr>"
            f"<p>快捷键: F5 跑选中, F6 跑 auto, F7 停止, "
            f"Ctrl+G GitHub Token, Ctrl+T TTS, Ctrl+B 备份, "
            f"Ctrl+L 日志, F1 帮助, Ctrl+, 关于, Ctrl+Q 退出</p>"
        )


def main() -> int:
    # ===== 自检模式: 检查 GUI 环境 =====
    if DOCTOR_MODE:
        return run_doctor()

    app = QApplication.instance() or QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    app.setApplicationVersion(APP_VERSION)
    app.setOrganizationName(APP_ORG)
    w = AcMainWindow()
    w.show()
    return getattr(app, APP_EXEC)()


def run_doctor() -> int:
    """检查本地 GUI 环境: DISPLAY/WAYLAND_DISPLAY/Qt 后端/平台插件"""
    print("=" * 60)
    print(f"  {APP_NAME} — 环境自检")
    print("=" * 60)

    # 1) 显示环境
    has_display = "DISPLAY" in os.environ and os.environ["DISPLAY"]
    has_wayland = "WAYLAND_DISPLAY" in os.environ and os.environ["WAYLAND_DISPLAY"]
    has_xdg = "XDG_SESSION_TYPE" in os.environ
    print(f"\n[1] 显示环境变量")
    print(f"  DISPLAY          = {os.environ.get('DISPLAY', '(未设)')}")
    print(f"  WAYLAND_DISPLAY  = {os.environ.get('WAYLAND_DISPLAY', '(未设)')}")
    print(f"  XDG_SESSION_TYPE = {os.environ.get('XDG_SESSION_TYPE', '(未设)')}")
    if not has_display and not has_wayland:
        print(f"  ⚠ 既没 DISPLAY 也没 WAYLAND_DISPLAY, Qt 弹不出窗口!")
        print(f"    → 必须 ssh -X 或在本地桌面环境跑")
    else:
        print(f"  ✓ 显示环境已设")

    # 2) Qt 后端
    import _qt_compat
    print(f"\n[2] Qt 后端")
    print(f"  QT_BACKEND (代码检测) = {QT_BACKEND}")
    print(f"  PYQT_VERSION_STR      = {_qt_compat.PYQT_VERSION_STR}")
    print(f"  GUI available         = {gui_available()}")
    if not gui_available():
        print(f"  ✗ 没有可用的 Qt 后端! 安装: pip install PySide6")
        return 1

    # 3) Qt 平台插件
    print(f"\n[3] Qt 平台插件 (QPA)")
    print(f"  QT_QPA_PLATFORM (env)  = {os.environ.get('QT_QPA_PLATFORM', '(未设=用默认)')}")
    print(f"  默认会按 XDG_SESSION_TYPE 自动选 (wayland/xcb/...)")

    # 4) 测试创建一个隐藏窗口看是否成功
    print(f"\n[4] 测试创建 QApplication...")
    try:
        test_app = QApplication.instance() or QApplication(sys.argv[:1] + ["ac-gui-test"])
        print(f"  ✓ QApplication 创建成功, platform = {test_app.platformName()}")
        # 尝试建一个 test 窗口
        test_w = QWidget()
        test_w.resize(100, 100)
        test_w.show()
        print(f"  ✓ test 窗口创建成功, visible={test_w.isVisible()}")
        test_w.close()
    except Exception as e:
        print(f"  ✗ 创建 QApplication 失败: {e}")
        return 1

    # 5) 项目配置
    print(f"\n[5] 项目配置")
    print(f"  PROJECT_DIR = {PROJECT_DIR}")
    p = Path(PROJECT_DIR)
    if p.exists():
        print(f"  ✓ 项目目录存在")
        ab = p / "ai_build.json"
        print(f"  ai_build.json = {'存在 ✓' if ab.exists() else '不存在 ✗'}")
    else:
        print(f"  ✗ 项目目录不存在!")

    print(f"\n[6] 建议")
    if not has_display and not has_wayland:
        print(f"  1. 终端连 SSH? 用 'ssh -X user@host' 转发 X11")
        print(f"  2. 或者 'ssh -Y' (trusted forwarding)")
        print(f"  3. 本地用户? 直接在桌面终端跑, 不要在远程 SSH")
        print(f"  4. 想看日志不弹窗? 用 'ac-gui --no-auto' + 强制 offscreen:")
        print(f"     QT_QPA_PLATFORM=offscreen ac-gui --no-auto")
    else:
        print(f"  显示环境正常, 直接跑 'ac-gui' 应该能弹窗口")
        print(f"  如果还是没弹, 试 'ac-gui --no-auto' 手动点 [⚡ 跑 Auto 链]")

    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
