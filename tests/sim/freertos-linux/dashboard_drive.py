#!/usr/bin/env python3
# Usage: .venv/bin/python tests/sim/freertos-linux/dashboard_drive.py [logfile]
# Run from the repo root with the FreeRTOS an500 linux_interop workspace active.
import os
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
from ove_dashboard.protocol import FRAME_CMD, FRAME_FB, FRAME_INPUT, FRAME_LOG  # noqa: E402

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
        deadline = time.monotonic() + 120
        log_header = struct.pack("<I", FRAME_LOG)
        fb_header = struct.pack("<IHHHH", FRAME_FB, 0, 0, 479, 271)
        log_buffer = bytearray()
        fbtest_seen = [False]

        def expect(needle):
            while time.monotonic() < deadline:
                pos = log_buffer.find(needle)
                if pos != -1:
                    before = bytes(log_buffer[:pos])
                    del log_buffer[:pos + len(needle)]
                    return before
                with suppress(TimeoutError):
                    frame = ws.recv(timeout=1)
                    if not isinstance(frame, bytes):
                        continue
                    if frame.startswith(log_header):
                        log_buffer.extend(frame[4:])
                    elif (not fbtest_seen[0] and frame.startswith(fb_header)
                          and len(frame) == len(fb_header) + 480 * 272 * 4):
                        px = memoryview(frame)[len(fb_header):].cast("I")
                        # fbtest's gradient starts with a pure blue pixel.
                        fbtest_seen[0] = px[0] == 0xFF0000FF and len(set(px)) > 1
            raise TimeoutError(f"waiting for {needle.decode()}")

        def send_command(command):
            ws.send(struct.pack("<IIII", FRAME_CMD, 0xFFFFFFFF, 0, len(command)) + command)

        expect(b"overtos login: ")
        send_command(b"root\n")
        expect(b"Password: ")
        send_command(b"root\n")
        expect(b"/root # ")
        send_command(b"fbtest\n")
        expect(b"fbtest: PASS")
        expect(b"/root # ")
        ws.send(struct.pack("<IhhB", FRAME_INPUT, 123, 45, 1))
        send_command(b"evread\n")
        reports = expect(b"evread: DONE got=")
        if reports.count(b"evread: touch x=123 y=45 down=1") < 3:
            raise RuntimeError("missing dashboard touch reports")
        ws.send(struct.pack("<IhhB", FRAME_INPUT, 123, 45, 0))
        expect(b"/root # ")
        send_command(b"uname -a\n")
        expect(b"Linux overtos")
        send_command(b"poweroff\n")

    if not fbtest_seen[0]:
        raise RuntimeError("no fbtest frame on the LCD")
    rc = proc.wait(timeout=60)
    print(f"Linux overtos seen, QEMU exit {rc} ({log})")
except (OSError, RuntimeError, subprocess.TimeoutExpired, WebSocketException) as exc:
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
