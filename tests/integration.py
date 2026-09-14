#!/usr/bin/env python3
"""Exercise real TLS, real X11 capture and native input on disposable Xvfb desktops.

Usage: python3 tests/integration.py /absolute/path/to/landesk [--artifacts DIR]
Requires Python PyQt5, Pillow, Xvfb. No real desktop is captured or controlled.
"""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument("binary", type=Path)
parser.add_argument("--artifacts", type=Path)
args = parser.parse_args()
binary = args.binary.resolve()
processes, handles = [], []
checks = []

def check(name):
    checks.append(name)
    print("PASS", name, flush=True)

def launch(command, env, logfile):
    if "--host" in command and "--no-lock-on-disconnect" not in command:
        command = list(command) + ["--no-lock-on-disconnect"]
    handle = open(logfile, "w")
    handles.append(handle)
    process = subprocess.Popen(command, env=env, stdout=handle, stderr=subprocess.STDOUT)
    processes.append(process)
    return process

def wait_for(predicate, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate(): return
        time.sleep(0.04)
    raise AssertionError("Timed out waiting for test condition")

def xvfb(directory, name):
    read, write = os.pipe()
    logfile = open(directory / (name + ".log"), "w"); handles.append(logfile)
    process = subprocess.Popen(["Xvfb", "-displayfd", str(write), "-screen", "0", "1280x800x24", "-nolisten", "tcp", "-ac"], pass_fds=[write], stdout=logfile, stderr=logfile)
    processes.append(process); os.close(write)
    with os.fdopen(read) as pipe: number = pipe.readline().strip()
    assert number, "Xvfb did not start"
    env = os.environ.copy()
    env.update(DISPLAY=":" + number, XDG_SESSION_TYPE="x11", QT_QPA_PLATFORM="xcb")
    env.pop("QT_SCALE_FACTOR", None); env.pop("QT_AUTO_SCREEN_SCALE_FACTOR", None)
    env.pop("XAUTHORITY", None)
    return env

def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0)); return sock.getsockname()[1]

def decode_invite(path):
    encoded = path.read_text().strip().split(":", 1)[1]
    return json.loads(base64.urlsafe_b64decode(encoded + "=" * (-len(encoded) % 4)))

def encode_invite(invite):
    return "landesk1:" + base64.urlsafe_b64encode(json.dumps(invite).encode()).decode().rstrip("=")

def tls(invite):
    # A test client using out-of-band SHA-256 pinning, just like the native client.
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False; context.verify_mode = ssl.CERT_NONE
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    sock = context.wrap_socket(socket.create_connection((invite["host"], invite["port"]), timeout=4), server_hostname=invite["host"])
    assert hashlib.sha256(sock.getpeercert(binary_form=True)).hexdigest() == invite["pin"]
    return sock

def send(sock, kind, payload=b"", fragmented=False):
    if isinstance(payload, dict): payload = json.dumps(payload).encode()
    data = struct.pack("!I", len(payload) + 1) + kind.encode() + payload
    if fragmented:
        for byte in data: sock.sendall(bytes([byte]))
    else: sock.sendall(data)

def exact(sock, count):
    data = b""
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part: raise EOFError()
        data += part
    return data

def receive(sock):
    size, = struct.unpack("!I", exact(sock, 4))
    assert 1 <= size <= 8 * 1024 * 1024
    data = exact(sock, size)
    return chr(data[0]), data[1:]

def closed(sock):
    try: assert sock.recv(1) == b""
    except (ConnectionResetError, ssl.SSLError): pass
    sock.close(); time.sleep(0.12)

def authorized(invite):
    sock = tls(invite)
    send(sock, "A", {"v": 1, "token": invite["token"]}, fragmented=True)
    kind, data = receive(sock); assert kind == "W"
    welcome = json.loads(data)
    kind, data = receive(sock); assert kind == "F"
    return sock, welcome, data

def events(path):
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

