#!/usr/bin/env python3
# fps_badge.py — FPS 徽章 + cloud_main 帧率监听
# =====================================================================
# 2026-09-10 编写 — 3D 区域右上角 FPS 显示
#   - FPSBadge: 浮动半透黑徽章, 永远贴父控件右上角
#   - CloudMainFPSListener: 后台线程, 监听 cloud_main log 的 [FPS] 行
#     (2026-09-10 改: 之前监听 "MultiMesh Updated" 频率不准确, 现在
#      main.gd _process 每秒打一次 "[FPS] 58.0", 这个是真 3D 帧率)
#   - 颜色规则: 绿(≥30) / 黄(15-30) / 红(<15) — 用户偏好的状态颜色编码
# =====================================================================

import os
import re
import subprocess
import time
from collections import deque
from pathlib import Path

try:
    from PyQt6.QtCore import Qt, QThread, pyqtSignal, QTimer
    from PyQt6.QtGui import QColor, QFont, QPainter, QPen, QBrush
    from PyQt6.QtWidgets import QWidget, QLabel, QVBoxLayout
    _HAS_PYQT6 = True
except ImportError:
    _HAS_PYQT6 = False


# ============================================================================
# FPS 徽章 (半透黑, 永远贴右上角, 32pt 大字)
# ============================================================================

class FPSBadge(QWidget if _HAS_PYQT6 else object):
    """浮动 FPS 徽章 — 永远贴父控件右上角, 10px 边距

    用法:
        badge = FPSBadge(parent_widget)
        badge.start_timer()       # 启动内置秒表模式 (测试用)
        # 或:
        badge.update_fps(58.3)     # 外部推 FPS (推荐, 配合 CloudMainFPSListener)
    """

    def __init__(self, parent=None):
        if not _HAS_PYQT6:
            raise RuntimeError("FPSBadge 需要 PyQt6")
        super().__init__(parent)
        self._fps = 0.0
        self._ms = 0.0
        self._color = QColor(0, 255, 136)  # 绿
        # 永远贴父控件右上角
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        # 200x70 足够装 FPS 数字 + 副信息
        self.setFixedSize(220, 76)
        if parent is not None:
            self._reposition()
            # 父控件 resize 时跟着走
            original_resize = parent.resizeEvent
            def new_resize(ev, _orig=original_resize):
                if _orig:
                    _orig(ev)
                self._reposition()
            parent.resizeEvent = new_resize
        # 内部定时器 (测试模式, 不用可关)
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._on_timer_tick)
        self._last_tick_ts = time.time()

    def _reposition(self):
        if self.parent() is None:
            return
        pw = self.parent().width()
        self.move(pw - self.width() - 10, 10)

    def _on_timer_tick(self):
        """内置秒表模式 (测试用) — 推算 self._fps"""
        now = time.time()
        dt = now - self._last_tick_ts
        self._last_tick_ts = now
        if dt > 0:
            self._fps = 1.0 / dt
        self._ms = dt * 1000.0
        self._update_color()
        self.update()

    def start_timer(self):
        self._last_tick_ts = time.time()
        self._timer.start(16)  # ~60 Hz

    def stop_timer(self):
        self._timer.stop()

    def update_fps(self, fps: float, ms: float = 0.0):
        """外部推 FPS (CloudMainFPSListener 用)"""
        self._fps = fps
        self._ms = ms if ms > 0 else (1000.0 / fps if fps > 0 else 0.0)
        self._update_color()
        self.update()

    def _update_color(self):
        # 绿 ≥30 / 黄 15-30 / 红 <15
        if self._fps >= 30.0:
            self._color = QColor(0, 255, 136)
        elif self._fps >= 15.0:
            self._color = QColor(255, 220, 60)
        else:
            self._color = QColor(255, 80, 80)

    def paintEvent(self, ev):
        if not _HAS_PYQT6:
            return
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        # 半透黑背景
        p.setBrush(QBrush(QColor(0, 0, 0, 180)))
        p.setPen(QPen(QColor(80, 80, 80, 200), 1))
        p.drawRoundedRect(0, 0, self.width(), self.height(), 6, 6)
        # FPS 数字 (24pt monospace, 粗体)
        font_main = QFont("monospace", 24)
        font_main.setBold(True)
        p.setFont(font_main)
        p.setPen(self._color)
        fps_text = f"{int(self._fps)}" if self._fps >= 1.0 else "--"
        p.drawText(self.rect().adjusted(8, 2, -8, -28), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop, fps_text)
        # "FPS" 标签 (12pt)
        font_label = QFont("sans-serif", 11)
        font_label.setBold(True)
        p.setFont(font_label)
        p.setPen(QColor(200, 200, 200))
        p.drawText(self.rect().adjusted(8, 2, -8, -28), Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignTop, "FPS")
        # 副信息 (ms + Hz, 10pt)
        font_sub = QFont("monospace", 10)
        p.setFont(font_sub)
        p.setPen(QColor(170, 170, 170))
        sub_text = f"{self._ms:.1f} ms  |  {self._fps:.1f} Hz" if self._fps >= 1.0 else "等待 cloud_main..."
        p.drawText(self.rect().adjusted(8, 0, -8, -6), Qt.AlignmentFlag.AlignBottom | Qt.AlignmentFlag.AlignLeft, sub_text)
        p.end()


