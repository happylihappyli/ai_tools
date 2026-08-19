# -*- coding: utf-8 -*-
"""
Qt 多后端兼容层
=================
自动检测并加载可用的 Qt 绑定 (PySide6 → PyQt5 → PySide2)。
用法:
    from _qt_compat import (
        QT_BACKEND, APP_EXEC, QtCore, QtWidgets, QtGui,
        QApplication, QMainWindow, ...
    )

backend 优先级:
    1. 环境变量 QT_BACKEND 强制 (qtbackend=pyside6|pyqt5|pyside2)
    2. PySide6 (Qt6)
    3. PyQt5   (Qt5)
    4. PySide2 (Qt5)
"""

import os
import sys
import importlib

# ====== 检测后端 ======
# 优先用环境变量强制
_FORCED = os.environ.get("QT_BACKEND", "").strip().lower()

QT_BACKEND: str = "none"
APP_EXEC: str = "exec_"  # PyQt5/PySide2 用 exec_, PySide6 用 exec
PYQT_VERSION_STR: str = ""


def _try_load(modname: str) -> bool:
    """尝试 import, 成功返回 True。"""
    try:
        importlib.import_module(modname)
        return True
    except Exception:
        return False


# 后端名 → python 模块名映射
_PYPREFIX = {"pyside6": "PySide6", "pyqt5": "PyQt5", "pyside2": "PySide2"}


def _detect_backend() -> str:
    """按优先级检测可用的 Qt 后端。返回 'pyside6' | 'pyqt5' | 'pyside2' | 'none'.

    优先级: 环境变量 QT_BACKEND > PySide6 > PyQt5 > PySide2
    如果强制后端不可用, 自动 fallback。
    """
    if _FORCED in ("pyside6", "pyqt5", "pyside2"):
        if _try_load(_PYPREFIX[_FORCED]):
            return _FORCED
        # 强制但不可用, 打印警告后 fallback
        sys.stderr.write(
            f"[qt_compat] 警告: QT_BACKEND={_FORCED} 不可用, 自动 fallback...\n"
        )
    # 自动检测
    for name in ("pyside6", "pyqt5", "pyside2"):
        if _try_load(_PYPREFIX[name]):
            return name
    return "none"


QT_BACKEND = _detect_backend()
if QT_BACKEND == "pyside6":
    APP_EXEC = "exec"
elif QT_BACKEND in ("pyqt5", "pyside2"):
    APP_EXEC = "exec_"


# ====== 按后端 import 并 re-export ======
QtCore = None
QtWidgets = None
QtGui = None
QAction = None
QApplication = None
QCheckBox = None
QColor = None
QComboBox = None
QFileDialog = None
QFont = None
QGraphicsItem = None
QGraphicsPathItem = None
QGraphicsScene = None
QGraphicsTextItem = None
QGraphicsView = None
QHBoxLayout = None
QBrush = None
QInputDialog = None
QKeySequence = None
QLabel = None
QLineEdit = None
QListWidget = None
QListWidgetItem = None
QMainWindow = None
QMenu = None
QMessageBox = None
QObject = None
QPainter = None
QPainterPath = None
QPalette = None
QPen = None
QPointF = None
QRectF = None
QPlainTextEdit = None
QProcess = None
QProcessEnvironment = None
QProgressBar = None
QPushButton = None
QShortcut = None
QSizePolicy = None
QSplitter = None
QStatusBar = None
QTabWidget = None
QTextCursor = None
QTextEdit = None
QTimer = None
QToolBar = None
QVBoxLayout = None
QWidget = None
Qt = None
Signal = None
Slot = None

