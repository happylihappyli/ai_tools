# -*- coding: utf-8 -*-
"""
步骤跟踪工具（Step Tracker）
===========================
一个基于 PySide6 的桌面应用，用于跟踪 AI / Agent 的工作步骤：
  - 显示当前主目标
  - 以流程图形式展示所有子目标节点
  - 支持添加 / 修改 / 删除节点
  - 支持 CLI 调用（让 AI 通过命令行更新步骤）
  - 自动持久化到 JSON 文件

用法：
    python step_tracker.py                # 启动 GUI
    python step_tracker.py gui            # 启动 GUI（同上）
    python step_tracker.py list           # 命令行列出所有节点
    python step_tracker.py set-goal "..."  # 设置主目标
    python step_tracker.py add ...        # 添加节点
    python step_tracker.py update ...     # 修改节点
    python step_tracker.py delete ID      # 删除节点
    python step_tracker.py current ID     # 设置当前节点
"""

import sys
import os
import json
import argparse
from pathlib import Path
from typing import Dict, List, Optional

# ===== 常量与配置 =====
APP_DIR = Path(__file__).parent
DEFAULT_DATA_FILE = APP_DIR / "step_data.json"


def _resolve_data_file() -> Path:
    """解析数据文件路径: env TRACKER_DATA_FILE > 默认。"""
    env = os.environ.get("TRACKER_DATA_FILE")
    if env:
        return Path(env).expanduser().resolve()
    return DEFAULT_DATA_FILE


# 模块加载时解析一次 (支持 env); 之后 set_data_file() / 全局赋值可改
DATA_FILE = _resolve_data_file()

# 暗色系主题色
COLOR_BG = "#1e1e2e"           # 主背景（深紫黑）
COLOR_PANEL = "#2a2a3e"        # 面板背景
COLOR_NODE_PENDING = "#3b3b5e"  # 未开始节点
COLOR_NODE_RUNNING = "#0d9488"  # 进行中节点（青绿色，呼应用户偏好）
COLOR_NODE_DONE = "#4a4a6a"    # 已完成节点
COLOR_NODE_CURRENT = "#fbbf24"  # 当前节点边框
COLOR_TEXT = "#e0e0e0"
COLOR_EDGE = "#6b7280"
COLOR_EDGE_ACTIVE = "#0d9488"
COLOR_ACCENT = "#0d9488"


# ===== 数据模型 =====
class StepData:
    """步骤数据管理，负责加载/保存 JSON 数据。"""

    def __init__(self, data_file: Optional[Path] = None):
        """初始化数据管理器，加载已有数据或创建默认数据。
        data_file 为 None 时用模块全局 DATA_FILE (支持运行时修改)。"""
        if data_file is None:
            data_file = DATA_FILE  # 读全局, 支持 CLI/env 覆盖
        self.data_file = data_file
        self.data: Dict = {}
        self.load()

    def load(self) -> None:
        """从 JSON 文件加载数据，若文件不存在则创建默认结构。"""
        if self.data_file.exists():
            try:
                with open(self.data_file, "r", encoding="utf-8") as f:
                    self.data = json.load(f)
                # 兼容老数据: 没 events 字段就补
                if "events" not in self.data:
                    self.data["events"] = []
            except (json.JSONDecodeError, OSError):
                self.data = self._default_data()
        else:
            self.data = self._default_data()
            self.save()

    def save(self) -> None:
        """将当前数据持久化到 JSON 文件。"""
        with open(self.data_file, "w", encoding="utf-8") as f:
            json.dump(self.data, f, ensure_ascii=False, indent=2)

    @staticmethod
    def _default_data() -> Dict:
        """返回默认的空数据结构。"""
        return {
            "main_goal": "",
            "current_node": None,
            "nodes": [],
            "events": [],
        }

    # --- 主目标 ---
    def set_main_goal(self, goal: str) -> None:
        """设置主目标。"""
        self.data["main_goal"] = goal
        self.save()

    # --- 节点操作 ---
    def add_node(self, node_id: str, title: str, description: str = "",
                 status: str = "pending", x: float = 0, y: float = 0,
                 next_nodes: Optional[List[str]] = None) -> Dict:
        """添加一个新节点，若 id 重复则抛错。"""
        if any(n["id"] == node_id for n in self.data["nodes"]):
            raise ValueError(f"节点 {node_id} 已存在")
        node = {
            "id": node_id,
            "title": title,
            "description": description,
            "status": status,  # pending / in_progress / completed
            "x": x,
            "y": y,
            "next": next_nodes or [],
        }
        self.data["nodes"].append(node)
        self.save()
        return node

    def update_node(self, node_id: str, **kwargs) -> Dict:
        """按节点 id 修改节点字段，未提供的字段保持不变。"""
        for node in self.data["nodes"]:
            if node["id"] == node_id:
                for k, v in kwargs.items():
                    if k in node:
                        node[k] = v
                self.save()
                return node
        raise ValueError(f"节点 {node_id} 不存在")

    def delete_node(self, node_id: str) -> None:
        """按 id 删除节点，同时清理其他节点 next 中对该 id 的引用。"""
        self.data["nodes"] = [n for n in self.data["nodes"] if n["id"] != node_id]
        for node in self.data["nodes"]:
            if node_id in node.get("next", []):
                node["next"] = [nid for nid in node["next"] if nid != node_id]
        if self.data.get("current_node") == node_id:
            self.data["current_node"] = None
        self.save()

    def set_current(self, node_id: str) -> None:
        """设置当前正在进行的节点。"""
        if not any(n["id"] == node_id for n in self.data["nodes"]):
            raise ValueError(f"节点 {node_id} 不存在")
        self.data["current_node"] = node_id
        # 自动把该节点状态置为 in_progress
        self.update_node(node_id, status="in_progress")
        self.save()

    def get_current(self) -> Optional[str]:
        """获取当前节点 id，没有则返回 None。"""
        return self.data.get("current_node")

    def get_node(self, node_id: str) -> Optional[Dict]:
        """按 id 获取节点，找不到返回 None。"""
        for n in self.data["nodes"]:
            if n["id"] == node_id:
                return n
        return None

    def list_nodes(self) -> List[Dict]:
        """返回所有节点列表。"""
        return list(self.data["nodes"])


# ===== GUI 部分 =====
# GUI 依赖: 通过 _qt_compat 自动选 PySide6 / PyQt5 / PySide2
# 如果都没有, 给占位类 (CLI 仍可工作)
try:
    from _qt_compat import (
        gui_available,
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QLabel, QLineEdit, QTextEdit, QPushButton, QGraphicsView,
        QGraphicsScene, QGraphicsItem, QGraphicsPathItem, QMenu, QInputDialog,
        QMessageBox, QSplitter, QStatusBar, QToolBar, QListWidget, QListWidgetItem,
        QComboBox, QBrush,
        Qt, QRectF, QPointF, QTimer,
        QPainter, QColor, QPen, QFont, QAction, QPainterPath
    )
    _GUI_AVAILABLE = gui_available()
