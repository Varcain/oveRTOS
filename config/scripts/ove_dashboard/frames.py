# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Queue frames from bridge threads for the WebSocket server."""

from contextlib import suppress

_server_loop = None
_clients = {}
_last_fb_frame = None


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
