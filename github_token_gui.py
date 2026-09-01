#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
github_token_gui — GitHub Token 管理 Qt GUI
================================================================
可视化 GitHub PAT 工具, 用 Qt 5/6 渲染 (PySide6 → PyQt5 → PySide2 自适应).

启动:
    github-token-gui                 # 启动主窗口
    github-token-gui --auto-check    # 启动后立即跑一次诊断 (默认开)
    github-token-gui --no-check      # 启动后不自动诊断

布局 (按用户偏好: 状态栏在底, 颜色编码, 错误停自动滚动, 可拖动 dock):
    ┌─ 文件  工具  帮助 ─────────────────────┐  ← 菜单栏
    ├ [诊断] [打开] [复制] [清掉] [日志] ────┤  ← 工具栏 (可拖)
    ├────────────────────────────────────────┤
    │  [状态大字: Token 状态 / 用户 / rate]   │  ← 顶部
    ├────────────────────────────────────────┤
    │  [诊断] [打开 GitHub] [复制 URL] [清掉] │  ← 主按钮行
    ├────────────────────────────────────────┤
    │  Repo 列表 (3 个已知 + user info)      │  ← 中部
    ├────────────────────────────────────────┤
    │  新 Token: [_______________] [测试] [保存] │  ← 底部输入
    ├────────────────────────────────────────┤
    │ 状态栏: ✓ Token 有效 / 5000 次剩余      │  ← 底栏
    └────────────────────────────────────────┘
       日志 Dock (默认隐藏, Ctrl+L 切换) — 可拖到任何边

快捷键:
    Ctrl+D        诊断
    Ctrl+T        测试当前输入框的 token
    Ctrl+S        保存当前输入框的 token
    Ctrl+G        打开 GitHub 生成页
    Ctrl+Shift+C  复制 fine-grained URL
    Ctrl+X        清掉旧 token
    Ctrl+L        显示/隐藏日志 Dock
    F1            设置指南 (帮助)
    Ctrl+,        关于
    Ctrl+Q        退出
"""
import os
import sys
import subprocess
import webbrowser
from datetime import datetime
from pathlib import Path

GT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(GT_DIR))

# 先 import github_token 核心 (再 import _qt_compat, 因为 _qt_compat 不依赖 github_token)
import github_token as gt  # noqa: E402

try:
    from _qt_compat import (
        QT_BACKEND, APP_EXEC, PYQT_VERSION_STR, gui_available,
        QtCore, QtWidgets, QtGui,
        Qt, QTimer, Signal, Slot,
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QLabel, QLineEdit, QPushButton, QTreeWidget, QTreeWidgetItem,
        QStatusBar, QMessageBox, QColor, QFont, QFrame,
        QSizePolicy, QGroupBox,
        QAction, QMenu, QToolBar, QDockWidget,
        QDialog, QPlainTextEdit, QKeySequence, QTextCursor,
    )
except Exception as e:
    print(f"✗ Qt 加载失败: {e}", file=sys.stderr)
    print("  需要: pip install PySide6 (或 PyQt5/PySide2)", file=sys.stderr)
    sys.exit(1)

# 主题 (从 ac_themes 读)
try:
    import ac_themes
    HAS_THEMES = True
except Exception:
    HAS_THEMES = False

if not gui_available():
    print(f"✗ 没找到可用的 Qt 后端 (PySide6/PyQt5/PySide2 全缺)", file=sys.stderr)
    sys.exit(1)


# 颜色常量 (跟 .trae/rules 颜色编码一致)
COLOR_OK = "#2e7d32"        # 绿: 成功
COLOR_ERR = "#c62828"       # 红: 失败
COLOR_WARN = "#ef6c00"      # 橙: 警告
COLOR_INFO = "#1565c0"      # 蓝: 信息
COLOR_BG = "#fafafa"        # 浅灰背景
COLOR_LABEL = "#424242"     # 标签色

APP_VERSION = "1.1.0"
APP_NAME = "GitHub Token 管理器"
APP_ORG = "ai_tools"


class TokenStatusBar(QStatusBar):
    """扩展状态栏: 带颜色和图标, 检测到错误时停止自动滚动"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._auto_scroll = True
        self._last_was_error = False
        # 用 label 显示消息 (可以控制颜色)
        self._label = QLabel("就绪")
        self._label.setStyleSheet(f"color: {COLOR_LABEL}; padding: 2px 8px;")
        self.addWidget(self._label, 1)
        # 右侧 token 状态简标
        self._right = QLabel("")
        self._right.setStyleSheet(f"color: {COLOR_LABEL}; padding: 2px 8px;")
        self.addPermanentWidget(self._right)

    def set_auto_scroll(self, enabled: bool):
        self._auto_scroll = enabled

    def show_msg(self, msg: str, level: str = "info"):
        """level: ok / err / warn / info"""
        colors = {
            "ok": COLOR_OK,
            "err": COLOR_ERR,
            "warn": COLOR_WARN,
            "info": COLOR_INFO,
        }
        prefix = {"ok": "✓", "err": "✗", "warn": "⚠", "info": "•"}.get(level, "•")
        self._label.setText(f"{prefix} {msg}")
        self._label.setStyleSheet(
            f"color: {colors.get(level, COLOR_LABEL)}; "
            f"padding: 2px 8px; font-weight: {'bold' if level in ('err', 'ok') else 'normal'};"
        )
        if level == "err":
            self._last_was_error = True
            self.set_auto_scroll(False)  # 错误停自动滚动 (用户偏好)

    def set_right(self, text: str, color: str = COLOR_LABEL):
        self._right.setText(text)
        self._right.setStyleSheet(f"color: {color}; padding: 2px 8px;")