if QT_BACKEND == "pyside6":
    from PySide6 import QtCore, QtWidgets, QtGui
    from PySide6.QtCore import (
        Qt, QTimer, QObject, Signal, Slot, QProcess, QProcessEnvironment,
        QPointF, QRectF, QFileSystemWatcher
    )
    from PySide6.QtGui import (
        QAction, QBrush, QColor, QFont, QKeySequence, QPainter, QPainterPath,
        QPalette, QPen, QTextCursor
    )
    from PySide6.QtWidgets import (
        QApplication, QCheckBox, QComboBox, QFileDialog, QGraphicsItem,
        QGraphicsPathItem, QGraphicsScene, QGraphicsTextItem, QGraphicsView,
        QGroupBox, QHBoxLayout, QInputDialog, QLabel, QLineEdit, QListWidget,
        QListWidgetItem, QMainWindow, QMenu, QMessageBox, QPlainTextEdit,
        QProgressBar, QPushButton, QShortcut, QSizePolicy, QSplitter, QStatusBar,
        QTabWidget, QTextEdit, QToolBar, QVBoxLayout, QWidget
    )
    PYQT_VERSION_STR = QtCore.__version__ if hasattr(QtCore, "__version__") else "6"

elif QT_BACKEND == "pyqt5":
    from PyQt5 import QtCore, QtWidgets, QtGui
    from PyQt5.QtCore import (
        Qt, QTimer, QObject, pyqtSignal as Signal, pyqtSlot as Slot,
        QProcess, QProcessEnvironment, QPointF, QRectF, QFileSystemWatcher
    )
    from PyQt5.QtGui import (
        QBrush, QColor, QFont, QKeySequence, QPainter, QPainterPath, QPalette,
        QPen, QTextCursor
    )
    # PyQt5 的 QAction 在 QtWidgets 里 (Qt6 移到 QtGui)
    from PyQt5.QtWidgets import (
        QAction, QApplication, QCheckBox, QComboBox, QFileDialog, QGraphicsItem,
        QGraphicsPathItem, QGraphicsScene, QGraphicsTextItem, QGraphicsView,
        QGroupBox, QHBoxLayout, QInputDialog, QLabel, QLineEdit, QListWidget,
        QListWidgetItem, QMainWindow, QMenu, QMessageBox, QPlainTextEdit,
        QProgressBar, QPushButton, QShortcut, QSizePolicy, QSplitter, QStatusBar,
        QTabWidget, QTextEdit, QToolBar, QVBoxLayout, QWidget
    )
    PYQT_VERSION_STR = QtCore.PYQT_VERSION_STR

elif QT_BACKEND == "pyside2":
    from PySide2 import QtCore, QtWidgets, QtGui
    from PySide2.QtCore import (
        Qt, QTimer, QObject, Signal, Slot, QProcess, QProcessEnvironment,
        QPointF, QRectF, QFileSystemWatcher
    )
    from PySide2.QtGui import (
        QBrush, QColor, QFont, QKeySequence, QPainter, QPainterPath, QPalette,
        QPen, QTextCursor
    )
    from PySide2.QtWidgets import (
        QAction, QApplication, QCheckBox, QComboBox, QFileDialog, QGraphicsItem,
        QGraphicsPathItem, QGraphicsScene, QGraphicsTextItem, QGraphicsView,
        QGroupBox, QHBoxLayout, QInputDialog, QLabel, QLineEdit, QListWidget,
        QListWidgetItem, QMainWindow, QMenu, QMessageBox, QPlainTextEdit,
        QProgressBar, QPushButton, QShortcut, QSizePolicy, QSplitter, QStatusBar,
        QTabWidget, QTextEdit, QToolBar, QVBoxLayout, QWidget
    )
    PYQT_VERSION_STR = QtCore.__version__


# ====== 公共函数 ======
def gui_available() -> bool:
    """返回 GUI 是否可用 (有可用 Qt 后端)."""
    return QT_BACKEND != "none"


def explain_choices() -> list:
    """返回可用的 Qt 后端列表 (供错误信息)."""
    out = []
    if _try_load("PySide6"):
        out.append("pyside6")
    if _try_load("PyQt5"):
        out.append("pyqt5")
    if _try_load("PySide2"):
        out.append("pyside2")
    return out


# 自检
if __name__ == "__main__":
    print(f"QT_BACKEND: {QT_BACKEND}")
    print(f"PYQT_VERSION_STR: {PYQT_VERSION_STR}")
    print(f"APP_EXEC: {APP_EXEC}")
    print(f"GUI available: {gui_available()}")
    print(f"Choices: {explain_choices()}")
