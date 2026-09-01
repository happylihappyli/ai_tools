#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ac_themes — ai_tools GUI 主题管理
================================================================
提供:
- DARK / LIGHT 两套配色 (深色保护眼睛, 浅色默认)
- 整段 QSS 样式表 (一个函数 get_qss(theme))
- 主题切换 + 持久化 (写到 ~/.config/ai_tools/theme.json)
- 启动 GUI 时自动读上次选择

使用:
    from ac_themes import apply_theme, THEMES, get_qss, get_current_theme

    # 应用主题到整个 QApplication
    app = QApplication.instance() or QApplication(sys.argv)
    apply_theme(app, "dark")        # 或 "light"

    # 切换并保存
    save_theme("dark")
    apply_theme(app, "dark")

配置: ~/.config/ai_tools/theme.json
    {"theme": "dark", "font_size": 10}
"""
import os
import sys
import json
from pathlib import Path


# 配色: 跟 .trae/rules 颜色编码一致 (ok/err/warn/info)
THEMES = {
    "light": {
        "name": "浅色 (默认)",
        "bg":           "#ffffff",
        "bg_alt":       "#f5f5f5",     # 工具栏/dock 背景
        "bg_frame":     "#fafafa",     # 卡片/Frame 背景
        "fg":           "#212121",     # 主文字
        "fg_label":     "#424242",     # 标签
        "fg_subtle":    "#757575",     # 次要文字
        "border":       "#e0e0e0",
        "accent":       "#1565c0",     # 蓝色高亮
        "accent_hover": "#1976d2",
        "ok":           "#2e7d32",
        "err":          "#c62828",
        "warn":         "#ef6c00",
        "info":         "#1565c0",
        "log_bg":       "#fafafa",     # 日志面板背景
        "log_fg":       "#212121",
        "selection":    "#bbdefb",
    },
    "dark": {
        "name": "暗色 (护眼)",
        "bg":           "#1e1e1e",     # VS Code 风格主背景
        "bg_alt":       "#252526",     # 工具栏
        "bg_frame":     "#2d2d30",     # 卡片
        "fg":           "#d4d4d4",     # 主文字 (浅灰, 不刺眼)
        "fg_label":     "#cccccc",
        "fg_subtle":    "#858585",
        "border":       "#3c3c3c",
        "accent":       "#569cd6",     # 蓝色高亮
        "accent_hover": "#4ec9b0",
        "ok":           "#6a9955",
        "err":          "#f48771",
        "warn":         "#dcdcaa",
        "info":         "#569cd6",
        "log_bg":       "#1e1e1e",     # 日志面板 — 跟主背景一致
        "log_fg":       "#d4d4d4",
        "selection":    "#264f78",     # VS Code 选区色
    },
}

DEFAULT_THEME = "dark"  # 用户最新偏好


def _config_path() -> Path:
    """配置文件路径: 优先 ~/.config/ai_tools/theme.json, 降级到 ~/ai_tools.cfg"""
    candidates = [
        Path.home() / ".config" / "ai_tools" / "theme.json",
        Path.home() / "ai_tools.cfg",
        Path("/tmp/ai_tools_theme.json"),
    ]
    for p in candidates:
        try:
            p.parent.mkdir(parents=True, exist_ok=True)
            # 测试可写
            if p.exists() or _try_write_test(p):
                return p
        except Exception:
            continue
    # 实在不行用 tmp
    return Path("/tmp/ai_tools_theme.json")


def _try_write_test(p: Path) -> bool:
    """测试 p 父目录是否可写"""
    try:
        test_file = p.parent / f".{p.name}.test"
        test_file.write_text("test")
        test_file.unlink()
        return True
    except Exception:
        return False


def get_current_theme() -> str:
    """读上次保存的主题, 失败返回默认 (dark)"""
    try:
        cfg_file = _config_path()
        if cfg_file.exists():
            with open(cfg_file, "r", encoding="utf-8") as f:
                data = json.load(f)
            t = data.get("theme", DEFAULT_THEME)
            if t in THEMES:
                return t
    except Exception:
        pass
    return DEFAULT_THEME


def save_theme(theme_name: str) -> bool:
    """保存主题到配置文件"""
    if theme_name not in THEMES:
        return False
    try:
        cfg_file = _config_path()
        data = {}
        if cfg_file.exists():
            try:
                with open(cfg_file, "r", encoding="utf-8") as f:
                    data = json.load(f)
            except Exception:
                data = {}
        data["theme"] = theme_name
        with open(cfg_file, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        return True
    except Exception as e:
        sys.stderr.write(f"⚠ 保存主题失败: {e}\n")
        return False


def get_qss(theme_name: str) -> str:
    """生成完整 QSS 样式表 (主背景/工具栏/菜单/按钮/输入框/Dock 等)"""
    if theme_name not in THEMES:
        theme_name = DEFAULT_THEME
    c = THEMES[theme_name]
    return f"""
