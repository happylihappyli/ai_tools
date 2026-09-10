#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ar.py — AI Run (运行工具)
================================================================
通用运行 binary 的 GUI 工具, 100% 由 ai_build.json 的 run_panel 段配置驱动.

设计目标 (2026-09-09):
  - 跟 ab (AI Build) 配对, ab 编译完一键切 ar 启动 + 监控
  - 配置驱动: binary_path / args / env / duration / markers 全在 ai_build.json
  - 0-click 启动 (auto_run=true), 跟 ab 同样的体验
  - 实时 log + ANSI 颜色 + 复制 error/log 按钮
  - 监控 duration 秒, 自动 grep success/fail markers, TTS 报结果

用法:
  python3 ar.py                                       # 弹 GUI, 读 ./ai_build.json 的 run_panel
  python3 ar.py --config /path/to/ai_build.json       # 显式指定配置
  python3 ar.py --no-gui --duration 30                # 纯后台, ssh/沙箱
  python3 ar.py --auto                                # GUI + 0-click 启动

配置文件 ai_build.json 模板 (跟 ui.run_after_build 同源):
  {
    "run_panel": {
      "title": "cloud_main — ar",
      "binary_path": "/abs/path/to/cloud_main",
      "args": ["--rendering-driver", "vulkan"],
      "env": {"VK_ICD_FILENAMES": "...", "SDL_VIDEODRIVER": "wayland"},
      "cwd": "/abs/path/to/dir",
      "auto_run": true,
      "duration": 30,
      "log_path": "/tmp/cloud_main.log",
      "success_markers": ["Vulkan 1.", "MultiMesh Updated"],
      "fail_markers": ["SIGSEGV", "FATAL"],
      "button_label": "🚀 启动 cloud_main",
      "theme": "dark"
    }
  }
