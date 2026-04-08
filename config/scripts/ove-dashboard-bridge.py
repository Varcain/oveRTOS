#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""
oveRTOS dashboard bridge — shared-memory to WebSocket.

Reads the framebuffer from /dev/shm/ove-fb and audio ring from
/dev/shm/ove-audio (written by firmware via mmap or semihosting),
and streams them to the browser dashboard over WebSocket.

Used by both host POSIX simulation and QEMU emulation.

Architecture:
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

# Common ring buffer layout (mirrors ove_sim_audio_ring.h):
#   Each ring: 32-byte header + 64KB data
#   SHM = [output_ring][input_ring]
AUDIO_RING_SIZE = 1 << 16  # 64 KB per direction
AUDIO_RING_HDR  = 32       # OVE_RING_OFF_BUF

# Per-ring field offsets (same as OVE_RING_OFF_*)
RING_OFF_WRITE_POS   = 0
RING_OFF_READ_POS    = 4
RING_OFF_SAMPLE_RATE = 8
RING_OFF_CHANNELS    = 12
RING_OFF_BIT_DEPTH   = 14
RING_OFF_SIZE        = 16
RING_OFF_UNDERRUNS   = 20
RING_OFF_OVERRUNS    = 24
RING_OFF_BUF         = 32

AUDIO_RING_TOTAL = AUDIO_RING_HDR + AUDIO_RING_SIZE
AUDIO_OUT_RING_OFF = 0
AUDIO_IN_RING_OFF  = AUDIO_RING_TOTAL
AUDIO_SHM_TOTAL    = 2 * AUDIO_RING_TOTAL

FRAME_FB    = 0x01
FRAME_AUDIO = 0x02
FRAME_EVENT = 0x03
FRAME_CMD   = 0x04
FRAME_STATE = 0x05
FRAME_LOG   = 0x06
FRAME_INPUT = 0x07

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
        self._audio = []   # queue — every chunk matters
        self._logs = []

    def post_fb(self, frame):
        with self._lock:
            self._fb = frame

    def post_audio(self, frame):
        with self._lock:
            self._audio.append(frame)

    def post_log(self, frame):
        with self._lock:
            self._logs.append(frame)

    def drain(self):
        with self._lock:
            fb, audio, logs = self._fb, self._audio, self._logs
            self._fb = None
            self._audio = []
            self._logs = []
        return fb, audio, logs

mailbox = Mailbox()
last_fb_frame = [None]  # cached for late-joining clients

# ── Audio input writer (dashboard mic → QEMU guest) ─────────────────

audio_in_mm = [None]   # mmap handle, set by audio_bridge
audio_in_fd = [None]   # file descriptor
audio_in_wpos = [0]    # host-side write position

def audio_input_write(pcm_bytes):
    """Write PCM data into the input ring of /dev/shm/ove-audio.
    Drops data if the ring is more than 500ms full (QEMU can't keep up)."""
    mm = audio_in_mm[0]
    fd = audio_in_fd[0]
    if mm is None or fd is None:
        return

    # Check how much the guest hasn't consumed yet.
    guest_rpos = struct.unpack_from(
        "<I", mm, AUDIO_IN_RING_OFF + RING_OFF_READ_POS)[0]
    wpos = audio_in_wpos[0]
    buffered = (wpos - guest_rpos) & 0xFFFFFFFF
    # Cap input latency at 500ms (sr * ch * bytes_per_sample * 0.5).
    # Use conservative 16000 * 1 * 2 * 0.5 = 16000 bytes.
    if buffered > 16000:
        audio_in_wpos[0] = (guest_rpos + 8000) & 0xFFFFFFFF
        wpos = audio_in_wpos[0]

    data = pcm_bytes
    buf_off = AUDIO_IN_RING_OFF + RING_OFF_BUF
    mask = AUDIO_RING_SIZE - 1
    pos = wpos & mask
    first = AUDIO_RING_SIZE - pos
    if first >= len(data):
        os.pwrite(fd, data, buf_off + pos)
    else:
        os.pwrite(fd, data[:first], buf_off + pos)
        os.pwrite(fd, data[first:], buf_off)
    audio_in_wpos[0] = (wpos + len(data)) & 0xFFFFFFFF
    struct.pack_into("<I", mm,
                     AUDIO_IN_RING_OFF + RING_OFF_WRITE_POS,
                     audio_in_wpos[0])

