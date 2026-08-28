# -*- coding: utf-8 -*-
import sys
from _qt_compat import (
    QT_BACKEND, gui_available, QApplication, QMainWindow, QWidget,
    QGraphicsEllipseItem, QGraphicsPathItem, QGraphicsScene, QGraphicsView,
    QPainterPath, QTabWidget
)

print(f"Backend: {QT_BACKEND}")
print(f"GUI Available: {gui_available()}")
print(f"QGraphicsEllipseItem: {QGraphicsEllipseItem}")
print(f"QGraphicsPathItem: {QGraphicsPathItem}")
print(f"QGraphicsScene: {QGraphicsScene}")
print(f"QGraphicsView: {QGraphicsView}")
print(f"QPainterPath: {QPainterPath}")
print(f"QTabWidget: {QTabWidget}")

if not gui_available():
    sys.exit(1)

app = QApplication(sys.argv)
w = QWidget()
w.setWindowTitle("Test Window")
w.show()
print("Window shown successfully")
sys.exit(0)
