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
from ove_dashboard.protocol import FRAME_CMD, FRAME_LOG  # noqa: E402

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
        log_buffer = bytearray()

        def expect(needle):
            while time.monotonic() < deadline:
                pos = log_buffer.find(needle)
                if pos != -1:
                    del log_buffer[:pos + len(needle)]
                    return
                with suppress(TimeoutError):
                    frame = ws.recv(timeout=1)
                    if isinstance(frame, bytes) and frame.startswith(log_header):
                        log_buffer.extend(frame[4:])
            raise TimeoutError(f"waiting for {needle.decode()}")

        for needle, command in ((b"overtos login: ", b"root\n"), (b"Password: ", b"root\n"),
                                (b"/root # ", b"uname -a\n"), (b"Linux overtos", b"poweroff\n")):
            expect(needle)
            ws.send(struct.pack("<IIII", FRAME_CMD, 0xFFFFFFFF, 0, len(command)) + command)

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
