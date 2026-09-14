#!/usr/bin/env python3
"""Real TLS file-transfer integration on a disposable desktop and transfer directory.

No production files, active desktop, OS lock, or local installed services are used.
Run: python3 tests/file_transfer_integration.py /absolute/path/landesk --artifacts DIR
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import time


def wait_for(predicate, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.025)
    raise AssertionError("Timed out waiting for an isolated test condition")


def unused_port():
    with socket.socket() as temporary:
        temporary.bind(("127.0.0.1", 0))
        return temporary.getsockname()[1]


def decode_invitation(path):
    prefix, data = path.read_text().strip().split(":", 1)
    assert prefix == "landesk1"
    return json.loads(base64.urlsafe_b64decode(data + "=" * (-len(data) % 4)))


class Harness:
    def __init__(self, binary, artifacts):
        self.binary = binary.resolve()
        self.artifacts = artifacts.resolve() if artifacts else None
        self.temporary = tempfile.TemporaryDirectory(prefix="landesk-transfer-integration-")
        self.directory = Path(self.temporary.name)
        self.processes, self.logs, self.checks = [], [], []
        self.sockets = []
        self.counter = 0
        self.env = os.environ.copy()
        read, write = os.pipe()
        log = self.open_log("xvfb")
        process = subprocess.Popen(["Xvfb", "-displayfd", str(write), "-screen", "0", "960x640x24",
                                    "-nolisten", "tcp", "-ac"], pass_fds=[write], stdout=log, stderr=log)
        self.processes.append(process)
        os.close(write)
        with os.fdopen(read) as stream:
            number = stream.readline().strip()
        assert number, "Disposable Xvfb failed to start"
        self.env.update(DISPLAY=":" + number, XDG_SESSION_TYPE="x11", QT_QPA_PLATFORM="xcb", LANDESK_ISOLATED_TEST="1")
        for key in ("XAUTHORITY", "QT_SCALE_FACTOR", "QT_AUTO_SCREEN_SCALE_FACTOR"):
            self.env.pop(key, None)
        # A private profile prevents Qt from persisting settings in the user's home.
        profile = self.directory / "profile"
        profile.mkdir()
        self.env.update(XDG_CONFIG_HOME=str(profile), XDG_DATA_HOME=str(profile), XDG_CACHE_HOME=str(profile))

    def open_log(self, name):
        stream = (self.directory / (name + ".log")).open("w")
        self.logs.append(stream)
        return stream

    def launch_host(self, extra=()):
        self.counter += 1
        root = self.directory / ("host-" + str(self.counter))
        root.mkdir()
        transfer = root / "transfer"
        transfer.mkdir()
        invitation = root / "invitation"
        command = [str(self.binary), "--host", "--bind", "127.0.0.1", "--port", str(unused_port()),
                   "--fps", "15", "--codec", "jpeg", "--no-lock-on-disconnect",
                   "--transfer-dir", str(transfer), "--invite-file", str(invitation), *extra]
        process = subprocess.Popen(command, env=self.env, stdout=self.open_log("host-" + str(self.counter)), stderr=subprocess.STDOUT)
        self.processes.append(process)
        wait_for(lambda: invitation.exists() or process.poll() is not None)
        assert process.poll() is None, "Host stopped before writing an invitation"
        return process, decode_invitation(invitation), transfer

    def tls(self, invitation):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        sock = context.wrap_socket(socket.create_connection((invitation["host"], invitation["port"]), timeout=4),
                                   server_hostname=invitation["host"])
        assert hashlib.sha256(sock.getpeercert(binary_form=True)).hexdigest() == invitation["pin"]
        self.sockets.append(sock)
        return sock

    def authenticated(self, invitation):
        sock = self.tls(invitation)
        send(sock, "A", {"v": 1, "token": invitation["token"], "codecs": ["jpeg"], "window": 1})
        kind, payload = receive(sock)
        assert kind == "W", kind
        welcome = json.loads(payload)
        assert receive(sock)[0] == "F"
        # Do not acknowledge the first frame: this keeps further video bounded
        # while exercising file traffic and heartbeat on the same TLS socket.
        return sock, welcome

    def check(self, name):
        self.checks.append(name)
        print("PASS", name, flush=True)

    def cleanup(self, failed=False):
        for sock in self.sockets:
            try:
                sock.close()
            except OSError:
                pass
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        for log in self.logs:
            log.close()
        if failed:
            for path in self.directory.glob("*.log"):
                print(path.name + ":\n" + path.read_text(errors="replace")[-5000:])
        if self.artifacts:
            self.artifacts.mkdir(parents=True, exist_ok=True)
            report = {"passed": len(self.checks), "checks": self.checks, "completed": not failed,
                      "platform": "Linux, private Xvfb, real LanDesk TLS over loopback",
                      "not_tested": ["physical Windows 10", "two physical machines", "file resume", "directory transfer"]}
            (self.artifacts / "file-transfer-integration.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
            if failed:
                for path in self.directory.glob("*.log"):
                    shutil.copyfile(path, self.artifacts / path.name)
        self.temporary.cleanup()


def send(sock, kind, payload=b""):
    if isinstance(payload, dict):
        payload = json.dumps(payload, separators=(",", ":")).encode()
    sock.sendall(struct.pack("!I", len(payload) + 1) + kind.encode("ascii") + payload)


def exact(sock, size):
    parts = []
    remaining = size
    while remaining:
        data = sock.recv(remaining)
        if not data:
            raise EOFError("TLS peer closed")
        parts.append(data)
        remaining -= len(data)
    return b"".join(parts)


def receive(sock):
    size, = struct.unpack("!I", exact(sock, 4))
    assert 1 <= size <= 8 * 1024 * 1024
    data = exact(sock, size)
    return chr(data[0]), data[1:]


def expect_closed(sock):
    try:
        while sock.recv(65536):
            pass
    except (ConnectionResetError, ssl.SSLError, EOFError):
        pass
    sock.close()
    time.sleep(0.12)


def heartbeat(sock):
    send(sock, "P")
    kind, data = receive(sock)
    assert kind == "Q" and not data, (kind, data[:100])


def identifier():
    return os.urandom(16).hex()


def file_command(sock, operation, request_id, **values):
    send(sock, "T", {"v": 1, "id": request_id, "op": operation, **values})


def file_response(sock, request_id, expected=None):
    kind, payload = receive(sock)
    assert kind == "T", (kind, payload[:100])
    assert len(payload) <= 65536
    response = json.loads(payload)
    assert response["v"] == 1 and response["id"] == request_id, response
    if expected:
        assert response["op"] == expected, response
    return response


def listing(sock):
    request_id = identifier()
    file_command(sock, "list", request_id)
    response = file_response(sock, request_id, "list")
    return {entry["name"]: int(entry["size"]) for entry in response["files"]}


def begin_upload(sock, name, size):
    request_id = identifier()
    file_command(sock, "put", request_id, name=name, size=size)
    response = file_response(sock, request_id, "ready")
    assert response["size"] == size and response["offset"] == 0
    return request_id


def upload_chunk(sock, request_id, offset, data):
    assert 1 <= len(data) <= 32768
    file_command(sock, "data", request_id, offset=offset, data=base64.b64encode(data).decode("ascii"))
    response = file_response(sock, request_id, "ack")
    assert response["offset"] == offset + len(data), response


def commit_upload(sock, request_id, size):
    file_command(sock, "commit", request_id, offset=size)
    assert file_response(sock, request_id, "done")["size"] == size


def upload(sock, name, data):
    request_id = begin_upload(sock, name, len(data))
    for offset in range(0, len(data), 32768):
        upload_chunk(sock, request_id, offset, data[offset:offset + 32768])
    commit_upload(sock, request_id, len(data))


def download(sock, name):
    request_id = identifier()
    file_command(sock, "get", request_id, name=name)
    response = file_response(sock, request_id, "ready")
    size = response["size"]
    assert response["offset"] == 0 and isinstance(size, int) and 0 <= size <= 8 * 1024 ** 3
    chunks, offset = [], 0
    while True:
        file_command(sock, "read", request_id, offset=offset)
        response = file_response(sock, request_id, "data")
        assert response["offset"] == offset and isinstance(response["eof"], bool)
        data = base64.b64decode(response["data"], validate=True)
        assert len(data) <= 32768 and (data or response["eof"])
        chunks.append(data)
        offset += len(data)
        assert offset <= size
        if response["eof"]:
            assert offset == size
            return b"".join(chunks)


def rejected_file_request(sock, operation, **values):
    request_id = identifier()
    file_command(sock, operation, request_id, **values)
    response = file_response(sock, request_id, "error")
    assert isinstance(response["error"], str) and response["error"]
    heartbeat(sock)


def rejected_permission(sock):
    request_id = identifier()
    file_command(sock, "put", request_id, name="must-not-exist.bin", size=0)
    try:
        kind, payload = receive(sock)
    except (EOFError, ConnectionResetError, ssl.SSLError):
        return
    assert kind in ("T", "E"), (kind, payload[:100])
    if kind == "T":
        response = json.loads(payload)
        assert response["op"] == "error", response
    else:
        assert payload


def run_tests(harness):
    _, invitation, transfer = harness.launch_host()
    sock = harness.tls(invitation)
    file_command(sock, "put", identifier(), name="before-auth.bin", size=0)
    expect_closed(sock)
    assert list(transfer.iterdir()) == []
    harness.check("File packets before authentication are rejected without creating files")

    sock, welcome = harness.authenticated(invitation)
    assert welcome.get("file_transfer") is True and welcome["control"] is True
    assert listing(sock) == {}
    harness.check("Authenticated host advertises file capability and lists only the transfer directory")

    data = bytes(range(256)) * 389 + b"\x00\xff\x80\x00LanDesk-binary\x00"
    upload(sock, "binary-test.dat", data)
    disk = (transfer / "binary-test.dat").read_bytes()
    assert hashlib.sha256(disk).digest() == hashlib.sha256(data).digest()
    assert listing(sock)["binary-test.dat"] == len(data)
    harness.check("Multi-chunk binary upload over real TLS accepts File payloads larger than 4 KiB and preserves SHA-256")

    result = download(sock, "binary-test.dat")
    assert hashlib.sha256(result).digest() == hashlib.sha256(data).digest()
    heartbeat(sock)
    harness.check("Multi-chunk binary download preserves SHA-256 while the desktop TLS connection remains alive")

    upload(sock, "empty.dat", b"")
    assert (transfer / "empty.dat").read_bytes() == b"" and download(sock, "empty.dat") == b""
    harness.check("Zero-byte files complete in both directions")

    rejected_file_request(sock, "put", name="binary-test.dat", size=4)
    assert (transfer / "binary-test.dat").read_bytes() == data
    harness.check("Uploading an existing name cannot overwrite an existing host file")

    outside = transfer.parent / "outside-test.dat"
    outside.write_bytes(b"outside-transfer-root-sentinel")
    rejected_file_request(sock, "get", name="../outside-test.dat")
    rejected_file_request(sock, "put", name="../outside-test.dat", size=0)
    (transfer / "linked.dat").symlink_to(outside)
    rejected_file_request(sock, "get", name="linked.dat")
    rejected_file_request(sock, "put", name="linked.dat", size=0)
    assert outside.read_bytes() == b"outside-transfer-root-sentinel"
    assert "linked.dat" not in listing(sock)
    harness.check("Remote traversal and symlink targets cannot escape the dedicated transfer directory")

    old_id = begin_upload(sock, "cancelled.dat", 100000)
    upload_chunk(sock, old_id, 0, data[:32768])
    assert not (transfer / "cancelled.dat").exists()
    file_command(sock, "cancel", old_id)
    file_response(sock, old_id, "canceled")
    assert not (transfer / "cancelled.dat").exists()
    heartbeat(sock)
    assert "cancelled.dat" not in listing(sock)
    harness.check("Cancelling an upload removes partial output and preserves the remote desktop connection")

    new_id = begin_upload(sock, "after-cancel.dat", 5)
    file_command(sock, "cancel", old_id)
    assert file_response(sock, old_id)["op"] in ("error", "canceled")
    upload_chunk(sock, new_id, 0, b"fresh")
    commit_upload(sock, new_id, 5)
    assert download(sock, "after-cancel.dat") == b"fresh"
    harness.check("A stale cancellation cannot cancel a subsequent transfer")

    request_id = identifier()
    file_command(sock, "get", request_id, name="binary-test.dat")
    file_response(sock, request_id, "ready")
    file_command(sock, "read", request_id, offset=0)
    file_response(sock, request_id, "data")
    file_command(sock, "cancel", request_id)
    file_response(sock, request_id, "canceled")
    heartbeat(sock)
    assert download(sock, "empty.dat") == b""
    harness.check("Cancelling a download preserves the connection and allows another transfer")

    request_id = begin_upload(sock, "wrong-offset.dat", 10)
    file_command(sock, "data", request_id, offset=5, data=base64.b64encode(b"bad").decode("ascii"))
    file_response(sock, request_id, "error")
    heartbeat(sock)
    assert not (transfer / "wrong-offset.dat").exists()
    harness.check("Out-of-order file chunks are rejected without publishing partial files")

    request_id = begin_upload(sock, "disconnected.dat", 100000)
    upload_chunk(sock, request_id, 0, b"incomplete")
    sock.close()
    time.sleep(0.2)
    sock, _ = harness.authenticated(invitation)
    assert "disconnected.dat" not in listing(sock) and not (transfer / "disconnected.dat").exists()
    harness.check("TLS disconnect aborts partial uploads and the same invitation can reconnect")

    # Raising the per-file limit must not raise keyboard/control message limits.
    send(sock, "I", b" " * 4096)
    expect_closed(sock)
    sock, _ = harness.authenticated(invitation)
    sock.sendall(struct.pack("!I", 65538) + b"T")
    expect_closed(sock)
    harness.check("Oversized File and non-File messages retain their separate hard limits")

    _, view_invite, view_root = harness.launch_host(("--view-only",))
    sock, welcome = harness.authenticated(view_invite)
    assert welcome.get("file_transfer") is False
    rejected_permission(sock)
    assert list(view_root.iterdir()) == []
    sock.close()
    harness.check("View-only sessions cannot transfer files")

    _, disabled_invite, disabled_root = harness.launch_host(("--no-file-transfer",))
    sock, welcome = harness.authenticated(disabled_invite)
    assert welcome.get("file_transfer") is False
    rejected_permission(sock)
    assert list(disabled_root.iterdir()) == []
    sock.close()
    harness.check("Explicitly disabled file transfer rejects requests even for a control session")

    native = harness.binary.parent / "landesk_file_client_tests"
    assert native.exists(), "Build with LANDESK_BUILD_TESTS=ON to validate the actual native Client"
    result = subprocess.run([str(native)], env=harness.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=60)
    (harness.directory / "native-file-client.log").write_text(result.stdout)
    assert result.returncode == 0, result.stdout
    assert "0 failed" in result.stdout and "0 skipped" in result.stdout, result.stdout
    harness.check("Actual native Host/Client/FileTransferClient passes upload, download, listing, cancellation, and permission tests over TLS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--artifacts", type=Path)
    arguments = parser.parse_args()
    harness = Harness(arguments.binary, arguments.artifacts)
    failed = True
    try:
        run_tests(harness)
        failed = False
        print(json.dumps({"passed": len(harness.checks)}), flush=True)
    finally:
        harness.cleanup(failed)


if __name__ == "__main__":
    main()