"""
import argparse
import json
import os
import shlex
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# ANSI 颜色 (跟 build_poc_a_stage3_gui.py 保持一致, 沙箱/终端可见)
# ---------------------------------------------------------------------------
_ANSI = {
    "reset":  "\033[0m",
    "red":    "\033[91m",   # error / FATAL
    "yellow": "\033[93m",   # warning
    "green":  "\033[92m",   # ok / success
    "blue":   "\033[94m",   # info
    "cyan":   "\033[96m",   # meta
    "gray":   "\033[90m",   # debug / 进度
    "bold":   "\033[1m",
}
COLOR_HEX = {
    "red":    "#ff8888",
    "yellow": "#ffff88",
    "green":  "#aaffaa",
    "blue":   "#88ccff",
    "cyan":   "#aaffff",
    "gray":   "#888888",
}

# 监控期内的 log 缓冲 (供 marker grep 用)
_LOG_BUFFER = []
_LOG_BUFFER_MAX = 50000  # 5 万行够 30s 跑满

# TTS (无 pyttsx3 不报错)
def tts_say(text: str) -> None:
    try:
        import pyttsx3  # type: ignore
        eng = pyttsx3.init()
        eng.say(text)
        eng.runAndWait()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# 配置加载
# ---------------------------------------------------------------------------
def load_run_panel(config_path: Path) -> dict:
    """从 ai_build.json 读 run_panel 段, 缺失字段走默认"""
    if not config_path.is_file():
        return {}
    try:
        data = json.loads(config_path.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"[ar] WARN: parse {config_path} 失败: {e}", file=sys.stderr)
        return {}
    panel = data.get("run_panel", {})
    # 兼容 ui.run_after_build (老格式)
    if not panel and "ui" in data and "run_after_build" in data["ui"]:
        panel = data["ui"]["run_after_build"]
    return panel


def resolve_panel(panel: dict, fallback_cwd: Path) -> dict:
    """字段默认值 + 路径展开 + 兼容老 ui.run_after_build 字段"""
    if not panel:
        return {}
    binary = panel.get("binary_path") or panel.get("binary", "")
    if binary and not os.path.isabs(binary):
        # 相对路径: 相对 fallback_cwd
        binary = str((fallback_cwd / binary).resolve())
    return {
        "title":         panel.get("title", "AI Run"),
        "binary_path":   binary,
        "args":          panel.get("args", []),
        "env":           panel.get("env", {}),
        "cwd":           panel.get("cwd", str(fallback_cwd)),
        "auto_run":      bool(panel.get("auto_run", False)),
        "duration":      int(panel.get("duration", 0)),  # 0 = 无限
        "log_path":      panel.get("log_path", str(Path("/tmp") / f"ar_{os.getpid()}.log")),
        "success_markers": panel.get("success_markers", []),
        "fail_markers":    panel.get("fail_markers", []),
        "button_label":  panel.get("button_label", "🚀 启动"),
        "theme":         panel.get("theme", "dark"),
        "show_command":  bool(panel.get("show_command", True)),
    }


# ---------------------------------------------------------------------------
# no-gui 模式 (无 PyQt6, 沙箱/SSH 用)
# ---------------------------------------------------------------------------
def run_no_gui(panel: dict) -> int:
    """headless 模式: 启动 binary, 监控 duration 秒, grep markers, 退出码"""
    binary = panel["binary_path"]
    if not binary or not os.path.isfile(binary):
        print(f"FATAL: binary_path 不存在: {binary}", file=sys.stderr)
        return 1

    env = os.environ.copy()
    env.update(panel["env"])
    cmd = [binary] + list(panel["args"])
    log_path = Path(panel["log_path"])
    log_path.parent.mkdir(parents=True, exist_ok=True)
    duration = panel["duration"]

    print(f">>> {shlex.join(cmd)}", file=sys.stderr)
    print(f"    cwd={panel['cwd']}", file=sys.stderr)
    print(f"    log → {log_path}", file=sys.stderr)
    print(f"    duration={duration}s  (0=无限)", file=sys.stderr)

    proc = subprocess.Popen(
        cmd,
        stdout=open(log_path, "w", encoding="utf-8"),
        stderr=subprocess.STDOUT,
        env=env,
        cwd=panel["cwd"] or None,
    )
    start = time.time()
    try:
        proc.wait(timeout=duration if duration > 0 else None)
    except subprocess.TimeoutExpired:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        print(f"[ar] duration={duration}s 到, 已停止", file=sys.stderr)
    rc = proc.returncode
    elapsed = time.time() - start
    print(f"<<< exit={rc} elapsed={elapsed:.1f}s", file=sys.stderr)

    # marker check
    text = ""
    if log_path.is_file():
        text = log_path.read_text(encoding="utf-8", errors="replace")
    ok = True
    for m in panel["fail_markers"]:
        if m in text:
            print(f"FAIL marker 出现: {m!r}", file=sys.stderr)
            ok = False
    for m in panel["success_markers"]:
        if m not in text:
            print(f"SUCCESS marker 缺失: {m!r}", file=sys.stderr)
            ok = False
    if panel["success_markers"] and ok:
        print(f"✓ all {len(panel['success_markers'])} success_markers 出现, 无 fail_markers", file=sys.stderr)
        tts_say("运行验证通过")
    elif not panel["success_markers"]:
        # 没配 markers, 只看 rc
        ok = (rc == 0)
        if ok:
            tts_say("运行成功")
    else:
        tts_say("运行验证失败")
    return 0 if ok else 2


# ---------------------------------------------------------------------------
# GUI 模式 (PyQt6)
# ---------------------------------------------------------------------------
def run_gui(panel: dict, config_path: Path) -> int:
    try:
        from PyQt6.QtCore import Qt, QProcess, QTimer  # type: ignore
        from PyQt6.QtGui import QColor, QFont, QTextCharFormat, QTextCursor  # type: ignore
        from PyQt6.QtWidgets import (  # type: ignore
            QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
            QProgressBar, QPlainTextEdit, QStatusBar, QPushButton, QLabel,
        )
    except ImportError:
        print("FATAL: PyQt6 未装, 请 pip install pyqt6  或用 --no-gui", file=sys.stderr)
        return run_no_gui(panel)

    is_dark = panel["theme"] == "dark"
    bg = "#0a0a0a" if is_dark else "#fafafa"
    fg = "#aaffaa" if is_dark else "#1a1a1a"
    status_bg = "#2a2a2a" if is_dark else "#e0e0e0"
    status_fg = "#ffff88" if is_dark else "#664400"

    app = QApplication(sys.argv)
    app.setStyle("Fusion")

    window = QMainWindow()
    window.setWindowTitle(panel["title"])
    window.resize(1100, 720)

    central = QWidget()
    layout = QVBoxLayout(central)

    title = QLabel(panel["title"])
    title.setStyleSheet("font-size: 18px; font-weight: bold; color: #ffff88; padding: 6px;")
    layout.addWidget(title)

    cmd_preview = QLabel("命令预览: 启动后显示")
    cmd_preview.setStyleSheet("color: #88ccff; font-family: monospace; padding: 4px;")
    cmd_preview.setWordWrap(True)
    layout.addWidget(cmd_preview)

    status_label = QLabel("● 等待启动")
    status_label.setStyleSheet("color: #888888; padding: 4px;")
    layout.addWidget(status_label)

    progress = QProgressBar()
    progress.setRange(0, 100)
    progress.setValue(0)
    progress.setStyleSheet(f"QProgressBar {{ background: {status_bg}; }} QProgressBar::chunk {{ background: #4488aa; }}")
    layout.addWidget(progress)

    log_view = QPlainTextEdit()
    log_view.setReadOnly(True)
    log_view.setMaximumBlockCount(5000)
    mono = QFont("monospace")
    mono.setStyleHint(QFont.StyleHint.Monospace)
    log_view.setFont(mono)
    log_view.setStyleSheet(f"background-color: {bg}; color: {fg};")
    layout.addWidget(log_view)

    btn_layout = QHBoxLayout()
    btn_start = QPushButton(panel["button_label"])
    btn_stop = QPushButton("■ 停止")
    btn_stop.setEnabled(False)
    btn_copy_err = QPushButton("复制 error (red+yellow)")
    btn_copy_log = QPushButton("复制全部 log")
    btn_copy_cmd = QPushButton("复制命令")
    # 2026-09-10 整合: 加 "📋 Tasks" 按钮调 ac task-browser
    btn_tasks = QPushButton("📋 Tasks (ac task-browser)")
    btn_quit = QPushButton("退出")
    for b in (btn_start, btn_stop, btn_copy_err, btn_copy_log, btn_copy_cmd, btn_tasks, btn_quit):
        btn_layout.addWidget(b)
    layout.addLayout(btn_layout)

    window.setCentralWidget(central)
    status = QStatusBar()
    status.setStyleSheet(f"background-color: {status_bg}; color: {status_fg};")
    window.setStatusBar(status)
    status.showMessage("0-click 启动中..." if panel["auto_run"] else "等待用户点击启动")

    # 颜色表
    color_map = {name: QColor(*rgb) for name, rgb in {
        "red": (255, 136, 136), "yellow": (255, 255, 136), "green": (170, 255, 170),
        "blue": (136, 204, 255), "cyan": (170, 255, 255), "gray": (136, 136, 136),
    }.items()}

    proc = QProcess(window)
    proc.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
    log_path = Path(panel["log_path"])
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_fp = open(log_path, "w", encoding="utf-8", buffering=1)

    _LOCAL_BUFFER = []  # GUI 内 marker 检查缓冲
    duration = panel["duration"]
    start_ts = [0.0]
    monitor_timer = QTimer()
    monitor_timer.setInterval(200)

    def colorize_and_append(line: str) -> None:
        # 写 log 文件 (无颜色, grep 友好)
        try:
            log_fp.write(line + "\n")
        except Exception:
            pass
        _LOCAL_BUFFER.append(line)
        if len(_LOCAL_BUFFER) > _LOG_BUFFER_MAX:
            del _LOCAL_BUFFER[:len(_LOCAL_BUFFER) - _LOG_BUFFER_MAX]
        # GUI 渲染 (按关键字颜色)
        low = line.lower()
        color = "gray"
        if any(k in low for k in ("error:", "fatal", "aborting", "fail", "undefined", "traceback")):
            color = "red"
        elif any(k in low for k in ("warning", "warn")):
            color = "yellow"
        elif any(k in low for k in ("ok", "success", "✓", "ready", "loaded")):
            color = "green"
        elif line.startswith(">>>") or line.startswith("==="):
            color = "blue"
        cf = QTextCharFormat()
        cf.setForeground(color_map[color])
        log_view.moveCursor(QTextCursor.MoveOperation.End)
        log_view.insertPlainText(line + "\n")
        # 重新应用颜色 (insertPlainText 后改 cursor 格式)
        cursor = log_view.textCursor()
        cursor.movePosition(QTextCursor.MoveOperation.End)
        cursor.insertText("", cf)

    def build_cmd_display() -> str:
        e = " ".join(f"{k}={shlex.quote(v)}" for k, v in panel["env"].items())
        a = " ".join(shlex.quote(x) for x in panel["args"])
        return f"{e} {shlex.quote(panel['binary_path'])} {a}".strip()

    def on_start():
        if not panel["binary_path"] or not os.path.isfile(panel["binary_path"]):
            colorize_and_append(f"FATAL: binary 不存在: {panel['binary_path']}")
            return
        env_qproc = QProcess.ProcessEnvironment.systemEnvironment()
        for k, v in panel["env"].items():
            env_qproc.insert(k, v)
        proc.setProcessEnvironment(env_qproc)
        if panel["cwd"]:
            proc.setWorkingDirectory(panel["cwd"])
        cmd_str = build_cmd_display()
        colorize_and_append(f">>> {cmd_str}")
        cmd_preview.setText(f"命令: {cmd_str[:200]}{'…' if len(cmd_str) > 200 else ''}")
        proc.start(panel["binary_path"], panel["args"])
        btn_start.setEnabled(False)
        btn_stop.setEnabled(True)
        status_label.setText("● 运行中…")
        status.showMessage("运行中")
        start_ts[0] = time.time()
        monitor_timer.start()
        tts_say("启动")

    def on_stop():
        if proc.state() != QProcess.ProcessState.NotRunning:
            proc.terminate()
            colorize_and_append("[ar] SIGTERM 发送, 等待 3s 后 SIGKILL")
            QTimer.singleShot(3000, lambda: (
                proc.kill(),
                colorize_and_append("[ar] SIGKILL 已发"),
            ) if proc.state() != QProcess.ProcessState.NotRunning else None)
        monitor_timer.stop()

    def on_stdout():
        data = bytes(proc.readAllStandardOutput()).decode("utf-8", errors="replace")
        for line in data.splitlines():
            colorize_and_append(line)
        _LOG_BUFFER.append(data)

    def on_finished(exit_code, exit_status):
        monitor_timer.stop()
        btn_start.setEnabled(True)
        btn_stop.setEnabled(False)
        elapsed = time.time() - start_ts[0]
        msg = f"<<< exit={exit_code} elapsed={elapsed:.1f}s"
        colorize_and_append(msg)
        # marker 检查
        text = "\n".join(_LOCAL_BUFFER)
        ok = True
        for m in panel["fail_markers"]:
            if m in text:
                colorize_and_append(f"FAIL marker: {m!r}")
                ok = False
        for m in panel["success_markers"]:
            if m not in text:
                colorize_and_append(f"SUCCESS marker 缺失: {m!r}")
                ok = False
        if ok and panel["success_markers"]:
            colorize_and_append(f"✓ all {len(panel['success_markers'])} success_markers 出现")
            tts_say("运行验证通过")
            status.showMessage("✓ 验证通过")
            status_label.setText("● ✓ 验证通过")
            status_label.setStyleSheet("color: #aaffaa; padding: 4px;")
        elif ok and not panel["success_markers"]:
            tts_say("运行结束")
            status.showMessage(f"运行结束 exit={exit_code}")
            status_label.setText(f"● 运行结束 exit={exit_code}")
        else:
            tts_say("运行验证失败")
            status.showMessage("✗ 验证失败")
            status_label.setText("● ✗ 验证失败")
            status_label.setStyleSheet("color: #ff8888; padding: 4px;")

    def on_monitor_tick():
        if duration > 0:
            elapsed = time.time() - start_ts[0]
            pct = min(100, int(elapsed / duration * 100))
            progress.setValue(pct)
            status.showMessage(f"运行中 {elapsed:.0f}/{duration}s ({pct}%)")
            if elapsed >= duration and proc.state() != QProcess.ProcessState.NotRunning:
                colorize_and_append(f"[ar] duration={duration}s 到, 停止")
                on_stop()

    def on_copy_error():
        text = log_view.toPlainText()
        kw_err = ("error:", "fatal", "error 1", "undefined reference", "collect2: error",
                  "aborting at", "ld returned", "no such file", "permission denied",
                  "traceback", "hit restricted", "fail marker")
        kw_warn = ("warning:", "warn", "deprecated")
        out = []
        for line in text.splitlines():
            low = line.lower()
            if any(k in low for k in kw_err):
                out.append("[E] " + line)
            elif any(k in low for k in kw_warn):
                out.append("[W] " + line)
        if not out:
            status.showMessage("⚠ 无 error/warning 行可复制", 3000)
            return
        app.clipboard().setText("\n".join(out))
        n_e = sum(1 for l in out if l.startswith("[E]"))
        n_w = sum(1 for l in out if l.startswith("[W]"))
        status.showMessage(f"✓ 已复制 {len(out)} 行 (error={n_e}, warning={n_w})", 4000)
        tts_say(f"已复制 {n_e} 个 error, {n_w} 个 warning")

    def on_copy_log():
        text = log_view.toPlainText()
        if not text:
            status.showMessage("⚠ log 区为空", 3000)
            return
        app.clipboard().setText(text)
        status.showMessage(f"✓ 已复制 {len(text.splitlines())} 行 log", 4000)

    def on_copy_cmd():
        cmd = build_cmd_display()
        cwd_line = f"cd {panel['cwd']}\n" if panel["cwd"] else ""
        env_lines = "\n".join(f"export {k}={shlex.quote(v)}" for k, v in panel["env"].items())
        text = f"{cwd_line}{env_lines}\n{cmd}"
        app.clipboard().setText(text)
        status.showMessage("✓ 命令已复制", 3000)

    def on_open_tasks():
        # 2026-09-10 整合: 调 ac task-browser 弹任务浏览器
        # 用 subprocess.Popen 非阻塞调起, 不影响 ar GUI
        try:
            import subprocess
            import shutil as _shutil
            ac = _shutil.which("ac") or os.path.expanduser("~/.local/bin/ac")
            subprocess.Popen(
                [ac, "task-browser"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True,  # 不阻塞 ar, 关闭窗口不影响
            )
            status.showMessage("📋 已弹 task browser 窗口 (ac task-browser)", 3000)
        except Exception as e:
            status.showMessage(f"⚠ 启动 ac task-browser 失败: {e}", 5000)

    def on_quit():
        if proc.state() != QProcess.ProcessState.NotRunning:
            proc.kill()
            proc.waitForFinished(2000)
        log_fp.close()
        monitor_timer.stop()
        app.quit()

    btn_start.clicked.connect(on_start)
    btn_stop.clicked.connect(on_stop)
    btn_copy_err.clicked.connect(on_copy_error)
    btn_copy_log.clicked.connect(on_copy_log)
    btn_copy_cmd.clicked.connect(on_copy_cmd)
    btn_tasks.clicked.connect(on_open_tasks)
    btn_quit.clicked.connect(on_quit)
    proc.readyReadStandardOutput.connect(on_stdout)
    proc.finished.connect(on_finished)
    monitor_timer.timeout.connect(on_monitor_tick)

    window.show()

    if panel["auto_run"]:
        # 0-click: 500ms 后自动启动
        QTimer.singleShot(500, on_start)

    return app.exec()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser(description="ar — AI Run (配置驱动的运行工具)")
    parser.add_argument("--config", type=Path, default=Path("ai_build.json"),
                        help="ai_build.json 路径 (默认 ./ai_build.json)")
    parser.add_argument("--no-gui", action="store_true", help="headless 模式 (沙箱/SSH)")
    parser.add_argument("--auto", action="store_true", help="GUI + 0-click 自动启动")
    parser.add_argument("--duration", type=int, default=None,
                        help="覆盖配置里的 duration (秒), 0=无限")
    parser.add_argument("--binary", type=str, default=None, help="覆盖 binary_path")
    parser.add_argument("--args", type=str, default=None, help="覆盖 args (空格分隔)")
    args = parser.parse_args()

    config_path = args.config.resolve()
    if not config_path.is_file():
        print(f"FATAL: config 不存在: {config_path}", file=sys.stderr)
        return 1

    panel = load_run_panel(config_path)
    panel = resolve_panel(panel, fallback_cwd=config_path.parent)

    # CLI 覆盖
    if args.duration is not None:
        panel["duration"] = args.duration
    if args.auto:
        panel["auto_run"] = True
    if args.binary:
        panel["binary_path"] = args.binary
    if args.args:
        panel["args"] = shlex.split(args.args)

    if not panel.get("binary_path"):
        print("FATAL: run_panel.binary_path 缺失", file=sys.stderr)
        return 1

    if args.no_gui:
        return run_no_gui(panel)
    return run_gui(panel, config_path)


if __name__ == "__main__":
    sys.exit(main())