/* ===== 全局 ===== */
* {{
    font-family: "Noto Sans CJK SC", "WenQuanYi Micro Hei", "PingFang SC", "Microsoft YaHei", sans-serif;
}}

QMainWindow, QDialog {{
    background: {c['bg']};
    color: {c['fg']};
}}

QWidget {{
    background: {c['bg']};
    color: {c['fg']};
}}

/* ===== 菜单栏 ===== */
QMenuBar {{
    background: {c['bg_alt']};
    color: {c['fg']};
    border-bottom: 1px solid {c['border']};
    padding: 2px;
}}
QMenuBar::item {{
    background: transparent;
    padding: 6px 12px;
    border-radius: 4px;
}}
QMenuBar::item:selected {{
    background: {c['accent']};
    color: white;
}}
QMenu {{
    background: {c['bg_alt']};
    color: {c['fg']};
    border: 1px solid {c['border']};
    padding: 4px;
}}
QMenu::item {{
    padding: 6px 24px;
    border-radius: 4px;
}}
QMenu::item:selected {{
    background: {c['accent']};
    color: white;
}}
QMenu::separator {{
    height: 1px;
    background: {c['border']};
    margin: 4px 8px;
}}

/* ===== 工具栏 ===== */
QToolBar {{
    background: {c['bg_alt']};
    border: 1px solid {c['border']};
    padding: 4px;
    spacing: 4px;
}}
QToolBar::separator {{
    background: {c['border']};
    width: 1px;
    margin: 4px 4px;
}}
QToolButton {{
    background: transparent;
    color: {c['fg']};
    border: 1px solid transparent;
    padding: 5px 10px;
    border-radius: 4px;
}}
QToolButton:hover {{
    background: {c['accent']};
    color: white;
}}
QToolButton:pressed {{
    background: {c['accent_hover']};
}}

/* ===== 状态栏 ===== */
QStatusBar {{
    background: {c['bg_alt']};
    color: {c['fg_label']};
    border-top: 1px solid {c['border']};
}}
QStatusBar QLabel {{
    color: {c['fg_label']};
    padding: 2px 8px;
}}

/* ===== 按钮 ===== */
QPushButton {{
    background: {c['bg_alt']};
    color: {c['fg']};
    border: 1px solid {c['border']};
    padding: 6px 14px;
    border-radius: 4px;
    min-height: 18px;
}}
QPushButton:hover {{
    background: {c['accent']};
    color: white;
    border-color: {c['accent']};
}}
QPushButton:pressed {{
    background: {c['accent_hover']};
}}
QPushButton:disabled {{
    background: {c['border']};
    color: {c['fg_subtle']};
    border-color: {c['border']};
}}

/* ===== 输入框 ===== */
QLineEdit, QTextEdit, QPlainTextEdit {{
    background: {c['bg']};
    color: {c['fg']};
    border: 1px solid {c['border']};
    padding: 4px 6px;
    border-radius: 3px;
    selection-background-color: {c['selection']};
}}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus {{
    border: 1px solid {c['accent']};
}}

/* ===== 标签 ===== */
QLabel {{
    color: {c['fg']};
    background: transparent;
}}