temp = tempfile.TemporaryDirectory(prefix="landesk-test-")
directory = Path(temp.name)
try:
    host_env, viewer_env = xvfb(directory, "host-xvfb"), xvfb(directory, "viewer-xvfb")
    tests = binary.parent / "landesk_tests"
    if tests.exists():
        result = subprocess.run([str(tests)], env=viewer_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=20)
        assert result.returncode == 0, result.stdout
        check("native viewer letterbox mapping, Tab delivery, viewing permissions, invitation validation")

    invite_file, host_log = directory / "invite", directory / "host.log"
    host = launch([str(binary), "--host", "--bind", "127.0.0.1", "--port", str(free_port()), "--invite-file", str(invite_file)], host_env, host_log)
    wait_for(invite_file.exists)
    invite = decode_invite(invite_file)
    assert invite_file.stat().st_mode & 0o777 == 0o600
    check("private invitation file permissions")

    probe_events = directory / "events.jsonl"
    probe = launch([sys.executable, str(Path(__file__).with_name("desktop_probe.py")), str(probe_events)], host_env, directory / "probe.log")
    wait_for(lambda: any(e["kind"] == "ready" for e in events(probe_events)))

    sock = tls(invite); sock.settimeout(0.35)
    try:
        sock.recv(1); raise AssertionError("Received data before authentication")
    except socket.timeout: pass
    sock.close(); time.sleep(0.15)
    check("TLS established but no desktop sent before authentication")

    sock = tls(invite); send(sock, "A", {"v": 1, "token": "0" * 48}); closed(sock)
    check("incorrect access token rejected")
    sock = tls(invite); send(sock, "I", {"kind": "key", "key": 65, "down": True}); closed(sock)
    check("input before authentication rejected")
    for length in (0, 4097, 0xFFFFFFFF):
        sock = tls(invite); sock.sendall(struct.pack("!I", length)); closed(sock)
    check("invalid and oversized messages rejected before allocation")

    sock, welcome, jpeg = authorized(invite)
    assert welcome["width"] == 1280 and welcome["height"] == 800 and welcome["control"] is True
    image = Image.open(io.BytesIO(jpeg)); image.load(); assert image.size == (1280, 800)
    pixel = image.getpixel((120, 360)); assert abs(pixel[0] - 37) < 12 and abs(pixel[1] - 99) < 12 and abs(pixel[2] - 235) < 12, pixel
    check("fragmented authentication and real X11 screen delivered as valid JPEG")
    sock.settimeout(0.35)
    try:
        receive(sock); raise AssertionError("Host sent another frame without acknowledgement")
    except socket.timeout: pass
    sock.settimeout(4); send(sock, "P"); assert receive(sock)[0] == "Q"
    send(sock, "K"); assert receive(sock)[0] == "F"
    check("frame backpressure and session heartbeat")

    point = {"x": 200 / 1279, "y": 300 / 799}
    send(sock, "I", {"kind": "button", **point, "button": 1, "down": True})
    send(sock, "I", {"kind": "button", **point, "button": 1, "down": False})
    send(sock, "I", {"kind": "key", "key": 65, "down": True})
    send(sock, "I", {"kind": "key", "key": 65, "down": False})
    send(sock, "I", {"kind": "wheel", **point, "steps": 1})
    wait_for(lambda: any(e["kind"] == "wheel" for e in events(probe_events)))
    seen = events(probe_events)
    assert any(e["kind"] == "key_down" and e["key"] == 65 for e in seen), seen
    assert any(e["kind"] == "key_up" and e["key"] == 65 for e in seen), seen
    assert any(e["kind"] == "mouse_down" and abs(e["x"] - 120) <= 1 and abs(e["y"] - 210) <= 1 for e in seen), seen
    check("native XTEST mouse click coordinates, key press/release, wheel")

    before_invalid = len(events(probe_events))
    send(sock, "I", {"kind": "wheel", **point, "steps": -2147483648})
    send(sock, "I", {"kind": "wheel", **point, "steps": 2147483647})
    send(sock, "I", {"kind": "button", **point, "button": 99, "down": True})
    send(sock, "I", {"kind": "button", "x": 2.0, "y": 0.5, "button": 1, "down": True})
    send(sock, "P"); assert receive(sock)[0] == "Q"
    time.sleep(0.1)
    assert not any(e["kind"] in ("wheel", "mouse_down") for e in events(probe_events)[before_invalid:])
    check("extreme wheel values, unknown buttons and out-of-range coordinates rejected")

    send(sock, "I", {"kind": "key", "key": 0x01000021, "down": True})
    wait_for(lambda: any(e["kind"] == "key_down" and e["key"] == 0x01000021 for e in events(probe_events)))
    sock.close()
    wait_for(lambda: any(e["kind"] == "key_up" and e["key"] == 0x01000021 for e in events(probe_events)))
    check("disconnect releases held Ctrl key")
    time.sleep(0.15)

    sock, _, _ = authorized(invite)
    start = len(events(probe_events))
    send(sock, "I", {"kind": "key", "key": 0x01000020, "down": True})
    wait_for(lambda: any(e["kind"] == "key_down" and e["key"] == 0x01000020 for e in events(probe_events)[start:]))
    wait_for(lambda: any(e["kind"] == "key_up" and e["key"] == 0x01000020 for e in events(probe_events)[start:]), seconds=10)
    closed(sock)
    check("silent connection times out and releases held Shift key")

    wrong = dict(invite, pin="0" * 64)
    wrong_file = directory / "wrong-pin"; wrong_file.write_text(encode_invite(wrong))
    client_log = directory / "wrong-client.log"
    client = launch([str(binary), "--connect-file", str(wrong_file)], viewer_env, client_log)
    wait_for(lambda: "证书指纹不匹配" in client_log.read_text())
    client.terminate(); client.wait(timeout=5); time.sleep(0.15)
    check("native client rejects certificate pin mismatch")

    client_log = directory / "client.log"
    client = launch([str(binary), "--connect-file", str(invite_file)], viewer_env, client_log)
    wait_for(lambda: "已连接 · 1280" in client_log.read_text())
    time.sleep(0.4)
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)
        script = "from PyQt5.QtWidgets import QApplication; import sys; a=QApplication([]); a.primaryScreen().grabWindow(0).save(sys.argv[1])"
        subprocess.run([sys.executable, "-c", script, str(args.artifacts.resolve() / "lan-desktop-preview.png")], env=viewer_env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    client.terminate(); client.wait(timeout=5); time.sleep(0.15)
    check("native client completes TLS auth and renders remote desktop")

    host.terminate(); host.wait(timeout=5)
    invite_file.unlink()
    host = launch([str(binary), "--host", "--view-only", "--bind", "127.0.0.1", "--port", str(invite["port"]), "--invite-file", str(invite_file)], host_env, directory / "readonly-host.log")
    wait_for(invite_file.exists); new_invite = decode_invite(invite_file)
    assert new_invite["token"] != invite["token"] and new_invite["pin"] != invite["pin"]
    check("restarting sharing rotates both access token and TLS identity")
    sock, welcome, _ = authorized(new_invite); assert welcome["control"] is False
    send(sock, "I", {"kind": "key", "key": 65, "down": True}); closed(sock)
    check("view-only permission enforced by host")
    report = {"passed": len(checks), "checks": checks, "platform": "Ubuntu 22.04, Qt 5.15, isolated X11/Xvfb, TLS over loopback", "not_tested": ["physical LAN between two computers", "Windows runtime", "Wayland", "login or UAC screens", "mixed-DPI multiple displays"]}
    if args.artifacts: (args.artifacts / "test-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"passed": len(checks)}, ensure_ascii=False), flush=True)
except Exception:
    for path in directory.glob("*.log"):
        print("\n---", path.name, "---\n", path.read_text()[-4000:], file=sys.stderr)
    raise
finally:
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
    for handle in handles: handle.close()
    temp.cleanup()