except ImportError:
    _GUI_AVAILABLE = False
    # 占位类: 任何属性 / 调用都返回自身 (用于惰性报错)
    def _make_stub(name):
        class _Stub:
            def __init__(self, *a, **kw): pass
            def __getattr__(self, name): return _stub_instance
            def __call__(self, *a, **kw): return _stub_instance
            def __repr__(self): return f"<{name}-Stub (Qt missing)>"
        _Stub.__name__ = name
        return _Stub
    _stub_instance = object()
    QApplication = _make_stub("QApplication")
    QMainWindow = _make_stub("QMainWindow")
    QWidget = _make_stub("QWidget")
    QVBoxLayout = _make_stub("QVBoxLayout")
    QHBoxLayout = _make_stub("QHBoxLayout")
    QLabel = _make_stub("QLabel")
    QLineEdit = _make_stub("QLineEdit")
    QTextEdit = _make_stub("QTextEdit")
    QPushButton = _make_stub("QPushButton")
    QGraphicsView = _make_stub("QGraphicsView")
    QGraphicsScene = _make_stub("QGraphicsScene")
    QGraphicsItem = _make_stub("QGraphicsItem")
    QGraphicsPathItem = _make_stub("QGraphicsPathItem")
    QMenu = _make_stub("QMenu")
    QInputDialog = _make_stub("QInputDialog")
    QMessageBox = _make_stub("QMessageBox")
    QSplitter = _make_stub("QSplitter")
    QStatusBar = _make_stub("QStatusBar")
    QToolBar = _make_stub("QToolBar")
    QListWidget = _make_stub("QListWidget")
    QListWidgetItem = _make_stub("QListWidgetItem")
    QComboBox = _make_stub("QComboBox")
    QBrush = _make_stub("QBrush")
    Qt = _make_stub("Qt")
    QRectF = _make_stub("QRectF")
    QPointF = _make_stub("QPointF")
    QTimer = _make_stub("QTimer")
    QPainter = _make_stub("QPainter")
    QColor = _make_stub("QColor")
    QPen = _make_stub("QPen")
    QFont = _make_stub("QFont")
    QAction = _make_stub("QAction")
    QPainterPath = _make_stub("QPainterPath")


class NodeItem(QGraphicsItem):
    """流程图节点的自定义图形项。"""

    WIDTH = 160
    HEIGHT = 70

    def __init__(self, node: Dict, on_click=None, on_move=None, on_conn_click=None,
                 on_conn_move=None, on_conn_end=None):
        """根据节点数据构造图形项；on_click/on_move 为外部回调。"""
        super().__init__()
        self.node = node
        self.on_click = on_click
        self.on_move = on_move
        self.on_conn_click = on_conn_click   # 从连接点按下时回调 (self, scene_pos)
        self.on_conn_move = on_conn_move     # 连接拖动中回调 (scene_pos)
        self.on_conn_end = on_conn_end       # 连接释放时回调 (scene_pos)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable, True)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, True)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemSendsScenePositionChanges, True)
        self.setAcceptHoverEvents(True)  # 接收 hover 以高亮连接点
        self.setPos(node.get("x", 0), node.get("y", 0))
        self._dragging = False
        self._connecting = False  # 是否处于"正在从连接点拖出连线"状态
        self._hover_conn = False  # 鼠标是否悬停在连接点上

    # ---- 连接点几何（节点右侧） ----
    CONN_OFFSET = 0  # 连接点相对节点右边界的水平偏移
    CONN_RADIUS = 6

    def connection_point(self) -> QPointF:
        """返回连接点在节点本地坐标系的位置（中心点）。"""
        return QPointF(self.WIDTH / 2 + self.CONN_OFFSET, 0)

    def connection_point_scene(self) -> QPointF:
        """返回连接点在 scene 坐标系中的位置。"""
        return self.mapToScene(self.connection_point())

    def _is_on_connection_point(self, local_pos: QPointF) -> bool:
        """判断给定的节点本地坐标是否在连接点圆形区域内。"""
        cp = self.connection_point()
        dx = local_pos.x() - cp.x()
        dy = local_pos.y() - cp.y()
        return (dx * dx + dy * dy) <= (self.CONN_RADIUS + 4) ** 2

    def boundingRect(self) -> QRectF:
        """返回节点的边界矩形（外扩以容纳连接点）。"""
        m = self.CONN_RADIUS + 6
        return QRectF(-self.WIDTH / 2 - m, -self.HEIGHT / 2 - m,
                      self.WIDTH + m * 2, self.HEIGHT + m * 2)

    def paint(self, painter: QPainter, option, widget=None) -> None:
        """绘制节点：圆角矩形 + 编号 + 标题 + 状态。"""
        status = self.node.get("status", "pending")
        is_current = (self.data_ref and self.data_ref.data.get("current_node") == self.node["id"])

        # 根据状态选填充色
        if status == "completed":
            fill = QColor(COLOR_NODE_DONE)
        elif status == "in_progress":
            fill = QColor(COLOR_NODE_RUNNING)
        else:
            fill = QColor(COLOR_NODE_PENDING)

        # 当前节点用更粗的描边和亮色边框
        if is_current:
            pen = QPen(QColor(COLOR_NODE_CURRENT), 3)
        elif self.isSelected():
            pen = QPen(QColor("#ffffff"), 2)
        else:
            pen = QPen(QColor("#888"), 1)

        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setBrush(QBrush(fill))
        painter.setPen(pen)
        path = QPainterPath()
        path.addRoundedRect(self.boundingRect(), 8, 8)
        painter.drawPath(path)

        # 绘制编号
        painter.setPen(QColor(COLOR_NODE_CURRENT))
        painter.setFont(QFont("Microsoft YaHei", 9, QFont.Weight.Bold))
        painter.drawText(QRectF(-self.WIDTH / 2 + 6, -self.HEIGHT / 2 + 4,
                                30, 18), Qt.AlignmentFlag.AlignLeft, f"#{self.node['id']}")

        # 绘制标题
        painter.setPen(QColor(COLOR_TEXT))
        painter.setFont(QFont("Microsoft YaHei", 10, QFont.Weight.Bold))
        title_rect = QRectF(-self.WIDTH / 2 + 6, -self.HEIGHT / 2 + 22,
                            self.WIDTH - 12, 24)
        painter.drawText(title_rect, Qt.AlignmentFlag.AlignLeft | Qt.TextFlag.TextWordWrap,
                         self.node.get("title", ""))

        # 状态文字
        status_cn = {"pending": "待开始", "in_progress": "进行中", "completed": "已完成"}.get(status, status)
        painter.setFont(QFont("Microsoft YaHei", 8))
        painter.setPen(QColor("#cbd5e1"))
        painter.drawText(QRectF(-self.WIDTH / 2 + 6, self.HEIGHT / 2 - 22,
                                self.WIDTH - 12, 18),
                         Qt.AlignmentFlag.AlignLeft, f"● {status_cn}")

        # ---- 绘制连接点（节点右侧小圆点）----
        cp = self.connection_point()
        # 外圈白底（增强对比）
        painter.setBrush(QBrush(QColor("#ffffff")))
        painter.setPen(QPen(QColor(COLOR_ACCENT if not self._hover_conn else COLOR_NODE_CURRENT), 2))
        painter.drawEllipse(cp, self.CONN_RADIUS + 1, self.CONN_RADIUS + 1)
        # 内圈实心
        painter.setBrush(QBrush(QColor(COLOR_ACCENT if not self._hover_conn else COLOR_NODE_CURRENT)))
        painter.setPen(QPen(Qt.PenStyle.NoPen))
        painter.drawEllipse(cp, self.CONN_RADIUS - 1, self.CONN_RADIUS - 1)

    def hoverEnterEvent(self, event) -> None:
        """鼠标进入节点：判断是否进入连接点区域，决定是否高亮。"""
        self._hover_conn = self._is_on_connection_point(event.pos())
        if self._hover_conn:
            self.setCursor(Qt.CursorShape.CrossCursor)
        self.update()

    def hoverLeaveEvent(self, event) -> None:
        """鼠标离开节点：取消连接点高亮。"""
        self._hover_conn = False
        self.unsetCursor()
        self.update()

    def hoverMoveEvent(self, event) -> None:
        """鼠标在节点上移动：动态更新连接点高亮状态。"""
        new_hover = self._is_on_connection_point(event.pos())
        if new_hover != self._hover_conn:
            self._hover_conn = new_hover
            self.setCursor(Qt.CursorShape.CrossCursor if new_hover else Qt.CursorShape.ArrowCursor)
            self.update()

    def mousePressEvent(self, event) -> None:
        """鼠标按下：左键在连接点上 → 开始连线；左键在节点上 → 拖动；右键交给画布。"""
        if event.button() == Qt.MouseButton.LeftButton:
            if self._is_on_connection_point(event.pos()):
                # 进入连线模式（不调用 super，避免进入节点拖动）
                self._connecting = True
                if self.on_conn_click:
                    self.on_conn_click(self, event.scenePos())
                event.accept()
                return
            super().mousePressEvent(event)
            if self.on_click:
                self.on_click(self.node["id"])
        else:
            # 右键 / 中键：交给 FlowView 处理（pan）
            event.ignore()

    def mouseMoveEvent(self, event) -> None:
        """鼠标移动：连线模式下更新临时线。"""
        if self._connecting:
            if self.on_conn_move:
                self.on_conn_move(event.scenePos())
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event) -> None:
        """鼠标释放：连线模式下完成连线。"""
        if self._connecting and event.button() == Qt.MouseButton.LeftButton:
            self._connecting = False
            if self.on_conn_end:
                self.on_conn_end(event.scenePos())
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def mouseDoubleClickEvent(self, event) -> None:
        """双击节点：弹出操作菜单。"""
        if event.button() == Qt.MouseButton.LeftButton:
            view = self.scene().views()[0]
            if hasattr(view, "show_node_menu"):
                view.show_node_menu(self.node["id"], event.screenPos())
        else:
            super().mouseDoubleClickEvent(event)

    def itemChange(self, change, value):
        """节点位置变化时通过回调通知外部。"""
        if change == QGraphicsItem.GraphicsItemChange.ItemPositionChange:
            new_pos = value
            if self.on_move:
                self.on_move(self.node["id"], new_pos.x(), new_pos.y())
        return super().itemChange(change, value)