/* ===== 分组框 ===== */
QGroupBox {{
    background: {c['bg_frame']};
    color: {c['fg_label']};
    border: 1px solid {c['border']};
    border-radius: 5px;
    margin-top: 12px;
    padding: 8px;
    font-weight: bold;
}}
QGroupBox::title {{
    subcontrol-origin: margin;
    subcontrol-position: top left;
    padding: 0 6px;
    color: {c['accent']};
}}

/* ===== 框架 (卡片) ===== */
QFrame {{
    background: {c['bg_frame']};
    border: 1px solid {c['border']};
    border-radius: 5px;
}}

/* ===== 树形列表 ===== */
QTreeWidget, QTreeView, QListWidget {{
    background: {c['bg']};
    color: {c['fg']};
    alternate-background-color: {c['bg_alt']};
    border: 1px solid {c['border']};
    selection-background-color: {c['selection']};
    selection-color: {c['fg']};
}}
QHeaderView::section {{
    background: {c['bg_alt']};
    color: {c['fg_label']};
    padding: 4px 8px;
    border: 1px solid {c['border']};
    font-weight: bold;
}}

/* ===== 进度条 ===== */
QProgressBar {{
    background: {c['bg_alt']};
    color: {c['fg']};
    border: 1px solid {c['border']};
    border-radius: 4px;
    text-align: center;
    min-height: 18px;
}}
QProgressBar::chunk {{
    background: {c['accent']};
    border-radius: 3px;
}}

/* ===== Dock ===== */
QDockWidget {{
    color: {c['fg']};
    titlebar-close-icon: none;
}}
QDockWidget::title {{
    background: {c['bg_alt']};
    color: {c['fg_label']};
    padding: 4px 8px;
    border: 1px solid {c['border']};
    font-weight: bold;
}}

/* ===== 滚动条 ===== */
QScrollBar:vertical {{
    background: {c['bg_alt']};
    width: 12px;
    border: none;
}}
QScrollBar::handle:vertical {{
    background: {c['border']};
    border-radius: 6px;
    min-height: 20px;
    margin: 2px;
}}
QScrollBar::handle:vertical:hover {{
    background: {c['fg_subtle']};
}}
QScrollBar:horizontal {{
    background: {c['bg_alt']};
    height: 12px;
    border: none;
}}
QScrollBar::handle:horizontal {{
    background: {c['border']};
    border-radius: 6px;
    min-width: 20px;
    margin: 2px;
}}

/* ===== 复选框 / 单选 ===== */
QCheckBox, QRadioButton {{
    color: {c['fg']};
    spacing: 6px;
}}
QCheckBox::indicator, QRadioButton::indicator {{
    width: 16px;
    height: 16px;
    border: 1px solid {c['border']};
    background: {c['bg']};
    border-radius: 3px;
}}
QRadioButton::indicator {{
    border-radius: 8px;
}}
QCheckBox::indicator:checked {{
    background: {c['accent']};
    border-color: {c['accent']};
}}

/* ===== 提示框 ===== */
QToolTip {{
    background: {c['bg_frame']};
    color: {c['fg']};
    border: 1px solid {c['accent']};
    padding: 4px 6px;
    border-radius: 3px;
}}

/* ===== 消息对话框 ===== */
QMessageBox {{
    background: {c['bg']};
    color: {c['fg']};
}}
QMessageBox QLabel {{
    color: {c['fg']};
}}
"""


def apply_theme(app, theme_name: str = None):
    """应用主题到 QApplication, 立即生效

    Args:
        app: QApplication 实例
        theme_name: "dark" / "light", None = 用 get_current_theme() 自动选
    """
    if theme_name is None:
        theme_name = get_current_theme()
    if theme_name not in THEMES:
        theme_name = DEFAULT_THEME
    app.setStyleSheet(get_qss(theme_name))
    # 写到配置
    save_theme(theme_name)
    return theme_name


def list_themes() -> list:
    """列出所有可用主题 [(name, display_name), ...]"""
    return [(k, v["name"]) for k, v in THEMES.items()]


# 自检
if __name__ == "__main__":
    print(f"默认主题: {get_current_theme()}")
    print(f"可用主题: {list_themes()}")
    print(f"配置路径: {_config_path()}")
    print(f"\nDARK 主题前 200 字符:")
    print(get_qss("dark")[:200])
