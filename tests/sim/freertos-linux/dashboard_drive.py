#!/usr/bin/env python3
# Usage: .venv/bin/python tests/sim/freertos-linux/dashboard_drive.py [logfile]
# Run from the repo root with the FreeRTOS an500 linux_interop workspace active.
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import time
from contextlib import suppress

from websockets.exceptions import WebSocketException
from websockets.sync.client import connect

sys.path.insert(0, "config/scripts")
from ove_dashboard.protocol import (  # noqa: E402
    FRAME_CMD, FRAME_FB, FRAME_INPUT, FRAME_LOG, FRAME_SERIAL, FRAME_SERIAL_TAIL, FRAME_THREAD,
    FRAME_TRACE,
)


def thread_snapshot_matches(frame):
    header = struct.Struct("<IB4I")  # Frame type, thread count, heap stats.
    pos = header.size
    if len(frame) < pos:
        return False
    _, count, heap_total, _, _, _ = header.unpack_from(frame)
    if heap_total != 262144:
        return False

    rows = {}
    for _ in range(count):
        if pos >= len(frame):
            return False
        name_end = pos + 1 + frame[pos]
        row_end = name_end + 42
        if row_end > len(frame):
            return False
        name = frame[pos + 1:name_end]
        rows[name], = struct.unpack_from("<I", frame, name_end + 2)  # Skip state and priority.
        pos = row_end
    cpu_percent = 1 << 2
    return (rows.get(b"IDLE", 0) & cpu_percent != 0
            and any(re.fullmatch(rb"lnx\d+", name) for name in rows)
            and b"sim_debug" in rows)


class TraceSummary:
    def __init__(self):
        self.names = {}
        self.state_tids = set()
        self.state_records = self.records = self.dropped = 0

    def add(self, frame):
        header = struct.Struct("<BBHI")
        pos = 4 + header.size
        sub, ver, count, dropped = header.unpack_from(frame, 4)
        self.dropped += dropped
        if sub == 0:  # DESCRIPTORS
            desc = struct.Struct("<IIB")
            for _ in range(count):
                tid, _, name_len = desc.unpack_from(frame, pos)
                pos += desc.size
                end = pos + name_len
                if end > len(frame):
                    raise RuntimeError("truncated FRAME_TRACE descriptor name")
                self.names[tid] = frame[pos:end]
                pos = end
        elif sub == 1:  # STREAM, ignoring NAMES batches
            if ver < 3:
                raise RuntimeError(f"FRAME_TRACE stream version {ver}, expected v3 or newer")
            record = struct.Struct("<QIBBHI4x")
            if len(frame) != pos + count * record.size:
                raise RuntimeError("invalid FRAME_TRACE stream batch size")
            self.records += count
            for _, tid, kind, _, _, _ in record.iter_unpack(memoryview(frame)[pos:]):
                if kind == 1:
                    self.state_tids.add(tid)
                    self.state_records += 1

    def check_lanes(self):
        names = {self.names.get(tid, b"") for tid in self.state_tids}
        if b"IDLE" not in names:
            raise RuntimeError("no FRAME_TRACE state records for IDLE")
        if not any(re.fullmatch(rb"lnx\d+", name) for name in names):
            raise RuntimeError("no FRAME_TRACE state records for an lnxN task")


