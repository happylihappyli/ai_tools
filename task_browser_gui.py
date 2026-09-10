#!/usr/bin/env python3
# task_browser_gui.py — ac 任务浏览器 (PyQt6 深色 0-click GUI)
# =====================================================================
# 2026-09-10 新增 — 按用户需求:
#   "任务要标识一下, 当前命令的几个任务, 还有历史任务, 增加一个类型, 区分一下"
#   "点击这个任务, 右边日志可以显示这个任务当前跑完成的日志信息"
#   "这样日志信息就属于每个任务, 界面布局也修改一下"
#
# 功能:
#   - 左侧: 任务列表 (按 type + category 分组, 颜色编码, 状态 icon)
#   - 右侧: 选中 task 的 log 查看器 (支持 latest.log + 历史 log 切换)
#   - 顶栏: 任务计数 (2026-09-10 去掉搜索/过滤, 任务全部显示)
#   - 底栏: 状态栏 + 按钮 (运行 / 复制 log / 打开 log 文件夹 / 刷新)
#
# 数据源:
#   - tasks: ai_build.json 的 tasks[] (每个 task 有 type + category)
#   - logs:  /tmp/ac_task_logs/<task_name>/<ts>.log + latest.meta.json
#
# 用户偏好落地:
#   - 深色主题 (Fusion 暗色 + 调色板)
#   - 任务列表左侧 dock (split 35%/65%)
#   - 状态栏底部 (状态 + 当前选中 task)
#   - 菜单栏 + 工具栏 + 快捷键 (Ctrl+R 运行 / Ctrl+L 复制 log / Ctrl+O 打开 log 文件夹)
#   - 颜色编码: build=蓝, rebuild=橙, poc=紫, test=绿, run=黄, diag=青, fix=粉, kill=红
#   - 配置驱动: 全部 task/type/category 从 ai_build.json 读
#   - 0 点击: 启动自动加载 task 列表, 自动选第一个
#
# 依赖: PyQt6. 缺失时 fallback 到 no-gui (终端表格)
# =====================================================================

import argparse
import json
import os
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Optional

# 防止无 PyQt6 环境的崩
try:
    from PyQt6.QtCore import Qt, QSize, QTimer, QProcess
    from PyQt6.QtGui import QAction, QFont, QColor, QPalette, QIcon, QKeySequence, QTextCursor
    from PyQt6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QSplitter,
        QListWidget, QListWidgetItem, QPlainTextEdit, QStatusBar, QPushButton,
        QLabel, QComboBox, QToolBar, QFileDialog, QMessageBox,
        QGroupBox, QFrame,
    )
    HAS_PYQT6 = True
except ImportError:
    HAS_PYQT6 = False


# ---------- 路径常量 ----------
GODOT_UI_DIR = Path("/home/bv/code/godot_ui_linux")
AI_BUILD_JSON = GODOT_UI_DIR / "ai_build.json"
TASK_LOG_ROOT = Path("/tmp/ac_task_logs")


# ---------- 类型/Category 颜色编码 (深色主题) ----------
TYPE_COLORS = {
    "build":   ("#88ccff", "🔨", "Build"),       # 浅蓝
    "rebuild": ("#ffaa44", "♻️",  "Rebuild"),     # 橙
    "poc":     ("#cc88ff", "🚀", "POC"),          # 紫
    "test":    ("#88ff88", "🧪", "Test"),         # 绿
    "run":     ("#ffff88", "▶️",  "Run"),         # 黄
    "diag":    ("#88ffff", "🔍", "Diag"),         # 青
    "fix":     ("#ff88cc", "🔧", "Fix"),          # 粉
    "kill":    ("#ff8888", "✖",  "Kill"),         # 红
    "deploy":  ("#aaffaa", "📦", "Deploy"),       # 浅绿
    "other":   ("#aaaaaa", "❓", "Other"),         # 灰
}

CATEGORY_BADGE = {
    "current": ("⭐", "当前"),
    "history": ("📜", "历史"),
}

STATUS_COLORS = {
    "ok":      "#88ff88",  # 绿
    "fail":    "#ff8888",  # 红
    "running": "#ffff88",  # 黄
    "never":   "#888888",  # 灰
}


# ---------- ai_build.json 加载 ----------
def load_ai_build() -> dict:
    if not AI_BUILD_JSON.is_file():
        return {"tasks": []}
    try:
        with open(AI_BUILD_JSON, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"⚠ 读 {AI_BUILD_JSON} 失败: {e}", file=sys.stderr)
        return {"tasks": []}


def get_task_meta(task: dict) -> dict:
    """从 task dict 抽取 name/type/category/cmd/desc"""
    return {
        "name": task.get("name", "?"),
        "type": task.get("type", "other"),
        "category": task.get("category", "current"),
        "cmd": task.get("cmd", ""),
        "desc": task.get("desc", ""),
    }


