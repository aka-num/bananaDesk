#!/usr/bin/env python3
"""Measure actual native-client decode/paint throughput on animated Xvfb desktops.

python3 tests/fps_benchmark.py /absolute/path/to/landesk --codec h264
Requires Linux, Xvfb and Python PyQt5. All desktops and TLS connections are local
and disposable. No input events are sent. Logs and JSON reports are retained.
This is a 1080p loopback benchmark, not a physical-LAN or monitor-refresh test.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import select
import socket
import ssl
import struct
import subprocess
import sys
import time


def latest(path):
    """Accept either an atomically replaced JSON snapshot or periodic JSONL."""
    try:
        contents = path.read_text()
        try:
            value = json.loads(contents)
            if isinstance(value, dict):
                return value
        except json.JSONDecodeError:
            pass
        for line in reversed(contents.splitlines()):
            try:
                value = json.loads(line)
                if isinstance(value, dict):
                    return value
            except json.JSONDecodeError:
                continue
    except FileNotFoundError:
        pass
    return {}


class Run:
    def __init__(self, directory):
        self.directory = directory
        self.processes = []
        self.handles = []

    def launch(self, command, env, name, pass_fds=()):
        handle = (self.directory / (name + ".log")).open("w")
        self.handles.append(handle)
        process = subprocess.Popen(command, env=env, stdout=handle,
                                   stderr=subprocess.STDOUT, pass_fds=pass_fds)
        self.processes.append(process)
        return process

    def wait(self, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for process in self.processes:
                if process.poll() is not None:
                    raise RuntimeError("Child process exited (%s): %s" % (process.returncode, process.args))
            result = predicate()
            if result:
                return result
            time.sleep(.04)
        raise TimeoutError("Timed out waiting for benchmark readiness/progress")

    def xvfb(self, name):
        read_fd, write_fd = os.pipe()
        try:
            self.launch(["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                         "1920x1080x24", "-nolisten", "tcp", "-ac"],
                        os.environ.copy(), name, pass_fds=(write_fd,))
            os.close(write_fd)
            write_fd = None
            if not select.select([read_fd], [], [], 10)[0]:
                raise TimeoutError("Xvfb did not publish a display number")
            number = os.read(read_fd, 128).decode().strip()
            if not number.isdigit():
                raise RuntimeError("Invalid Xvfb display number: " + repr(number))
        finally:
            os.close(read_fd)
            if write_fd is not None:
                os.close(write_fd)
        env = os.environ.copy()
        for key in ("XAUTHORITY", "QT_SCALE_FACTOR", "QT_AUTO_SCREEN_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "WAYLAND_DISPLAY"):
            env.pop(key, None)
        env.update(DISPLAY=":" + number, XDG_SESSION_TYPE="x11", QT_QPA_PLATFORM="xcb",
                   LANDESK_TEST_DESKTOP="1")
        return env

    def stop(self, process):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        self.processes.remove(process)

    def close(self):
        for process in list(reversed(self.processes)):
            self.stop(process)
        for handle in self.handles:
            handle.close()


def invitation(path):
    encoded = path.read_text().strip().split(":", 1)[1]
    return json.loads(base64.urlsafe_b64decode(encoded + "=" * (-len(encoded) % 4)))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def send(sock, kind, payload=b""):
    if isinstance(payload, dict):
        payload = json.dumps(payload).encode()
    sock.sendall(struct.pack("!I", len(payload) + 1) + kind.encode() + payload)


class Receiver:
    def __init__(self, sock):
        self.sock = sock
        self.pending = bytearray()

    def receive(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            if len(self.pending) >= 4:
                size, = struct.unpack("!I", self.pending[:4])
                if not 1 <= size <= 8 * 1024 * 1024:
                    raise AssertionError("Invalid server packet size: %d" % size)
                if len(self.pending) >= 4 + size:
                    body = bytes(self.pending[4:4 + size])
                    del self.pending[:4 + size]
                    return chr(body[0]), body[1:]
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.sock.settimeout(remaining)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not data:
                raise EOFError("Server closed slow-client connection")
            self.pending.extend(data)


def slow_receiver(invite, max_unacked):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    with socket.create_connection((invite["host"], invite["port"]), timeout=5) as raw:
        with context.wrap_socket(raw, server_hostname=invite["host"]) as sock:
            assert hashlib.sha256(sock.getpeercert(binary_form=True)).hexdigest() == invite["pin"]
            receiver = Receiver(sock)
            send(sock, "A", {"v": 1, "token": invite["token"], "codecs": ["h264", "jpeg"],
                              "window": max_unacked})
            # Hold all ACKs while continuously draining TLS. An unbounded server
            # cannot hide extra frames in socket buffers in this check.
            welcome, unacked, received_bytes = None, 0, 0
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                message = receiver.receive(deadline - time.monotonic())
                if message is None:
                    break
                kind, payload = message
                if kind == "W":
                    welcome = json.loads(payload)
                elif kind == "F":
                    unacked += 1
                    received_bytes += len(payload)
                    assert unacked <= max_unacked, "Host streams indefinitely without ACKs"
                else:
                    raise AssertionError("Unexpected slow-client packet: " + kind)
            assert welcome and unacked >= 1, "No authenticated video delivered"
            assert welcome.get("window") == max_unacked, "Host did not negotiate the requested frame window"
            assert unacked == max_unacked, "Host did not use the negotiated frame window"
            send(sock, "P")
            pong = receiver.receive(2)
            assert pong and pong[0] == "Q", "Backpressure blocked the heartbeat"
            send(sock, "K")
            resumed = receiver.receive(2)
            assert resumed and resumed[0] == "F", "Video did not resume after an ACK"
            additional = receiver.receive(.4)
            assert additional is None, "One ACK released more than one frame"
            return {"passed": True, "held_ack_seconds": 2, "frames_without_ack": unacked,
                    "max_allowed_unacked_frames": max_unacked, "bytes_without_ack": received_bytes,
                    "codec": welcome.get("codec"), "heartbeat_while_blocked": True,
                    "single_ack_resumed_one_frame": True}


def screenshot(env, path):
    code = ("from PyQt5.QtWidgets import QApplication; import sys; "
            "a=QApplication([]); ok=a.primaryScreen().grabWindow(0).save(sys.argv[1]); "
            "sys.exit(0 if ok else 1)")
    subprocess.run([sys.executable, "-c", code, str(path)], env=env,
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--codec", choices=("auto", "h264", "jpeg"), default="auto")
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--duration", type=float, default=10)
    parser.add_argument("--warmup", type=float, default=2)
    parser.add_argument("--min-fps", type=float, default=55)
    parser.add_argument("--max-unacked-frames", type=int, default=3)
    args = parser.parse_args()
    if args.duration < 2 or args.warmup < 0 or not 1 <= args.fps <= 120 or args.min_fps < 0 or args.max_unacked_frames < 1:
        parser.error("Invalid measurement duration, warmup, fps, threshold or frame window")
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error("Binary does not exist: " + str(binary))
    directory = (args.artifacts or Path("work") / time.strftime("fps-benchmark-%Y%m%d-%H%M%S")).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    run = Run(directory)
    report = {"passed": False, "requested_fps": args.fps, "requested_codec": args.codec,
              "minimum_fps": args.min_fps, "resolution": [1920, 1080],
              "scope": "Two isolated Xvfb desktops, real native host/client, TLS over loopback",
              "limitations": ["Not physical LAN performance", "Not physical monitor refresh or input latency", "Not Windows, Wayland or 4K"],
              "artifacts": str(directory)}
    try:
        host_env, client_env = run.xvfb("host-xvfb"), run.xvfb("client-xvfb")
        invite_file, stats_file = directory / "invite", directory / "client-stats.json"
        source_file = directory / "source-stats.json"
        for stale in (invite_file, stats_file, source_file):
            stale.unlink(missing_ok=True)
        host = run.launch([str(binary), "--host", "--view-only", "--bind", "127.0.0.1",
                           "--port", str(free_port()), "--invite-file", str(invite_file),
                           "--fps", str(args.fps), "--codec", args.codec], host_env, "host")
        run.wait(invite_file.exists)
        run.launch([sys.executable, str(Path(__file__).with_name("animated_desktop.py")),
                    "--stats-file", str(source_file), "--fps", "120"], host_env, "animation")
        run.wait(lambda: latest(source_file).get("source_frames", 0) > 50)
        client = run.launch([str(binary), "--connect-file", str(invite_file),
                             "--stats-file", str(stats_file)], client_env, "client")
        run.wait(lambda: latest(stats_file).get("painted_frames", 0) >= 30, timeout=20)
        if args.warmup:
            warm_start = time.monotonic()
            run.wait(lambda: time.monotonic() - warm_start >= args.warmup, timeout=args.warmup + 2)
        first, source_first = latest(stats_file), latest(source_file)
        required = ("elapsed_seconds", "decoded_frames", "painted_frames", "bytes_received", "codec", "target_fps")
        assert all(key in first for key in required), "Missing native-client statistics: " + repr(first)
        # Screenshot helpers execute only on the separate viewer Xvfb, never DISPLAY from the user.
        screenshot(client_env, directory / "viewer-start.png")
        run.wait(lambda: latest(stats_file).get("elapsed_seconds", 0) - first["elapsed_seconds"] >= args.duration,
                 timeout=args.duration + 15)
        last, source_last = latest(stats_file), latest(source_file)
        screenshot(client_env, directory / "viewer-end.png")
        screenshot(host_env, directory / "source-end.png")
        elapsed = last["elapsed_seconds"] - first["elapsed_seconds"]
        rates = {}
        for name in ("received_frames", "decoded_frames", "painted_frames", "distinct_decoded_frames", "distinct_painted_frames"):
            if name in first and name in last:
                delta = last[name] - first[name]
                assert delta >= 0, "Counter moved backwards: " + name
                rates[name.replace("_frames", "_fps")] = round(delta / elapsed, 3)
        byte_delta = last["bytes_received"] - first["bytes_received"]
        source_elapsed = source_last["elapsed_seconds"] - source_first["elapsed_seconds"]
        source_fps = (source_last["source_frames"] - source_first["source_frames"]) / source_elapsed
        report.update(measurement_seconds=round(elapsed, 3), codec=last["codec"],
                      rates=rates, source_actual_fps=round(source_fps, 3),
                      video_payload_mbps=round(byte_delta * 8 / elapsed / 1_000_000, 3),
                      stats_first=first, stats_last=last)
        run.stop(client)
        time.sleep(.2)
        report["backpressure"] = slow_receiver(invitation(invite_file), args.max_unacked_frames)
        criteria = {"source_has_sufficient_motion": source_fps >= args.min_fps,
                    "decoded_fps": rates["decoded_fps"] >= args.min_fps,
                    "painted_fps": rates["painted_fps"] >= args.min_fps,
                    "codec_matches_request": args.codec == "auto" or last["codec"] == args.codec,
                    "target_matches_request": last["target_fps"] == args.fps,
                    "backpressure": report["backpressure"]["passed"]}
        if "distinct_decoded_fps" in rates:
            criteria["distinct_decoded_fps"] = rates["distinct_decoded_fps"] >= args.min_fps
        else:
            report["limitations"].append("Client does not report distinct decoded contents; duplicate-frame rate unverified")
            criteria["distinct_decoded_fps"] = False
        if "distinct_painted_fps" in rates:
            criteria["distinct_painted_fps"] = rates["distinct_painted_fps"] >= args.min_fps
        report.update(criteria=criteria, passed=all(criteria.values()))
    except Exception as error:
        report["error"] = str(error)
        for path in directory.glob("*.log"):
            print("\n%s:\n%s" % (path.name, path.read_text()[-3000:]), file=sys.stderr)
    finally:
        run.close()
        # An invitation is a short-lived capability; do not retain it in artifacts.
        (directory / "invite").unlink(missing_ok=True)
        (directory / "fps-report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(report, indent=2, ensure_ascii=False), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
