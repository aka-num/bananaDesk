#!/usr/bin/env python3
"""Synthetic login fixture on a test-owned Xvfb; never performs OS authentication."""
import ctypes
import json
import os
from pathlib import Path
import sys

from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtGui import QColor, QPainter
from PyQt5.QtWidgets import QApplication, QWidget

if os.environ.get("LANDESK_TEST_DESKTOP") != "1" or not os.environ.get("DISPLAY", "").startswith(":"):
    raise SystemExit("Refusing to run outside the integration test desktop")

log = Path(sys.argv[1]).open("w", buffering=1)


def record(kind, **fields):
    log.write(json.dumps(dict(kind=kind, **fields)) + "\n")


class LoginFixture(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowFlags(Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint)
        self.setWindowTitle("LanDesk isolated synthetic login fixture")
        self.setFocusPolicy(Qt.StrongFocus)
        self.typed = ""
        self.authenticated = False

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#16a34a" if self.authenticated else "#2563eb"))
        painter.setPen(Qt.white)
        font = painter.font()
        font.setPointSize(26)
        painter.setFont(font)
        painter.drawText(80, 150, "SIMULATED LOGIN: " + ("ACCEPTED" if self.authenticated else "LOCKED"))
        font.setPointSize(18)
        painter.setFont(font)
        painter.drawText(80, 230, "Disposable Xvfb. No real account or password is used.")
        painter.drawText(80, 310, "Test input: " + "*" * len(self.typed))

    def keyPressEvent(self, event):
        if self.authenticated:
            record("desktop_input", key=event.key())
            return
        if event.key() in (Qt.Key_Return, Qt.Key_Enter):
            # This fixed public string is test data, not an OS credential.
            accepted = self.typed == "landesk-test-42"
            record("attempt", accepted=accepted, character_count=len(self.typed))
            self.typed = ""
            if accepted:
                self.authenticated = True
                self.releaseKeyboard()
                self.releaseMouse()
        elif event.key() == Qt.Key_Backspace:
            self.typed = self.typed[:-1]
        elif event.text() and all(32 <= ord(c) < 127 for c in event.text()):
            self.typed = (self.typed + event.text())[:64]
        self.update()

    def mousePressEvent(self, event):
        record("mouse_down")


app = QApplication([])
fixture = LoginFixture()
fixture.showFullScreen()


def ready():
    fixture.activateWindow()
    fixture.setFocus(Qt.OtherFocusReason)
    fixture.grabKeyboard()
    fixture.grabMouse()
    # A separate X11 connection must be unable to steal either grab.
    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    x11.XGrabKeyboard.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
    x11.XGrabPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_uint,
        ctypes.c_int, ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
    x11.XFlush.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(os.environ["DISPLAY"].encode())
    if not display:
        raise RuntimeError("Cannot open test display")
    x11.XSetInputFocus(display, int(fixture.winId()), 1, 0)
    x11.XFlush(display)
    keyboard = x11.XGrabKeyboard(display, int(fixture.winId()), 0, 1, 1, 0)
    pointer = x11.XGrabPointer(display, int(fixture.winId()), 0, (1 << 2) | (1 << 3), 1, 1, 0, 0, 0)
    x11.XCloseDisplay(display)
    if keyboard != 1 or pointer != 1:
        record("error", reason="Fixture did not own both X11 grabs", keyboard=keyboard, pointer=pointer)
        app.exit(2)
        return
    record("ready", keyboard_grab_held=True, pointer_grab_held=True, simulated=True)


QTimer.singleShot(150, ready)
raise SystemExit(app.exec_())