# ── WebSocket frame parser ───────────────────────────────────────────

def ws_parse_frame(buf):
    """Parse one WebSocket frame from buf. Returns (payload, consumed) or (None, 0)."""
    if len(buf) < 2:
        return None, 0
    b0, b1 = buf[0], buf[1]
    masked = (b1 & 0x80) != 0
    plen = b1 & 0x7F
    hlen = 2
    if plen == 126:
        if len(buf) < 4:
            return None, 0
        plen = struct.unpack_from(">H", buf, 2)[0]
        hlen = 4
    elif plen == 127:
        if len(buf) < 10:
            return None, 0
        plen = struct.unpack_from(">Q", buf, 2)[0]
        hlen = 10
    mlen = 4 if masked else 0
    total = hlen + mlen + plen
    if len(buf) < total:
        return None, 0
    mask_key = buf[hlen:hlen + mlen] if masked else None
    payload = bytearray(buf[hlen + mlen:hlen + mlen + plen])
    if masked:
        for i in range(len(payload)):
            payload[i] ^= mask_key[i & 3]
    opcode = b0 & 0x0F
    return (opcode, bytes(payload)), total

# ── Single-threaded server ────────────────────────────────────────────

MIME = {
    ".html": "text/html", ".js": "application/javascript",
    ".css": "text/css", ".png": "image/png",
    ".svg": "image/svg+xml", ".json": "application/json",
}

ST_HTTP = 0
ST_WS   = 1

class Client:
    __slots__ = ("sock", "addr", "state", "buf", "send_lock")
    def __init__(self, sock, addr):
        self.sock = sock
        self.addr = addr
        self.state = ST_HTTP
        self.buf = b""
        self.send_lock = threading.Lock()

# Shared list of WS clients for audio_bridge to send directly.
ws_client_list = []
ws_client_lock = threading.Lock()

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
                data = c.sock.recv(16384)
            except (BlockingIOError, ConnectionError):
                data = None
            if not data:
                dead.append(c)
                continue
            if c.state == ST_HTTP:
                c.buf += data
                if b"\r\n\r\n" in c.buf:
                    _handle_http(c, dashboard_dir)
            elif c.state == ST_WS:
                c.buf += data
                while True:
                    result, consumed = ws_parse_frame(c.buf)
                    if result is None:
                        break
                    opcode, payload = result
                    c.buf = c.buf[consumed:]
                    if opcode == 0x08:  # close
                        dead.append(c)
                        break
                    if opcode == 0x02 and len(payload) >= 4:  # binary
                        ftype = struct.unpack_from("<I", payload, 0)[0]
                        fdata = payload[4:]
                        if ftype == FRAME_CMD and len(fdata) >= 12:
                            # Plugin command: [plugin_id:4][cmd_type:4][data_len:4][data]
                            plugin_id = struct.unpack_from("<I", fdata, 0)[0]
                            cmd_type = struct.unpack_from("<I", fdata, 4)[0]
                            pcm = fdata[12:]
                            # Route audio inject (cmd_type 0) for any
                            # real plugin ID (skip console sentinel).
                            if (plugin_id != 0xFFFFFFFF
                                    and cmd_type == 0 and pcm):
                                audio_input_write(pcm)
                        elif ftype == FRAME_AUDIO and len(fdata) >= 8:
                            # Raw audio frame from dashboard mic
                            pcm = fdata[8:]
                            if pcm:
                                audio_input_write(pcm)

        for c in dead:
            clients.remove(c)
            with ws_client_lock:
                if c in ws_client_list:
                    ws_client_list.remove(c)
            c.sock.close()

        # Drain mailbox and broadcast to WS clients.
        # Audio is sent directly by audio_bridge thread — only fb/logs here.
        fb, audio, logs = mailbox.drain()
        ws_clients = [c for c in clients if c.state == ST_WS]
        frames = logs + ([fb] if fb else [])
        for frame in frames:
            dead2 = []
            for c in ws_clients:
                try:
                    _send_blocking(c, frame)
                except (ConnectionError, OSError, TimeoutError):
                    dead2.append(c)
            for c in dead2:
                if c in clients:
                    clients.remove(c)
                try:
                    c.sock.close()
                except Exception:
                    pass


