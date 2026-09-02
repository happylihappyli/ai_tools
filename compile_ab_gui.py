#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
compile_ab_gui.py
================================================================
一键编译 ai_tools/ab (C++ Qt5/6 GUI 工具) + 实时日志 GUI

目的 (2026-09-02):
  编译 /home/bv/code/ai_tools/ab (C++ Qt), 显示 GUI 日志窗口,
  按你规则:
    - 执行时间长 (scons / cmake + make -j) → 不直接跑
    - 写 GUI 启动器 + --auto 自动跑, 不让你点

后端 (2026-09-02 新增 scons 支持):
  --backend scons  (默认)  走 SConstruct, scons -j$(nproc)
  --backend cmake          走老 CMakeLists.txt, cmake + make
  --backend all            两个都跑 (验证一致性)

功能 (scons 路径):
  STEP 0  验源码 (ab/SConstruct + ab/src/*.cpp + ab/src/*.h)
  STEP 1  验 build 目录 + 现有 binary 时间戳
  STEP 2  跑 scons -j$(nproc)  (MOC + 编译 + 链接, 实时输出)
  STEP 3  验产物: ab/build/ab 存在 + 可执行 + size > 100K
  STEP 4  报告 (编译时长 + 产物信息)

用法:
  python3 compile_ab_gui.py                    # 弹 GUI 手动, 默认 scons
  python3 compile_ab_gui.py --auto             # GUI + 自动跑
  python3 compile_ab_gui.py --no-gui           # 纯后台 (SSH/沙箱)
  python3 compile_ab_gui.py --clean            # 干净重建
  python3 compile_ab_gui.py --backend cmake    # 用 cmake 不用 scons
  python3 compile_ab_gui.py --backend all      # scons + cmake 都跑
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

AB_DIR = Path("/home/bv/code/ai_tools/ab")
AB_BUILD = AB_DIR / "build"
AB_BIN = AB_BUILD / "ab"
AB_SCONSTRUCT = AB_DIR / "SConstruct"
AB_CMAKE = AB_DIR / "CMakeLists.txt"
LOG_DIR = Path("/home/bv/code/ai_tools/.ai_tools")
LOG_FILE = LOG_DIR / "compile_ab.log"


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


def _newest_src_mtime(backend: str) -> int:
    """扫 ab/src/ 全部 .cpp/.h + build script, 返最大 mtime"""
    newest = 0
    candidates = [AB_DIR / "SConstruct", AB_DIR / "CMakeLists.txt", AB_DIR / "SConstruct.py"]
    for p in candidates:
        if p.exists():
            m = p.stat().st_mtime
            if m > newest:
                newest = m
    if (AB_DIR / "src").exists():
        for f in (AB_DIR / "src").rglob("*"):
            if f.is_file() and (f.suffix in (".cpp", ".h", ".hpp")):
                m = f.stat().st_mtime
                if m > newest:
                    newest = m
    return newest


def need_rebuild(backend: str) -> tuple:
    """返 (need: bool, reason: str)"""
    if not AB_BIN.exists():
        return True, "binary 不存在"
    bin_mtime = AB_BIN.stat().st_mtime
    src_mtime = _newest_src_mtime(backend)
    if src_mtime > bin_mtime:
        return True, f"源码比 binary 新 (src={src_mtime:.0f} > bin={bin_mtime:.0f})"
    return False, f"binary 已是最新 (src={src_mtime:.0f} <= bin={bin_mtime:.0f})"


def step_check_src(backend: str, widget):
    log("=" * 60, widget)
    log(f"STEP 0: 验源码 (backend={backend})", widget)
    log("=" * 60, widget)
    src_dir = AB_DIR / "src"
    if not src_dir.exists():
        log(f"  ❌ 缺 {src_dir}", widget)
        return False
    cpp_count = len(list(src_dir.glob("*.cpp")))
    h_count = len(list(src_dir.glob("*.h")))
    log(f"  ✓ {src_dir} ({cpp_count} .cpp + {h_count} .h)", widget)
    # 找 Q_OBJECT 头
    qobj = []
    for h in src_dir.glob("*.h"):
        try:
            txt = h.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        if "Q_OBJECT" in txt:
            qobj.append(h.name)
    log(f"  Q_OBJECT headers (需要 MOC): {qobj}", widget)
    # 后端 build script
    if backend in ("scons", "all"):
        if not AB_SCONSTRUCT.exists():
            log(f"  ❌ 缺 {AB_SCONSTRUCT} (--backend scons 必需)", widget)
            return False
        log(f"  ✓ {AB_SCONSTRUCT}", widget)
    if backend in ("cmake", "all"):
        if not AB_CMAKE.exists():
            log(f"  ❌ 缺 {AB_CMAKE} (--backend cmake 必需)", widget)
            return False
        log(f"  ✓ {AB_CMAKE}", widget)
    # pkg-config
    for qtv in ("Qt5Widgets", "Qt5Core"):
        res = subprocess.run(["pkg-config", "--exists", qtv], capture_output=True)
        if res.returncode == 0:
            ver = subprocess.run(["pkg-config", "--modversion", qtv], capture_output=True, text=True).stdout.strip()
            log(f"  ✓ pkg-config {qtv} = {ver}", widget)
            break
    else:
        log("  ⚠ pkg-config Qt5 未找到, scons 会自己兜底 (沙箱常见)", widget)
    return True


def step_check_state(clean: bool, backend: str, widget):
    log("=" * 60, widget)
    log(f"STEP 1: 验 build 目录 + binary 状态 (backend={backend})" + (" (--clean: 干净重建)" if clean else ""), widget)
    log("=" * 60, widget)
    if clean and AB_BUILD.exists():
        log(f"  --clean: rm -rf {AB_BUILD}", widget)
        try:
            shutil.rmtree(AB_BUILD)
            log(f"  ✓ 已清", widget)
        except Exception as e:
            log(f"  ❌ rm 失败: {e}", widget)
            return False
    if AB_BIN.exists():
        st = AB_BIN.stat()
        log(f"  现有 binary: {AB_BIN} size={st.st_size} mtime={time.strftime('%Y-%m-%d %H:%M', time.localtime(st.st_mtime))}", widget)
    else:
        log(f"  现有 binary: 不存在", widget)
    need, reason = need_rebuild(backend)
    log(f"  rebuild 判定: {reason}", widget)
    return need  # True = 要重编, False = skip


def _stream_subprocess(cmd: list, cwd: str, widget, label: str, timeout: int = 600) -> tuple:
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
        if is_progress or is_error:
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


def step_scons(jobs: int, widget) -> bool:
    """跑 scons -j<jobs> (走 SConstruct)"""
    log("=" * 60, widget)
    log(f"STEP 2: scons -j{jobs} (MOC + 编译 + 链接)", widget)
    log("=" * 60, widget)
    rc, _ = _stream_subprocess(
        ["scons", f"-j{jobs}"], cwd=str(AB_DIR), widget=widget, label="scons",
    )
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


def step_cmake_then_make(jobs: int, widget) -> bool:
    """cmake .. + make -j<jobs> (老 cmake 路径)"""
    log("=" * 60, widget)
    log(f"STEP 2a: cmake .. (生成 Makefile)", widget)
    log("=" * 60, widget)
    AB_BUILD.mkdir(parents=True, exist_ok=True)
    rc, _ = _stream_subprocess(
        ["cmake", ".."], cwd=str(AB_BUILD), widget=widget, label="cmake",
        timeout=120,
    )
    if rc != 0:
        log(f"  ❌ cmake 失败 rc={rc}", widget)
        return False
    log("=" * 60, widget)
    log(f"STEP 2b: make -j{jobs} (编译)", widget)
    log("=" * 60, widget)
    rc, _ = _stream_subprocess(
        ["make", f"-j{jobs}"], cwd=str(AB_BUILD), widget=widget, label="make",
        timeout=600,
    )
    if rc != 0:
        log(f"  ❌ make 失败 rc={rc}", widget)
        try:
            lines = LOG_FILE.read_text(encoding="utf-8", errors="replace").splitlines()
            log("  -- 末尾 30 行 LOG_FILE 上下文 --", widget)
            for ln in lines[-30:]:
                log(f"    {ln}", widget)
        except Exception:
            pass
        return False
    return True


def step_verify(widget, start_ts: float) -> bool:
    log("=" * 60, widget)
    log("STEP 3: 验产物 (ab/build/ab 存在 + 可执行 + size)", widget)
    log("=" * 60, widget)
    if not AB_BIN.exists():
        log(f"  ❌ {AB_BIN} 仍不存在", widget)
        return False
    st = AB_BIN.stat()
    log(f"  ✓ {AB_BIN}", widget)
    log(f"    size={st.st_size} bytes ({st.st_size/1024:.1f} KB)", widget)
    log(f"    mtime={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(st.st_mtime))}", widget)
    if not os.access(AB_BIN, os.X_OK):
        log(f"  ❌ {AB_BIN} 不可执行 (缺 +x)", widget)
        return False
    log(f"  ✓ +x OK", widget)
    if st.st_size < 100_000:
        log(f"  ⚠ size={st.st_size} 偏小 (期望 400K+), 编译可能没全过", widget)
        return False
    total = time.time() - start_ts
    log(f"  ✓ 全程耗时 {total:.1f}s", widget)
    return True


def run_pipeline(clean: bool, jobs: int, backend: str, widget=None) -> bool:
    """主流程: STEP 0/1/2/3/4 串行, 任一失败立刻 return False"""
    start_ts = time.time()
    log("=" * 60, widget)
    log(f"compile_ab_gui.py 启动 (--backend={backend}  --clean={clean}  --jobs={jobs})", widget)
    log(f"  ab_dir = {AB_DIR}", widget)
    log(f"  log    = {LOG_FILE}", widget)
    log("=" * 60, widget)
    if not step_check_src(backend, widget):
        return False
    need = step_check_state(clean, backend, widget)
    if not need and backend in ("scons", "cmake"):
        log("  binary 已是最新, 跳过编译 (如想强制重编加 --clean)", widget)
        return step_verify(widget, start_ts)
    # 按 backend 跑
    if backend == "scons":
        if not step_scons(jobs, widget):
            return False
    elif backend == "cmake":
        if not step_cmake_then_make(jobs, widget):
            return False
    elif backend == "all":
        log("  --backend all: 先跑 scons, 再跑 cmake (验一致性)", widget)
        if not step_scons(jobs, widget):
            return False
        # scons build/ 已存在, cmake 路径会冲突, 临时挪开
        backup = AB_BUILD.parent / "build.scons.bak"
        if AB_BUILD.exists():
            log(f"  --backend all: 备份 {AB_BUILD} → {backup}", widget)
            if backup.exists():
                shutil.rmtree(backup)
            shutil.move(str(AB_BUILD), str(backup))
        if not step_cmake_then_make(jobs, widget):
            # 还原 scons build
            if backup.exists():
                if AB_BUILD.exists():
                    shutil.rmtree(AB_BUILD)
                shutil.move(str(backup), str(AB_BUILD))
            return False
        # cmake 跑完删掉, 还原 scons build (因为 ab_launcher 找 build/ab)
        if AB_BUILD.exists():
            shutil.rmtree(AB_BUILD)
        if backup.exists():
            shutil.move(str(backup), str(AB_BUILD))
            log(f"  --backend all: 还原 scons build → {AB_BUILD}", widget)
    else:
        log(f"  ❌ 未知 backend: {backend}", widget)
        return False
    if not step_verify(widget, start_ts):
        return False
    log("=" * 60, widget)
    log(f"✓ 编译完成 (backend={backend}): ab 已就绪", widget)
    log(f"  binary: {AB_BIN}", widget)
    log(f"  全程 log: {LOG_FILE}", widget)
    log("  跑 GUI: ab / ab --doctor / ac ab", widget)
    log("=" * 60, widget)
    return True


def run_auto(clean: bool, jobs: int, backend: str):
    """--no-gui: 端到端跑, 不弹 GUI"""
    widget = None
    ok = run_pipeline(clean, jobs, backend, widget)
    sys.exit(0 if ok else 1)


def run_gui(clean: bool, jobs: int, backend: str, auto_start: bool = False):
    """GUI 模式: 弹窗 + Run 按钮
    auto_start=True 时 启动 400ms 后自动点 Run (按你规则不让你点)
    """
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    if LOG_FILE.exists():
        try:
            LOG_FILE.unlink()
        except Exception:
            pass
    root = tk.Tk()
    root.title(f"编译 ab (ai_tools) — {backend} — compile_ab_gui")
    root.geometry("1100x650")
    head = tk.Frame(root, bg="#1e1e1e", height=40)
    head.pack(fill="x")
    head.pack_propagate(False)
    lbl = tk.Label(
        head,
        text=f"backend={backend}  jobs={jobs}  --clean={clean}  --auto={auto_start}",
        bg="#1e1e1e", fg="#e0e0e0", font=("monospace", 10),
        anchor="w", padx=12,
    )
    lbl.pack(side="left", fill="y")
    txt = scrolledtext.ScrolledText(
        root, font=("monospace", 9), bg="#0e0e0e", fg="#e0e0e0",
        insertbackground="#e0e0e0", wrap="word",
    )
    txt.pack(fill="both", expand=True, padx=4, pady=4)
    btn_frame = tk.Frame(root)
    btn_frame.pack(fill="x", pady=4)

    def go():
        btn_run.config(state="disabled", text="Running...")
        def worker():
            try:
                run_pipeline(clean, jobs, backend, txt)
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
    p = argparse.ArgumentParser(description="编译 ai_tools/ab (C++ Qt) GUI 启动器")
    p.add_argument("--auto", action="store_true", help="弹 GUI + 启动后自动跑 (不让你点 Run 按钮)")
    p.add_argument("--no-gui", action="store_true", help="纯后台跑, 不弹 GUI (适合 SSH/沙箱)")
    p.add_argument("--clean", action="store_true", help="干净重建 (rm -rf build/ab + 重跑 backend)")
    p.add_argument("--jobs", "-j", type=int, default=max(1, os.cpu_count() or 2), help="并行 jobs (默认 nproc)")
    p.add_argument("--backend", "-b", choices=["scons", "cmake", "all"], default="scons",
                   help="编译后端: scons (默认) / cmake / all (两个都跑验一致性)")
    args = p.parse_args()
    if args.no_gui:
        run_auto(args.clean, args.jobs, args.backend)
    else:
        run_gui(args.clean, args.jobs, args.backend, auto_start=args.auto)


if __name__ == "__main__":
    main()
