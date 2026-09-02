#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
compile_ai_tools_gui.py
================================================================
一键编译 ai_tools/ 下所有 C++ 子项目 (scons) + 实时日志 GUI

目的 (2026-09-02):
  解决 `scons` 在 ai_tools 根目录报 "No SConstruct file found" 的问题
  写 GUI 启动器, 按你规则:
    - 执行时间长 (scons -j) → 不直接跑
    - 加 --auto 自动跑, 不让你点
  自动跑顶层 SConstruct → 子项目 (ab + cpp_panel + ...) 全编

子项目 (2026-09-02):
  ab/          C++ Qt5/6 GUI 编译面板
  cpp_panel/   C++ Qt5 旧版编译面板

用法:
  python3 compile_ai_tools_gui.py              # 弹 GUI, 默认勾选所有 C++ 项目
  python3 compile_ai_tools_gui.py --auto       # GUI + 自动跑
  python3 compile_ai_tools_gui.py --no-gui     # 纯后台 (SSH/沙箱)
  python3 compile_ai_tools_gui.py --project ab # 只编 ab
  python3 compile_ai_tools_gui.py --clean      # 干净重建
"""
import os
import sys
import time
import shutil
import threading
import subprocess
import argparse
import tkinter as tk
from tkinter import scrolledtext
from pathlib import Path

AI_TOOLS_DIR = Path("/home/bv/code/ai_tools")
LOG_DIR = AI_TOOLS_DIR / ".ai_tools"
LOG_FILE = LOG_DIR / "compile_ai_tools.log"

# 顶层 SConstruct 管理的子项目 (必须跟 SConstruct 里 SUBDIRS 一致)
SUBDIRS = ['ab', 'cpp_panel']


def _now():
    return time.strftime("%H:%M:%S")


def log(msg, widget=None):
    line = f"[{_now()}] {msg}"
    print(line, flush=True)
    if widget is not None:
        widget.insert("end", line + "\n")
        widget.see("end")
        widget.update_idletasks()
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    try:
        with LOG_FILE.open("a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def detect_subprojects() -> list:
    """扫 ai_tools/ 下的子项目 (有 SConstruct + 至少 1 个 .cpp)"""
    found = []
    for sub in sorted(AI_TOOLS_DIR.iterdir()):
        if not sub.is_dir():
            continue
        if sub.name.startswith(('.', 'bak')):
            continue
        scons_p = sub / "SConstruct"
        if not scons_p.exists():
            continue
        cpp_count = len(list((sub / "src").glob("*.cpp"))) if (sub / "src").exists() else 0
        if cpp_count == 0:
            continue
        found.append((sub.name, scons_p, cpp_count))
    return found


def step_check_top(widget):
    log("=" * 60, widget)
    log("STEP 0: 验顶层 SConstruct + 子项目", widget)
    log("=" * 60, widget)
    top_scons = AI_TOOLS_DIR / "SConstruct"
    if not top_scons.exists():
        log(f"  ❌ 缺 {top_scons}", widget)
        return False
    log(f"  ✓ {top_scons} (size={top_scons.stat().st_size})", widget)
    subs = detect_subprojects()
    if not subs:
        log(f"  ❌ 没找到任何 C++ 子项目 (SConstruct + *.cpp)", widget)
        return False
    for name, scons_p, cpp_count in subs:
        log(f"  ✓ {name}/: {scons_p.name} + {cpp_count} .cpp", widget)
    return True


def step_check_tools(widget):
    log("=" * 60, widget)
    log("STEP 1: 验工具链 (scons + Qt5/Qt6 + moc + g++)", widget)
    log("=" * 60, widget)
    # scons
    res = subprocess.run(["scons", "--version"], capture_output=True, text=True, timeout=10)
    if res.returncode == 0:
        ver = res.stdout.splitlines()[0] if res.stdout else "?"
        log(f"  ✓ scons: {ver}", widget)
    else:
        log(f"  ❌ scons 不可用", widget)
        return False
    # g++
    res = subprocess.run(["g++", "--version"], capture_output=True, text=True, timeout=10)
    if res.returncode == 0:
        ver = res.stdout.splitlines()[0] if res.stdout else "?"
        log(f"  ✓ g++: {ver}", widget)
    else:
        log(f"  ❌ g++ 不可用", widget)
        return False
    # Qt
    for qtv in ("Qt5Widgets", "Qt5Core", "Qt6Widgets", "Qt6Core"):
        res = subprocess.run(["pkg-config", "--exists", qtv], capture_output=True)
        if res.returncode == 0:
            ver = subprocess.run(["pkg-config", "--modversion", qtv], capture_output=True, text=True).stdout.strip()
            log(f"  ✓ pkg-config {qtv} = {ver}", widget)
            break
    else:
        log("  ⚠ pkg-config Qt5/Qt6 都未找到 (scons 编译 Qt 项目会失败)", widget)
    return True


def _stream_subprocess(cmd: list, cwd: str, widget, label: str) -> tuple:
    """跑子进程, 实时输出到 widget, 返 (rc, elapsed)"""
    log(f"  -> {' '.join(cmd)} (cwd={cwd})", widget)
    start = time.time()
    try:
        proc = subprocess.Popen(
            cmd, cwd=cwd,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,
        )
    except FileNotFoundError as e:
        log(f"  ❌ 命令不存在: {e}", widget)
        return 127, time.time() - start
    line_count = 0
    assert proc.stdout is not None
    for raw in proc.stdout:
        line = raw.rstrip()
        if not line:
            continue
        line_count += 1
        is_progress = "[%" in line and ("]" in line)
        is_error = "error:" in line.lower() or "Error " in line or "warning:" in line.lower()
        is_gxx = line.startswith("g++") or line.startswith("moc ") or line.startswith("cc ")
        if is_progress or is_error or is_gxx:
            log(f"  {line}", widget)
        else:
            try:
                with LOG_FILE.open("a", encoding="utf-8") as f:
                    f.write(f"[{_now()}]   {line}\n")
            except Exception:
                pass
            if widget is not None and line_count % 50 == 0:
                log(f"  ... ({label} {line_count} lines, see {LOG_FILE})", widget)
    proc.wait()
    elapsed = time.time() - start
    log(f"  {label} 完成: rc={proc.returncode} 耗时 {elapsed:.1f}s 共 {line_count} 行", widget)
    return proc.returncode, elapsed


def step_scons(jobs: int, projects: list, clean: bool, widget) -> bool:
    """跑顶层 scons -j<jobs> [project1] [project2]"""
    log("=" * 60, widget)
    cmd = ["scons", f"-j{jobs}"]
    if clean:
        cmd.append("--clean")
    cmd.extend(projects)
    log(f"STEP 2: {' '.join(cmd)} (顶层 SConstruct)", widget)
    log("=" * 60, widget)
    rc, _ = _stream_subprocess(cmd, cwd=str(AI_TOOLS_DIR), widget=widget, label="scons")
    if rc != 0:
        log(f"  ❌ scons 失败 rc={rc}", widget)
        try:
            lines = LOG_FILE.read_text(encoding="utf-8", errors="replace").splitlines()
            log("  -- 末尾 30 行 LOG_FILE 上下文 --", widget)
            for ln in lines[-30:]:
                log(f"    {ln}", widget)
        except Exception:
            pass
        return False
    return True


def step_verify(widget, projects: list, start_ts: float) -> bool:
    log("=" * 60, widget)
    log("STEP 3: 验产物 (各子项目 binary)", widget)
    log("=" * 60, widget)
    all_ok = True
    for sub in projects:
        sub_dir = AI_TOOLS_DIR / sub
        # 找 binary: 子项目 build/ 下的可执行文件, 或根目录的 ai_panel
        bins = list((sub_dir / "build").glob(sub)) if (sub_dir / "build").exists() else []
        if not bins:
            bins = list(sub_dir.glob("ai_panel"))
        if not bins:
            # 兜底: 找最大的可执行
            for f in sub_dir.glob("*"):
                if f.is_file() and os.access(f, os.X_OK) and not f.suffix:
                    bins.append(f)
        if not bins:
            log(f"  ⚠ {sub}/: 没找到 binary (scons 输出可能在别处)", widget)
            all_ok = False
            continue
        for b in bins:
            st = b.stat()
            ok = os.access(b, os.X_OK) and st.st_size > 50_000
            mark = "✓" if ok else "⚠"
            log(f"  {mark} {sub}/{b.name}: {st.st_size/1024:.1f} KB  mtime={time.strftime('%H:%M:%S', time.localtime(st.st_mtime))}", widget)
            if not ok:
                all_ok = False
    total = time.time() - start_ts
    log(f"  ✓ 全程耗时 {total:.1f}s", widget)
    return all_ok


def run_pipeline(jobs: int, projects: list, clean: bool, widget=None) -> bool:
    """主流程"""
    start_ts = time.time()
    log("=" * 60, widget)
    log(f"compile_ai_tools_gui.py 启动 (--projects={projects}  --clean={clean}  --jobs={jobs})", widget)
    log(f"  ai_tools_dir = {AI_TOOLS_DIR}", widget)
    log(f"  log          = {LOG_FILE}", widget)
    log("=" * 60, widget)
    if not step_check_top(widget):
        return False
    if not step_check_tools(widget):
        return False
    if not step_scons(jobs, projects, clean, widget):
        return False
    if not step_verify(widget, projects, start_ts):
        return False
    log("=" * 60, widget)
    log(f"✓ 全部编译完成: {projects}", widget)
    log(f"  跑产物: {' / '.join(['./' + p + '/build/' + p if (AI_TOOLS_DIR / p / 'build' / p).exists() else './' + p + '/ai_panel' for p in projects])}", widget)
    log(f"  全程 log: {LOG_FILE}", widget)
    log("=" * 60, widget)
    return True


def run_auto(jobs: int, projects: list, clean: bool):
    """--no-gui: 端到端跑, 不弹 GUI"""
    widget = None
    ok = run_pipeline(jobs, projects, clean, widget)
    sys.exit(0 if ok else 1)


def run_gui(jobs: int, projects: list, clean: bool, auto_start: bool = False):
    """GUI 模式: 弹窗 + Run 按钮 + 项目勾选"""
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    if LOG_FILE.exists():
        try:
            LOG_FILE.unlink()
        except Exception:
            pass
    root = tk.Tk()
    root.title(f"编译 ai_tools (scons) — compile_ai_tools_gui")
    root.geometry("1100x680")
    head = tk.Frame(root, bg="#1e1e1e", height=40)
    head.pack(fill="x")
    head.pack_propagate(False)
    lbl = tk.Label(
        head,
        text=f"ai_tools_dir={AI_TOOLS_DIR.name}/  jobs={jobs}  --clean={clean}  --auto={auto_start}",
        bg="#1e1e1e", fg="#e0e0e0", font=("monospace", 10),
        anchor="w", padx=12,
    )
    lbl.pack(side="left", fill="y")
    # 项目勾选区
    proj_frame = tk.LabelFrame(root, text="  编译的 C++ 子项目 (scons)  ", font=("monospace", 10, "bold"))
    proj_frame.pack(fill="x", padx=4, pady=4)
    check_vars = {}
    detected = detect_subprojects()
    if not detected:
        tk.Label(proj_frame, text="(没找到 SConstruct)", fg="red").pack(anchor="w", padx=8, pady=4)
    else:
        for name, scons_p, cpp_count in detected:
            var = tk.BooleanVar(value=(name in projects))
            check_vars[name] = var
            cb = tk.Checkbutton(
                proj_frame,
                text=f"  {name}/  ({cpp_count} .cpp, SConstruct)",
                variable=var, font=("monospace", 10),
                anchor="w",
            )
            cb.pack(anchor="w", padx=8, pady=2)
    # 日志区
    txt = scrolledtext.ScrolledText(
        root, font=("monospace", 9), bg="#0e0e0e", fg="#e0e0e0",
        insertbackground="#e0e0e0", wrap="word",
    )
    txt.pack(fill="both", expand=True, padx=4, pady=4)
    # 按钮区
    btn_frame = tk.Frame(root)
    btn_frame.pack(fill="x", pady=4)

    def get_selected_projects():
        return [n for n, v in check_vars.items() if v.get()]

    def go():
        sel = get_selected_projects()
        if not sel:
            log("⚠ 没勾选任何子项目, 不跑", txt)
            return
        btn_run.config(state="disabled", text=f"Running ({len(sel)} proj)...")
        def worker():
            try:
                run_pipeline(jobs, sel, clean, txt)
            finally:
                btn_run.config(state="normal", text="Run (--auto)")
        threading.Thread(target=worker, daemon=True).start()

    btn_run = tk.Button(btn_frame, text="Run (--auto)", command=go, font=("monospace", 11, "bold"))
    btn_run.pack(side="left", padx=8)

    def open_log():
        if LOG_FILE.exists():
            subprocess.Popen(["xdg-open", str(LOG_FILE)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    btn_log = tk.Button(btn_frame, text="Open log", command=open_log)
    btn_log.pack(side="left", padx=4)

    def quit_app():
        root.destroy()
    btn_q = tk.Button(btn_frame, text="Quit", command=quit_app)
    btn_q.pack(side="right", padx=8)

    if auto_start:
        root.after(400, go)
    root.mainloop()


def main():
    p = argparse.ArgumentParser(description="编译 ai_tools/ 下所有 C++ 子项目 (scons) GUI 启动器")
    p.add_argument("--auto", action="store_true", help="弹 GUI + 启动后自动跑 (不让你点 Run 按钮)")
    p.add_argument("--no-gui", action="store_true", help="纯后台跑, 不弹 GUI (适合 SSH/沙箱)")
    p.add_argument("--clean", action="store_true", help="干净重建 (scons --clean + rm .o)")
    p.add_argument("--jobs", "-j", type=int, default=max(1, os.cpu_count() or 2), help="并行 jobs (默认 nproc)")
    p.add_argument("--project", action="append", help="指定编译哪个子项目 (可多次, 默认所有), 例: --project ab")
    args = p.parse_args()
    # 默认项目: 所有 SUBDIRS (或 detect 出来的)
    if args.project:
        projects = args.project
    else:
        detected = detect_subprojects()
        projects = [n for n, _, _ in detected] or SUBDIRS
    if args.no_gui:
        run_auto(args.jobs, projects, args.clean)
    else:
        run_gui(args.jobs, projects, args.clean, auto_start=args.auto)


if __name__ == "__main__":
    main()