def get_latest_task_log(task_name: str) -> Optional[dict]:
    """读 /tmp/ac_task_logs/<name>/latest.meta.json"""
    meta_file = TASK_LOG_ROOT / task_name / "latest.meta.json"
    if not meta_file.is_file():
        return None
    try:
        with open(meta_file, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def list_task_log_history(task_name: str) -> list:
    """列 task 的所有历史 log 文件 (按 ts 倒序)"""
    task_dir = TASK_LOG_ROOT / task_name
    if not task_dir.is_dir():
        return []
    out = []
    for log_file in sorted(task_dir.glob("*.log"), reverse=True):
        # 跳过 latest.log (那是 symlink/copy)
        if log_file.name == "latest.log":
            continue
        meta_file = task_dir / (log_file.stem + ".meta.json")
        meta = {}
        if meta_file.is_file():
            try:
                with open(meta_file, "r", encoding="utf-8") as f:
                    meta = json.load(f)
            except (OSError, json.JSONDecodeError):
                pass
        out.append({
            "ts": log_file.stem,
            "log_file": str(log_file),
            "rc": meta.get("rc", -1),
            "size": log_file.stat().st_size,
            "ts_iso": meta.get("ts_iso", ""),
        })
    return out


def get_task_status(task_name: str) -> str:
    """从 latest.meta.json 返 status 字符串 (ok/fail/running/never)"""
    meta = get_latest_task_log(task_name)
    if meta is None:
        return "never"
    rc = meta.get("rc", -1)
    if rc == 0:
        return "ok"
    if rc == 999:  # 用 999 标 running
        return "running"
    return "fail"


def is_multi_subtask(task_name: str) -> bool:
    """task 是否是 multi-sub-task (cmd 是数组)"""
    meta = get_latest_task_log(task_name)
    if meta is None:
        return False
    return bool(meta.get("is_multi_subtask"))


def get_sub_tasks(task_name: str) -> list:
    """读 latest.meta.json 的 sub_tasks[] 列表"""
    meta = get_latest_task_log(task_name)
    if meta is None:
        return []
    return meta.get("sub_tasks", [])


def get_sub_status_counts(task_name: str) -> dict:
    """统计 sub-task 状态: {ok: n, fail: n, total: n}"""
    subs = get_sub_tasks(task_name)
    if not subs:
        return {"ok": 0, "fail": 0, "total": 0}
    ok = sum(1 for s in subs if s.get("sub_rc") == 0)
    return {"ok": ok, "fail": len(subs) - ok, "total": len(subs)}


# ---------- 任务运行 (调 ac --task) ----------
def run_task(task_name: str, log_path: str) -> int:
    """用 ac --task 跑 task, log 落盘到 log_path. 同步阻塞. 返 rc."""
    cmd = ["ac", "--offscreen", "--cli", "--log", log_path, "--task", task_name]
    print(f"$ {' '.join(cmd)}")
    try:
        return subprocess.call(cmd)
    except FileNotFoundError as e:
        print(f"FATAL: ac 找不到: {e}", file=sys.stderr)
        return 127


# ---------- GUI ----------
def build_gui() -> "QMainWindow":
    """构造 PyQt6 主窗口. 返回 QMainWindow 实例."""
    app = QApplication.instance() or QApplication(sys.argv)
    app.setStyle("Fusion")

    # 暗色调色板
    palette = QPalette()
    palette.setColor(QPalette.ColorRole.Window, QColor(30, 30, 30))
    palette.setColor(QPalette.ColorRole.WindowText, QColor(220, 220, 220))
    palette.setColor(QPalette.ColorRole.Base, QColor(20, 20, 20))
    palette.setColor(QPalette.ColorRole.AlternateBase, QColor(40, 40, 40))
    palette.setColor(QPalette.ColorRole.Text, QColor(220, 220, 220))
    palette.setColor(QPalette.ColorRole.Button, QColor(50, 50, 50))
    palette.setColor(QPalette.ColorRole.ButtonText, QColor(220, 220, 220))
    palette.setColor(QPalette.ColorRole.Highlight, QColor(70, 130, 180))
    palette.setColor(QPalette.ColorRole.HighlightedText, QColor(255, 255, 255))
    palette.setColor(QPalette.ColorRole.ToolTipBase, QColor(50, 50, 50))
    palette.setColor(QPalette.ColorRole.ToolTipText, QColor(220, 220, 220))
    app.setPalette(palette)

    window = QMainWindow()
    window.setWindowTitle("ac 任务浏览器 (0-click) — task_browser_gui.py")
    window.resize(1400, 800)

    # ===== 菜单栏 =====
    menu_bar = window.menuBar()
    file_menu = menu_bar.addMenu("&文件")
    act_refresh = QAction("刷新 (F5)", window)
    act_refresh.setShortcut(QKeySequence("F5"))
    file_menu.addAction(act_refresh)
    file_menu.addSeparator()
    act_open_log_dir = QAction("打开 log 文件夹 (Ctrl+O)", window)
    act_open_log_dir.setShortcut(QKeySequence("Ctrl+O"))
    file_menu.addAction(act_open_log_dir)
    file_menu.addSeparator()
    act_quit = QAction("退出 (Ctrl+Q)", window)
    act_quit.setShortcut(QKeySequence("Ctrl+Q"))
    file_menu.addAction(act_quit)

    run_menu = menu_bar.addMenu("&运行")
    act_run = QAction("运行选中 task (Ctrl+R)", window)
    act_run.setShortcut(QKeySequence("Ctrl+R"))
    run_menu.addAction(act_run)
    act_kill = QAction("杀 cloud_main", window)
    act_kill.setShortcut(QKeySequence("Ctrl+K"))
    run_menu.addAction(act_kill)

    help_menu = menu_bar.addMenu("&帮助")
    act_about = QAction("关于", window)
    help_menu.addAction(act_about)

    # ===== 工具栏 =====
    toolbar = QToolBar("Main")
    toolbar.setIconSize(QSize(16, 16))
    toolbar.setMovable(True)
    window.addToolBar(toolbar)
    toolbar.addAction(act_run)
    toolbar.addAction(act_refresh)
    toolbar.addSeparator()
    toolbar.addAction(act_open_log_dir)
    toolbar.addAction(act_kill)

    # ===== 顶栏: 任务计数 (2026-09-10 去掉搜索/过滤, 任务全部显示) =====
    top = QWidget()
    top_layout = QHBoxLayout(top)
    top_layout.setContentsMargins(8, 4, 8, 4)
    top_layout.addStretch()
    lbl_count = QLabel("任务数: 0")
    lbl_count.setFont(QFont("monospace", 11, QFont.Weight.Bold))
    lbl_count.setStyleSheet("color: #88ccff;")
    top_layout.addWidget(lbl_count)
    top_layout.addStretch()

    # ===== 主 split: 左 task list / 右 log =====
    splitter = QSplitter(Qt.Orientation.Horizontal)

    # --- 左: task list ---
    left = QWidget()
    left_layout = QVBoxLayout(left)
    left_layout.setContentsMargins(4, 4, 4, 4)
    lbl_list = QLabel("📋 任务列表 (44 个, 按 type 颜色编码 + 状态 icon)")
    lbl_list.setStyleSheet("color: #88ccff; font-weight: bold;")
    left_layout.addWidget(lbl_list)
    list_tasks = QListWidget()
    list_tasks.setFont(QFont("monospace", 10))
    list_tasks.setStyleSheet(
        "QListWidget { background-color: #1a1a1a; }"
        "QListWidget::item { padding: 4px; border-bottom: 1px solid #2a2a2a; }"
        "QListWidget::item:selected { background-color: #4682b4; }"
    )
    left_layout.addWidget(list_tasks)

    # --- 左下: type 统计 panel (新增, 布局优化) ---
    stat_box = QGroupBox("📊 按类型统计 (type)")
    stat_layout = QVBoxLayout(stat_box)
    stat_layout.setContentsMargins(6, 6, 6, 6)
    stat_layout.setSpacing(2)
    lbl_stat_lines = []  # 每行一个 type 统计
    # 提前创建 10 行 (9 type + 其他), 内容后面 reload 时填
    for typ_key, (color, icon, label) in TYPE_COLORS.items():
        line = QLabel(f"  {icon} {label:8s} : 0")
        line.setStyleSheet(f"color: {color};")
        line.setFont(QFont("monospace", 10))
        stat_layout.addWidget(line)
        lbl_stat_lines.append((typ_key, line))
    left_layout.addWidget(stat_box)

    splitter.addWidget(left)

    # --- 右: log viewer ---
    right = QWidget()
    right_layout = QVBoxLayout(right)
    right_layout.setContentsMargins(4, 4, 4, 4)

    # task 头部信息 (含大 type badge, 布局优化)
    header_box = QGroupBox("任务详情")
    header_layout = QVBoxLayout(header_box)
    # 顶行: 大 badge + task name
    header_top = QHBoxLayout()
    lbl_type_badge = QLabel("(类型)")
    lbl_type_badge.setFont(QFont("monospace", 11, QFont.Weight.Bold))
    lbl_type_badge.setAlignment(Qt.AlignmentFlag.AlignCenter)
    lbl_type_badge.setStyleSheet(
        "background-color: #444; color: #fff; padding: 4px 10px; "
        "border-radius: 4px; border: 1px solid #888;"
    )
    lbl_type_badge.setMaximumWidth(100)
    header_top.addWidget(lbl_type_badge)
    lbl_task_name = QLabel("(无选中 task)")
    lbl_task_name.setFont(QFont("monospace", 14, QFont.Weight.Bold))
    lbl_task_name.setStyleSheet("color: #88ccff;")
    header_top.addWidget(lbl_task_name, stretch=1)
    lbl_status_badge = QLabel("(状态)")
    lbl_status_badge.setFont(QFont("monospace", 11, QFont.Weight.Bold))
    lbl_status_badge.setAlignment(Qt.AlignmentFlag.AlignCenter)
    lbl_status_badge.setStyleSheet(
        "background-color: #444; color: #fff; padding: 4px 10px; "
        "border-radius: 4px; border: 1px solid #888;"
    )
    lbl_status_badge.setMaximumWidth(120)
    header_top.addWidget(lbl_status_badge)
    header_layout.addLayout(header_top)
    lbl_task_meta = QLabel("")
    lbl_task_meta.setStyleSheet("color: #888888;")
    header_layout.addWidget(lbl_task_meta)
    lbl_task_cmd = QLabel("")
    lbl_task_cmd.setStyleSheet("color: #aaffaa; font-family: monospace; font-size: 10pt;")
    lbl_task_cmd.setWordWrap(True)
    lbl_task_cmd.setMaximumHeight(50)
    header_layout.addWidget(lbl_task_cmd)
    lbl_task_desc = QLabel("")
    lbl_task_desc.setWordWrap(True)
    lbl_task_desc.setStyleSheet("color: #cccccc;")
    lbl_task_desc.setMaximumHeight(80)
    header_layout.addWidget(lbl_task_desc)
    right_layout.addWidget(header_box)

    # log 切换: 最新 + 历史下拉
    log_ctrl = QHBoxLayout()
    lbl_log = QLabel("📄 Log:")
    lbl_log.setStyleSheet("color: #88ccff; font-weight: bold;")
    log_ctrl.addWidget(lbl_log)
    combo_log_history = QComboBox()
    combo_log_history.setMaximumWidth(380)
    combo_log_history.addItem("🆕 latest.log", "latest")
    log_ctrl.addWidget(combo_log_history)
    btn_refresh_log = QPushButton("🔄 刷新 log")
    log_ctrl.addWidget(btn_refresh_log)
    btn_copy_log = QPushButton("📋 复制")
    log_ctrl.addWidget(btn_copy_log)
    btn_open_log_file = QPushButton("📂 打开")
    log_ctrl.addWidget(btn_open_log_file)
    log_ctrl.addStretch()
    right_layout.addLayout(log_ctrl)

    # log 摘要 (新增, 布局优化): 行数 / 字节数 / FATAL / Error / Warning 计数
    lbl_log_stats = QLabel("📊 log 摘要: 等待加载")
    lbl_log_stats.setStyleSheet(
        "color: #888888; background-color: #1a1a1a; padding: 4px 8px; "
        "border: 1px solid #333; border-radius: 3px;"
    )
    lbl_log_stats.setFont(QFont("monospace", 10))
    right_layout.addWidget(lbl_log_stats)

    # sub-task 列表 (multi-sub-task 才显示, 默认隐藏)
    sub_box = QGroupBox("📋 Sub-task 列表 (multi-sub-task)")
    sub_layout = QVBoxLayout(sub_box)
    sub_layout.setContentsMargins(4, 4, 4, 4)
    sub_layout.setSpacing(2)
    sub_list = QListWidget()
    sub_list.setFont(QFont("monospace", 10))
    sub_list.setMaximumHeight(120)
    sub_list.setStyleSheet(
        "QListWidget { background-color: #1a1a1a; }"
        "QListWidget::item { padding: 2px; border-bottom: 1px solid #2a2a2a; }"
        "QListWidget::item:selected { background-color: #4682b4; }"
    )
    sub_layout.addWidget(sub_list)
    sub_box.setVisible(False)  # 默认隐藏
    right_layout.addWidget(sub_box)

    # log 文本区
    log_view = QPlainTextEdit()
    log_view.setReadOnly(True)
    log_view.setFont(QFont("monospace", 10))
    log_view.setStyleSheet(
        "QPlainTextEdit { background-color: #0a0a0a; color: #aaffaa; }"
    )
    right_layout.addWidget(log_view)

    # 操作按钮
    btn_layout = QHBoxLayout()
    btn_run = QPushButton("▶ 运行 (Ctrl+R)")
    btn_run.setStyleSheet("background-color: #2d4a2d; color: #88ff88; font-weight: bold;")
    btn_layout.addWidget(btn_run)
    btn_show_cmd = QPushButton("📜 显示命令")
    btn_layout.addWidget(btn_show_cmd)
    btn_open_log_dir2 = QPushButton("📂 log 文件夹")
    btn_layout.addWidget(btn_open_log_dir2)
    btn_layout.addStretch()
    right_layout.addLayout(btn_layout)

    splitter.addWidget(right)
    splitter.setSizes([450, 950])  # 左 35% 右 65%
    splitter.setStretchFactor(0, 0)
    splitter.setStretchFactor(1, 1)

    # ===== 主布局 =====
    central = QWidget()
    central_layout = QVBoxLayout(central)
    central_layout.setContentsMargins(0, 0, 0, 0)
    central_layout.addWidget(top)
    central_layout.addWidget(splitter)
    window.setCentralWidget(central)

    # ===== 底栏: 状态栏 =====
    status = QStatusBar()
    status.setStyleSheet("background-color: #2a2a2a; color: #ffff88;")
    window.setStatusBar(status)
    status.showMessage("✓ 加载 ai_build.json 中...")

    # ===== 业务逻辑 =====
    config = {"data": load_ai_build(), "selected_task": None,
              "running_proc": None, "tasks_index": {}}  # name -> list item

    def reload_task_list():
        """从 ai_build.json 重新加载 task 列表"""
        config["data"] = load_ai_build()
        list_tasks.clear()
        config["tasks_index"] = {}
        tasks = config["data"].get("tasks", [])
        # type 计数 (新增, 布局优化)
        type_counts = {typ: 0 for typ in TYPE_COLORS}
        for t in tasks:
            meta = get_task_meta(t)
            status_str = get_task_status(meta["name"])
            # 颜色 / 图标
            color_hex, icon, type_label = TYPE_COLORS.get(meta["type"], TYPE_COLORS["other"])
            cat_icon, cat_label = CATEGORY_BADGE.get(meta["category"], CATEGORY_BADGE["current"])
            status_color = STATUS_COLORS[status_str]
            # 状态符号
            status_sym = {"ok": "✓", "fail": "✗", "running": "⟳", "never": "·"}[status_str]
            # multi-sub-task 标识
            sub_tag = ""
            if is_multi_subtask(meta["name"]):
                sc = get_sub_status_counts(meta["name"])
                sub_tag = f" [{sc['ok']}/{sc['total']}]"
            # 显示文本: 紧凑
            text = f"  {icon} {meta['name']:30s}  {cat_icon} {status_sym}  [{type_label}]{sub_tag}"
            item = QListWidgetItem(text)
            # 颜色用 setForeground 传 QColor
            item.setForeground(QColor(color_hex))
            item.setData(Qt.ItemDataRole.UserRole, meta["name"])
            # hover tooltip 显示 desc (布局优化)
            tip_extra = ""
            if is_multi_subtask(meta["name"]):
                sc = get_sub_status_counts(meta["name"])
                tip_extra = f"\n📋 multi-sub-task: {sc['ok']}/{sc['total']} 成功"
            tip = (f"{icon} {meta['name']}\n"
                   f"类型: {type_label}  状态: {status_sym}\n"
                   f"类别: {cat_label}{tip_extra}\n\n{meta['desc'][:300]}")
            item.setToolTip(tip)
            list_tasks.addItem(item)
            config["tasks_index"][meta["name"]] = item
            # 计数
            tkey = meta["type"] if meta["type"] in type_counts else "other"
            type_counts[tkey] += 1
        lbl_count.setText(f"任务数: {len(tasks)}")
        # 更新 type 统计 panel
        for typ_key, line in lbl_stat_lines:
            color, icon, label = TYPE_COLORS[typ_key]
            n = type_counts.get(typ_key, 0)
            line.setText(f"  {icon} {label:8s} : {n}")
        status.showMessage(f"✓ 加载 {len(tasks)} 个 task (data: {AI_BUILD_JSON})", 4000)
        # 默认选第一个
        if list_tasks.count() > 0:
            list_tasks.setCurrentRow(0)

    def show_task_log(task_name: str):
        """读 task 的 log 落 log_view"""
        # 1) 更新 header
        t_meta = next((t for t in config["data"].get("tasks", [])
                       if t.get("name") == task_name), None)
        if t_meta is None:
            lbl_task_name.setText("(无选中 task)")
            lbl_task_meta.setText("")
            lbl_task_cmd.setText("")
            lbl_task_desc.setText("")
            lbl_type_badge.setText("(类型)")
            lbl_status_badge.setText("(状态)")
            return
        meta = get_task_meta(t_meta)
        status_str = get_task_status(task_name)
        color_hex, icon, type_label = TYPE_COLORS.get(meta["type"], TYPE_COLORS["other"])
        cat_icon, cat_label = CATEGORY_BADGE.get(meta["category"], CATEGORY_BADGE["current"])
        status_sym = {"ok": "✓", "fail": "✗", "running": "⟳", "never": "·"}[status_str]
        # 大 badge (布局优化)
        lbl_type_badge.setText(f"{icon} {type_label}")
        lbl_type_badge.setStyleSheet(
            f"background-color: {color_hex}; color: #000; padding: 4px 10px; "
            f"border-radius: 4px; border: 1px solid #888; font-weight: bold;"
        )
        status_color = STATUS_COLORS[status_str]
        lbl_status_badge.setText(f"{status_sym} {status_str.upper()}")
        lbl_status_badge.setStyleSheet(
            f"background-color: {status_color}; color: #000; padding: 4px 10px; "
            f"border-radius: 4px; border: 1px solid #888; font-weight: bold;"
        )
        lbl_task_name.setText(f"{task_name}")
        lbl_task_name.setStyleSheet(f"color: {color_hex};")
        latest = get_latest_task_log(task_name)
        if latest:
            meta_text = (f"类别: {cat_icon} {cat_label}  |  rc={latest.get('rc', '?')}  "
                         f"|  跑: {latest.get('ts_iso', '?')}")
            if latest.get("log_file"):
                meta_text += f"  |  log: {latest.get('log_file')}"
        else:
            meta_text = (f"类别: {cat_icon} {cat_label}  |  从未跑过 "
                         f"(先跑一次会写 /tmp/ac_task_logs/{task_name}/)")
        lbl_task_meta.setText(meta_text)
        # cmd (布局优化: 单行, 等宽)
        cmd = meta["cmd"]
        if len(cmd) > 200:
            cmd_show = cmd[:200] + "..."
        else:
            cmd_show = cmd
        lbl_task_cmd.setText(f"$ {cmd_show}" if cmd_show else "")
        # desc 摘要
        desc = meta["desc"]
        if len(desc) > 300:
            desc = desc[:300] + "..."
        lbl_task_desc.setText(desc or "(无描述)")

        # 2) 刷新 log history 下拉
        combo_log_history.clear()
        combo_log_history.addItem("🆕 latest.log", "latest")
        for h in list_task_log_history(task_name):
            rc_sym = "✓" if h["rc"] == 0 else "✗"
            size_kb = h["size"] // 1024
            label = f"{h['ts']} {rc_sym} rc={h['rc']} {size_kb}K"
            combo_log_history.addItem(label, h["log_file"])
        # 默认选 latest
        combo_log_history.setCurrentIndex(0)

        # 2.5) multi-sub-task: 填 sub_list
        sub_list.clear()
        subs = get_sub_tasks(task_name)
        if subs:
            sub_box.setVisible(True)
            for s in subs:
                sub_sym = "✓" if s.get("sub_rc") == 0 else "✗"
                dt_str = s.get("sub_dt_sec", 0)
                cmd_short = s.get("sub_cmd", "")[:70]
                # 添加 "ALL" 项, 用于看主 log (all sub-task 串起来)
                if s.get("sub_idx") == 0:
                    item_all = QListWidgetItem(f"  📂 全部 (latest.log, 串行拼接)")
                    item_all.setForeground(QColor("#88ccff"))
                    item_all.setData(Qt.ItemDataRole.UserRole, "ALL")
                    sub_list.addItem(item_all)
                item = QListWidgetItem(
                    f"  {sub_sym} sub[{s['sub_idx']+1}/{s['sub_n']}] "
                    f"rc={s.get('sub_rc', '?')} {dt_str:.1f}s  {cmd_short}"
                )
                color = "#88ff88" if s.get("sub_rc") == 0 else "#ff8888"
                item.setForeground(QColor(color))
                item.setData(Qt.ItemDataRole.UserRole, str(s.get("sub_log", "")))
                sub_list.addItem(item)
        else:
            sub_box.setVisible(False)

        # 3) 读 log 内容
        load_log_to_view(task_name, "latest")

        config["selected_task"] = task_name
        status.showMessage(f"已选中: {task_name} ({type_label}, {status_str})", 3000)

    def load_log_to_view(task_name: str, which: str):
        """把 log 内容写进 log_view. which=latest 或 file path"""
        if which == "latest":
            log_path = TASK_LOG_ROOT / task_name / "latest.log"
        else:
            log_path = Path(which)
        if not log_path.is_file():
            log_view.setPlainText(f"⚠ log 文件不存在: {log_path}\n"
                                  f"(先跑一次 task: ac --task {task_name})")
            lbl_log_stats.setText(f"📊 log 摘要: ⚠ 文件不存在 {log_path}")
            return
        try:
            # 限制最大 500K 字符, 防止大 log 卡 UI
            text = log_path.read_text(encoding="utf-8", errors="replace")
            n_lines = text.count("\n")
            n_bytes = log_path.stat().st_size
            # 关键字统计 (布局优化)
            n_fatal = sum(1 for ln in text.splitlines() if "FATAL" in ln)
            n_err = sum(1 for ln in text.splitlines() if "error" in ln.lower() or "FAIL" in ln)
            n_warn = sum(1 for ln in text.splitlines() if "warn" in ln.lower())
            n_ok = sum(1 for ln in text.splitlines() if "[OK]" in ln or "success" in ln.lower())
            stats = (f"📊 log 摘要: {n_lines:,} 行 / {n_bytes:,} 字节  |  "
                     f"🔴 FATAL: {n_fatal}  ❌ Error: {n_err}  "
                     f"⚠ Warning: {n_warn}  ✓ OK: {n_ok}")
            lbl_log_stats.setText(stats)
            if len(text) > 500_000:
                text = text[:500_000] + f"\n\n... (truncated, full log: {log_path})\n"
            log_view.setPlainText(text)
            # 滚到底
            cursor = log_view.textCursor()
            cursor.movePosition(QTextCursor.MoveOperation.End)
            log_view.setTextCursor(cursor)
            status.showMessage(f"✓ 加载 log: {log_path} ({len(text):,} 字符)", 3000)
        except OSError as e:
            log_view.setPlainText(f"⚠ 读 log 失败: {e}")
            lbl_log_stats.setText(f"📊 log 摘要: ⚠ 读失败 {e}")

    def on_task_selected():
        item = list_tasks.currentItem()
        if item is None:
            return
        task_name = item.data(Qt.ItemDataRole.UserRole)
        show_task_log(task_name)

    def on_log_history_changed(_idx):
        if config["selected_task"] is None:
            return
        data = combo_log_history.currentData()
        if data == "latest":
            load_log_to_view(config["selected_task"], "latest")
        else:
            load_log_to_view(config["selected_task"], data)

    def on_sub_task_clicked():
        """点 sub-task item, 加载对应的 sub-task log"""
        if config["selected_task"] is None:
            return
        item = sub_list.currentItem()
        if item is None:
            return
        data = item.data(Qt.ItemDataRole.UserRole)
        if data == "ALL":
            # 全部 (latest.log, 串行拼接)
            load_log_to_view(config["selected_task"], "latest")
            status.showMessage(f"📂 全部 sub-task log (串行拼接, latest.log)", 3000)
        elif data:
            # 单个 sub-task log
            log_path = Path(data)
            if log_path.is_file():
                # 调 load_log_to_view 走 file path 分支
                load_log_to_view(config["selected_task"], str(log_path))
                status.showMessage(f"✓ 加载 sub-task log: {log_path.name}", 3000)
            else:
                status.showMessage(f"⚠ sub-task log 不存在: {log_path}", 3000)

    def on_run_task():
        if config["selected_task"] is None:
            QMessageBox.warning(window, "无选中", "先在左边选一个 task")
            return
        task_name = config["selected_task"]
        # 准备 log 路径
        log_path = str(TASK_LOG_ROOT / task_name / f"manual_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log")
        status.showMessage(f"▶ 启动: ac --task {task_name} (log: {log_path})", 5000)
        # 后台跑 (线程, 不阻塞 UI)
        def _run_in_thread():
            rc = run_task(task_name, log_path)
            # 刷新显示
            QTimer.singleShot(0, lambda: on_run_done(task_name, rc, log_path))
        threading.Thread(target=_run_in_thread, daemon=True).start()

    def on_run_done(task_name: str, rc: int, log_path: str):
        status.showMessage(f"{'✓' if rc == 0 else '✗'} {task_name} 完成 rc={rc} (log: {log_path})", 5000)
        # 刷新 list (更新状态 icon)
        reload_task_list()
        # 重新显示选中 task (含新 log)
        if config["selected_task"] == task_name:
            show_task_log(task_name)
        # 弹通知
        try:
            import subprocess as sp
            sym = "✓" if rc == 0 else "✗"
            sp.Popen(["ac", "notify", "-i",
                      "dialog-info" if rc == 0 else "dialog-error",
                      f"{sym} {task_name}", f"rc={rc} | log: {log_path}"])
        except Exception:
            pass

    def on_copy_log():
        if config["selected_task"] is None:
            return
        text = log_view.toPlainText()
        if not text:
            status.showMessage("⚠ log 区为空", 3000)
            return
        QApplication.clipboard().setText(text)
        status.showMessage(f"✓ 已复制 {len(text.splitlines())} 行 log 到剪贴板", 3000)

    def on_open_log_file():
        if config["selected_task"] is None:
            return
        log_path = TASK_LOG_ROOT / config["selected_task"] / "latest.log"
        if not log_path.is_file():
            status.showMessage(f"⚠ log 不存在: {log_path}", 3000)
            return
        try:
            subprocess.Popen(["xdg-open", str(log_path)])
            status.showMessage(f"✓ 打开 {log_path}", 3000)
        except FileNotFoundError:
            status.showMessage("⚠ xdg-open 不存在", 3000)

    def on_open_log_dir():
        """打开 /tmp/ac_task_logs 目录 (或当前选中 task 的子目录)"""
        if config["selected_task"] is not None:
            d = TASK_LOG_ROOT / config["selected_task"]
            if d.is_dir():
                target = d
            else:
                target = TASK_LOG_ROOT
        else:
            target = TASK_LOG_ROOT
        try:
            subprocess.Popen(["xdg-open", str(target)])
            status.showMessage(f"✓ 打开 {target}", 3000)
        except FileNotFoundError:
            status.showMessage("⚠ xdg-open 不存在", 3000)

    def on_show_cmd():
        if config["selected_task"] is None:
            return
        t_meta = next((t for t in config["data"].get("tasks", [])
                       if t.get("name") == config["selected_task"]), None)
        if t_meta is None:
            return
        cmd = t_meta.get("cmd", "")
        QMessageBox.information(window, f"命令: {config['selected_task']}",
                                 f"$ {cmd}\n\n(等价的 ac --task 调用: ac --task {config['selected_task']})")

    def on_kill_cm():
        try:
            n = subprocess.run(["pkill", "-KILL", "-f", "cloud_main"],
                               capture_output=True).returncode
            status.showMessage(f"{'✓' if n == 0 else '⚠'} 杀 cloud_main (rc={n})", 3000)
        except FileNotFoundError:
            status.showMessage("⚠ pkill 不存在", 3000)

    def on_about():
        QMessageBox.about(window, "关于", (
            "ac 任务浏览器 v1.0 (2026-09-10)\n\n"
            "数据源:\n"
            f"  • tasks: {AI_BUILD_JSON}\n"
            f"  • logs:  {TASK_LOG_ROOT}/\n\n"
            "快捷键:\n"
            "  • F5: 刷新\n"
            "  • Ctrl+R: 运行选中 task\n"
            "  • Ctrl+L: 复制 log\n"
            "  • Ctrl+O: 打开 log 文件夹\n"
            "  • Ctrl+K: 杀 cloud_main\n"
            "  • Ctrl+Q: 退出\n\n"
            "颜色编码: 任务按 type 着色 (build=蓝, rebuild=橙, poc=紫, test=绿, run=黄, ...)\n"
            "状态: ✓=成功 ✗=失败 ⟳=跑中 ·=没跑过"
        ))

    # 绑定事件 (2026-09-10 去掉搜索/过滤相关绑定, 任务全部显示)
    list_tasks.currentItemChanged.connect(lambda *_: on_task_selected())
    combo_log_history.currentIndexChanged.connect(on_log_history_changed)
    sub_list.currentItemChanged.connect(lambda *_: on_sub_task_clicked())
    btn_run.clicked.connect(on_run_task)
    btn_refresh_log.clicked.connect(lambda: load_log_to_view(
        config["selected_task"] or "?", "latest"))
    btn_copy_log.clicked.connect(on_copy_log)
    btn_open_log_file.clicked.connect(on_open_log_file)
    btn_open_log_dir2.clicked.connect(on_open_log_dir)
    btn_show_cmd.clicked.connect(on_show_cmd)
    act_refresh.triggered.connect(lambda: (reload_task_list(),))
    act_open_log_dir.triggered.connect(on_open_log_dir)
    act_run.triggered.connect(on_run_task)
    act_kill.triggered.connect(on_kill_cm)
    act_quit.triggered.connect(window.close)
    act_about.triggered.connect(on_about)

    # 初始加载
    window.show()
    QTimer.singleShot(100, reload_task_list)

    return window


# ---------- No-GUI fallback ----------
def print_terminal_table():
    """无 PyQt6 时打印终端表格"""
    config = load_ai_build()
    tasks = config.get("tasks", [])
    print(f"\n{'='*100}")
    print(f" ac 任务浏览器 v1 (终端模式, 无 PyQt6)  |  tasks: {len(tasks)}")
    print(f"{'='*100}")
    print(f"{'状态':4s} {'类型':10s} {'cat':4s} {'task name':30s} {'最近 log':30s} {'rc'}")
    print(f"{'-'*100}")
    for t in tasks:
        meta = get_task_meta(t)
        status_str = get_task_status(meta["name"])
        color, icon, type_label = TYPE_COLORS.get(meta["type"], TYPE_COLORS["other"])
        cat_icon, _ = CATEGORY_BADGE.get(meta["category"], CATEGORY_BADGE["current"])
        latest = get_latest_task_log(meta["name"])
        rc_str = str(latest.get("rc", "-")) if latest else "-"
        ts_str = latest.get("ts", "-") if latest else "(从未跑)"
        status_sym = {"ok": "✓", "fail": "✗", "running": "⟳", "never": "·"}[status_str]
        # multi-sub-task 标识
        sub_tag = ""
        if is_multi_subtask(meta["name"]):
            sc = get_sub_status_counts(meta["name"])
            sub_tag = f" [{sc['ok']}/{sc['total']}]"
        print(f"  {status_sym:2s}  {type_label:10s} {cat_icon:2s}  "
              f"{meta['name']:30s} {ts_str:30s} {rc_str}{sub_tag}")
    print(f"{'='*100}")
    print(f"\n详细 log: /tmp/ac_task_logs/<task_name>/latest.log")
    print(f"元数据:   /tmp/ac_task_logs/<task_name>/latest.meta.json")
    # multi-sub-task 详情
    print()
    multi_tasks = [t for t in tasks if is_multi_subtask(t.get("name", ""))]
    if multi_tasks:
        print(f"📋 multi-sub-task 详情 ({len(multi_tasks)} 个):")
        for t in multi_tasks:
            tname = t.get("name", "?")
            subs = get_sub_tasks(tname)
            sc = get_sub_status_counts(tname)
            print(f"  {tname}: 总 {sc['total']} 个 sub-task, {sc['ok']} 成功 / {sc['fail']} 失败")
            for s in subs:
                sym = "✓" if s.get("sub_rc") == 0 else "✗"
                print(f"    {sym} sub[{s['sub_idx']+1}/{s['sub_n']}] "
                      f"rc={s.get('sub_rc', '?')} {s.get('sub_dt_sec', 0):.1f}s  "
                      f"{s.get('sub_cmd', '')[:60]}")
            print(f"    sub-task log: /tmp/ac_task_logs/{tname}/<ts>_*.log")


def main():
    parser = argparse.ArgumentParser(
        description="ac 任务浏览器 (PyQt6 GUI 0-click, 左侧 task 列表 + 右侧 log)")
    parser.add_argument("--no-gui", action="store_true",
                        help="无 GUI 模式, 终端打印 task 列表")
    args = parser.parse_args()

    if args.no_gui or not HAS_PYQT6:
        if not HAS_PYQT6 and not args.no_gui:
            print("[!] PyQt6 not installed, fallback to no-gui", file=sys.stderr)
        print_terminal_table()
        return

    # GUI 模式: 必须先有 QApplication 实例
    app = QApplication(sys.argv)
    window = build_gui()
    # 持有引用, 防止 GC 后 wrapped C/C++ object 被释放
    window._keep_alive = (app, window)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