# ============================================================================
# CloudMainFPSListener — 后台线程, 监听 cloud_main log 的 [FPS] 行
# ============================================================================

class CloudMainFPSListener(QThread if _HAS_PYQT6 else object):
    """监听 cloud_main log, 解析 main.gd _process 写的 [FPS] X.X 行

    信号:
        fps_updated(float)  — 新的 FPS 值 (1s 一次, 来自 main.gd)
    """

    fps_updated = pyqtSignal(float) if _HAS_PYQT6 else None

    # main.gd 写: print("[FPS] %.1f (dt=%.3fs)" % [_fps_value, _delta])
    _RE_FPS = re.compile(r"\[FPS\]\s*([\d.]+)")

    def __init__(self, log_path: str = "/home/bv/code/godot_ui_linux/.ai_tools/cloud_main.log", parent=None):
        if not _HAS_PYQT6:
            raise RuntimeError("CloudMainFPSListener 需要 PyQt6")
        super().__init__(parent)
        self._log_path = log_path
        self._stop_flag = False
        # 用 deque 缓冲最近 FPS (1s 滑动平均)
        self._fps_window = deque(maxlen=10)

    def stop(self):
        self._stop_flag = True

    def run(self):
        """主循环: tail -F cloud_main log, 解析 [FPS] 行"""
        if not Path(self._log_path).exists():
            # log 还没建, 等
            self._wait_for_log()
        if self._stop_flag:
            return
        # 启动 tail -F (用 grep --line-buffered 只看 [FPS] 行)
        # -F 跟 -f 不同, -F 会跟踪文件 rename/truncate (cloud_main 重启时 log 可能轮转)
        cmd = ["tail", "-F", "-n", "0", self._log_path]
        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                bufsize=0, text=False,
            )
        except Exception as e:
            print(f"[CloudMainFPSListener] 启动 tail 失败: {e}")
            return
        try:
            while not self._stop_flag:
                line = proc.stdout.readline()
                if not line:
                    # EOF — log 文件被 truncate/rename, tail -F 会重开, 但保险起见
                    time.sleep(0.1)
                    continue
                line_s = line.decode("utf-8", errors="replace")
                m = self._RE_FPS.search(line_s)
                if m:
                    fps = float(m.group(1))
                    self._fps_window.append(fps)
                    # 1s 滑动平均 (防止某一帧异常)
                    avg_fps = sum(self._fps_window) / len(self._fps_window)
                    self.fps_updated.emit(avg_fps)
        except Exception as e:
            print(f"[CloudMainFPSListener] 主循环异常: {e}")
        finally:
            try:
                proc.terminate()
            except Exception:
                pass

    def _wait_for_log(self, max_wait_sec: float = 30.0):
        """等 cloud_main log 文件出现 (cloud_main 启动需要时间)"""
        t0 = time.time()
        while not self._stop_flag and time.time() - t0 < max_wait_sec:
            if Path(self._log_path).exists():
                return
            time.sleep(0.5)


# ============================================================================
# 测试入口
# ============================================================================

if __name__ == "__main__":
    import sys
    from PyQt6.QtWidgets import QApplication, QMainWindow
    app = QApplication(sys.argv)
    win = QMainWindow()
    win.resize(1200, 800)
    win.setStyleSheet("QMainWindow { background-color: #1a1a1a; }")
    win.setWindowTitle("FPSBadge 测试")
    badge = FPSBadge(win)
    badge.start_timer()
    win.show()
    sys.exit(app.exec())
