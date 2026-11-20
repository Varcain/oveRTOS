# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import importlib.util
import io
import os
import re
import select
import socket
import struct
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from queue import Queue
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
BRIDGE = ROOT / "config/scripts/ove-dashboard-bridge.py"
QEMU_RUN = ROOT / "boards/qemu-mps2-an500/qemu-run.sh"


def debug_command(command, path, *args):
    path = str(path).encode()
    return struct.pack("<BH", command, len(path)) + path + struct.pack(
        "<" + "I" * len(args), *args
    )


def upgrade_request(host, origin=None):
    origin_header = f"Origin: {origin}\r\n" if origin is not None else ""
    return (
        f"GET /ws HTTP/1.1\r\nHost: {host}\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        f"Sec-WebSocket-Version: 13\r\n{origin_header}\r\n"
    ).encode()


class DashboardBridgeTest(unittest.TestCase):
    def setUp(self):
        path = os.environ.get("OVE_DASHBOARD_TEST_BRIDGE", BRIDGE)
        spec = importlib.util.spec_from_file_location("dashboard_bridge", path)
        self.bridge = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.bridge)
        if Path(path) != BRIDGE:
            self.bridge.OVE_DIR = str(ROOT)

        tempdir = TemporaryDirectory()
        self.addCleanup(tempdir.cleanup)
        self.tmp = tmp = Path(tempdir.name)
        paths = patch.multiple(
            self.bridge, FB_PATH=tmp / "fb", AUDIO_PATH=tmp / "audio",
            SIM_SHM_PATH=tmp / "sim", INPUT_PATH=tmp / "input",
        )
        paths.start()
        self.addCleanup(paths.stop)
        self.gdb = self.bridge.GdbController("gdb", None, 0)
        self.gdb.proc = SimpleNamespace(stdin=io.StringIO())

    def test_breakpoint_rejects_control_characters(self):
        gdb = self.gdb
        send = gdb._send_mi_sync
        with patch.object(gdb, "_send_mi_sync", side_effect=lambda cmd: send(cmd, timeout=0)):
            for separator in ("\n", "\r", "\x00"):
                with self.subTest(separator=separator):
                    gdb.proc.stdin = io.StringIO()
                    gdb.handle_debug_cmd(debug_command(
                        self.bridge.DBG_CMD_BP_SET, f"first{separator}second.c", 7
                    ))
                    self.assertEqual(gdb.proc.stdin.getvalue(), "")

    def test_breakpoint_uses_explicit_source_location(self):
        gdb = self.gdb
        send = gdb._send_mi_sync
        with patch.object(gdb, "_send_mi_sync", side_effect=lambda cmd: send(cmd, timeout=0)):
            gdb.handle_debug_cmd(debug_command(self.bridge.DBG_CMD_BP_SET, '*a b"c\\d.c', 7))
        self.assertEqual(
            gdb.proc.stdin.getvalue(), r'1-break-insert --source "*a b\"c\\d.c" --line 7' + "\n"
        )

    def test_disassembly_clamps_large_count(self):
        for count, end_addr in ((12, 0x1030), (0xFFFFFFFF, 0x5000)):
            with self.subTest(count=count), patch.object(self.gdb, "_send_mi_sync") as send:
                data = struct.pack("<BII", self.bridge.DBG_CMD_DISASSEMBLE, 0x1000, count)
                self.gdb.handle_debug_cmd(data)
                send.assert_called_once_with(f"-data-disassemble -s 0x1000 -e {end_addr:#x} -- 1")

    def test_source_is_confined_to_repository(self):
        gdb = self.gdb
        outside = self.tmp / "outside.c"
        outside.write_text("outside repository\n", encoding="utf-8")
        with patch.object(gdb, "_send_debug_resp") as reply:
            for path, expected in ((ROOT / "Makefile", True), (outside, False)):
                with self.subTest(path=path):
                    reply.reset_mock()
                    gdb.handle_debug_cmd(debug_command(self.bridge.DBG_CMD_SOURCE, path, 1, 0))
                    reply.assert_called_once()
                    response, data = reply.call_args.args
                    self.assertEqual(response, self.bridge.DBG_RESP_SOURCE)
                    self.assertEqual(bool(data["data"]["lines"]), expected)

    def test_gdb_connects_to_loopback(self):
        gdb = self.gdb
        gdb.gdb_port = 12345
        with patch.object(self.bridge.subprocess, "Popen"), \
                patch.object(self.bridge.threading, "Thread"), \
                patch.object(self.bridge.time, "sleep"), \
                patch.object(gdb, "_send_mi_sync") as send:
            gdb.start()
        self.assertEqual(send.call_args_list[0].args, ("-target-select remote 127.0.0.1:12345",))

    def test_qemu_gdb_listens_on_loopback(self):
        script = Path(os.environ.get("OVE_DASHBOARD_TEST_QEMU_RUN", QEMU_RUN))
        endpoints = re.findall(r'-gdb "(tcp:[^"]+)"', script.read_text(encoding="utf-8"))
        self.assertEqual(endpoints, ["tcp:127.0.0.1:${GDB_PORT}"] * 2)

    def test_default_bind(self):
        listener = Mock()
        listener.listen.side_effect = StopIteration
        with patch.object(self.bridge.socket, "socket", return_value=listener):
            with self.assertRaises(StopIteration):
                self.bridge.serve(0, "")
        listener.bind.assert_called_once_with(("127.0.0.1", 0))

    def test_websocket_origins(self):
        host = "127.0.0.1:12345"
        cases = (
            (host, "http://evil.example", b"403"),
            (host, f"http://{host}", b"101"),
            (host, f"https://{host}", b"403"),
            (host, None, b"101"),
            ("localhost:12345", "http://localhost:12345", b"101"),
            ("[::1]:12345", "http://[::1]:12345", b"101"),
            ("evil.example:12345", "http://evil.example:12345", b"403"),
            ("evil.example:12345", None, b"403"),
        )
        for authority, origin, status in cases:
            with self.subTest(host=authority, origin=origin):
                client = self.bridge.Client(Mock(), None)
                client.buf = upgrade_request(authority, origin)
                with patch.object(self.bridge, "_send_http",
                                  side_effect=lambda c, data: c.sock.sendall(data)):
                    self.bridge._handle_http(client, "")
                self.assertEqual(client.sock.sendall.call_args_list[0].args[0].split()[1], status)

    def test_loopback_upgrade(self):
        try:
            with socket.socket():
                pass
        except PermissionError as exc:
            self.skipTest(f"cannot create TCP socket: {exc}")

        address = Queue()
        stop = threading.Event()
        listener = None

        def poll(readers, writers, errors, timeout):
            nonlocal listener
            if listener is None:
                listener = readers[0]
                address.put(listener.getsockname())
            if stop.is_set():
                for sock in readers:
                    sock.close()
                raise StopIteration
            return select.select(readers, writers, errors, timeout)

        with patch.object(self.bridge, "select", SimpleNamespace(select=poll)):
            with ThreadPoolExecutor(max_workers=1) as pool:
                server = pool.submit(self.bridge.serve, 0, "")
                try:
                    host, port = address.get(timeout=2)
                    authority = f"127.0.0.1:{port}"
                    rebinding = f"evil.example:{port}"
                    for name, origin, status in (
                        (authority, "http://evil.example", b"403"),
                        (rebinding, f"http://{rebinding}", b"403"),
                        (authority, f"http://{authority}", b"101"),
                    ):
                        with self.subTest(host=name, origin=origin):
                            with socket.create_connection(("127.0.0.1", port), timeout=2) as peer:
                                peer.sendall(upgrade_request(name, origin))
                                with peer.makefile("rb") as response:
                                    self.assertEqual(response.readline().split()[1], status)
                    self.assertEqual(host, "127.0.0.1")
                finally:
                    stop.set()
                    with self.assertRaises(StopIteration):
                        server.result(timeout=2)