class FlowView(QGraphicsView):
    """流程图画布，支持滚轮缩放和右键 pan。"""

    def __init__(self, scene: QGraphicsScene, main_window):
        """初始化画布视图。"""
        super().__init__(scene)
        self.main_window = main_window
        self.setRenderHint(QPainter.RenderHint.Antialiasing)
        self.setDragMode(QGraphicsView.DragMode.RubberBandDrag)
        self.setBackgroundBrush(QColor(COLOR_BG))
        self.setMouseTracking(True)  # 让 move 事件持续触发
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.setTransformationAnchor(QGraphicsView.ViewportAnchor.AnchorUnderMouse)
        # 禁用默认 context menu (我们用双击节点弹自定义菜单)
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.NoContextMenu)
        self.setAcceptDrops(False)
        self._scale = 1.0
        # pan 状态
        self._panning = False
        self._pan_start = None
        self._pan_pressed_button = None

    def wheelEvent(self, event) -> None:
        """鼠标滚轮缩放画布。"""
        factor = 1.15 if event.angleDelta().y() > 0 else 1 / 1.15
        self._scale *= factor
        self.scale(factor, factor)

    def mousePressEvent(self, event) -> None:
        """鼠标按下：左键走默认（节点拖动 / 框选），右键 / 中键进入 pan 模式。"""
        if event.button() in (Qt.MouseButton.RightButton, Qt.MouseButton.MiddleButton):
            self._panning = True
            self._pan_start = event.position().toPoint()
            self._pan_pressed_button = event.button()
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            self.viewport().grabMouse()  # 确保即使移出节点也能持续收到 move
            if self.main_window and self.main_window.statusBar():
                self.main_window.statusBar().showMessage("✋ Pan 模式 (拖动即可, 释放退出)", 0)
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        """鼠标移动：pan 模式下调整滚动条；其他走默认。"""
        if self._panning:
            pos = event.position().toPoint()
            delta = pos - self._pan_start
            self._pan_start = pos
            hbar = self.horizontalScrollBar()
            vbar = self.verticalScrollBar()
            hbar.setValue(hbar.value() - delta.x())
            vbar.setValue(vbar.value() - delta.y())
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event) -> None:
        """鼠标释放：退出 pan 模式。"""
        if self._panning and event.button() == self._pan_pressed_button:
            self._panning = False
            self._pan_start = None
            self._pan_pressed_button = None
            self.setCursor(Qt.CursorShape.ArrowCursor)
            self.viewport().releaseMouse()
            if self.main_window and self.main_window.statusBar():
                self.main_window.statusBar().showMessage(
                    "就绪 · 左键拖节点 · 右键 pan · 滚轮缩放 · 🎯 居中 / 🔍 查找节点 (C / Ctrl+F)",
                    5000
                )
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def show_node_menu(self, node_id: str, global_pos) -> None:
        """在鼠标位置弹出节点操作菜单（双击节点时调用）。"""
        menu = QMenu(self)
        act_set_current = menu.addAction("设为当前步骤")
        act_edit = menu.addAction("编辑")
        act_delete = menu.addAction("删除")
        action = menu.exec(global_pos)
        if action == act_set_current:
            self.main_window.set_current_node(node_id)
        elif action == act_edit:
            self.main_window.edit_node(node_id)
        elif action == act_delete:
            self.main_window.delete_node(node_id)



