# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import asyncio
import importlib.util
import io
import json
import os
import re
import socket
import struct
import sys
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from contextlib import (
    asynccontextmanager,
    contextmanager,
    redirect_stderr,
    redirect_stdout,
    suppress,
)
from http.client import HTTPResponse
from pathlib import Path
from queue import Empty, Queue
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import Mock, patch

from websockets.asyncio.client import connect
from websockets.asyncio.server import serve as websocket_serve
from websockets.datastructures import Headers
from websockets.http11 import Request

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
        path = Path(os.environ.get("OVE_DASHBOARD_TEST_BRIDGE", BRIDGE)).resolve()
        if path == BRIDGE.resolve():
            with patch.object(sys, "path", [str(path.parent), *sys.path]):
                from ove_dashboard import frames, gdb, protocol, server, shm
        else:
            # Older bridge revisions put all of these names in one module.
            spec = importlib.util.spec_from_file_location("dashboard_bridge", path)
            bridge = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(bridge)
            bridge.OVE_DIR = str(ROOT)
            frames = gdb = protocol = server = shm = bridge
        self.frames, self.debug, self.protocol, self.server, self.shm = (
            frames, gdb, protocol, server, shm
        )

        tempdir = TemporaryDirectory()
        self.addCleanup(tempdir.cleanup)
        self.tmp = tmp = Path(tempdir.name)
        paths = patch.multiple(
            self.shm, FB_PATH=tmp / "fb", AUDIO_PATH=tmp / "audio",
            SIM_SHM_PATH=tmp / "sim", INPUT_PATH=tmp / "input", input_shm=[None],
        )
        paths.start()
        self.addCleanup(paths.stop)
        framebuffer = patch.object(self.frames, "_last_fb_frame", None)
        framebuffer.start()
        self.addCleanup(framebuffer.stop)
        self.gdb = self.debug.GdbController("gdb", None, 0)
        self.gdb.proc = SimpleNamespace(stdin=io.StringIO())

    def test_gdb_mi_records(self):
        # Captured ARM GDB records with the checkout prefix removed from paths.
        cases = json.loads(Path(__file__).with_name("dashboard_bridge_mi.json").read_text())
        gdb = self.gdb

        def worker(target, args, daemon):
            return SimpleNamespace(start=lambda: target(*args))

        for name, expected in cases.items():
            with self.subTest(record=name):
                gdb.proc.stdout = io.StringIO(expected["line"] + "\n")
                entry = {"event": threading.Event(), "result": None}
                gdb._pending[1] = entry
                with patch.object(gdb, "_send_mi_sync", return_value=None), \
                        patch.object(gdb, "_handle_source_request") as source, \
                        patch.object(gdb, "_send_debug_resp") as response, \
                        patch.object(threading, "Thread", side_effect=worker), \
                        redirect_stdout(io.StringIO()) as stdout, \
                        redirect_stderr(io.StringIO()):
                    gdb._reader_loop()
                if name == "disassembly":
                    self.assertEqual(entry["result"]["data"]["asm_insns"][0]["inst"],
                                     "push\t{r7, lr}")
                self.assertEqual(entry["result"], expected.get("result"))
                self.assertEqual(entry["event"].is_set(), "result" in expected)
                for key, mock in (("responses", response), ("source_requests", source)):
                    self.assertEqual([list(call.args) for call in mock.call_args_list],
                                     expected.get(key, []))
                self.assertEqual(stdout.getvalue(), expected.get("stdout", ""))

    def test_gdb_reader_exits_on_truncated_record(self):
        self.gdb.proc.stdout = io.StringIO('3^done,asm_insns=[src_and_asm_line={line="46"')
        reader = threading.Thread(target=self.gdb._reader_loop, daemon=True)
        reader.start()
        reader.join(timeout=1)
        self.assertFalse(reader.is_alive(), "GDB reader did not exit at EOF")

    def test_breakpoint_rejects_control_characters(self):
        gdb = self.gdb
        send = gdb._send_mi_sync
        with patch.object(gdb, "_send_mi_sync", side_effect=lambda cmd: send(cmd, timeout=0)):
            for separator in ("\n", "\r", "\x00"):
                with self.subTest(separator=separator):
                    gdb.proc.stdin = io.StringIO()
                    gdb.handle_debug_cmd(debug_command(
                        self.protocol.DBG_CMD_BP_SET, f"first{separator}second.c", 7
                    ))
                    self.assertEqual(gdb.proc.stdin.getvalue(), "")

    def test_breakpoint_uses_explicit_source_location(self):
        gdb = self.gdb
        send = gdb._send_mi_sync
        with patch.object(gdb, "_send_mi_sync", side_effect=lambda cmd: send(cmd, timeout=0)):
            gdb.handle_debug_cmd(debug_command(self.protocol.DBG_CMD_BP_SET, '*a b"c\\d.c', 7))
        self.assertEqual(
            gdb.proc.stdin.getvalue(), r'1-break-insert --source "*a b\"c\\d.c" --line 7' + "\n"
        )

    def test_disassembly_clamps_large_count(self):
        for count, end_addr in ((12, 0x1030), (0xFFFFFFFF, 0x5000)):
            with self.subTest(count=count), patch.object(self.gdb, "_send_mi_sync") as send:
                data = struct.pack("<BII", self.protocol.DBG_CMD_DISASSEMBLE, 0x1000, count)
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
                    gdb.handle_debug_cmd(debug_command(self.protocol.DBG_CMD_SOURCE, path, 1, 0))
                    reply.assert_called_once()
                    response, data = reply.call_args.args
                    self.assertEqual(response, self.protocol.DBG_RESP_SOURCE)
                    self.assertEqual(bool(data["data"]["lines"]), expected)

    def test_gdb_connects_to_loopback(self):
        gdb = self.gdb
        gdb.gdb_port = 12345
        with patch.object(self.debug.subprocess, "Popen"), \
                patch.object(self.debug.threading, "Thread"), \
                patch.object(self.debug.time, "sleep"), \
                patch.object(gdb, "_send_mi_sync") as send:
            gdb.start()
        self.assertEqual(send.call_args_list[0].args, ("-target-select remote 127.0.0.1:12345",))

    def test_qemu_gdb_listens_on_loopback(self):
        script = Path(os.environ.get("OVE_DASHBOARD_TEST_QEMU_RUN", QEMU_RUN))
        endpoints = re.findall(r'-gdb "(tcp:[^"]+)"', script.read_text(encoding="utf-8"))
        self.assertEqual(endpoints, ["tcp:127.0.0.1:${GDB_PORT}"] * 2)

    @contextmanager
    def running_bridge(self):
        try:
            with socket.socket():
                pass
        except PermissionError as exc:
            self.skipTest(f"cannot create TCP socket: {exc}")

        bridge = self.server
        dashboard = self.tmp / "dashboard"
        dashboard.mkdir()
        (dashboard / "index.html").write_bytes(b"<html>dashboard</html>")
        ready = Queue()

        async def run():
            @asynccontextmanager
            async def listen(handler, host, port, **kwargs):
                loop = asyncio.get_running_loop()
                async with websocket_serve(handler, host, port, **kwargs) as server:
                    ready.put((server.sockets[0].getsockname(),
                               lambda: loop.call_soon_threadsafe(server.close)))
                    yield server

            with patch.object(bridge, "websocket_serve", listen), suppress(asyncio.CancelledError):
                await bridge.serve(0, dashboard)

        with ThreadPoolExecutor(max_workers=1) as pool:
            server = pool.submit(asyncio.run, run())
            try:
                address, shutdown = ready.get(timeout=2)
            except Empty:
                server.result(timeout=2)
                raise
            try:
                self.assertEqual(address[0], "127.0.0.1")
                yield address
            finally:
                shutdown()
                server.result(timeout=3)

    @contextmanager
    def http_response(self, address, request):
        with socket.create_connection(address, timeout=2) as sock, HTTPResponse(sock) as response:
            sock.sendall(request)
            response.begin()
            yield response

    def connect_websocket(self, address, **kwargs):
        sock = socket.create_connection(address, timeout=2)
        sock.setblocking(False)
        host, port = address
        return connect(f"ws://{host}:{port}/ws", sock=sock, **kwargs)

    def test_command_routing(self):
        server, shm = self.server, self.shm
        audio_inject, profiler_set_rate = 10, 20
        sent = []
        with patch.object(shm, "sim_cmd_send", lambda *args: sent.append(args)), \
                patch.object(shm, "audio_input_write", lambda pcm: sent.append(("audio", pcm))), \
                patch.object(shm, "_learned_profiler_id", 3, create=True), \
                patch.object(server, "console_fd", [None]):
            for plugin_id, cmd_type in (
                (0xFFFFFFFF, 0), (1, audio_inject), (0xFFFFFFFE, profiler_set_rate), (2, 0),
            ):
                frame = struct.pack("<IIII", self.protocol.FRAME_CMD, plugin_id, cmd_type, 2)
                asyncio.run(server._dispatch(frame + b"ab", None))
        self.assertEqual(sent, [("audio", b"ab"), (3, profiler_set_rate, b"ab"), (2, 0, b"ab")])

    def test_websocket_origins(self):
        host = "127.0.0.1:12345"
        cases = (
            (host, "http://evil.example", 403),
            (host, f"http://{host}", 101),
            (host, f"https://{host}", 403),
            (host, None, 101),
            ("localhost:12345", "http://localhost:12345", 101),
            ("[::1]:12345", "http://[::1]:12345", 101),
            ("evil.example:12345", "http://evil.example:12345", 403),
            ("evil.example:12345", None, 403),
        )
        bridge = self.server
        for authority, origin, status in cases:
            with self.subTest(host=authority, origin=origin):
                headers = Headers({"Host": authority, "Upgrade": "websocket"})
                if origin is not None:
                    headers["Origin"] = origin
                connection = Mock(respond=lambda status, _: status)
                result = bridge._process_request(connection, Request("/ws", headers),
                                                 "", bridge.DEFAULT_HOST)
                self.assertEqual(101 if result is None else result, status)

    def test_dashboard_websocket_roundtrip(self):
        shm, protocol = self.shm, self.protocol
        shm.INPUT_PATH.write_bytes(struct.pack("<I12x", shm.INPUT_MAGIC))
        sim_header = struct.pack("<I", shm.SIM_SHM_MAGIC)
        shm.SIM_SHM_PATH.write_bytes(sim_header.ljust(shm.SIM_SHM_TOTAL_SIZE, b"\0"))
        cmd = struct.pack("<III", 2, 1, 3) + b"cmd"
        ring_entry = struct.pack("<H", len(cmd)) + cmd
        pointer = struct.pack("<hhB", 25, -36, 1)

        async def exchange(address):
            host, port = address
            async with self.connect_websocket(address, origin=f"http://{host}:{port}") as peer:
                self.assertNotIn("Sec-WebSocket-Extensions", peer.response.headers)
                self.assertEqual(await peer.recv(), struct.pack("<II", protocol.FRAME_STATE, 0)
                                 + b'{"type":"audio","sample_rate":16000,"channels":1,"bit_depth":16}')
                await peer.send(struct.pack("<I", protocol.FRAME_INPUT) + pointer)
                await peer.send(struct.pack("<I", protocol.FRAME_CMD) + cmd)
                while struct.unpack_from("<I", shm.SIM_SHM_PATH.read_bytes(),
                                         shm.SIM_CMD_WRITE_POS_OFF)[0] == 0:
                    await asyncio.sleep(0.01)
                self.assertEqual(shm.INPUT_PATH.read_bytes()[4:9], pointer)
                self.addCleanup(shm.input_shm[0].close)
                off = shm.SIM_CMD_RING_OFF
                self.assertEqual(shm.SIM_SHM_PATH.read_bytes()[off:off + len(ring_entry)], ring_entry)

                pixels = b"\x01\x02\x03\xff" * 6
                shm.FB_PATH.write_bytes(struct.pack("<IHHII", shm.FB_MAGIC, 3, 2, 1, 1) + pixels)
                # Exit the display reader after its first dirty frame.
                with patch.object(shm, "time", Mock(sleep=Mock(side_effect=SystemExit))), \
                        self.assertRaises(SystemExit):
                    shm.display_bridge()
                self.assertEqual(await peer.recv(), struct.pack("<IHHHH", protocol.FRAME_FB, 0, 0, 2, 1)
                                 + pixels)

        with self.running_bridge() as address:
            with self.http_response(address, b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n") as response:
                self.assertEqual(response.status, 200)
                self.assertEqual(response.getheader("Cross-Origin-Opener-Policy"), "same-origin")
                self.assertEqual(response.getheader("Cross-Origin-Embedder-Policy"), "require-corp")
                self.assertEqual(response.getheader("Cache-Control"), "no-cache")
                self.assertEqual(response.read(), b"<html>dashboard</html>")
            request = b"GET /manifest.json HTTP/1.1\r\nHost: localhost\r\n\r\n"
            with patch.object(self.server, "manifest", {"app": "demo"}), \
                    self.http_response(address, request) as response:
                self.assertEqual(response.status, 200)
                self.assertEqual(response.getheader("Content-Type"), "application/json")
                self.assertEqual(json.loads(response.read()), {"app": "demo"})
            host, port = address
            for authority, origin in ((f"{host}:{port}", "http://evil.example"),
                                      (f"evil.example:{port}", f"http://evil.example:{port}")):
                with self.http_response(address, upgrade_request(authority, origin)) as response:
                    self.assertEqual(response.status, 403)
            asyncio.run(asyncio.wait_for(exchange(address), 4))

    def test_websocket_keepalive(self):
        async def check(address):
            async with self.connect_websocket(address, ping_interval=0.2, ping_timeout=0.5,
                                              close_timeout=0.5) as peer:
                await peer.recv()
                await asyncio.sleep(2)
                self.assertIsNone(peer.close_code, f"keepalive closed the connection: {peer.close_reason}")

        with self.running_bridge() as address:
            asyncio.run(asyncio.wait_for(check(address), 4))

    def test_event_bridge_reports_guest_drops(self):
        shm = self.shm
        # event_dropped (offset 32) = 3, no events to read.
        header = struct.pack("<I28xI", shm.SIM_SHM_MAGIC, 3)
        shm.SIM_SHM_PATH.write_bytes(header.ljust(shm.SIM_SHM_TOTAL_SIZE, b"\0"))
        posted = []
        with patch.object(self.frames, "post_frame", posted.append), \
                patch.object(shm, "time", Mock(sleep=Mock(side_effect=SystemExit))), \
                redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            shm._event_bridge_loop()
        self.assertEqual(posted, [struct.pack("<I", self.protocol.FRAME_LOG)
                                  + b"[sim] 3 event(s) dropped: host behind\n"])

    def test_log_bridge_flushes_prompt(self):
        read_fd, write_fd = os.pipe()
        posted = Queue()
        prefix = struct.pack("<I", self.protocol.FRAME_LOG)
        self.enterContext(patch.object(self.frames, "post_frame", posted.put))
        reader = threading.Thread(target=self.shm.log_bridge, args=(read_fd,), daemon=True)
        reader.start()
        self.addCleanup(reader.join, 1)
        writer = self.enterContext(open(write_fd, "wb", buffering=0))
        writer.write(b"boot\r\novertos login: caf\xc3")
        self.assertEqual(posted.get(timeout=1), prefix + b"boot\r\n")
        self.assertEqual(posted.get(timeout=1), prefix + b"overtos login: caf")
        writer.write(b"\xa9\xff\n")
        self.assertEqual(posted.get(timeout=1), prefix + "é�\n".encode())
        writer.close()
        reader.join(timeout=1)
        self.assertFalse(reader.is_alive())

    def test_websocket_log_burst(self):
        frame_delivery = self.frames
        prefix = struct.pack("<I", self.protocol.FRAME_LOG)
        frames = [prefix + f"log {index}".encode() for index in range(1000)]

        def post_burst():
            for frame in frames:
                frame_delivery.post_frame(frame)

        def queue_burst():
            # Queue all callbacks in one server loop turn.
            worker = threading.Thread(target=post_burst)
            worker.start()
            worker.join(timeout=2)

        async def check(address):
            async with self.connect_websocket(address, close_timeout=0.5) as peer:
                await peer.recv()
                frame_delivery._server_loop.call_soon_threadsafe(queue_burst)
                self.assertEqual([await peer.recv() for _ in frames], frames)

        with self.running_bridge() as address:
            asyncio.run(asyncio.wait_for(check(address), 5))