signal.signal(signal.SIGTERM, signal.default_int_handler)
log = sys.argv[1] if len(sys.argv) > 1 else "/tmp/dashboard_drive.log"
proc, rc = None, 1
try:
    for name in ("fb", "sim", "audio", "input"):
        path = f"/dev/shm/ove-{name}"
        if os.path.lexists(path):
            raise RuntimeError(f"{path} exists. Stop any running dashboard before testing.")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    with open(log, "wb") as output:
        proc = subprocess.Popen(
            [".venv/bin/ove", "run", "--", "--no-gdb", "--timeout", "150"],
            stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
            env=dict(os.environ, OVE_DASHBOARD_PORT=str(port)), start_new_session=True)

    startup_deadline = time.monotonic() + 15
    while time.monotonic() < startup_deadline:
        with suppress(OSError):
            ws = connect(f"ws://127.0.0.1:{port}/ws", open_timeout=1)
            break
        time.sleep(0.1)
    else:
        raise TimeoutError("waiting for the dashboard WebSocket")

    with ws:
        start = time.monotonic()
        deadline = start + 120
        log_header = struct.pack("<I", FRAME_LOG)
        serial_headers = tuple(struct.pack("<I", kind) for kind in (FRAME_SERIAL, FRAME_SERIAL_TAIL))
        fb_header = struct.pack("<IHHHH", FRAME_FB, 0, 0, 479, 271)
        thread_header = struct.pack("<I", FRAME_THREAD)
        trace_header = struct.pack("<I", FRAME_TRACE)
        log_buffer = bytearray()
        serial_buffer = bytearray()
        thread_frames = []
        trace = TraceSummary()
        fbtest_seen = [False]

        def expect(needle):
            while time.monotonic() < deadline:
                pos = serial_buffer.find(needle)
                if pos != -1:
                    before = bytes(serial_buffer[:pos])
                    del serial_buffer[:pos + len(needle)]
                    return before
                with suppress(TimeoutError):
                    frame = ws.recv(timeout=1)
                    if not isinstance(frame, bytes):
                        continue
                    if frame.startswith(serial_headers):
                        serial_buffer.extend(frame[4:])
                    elif frame.startswith(log_header):
                        log_buffer.extend(frame[4:])
                        if b"overtos login" in log_buffer:
                            raise RuntimeError("guest login prompt appeared in FRAME_LOG")
                    elif frame.startswith(thread_header):
                        thread_frames.append(frame)
                    elif frame.startswith(trace_header):
                        trace.add(frame)
                    elif (not fbtest_seen[0] and frame.startswith(fb_header)
                          and len(frame) == len(fb_header) + 480 * 272 * 4):
                        px = memoryview(frame)[len(fb_header):].cast("I")
                        # fbtest's gradient starts with a pure blue pixel.
                        fbtest_seen[0] = px[0] == 0xFF0000FF and len(set(px)) > 1
            raise TimeoutError(f"waiting for {needle.decode()}")

        def send_command(command):
            ws.send(struct.pack("<IIII", FRAME_CMD, 0xFFFFFFFF, 0, len(command)) + command)

        expect(b"overtos login: ")
        send_command(b"root\r")
        expect(b"Password: ")
        send_command(b"root\r")
        expect(b"/root # ")
        send_command(b"fbtest\r")
        expect(b"fbtest: PASS")
        expect(b"/root # ")
        ws.send(struct.pack("<IhhB", FRAME_INPUT, 123, 45, 1))
        send_command(b"evread\r")
        reports = expect(b"evread: DONE got=")
        if reports.count(b"evread: touch x=123 y=45 down=1") < 3:
            raise RuntimeError("missing dashboard touch reports")
        ws.send(struct.pack("<IhhB", FRAME_INPUT, 123, 45, 0))
        expect(b"/root # ")
        send_command(b"uname -a\r")
        expect(b"Linux overtos")
        send_command(b"poweroff\r")
        elapsed = time.monotonic() - start

    if not fbtest_seen[0]:
        raise RuntimeError("no fbtest frame on the LCD")
    if not any(thread_snapshot_matches(frame) for frame in thread_frames):
        raise RuntimeError("no FRAME_THREAD with a 256 KiB heap, IDLE CPU %, an lnxN task, and sim_debug")
    trace.check_lanes()
    print(f"Trace: {trace.state_records} state records, {trace.records / elapsed:.0f} records/s, "
          f"{trace.dropped} dropped")
    qemu_exit = proc.wait(timeout=60)
    with open(log, "rb") as output:
        if b"Linux overtos" not in output.read():
            raise RuntimeError("Linux overtos missing from terminal output")
    rc = qemu_exit
    print(f"Linux overtos seen, QEMU exit {rc} ({log})")
except (OSError, RuntimeError, struct.error, subprocess.TimeoutExpired, WebSocketException) as exc:
    print(f"Dashboard smoke failed: {exc} ({log})")
finally:
    if proc is not None:
        with suppress(ProcessLookupError):
            os.killpg(proc.pid, signal.SIGTERM)
            time.sleep(0.5)
            os.killpg(proc.pid, signal.SIGKILL)
        proc.wait(timeout=5)

print("RESULT:", "PASS" if rc == 0 else "FAIL")
sys.exit(0 if rc == 0 else 1)