class MainWindow(QMainWindow):
    """主窗口：顶部主目标 + 中部画布 + 右侧详情 + 工具栏。"""

    def __init__(self):
        """初始化主窗口并加载数据。"""
        super().__init__()
        self.step_data = StepData()
        self.node_items: Dict[str, NodeItem] = {}
        self.edge_items: Dict[str, QGraphicsPathItem] = {}  # key: "from->to"
        self._temp_edge: Optional[QGraphicsPathItem] = None  # 拖动时的临时连线
        self._conn_from: Optional[NodeItem] = None  # 连线源节点
        self.setWindowTitle("步骤跟踪工具 - Step Tracker")
        self.resize(1280, 800)
        self.setStyleSheet(f"""
            QMainWindow {{ background: {COLOR_BG}; }}
            QLabel {{ color: {COLOR_TEXT}; }}
            QPushButton {{
                background: {COLOR_ACCENT}; color: white; border: none;
                padding: 6px 12px; border-radius: 4px;
            }}
            QPushButton:hover {{ background: #14b8a6; }}
            QLineEdit, QTextEdit {{
                background: {COLOR_PANEL}; color: {COLOR_TEXT};
                border: 1px solid #444; border-radius: 4px; padding: 4px;
            }}
            QToolBar {{ background: {COLOR_PANEL}; border: none; }}
            QStatusBar {{ background: {COLOR_PANEL}; color: {COLOR_TEXT}; }}
        """)

        self._build_ui()
        self.render_all()

    # ---- UI 构建 ----
    def _build_ui(self) -> None:
        """构建界面布局。"""
        # 顶部主目标栏
        top = QWidget()
        top_layout = QHBoxLayout(top)
        top_layout.setContentsMargins(10, 8, 10, 8)
        lbl = QLabel("🎯 主目标:")
        lbl.setFont(QFont("Microsoft YaHei", 11, QFont.Weight.Bold))
        top_layout.addWidget(lbl)
        self.goal_edit = QLineEdit()
        self.goal_edit.setPlaceholderText("在此输入或编辑主目标...")
        self.goal_edit.returnPressed.connect(self.save_goal)
        top_layout.addWidget(self.goal_edit, 1)
        btn_save = QPushButton("保存主目标")
        btn_save.clicked.connect(self.save_goal)
        top_layout.addWidget(btn_save)

        # 工具栏
        tb = QToolBar()
        self.addToolBar(tb)
        tb.addAction("➕ 添加节点", self.add_node)
        tb.addAction("✏️ 编辑当前选中", self.edit_selected)
        tb.addAction("▶ 设为当前", self.set_selected_current)
        tb.addAction("🗑 删除", self.delete_selected)
        tb.addSeparator()
        tb.addAction("🎯 居中", self.center_view)            # C
        tb.addAction("🔍 查找节点", self.find_node)           # Ctrl+F
        tb.addSeparator()
        tb.addAction("🔄 刷新", self.render_all)
        tb.addAction("📋 命令行用法", self.show_help)

        # 快捷键: 居中 / 查找
        from _qt_compat import QShortcut, QKeySequence
        QShortcut(QKeySequence("C"), self, activated=self.center_view)
        QShortcut(QKeySequence("Ctrl+F"), self, activated=self.find_node)
        QShortcut(QKeySequence("Home"), self, activated=self.fit_all_nodes)

        # 主分割：左画布 / 右详情
        splitter = QSplitter(Qt.Orientation.Horizontal)

        # 画布
        self.scene = QGraphicsScene()
        self.scene.setSceneRect(-2000, -2000, 4000, 4000)
        self.view = FlowView(self.scene, self)
        splitter.addWidget(self.view)

        # 右侧详情 + 事件日志 (上下分割)
        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.setContentsMargins(8, 8, 8, 8)

        # 详情区域
        self.detail_title = QLabel("请选中一个节点")
        self.detail_title.setFont(QFont("Microsoft YaHei", 14, QFont.Weight.Bold))
        right_layout.addWidget(self.detail_title)

        self.detail_id = QLabel("")
        self.detail_id.setStyleSheet("color: #fbbf24;")
        right_layout.addWidget(self.detail_id)

        self.detail_status = QLabel("")
        right_layout.addWidget(self.detail_status)

        right_layout.addWidget(QLabel("描述:"))
        self.detail_desc = QTextEdit()
        self.detail_desc.setReadOnly(True)
        right_layout.addWidget(self.detail_desc, 1)

        self.detail_next = QLabel("")
        self.detail_next.setWordWrap(True)
        right_layout.addWidget(self.detail_next)

        # 分割: 详情 / 事件日志
        right_splitter = QSplitter(Qt.Orientation.Vertical)
        # 包一层 widget, 让 detail + next 一起放进上半部
        top_right = QWidget()
        top_right_layout = QVBoxLayout(top_right)
        top_right_layout.setContentsMargins(0, 0, 0, 0)
        # 把 right_layout 中已加的 detail 控件先 "搬" 到 top_right
        # 简单做法: 重新建一遍 (保留引用)
        top_right_layout.addWidget(self.detail_title)
        top_right_layout.addWidget(self.detail_id)
        top_right_layout.addWidget(self.detail_status)
        desc_lbl = QLabel("描述:")
        top_right_layout.addWidget(desc_lbl)
        top_right_layout.addWidget(self.detail_desc, 1)
        top_right_layout.addWidget(self.detail_next)
        # 清空 right_layout
        while right_layout.count():
            item = right_layout.takeAt(0)
            if item.widget():
                item.widget().setParent(None)
        right_splitter.addWidget(top_right)

        # 事件日志面板
        ev_panel = QWidget()
        ev_layout = QVBoxLayout(ev_panel)
        ev_layout.setContentsMargins(0, 4, 0, 0)
        ev_layout.setSpacing(4)

        ev_header = QHBoxLayout()
        ev_header.addWidget(QLabel("📜 事件日志:"))
        self.event_filter_combo = QComboBox()
        self.event_filter_combo.addItem("全部")
        self.event_filter_combo.addItems([
            "compile_start", "compile_finish", "compile_fail",
            "clean", "deep_clean", "touch", "diagnose", "launch",
            "info", "error"
        ])
        self.event_filter_combo.currentIndexChanged.connect(self._refresh_events)
        ev_header.addWidget(self.event_filter_combo, 1)
        btn_clear_ev = QPushButton("🧹 清空")
        btn_clear_ev.clicked.connect(self._clear_events)
        ev_header.addWidget(btn_clear_ev)
        ev_layout.addLayout(ev_header)

        self.event_list = QListWidget()
        self.event_list.setFont(QFont("Consolas", 9))
        ev_layout.addWidget(self.event_list, 1)

        right_splitter.addWidget(ev_panel)
        right_splitter.setSizes([350, 250])

        right.setStyleSheet(f"background: {COLOR_PANEL};")
        right_layout.addWidget(right_splitter)

        splitter.addWidget(right)
        splitter.setSizes([820, 460])

        # 启动定时器: 每 2 秒刷一次事件 (其他工具在写, 这里轮询拉)
        self._event_timer = QTimer(self)
        self._event_timer.timeout.connect(self._refresh_events)
        self._event_timer.start(2000)
        # 记录上次事件 id, 只在新增时追加
        self._last_event_id = 0
        # 初次填充
        self._refresh_events()

        # 总布局
        central = QWidget()
        v = QVBoxLayout(central)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(0)
        v.addWidget(top)
        v.addWidget(splitter, 1)
        self.setCentralWidget(central)

        self.setStatusBar(QStatusBar())
        self.statusBar().showMessage("就绪 · 左键拖节点 · 右侧小圆点拖出连线 · 右键 pan · 双击节点弹菜单 · 滚轮缩放")

    # ---- 数据渲染 ----
    def render_all(self) -> None:
        """重新渲染整个流程图。"""
        self._clear_edges()
        self.scene.clear()
        self.node_items.clear()

        # 设置主目标
        self.goal_edit.blockSignals(True)
        self.goal_edit.setText(self.step_data.data.get("main_goal", ""))
        self.goal_edit.blockSignals(False)

        # 创建所有节点
        for node in self.step_data.list_nodes():
            item = NodeItem(node,
                            on_click=self.on_node_clicked,
                            on_move=self.on_node_moved,
                            on_conn_click=self.on_connection_start,
                            on_conn_move=self.update_connection_line,
                            on_conn_end=self.finish_connection)
            item.data_ref = self.step_data  # 用于 paint 中判断 current
            self.scene.addItem(item)
            self.node_items[node["id"]] = item

        # 绘制边
        self._draw_edges()

    def _clear_edges(self) -> None:
        """移除所有边图形项（节点和临时线保留）。"""
        for item in list(self.edge_items.values()):
            if item.scene() is self.scene:
                self.scene.removeItem(item)
        self.edge_items.clear()

    def _draw_edges(self) -> None:
        """根据 next 关系绘制节点之间的连线（每次重绘前先清空旧的）。"""
        self._clear_edges()
        for node in self.step_data.list_nodes():
            from_item = self.node_items.get(node["id"])
            if not from_item:
                continue
            for nid in node.get("next", []):
                to_item = self.node_items.get(nid)
                if not to_item:
                    continue
                edge = self._build_edge(from_item, to_item)
                self.edge_items[f"{node['id']}->{nid}"] = edge
                self.scene.addItem(edge)

    def _build_edge(self, from_item: NodeItem, to_item: NodeItem) -> QGraphicsPathItem:
        """构造一条从 from 到 to 的边图形项（直线 + 正确方向箭头）。"""
        p1 = from_item.connection_point_scene()  # 起点用 from 的连接点
        p2 = to_item.pos()                       # 终点用 to 的中心
        dx = p2.x() - p1.x()
        dy = p2.y() - p1.y()
        length = max((dx * dx + dy * dy) ** 0.5, 1)
        ux, uy = dx / length, dy / length

        # 终点裁剪到 to 节点矩形边界（避免线穿进节点）
        end = self._clip_to_rect(p2, -ux, -uy, to_item)

        # 状态色
        is_active = self.step_data.data.get("current_node") == from_item.node["id"]
        color = QColor(COLOR_EDGE_ACTIVE if is_active else COLOR_EDGE)
        pen = QPen(color, 2)

        # 构造路径：主线 + 箭头（在同一个 path 中）
        path = QPainterPath(p1)
        path.lineTo(end)
        # 箭头
        import math
        ang = math.atan2(uy, ux)
        size = 9
        spread = 0.45  # 弧度
        a1x = end.x() - size * math.cos(ang - spread)
        a1y = end.y() - size * math.sin(ang - spread)
        a2x = end.x() - size * math.cos(ang + spread)
        a2y = end.y() - size * math.sin(ang + spread)
        path.moveTo(a1x, a1y)
        path.lineTo(end)
        path.lineTo(a2x, a2y)

        item = QGraphicsPathItem(path)
        item.setPen(pen)
        item.setBrush(QBrush(color))  # 箭头填充
        item.setZValue(-1)            # 节点在边上
        return item

    @staticmethod
    def _clip_to_rect(center: QPointF, dx: float, dy: float, node: NodeItem) -> QPointF:
        """给定入射方向（归一化的），把 center 沿反方向裁剪到节点矩形边界。"""
        # 反向：从中心向节点外走到边界
        inv_x, inv_y = -dx, -dy
        local = node.mapFromScene(center)
        t_candidates = []
        if inv_x != 0:
            for edge_x in (-node.WIDTH / 2, node.WIDTH / 2):
                t = (edge_x - local.x()) / inv_x
                if t > 0:
                    t_candidates.append(t)
        if inv_y != 0:
            for edge_y in (-node.HEIGHT / 2, node.HEIGHT / 2):
                t = (edge_y - local.y()) / inv_y
                if t > 0:
                    t_candidates.append(t)
        if not t_candidates:
            return center
        t_min = min(t_candidates)
        return QPointF(center.x() + inv_x * t_min, center.y() + inv_y * t_min)

    # ---- 手动连线（拖拽连接点） ----
    def on_connection_start(self, from_node: NodeItem, scene_pos: QPointF) -> None:
        """用户按下节点的连接点：创建临时虚线，从连接点延伸到鼠标位置。"""
        self._conn_from = from_node
        self._temp_edge = QGraphicsPathItem()
        pen = QPen(QColor(COLOR_ACCENT), 2)
        pen.setStyle(Qt.PenStyle.DashLine)
        self._temp_edge.setPen(pen)
        self._temp_edge.setBrush(QBrush(QColor(COLOR_ACCENT)))
        self._temp_edge.setZValue(0)  # 在节点上方
        self.scene.addItem(self._temp_edge)
        self._temp_edge_last_pos = scene_pos
        self._update_temp_edge(scene_pos)
        self.statusBar().showMessage(f"🔗 正在从 #{from_node.node['id']} 拖出连线… 释放到目标节点完成连接", 0)

    def update_connection_line(self, scene_pos: QPointF) -> None:
        """鼠标在连接过程中移动：更新临时线。"""
        if self._temp_edge and self._conn_from:
            self._temp_edge_last_pos = scene_pos
            self._update_temp_edge(scene_pos)

    def _update_temp_edge(self, scene_pos: QPointF) -> None:
        """根据当前源节点 + 鼠标位置更新临时线（含箭头）。"""
        if not (self._temp_edge and self._conn_from):
            return
        p1 = self._conn_from.connection_point_scene()
        dx = scene_pos.x() - p1.x()
        dy = scene_pos.y() - p1.y()
        length = max((dx * dx + dy * dy) ** 0.5, 1)
        ux, uy = dx / length, dy / length
        import math
        ang = math.atan2(uy, ux)
        size = 9
        spread = 0.45
        a1x = scene_pos.x() - size * math.cos(ang - spread)
        a1y = scene_pos.y() - size * math.sin(ang - spread)
        a2x = scene_pos.x() - size * math.cos(ang + spread)
        a2y = scene_pos.y() - size * math.sin(ang + spread)
        path = QPainterPath(p1)
        path.lineTo(scene_pos)
        path.moveTo(a1x, a1y)
        path.lineTo(scene_pos)
        path.lineTo(a2x, a2y)
        self._temp_edge.setPath(path)

    def finish_connection(self, scene_pos: QPointF) -> None:
        """用户释放鼠标：判断是否落在某个节点上，落在则添加边。"""
        if not (self._conn_from and self._temp_edge):
            self._cancel_connection()
            return
        # 移除临时线
        if self._temp_edge.scene() is self.scene:
            self.scene.removeItem(self._temp_edge)
        self._temp_edge = None

        # 查找目标节点
        target_item = self._node_at_scene_pos(scene_pos)
        src_id = self._conn_from.node["id"]
        self._conn_from = None

        if not isinstance(target_item, NodeItem):
            self.statusBar().showMessage("已取消连线（未落到节点上）", 3000)
            return
        if target_item.node["id"] == src_id:
            self.statusBar().showMessage("已取消连线（不能连自己）", 3000)
            return
        dst_id = target_item.node["id"]

        # 检查是否已存在
        src_node = self.step_data.get_node(src_id)
        if not src_node:
            return
        if dst_id in src_node.get("next", []):
            self.statusBar().showMessage(f"已存在 #{src_id} → #{dst_id} 的连线", 3000)
            return

        # 添加边
        new_next = list(src_node.get("next", [])) + [dst_id]
        self.step_data.update_node(src_id, next=new_next)
        self.render_all()
        self.statusBar().showMessage(f"✓ 已添加连线 #{src_id} → #{dst_id}", 3000)

    def _cancel_connection(self) -> None:
        """取消当前连线（兜底）。"""
        if self._temp_edge and self._temp_edge.scene() is self.scene:
            self.scene.removeItem(self._temp_edge)
        self._temp_edge = None
        self._conn_from = None

    def _node_at_scene_pos(self, scene_pos: QPointF) -> Optional[QGraphicsItem]:
        """查找 scene_pos 处最上层的 NodeItem。"""
        for item in self.scene.items(scene_pos):
            if isinstance(item, NodeItem):
                return item
        return None

    # ---- 事件回调 ----
    def save_goal(self) -> None:
        """保存主目标。"""
        self.step_data.set_main_goal(self.goal_edit.text())
        self.statusBar().showMessage("✓ 主目标已保存", 3000)

    def on_node_clicked(self, node_id: str) -> None:
        """节点被点击：显示详情。"""
        node = self.step_data.get_node(node_id)
        if not node:
            return
        self.detail_title.setText(node.get("title", ""))
        self.detail_id.setText(f"编号: #{node['id']}")
        status_cn = {"pending": "待开始", "in_progress": "进行中", "completed": "已完成"}.get(
            node.get("status", "pending"), node.get("status"))
        self.detail_status.setText(f"状态: {status_cn}")
        self.detail_desc.setPlainText(node.get("description", "（无描述）"))
        nxt = node.get("next", [])
        self.detail_next.setText("后续步骤: " + (", ".join(f"#{n}" for n in nxt) if nxt else "（无）"))

    def on_node_moved(self, node_id: str, x: float, y: float) -> None:
        """节点被拖动：保存新坐标 + 只重绘相关连线（不重建节点，避免闪烁）。"""
        self.step_data.update_node(node_id, x=x, y=y)
        # 更新所有涉及该节点的边的 path
        for key, edge in list(self.edge_items.items()):
            src, dst = key.split("->")
            if src == node_id or dst == node_id:
                from_item = self.node_items.get(src)
                to_item = self.node_items.get(dst)
                if from_item and to_item:
                    new_edge = self._build_edge(from_item, to_item)
                    edge.setPath(new_edge.path())
        # 同时更新临时线（如果拖的是连接源节点）
        if self._conn_from and self._conn_from.node["id"] == node_id and self._temp_edge:
            last = getattr(self, "_temp_edge_last_pos", self._conn_from.connection_point_scene())
            self._update_temp_edge(last)

    # ---- 节点操作 ----
    def _ask_node_id(self, title: str, default: str = "") -> Optional[str]:
        """弹窗询问节点编号。"""
        text, ok = QInputDialog.getText(self, title, "节点编号（字符串，例如 1, 1.1, A）:", text=default)
        return text if ok and text.strip() else None

    def add_node(self) -> None:
        """添加一个新节点：弹窗输入 id 和 title。"""
        nid = self._ask_node_id("添加节点")
        if not nid:
            return
        if self.step_data.get_node(nid):
            QMessageBox.warning(self, "错误", f"节点 #{nid} 已存在")
            return
        title, ok = QInputDialog.getText(self, "添加节点", "节点标题:")
        if not ok or not title.strip():
            return
        desc, _ = QInputDialog.getMultiLineText(self, "添加节点", "详细描述（可留空）:", "")
        # 默认放在画布中心
        view_center = self.view.mapToScene(self.view.viewport().rect().center())
        self.step_data.add_node(nid, title.strip(), desc.strip(),
                                x=view_center.x(), y=view_center.y())
        self.render_all()
        self.statusBar().showMessage(f"✓ 已添加节点 #{nid}", 3000)

    def edit_node(self, node_id: str) -> None:
        """编辑指定节点的标题和描述。"""
        node = self.step_data.get_node(node_id)
        if not node:
            return
        title, ok = QInputDialog.getText(self, "编辑节点", "标题:", text=node.get("title", ""))
        if not ok:
            return
        desc, _ = QInputDialog.getMultiLineText(self, "编辑节点", "描述:", text=node.get("description", ""))
        status, ok2 = QInputDialog.getItem(self, "编辑节点", "状态:",
                                          ["pending", "in_progress", "completed"],
                                          ["pending", "in_progress", "completed"].index(
                                              node.get("status", "pending")), False)
        if not ok2:
            return
        nxt_text, _ = QInputDialog.getText(self, "编辑节点",
                                           "后续节点编号（英文逗号分隔，留空表示无）:",
                                           text=",".join(node.get("next", [])))
        next_list = [s.strip() for s in nxt_text.split(",") if s.strip()]
        self.step_data.update_node(node_id, title=title, description=desc,
                                   status=status, next=next_list)
        self.render_all()
        self.statusBar().showMessage(f"✓ 节点 #{node_id} 已更新", 3000)

    def edit_selected(self) -> None:
        """编辑当前选中的节点。"""
        for item in self.scene.selectedItems():
            if isinstance(item, NodeItem):
                self.edit_node(item.node["id"])
                return
        QMessageBox.information(self, "提示", "请先在画布上选中一个节点")

    def set_current_node(self, node_id: str) -> None:
        """设置当前节点。"""
        self.step_data.set_current(node_id)
        self.render_all()
        self.on_node_clicked(node_id)
        self.statusBar().showMessage(f"▶ 当前节点已设为 #{node_id}", 3000)

    def set_selected_current(self) -> None:
        """把当前选中节点设为当前步骤。"""
        for item in self.scene.selectedItems():
            if isinstance(item, NodeItem):
                self.set_current_node(item.node["id"])
                return
        QMessageBox.information(self, "提示", "请先在画布上选中一个节点")

    def delete_node(self, node_id: str) -> None:
        """删除指定节点（带确认）。"""
        ret = QMessageBox.question(self, "确认", f"确定删除节点 #{node_id} 吗？")
        if ret == QMessageBox.StandardButton.Yes:
            self.step_data.delete_node(node_id)
            self.render_all()
            self.statusBar().showMessage(f"🗑 已删除节点 #{node_id}", 3000)

    def delete_selected(self) -> None:
        """删除当前选中节点。"""
        for item in self.scene.selectedItems():
            if isinstance(item, NodeItem):
                self.delete_node(item.node["id"])
                return
        QMessageBox.information(self, "提示", "请先在画布上选中一个节点")

    # ---- 视图导航 (居中 / 查找) ----
    def center_on_node(self, node_id: str, animate: bool = True) -> bool:
        """将视图居中到指定节点。返回是否成功。"""
        item = self.node_items.get(node_id)
        if not item:
            self.statusBar().showMessage(f"⚠ 找不到节点 #{node_id}", 3000)
            return False
        # 节点中心 (scene 坐标)
        center = item.scenePos()
        if animate:
            # 用 transform 动画平滑滚到目标
            self._animate_center_to(center)
        else:
            self.view.centerOn(center)
        # 选中并确保可见
        self.scene.clearSelection()
        item.setSelected(True)
        self.on_node_clicked(node_id)
        return True

    def _animate_center_to(self, scene_pos: QPointF, duration_ms: int = 280) -> None:
        """平滑居中动画 (用 QTimer 插值 60 帧)."""
        from _qt_compat import QTimer
        # 拿当前 view 中心
        cur = self.view.mapToScene(self.view.viewport().rect().center())
        end = scene_pos
        steps = max(1, duration_ms // 16)
        step_n = [0]

        def tick():
            step_n[0] += 1
            t = min(1.0, step_n[0] / steps)
            # ease-out
            t2 = 1 - (1 - t) ** 2
            x = cur.x() + (end.x() - cur.x()) * t2
            y = cur.y() + (end.y() - cur.y()) * t2
            self.view.centerOn(x, y)
            if t >= 1.0:
                timer.stop()

        timer = QTimer(self)
        timer.timeout.connect(tick)
        timer.start(16)

    def center_view(self) -> None:
        """🎯 居中: 优先选中节点 → 当前节点 → 所有节点。"""
        # 1. 选中的节点
        for item in self.scene.selectedItems():
            if isinstance(item, NodeItem):
                self.center_on_node(item.node["id"])
                return
        # 2. 当前节点
        cur = self.step_data.get_current()
        if cur and self.center_on_node(cur):
            return
        # 3. 所有节点 (fit)
        self.fit_all_nodes()

    def fit_all_nodes(self) -> None:
        """把所有节点都框进视图 (Home 键)."""
        if not self.node_items:
            self.statusBar().showMessage("画布为空", 3000)
            return
        rect = self.scene.itemsBoundingRect().adjusted(-60, -60, 60, 60)
        self.view.fitInView(rect, Qt.AspectRatioMode.KeepAspectRatio)
        self.statusBar().showMessage(f"📐 已显示全部 {len(self.node_items)} 个节点", 3000)

    def find_node(self) -> None:
        """🔍 查找节点 (按 id / 标题模糊搜索), 回车跳转。"""
        items = list(self.node_items.keys())
        if not items:
            QMessageBox.information(self, "提示", "画布上没有节点")
            return
        # 准备候选项 (id + title)
        candidates = []
        for nid, item in self.node_items.items():
            title = item.node.get("title", "")
            candidates.append((nid, title))
            candidates.append((f"#{nid} {title}", nid))
        # 用 QInputDialog 拿输入
        from _qt_compat import QInputDialog
        query, ok = QInputDialog.getText(
            self, "查找节点", "输入节点 id 或标题 (模糊匹配):",
            text=""
        )
        if not ok or not query.strip():
            return
        query = query.strip().lower()
        # 优先精确匹配 id
        if query in self.node_items:
            self.center_on_node(query)
            return
        # 模糊匹配 id 或 title
        for nid, title in self.node_items.items():
            if query in nid.lower() or query in title.lower():
                self.center_on_node(nid)
                return
        self.statusBar().showMessage(f"⚠ 没找到匹配 \"{query}\" 的节点", 4000)

    def show_help(self) -> None:
        """显示命令行用法帮助。"""
        QMessageBox.information(self, "命令行用法",
            "AI 可通过命令行更新步骤：\n\n"
            "  python step_tracker.py set-goal \"目标\"\n"
            "  python step_tracker.py add --id 1 --title \"xxx\" --desc \"...\" --next 2,3\n"
            "  python step_tracker.py update --id 1 --status in_progress\n"
            "  python step_tracker.py current 1\n"
            "  python step_tracker.py delete 1\n"
            "  python step_tracker.py list\n\n"
            "事件日志:\n"
            "  python step_tracker.py events [--limit 50] [--type compile_start]\n"
            "  python step_tracker.py event <type> <title> [--desc ...] [--node ID]\n"
            "  python step_tracker.py clear-events")

    # ---- 事件日志 ----
    def _refresh_events(self) -> None:
        """从 step_data.json 拉取事件, 刷新到事件列表。"""
        # 重新加载数据 (其他进程可能改了)
        self.step_data.load()
        events = list(self.step_data.data.get("events", []))
        # 类型过滤
        ftype = self.event_filter_combo.currentText() if hasattr(self, "event_filter_combo") else "全部"
        if ftype and ftype != "全部":
            events = [e for e in events if e.get("type") == ftype]
        # 新→旧, 限 200 条
        events = list(reversed(events[-200:]))
        # 重建列表 (数量不大, 全量重建简单可靠)
        self.event_list.clear()
        for e in events:
            node_tag = f" [#{e['node_id']}]" if e.get("node_id") else ""
            desc = f"  — {e['desc']}" if e.get("desc") else ""
            text = f"{e['ts']}  [{e['type']}]{node_tag}  {e['title']}{desc}"
            item = QListWidgetItem(text)
            # 按类型上色
            color = {
                "compile_start": "#0d9488",
                "compile_finish": "#10b981",
                "compile_fail": "#ef4444",
                "clean": "#94a3b8",
                "deep_clean": "#a78bfa",
                "touch": "#fbbf24",
                "diagnose": "#f97316",
                "launch": "#3b82f6",
                "error": "#ef4444",
                "info": "#94a3b8",
            }.get(e.get("type"), "#cbd5e1")
            item.setForeground(QColor(color))
            self.event_list.addItem(item)

    def _clear_events(self) -> None:
        """清空事件日志 (带确认)。"""
        ret = QMessageBox.question(self, "确认", "清空所有事件日志?")
        if ret == QMessageBox.StandardButton.Yes:
            self.step_data.data["events"] = []
            self.step_data.save()
            self._refresh_events()
            self.statusBar().showMessage("🧹 事件已清空", 3000)


def run_gui() -> None:
    """启动图形界面 (PySide6 / PyQt5 / PySide2 多后端兼容)."""
    import _qt_compat
    if not _qt_compat.gui_available():
        sys.stderr.write(
            "✗ 没找到任何 Qt 后端 (PySide6 / PyQt5 / PySide2)\n"
            "  安装: pip install --user PySide6  (或 apt install python3-pyqt5)\n"
        )
        sys.exit(1)
    sys.stderr.write(f"[step_tracker] using {_qt_compat.QT_BACKEND}\n")
    # 启动应用
    app = QApplication(sys.argv)
    win = MainWindow()
    win.show()
    sys.exit(getattr(app, _qt_compat.APP_EXEC)())


# ===== CLI 部分 =====
def run_cli(args: List[str]) -> int:
    """处理命令行调用，返回退出码。"""
    parser = argparse.ArgumentParser(
        prog="step_tracker.py",
        description="步骤跟踪工具 CLI - 供 AI 或脚本调用",
    )
    parser.add_argument("--data-file", default=None,
                        help="step_data.json 路径 (默认 /home/bv/code/ai_tools/step_data.json, "
                             "也可用 env TRACKER_DATA_FILE)")
    sub = parser.add_subparsers(dest="cmd")

    # gui
    sub.add_parser("gui", help="启动图形界面")

    # list
    sub.add_parser("list", help="列出所有节点")

    # set-goal
    p_goal = sub.add_parser("set-goal", help="设置主目标")
    p_goal.add_argument("goal", help="主目标内容")

    # add
    p_add = sub.add_parser("add", help="添加节点")
    p_add.add_argument("--id", required=True, help="节点编号")
    p_add.add_argument("--title", required=True, help="标题")
    p_add.add_argument("--desc", default="", help="描述")
    p_add.add_argument("--status", default="pending", choices=["pending", "in_progress", "completed"])
    p_add.add_argument("--x", type=float, default=0, help="X 坐标")
    p_add.add_argument("--y", type=float, default=0, help="Y 坐标")
    p_add.add_argument("--next", default="", help="后续节点编号，逗号分隔（留空或省略表示无）")

    # update
    p_up = sub.add_parser("update", help="更新节点")
    p_up.add_argument("--id", required=True)
    p_up.add_argument("--title", default=None)
    p_up.add_argument("--desc", default=None)
    p_up.add_argument("--status", default=None, choices=["pending", "in_progress", "completed"])
    # --next: --clear-next 表示清空；--next 1,2 表示设置
    p_up.add_argument("--next", default=None, help="覆盖后续节点列表，逗号分隔（不传则保持原值）")
    p_up.add_argument("--clear-next", action="store_true", help="清空后续节点列表")
    p_up.add_argument("--x", type=float, default=None)
    p_up.add_argument("--y", type=float, default=None)

    # delete
    p_del = sub.add_parser("delete", help="删除节点")
    p_del.add_argument("id")

    # current
    p_cur = sub.add_parser("current", help="设置当前节点")
    p_cur.add_argument("id")

    # show
    p_show = sub.add_parser("show", help="查看节点详情")
    p_show.add_argument("id")

    # ===== 事件日志子命令 =====
    p_ev_list = sub.add_parser("events", help="列最近事件 (新→旧)")
    p_ev_list.add_argument("--limit", type=int, default=50)
    p_ev_list.add_argument("--type", default=None)

    p_ev_add = sub.add_parser("event", help="追加一条事件")
    p_ev_add.add_argument("type", help="事件类型")
    p_ev_add.add_argument("title", help="事件标题")
    p_ev_add.add_argument("--desc", default="")
    p_ev_add.add_argument("--node", default=None)

    p_ev_clear = sub.add_parser("clear-events", help="清空事件日志")

    parsed = parser.parse_args(args)

    # 全局: --data-file 覆盖 (优先 CLI > env TRACKER_DATA_FILE > 默认)
    global DATA_FILE
    if parsed.data_file:
        DATA_FILE = Path(parsed.data_file).expanduser().resolve()
    elif os.environ.get("TRACKER_DATA_FILE"):
        DATA_FILE = Path(os.environ["TRACKER_DATA_FILE"]).expanduser().resolve()
    if parsed.data_file or os.environ.get("TRACKER_DATA_FILE"):
        print(f"📁 数据文件: {DATA_FILE}", file=sys.stderr)

    sd = StepData()

    if parsed.cmd in (None, "gui"):
        run_gui()
        return 0

    if parsed.cmd == "list":
        nodes = sd.list_nodes()
        goal = sd.data.get("main_goal", "")
        cur = sd.data.get("current_node")
        evs = sd.data.get("events", [])
        print(f"主目标: {goal or '（未设置）'}")
        print(f"当前节点: #{cur}" if cur else "当前节点: （无）")
        print(f"共 {len(nodes)} 个节点, {len(evs)} 条事件:")
        for n in nodes:
            mark = " ▶" if n["id"] == cur else ""
            print(f"  #{n['id']} [{n['status']}] {n['title']}{mark}")
            if n.get("next"):
                print(f"      next: {', '.join(n['next'])}")
        return 0

    if parsed.cmd == "set-goal":
        sd.set_main_goal(parsed.goal)
        print(f"✓ 主目标已设置为: {parsed.goal}")
        return 0

    if parsed.cmd == "add":
        try:
            nxt_raw = parsed.next or ""
            nxt = [s.strip() for s in nxt_raw.split(",") if s.strip()]
            sd.add_node(parsed.id, parsed.title, parsed.desc,
                        parsed.status, parsed.x, parsed.y, nxt)
            print(f"✓ 已添加节点 #{parsed.id}: {parsed.title}")
        except ValueError as e:
            print(f"✗ 错误: {e}", file=sys.stderr)
            return 1
        return 0

    if parsed.cmd == "update":
        kwargs = {}
        if parsed.title is not None:
            kwargs["title"] = parsed.title
        if parsed.desc is not None:
            kwargs["description"] = parsed.desc
        if parsed.status is not None:
            kwargs["status"] = parsed.status
        if parsed.next is not None:
            kwargs["next"] = [s.strip() for s in parsed.next.split(",") if s.strip()]
        if parsed.clear_next:
            kwargs["next"] = []
        if parsed.x is not None:
            kwargs["x"] = parsed.x
        if parsed.y is not None:
            kwargs["y"] = parsed.y
        try:
            sd.update_node(parsed.id, **kwargs)
            print(f"✓ 节点 #{parsed.id} 已更新")
        except ValueError as e:
            print(f"✗ 错误: {e}", file=sys.stderr)
            return 1
        return 0

    if parsed.cmd == "delete":
        sd.delete_node(parsed.id)
        print(f"🗑 已删除节点 #{parsed.id}")
        return 0

    if parsed.cmd == "current":
        try:
            sd.set_current(parsed.id)
            print(f"▶ 当前节点已设为 #{parsed.id}")
        except ValueError as e:
            print(f"✗ 错误: {e}", file=sys.stderr)
            return 1
        return 0

    if parsed.cmd == "show":
        n = sd.get_node(parsed.id)
        if not n:
            print(f"✗ 节点 #{parsed.id} 不存在", file=sys.stderr)
            return 1
        print(f"#{n['id']}  {n['title']}")
        print(f"  状态: {n['status']}")
        print(f"  描述: {n.get('description', '（无）')}")
        print(f"  后续: {', '.join(n.get('next', [])) or '（无）'}")
        return 0

    if parsed.cmd == "events":
        events = list(sd.data.get("events", []))
        if parsed.type:
            events = [e for e in events if e.get("type") == parsed.type]
        events = list(reversed(events[-parsed.limit:]))
        print(f"共 {len(events)} 条事件 (新→旧):")
        for e in events:
            node = f" [#{e['node_id']}]" if e.get("node_id") else ""
            desc = f"  — {e['desc']}" if e.get("desc") else ""
            print(f"  #{e['id']:>4} {e['ts']}  [{e['type']}]{node}  {e['title']}{desc}")
        return 0

    if parsed.cmd == "event":
        evs = sd.data.setdefault("events", [])
        next_id = (max((e["id"] for e in evs), default=0)) + 1
        from datetime import datetime
        evs.append({
            "id": next_id,
            "ts": datetime.now().isoformat(timespec="seconds"),
            "type": parsed.type,
            "title": parsed.title,
            "desc": parsed.desc,
            "node_id": parsed.node,
        })
        if len(evs) > 200:
            sd.data["events"] = evs[-200:]
        sd.save()
        print(f"✓ 已记录事件 #{next_id}: [{parsed.type}] {parsed.title}")
        return 0

    if parsed.cmd == "clear-events":
        sd.data["events"] = []
        sd.save()
        print("🧹 事件日志已清空")
        return 0

    parser.print_help()
    return 1


# ===== 入口 =====
if __name__ == "__main__":
    if len(sys.argv) <= 1:
        # 无参数默认启动 GUI
        run_gui()
    else:
        sys.exit(run_cli(sys.argv[1:]))
