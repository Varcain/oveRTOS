#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""
QEMU → browser dashboard bridge for oveRTOS.

Reads the framebuffer from /dev/shm/ove-fb and audio ring from
/dev/shm/ove-audio (written by QEMU guest via semihosting), and
streams them to the browser dashboard over WebSocket.

Architecture mirrors sim/src/ove_sim_ws.c:
  - Single server thread: accept, poll, read, write (no blocking).
  - Shmem bridge threads post frames into a mailbox.
  - Server thread drains mailbox and writes WS frames to clients.
"""

import argparse
import array
import base64
import hashlib
import mmap
import os
import select
import socket
import struct
import sys
import threading
import time
import traceback

# ── Shared-memory constants ──────────────────────────────────────────

FB_PATH = "/dev/shm/ove-fb"
FB_MAGIC = 0x42465854
FB_HEADER_SIZE = 16

AUDIO_PATH = "/dev/shm/ove-audio"
AUDIO_SHM_MAGIC = 0x4F564155
AUDIO_HDR_SIZE = 64
AUDIO_OFF_MAGIC = 0
AUDIO_OFF_SAMPLE_RATE = 4
AUDIO_OFF_CHANNELS = 8
AUDIO_OFF_BIT_DEPTH = 10
AUDIO_OFF_RING_SIZE = 16
AUDIO_OFF_OUT_WPOS = 20
AUDIO_OFF_OUT_RPOS = 24

FRAME_FB    = 0x01
FRAME_AUDIO = 0x02
FRAME_LOG   = 0x06

# ── RGB565 → XRGB8888 ───────────────────────────────────────────────

_LUT = array.array("I", [0] * 65536)
for _i in range(65536):
    _r = ((_i >> 11) & 0x1F) * 255 // 31
    _g = ((_i >> 5) & 0x3F) * 255 // 63
    _b = (_i & 0x1F) * 255 // 31
    _LUT[_i] = _b | (_g << 8) | (_r << 16) | 0xFF000000

def rgb565_to_xrgb8888(data, npixels):
    pixels = struct.unpack_from(f"<{npixels}H", data)
    return array.array("I", (_LUT[px] for px in pixels)).tobytes()

# ── WebSocket helpers ────────────────────────────────────────────────

WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

def ws_accept(key):
    return base64.b64encode(
        hashlib.sha1(key.strip().encode() + WS_GUID).digest()
    ).decode()

def ws_frame(data):
    n = len(data)
    if n < 126:
        hdr = bytes([0x82, n])
    elif n < 65536:
        hdr = struct.pack(">BBH", 0x82, 126, n)
    else:
        hdr = struct.pack(">BBQ", 0x82, 127, n)
    return hdr + data

# ── Mailbox (single-slot, latest-wins) ───────────────────────────────

class Mailbox:
    def __init__(self):
        self._lock = threading.Lock()
        self._fb = None
        self._audio = None
        self._logs = []

    def post_fb(self, frame):
        with self._lock:
            self._fb = frame

    def post_audio(self, frame):
        with self._lock:
            self._audio = frame

    def post_log(self, frame):
        with self._lock:
            self._logs.append(frame)

    def drain(self):
        with self._lock:
            fb, audio, logs = self._fb, self._audio, self._logs
            self._fb = self._audio = None
            self._logs = []
        return fb, audio, logs

mailbox = Mailbox()
last_fb_frame = [None]  # cached for late-joining clients

# ── Single-threaded server (mirrors ove_sim_ws.c) ────────────────────

MIME = {
    ".html": "text/html", ".js": "application/javascript",
    ".css": "text/css", ".png": "image/png",
    ".svg": "image/svg+xml", ".json": "application/json",
}

ST_HTTP = 0
ST_WS   = 1

class Client:
    __slots__ = ("sock", "addr", "state", "buf")
    def __init__(self, sock, addr):
        self.sock = sock
        self.addr = addr
        self.state = ST_HTTP
        self.buf = b""

def serve(port, dashboard_dir):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(8)
    srv.setblocking(False)
    print(f"Dashboard: http://localhost:{port}", flush=True)

    clients = []

    while True:
        # Build poll list: server + all client sockets
        rlist = [srv] + [c.sock for c in clients]
        readable, _, _ = select.select(rlist, [], [], 0.015)

        # Accept new connections
        if srv in readable:
            try:
                csock, caddr = srv.accept()
                csock.setblocking(False)
                csock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                clients.append(Client(csock, caddr))
            except BlockingIOError:
                pass

        # Read from clients
        dead = []
        for c in clients:
            if c.sock not in readable:
                continue
            try:
                data = c.sock.recv(8192)
            except (BlockingIOError, ConnectionError):
                data = None
            if not data:
                dead.append(c)
                continue
            if c.state == ST_HTTP:
                c.buf += data
                if b"\r\n\r\n" in c.buf:
                    _handle_http(c, dashboard_dir)

        for c in dead:
            clients.remove(c)
            c.sock.close()

        # Drain mailbox and broadcast to WS clients
        fb, audio, logs = mailbox.drain()
        ws_clients = [c for c in clients if c.state == ST_WS]
        frames = [f for f in (fb, audio) if f is not None] + logs
        for frame in frames:
            dead2 = []
            for c in ws_clients:
                try:
                    _send_blocking(c.sock, frame)
                except (ConnectionError, OSError, TimeoutError):
                    dead2.append(c)
            for c in dead2:
                if c in clients:
                    clients.remove(c)
                try:
                    c.sock.close()
                except Exception:
                    pass


def _send_blocking(sock, data):
    """Temporarily switch to blocking mode for a reliable send."""
    sock.setblocking(True)
    sock.settimeout(5)
    try:
        sock.sendall(data)
    finally:
        sock.setblocking(False)


def _handle_http(c, dashboard_dir):
    request = c.buf.decode(errors="replace")
    c.buf = b""

    lines = request.split("\r\n")
    parts = lines[0].split(" ")
    path = parts[1] if len(parts) > 1 else "/"

    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()

    # WebSocket upgrade?
    if (path == "/ws"
            and "upgrade" in headers.get("connection", "").lower()
            and "websocket" in headers.get("upgrade", "").lower()):
        key = headers.get("sec-websocket-key", "")
        accept = ws_accept(key)
        resp = (f"HTTP/1.1 101 Switching Protocols\r\n"
                f"Upgrade: websocket\r\n"
                f"Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n"
                f"\r\n").encode()
        # Temporarily blocking for handshake + cached frame.
        c.sock.setblocking(True)
        c.sock.settimeout(5)
        try:
            c.sock.sendall(resp)
            if last_fb_frame[0]:
                c.sock.sendall(last_fb_frame[0])
        except Exception:
            c.sock.close()
            return
        c.sock.setblocking(False)
        c.state = ST_WS
        print("WebSocket client connected", flush=True)
        return

    # Static file
    if path == "/":
        path = "/index.html"
    filepath = os.path.realpath(os.path.join(dashboard_dir, path.lstrip("/")))
    if not filepath.startswith(os.path.realpath(dashboard_dir)):
        _send_blocking(c.sock, b"HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n")
        return

    if not os.path.isfile(filepath):
        _send_blocking(c.sock, b"HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n")
        return

    ext = os.path.splitext(filepath)[1]
    ct = MIME.get(ext, "application/octet-stream")
    body = open(filepath, "rb").read()
    hdr = (f"HTTP/1.1 200 OK\r\n"
           f"Content-Type: {ct}\r\n"
           f"Content-Length: {len(body)}\r\n"
           f"Connection: close\r\n"
           f"\r\n").encode()
    _send_blocking(c.sock, hdr + body)

# ── Shmem bridges ────────────────────────────────────────────────────

def display_bridge():
    while not os.path.exists(FB_PATH):
        time.sleep(0.1)
    with open(FB_PATH, "r+b") as f:
        mm = mmap.mmap(f.fileno(), 0)
        try:
            while True:
                magic, w, h, fmt, dirty = struct.unpack(
                    "<IHHII", mm[:FB_HEADER_SIZE])
                if magic == FB_MAGIC and w > 0 and h > 0 and dirty:
                    npixels = w * h
                    pdata = bytes(mm[FB_HEADER_SIZE:FB_HEADER_SIZE+npixels*2])
                    struct.pack_into("<I", mm, 12, 0)
                    xrgb = rgb565_to_xrgb8888(pdata, npixels)
                    coords = struct.pack("<HHHH", 0, 0, w-1, h-1)
                    payload = struct.pack("<I", FRAME_FB) + coords + xrgb
                    frame = ws_frame(payload)
                    last_fb_frame[0] = frame
                    mailbox.post_fb(frame)
                time.sleep(1.0 / 30)
        except Exception:
            traceback.print_exc()
        finally:
            mm.close()


def audio_bridge():
    while not os.path.exists(AUDIO_PATH):
        time.sleep(0.2)
    with open(AUDIO_PATH, "r+b") as f:
        fd = f.fileno()
        mm = mmap.mmap(fd, 0)
        out_rpos = 0
        try:
            while True:
                magic = struct.unpack_from("<I", mm, AUDIO_OFF_MAGIC)[0]
                if magic == AUDIO_SHM_MAGIC:
                    sr = struct.unpack_from("<I", mm, AUDIO_OFF_SAMPLE_RATE)[0]
                    ch = struct.unpack_from("<H", mm, AUDIO_OFF_CHANNELS)[0]
                    bd = struct.unpack_from("<H", mm, AUDIO_OFF_BIT_DEPTH)[0]
                    rs = struct.unpack_from("<I", mm, AUDIO_OFF_RING_SIZE)[0]
                    if sr and ch and rs:
                        break
                time.sleep(0.1)

            frame_bytes = ch * (bd // 8)
            print(f"Audio bridge: {sr} Hz, {ch} ch, {bd}-bit", flush=True)

            while True:
                wpos = struct.unpack_from("<I", mm, AUDIO_OFF_OUT_WPOS)[0]
                avail = (wpos - out_rpos) & 0xFFFFFFFF
                if avail > rs:
                    out_rpos = wpos - rs
                    avail = rs
                to_read = min(avail, 4096)
                to_read = (to_read // frame_bytes) * frame_bytes
                if to_read == 0:
                    time.sleep(0.005)
                    continue
                mask = rs - 1
                pos = out_rpos & mask
                first = min(rs - pos, to_read)
                if first >= to_read:
                    data = os.pread(fd, to_read, AUDIO_HDR_SIZE + pos)
                else:
                    data = (os.pread(fd, first, AUDIO_HDR_SIZE + pos) +
                            os.pread(fd, to_read - first, AUDIO_HDR_SIZE))
                out_rpos = (out_rpos + to_read) & 0xFFFFFFFF
                struct.pack_into("<I", mm, AUDIO_OFF_OUT_RPOS, out_rpos)
                audio_hdr = struct.pack("<IHH", sr, ch, bd)
                payload = struct.pack("<I", FRAME_AUDIO) + audio_hdr + data
                mailbox.post_audio(ws_frame(payload))
                time.sleep(0.005)
        except Exception:
            traceback.print_exc()
        finally:
            mm.close()

# ── Console log bridge ───────────────────────────────────────────────

def log_bridge(read_fd):
    """Read lines from a file descriptor and broadcast as FRAME_LOG."""
    with os.fdopen(read_fd, "r", errors="replace") as f:
        for line in f:
            text = line.rstrip("\n\r")
            if text:
                payload = struct.pack("<I", FRAME_LOG) + text.encode("utf-8", "replace")
                mailbox.post_log(ws_frame(payload))

# ── Main ─────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="QEMU → browser dashboard bridge for oveRTOS")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--log-fd", type=int, default=-1,
                        help="File descriptor to read console log from")
    args = parser.parse_args()

    ove_dir = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    dashboard_dir = os.path.join(ove_dir, "sim", "dashboard")
    if not os.path.isdir(dashboard_dir):
        print(f"ERROR: Dashboard not found at {dashboard_dir}",
              file=sys.stderr)
        sys.exit(1)

    threading.Thread(target=display_bridge, daemon=True).start()
    threading.Thread(target=audio_bridge, daemon=True).start()
    if args.log_fd >= 0:
        threading.Thread(target=log_bridge, args=(args.log_fd,),
                         daemon=True).start()

    try:
        serve(args.port, dashboard_dir)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