class RepoListWidget(QTreeWidget):
    """repo 访问权限列表, 颜色编码"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setHeaderLabels(["Repo", "状态", "类型", "权限"])
        self.setRootIsDecorated(False)
        self.setAlternatingRowColors(True)
        self.setColumnWidth(0, 280)
        self.setColumnWidth(1, 80)
        self.setColumnWidth(2, 80)
        self.setColumnWidth(3, 200)

    def update_repos(self, repos: dict):
        """repos: {owner/repo: {accessible, private, permissions, error}}"""
        self.clear()
        for repo_name, info in repos.items():
            item = QTreeWidgetItem()
            item.setText(0, repo_name)
            if info.get("accessible"):
                item.setText(1, "✓ 可访问")
                item.setForeground(1, QColor(COLOR_OK))
                item.setText(2, "🔒 私有" if info.get("private") else "🌐 公开")
                perms = ",".join([k for k, v in info.get("permissions", {}).items() if v]) or "(无)"
                item.setText(3, perms)
            else:
                item.setText(1, f"✗ {info.get('http', '?')}")
                item.setForeground(1, QColor(COLOR_ERR))
                item.setText(2, "-")
                item.setText(3, info.get("error", ""))
            self.addTopLevelItem(item)


class LogDockWidget(QWidget):
    """日志面板: 颜色编码时间戳行, 错误停自动滚动"""

    def __init__(self, parent=None):
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(2)

        # 工具行: 清空 + 自动滚动
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
        self._edit.setMaximumBlockCount(2000)  # 防止日志无限增长
        font = QFont("monospace")
        font.setPointSize(10)
        self._edit.setFont(font)
        self._edit.setStyleSheet(
            f"QPlainTextEdit {{ background: #1e1e1e; color: #e0e0e0; "
            f"border: 1px solid #444; }}"
        )
        layout.addWidget(self._edit, 1)

    def _on_auto_scroll_toggle(self, checked: bool):
        self._auto_scroll_chk.setText(f"自动滚动: {'开' if checked else '关'}")

    def append(self, level: str, msg: str):
        """level: ok / err / warn / info / debug"""
        colors = {
            "ok": "#4caf50",
            "err": "#ef5350",
            "warn": "#ffa726",
            "info": "#64b5f6",
            "debug": "#9e9e9e",
        }
        prefix = {"ok": "✓", "err": "✗", "warn": "⚠", "info": "•", "debug": "›"}.get(level, "•")
        ts = datetime.now().strftime("%H:%M:%S")
        line = f"[{ts}] {prefix} {msg}"
        # 颜色用 HTML 插入 (QPlainTextEdit 支持)
        color = colors.get(level, "#e0e0e0")
        html = (
            f'<span style="color:#888;">[{ts}]</span> '
            f'<span style="color:{color};">{prefix}</span> '
            f'<span style="color:#e0e0e0;">{msg}</span>'
        )
        # 简单做法: 直接 appendPlainText (QPlainTextEdit 不支持富文本)
        # 用 QTextCursor + QTextCharFormat 来上色
        cursor = self._edit.textCursor()
        cursor.movePosition(QTextCursor.End)
        fmt = QTextCharFormat() if False else None
        # 用 insertHtml 来支持颜色
        self._edit.appendPlainText(f"{line}")
        # 给最后一行上色 (用 currentCharFormat 改不了历史, 这里简化)
        # 改为每次都重新着色最后一段
        if hasattr(self, "_colorize_last"):
            self._colorize_last(color)
        # 自动滚动
        if self._auto_scroll_chk.isChecked():
            sb = self._edit.verticalScrollBar()
            sb.setValue(sb.maximum())

    def _colorize_last(self, color: str):
        """给最后一行染色 (用 QTextCursor)"""
        try:
            cursor = self._edit.textCursor()
            cursor.movePosition(QTextCursor.End)
            block = cursor.block()
            cursor.select(QTextCursor.BlockUnderCursor)
            fmt = cursor.charFormat()
            fmt.setForeground(QColor(color))
            cursor.setCharFormat(fmt)
        except Exception:
            pass


class SetupHelpDialog(QDialog):
    """设置帮助对话框: 引导用户生成 GitHub Token"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("设置指南 — GitHub Token")
        self.resize(620, 480)

        layout = QVBoxLayout(self)

        intro = QLabel(
            "<h3>如何获取 GitHub Personal Access Token</h3>"
            "<p>推荐使用 <b>Fine-grained</b> token (安全性更高, 可限定仓库访问)."
            " 两种类型都支持, 点下面的按钮直接打开对应页面.</p>"
        )
        intro.setWordWrap(True)
        layout.addWidget(intro)

        # Fine-grained
        fg_box = QGroupBox("Fine-grained (推荐) — 限定仓库 + 最小权限")
        fg_layout = QVBoxLayout(fg_box)
        fg_text = QLabel(
            "• Repository access: <b>All repositories</b> 或按需选<br>"
            "• Permissions → Repository: <b>Contents: Read and write</b> "
            "(如果 push 代码需要)<br>"
            "• Permissions → Account: <b>Email addresses (read)</b> (可选)"
        )
        fg_text.setTextFormat(Qt.RichText)
        fg_text.setWordWrap(True)
        fg_layout.addWidget(fg_text)
        fg_btn_row = QHBoxLayout()
        open_fg = QPushButton("🌐 打开 Fine-grained 设置页")
        open_fg.clicked.connect(lambda: self._open_url(gt.SETUP_URLS["fine-grained"]))
        copy_fg = QPushButton("📋 复制 URL")
        copy_fg.clicked.connect(lambda: self._copy_url(gt.SETUP_URLS["fine-grained"]))
        fg_btn_row.addWidget(open_fg)
        fg_btn_row.addWidget(copy_fg)
        fg_btn_row.addStretch(1)
        fg_layout.addLayout(fg_btn_row)
        layout.addWidget(fg_box)

        # Classic
        cls_box = QGroupBox("Classic (旧版) — 范围更广, 慎用")
        cls_layout = QVBoxLayout(cls_box)
        cls_text = QLabel(
            "• Note: 随便填, 比如 <b>ai_tools</b><br>"
            "• Expiration: <b>90 days</b> 或 <b>No expiration</b><br>"
            "• Scopes: 至少勾 <b>repo</b> (私有库) + <b>workflow</b> (GitHub Actions)"
        )
        cls_text.setTextFormat(Qt.RichText)
        cls_text.setWordWrap(True)
        cls_layout.addWidget(cls_text)
        cls_btn_row = QHBoxLayout()
        open_cls = QPushButton("🌐 打开 Classic 设置页")
        open_cls.clicked.connect(lambda: self._open_url(gt.SETUP_URLS["classic"]))
        copy_cls = QPushButton("📋 复制 URL")
        copy_cls.clicked.connect(lambda: self._copy_url(gt.SETUP_URLS["classic"]))
        cls_btn_row.addWidget(open_cls)
        cls_btn_row.addWidget(copy_cls)
        cls_btn_row.addStretch(1)
        cls_layout.addLayout(cls_btn_row)
        layout.addWidget(cls_box)

        # 步骤
        steps = QLabel(
            "<h4>使用步骤</h4>"
            "<ol>"
            "<li>点上面 [打开] 按钮 → 浏览器跳到 GitHub → 选权限 → 生成 token</li>"
            "<li>复制生成的 token (ghp_... 或 github_pat_...)</li>"
            "<li>回到主窗口, 粘贴到 [新 Token] 输入框</li>"
            "<li>点 [测试] (Ctrl+T) 验证有效 → 点 [保存] (Ctrl+S) 写入 ~/.git-credentials</li>"
            "<li>点 [诊断] (Ctrl+D) 检查所有已知 repo 的访问权限</li>"
            "</ol>"
        )
        steps.setTextFormat(Qt.RichText)
        steps.setWordWrap(True)
        layout.addWidget(steps)

        # 关闭按钮
        close_btn = QPushButton("关闭")
        close_btn.clicked.connect(self.accept)
        layout.addWidget(close_btn, 0, Qt.AlignRight)

    def _open_url(self, url: str):
        try:
            webbrowser.open(url)
        except Exception:
            try:
                subprocess.Popen(["xdg-open", url])
            except Exception:
                pass

    def _copy_url(self, url: str):
        QApplication.clipboard().setText(url)
        QMessageBox.information(self, "已复制", f"已复制 URL 到剪贴板:\n{url}")


