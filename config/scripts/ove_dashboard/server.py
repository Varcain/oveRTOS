# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import asyncio
import ipaddress
import json
import os
import struct
from contextlib import suppress
from http import HTTPStatus
from urllib.parse import urlsplit

from websockets.asyncio.server import serve as websocket_serve
from websockets.datastructures import Headers
from websockets.exceptions import ConnectionClosed
from websockets.http11 import Response

from . import frames, profiler, shm, sources
from . import gdb as gdb_bridge
from .protocol import (
    DBG_RESP_STATE,
    FRAME_AUDIO,
    FRAME_CMD,
    FRAME_DEBUG_CMD,
    FRAME_DEBUG_RESP,
    FRAME_FILE_REQ,
    FRAME_INPUT,
    FRAME_STATE,
)

DEFAULT_HOST = "127.0.0.1"

# Write end of the firmware console UART's input FIFO (set in main if
# --console-fifo).
console_fd = [None]

# ── HTTP and WebSocket server ────────────────────────────────────────

MIME = {
    ".html": "text/html", ".js": "application/javascript",
    ".css": "text/css", ".png": "image/png",
    ".svg": "image/svg+xml", ".json": "application/json",
}

def _ws_host_allowed(authority, bind_host):
    try:
        host = urlsplit(f"//{authority}")
        if host.netloc != authority or host.username is not None:
            return False
        if host.hostname in ("localhost", bind_host.lower()):
            return True
        ipaddress.ip_address(host.hostname)
    except ValueError:
        return False
    return True


def _process_request(connection, request, dashboard_dir, bind_host):
    path = request.path.split("?", 1)[0]
    if path == "/ws" and request.headers.get_all("Upgrade"):
        hosts = request.headers.get_all("Host")
        origins = request.headers.get_all("Origin")
        if (len(hosts) != 1 or not _ws_host_allowed(hosts[0], bind_host)
                or len(origins) > 1
                or (origins and origins[0].lower() != f"http://{hosts[0].lower()}")):
            return connection.respond(HTTPStatus.FORBIDDEN, "Forbidden\n")
        return None  # Let websockets validate the upgrade.

    if path == "/":
        path = "/index.html"
    filepath = os.path.realpath(os.path.join(dashboard_dir, path.lstrip("/")))
    if os.path.commonpath((dashboard_dir, filepath)) != dashboard_dir:
        return connection.respond(HTTPStatus.FORBIDDEN, "Forbidden\n")
    if not os.path.isfile(filepath):
        return connection.respond(HTTPStatus.NOT_FOUND, "Not Found\n")

    ext = os.path.splitext(filepath)[1]
    with open(filepath, "rb") as f:
        body = f.read()
    return Response(200, "OK", Headers([
        ("Content-Type", MIME.get(ext, "application/octet-stream")),
        ("Content-Length", str(len(body))),
        ("Cross-Origin-Opener-Policy", "same-origin"),
        ("Cross-Origin-Embedder-Policy", "require-corp"),
        ("Cache-Control", "no-cache"),
        ("Connection", "close"),
    ]), body)


async def _send_frames(connection, pending, last_fb):
    try:
        # Catch up on framebuffer changes during the initial sends.
        starting = True
        while True:
            if (starting or pending.empty()) and last_fb is not frames._last_fb_frame:
                last_fb = frames._last_fb_frame
                await asyncio.wait_for(connection.send(last_fb), timeout=2)
            starting = False
            frame = await pending.get()
            if frame is not None:
                await asyncio.wait_for(connection.send(frame), timeout=2)
    except ConnectionClosed:
        pass
    except asyncio.TimeoutError:  # noqa: UP041 - wait_for uses this type on Python 3.9 and 3.10.
        connection.transport.abort()


def _initial_frames():
    audio_id = 1 if os.path.exists(shm.FB_PATH) else 0
    state = b'{"type":"audio","sample_rate":16000,"channels":1,"bit_depth":16}'
    yield struct.pack("<II", FRAME_STATE, audio_id) + state

    gdb = gdb_bridge.gdb_controller[0]
    if gdb and gdb.proc:
        state = {"state": "running", "reason": None, "file": None, "line": None}
        dbg_json = json.dumps({"status": "done", "data": state}).encode("utf-8")
        yield struct.pack("<IB", FRAME_DEBUG_RESP, DBG_RESP_STATE) + dbg_json

    for frame in (sources._project_file_list_frame, profiler._profiler_symbol_frame,
                  shm._profiler_caps_frame):
        if frame:
            yield frame


async def _dispatch(payload, debug_lock):
    if not isinstance(payload, bytes) or len(payload) < 4:
        return
    ftype, fdata = struct.unpack_from("<I", payload)[0], payload[4:]
    if ftype == FRAME_CMD and len(fdata) >= 12:
        plugin_id, cmd_type = struct.unpack_from("<II", fdata)
        pcm = fdata[12:]
        if plugin_id == 0xFFFFFFFF and console_fd[0] is not None:
            # Console text goes straight to the firmware's UART.
            with suppress(OSError):
                os.write(console_fd[0], pcm)
        elif plugin_id != 0xFFFFFFFF and cmd_type == 0 and pcm:
            shm.audio_input_write(pcm)
        else:
            shm.sim_cmd_send(plugin_id, cmd_type, pcm)
    elif ftype == FRAME_AUDIO and len(fdata) >= 8:
        # Raw audio frame from dashboard mic
        pcm = fdata[8:]
        if pcm:
            shm.audio_input_write(pcm)
    elif ftype == FRAME_INPUT and len(fdata) >= 5:
        shm.input_shm_write(*struct.unpack_from("<hhB", fdata))
    elif ftype == FRAME_DEBUG_CMD and fdata:
        gdb = gdb_bridge.gdb_controller[0]
        if gdb:
            async with debug_lock:
                await asyncio.to_thread(gdb.handle_debug_cmd, fdata)
    elif ftype == FRAME_FILE_REQ and fdata:
        sources.handle_file_request(fdata)


async def serve(port, dashboard_dir, host=DEFAULT_HOST):
    dashboard_dir = os.path.realpath(dashboard_dir)
    debug_lock = asyncio.Lock()

    async def handle(connection):
        pending = asyncio.Queue()
        frames._clients[connection] = pending
        sender = None
        try:
            last_fb = frames._last_fb_frame
            if last_fb:
                await asyncio.wait_for(connection.send(last_fb), timeout=2)
            for frame in _initial_frames():
                await asyncio.wait_for(connection.send(frame), timeout=2)
            print("WebSocket client connected", flush=True)
            sender = asyncio.create_task(_send_frames(connection, pending, last_fb))
            async for payload in connection:
                await _dispatch(payload, debug_lock)
        except ConnectionClosed:
            pass
        except asyncio.TimeoutError:  # noqa: UP041 - wait_for uses this type on Python 3.9 and 3.10.
            connection.transport.abort()
        finally:
            del frames._clients[connection]
            if sender is not None:
                sender.cancel()
                with suppress(asyncio.CancelledError):
                    await sender

    def process_request(connection, request):
        return _process_request(connection, request, dashboard_dir, host)

    async with websocket_serve(handle, host, port, compression=None,
                               process_request=process_request) as server:
        bound_port = server.sockets[0].getsockname()[1]
        print(f"Dashboard: http://{host}:{bound_port}", flush=True)
        frames._server_loop = asyncio.get_running_loop()
        try:
            await server.serve_forever()
        finally:
            frames._server_loop = None
