#!/usr/bin/env python3
"""Animated office-like desktop for fps_benchmark.py's disposable Xvfb only."""
import argparse
import json
import math
import os
from pathlib import Path
import time

from PyQt5.QtCore import QRect, Qt, QTimer
from PyQt5.QtGui import QColor, QFont, QPainter
from PyQt5.QtWidgets import QApplication, QWidget


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stats-file", type=Path, required=True)
    parser.add_argument("--fps", type=int, default=120)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    args = parser.parse_args()
    if os.environ.get("LANDESK_TEST_DESKTOP") != "1":
        parser.error("Run through fps_benchmark.py; a disposable test display is required")
    if not 1 <= args.fps <= 240:
        parser.error("fps must be between 1 and 240")

    class Desktop(QWidget):
        def __init__(self):
            super().__init__()
            self.setWindowFlags(Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint)
            self.setGeometry(0, 0, args.width, args.height)
            self.setAttribute(Qt.WA_OpaquePaintEvent)
            self.started = time.monotonic()
            self.frames = 0
            self.last_report = 0.0
            self.timer = QTimer(self)
            self.timer.setTimerType(Qt.PreciseTimer)
            self.timer.timeout.connect(self.update)
            self.timer.start(max(1, round(1000 / args.fps)))

        def paintEvent(self, _event):
            elapsed = time.monotonic() - self.started
            self.frames += 1
            p = QPainter(self)
            p.scale(self.width() / 1920, self.height() / 1080)
            p.fillRect(QRect(0, 0, 1920, 1080), QColor("#e7edf5"))
            p.fillRect(QRect(0, 0, 1920, 64), QColor("#14283e"))
            p.setFont(QFont("DejaVu Sans", 17))
            p.setPen(QColor("#ffffff"))
            p.drawText(32, 42, "LanDesk   /   isolated animated desktop   /   1920 x 1080")
            p.drawText(1380, 42, "FRAME %08d" % self.frames)

            # A scrolling document: coherent text and flat areas, no random-noise load.
            p.fillRect(QRect(40, 100, 1130, 880), QColor("#ffffff"))
            p.setPen(QColor("#122a45"))
            p.setFont(QFont("DejaVu Sans", 24))
            p.drawText(72, 153, "Engineering notebook")
            p.setClipRect(QRect(72, 183, 1060, 760))
            p.setFont(QFont("DejaVu Sans Mono", 14))
            scroll = elapsed * 138
            offset, first = int(scroll) % 38, int(scroll) // 38
            for row in range(23):
                n = first + row
                y = 216 + row * 38 - offset
                if n % 6 == 0:
                    p.fillRect(QRect(72, y - 24, 1060, 32), QColor("#d9e9ff"))
                p.setPen(QColor("#294765"))
                p.drawText(85, y, "%05d  Capture -> encode -> TLS -> decode -> paint" % n)
            p.setClipping(False)

            # A moving application window and a continuously advancing chart.
            x = 1270 + int(70 * math.sin(elapsed * 1.3))
            y = 130 + int(50 * math.cos(elapsed * 1.1))
            p.fillRect(QRect(x + 9, y + 9, 510, 420), QColor("#b5c1d0"))
            p.fillRect(QRect(x, y, 510, 420), QColor("#f9fbfd"))
            p.fillRect(QRect(x, y, 510, 42), QColor("#2867ba"))
            p.setFont(QFont("DejaVu Sans", 14))
            p.setPen(Qt.white)
            p.drawText(x + 15, y + 29, "Activity monitor")
            for col in range(13):
                height = int(100 + 75 * math.sin(elapsed * 2.2 + col * .6))
                p.fillRect(QRect(x + 25 + col * 36, y + 320 - height, 23, height), QColor("#3784dc"))
            p.setPen(QColor("#294765"))
            p.drawText(x + 25, y + 385, "Live desktop motion / %.2f seconds" % elapsed)

            # Visible binary frame marker survives lossy compression and scaling.
            p.fillRect(QRect(1210, 735, 654, 200), QColor("#ffffff"))
            p.setFont(QFont("DejaVu Sans Mono", 19))
            p.setPen(QColor("#14283e"))
            p.drawText(1230, 783, "SOURCE FRAME %08d" % self.frames)
            for bit in range(16):
                color = "#14283e" if (self.frames >> bit) & 1 else "#e8eef5"
                p.fillRect(QRect(1230 + bit * 38, 815, 33, 83), QColor(color))
            p.end()
            if elapsed - self.last_report >= .25:
                self.report(elapsed)

        def report(self, elapsed=None):
            elapsed = time.monotonic() - self.started if elapsed is None else elapsed
            record = {"ready": self.frames > 0, "source_frames": self.frames,
                      "elapsed_seconds": elapsed, "requested_source_fps": args.fps,
                      "width": args.width, "height": args.height}
            temporary = args.stats_file.with_suffix(".tmp")
            temporary.write_text(json.dumps(record) + "\n")
            temporary.replace(args.stats_file)
            self.last_report = elapsed

    app = QApplication([])
    desktop = Desktop()
    desktop.show()
    app.aboutToQuit.connect(desktop.report)
    return app.exec_()


if __name__ == "__main__":
    raise SystemExit(main())
