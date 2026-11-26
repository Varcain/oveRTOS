# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Queue frames from bridge threads for the WebSocket server."""

import struct
from contextlib import suppress
from threading import Lock

from .protocol import FRAME_SERIAL

SERIAL_TAIL_SIZE = 64 * 1024

_server_loop = None
_clients = {}
_last_fb_frame = None
_serial_tail = bytearray()
_serial_lock = Lock()  # Protect the handoff from the serial thread to the server loop.


def post_frame(frame):
    # None wakes the sender to check the latest framebuffer.
    loop = _server_loop
    if loop is not None:
        with suppress(RuntimeError):  # The loop may close during shutdown.
            loop.call_soon_threadsafe(_publish_frame, frame)


def _publish_frame(frame):
    for pending in _clients.values():
        if frame is not None or pending.empty():
            pending.put_nowait(frame)


def post_serial(data):
    with _serial_lock:
        loop = _server_loop
        if loop is None:
            _keep_serial(data)
            return
    with suppress(RuntimeError):  # The loop may close during shutdown.
        loop.call_soon_threadsafe(_publish_serial, data)


def _publish_serial(data):
    # Update the replay and the live queues in the same loop callback.
    _keep_serial(data)
    _publish_frame(struct.pack("<I", FRAME_SERIAL) + data)


def _keep_serial(data):
    _serial_tail.extend(data)
    excess = len(_serial_tail) - SERIAL_TAIL_SIZE
    if excess > 0:
        newline = _serial_tail.find(b"\n", excess)
        del _serial_tail[:newline + 1 if newline >= 0 else excess]