class AboutDialog(QDialog):
    """关于对话框"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"关于 — {APP_NAME}")
        self.resize(460, 320)

        layout = QVBoxLayout(self)

        title = QLabel(f"<h2>{APP_NAME}</h2><p>版本 {APP_VERSION}</p>")
        title.setAlignment(Qt.AlignCenter)
        layout.addWidget(title)

        info = QLabel(
            f"<p style='text-align:center;'>"
            f"Qt 后端: <b>{QT_BACKEND}</b> ({PYQT_VERSION_STR})<br>"
            f"Python: {sys.version.split()[0]}<br>"
            f"安装路径: {GT_DIR}"
            f"</p>"
        )
        info.setTextFormat(Qt.RichText)
        info.setAlignment(Qt.AlignCenter)
        layout.addWidget(info)

        desc = QLabel(
            "<h4>快捷键</h4>"
            "<table cellpadding='3'>"
            "<tr><td><b>Ctrl+D</b></td><td>诊断</td></tr>"
            "<tr><td><b>Ctrl+T</b></td><td>测试当前输入框的 token</td></tr>"
            "<tr><td><b>Ctrl+S</b></td><td>保存当前 token</td></tr>"
            "<tr><td><b>Ctrl+G</b></td><td>打开 GitHub 生成页</td></tr>"
            "<tr><td><b>Ctrl+Shift+C</b></td><td>复制 fine-grained URL</td></tr>"
            "<tr><td><b>Ctrl+X</b></td><td>清掉旧 token</td></tr>"
            "<tr><td><b>Ctrl+L</b></td><td>显示/隐藏日志 Dock</td></tr>"
            "<tr><td><b>F1</b></td><td>设置指南</td></tr>"
            "<tr><td><b>Ctrl+,</b></td><td>关于</td></tr>"
            "<tr><td><b>Ctrl+Q</b></td><td>退出</td></tr>"
            "</table>"
        )
        desc.setTextFormat(Qt.RichText)
        layout.addWidget(desc)

        close_btn = QPushButton("关闭")
        close_btn.clicked.connect(self.accept)
        layout.addWidget(close_btn, 0, Qt.AlignRight)


class TokenDialog(QMainWindow):
    """GitHub Token 管理主窗口"""

    def __init__(self, auto_check: bool = True):
        super().__init__()
        self.setWindowTitle(f"{APP_NAME} — {APP_ORG}")
        self.resize(760, 580)

        # 当前 token 状态
        self._status: gt.TokenStatus | None = None

        # ===== 中心 widget =====
        central = QWidget()
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(10)

        # 顶部: 状态大字
        status_frame = QFrame()
        status_frame.setFrameShape(QFrame.StyledPanel)
        status_frame.setStyleSheet(
            f"QFrame {{ background: {COLOR_BG}; border: 1px solid #e0e0e0; "
            f"border-radius: 6px; padding: 8px; }}"
        )
        status_layout = QVBoxLayout(status_frame)
        self._big_status = QLabel("未检测")
        big_font = QFont()
        big_font.setPointSize(18)
        big_font.setBold(True)
        self._big_status.setFont(big_font)
        self._big_status.setAlignment(Qt.AlignCenter)
        status_layout.addWidget(self._big_status)

        self._detail_label = QLabel("点击 [诊断] 检查当前 token")
        self._detail_label.setAlignment(Qt.AlignCenter)
        self._detail_label.setStyleSheet(f"color: {COLOR_LABEL}; font-size: 11px;")
        status_layout.addWidget(self._detail_label)
        layout.addWidget(status_frame)

        # 按钮行
        btn_row = QHBoxLayout()
        self._btn_diag = QPushButton("诊断 (Ctrl+D)")
        self._btn_open = QPushButton("打开 GitHub (Ctrl+G)")
        self._btn_copy = QPushButton("复制 URL (Ctrl+Shift+C)")
        self._btn_clear = QPushButton("清掉旧 token (Ctrl+X)")
        for b in (self._btn_diag, self._btn_open, self._btn_copy, self._btn_clear):
            btn_row.addWidget(b)
        layout.addLayout(btn_row)

        # repo 列表
        repo_group = QGroupBox("Repo 访问权限")
        repo_layout = QVBoxLayout(repo_group)
        self._repo_list = RepoListWidget()
        repo_layout.addWidget(self._repo_list)
        layout.addWidget(repo_group, 1)

        # 新 token 输入
        input_row = QHBoxLayout()
        self._token_label = QLabel("新 Token:")
        self._token_label.setStyleSheet(f"color: {COLOR_LABEL}; font-weight: bold;")
        self._token_edit = QLineEdit()
        self._token_edit.setEchoMode(QLineEdit.Password)
        self._token_edit.setPlaceholderText("ghp_xxxxxxxxxxxxxxxxxxxx 或 github_pat_xxxx...")
        self._btn_test = QPushButton("测试 (Ctrl+T)")
        self._btn_save = QPushButton("保存 (Ctrl+S)")
        self._btn_save.setStyleSheet(
            f"QPushButton {{ background: {COLOR_OK}; color: white; padding: 6px 16px; "
            f"border: none; border-radius: 4px; font-weight: bold; }}"
            f"QPushButton:hover {{ background: #1b5e20; }}"
            f"QPushButton:disabled {{ background: #9e9e9e; }}"
        )
        input_row.addWidget(self._token_label)
        input_row.addWidget(self._token_edit, 1)
        input_row.addWidget(self._btn_test)
        input_row.addWidget(self._btn_save)
        layout.addLayout(input_row)

        # 状态栏
        self._statusbar = TokenStatusBar()
        self.setStatusBar(self._statusbar)

        # ===== 构建 QAction / 菜单 / 工具栏 / 日志 Dock =====
        self._build_actions()
        self._build_menu()
        self._build_toolbar()
        self._build_log_dock()

        # ===== 信号连接 =====
        self._btn_diag.clicked.connect(self.on_diagnose)
        self._btn_open.clicked.connect(self.on_open_github)
        self._btn_copy.clicked.connect(self.on_copy_url)
        self._btn_clear.clicked.connect(self.on_clear)
        self._btn_test.clicked.connect(self.on_test)
        self._btn_save.clicked.connect(self.on_save)
        self._token_edit.returnPressed.connect(self.on_test)

        # 启动日志
        self._log("info", f"{APP_NAME} v{APP_VERSION} 启动 (Qt={QT_BACKEND})")

        # 自动诊断
        if auto_check:
            QTimer.singleShot(150, self.on_diagnose)

    # ===== 构建: Action / Menu / Toolbar / Log Dock =====

    def _build_actions(self):
        """构建所有 QAction (菜单/工具栏/快捷键共用)"""
        self.act_diagnose = QAction("诊断(&D)", self)
        self.act_diagnose.setShortcut(QKeySequence("Ctrl+D"))
        self.act_diagnose.setStatusTip("跑完整诊断 (Token + 所有已知 repo)")
        self.act_diagnose.triggered.connect(self.on_diagnose)

        self.act_test = QAction("测试(&T)", self)
        self.act_test.setShortcut(QKeySequence("Ctrl+T"))
        self.act_test.setStatusTip("测试当前输入框的 Token (不保存)")
        self.act_test.triggered.connect(self.on_test)

        self.act_save = QAction("保存(&S)", self)
        self.act_save.setShortcut(QKeySequence("Ctrl+S"))
        self.act_save.setStatusTip("保存当前 Token 到 ~/.git-credentials")
        self.act_save.triggered.connect(self.on_save)

        self.act_open_github = QAction("打开 GitHub(&G)...", self)
        self.act_open_github.setShortcut(QKeySequence("Ctrl+G"))
        self.act_open_github.setStatusTip("打开浏览器到 GitHub Token 生成页")
        self.act_open_github.triggered.connect(self.on_open_github)

        self.act_copy_url = QAction("复制 Fine-grained URL", self)
        self.act_copy_url.setShortcut(QKeySequence("Ctrl+Shift+C"))
        self.act_copy_url.setStatusTip("复制 fine-grained 设置 URL 到剪贴板")
        self.act_copy_url.triggered.connect(self.on_copy_url)

        self.act_clear = QAction("清掉旧 Token(&X)...", self)
        self.act_clear.setShortcut(QKeySequence("Ctrl+X"))
        self.act_clear.setStatusTip("清掉 ~/.git-credentials 里的 github.com 条目")
        self.act_clear.triggered.connect(self.on_clear)

        self.act_copy_cred_path = QAction("复制 ~/.git-credentials 路径", self)
        self.act_copy_cred_path.setStatusTip("复制凭证文件路径到剪贴板")
        self.act_copy_cred_path.triggered.connect(self.on_copy_credentials_path)

        self.act_open_cred_file = QAction("用编辑器打开 ~/.git-credentials", self)
        self.act_open_cred_file.setStatusTip("用默认编辑器打开凭证文件 (需 GUI 环境)")
        self.act_open_cred_file.triggered.connect(self.on_open_credentials_file)

        # 工具菜单
        self.act_cli_diag = QAction("打开 CLI 诊断 (ght-cli)", self)
        self.act_cli_diag.setStatusTip("调起 github-token CLI 跑诊断, 输出到日志面板")
        self.act_cli_diag.triggered.connect(self.on_run_cli_diagnose)

        # 视图菜单
        self.act_toggle_log = QAction("显示日志面板(&L)", self)
        self.act_toggle_log.setCheckable(True)
        self.act_toggle_log.setChecked(False)
        self.act_toggle_log.setShortcut(QKeySequence("Ctrl+L"))
        self.act_toggle_log.setStatusTip("显示/隐藏日志 Dock 面板")
        self.act_toggle_log.toggled.connect(self.on_toggle_log)

        # 帮助菜单
        self.act_help_setup = QAction("设置指南(&F1)...", self)
        self.act_help_setup.setShortcut(QKeySequence("F1"))
        self.act_help_setup.setStatusTip("打开 GitHub Token 设置步骤对话框")
        self.act_help_setup.triggered.connect(self.on_help_setup)

        self.act_about = QAction("关于(&,)...", self)
        self.act_about.setShortcut(QKeySequence("Ctrl+,"))
        self.act_about.setStatusTip("显示关于对话框")
        self.act_about.triggered.connect(self.on_about)

        self.act_about_qt = QAction("关于 Qt...", self)
        self.act_about_qt.triggered.connect(lambda: QMessageBox.aboutQt(self, "About Qt"))

        # 文件菜单
        self.act_quit = QAction("退出(&Q)", self)
        self.act_quit.setShortcut(QKeySequence("Ctrl+Q"))
        self.act_quit.setStatusTip("关闭窗口")
        self.act_quit.triggered.connect(self.close)

    def _build_menu(self):
        """构建菜单栏 (文件 / 编辑 / 工具 / 视图 / 帮助)"""
        mb = self.menuBar()

        # 文件
        m_file = mb.addMenu("文件(&F)")
        m_file.addAction(self.act_open_github)
        m_file.addAction(self.act_copy_url)
        m_file.addSeparator()
        m_file.addAction(self.act_copy_cred_path)
        m_file.addAction(self.act_open_cred_file)
        m_file.addSeparator()
        m_file.addAction(self.act_quit)

        # 编辑 (token 输入相关)
        m_edit = mb.addMenu("编辑(&E)")
        m_edit.addAction(self.act_test)
        m_edit.addAction(self.act_save)
        m_edit.addSeparator()
        m_edit.addAction(self.act_clear)

        # 工具
        m_tools = mb.addMenu("工具(&T)")
        m_tools.addAction(self.act_diagnose)
        m_tools.addAction(self.act_cli_diag)

        # 视图
        m_view = mb.addMenu("视图(&V)")
        m_view.addAction(self.act_toggle_log)

        # 帮助
        m_help = mb.addMenu("帮助(&H)")
        m_help.addAction(self.act_help_setup)
        m_help.addSeparator()
        m_help.addAction(self.act_about)
        m_help.addAction(self.act_about_qt)

        # 主题 (如果有 ac_themes 模块)
        if HAS_THEMES:
            m_theme = mb.addMenu("主题(&Y)")
            self._theme_actions: dict = {}
            for tname, tinfo in ac_themes.THEMES.items():
                act = QAction(tinfo["name"], self)
                act.setCheckable(True)
                act.setData(tname)
                act.triggered.connect(
                    lambda checked, n=tname: self.on_change_theme(n)
                )
                self._theme_actions[tname] = act
                m_theme.addAction(act)
            # 同步当前主题打勾
            cur = ac_themes.get_current_theme()
            for tn, ta in self._theme_actions.items():
                ta.setChecked(tn == cur)

    def _build_toolbar(self):
        """构建可拖动工具栏"""
        tb = QToolBar("主工具栏", self)
        tb.setObjectName("MainToolBar")  # 必填, 否则 saveState/restoreState 报警
        tb.setMovable(True)
        tb.setFloatable(True)
        tb.setIconSize(tb.iconSize())  # 默认尺寸
        # 添加工具按钮 (跟菜单共享 action, 这样快捷键/状态/可见性自动同步)
        tb.addAction(self.act_diagnose)
        tb.addAction(self.act_open_github)
        tb.addAction(self.act_copy_url)
        tb.addSeparator()
        tb.addAction(self.act_test)
        tb.addAction(self.act_save)
        tb.addSeparator()
        tb.addAction(self.act_clear)
        tb.addSeparator()
        tb.addAction(self.act_toggle_log)
        tb.addAction(self.act_help_setup)
        self.addToolBar(tb)

    def _build_log_dock(self):
        """构建日志 Dock (默认隐藏, Ctrl+L 切换)"""
        self._log_widget = LogDockWidget()
        self._log_dock = QDockWidget("操作日志", self)
        self._log_dock.setObjectName("LogDock")
        self._log_dock.setWidget(self._log_widget)
        self._log_dock.setAllowedAreas(
            Qt.LeftDockWidgetArea | Qt.RightDockWidgetArea
            | Qt.BottomDockWidgetArea | Qt.TopDockWidgetArea
        )
        self._log_dock.setFeatures(
            QDockWidget.DockWidgetMovable
            | QDockWidget.DockWidgetFloatable
            | QDockWidget.DockWidgetClosable
        )
        # 默认放底部, 但隐藏
        self.addDockWidget(Qt.BottomDockWidgetArea, self._log_dock)
        self._log_dock.hide()
        # dock 关闭按钮 → 同步到 action
        self._log_dock.visibilityChanged.connect(
            lambda v: self.act_toggle_log.setChecked(v)
        )

    # ===== 日志助手 =====

    def _log(self, level: str, msg: str):
        """写一行日志到 Dock 面板"""
        if hasattr(self, "_log_widget"):
            self._log_widget.append(level, msg)
        # 同时打印到 stderr (方便沙箱/CI 抓 log)
        try:
            ts = datetime.now().strftime("%H:%M:%S")
            print(f"[{ts}] [{level.upper()}] {msg}", file=sys.stderr)
        except Exception:
            pass

    # ===== 槽函数 =====

    def on_diagnose(self):
        """诊断按钮: 跑完整诊断"""
        self._log("info", "开始诊断...")
        self._statusbar.show_msg("诊断中...", "info")
        self._btn_diag.setEnabled(False)
        # 用 QTimer 异步跑, 不阻塞 UI
        QTimer.singleShot(50, self._do_diagnose_async)

    def _do_diagnose_async(self):
        try:
            self._status = gt.diagnose(verbose=False)
            self._update_ui_from_status()
            if self._status.valid:
                msg = f"诊断完成: ✓ 有效 (用户 {self._status.user})"
                self._statusbar.show_msg(msg, "ok")
                self._log("ok", msg)
            else:
                msg = f"诊断完成: ✗ 无效 ({self._status.error or '未知错误'})"
                self._statusbar.show_msg(msg, "err")
                self._log("err", msg)
        except Exception as e:
            self._statusbar.show_msg(f"诊断异常: {e}", "err")
            self._log("err", f"诊断异常: {e}")
        finally:
            self._btn_diag.setEnabled(True)

    def _update_ui_from_status(self):
        """根据 self._status 更新 UI"""
        st = self._status
        if not st:
            return
        if not st.token:
            self._big_status.setText("⚠ 未设置 Token")
            self._big_status.setStyleSheet(f"color: {COLOR_WARN};")
            self._detail_label.setText("点击 [打开 GitHub] 生成, 然后粘贴并保存")
            self._statusbar.set_right("无 token", COLOR_WARN)
            return
        if st.valid:
            masked = st.token[:4] + "..." + st.token[-4:] if len(st.token) > 8 else "***"
            self._big_status.setText(f"✓ Token 有效")
            self._big_status.setStyleSheet(f"color: {COLOR_OK};")
            detail = f"用户: {st.user}  |  类型: {'fine-grained' if st.is_fine_grained else 'classic'}"
            if st.rate_remaining >= 0:
                detail += f"  |  Rate: {st.rate_remaining}"
            detail += f"  |  来源: {st.source}"
            self._detail_label.setText(detail)
            self._statusbar.set_right(masked, COLOR_OK)
        else:
            self._big_status.setText("✗ Token 无效")
            self._big_status.setStyleSheet(f"color: {COLOR_ERR};")
            self._detail_label.setText(f"{st.error}  |  来源: {st.source}")
            self._statusbar.set_right("无效", COLOR_ERR)
        # 更新 repo 列表
        if st.repos:
            self._repo_list.update_repos(st.repos)

    def on_open_github(self):
        """打开 GitHub 生成 token 页面 (fine-grained)"""
        url = gt.SETUP_URLS["fine-grained"]
        self._log("info", f"打开浏览器: {url}")
        self._statusbar.show_msg(f"打开 {url}", "info")
        # 优先用 webbrowser, 失败回退到 xdg-open
        try:
            webbrowser.open(url)
        except Exception:
            try:
                subprocess.Popen(["xdg-open", url])
            except Exception as e:
                self._statusbar.show_msg(f"打开失败: {e}  (请手动访问 URL)", "err")
                self._log("err", f"浏览器打开失败: {e}")
                return
        # 弹个提示
        QMessageBox.information(
            self, "打开 GitHub",
            f"已在浏览器中打开:\n  {url}\n\n"
            f"生成后复制 token, 粘贴到下方输入框, 点击 [测试] → [保存]\n\n"
            f"推荐: Contents: Read and write + Resources: All repositories"
        )

    def on_copy_url(self):
        """复制 fine-grained token URL 到剪贴板"""
        url = gt.SETUP_URLS["fine-grained"]
        QApplication.clipboard().setText(url)
        self._statusbar.show_msg(f"已复制 URL 到剪贴板: {url}", "ok")
        self._log("ok", f"已复制 URL 到剪贴板: {url}")

    def on_clear(self):
        """清掉旧 token (弹确认)"""
        ret = QMessageBox.question(
            self, "确认清掉",
            "确定要清掉 ~/.git-credentials 里的所有 github.com token 条目吗?\n"
            "(其他 host 的 token 不会动)",
            QMessageBox.Yes | QMessageBox.No,
            QMessageBox.No,
        )
        if ret != QMessageBox.Yes:
            self._statusbar.show_msg("已取消", "info")
            return
        self._log("warn", "清掉 ~/.git-credentials 里的 github.com 条目")
        rc = gt.clear_token()
        if rc == 0:
            self._statusbar.show_msg("已清掉旧 token", "ok")
            self._log("ok", "已清掉旧 token, 重新诊断...")
        else:
            self._statusbar.show_msg("清掉失败", "err")
            self._log("err", "清掉失败")
        # 重新诊断
        QTimer.singleShot(100, self.on_diagnose)

    def on_test(self):
        """测试新 token (但不保存)"""
        token = self._token_edit.text().strip()
        if not token:
            self._statusbar.show_msg("token 为空, 先粘贴", "warn")
            self._log("warn", "测试被取消: token 输入框为空")
            return
        self._log("info", f"测试 token: {token[:4]}...{token[-4:] if len(token) > 8 else '...'}")
        self._statusbar.show_msg("测试中...", "info")
        self._btn_test.setEnabled(False)
        QTimer.singleShot(50, lambda: self._do_test_async(token))

    def _do_test_async(self, token: str):
        try:
            st = gt.check_token(token)
            if st.valid:
                self._statusbar.show_msg(
                    f"✓ 测试通过: 用户 {st.user}  ({'fine-grained' if st.is_fine_grained else 'classic'})",
                    "ok"
                )
                self._log("ok", f"测试通过: user={st.user}, type={'fine-grained' if st.is_fine_grained else 'classic'}")
                # 显示用户/类型在 big_status
                self._big_status.setText(f"✓ Token 测试通过 (未保存)")
                self._big_status.setStyleSheet(f"color: {COLOR_INFO};")
                self._detail_label.setText(f"用户: {st.user}  |  类型: {'fine-grained' if st.is_fine_grained else 'classic'}")
            else:
                self._statusbar.show_msg(f"✗ 测试失败: {st.error}", "err")
                self._log("err", f"测试失败: {st.error}")
                self._big_status.setText(f"✗ Token 测试失败")
                self._big_status.setStyleSheet(f"color: {COLOR_ERR};")
                self._detail_label.setText(st.error)
        except Exception as e:
            self._statusbar.show_msg(f"测试异常: {e}", "err")
            self._log("err", f"测试异常: {e}")
        finally:
            self._btn_test.setEnabled(True)

    def on_save(self):
        """保存新 token 到 ~/.git-credentials"""
        token = self._token_edit.text().strip()
        if not token:
            self._statusbar.show_msg("token 为空, 先粘贴", "warn")
            self._log("warn", "保存被取消: token 输入框为空")
            return
        self._log("info", f"保存 token: {token[:4]}...{token[-4:] if len(token) > 8 else '...'}")
        self._statusbar.show_msg("保存中...", "info")
        self._btn_save.setEnabled(False)
        QTimer.singleShot(50, lambda: self._do_save_async(token))

    def _do_save_async(self, token: str):
        try:
            # 先测试, 确保有效
            st = gt.check_token(token)
            if not st.valid:
                self._statusbar.show_msg(f"✗ 不能保存: token 无效 ({st.error})", "err")
                self._log("err", f"保存前测试失败: {st.error}")
                return
            # 保存
            rc = gt.set_token(token)
            if rc == 0:
                msg = f"✓ 已保存, 用户 {st.user}  ({len(gt.KNOWN_REPOS)} 个 repo 待诊断)"
                self._statusbar.show_msg(msg, "ok")
                self._log("ok", msg)
                self._token_edit.clear()
                # 重新诊断 (用新 token)
                QTimer.singleShot(100, self.on_diagnose)
            else:
                self._statusbar.show_msg("保存失败", "err")
                self._log("err", "set_token 返回非 0")
        except Exception as e:
            self._statusbar.show_msg(f"保存异常: {e}", "err")
            self._log("err", f"保存异常: {e}")
        finally:
            self._btn_save.setEnabled(True)

    def on_help_setup(self):
        """打开设置指南对话框"""
        self._log("info", "打开设置指南")
        dlg = SetupHelpDialog(self)
        dlg.exec()

    def on_about(self):
        """打开关于对话框"""
        dlg = AboutDialog(self)
        dlg.exec()

    def on_toggle_log(self, checked: bool):
        """切换日志 Dock 可见性"""
        if checked:
            self._log_dock.show()
            self._log_dock.raise_()
        else:
            self._log_dock.hide()

    def on_copy_credentials_path(self):
        """复制 ~/.git-credentials 路径到剪贴板"""
        path = str(Path.home() / ".git-credentials")
        QApplication.clipboard().setText(path)
        self._statusbar.show_msg(f"已复制: {path}", "ok")
        self._log("ok", f"复制凭证路径: {path}")

    def on_open_credentials_file(self):
        """用默认编辑器打开 ~/.git-credentials"""
        path = Path.home() / ".git-credentials"
        if not path.exists():
            self._statusbar.show_msg(f"文件不存在: {path}", "warn")
            self._log("warn", f"凭证文件不存在: {path}")
            return
        # 用 xdg-open (Linux 默认)
        try:
            subprocess.Popen(["xdg-open", str(path)])
            self._statusbar.show_msg(f"已打开: {path}", "ok")
            self._log("ok", f"xdg-open {path}")
        except Exception as e:
            self._statusbar.show_msg(f"打开失败: {e}", "err")
            self._log("err", f"打开凭证文件失败: {e}")

    def on_change_theme(self, theme_name: str):
        """切主题, 立即应用 + 写配置"""
        if not HAS_THEMES:
            self._log("warn", "ac_themes 模块未加载, 切主题不可用")
            return
        if theme_name not in ac_themes.THEMES:
            return
        app = QApplication.instance()
        if app is None:
            return
        ac_themes.apply_theme(app, theme_name)
        # 同步打勾 (单选)
        for tn, act in self._theme_actions.items():
            act.setChecked(tn == theme_name)
        self._log("ok", f"主题切换: {ac_themes.THEMES[theme_name]['name']} (已保存)")

    def on_run_cli_diagnose(self):
        """调起 github-token CLI 跑诊断, 把输出打到日志"""
        tool = GT_DIR / "github_token"
        if not tool.exists():
            self._log("err", f"找不到 CLI: {tool}")
            return
        self._log("info", f"运行 CLI: {tool} diag")
        try:
            # 用 OS 解析 shebang (不强制 sys.executable)
            result = subprocess.run(
                [str(tool), "diag"],
                capture_output=True, text=True, timeout=30,
            )
            self._log("info", f"CLI 返回码: {result.returncode}")
            for line in (result.stdout or "").splitlines():
                self._log("info", f"  > {line}")
            for line in (result.stderr or "").splitlines():
                self._log("warn", f"  ! {line}")
            if result.returncode == 0:
                self._statusbar.show_msg("CLI 诊断完成 (见日志)", "ok")
            else:
                self._statusbar.show_msg(f"CLI 诊断失败 rc={result.returncode}", "err")
        except subprocess.TimeoutExpired:
            self._log("err", "CLI 诊断超时 (30s)")
            self._statusbar.show_msg("CLI 诊断超时", "err")
        except Exception as e:
            self._log("err", f"CLI 诊断异常: {e}")
            self._statusbar.show_msg(f"CLI 诊断异常: {e}", "err")


def main() -> int:
    parser_args = []
    auto_check = True
    # 简易 argv 解析 (避免跟 Qt QApplication 冲突)
    for a in sys.argv[1:]:
        if a in ("--auto-check", "--check"):
            auto_check = True
        elif a in ("--no-check",):
            auto_check = False
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        elif a in ("--cli",):
            # GUI 不跑, 走 CLI 模式
            return subprocess.call([str(GT_DIR / "github_token")] + sys.argv[2:])
    # Qt 应用
    if "QT_QPA_PLATFORM" not in os.environ and not os.environ.get("DISPLAY") \
            and not os.environ.get("WAYLAND_DISPLAY"):
        os.environ["QT_QPA_PLATFORM"] = "offscreen"  # 沙箱默认 offscreen
    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    app.setApplicationVersion(APP_VERSION)
    app.setOrganizationName(APP_ORG)
    # 应用主题 (从配置读, 默认 dark)
    if HAS_THEMES:
        cur = ac_themes.apply_theme(app, None)
        sys.stderr.write(f"[主题] {ac_themes.THEMES[cur]['name']}\n")
    w = TokenDialog(auto_check=auto_check)
    w.show()
    return getattr(app, APP_EXEC)()


if __name__ == "__main__":
    sys.exit(main())