def _send_blocking(client, data):
    """Send with per-client lock to prevent frame interleaving with audio thread."""
    with client.send_lock:
        client.sock.setblocking(True)
        client.sock.settimeout(5)
        try:
            client.sock.sendall(data)
        finally:
            client.sock.setblocking(False)


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
        # Send plugin state so dashboard discovers audio plugin ID.
        # QEMU board: display=0, audio=1 (when both present).
        audio_id = 1 if os.path.exists(FB_PATH) else 0
        state_json = (
            f'{{"type":"audio","sample_rate":16000,'
            f'"channels":1,"bit_depth":16}}'
        ).encode()
        state_payload = (
            struct.pack("<II", FRAME_STATE, audio_id) + state_json
        )
        c.sock.sendall(ws_frame(state_payload))

        c.sock.setblocking(False)
        c.state = ST_WS
        with ws_client_lock:
            ws_client_list.append(c)
        print("WebSocket client connected", flush=True)
        return

    # Static file
    if path == "/":
        path = "/index.html"
    filepath = os.path.realpath(os.path.join(dashboard_dir, path.lstrip("/")))
    if not filepath.startswith(os.path.realpath(dashboard_dir)):
        _send_blocking(c, b"HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n")
        return

    if not os.path.isfile(filepath):
        _send_blocking(c, b"HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n")
        return

    ext = os.path.splitext(filepath)[1]
    ct = MIME.get(ext, "application/octet-stream")
    body = open(filepath, "rb").read()
    hdr = (f"HTTP/1.1 200 OK\r\n"
           f"Content-Type: {ct}\r\n"
           f"Content-Length: {len(body)}\r\n"
           f"Cross-Origin-Opener-Policy: same-origin\r\n"
           f"Cross-Origin-Embedder-Policy: require-corp\r\n"
           f"Cache-Control: no-cache\r\n"
           f"Connection: close\r\n"
           f"\r\n").encode()
    _send_blocking(c, hdr + body)

# ── Shmem bridges ────────────────────────────────────────────────────

def display_bridge():
    while not os.path.exists(FB_PATH):
        time.sleep(0.1)
    # Wait for the file to be pre-allocated (non-empty).
    while os.path.getsize(FB_PATH) < FB_HEADER_SIZE:
        time.sleep(0.1)
    with open(FB_PATH, "r+b") as f:
        mm = mmap.mmap(f.fileno(), 0)
        try:
            while True:
                magic, w, h, fmt, dirty = struct.unpack(
                    "<IHHII", mm[:FB_HEADER_SIZE])
                if magic == FB_MAGIC and w > 0 and h > 0 and dirty:
                    npixels = w * h
                    struct.pack_into("<I", mm, 12, 0)  # clear dirty
                    if fmt == 1:  # XRGB8888 — already in dashboard format
                        xrgb = bytes(mm[FB_HEADER_SIZE:FB_HEADER_SIZE+npixels*4])
                    else:  # RGB565 — convert via LUT
                        pdata = bytes(mm[FB_HEADER_SIZE:FB_HEADER_SIZE+npixels*2])
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
    """Read audio ring and forward to dashboard. Auto-restarts on error."""
    while True:
        try:
            _audio_bridge_loop()
        except Exception:
            print("[audio-bridge] crashed, restarting in 1s:", flush=True)
            traceback.print_exc()
            time.sleep(1)

