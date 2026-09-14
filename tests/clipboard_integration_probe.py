#!/usr/bin/env python3
"""A persistent QClipboard owner/observer for test-owned Xvfb servers only."""
import base64
import json
import os
import sys
from PyQt5.QtCore import QSocketNotifier
from PyQt5.QtGui import QImage
from PyQt5.QtWidgets import QApplication

if os.environ.get("LANDESK_ISOLATED_TEST") != "1" or not os.environ.get("DISPLAY", "").startswith(":"):
    raise SystemExit("Refusing to access a clipboard without the private-desktop test marker")

application = QApplication([])
clipboard = application.clipboard()
changes = 0


def changed():
    global changes
    changes += 1


def respond(message):
    sys.stdout.write(json.dumps(message, ensure_ascii=True, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def command(_):
    line = sys.stdin.readline()
    if not line:
        notifier.setEnabled(False)
        application.quit()
        return
    request = json.loads(line)
    operation = request["op"]
    if operation == "set":
        clipboard.setText(request["text"])
    elif operation == "image":
        image = QImage(2, 2, QImage.Format_RGB32)
        image.fill(0xff2563eb)
        clipboard.setImage(image)
    elif operation not in ("get", "inspect"):
        raise RuntimeError("Unsupported clipboard test command")
    result = {"id": request["id"], "text": clipboard.text(), "changes": changes}
    if operation == "inspect":
        result["raw_plain_base64"] = base64.b64encode(bytes(clipboard.mimeData().data("text/plain"))).decode("ascii")
    respond(result)


clipboard.dataChanged.connect(changed)
notifier = QSocketNotifier(sys.stdin.fileno(), QSocketNotifier.Read)
notifier.activated.connect(command)
respond({"ready": True})
application.exec_()
