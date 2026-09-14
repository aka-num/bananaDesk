#!/usr/bin/env python3
"""Actual native Host/Client login simulation over TLS on two private Xvfb servers.

python3 tests/linux_login_integration.py /path/to/landesk --artifacts /path/to/results
Requires PyQt5, Pillow, Xvfb, libXtst. Never locks a real desktop or uses PAM.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import sys
import time

from PIL import Image
from fps_benchmark import Run, free_port, latest, screenshot


def events(path):
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def color_points(path, expected):
    image = Image.open(path).convert("RGB")
    return [(x, y) for y in range(0, image.height, 6) for x in range(0, image.width, 6)
            if all(abs(actual - target) < 15 for actual, target in zip(image.getpixel((x, y)), expected))]


class ViewerInput:
    """Injects only into the test-owned viewer display, never into the Host."""
    def __init__(self, env):
        assert env["LANDESK_TEST_DESKTOP"] == "1" and env["DISPLAY"].startswith(":")
        self.x11 = x11 = ctypes.CDLL("libX11.so.6")
        self.xtst = xtst = ctypes.CDLL("libXtst.so.6")
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x11.XDefaultRootWindow.restype = ctypes.c_ulong
        x11.XFlush.argtypes = [ctypes.c_void_p]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        x11.XKeysymToKeycode.restype = ctypes.c_uint
        x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        x11.XQueryPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint)]
        xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
        xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.display = x11.XOpenDisplay(env["DISPLAY"].encode())
        assert self.display

    def click(self, point):
        self.xtst.XTestFakeMotionEvent(self.display, -1, *point, 0)
        self.x11.XFlush(self.display)
        root, child = ctypes.c_ulong(), ctypes.c_ulong()
        rx, ry, wx, wy = (ctypes.c_int() for _ in range(4))
        mask = ctypes.c_uint()
        assert self.x11.XQueryPointer(self.display, self.x11.XDefaultRootWindow(self.display),
            ctypes.byref(root), ctypes.byref(child), ctypes.byref(rx), ctypes.byref(ry),
            ctypes.byref(wx), ctypes.byref(wy), ctypes.byref(mask))
        assert child.value
        self.x11.XSetInputFocus(self.display, child.value, 1, 0)
        self.xtst.XTestFakeButtonEvent(self.display, 1, 1, 0)
        self.xtst.XTestFakeButtonEvent(self.display, 1, 0, 0)
        self.x11.XFlush(self.display)

    def type_test_string(self, text):
        assert text in ("wrong", "landesk-test-42", "a")
        for symbol in [ord(char) for char in text] + ([0xff0d] if text != "a" else []):
            code = self.x11.XKeysymToKeycode(self.display, symbol)
            assert code
            self.xtst.XTestFakeKeyEvent(self.display, code, 1, 0)
            self.xtst.XTestFakeKeyEvent(self.display, code, 0, 0)
            self.x11.XFlush(self.display)
            time.sleep(.03)

    def close(self):
        self.x11.XCloseDisplay(self.display)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--codec", choices=("h264", "jpeg"), default="h264")
    args = parser.parse_args()
    binary = args.binary.resolve()
    assert binary.is_file()
    directory = args.artifacts.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    run = Run(directory)
    report = {"passed": False, "simulated": True,
        "scope": "Actual Linux native Host/Client, X11 capture and input over TLS loopback, two isolated Xvfb servers",
        "not_tested": ["Real GNOME lockscreen and OS password authentication", "Physical LAN", "Windows lockscreen", "Wayland", "Display DPMS wake"],
        "real_desktop_changed": False, "real_credentials_used": False}
    driver = None
    invite_file = directory / "invite"
    try:
        host_env, viewer_env = run.xvfb("host-xvfb"), run.xvfb("viewer-xvfb")
        for env in (host_env, viewer_env):
            env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=" + str(directory / "unused-test-bus")
        fixture_events, stats_file = directory / "fixture-events.jsonl", directory / "client-stats.json"
        for stale in (invite_file, fixture_events, stats_file):
            stale.unlink(missing_ok=True)
        run.launch([str(binary), "--host", "--no-lock-on-disconnect", "--bind", "127.0.0.1",
            "--port", str(free_port()), "--invite-file", str(invite_file), "--codec", args.codec], host_env, "host")
        run.wait(invite_file.exists)
        run.launch([sys.executable, str(Path(__file__).with_name("linux_login_fixture.py")), str(fixture_events)], host_env, "fixture")
        ready = run.wait(lambda: next((e for e in events(fixture_events) if e["kind"] == "ready"), None))
        run.launch([str(binary), "--connect-file", str(invite_file), "--stats-file", str(stats_file)], viewer_env, "client")
        run.wait(lambda: latest(stats_file).get("painted_frames", 0) >= 15)
        screenshot(viewer_env, directory / "viewer-locked.png")
        locked_points = color_points(directory / "viewer-locked.png", (37, 99, 235))
        assert len(locked_points) > 2500, "Native client did not display the simulated locked screen"
        driver = ViewerInput(viewer_env)
        driver.click(locked_points[len(locked_points) // 2])
        run.wait(lambda: any(e["kind"] == "mouse_down" for e in events(fixture_events)))
        driver.type_test_string("wrong")
        run.wait(lambda: any(e["kind"] == "attempt" for e in events(fixture_events)))
        attempts = [e for e in events(fixture_events) if e["kind"] == "attempt"]
        assert attempts == [{"kind": "attempt", "accepted": False, "character_count": 5}], attempts
        screenshot(viewer_env, directory / "viewer-wrong-string.png")
        assert len(color_points(directory / "viewer-wrong-string.png", (37, 99, 235))) > 2500
        driver.type_test_string("landesk-test-42")
        run.wait(lambda: any(e["kind"] == "attempt" and e["accepted"] for e in events(fixture_events)))
        before = latest(stats_file).get("painted_frames", 0)
        run.wait(lambda: latest(stats_file).get("painted_frames", 0) >= before + 10)
        screenshot(viewer_env, directory / "viewer-unlocked.png")
        green_points = color_points(directory / "viewer-unlocked.png", (22, 163, 74))
        assert len(green_points) > 2500, "Native client did not display the simulated authenticated desktop"
        driver.type_test_string("a")
        run.wait(lambda: any(e["kind"] == "desktop_input" for e in events(fixture_events)))
        stats = latest(stats_file)
        assert stats.get("codec") == args.codec, stats
        attempts = [e for e in events(fixture_events) if e["kind"] == "attempt"]
        assert attempts[-1] == {"kind": "attempt", "accepted": True, "character_count": 15}, attempts
        report.update(passed=True, codec=stats["codec"], input_path="XTEST into viewer -> native Viewer -> TLS -> native Host -> NativeInput XTEST -> grabbed fixture",
            checks={"fixture_keyboard_and_pointer_grabs": ready["keyboard_grab_held"] and ready["pointer_grab_held"],
                "locked_screen_rendered_by_native_client": True, "mouse_reaches_grabbed_fixture": True,
                "wrong_test_string_does_not_unlock_fixture": True, "correct_test_string_reaches_fixture_via_tls": True,
                "changed_screen_rendered_by_native_client": True, "input_after_simulated_unlock": True},
            attempts=attempts, painted_frames=stats.get("painted_frames"), screenshot_color_samples={"locked_blue": len(locked_points), "unlocked_green": len(green_points)})
    except Exception as error:
        report["error"] = str(error)
        for path in directory.glob("*.log"):
            print(path.name + ":\n" + path.read_text()[-3000:], file=sys.stderr)
    finally:
        if driver:
            driver.close()
        run.close()
        invite_file.unlink(missing_ok=True)
        (directory / "linux-login-integration.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