def _audio_bridge_loop():
    while not os.path.exists(AUDIO_PATH):
        time.sleep(0.2)
    # Ensure the file is large enough for both rings.
    with open(AUDIO_PATH, "r+b") as f:
        f.seek(0, 2)  # seek to end
        if f.tell() < AUDIO_SHM_TOTAL:
            f.truncate(AUDIO_SHM_TOTAL)
    with open(AUDIO_PATH, "r+b") as f:
        fd = f.fileno()
        mm = mmap.mmap(fd, 0)
        audio_in_mm[0] = mm
        audio_in_fd[0] = fd
        out_rpos = 0
        try:
            # Wait for guest to write the output ring header.
            while True:
                sr = struct.unpack_from(
                    "<I", mm,
                    AUDIO_OUT_RING_OFF + RING_OFF_SAMPLE_RATE)[0]
                ch = struct.unpack_from(
                    "<H", mm,
                    AUDIO_OUT_RING_OFF + RING_OFF_CHANNELS)[0]
                bd = struct.unpack_from(
                    "<H", mm,
                    AUDIO_OUT_RING_OFF + RING_OFF_BIT_DEPTH)[0]
                rs = struct.unpack_from(
                    "<I", mm,
                    AUDIO_OUT_RING_OFF + RING_OFF_SIZE)[0]
                if sr and ch and rs:
                    break
                time.sleep(0.1)

            frame_bytes = ch * (bd // 8)
            buf_off = AUDIO_OUT_RING_OFF + RING_OFF_BUF
            print(f"Audio bridge: {sr} Hz, {ch} ch, {bd}-bit", flush=True)

            while True:
                wpos = struct.unpack_from(
                    "<I", mm,
                    AUDIO_OUT_RING_OFF + RING_OFF_WRITE_POS)[0]
                avail = (wpos - out_rpos) & 0xFFFFFFFF
                if avail > rs:
                    out_rpos = wpos - (rs >> 1)
                    avail = rs >> 1
                to_read = min(avail, 4096)
                to_read = (to_read // frame_bytes) * frame_bytes
                if to_read == 0:
                    time.sleep(0.005)
                    continue
                mask = rs - 1
                pos = out_rpos & mask
                first = min(rs - pos, to_read)
                if first >= to_read:
                    data = os.pread(fd, to_read, buf_off + pos)
                else:
                    data = (os.pread(fd, first, buf_off + pos) +
                            os.pread(fd, to_read - first, buf_off))
                out_rpos = (out_rpos + to_read) & 0xFFFFFFFF
                struct.pack_into(
                    "<I", mm,
                    AUDIO_OUT_RING_OFF + RING_OFF_READ_POS, out_rpos)
                audio_hdr = struct.pack("<IHH", sr, ch, bd)
                payload = struct.pack("<I", FRAME_AUDIO) + audio_hdr + data
                frame = ws_frame(payload)

                # Send audio directly to WS clients from this thread.
                # Non-blocking lock: skip if FB send is in progress
                # (drops ~1 audio frame per FB send, inaudible).
                with ws_client_lock:
                    targets = list(ws_client_list)
                for c in targets:
                    acquired = c.send_lock.acquire(blocking=False)
                    if not acquired:
                        continue  # FB send in progress, skip
                    try:
                        c.sock.setblocking(True)
                        c.sock.settimeout(0.05)
                        c.sock.sendall(frame)
                        c.sock.setblocking(False)
                    except Exception:
                        pass
                    finally:
                        c.send_lock.release()

                time.sleep(0.005)
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
        description="oveRTOS dashboard bridge (shmem → WebSocket)")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--dashboard", type=str, default=None,
                        help="Path to dashboard static files")
    parser.add_argument("--log-fd", type=int, default=-1,
                        help="File descriptor to read console log from")
    args = parser.parse_args()

    if args.dashboard:
        dashboard_dir = args.dashboard
    else:
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
