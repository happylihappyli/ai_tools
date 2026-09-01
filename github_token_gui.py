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

布局 (按用户偏好: 状态栏在底, 颜色编码, 错误停自动滚动):
    ┌─────────────────────────────────────────────┐
    │  [状态大字: Token 状态 / 用户 / rate]       │  ← 顶部
    ├─────────────────────────────────────────────┤
    │  [诊断] [打开 GitHub] [复制 URL] [清掉]      │  ← 按钮行
    ├─────────────────────────────────────────────┤
    │  Repo 列表 (3 个已知 + user info)            │  ← 中部
    ├─────────────────────────────────────────────┤
    │  新 Token: [_______________] [测试] [保存]   │  ← 底部输入
    ├─────────────────────────────────────────────┤
    │  状态栏: ✓ Token 有效 / 5000 次剩余         │  ← 底栏
    └─────────────────────────────────────────────┘
"""
import os
import sys
import subprocess
import webbrowser
from pathlib import Path

GT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(GT_DIR))

# 先 import github_token 核心 (再 import _qt_compat, 因为 _qt_compat 不依赖 github_token)
import github_token as gt  # noqa: E402

try:
    from _qt_compat import (
        QT_BACKEND, APP_EXEC, gui_available,
        QtCore, QtWidgets, QtGui,
        Qt, QTimer, Signal, Slot,
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QLabel, QLineEdit, QPushButton, QTreeWidget, QTreeWidgetItem,
        QStatusBar, QMessageBox, QColor, QFont, QFrame,
        QSizePolicy, QGroupBox,
    )
except Exception as e:
    print(f"✗ Qt 加载失败: {e}", file=sys.stderr)
    print("  需要: pip install PySide6 (或 PyQt5/PySide2)", file=sys.stderr)
    sys.exit(1)

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
        else:
            # 成功/警告后, 等用户点击才恢复
            pass

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


class TokenDialog(QMainWindow):
    """GitHub Token 管理主窗口"""

    def __init__(self, auto_check: bool = True):
        super().__init__()
        self.setWindowTitle("GitHub Token 管理器 — ai_tools")
        self.resize(720, 540)

        # 当前 token 状态
        self._status: gt.TokenStatus | None = None

        # 中心 widget
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
        self._btn_diag = QPushButton("诊断 (D)")
        self._btn_diag.setShortcut("D")
        self._btn_open = QPushButton("打开 GitHub (G)")
        self._btn_open.setShortcut("G")
        self._btn_copy = QPushButton("复制 URL (C)")
        self._btn_copy.setShortcut("C")
        self._btn_clear = QPushButton("清掉旧 token (X)")
        self._btn_clear.setShortcut("X")
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
        self._btn_test = QPushButton("测试")
        self._btn_save = QPushButton("保存")
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

        # 信号连接
        self._btn_diag.clicked.connect(self.on_diagnose)
        self._btn_open.clicked.connect(self.on_open_github)
        self._btn_copy.clicked.connect(self.on_copy_url)
        self._btn_clear.clicked.connect(self.on_clear)
        self._btn_test.clicked.connect(self.on_test)
        self._btn_save.clicked.connect(self.on_save)
        self._token_edit.returnPressed.connect(self.on_test)

        # 自动诊断
        if auto_check:
            QTimer.singleShot(100, self.on_diagnose)

    # ===== 槽函数 =====

    def on_diagnose(self):
        """诊断按钮: 跑完整诊断"""
        self._statusbar.show_msg("诊断中...", "info")
        self._btn_diag.setEnabled(False)
        # 用 QTimer 异步跑, 不阻塞 UI
        QTimer.singleShot(50, self._do_diagnose_async)

    def _do_diagnose_async(self):
        try:
            self._status = gt.diagnose(verbose=False)
            self._update_ui_from_status()
            if self._status.valid:
                self._statusbar.show_msg(
                    f"诊断完成: ✓ 有效 (用户 {self._status.user})", "ok"
                )
            else:
                self._statusbar.show_msg(
                    f"诊断完成: ✗ 无效 ({self._status.error or '未知错误'})", "err"
                )
        except Exception as e:
            self._statusbar.show_msg(f"诊断异常: {e}", "err")
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
        self._statusbar.show_msg(f"打开 {url}", "info")
        # 优先用 webbrowser, 失败回退到 xdg-open
        try:
            webbrowser.open(url)
        except Exception:
            try:
                subprocess.Popen(["xdg-open", url])
            except Exception as e:
                self._statusbar.show_msg(f"打开失败: {e}  (请手动访问 URL)", "err")
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
        rc = gt.clear_token()
        if rc == 0:
            self._statusbar.show_msg("已清掉旧 token", "ok")
        else:
            self._statusbar.show_msg("清掉失败", "err")
        # 重新诊断
        QTimer.singleShot(100, self.on_diagnose)

    def on_test(self):
        """测试新 token (但不保存)"""
        token = self._token_edit.text().strip()
        if not token:
            self._statusbar.show_msg("token 为空, 先粘贴", "warn")
            return
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
                # 显示用户/类型在 big_status
                self._big_status.setText(f"✓ Token 测试通过 (未保存)")
                self._big_status.setStyleSheet(f"color: {COLOR_INFO};")
                self._detail_label.setText(f"用户: {st.user}  |  类型: {'fine-grained' if st.is_fine_grained else 'classic'}")
            else:
                self._statusbar.show_msg(f"✗ 测试失败: {st.error}", "err")
                self._big_status.setText(f"✗ Token 测试失败")
                self._big_status.setStyleSheet(f"color: {COLOR_ERR};")
                self._detail_label.setText(st.error)
        except Exception as e:
            self._statusbar.show_msg(f"测试异常: {e}", "err")
        finally:
            self._btn_test.setEnabled(True)

    def on_save(self):
        """保存新 token 到 ~/.git-credentials"""
        token = self._token_edit.text().strip()
        if not token:
            self._statusbar.show_msg("token 为空, 先粘贴", "warn")
            return
        self._statusbar.show_msg("保存中...", "info")
        self._btn_save.setEnabled(False)
        QTimer.singleShot(50, lambda: self._do_save_async(token))

    def _do_save_async(self, token: str):
        try:
            # 先测试, 确保有效
            st = gt.check_token(token)
            if not st.valid:
                self._statusbar.show_msg(f"✗ 不能保存: token 无效 ({st.error})", "err")
                return
            # 保存
            rc = gt.set_token(token)
            if rc == 0:
                self._statusbar.show_msg(
                    f"✓ 已保存, 用户 {st.user}  ({len(gt.KNOWN_REPOS)} 个 repo 待诊断)",
                    "ok"
                )
                self._token_edit.clear()
                # 重新诊断 (用新 token)
                QTimer.singleShot(100, self.on_diagnose)
            else:
                self._statusbar.show_msg("保存失败", "err")
        except Exception as e:
            self._statusbar.show_msg(f"保存异常: {e}", "err")
        finally:
            self._btn_save.setEnabled(True)


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
            return subprocess.call([sys.executable, str(GT_DIR / "github_token")] + sys.argv[2:])
    # Qt 应用
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")  # 沙箱默认 offscreen
    app = QApplication(sys.argv)
    app.setApplicationName("GitHub Token Manager")
    w = TokenDialog(auto_check=auto_check)
    w.show()
    return getattr(app, APP_EXEC)()


if __name__ == "__main__":
    sys.exit(main())
