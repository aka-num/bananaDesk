#!/usr/bin/env python3
"""Verify text clipboard synchronization using real TLS and native app processes.

Two private Xvfb displays and persistent Qt clipboard probes are created. This
never reads or writes the user's live clipboard, credentials, or actual desktop.
Run: python3 tests/clipboard_integration.py /path/to/bananaDesk --artifacts DIR
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import socket
import ssl
import subprocess
import sys
import tempfile
import time

from file_transfer_integration import Harness, send, receive, heartbeat, expect_closed, wait_for


class Probe:
    def __init__(self, harness, env, name):
        self.sequence = 0
        self.process = subprocess.Popen([sys.executable, str(Path(__file__).with_name("clipboard_integration_probe.py"))],
                                        env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=harness.open_log(name), text=True, bufsize=1)
        harness.processes.append(self.process)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        assert self.read().get("ready") is True

    def read(self):
        assert self.selector.select(timeout=4), "Private clipboard probe did not respond"
        line = self.process.stdout.readline()
        assert line, "Private clipboard probe exited"
        return json.loads(line)

    def command(self, operation, **values):
        self.sequence += 1
        self.process.stdin.write(json.dumps({"id": self.sequence, "op": operation, **values}, ensure_ascii=True) + "\n")
        self.process.stdin.flush()
        response = self.read()
        assert response["id"] == self.sequence
        return response

    def set(self, text):
        response = self.command("set", text=text)
        assert response["text"] == text

    def get(self):
        return self.command("get")["text"]



def windows_path(path):
    return "Z:" + str(path.resolve()).replace("/", "\\")


class WindowsProbe:
    """Reads the Windows CF_UNICODETEXT clipboard itself, not Wine's X11 export."""
    def __init__(self, harness, env, wine, executable, directory):
        self.directory = directory
        self.directory.mkdir()
        self.sequence = 0
        wine_env = env.copy()
        wine_env["LANDESK_WINE_DISPLAY"] = wine_env["DISPLAY"]
        self.process = subprocess.Popen([str(wine), str(executable), windows_path(directory)], env=wine_env,
                                        stdout=harness.open_log("windows-clipboard-probe"), stderr=subprocess.STDOUT)
        harness.processes.append(self.process)
        wait_for(lambda: (directory / "result-ready").exists() or self.process.poll() is not None, timeout=20)
        assert self.process.poll() is None, "Windows test clipboard probe exited"

    def command(self, operation, **values):
        assert operation in ("get", "set")
        self.sequence += 1
        if operation == "set":
            (self.directory / "command-data").write_bytes(values["text"].encode("utf-8"))
        temporary = self.directory / "command.tmp"
        temporary.write_text(str(self.sequence) + (" S\n" if operation == "set" else " G\n"))
        temporary.replace(self.directory / "command")
        def ready():
            path = self.directory / "result-ready"
            return path.exists() and path.read_text().split()[0] == str(self.sequence)
        wait_for(ready, timeout=4)
        sequence, error, changes = map(int, (self.directory / "result-ready").read_text().split())
        assert error == 0, "Windows test clipboard API failed: " + str(error)
        text = (self.directory / "result-data").read_bytes().decode("utf-8")
        return {"id": sequence, "text": text, "changes": changes}

    def set(self, text):
        assert self.command("set", text=text)["text"] == text

    def get(self):
        return self.command("get")["text"]


def second_display(harness):
    read, write = os.pipe()
    log = harness.open_log("client-xvfb")
    process = subprocess.Popen(["Xvfb", "-displayfd", str(write), "-screen", "0", "960x640x24", "-nolisten", "tcp", "-ac"],
                               pass_fds=[write], stdout=log, stderr=log)
    harness.processes.append(process)
    os.close(write)
    with os.fdopen(read) as stream:
        number = stream.readline().strip()
    assert number
    env = harness.env.copy()
    env["DISPLAY"] = ":" + number
    assert env["DISPLAY"] != harness.env["DISPLAY"]
    return env


def authenticated(harness, invitation, clipboard=True):
    sock = harness.tls(invitation)
    authentication = {"v": 1, "token": invitation["token"], "codecs": ["jpeg"], "window": 1}
    if clipboard is not None:
        authentication["clipboard"] = clipboard
    send(sock, "A", authentication)
    kind, payload = receive(sock)
    assert kind == "W", "Host must send Welcome before clipboard traffic"
    welcome = json.loads(payload)
    assert receive(sock)[0] == "F"
    return sock, welcome


