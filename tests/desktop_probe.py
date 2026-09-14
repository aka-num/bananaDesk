"""Synthetic X11 desktop for integration tests. Never uses the real user desktop."""
import ctypes
import json
import sys
from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtGui import QColor, QPainter
from PyQt5.QtWidgets import QApplication, QWidget

log = open(sys.argv[1], "w", buffering=1)
def record(kind, **values):
    log.write(json.dumps({"kind": kind, **values}) + "\n")

class Probe(QWidget):
    def __init__(self):
        super().__init__()
        self.setGeometry(80, 90, 740, 430)
        self.setWindowTitle("LanDesk synthetic test desktop")
        self.setFocusPolicy(Qt.StrongFocus)
        self.setMouseTracking(True)
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#2563eb"))
        painter.setPen(Qt.white)
        font = painter.font(); font.setPointSize(24); painter.setFont(font)
        painter.drawText(40, 80, "LanDesk test desktop")
        font.setPointSize(14); painter.setFont(font)
        painter.drawText(40, 140, "Synthetic screen / keyboard / mouse target")
        painter.drawText(40, 190, "No personal desktop content in this test.")
    def keyPressEvent(self, event):
        record("key_down", key=event.key(), text=event.text())
    def keyReleaseEvent(self, event):
        record("key_up", key=event.key())
    def mousePressEvent(self, event):
        record("mouse_down", button=int(event.button()), x=event.x(), y=event.y())
    def mouseReleaseEvent(self, event):
        record("mouse_up", button=int(event.button()), x=event.x(), y=event.y())
    def wheelEvent(self, event):
        record("wheel", delta=event.angleDelta().y())

app = QApplication([])
probe = Probe(); probe.show()
def focus():
    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]; x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    x11.XFlush.argtypes = [ctypes.c_void_p]; x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    x11.XSetInputFocus(display, int(probe.winId()), 1, 0); x11.XFlush(display); x11.XCloseDisplay(display)
    record("ready", window=int(probe.winId()))
QTimer.singleShot(100, focus)
app.exec_()