def quiet(sock, duration=0.35):
    previous = sock.gettimeout()
    sock.settimeout(duration)
    try:
        kind, payload = receive(sock)
        raise AssertionError("Unexpected packet while no clipboard should be exported: " + kind + " " + repr(payload[:80]))
    except socket.timeout:
        pass
    finally:
        sock.settimeout(previous)


def clipboard_packet(sock, text):
    kind, payload = receive(sock)
    assert kind == "C", kind
    assert payload == text.encode("utf-8"), "Clipboard UTF-8 bytes changed in transit"


def invalid_clipboard(harness, invitation, probe, payload):
    original = probe.get()
    sock, welcome = authenticated(harness, invitation)
    assert welcome.get("clipboard") is True
    send(sock, "C", payload)
    expect_closed(sock)
    assert probe.get() == original, "Rejected clipboard packet modified the host clipboard"


def run_tests(harness, windows_client=None, wine=None, windows_probe=None):
    host_probe = Probe(harness, harness.env, "host-clipboard-probe")
    host_probe.set("old-host-private-test-sentinel")
    host, invitation, _ = harness.launch_host()

    sock = harness.tls(invitation)
    send(sock, "C", "not-authenticated".encode())
    expect_closed(sock)
    assert host_probe.get() == "old-host-private-test-sentinel"
    harness.check("Clipboard packets before authentication cannot read or modify the host clipboard")

    sock, welcome = authenticated(harness, invitation)
    assert welcome.get("clipboard") is True
    quiet(sock)
    assert host_probe.get() == "old-host-private-test-sentinel"
    harness.check("Clipboard capability is negotiated and initial clipboard content is never exported")
    host_probe.set("old-host-private-test-sentinel")
    clipboard_packet(sock, "old-host-private-test-sentinel")
    quiet(sock)
    harness.check("An explicit copy after connection synchronizes even when its text equals the initial snapshot")

    host_probe.set("pending-text-interrupted-by-image")
    host_probe.command("image")
    quiet(sock)
    host_probe.set("pending-text-interrupted-by-image")
    clipboard_packet(sock, "pending-text-interrupted-by-image")
    quiet(sock)
    harness.check("Replacing a pending text copy with an image cancels it and copying the same text later still works")

    host_text = "主机复制：香蕉 🍌\n第二行\r\nTab\t尾行"
    host_probe.set(host_text)
    clipboard_packet(sock, host_text)
    quiet(sock)
    remote_text = "控制端复制：café / 中文 / 😀\nline 2\r\n"
    send(sock, "C", remote_text.encode("utf-8"))
    wait_for(lambda: host_probe.get() == remote_text)
    quiet(sock)
    heartbeat(sock)
    harness.check("Unicode, emoji, tabs and line endings synchronize in both TLS directions without reflecting received text")

    send(sock, "C", b"")
    wait_for(lambda: host_probe.get() == "")
    quiet(sock)
    harness.check("Empty UTF-8 clears the remote clipboard without a reflection loop")

    maximum = "中" * 21845 + "a"
    assert len(maximum.encode("utf-8")) == 65536
    send(sock, "C", maximum.encode("utf-8"))
    wait_for(lambda: host_probe.get() == maximum)
    quiet(sock)
    host_probe.set("x" * 65536)
    clipboard_packet(sock, "x" * 65536)
    quiet(sock)
    harness.check("Exactly 64 KiB of UTF-8 transfers in both directions and bypasses the smaller keyboard-packet limit")

    host_probe.set("x" * 65537)
    quiet(sock)
    host_probe.set("valid-copy-after-local-rejection")
    clipboard_packet(sock, "valid-copy-after-local-rejection")
    # Cross-process Qt/X11 clipboard export itself truncates local NUL strings,
    # including raw text/plain before this app sees them; raw network NUL is
    # tested independently below, and backend unit tests cover in-process NUL.
    harness.check("Oversized local copies are ignored while later valid copies still synchronize")

    # Disconnect before the 100 ms copy debounce fires; a new session must not
    # receive either the queued old copy or text copied while disconnected.
    host_probe.set("copy-queued-before-disconnect")
    sock.close()
    time.sleep(0.18)
    host_probe.set("copied-while-disconnected")
    time.sleep(0.15)
    sock, welcome = authenticated(harness, invitation)
    assert welcome.get("clipboard") is True
    quiet(sock)
    heartbeat(sock)
    sock.close()
    time.sleep(0.15)
    harness.check("Disconnect cancels queued clipboard sends and reconnecting does not leak copies made outside the session")

    for payload in (b"\xc0\xaf", b"\xed\xa0\x80", b"\xf0\x9f\x8d", b"a\x00b", b"x" * 65537):
        invalid_clipboard(harness, invitation, host_probe, payload)
    harness.check("Overlong UTF-8, surrogate UTF-8, truncated UTF-8, NUL and over-limit packets are rejected without changing the clipboard")

    sock, welcome = authenticated(harness, invitation, clipboard=None)
    assert welcome.get("clipboard") is False
    host_probe.set("old-peer-does-not-receive-this")
    quiet(sock)
    send(sock, "C", b"old-peer-cannot-write")
    expect_closed(sock)
    assert host_probe.get() == "old-peer-does-not-receive-this"
    harness.check("An older peer without Auth clipboard capability neither receives nor writes clipboard text")

    readonly, readonly_invite, _ = harness.launch_host(("--view-only",))
    sock, welcome = authenticated(harness, readonly_invite)
    assert welcome["control"] is False and welcome.get("clipboard") is False
    host_probe.set("view-only-local-copy")
    quiet(sock)
    send(sock, "C", b"view-only-cannot-write")
    expect_closed(sock)
    assert host_probe.get() == "view-only-local-copy"
    readonly.terminate()
    readonly.wait(timeout=5)
    harness.check("View-only sessions cannot receive or alter the clipboard even when advertising support")

    client_env = second_display(harness)
    wine_directory = None
    if windows_client:
        shared = wine.parent / "shared"
        shared.mkdir(exist_ok=True)
        harness.wine_temporary = tempfile.TemporaryDirectory(prefix="clipboard-integration-", dir=shared)
        wine_directory = Path(harness.wine_temporary.name)
        client_probe = WindowsProbe(harness, client_env, wine, windows_probe, wine_directory / "probe")
    else:
        client_probe = Probe(harness, client_env, "client-clipboard-probe")
    host_probe.set("native-host-old-content")
    client_probe.set("native-client-old-content")
    invitation_file = harness.directory / "host-1" / "invitation"
    client_log_path = harness.directory / "native-client.log"
    client_log = harness.open_log("native-client")
    command = [str(harness.binary), "--connect-file", str(invitation_file)]
    if windows_client:
        wine_invitation = wine_directory / "invitation"
        wine_invitation.write_bytes(invitation_file.read_bytes())
        wine_invitation.chmod(0o600)
        client_log_path = wine_directory / "client.log"
        harness.wine_log = client_log_path
        command = [str(wine), str(windows_client), "--connect-file", windows_path(wine_invitation),
                   "--log-file", windows_path(client_log_path)]
        client_env["LANDESK_WINE_DISPLAY"] = client_env["DISPLAY"]
    client = subprocess.Popen(command, env=client_env, stdout=client_log, stderr=subprocess.STDOUT)
    harness.processes.append(client)
    wait_for(lambda: (client_log_path.exists() and "已连接 ·" in client_log_path.read_text(errors="replace")) or client.poll() is not None, timeout=20)
    assert client.poll() is None
    time.sleep(0.3)
    assert host_probe.get() == "native-host-old-content" and client_probe.get() == "native-client-old-content"
    def same_native_text(actual, expected):
        if windows_client and actual.replace("\r\n", "\n") == expected.replace("\r\n", "\n"):
            if actual != expected:
                harness.eol_normalized = True
            return True
        return actual == expected
    native_host = "真实 Host → Client：🍌\nalpha\r\nbeta"
    host_probe.set(native_host)
    try:
        wait_for(lambda: same_native_text(client_probe.get(), native_host))
    except AssertionError as problem:
        raise AssertionError("Native client clipboard mismatch: " + repr(client_probe.get()[:160])) from problem
    native_client = "真实 Client → Host：漢字 / café\n\t😀"
    client_probe.set(native_client)
    try:
        wait_for(lambda: same_native_text(host_probe.get(), native_client))
    except AssertionError as problem:
        raise AssertionError("Native host clipboard mismatch: " + repr(host_probe.get()[:160])) from problem
    host_before = host_probe.command("get")["changes"]
    client_before = client_probe.command("get")["changes"]
    time.sleep(0.4)
    assert host_probe.command("get")["changes"] == host_before
    assert client_probe.command("get")["changes"] == client_before
    harness.check(("Actual Linux Host and Windows Client under Wine" if windows_client else "Two actual native Linux app processes") + " on separate private desktops synchronize both ways without initial-content leakage or clipboard churn")

    host.terminate()
    host.wait(timeout=5)
    time.sleep(0.3)
    host_probe.set("host-copy-after-session-ended")
    client_probe.set("client-copy-after-session-ended")
    time.sleep(0.3)
    assert host_probe.get() == "host-copy-after-session-ended"
    assert client_probe.get() == "client-copy-after-session-ended"
    harness.check("The native client stops applying clipboard synchronization after the TLS session ends")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--wine", type=Path, help="Wine wrapper with LANDESK_WINE_DISPLAY support; do not run other jobs using its prefix concurrently")
    parser.add_argument("--windows-client", type=Path, help="Packaged Windows bananaDesk.exe to use as the native controller")
    parser.add_argument("--windows-probe", type=Path, help="Test-only CF_UNICODETEXT probe built from tests/windows/clipboard_probe.cpp")
    arguments = parser.parse_args()
    if bool(arguments.wine) != bool(arguments.windows_client) or bool(arguments.wine) != bool(arguments.windows_probe):
        parser.error("--wine, --windows-client and --windows-probe must be used together")
    artifacts = arguments.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    harness = Harness(arguments.binary, None)
    harness.wine_temporary = None
    harness.wine_log = None
    harness.eol_normalized = False
    failed = True
    try:
        run_tests(harness, arguments.windows_client.resolve() if arguments.windows_client else None, arguments.wine.resolve() if arguments.wine else None, arguments.windows_probe.resolve() if arguments.windows_probe else None)
        failed = False
    finally:
        report = {"passed": not failed, "checks_passed": len(harness.checks), "checks": harness.checks,
                  "platform": ("Linux host + Windows client under Wine, two private Xvfb servers, real TLS loopback" if arguments.wine else "Linux, two private Xvfb servers, real native Host/Client and TLS loopback"),
                  "binary": str(harness.binary), "binary_sha256": hashlib.sha256(harness.binary.read_bytes()).hexdigest(),
                  "real_clipboard_accessed": False,
                  "windows_cf_unicode_line_endings_normalized": harness.eol_normalized,
                  "protocol_test_platform": "Linux Host and Python TLS peer",
                  "native_client_test_platform": "Windows executable under Wine using CF_UNICODETEXT" if arguments.wine else "Linux executable using Qt/X11 clipboard",
                  "not_tested": ["physical Windows clipboard", "Wayland clipboard", "two physical machines"]}
        if arguments.windows_client:
            probe_source = Path(__file__).with_name("windows") / "clipboard_probe.cpp"
            report.update(windows_client=str(arguments.windows_client.resolve()),
                          windows_client_sha256=hashlib.sha256(arguments.windows_client.read_bytes()).hexdigest(),
                          windows_probe=str(arguments.windows_probe.resolve()),
                          windows_probe_sha256=hashlib.sha256(arguments.windows_probe.read_bytes()).hexdigest(),
                          windows_probe_source=str(probe_source.resolve()),
                          windows_probe_source_sha256=hashlib.sha256(probe_source.read_bytes()).hexdigest())
        (artifacts / "clipboard-integration-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        if failed:
            for path in harness.directory.glob("*.log"):
                (artifacts / path.name).write_text(path.read_text(errors="replace"))
        if harness.wine_log and harness.wine_log.exists():
            (artifacts / "windows-client.log").write_text(harness.wine_log.read_text(errors="replace"))
        harness.cleanup(failed)
        if harness.wine_temporary:
            harness.wine_temporary.cleanup()
    print(json.dumps({"passed": len(harness.checks)}), flush=True)


if __name__ == "__main__":
    main()
