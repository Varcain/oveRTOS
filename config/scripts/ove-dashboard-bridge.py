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

# Pointer input — 16-byte struct the firmware creates in
# ove_sim_transport_shm_local.c::local_open(). We mmap it lazily on
# the first FRAME_INPUT event and write click coordinates into it.
INPUT_PATH = "/dev/shm/ove-input"
INPUT_MAGIC = 0x54504E49  # "INPT"
INPUT_SHM_SIZE = 16

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

FRAME_FB         = 0x01
FRAME_AUDIO      = 0x02
FRAME_EVENT      = 0x03
FRAME_CMD        = 0x04
FRAME_STATE      = 0x05
FRAME_LOG        = 0x06
FRAME_INPUT      = 0x07
FRAME_DEBUG_CMD  = 0x08
FRAME_DEBUG_RESP = 0x09
FRAME_THREAD     = 0x0A
FRAME_FILE_LIST  = 0x0B
FRAME_FILE_REQ   = 0x0C
FRAME_FILE_RESP  = 0x0D

# Lazy-opened mmap of /dev/shm/ove-input. None until the firmware has
# created the region; every FRAME_INPUT retries the attach.
input_shm = [None]  # single-slot container for mutable reference


def input_shm_attach():
    """Try to mmap /dev/shm/ove-input if the firmware has created it.

    Returns the mmap object or None. Cached in `input_shm[0]`.
    """
    if input_shm[0] is not None:
        return input_shm[0]
    try:
        if not os.path.exists(INPUT_PATH):
            return None
        size = os.path.getsize(INPUT_PATH)
        if size < INPUT_SHM_SIZE:
            return None
        fd = os.open(INPUT_PATH, os.O_RDWR)
        try:
            mm = mmap.mmap(fd, INPUT_SHM_SIZE)
        finally:
            os.close(fd)
        magic = struct.unpack_from("<I", mm, 0)[0]
        if magic != INPUT_MAGIC:
            mm.close()
            return None
        input_shm[0] = mm
        return mm
    except OSError:
        return None


def input_shm_write(x, y, pressed):
    """Write a single pointer sample into /dev/shm/ove-input."""
    mm = input_shm_attach()
    if mm is None:
        return
    try:
        struct.pack_into("<hhB", mm, 4, int(x), int(y), 1 if pressed else 0)
    except (OSError, ValueError):
        pass

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
ST_DONE = 2  # HTTP response sent, close on next loop

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
        # Build poll list: server + active client sockets
        rlist = [srv] + [c.sock for c in clients if c.state != ST_DONE]
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
            if c.state == ST_DONE or c.sock not in readable:
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
                        elif ftype == FRAME_INPUT and len(fdata) >= 5:
                            # Pointer input: int16 x, int16 y, uint8 pressed
                            x, y, pressed = struct.unpack_from(
                                "<hhB", fdata, 0)
                            input_shm_write(x, y, pressed)
                        elif ftype == FRAME_DEBUG_CMD and len(fdata) >= 1:
                            gdb = gdb_controller[0]
                            if gdb:
                                gdb.handle_debug_cmd(fdata)
                        elif ftype == FRAME_FILE_REQ and fdata:
                            handle_file_request(fdata)

        # Remove dead WS clients
        for c in dead:
            clients.remove(c)
            with ws_client_lock:
                if c in ws_client_list:
                    ws_client_list.remove(c)
            try:
                c.sock.close()
            except Exception:
                pass

        # Close completed HTTP clients (static file served)
        done = [c for c in clients if c.state == ST_DONE]
        for c in done:
            clients.remove(c)
            try:
                c.sock.close()
            except Exception:
                pass

        # Drain mailbox and broadcast to WS clients.
        # Audio is sent directly by audio_bridge thread — only fb/logs here.
        fb, audio, logs = mailbox.drain()
        ws_clients = [c for c in clients if c.state == ST_WS]
        frames = logs + ([fb] if fb else [])
        for frame in frames:
            dead2 = []
            for c in ws_clients:
                try:
                    _send_nonblock(c, frame)
                except (ConnectionError, OSError, TimeoutError):
                    dead2.append(c)
            for c in dead2:
                if c in clients:
                    clients.remove(c)
                with ws_client_lock:
                    if c in ws_client_list:
                        ws_client_list.remove(c)
                try:
                    c.sock.close()
                except Exception:
                    pass


def _send_http(client, data):
    """Send an HTTP response in a thread. Marks client for cleanup."""
    def _do_send():
        try:
            client.sock.setblocking(True)
            client.sock.settimeout(2)
            client.sock.sendall(data)
        except Exception:
            pass
        client.state = ST_DONE
    threading.Thread(target=_do_send, daemon=True).start()


def _send_nonblock(client, data):
    """Send with per-client lock, short timeout to avoid stalling the server."""
    acquired = client.send_lock.acquire(timeout=0.05)
    if not acquired:
        return  # audio thread is sending — skip this frame
    try:
        client.sock.setblocking(True)
        client.sock.settimeout(0.5)
        client.sock.sendall(data)
    finally:
        client.sock.setblocking(False)
        client.send_lock.release()


def _handle_http(c, dashboard_dir):
    request = c.buf.decode(errors="replace")
    c.buf = b""

    lines = request.split("\r\n")
    parts = lines[0].split(" ")
    path = parts[1].split("?")[0] if len(parts) > 1 else "/"

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
        # Temporarily blocking for handshake + initial state.
        c.sock.setblocking(True)
        c.sock.settimeout(2)
        try:
            c.sock.sendall(resp)
            if last_fb_frame[0]:
                c.sock.sendall(last_fb_frame[0])

            # Plugin state so dashboard discovers audio plugin ID.
            audio_id = 1 if os.path.exists(FB_PATH) else 0
            state_json = (
                f'{{"type":"audio","sample_rate":16000,'
                f'"channels":1,"bit_depth":16}}'
            ).encode()
            c.sock.sendall(ws_frame(
                struct.pack("<II", FRAME_STATE, audio_id) + state_json))

            # GDB debug state.
            gdb = gdb_controller[0]
            if gdb and gdb.proc:
                state = {"state": "running", "reason": None,
                         "file": None, "line": None}
                dbg_json = json.dumps(
                    {"status": "done", "data": state},
                    default=str).encode("utf-8")
                c.sock.sendall(ws_frame(
                    struct.pack("<IB", FRAME_DEBUG_RESP, 0x00) + dbg_json))

            # Project file list for source explorer.
            if _project_file_list_frame:
                c.sock.sendall(_project_file_list_frame)

        except Exception:
            try:
                c.sock.close()
            except Exception:
                pass
            return

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
        _send_http(c, b"HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n")
        return

    if not os.path.isfile(filepath):
        _send_http(c, b"HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n")
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
    _send_http(c, hdr + body)

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

# ── Sim event ring bridge ────────────────────────────────────────────

SIM_SHM_PATH       = "/dev/shm/ove-sim"
SIM_SHM_MAGIC      = 0x4F565349  # "OVSI"
SIM_SHM_HDR_SIZE   = 64
SIM_SHM_RING_SIZE  = 1 << 16  # 64 KB
SIM_SHM_EVENT_OFF  = SIM_SHM_HDR_SIZE
SIM_SHM_TOTAL_SIZE = SIM_SHM_HDR_SIZE + 2 * SIM_SHM_RING_SIZE

# ove_sim_event header: plugin_id(4) + event_type(4) + timestamp_ms(4) + data_len(4)
SIM_EVENT_HDR_SIZE = 16
# Debug plugin event type
SIM_DEBUG_EVT_THREADS = 0


def event_bridge():
    """Read plugin events from /dev/shm/ove-sim event ring.

    Forwards debug plugin thread snapshots as FRAME_THREAD frames.
    Uses os.pread() for header reads to guarantee coherence with
    QEMU semihosting writes (avoids mmap staleness).
    """
    while True:
        try:
            _event_bridge_loop()
        except Exception:
            sys.stderr.write("[event-bridge] crashed, restarting in 1s\n")
            traceback.print_exc(file=sys.stderr)
            time.sleep(1)


def _event_bridge_loop():
    sys.stderr.write(f"[event-bridge] waiting for {SIM_SHM_PATH}\n")
    sys.stderr.flush()
    while not os.path.exists(SIM_SHM_PATH):
        time.sleep(0.2)
    # Don't check file size — QEMU semihosting truncates on open and
    # the file grows incrementally as the firmware writes events.
    # Just wait for the magic header to appear.

    fd = os.open(SIM_SHM_PATH, os.O_RDWR)
    try:
        while True:
            try:
                hdr = os.pread(fd, 64, 0)
            except OSError:
                time.sleep(0.2)
                continue
            if len(hdr) >= 20:
                magic = struct.unpack_from("<I", hdr, 0)[0]
                if magic == SIM_SHM_MAGIC:
                    break
            time.sleep(0.1)

        # The semihosting file is small — the firmware writes the
        # header + events sequentially, so the file size tracks the
        # write position.  The ring offsets are relative to the
        # start of the file, not a full 64KB mmap.  Read all data
        # via pread using absolute file offsets.

        rpos = 0
        ring_mask = SIM_SHM_RING_SIZE - 1
        sys.stderr.write("[event-bridge] SIM SHM ready\n")
        sys.stderr.flush()

        while True:
            # Read event_write_pos via pread (offset 16 in header)
            wbuf = os.pread(fd, 4, 16)
            if len(wbuf) < 4:
                time.sleep(0.1)
                continue
            wpos = struct.unpack_from("<I", wbuf, 0)[0]
            avail = (wpos - rpos) & 0xFFFFFFFF
            if avail < 2:
                time.sleep(0.05)
                continue
            if avail > SIM_SHM_RING_SIZE:
                rpos = wpos
                os.pwrite(fd, struct.pack("<I", rpos), 20)
                continue

            # Read message length (uint16 LE) from ring
            off0 = SIM_SHM_EVENT_OFF + (rpos & ring_mask)
            off1 = SIM_SHM_EVENT_OFF + ((rpos + 1) & ring_mask)
            b0 = os.pread(fd, 1, off0)
            b1 = os.pread(fd, 1, off1)
            if len(b0) < 1 or len(b1) < 1:
                time.sleep(0.05)
                continue
            msg_len = b0[0] | (b1[0] << 8)
            rpos += 2

            if msg_len < SIM_EVENT_HDR_SIZE or msg_len > 4096:
                rpos += msg_len
                os.pwrite(fd, struct.pack("<I", rpos), 20)
                continue

            # Read event from ring (handle wrap-around)
            ring_off = rpos & ring_mask
            first = min(SIM_SHM_RING_SIZE - ring_off, msg_len)
            buf = os.pread(fd, first, SIM_SHM_EVENT_OFF + ring_off)
            if first < msg_len:
                buf += os.pread(fd, msg_len - first, SIM_SHM_EVENT_OFF)
            rpos += msg_len

            # Update read position
            os.pwrite(fd, struct.pack("<I", rpos), 20)

            # Parse ove_sim_event header
            if len(buf) < SIM_EVENT_HDR_SIZE:
                continue
            plugin_id, event_type, _, data_len = struct.unpack_from(
                "<IIII", buf, 0)
            payload = bytes(buf[SIM_EVENT_HDR_SIZE:
                                SIM_EVENT_HDR_SIZE + data_len])

            # Forward debug thread snapshots as FRAME_THREAD
            if event_type == SIM_DEBUG_EVT_THREADS and payload:
                frame = ws_frame(
                    struct.pack("<I", FRAME_THREAD) + payload)
                mailbox.post_log(frame)
    finally:
        os.close(fd)

# ── Console log bridge ───────────────────────────────────────────────

def log_bridge(read_fd):
    """Read lines from a file descriptor and broadcast as FRAME_LOG."""
    with os.fdopen(read_fd, "r", errors="replace") as f:
        for line in f:
            text = line.rstrip("\n\r")
            if text:
                payload = struct.pack("<I", FRAME_LOG) + text.encode("utf-8", "replace")
                mailbox.post_log(ws_frame(payload))

# ── GDB/MI Controller ────────────────────────────────────────────────

import json
import re
import subprocess

# Debug command types (dashboard → bridge)
DBG_CMD_CONTINUE    = 0x01
DBG_CMD_PAUSE       = 0x02
# ── Source file list (built from CMake build objects) ────────────────

_project_files = []  # list of {"path": abs_path, "short": display, "cat": category}
_project_file_list_frame = None  # cached WS frame for FRAME_FILE_LIST


def build_project_file_list(build_dir, ove_dir):
    """Scan CMake build dir for compiled .c.obj files and categorize."""
    global _project_files, _project_file_list_frame
    if not build_dir or not os.path.isdir(build_dir):
        return

    # Scan all .c.obj files in the build tree.  Works with both
    # FreeRTOS (single CMakeFiles/firmware.elf.dir/) and Zephyr
    # (objects scattered across app/, zephyr/kernel/, etc.).
    # Object paths encode the source path — extract it and filter
    # to files under ove_dir (skip RTOS/SDK internals).

    files = []
    seen = set()
    for root, _, names in os.walk(build_dir):
        for name in names:
            # CMake uses .c.obj (cross) or .c.o (native)
            if name.endswith(".c.obj"):
                ext = ".c.obj"
            elif name.endswith(".c.o"):
                ext = ".c.o"
            else:
                continue
            obj_path = os.path.join(root, name)
            # Extract source path from the obj dir structure.
            # CMake encodes the full source path in the obj directory.
            rel = os.path.relpath(obj_path, build_dir)
            src = rel[:-(len(ext) - 2)]  # strip extension, keep .c
            # Strip CMake dir prefixes
            # FreeRTOS: CMakeFiles/firmware.elf.dir/home/user/.../file.c
            # Zephyr:   app/CMakeFiles/app.dir/home/user/.../file.c
            idx = src.find("/home/")
            if idx >= 0:
                abs_path = src[idx:]  # "/home/user/.../file.c"
            else:
                # Relative source (main.c, system_*.c) — resolve
                # against workspace dir
                ws_dir = build_dir
                if "/build/" in ws_dir:
                    ws_dir = ws_dir.split("/build/")[0]
                abs_path = os.path.join(ws_dir, src.split("/")[-1])

            if not os.path.isfile(abs_path):
                continue
            abs_path = os.path.realpath(abs_path)
            if abs_path in seen:
                continue
            seen.add(abs_path)

            # Only include files under ove_dir (skip Zephyr kernel,
            # SDK, and driver internals).
            if ove_dir and not abs_path.startswith(ove_dir):
                continue

            # Categorize
            short = os.path.relpath(abs_path, ove_dir) if ove_dir else abs_path
            if "/apps/" in abs_path:
                cat = "App"
            elif "/backends/" in abs_path:
                cat = "Backend"
            elif "/src/ove_" in abs_path:
                cat = "Core"
            elif "/sim/" in abs_path:
                cat = "Sim"
            elif "/boards/" in abs_path or short.endswith("main.c") \
                    or "system_" in short:
                cat = "Board"
            elif "FreeRTOS" in abs_path or "freertos" in abs_path.lower():
                cat = "RTOS"
            elif "/dl/lvgl" in abs_path or "/lvgl/" in abs_path:
                cat = "LVGL"
            elif "/tests/" in abs_path or "/stub" in abs_path:
                cat = "Stubs"
            else:
                cat = "Other"

            files.append({"path": abs_path, "short": short, "cat": cat})

    # Sort: App first, then Core, Backend, Board, Sim, RTOS, LVGL, Stubs
    cat_order = {"App": 0, "Core": 1, "Backend": 2, "Board": 3,
                 "Sim": 4, "RTOS": 5, "LVGL": 6, "Stubs": 7, "Other": 8}
    files.sort(key=lambda f: (cat_order.get(f["cat"], 9), f["short"]))

    _project_files = files

    # Build cached FRAME_FILE_LIST payload
    file_list_json = json.dumps(
        [{"path": f["path"], "short": f["short"], "cat": f["cat"]}
         for f in files]).encode("utf-8")
    _project_file_list_frame = ws_frame(
        struct.pack("<I", FRAME_FILE_LIST) + file_list_json)

    sys.stderr.write(f"[files] {len(files)} source files indexed\n")
    sys.stderr.flush()


def handle_file_request(fdata):
    """Handle FRAME_FILE_REQ: read and send a full source file."""
    # Payload: UTF-8 file path
    path = fdata.decode("utf-8", "replace").strip()
    if not path:
        return
    # Security: only serve files in the project file list
    allowed = any(f["path"] == path for f in _project_files)
    if not allowed:
        return
    try:
        with open(path, "r", errors="replace") as f:
            content = f.read()
    except OSError:
        content = "// File not found: " + path
    payload = struct.pack("<I", FRAME_FILE_RESP)
    # [uint32 frame_type][uint16 path_len][path][content]
    path_bytes = path.encode("utf-8")
    payload += struct.pack("<H", len(path_bytes))
    payload += path_bytes
    payload += content.encode("utf-8")
    frame = ws_frame(payload)
    mailbox.post_log(frame)


DBG_CMD_STEP_OVER   = 0x03
DBG_CMD_STEP_INTO   = 0x04
DBG_CMD_STEP_OUT    = 0x05
DBG_CMD_RESET       = 0x06
DBG_CMD_BACKTRACE   = 0x07
DBG_CMD_REGISTERS   = 0x08
DBG_CMD_DISASSEMBLE = 0x09
DBG_CMD_BP_SET      = 0x0A
DBG_CMD_BP_CLEAR    = 0x0B
DBG_CMD_SOURCE      = 0x0C

# Debug response types (bridge → dashboard)
DBG_RESP_STATE      = 0x00
DBG_RESP_BACKTRACE  = 0x07
DBG_RESP_REGISTERS  = 0x08
DBG_RESP_DISASSEMBLY = 0x09
DBG_RESP_BREAKPOINT = 0x0A
DBG_RESP_SOURCE     = 0x0C


class GdbController:
    """Drives a GDB/MI subprocess to debug the target."""

    def __init__(self, toolchain, elf_path, gdb_port=None, attach_pid=None):
        self.toolchain = toolchain
        self.elf_path = elf_path
        self.gdb_port = gdb_port
        self.attach_pid = attach_pid
        self.proc = None
        self._token = 0
        self._lock = threading.Lock()
        self._reader_thread = None
        self._pending = {}  # token → threading.Event + result
        self._source_cache = {}  # file path → list of lines
        self._last_interrupt = 0  # monotonic time of last interrupt
        self._reg_names = None  # cached register names from GDB

    def start(self):
        """Launch GDB subprocess and connect to target."""
        cmd = [self.toolchain, "--interpreter=mi3", "-q"]
        if self.elf_path:
            cmd += [self.elf_path]
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)

        self._reader_thread = threading.Thread(
            target=self._reader_loop, daemon=True)
        self._reader_thread.start()

        # Wait for GDB startup
        time.sleep(0.3)

        if self.gdb_port:
            self._send_mi_sync(
                f"-target-select remote localhost:{self.gdb_port}")
        elif self.attach_pid:
            self._send_mi_sync(f"-target-attach {self.attach_pid}")

        # Fetch register names once (architecture-specific).
        result = self._send_mi_sync("-data-list-register-names")
        if result and isinstance(result.get("data"), dict):
            names = result["data"].get("register-names", [])
            if isinstance(names, list):
                self._reg_names = names

        # Connecting halts the target — resume it immediately so the
        # firmware keeps running.  The user can pause from the dashboard.
        time.sleep(0.1)
        self._send_mi("-exec-continue")

        self._broadcast_state("running", None, None, None)

    def stop(self):
        if self.proc:
            try:
                self.proc.stdin.write("-gdb-exit\n")
                self.proc.stdin.flush()
            except Exception:
                pass
            self.proc.terminate()
            self.proc = None

    def handle_debug_cmd(self, data):
        """Handle a FRAME_DEBUG_CMD from the dashboard."""
        if len(data) < 1 or not self.proc:
            return
        cmd_type = data[0]
        payload = data[1:]

        if cmd_type == DBG_CMD_CONTINUE:
            self._send_mi("-exec-continue --all")
            self._broadcast_state("running", None, None, None)
        elif cmd_type == DBG_CMD_PAUSE:
            # Debounce: ignore rapid clicks (< 1s apart) to prevent
            # multiple breaks from killing the remote target.
            import signal as _signal
            now = time.monotonic()
            if now - self._last_interrupt < 1.0:
                return
            self._last_interrupt = now
            # SIGINT to GDB process — GDB catches it and sends a break
            # character to the remote gdbstub.  -exec-interrupt alone
            # doesn't reliably work with QEMU's gdbstub.
            try:
                os.kill(self.proc.pid, _signal.SIGINT)
            except (OSError, ProcessLookupError):
                pass
        elif cmd_type == DBG_CMD_STEP_OVER:
            self._send_mi("-exec-next")
        elif cmd_type == DBG_CMD_STEP_INTO:
            self._send_mi("-exec-step")
        elif cmd_type == DBG_CMD_STEP_OUT:
            self._send_mi("-exec-finish")
        elif cmd_type == DBG_CMD_RESET:
            self._send_mi("monitor system_reset")
            time.sleep(0.1)
            self._send_mi("-exec-continue")
            self._broadcast_state("running", None, None, None)
        elif cmd_type == DBG_CMD_BACKTRACE:
            result = self._send_mi_sync("-stack-list-frames")
            if result:
                self._send_debug_resp(DBG_RESP_BACKTRACE, result)
        elif cmd_type == DBG_CMD_REGISTERS:
            result = self._send_mi_sync(
                "-data-list-register-values x")
            if result:
                self._send_debug_resp(
                    DBG_RESP_REGISTERS, self._enrich_registers(result))
        elif cmd_type == DBG_CMD_DISASSEMBLE:
            if len(payload) >= 8:
                addr = struct.unpack_from("<I", payload, 0)[0]
                count = struct.unpack_from("<I", payload, 4)[0]
                end_addr = addr + count * 4
                result = self._send_mi_sync(
                    f"-data-disassemble -s 0x{addr:x} -e 0x{end_addr:x} -- 1")
                if result:
                    self._send_debug_resp(DBG_RESP_DISASSEMBLY, result)
        elif cmd_type == DBG_CMD_BP_SET:
            if len(payload) >= 6:
                path_len = struct.unpack_from("<H", payload, 0)[0]
                path = payload[2:2 + path_len].decode("utf-8", "replace")
                line = struct.unpack_from("<I", payload, 2 + path_len)[0]
                result = self._send_mi_sync(
                    f"-break-insert {path}:{line}")
                if result:
                    self._send_debug_resp(DBG_RESP_BREAKPOINT, result)
        elif cmd_type == DBG_CMD_BP_CLEAR:
            if len(payload) >= 4:
                bp_id = struct.unpack_from("<I", payload, 0)[0]
                self._send_mi(f"-break-delete {bp_id}")
        elif cmd_type == DBG_CMD_SOURCE:
            if len(payload) >= 10:
                path_len = struct.unpack_from("<H", payload, 0)[0]
                path = payload[2:2 + path_len].decode("utf-8", "replace")
                line = struct.unpack_from("<I", payload, 2 + path_len)[0]
                ctx_lines = struct.unpack_from(
                    "<I", payload, 6 + path_len)[0]
                self._handle_source_request(path, line, ctx_lines)

    def _send_mi(self, cmd):
        """Send a GDB/MI command (fire-and-forget)."""
        if not self.proc:
            return
        try:
            self.proc.stdin.write(cmd + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    def _send_mi_sync(self, cmd, timeout=2.0):
        """Send a GDB/MI command and wait for the result record."""
        if not self.proc:
            return None
        with self._lock:
            self._token += 1
            token = self._token
        evt = threading.Event()
        self._pending[token] = {"event": evt, "result": None}
        try:
            self.proc.stdin.write(f"{token}{cmd}\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            del self._pending[token]
            return None
        evt.wait(timeout=timeout)
        entry = self._pending.pop(token, None)
        return entry["result"] if entry else None

    def _reader_loop(self):
        """Read GDB/MI output and dispatch async events + result records."""
        while self.proc:
            try:
                line = self.proc.stdout.readline()
                if not line:
                    break  # EOF
                line = line.rstrip("\n\r")
                if not line or line == "(gdb)":
                    continue
                # Log only *stopped and *running,thread-id="all"
                # (skip per-thread *running from short-lived threads)
                if line.startswith("*stopped") or \
                        line == '*running,thread-id="all"':
                    short = line[:120] + "..." if len(line) > 120 else line
                    try:
                        sys.stderr.write(f"[gdb] {short}\n")
                        sys.stderr.flush()
                    except Exception:
                        pass
                self._handle_mi_output(line)
            except Exception:
                try:
                    sys.stderr.write("[gdb] reader error\n")
                    traceback.print_exc(file=sys.stderr)
                except Exception:
                    pass

    def _handle_mi_output(self, line):
        # Result record: token^status,results  (token may be absent)
        m = re.match(r"^(\d+)?\^(\w+)(,.+)?$", line)
        if m:
            token_str = m.group(1)
            status = m.group(2)
            data = m.group(3) or ""
            if data.startswith(","):
                data = data[1:]
            parsed = {"status": status, "data": _mi_parse_kv(data)}
            if token_str:
                token = int(token_str)
                entry = self._pending.get(token)
                if entry:
                    entry["result"] = parsed
                    entry["event"].set()
            # ^error from any command — log it
            if status == "error":
                msg = parsed["data"].get("msg", "")
                print(f"[gdb] ERROR: {msg}", flush=True)
            return

        # Async exec record: *stopped, *running
        if line.startswith("*stopped"):
            # Dispatch to a worker thread — _handle_stopped uses
            # _send_mi_sync which would deadlock in the reader loop.
            threading.Thread(
                target=self._handle_stopped, args=(line,),
                daemon=True).start()
        elif line == '*running,thread-id="all"':
            # Only broadcast on "all threads running" — ignore
            # per-thread *running from short-lived glibc threads.
            self._broadcast_state("running", None, None, None)

    def _handle_stopped(self, line):
        """Parse *stopped event and broadcast state to dashboard."""
        data = line[len("*stopped"):]
        if data.startswith(","):
            data = data[1:]
        info = _mi_parse_kv(data)

        reason = info.get("reason", "unknown")
        frame = info.get("frame", {})
        func = frame.get("func", "??")
        src_file = frame.get("fullname", frame.get("file", ""))
        src_line = int(frame.get("line", "0") or "0")
        addr = frame.get("addr", "0x0")

        self._broadcast_state(
            "stopped", reason, src_file, src_line, addr=addr)

        # Auto-fetch source context for the current location
        if src_file and src_line:
            self._handle_source_request(src_file, src_line, 20)
        else:
            # No source — send a placeholder so the UI shows something
            resp = {
                "file": "",
                "start_line": 0,
                "current_line": 0,
                "lines": [f"  No source available for {func} ({addr})"],
            }
            self._send_debug_resp(
                DBG_RESP_SOURCE,
                {"status": "done", "data": resp})

        # Auto-fetch backtrace
        result = self._send_mi_sync("-stack-list-frames", timeout=3.0)
        if result:
            self._send_debug_resp(DBG_RESP_BACKTRACE, result)

        # Auto-fetch registers
        result = self._send_mi_sync(
            "-data-list-register-values x", timeout=3.0)
        if result:
            self._send_debug_resp(
                DBG_RESP_REGISTERS, self._enrich_registers(result))

        # Auto-fetch disassembly around current PC
        if addr:
            try:
                pc = int(addr, 16)
                start = max(0, pc - 32)
                end = pc + 64
                result = self._send_mi_sync(
                    f"-data-disassemble -s 0x{start:x} -e 0x{end:x} -- 1",
                    timeout=3.0)
                if result:
                    self._send_debug_resp(DBG_RESP_DISASSEMBLY, result)
            except (ValueError, TypeError):
                pass

    def _enrich_registers(self, result):
        """Add register names to register-values result."""
        if not result or not self._reg_names:
            return result
        data = result.get("data", {})
        regs = data.get("register-values", [])
        if not isinstance(regs, list):
            return result
        for r in regs:
            num = int(r.get("number", -1))
            if 0 <= num < len(self._reg_names):
                name = self._reg_names[num]
                if name:  # skip empty names (unused registers)
                    r["name"] = name
        # Filter out registers with empty names (GDB returns many
        # unused slots like "" for internal pseudo-registers).
        data["register-values"] = [
            r for r in regs
            if r.get("name") or (
                int(r.get("number", -1)) < len(self._reg_names)
                and self._reg_names[int(r.get("number", -1))]
            )
        ]
        return result

    def _broadcast_state(self, exec_state, reason, src_file, src_line,
                         addr=None):
        """Send debug state change to all dashboard clients."""
        state = {
            "state": exec_state,
            "reason": reason,
            "file": src_file,
            "line": src_line,
            "addr": addr,
        }
        self._send_debug_resp(DBG_RESP_STATE, {"status": "done", "data": state})

    def _handle_source_request(self, path, line, ctx_lines):
        """Read source file and send to dashboard."""
        lines = self._source_cache.get(path)
        if lines is None:
            try:
                with open(path, "r", errors="replace") as f:
                    lines = f.readlines()
                self._source_cache[path] = lines
            except OSError:
                lines = []

        start = max(0, line - ctx_lines - 1)
        end = min(len(lines), line + ctx_lines)
        snippet = [l.rstrip("\n\r") for l in lines[start:end]]

        resp = {
            "file": path,
            "start_line": start + 1,
            "current_line": line,
            "lines": snippet,
        }
        self._send_debug_resp(DBG_RESP_SOURCE, {"status": "done", "data": resp})

    def _send_debug_resp(self, resp_type, data):
        """Send a FRAME_DEBUG_RESP to all WebSocket clients."""
        try:
            json_bytes = json.dumps(data, default=str).encode("utf-8")
        except Exception:
            return
        payload = struct.pack("<IB", FRAME_DEBUG_RESP, resp_type) + json_bytes
        frame = ws_frame(payload)
        mailbox.post_log(frame)


def _mi_parse_list(s):
    """Parse a GDB/MI list.  Handles:
      [{...},{...}]            → list of dicts
      [name={...},name={...}]  → list of dicts (strip name= prefix)
      ["str","str",...]        → list of strings
      other                    → raw string
    """
    s = s.strip()
    if not s:
        return []

    # Try quoted-string list first: "val","val",...
    if s.startswith('"'):
        strings = []
        i = 0
        while i < len(s):
            while i < len(s) and s[i] in " ,":
                i += 1
            if i >= len(s):
                break
            if s[i] == '"':
                i += 1
                val = []
                while i < len(s) and s[i] != '"':
                    if s[i] == '\\' and i + 1 < len(s):
                        val.append(s[i + 1])
                        i += 2
                    else:
                        val.append(s[i])
                        i += 1
                if i < len(s):
                    i += 1
                strings.append("".join(val))
            else:
                break
        if strings:
            return strings

    # Collect brace-delimited items, ignoring name= prefixes.
    items = []
    brace_depth = 0
    brace_start = -1
    i = 0
    while i < len(s):
        ch = s[i]
        if ch == '"':
            i += 1
            while i < len(s) and s[i] != '"':
                if s[i] == '\\':
                    i += 1
                i += 1
            i += 1
            continue
        if ch == '{':
            if brace_depth == 0:
                brace_start = i
            brace_depth += 1
        elif ch == '}':
            brace_depth -= 1
            if brace_depth == 0 and brace_start >= 0:
                items.append(_mi_parse_kv(s[brace_start + 1:i]))
                brace_start = -1
        i += 1

    if items:
        return items

    # No braces found — return as raw string
    return s


def _mi_parse_kv(s):
    """Minimal GDB/MI key=value parser. Returns a dict.

    Handles: key="val", key={nested}, key=[list], and key=word.
    Not a full MI parser but covers the common result records.
    """
    result = {}
    i = 0
    while i < len(s):
        # Skip whitespace and commas
        while i < len(s) and s[i] in " ,":
            i += 1
        if i >= len(s):
            break
        # Parse key
        j = i
        while j < len(s) and s[j] not in "=,}]":
            j += 1
        key = s[i:j].strip()
        if j >= len(s) or s[j] != "=":
            i = j + 1
            continue
        i = j + 1  # skip '='
        if i >= len(s):
            break
        # Parse value
        if s[i] == '"':
            # Quoted string
            i += 1
            val = []
            while i < len(s) and s[i] != '"':
                if s[i] == '\\' and i + 1 < len(s):
                    val.append(s[i + 1])
                    i += 2
                else:
                    val.append(s[i])
                    i += 1
            if i < len(s):
                i += 1  # skip closing quote
            result[key] = "".join(val)
        elif s[i] == '{':
            # Nested dict — find matching brace
            depth = 1
            start = i + 1
            i += 1
            while i < len(s) and depth > 0:
                if s[i] == '{':
                    depth += 1
                elif s[i] == '}':
                    depth -= 1
                elif s[i] == '"':
                    i += 1
                    while i < len(s) and s[i] != '"':
                        if s[i] == '\\':
                            i += 1
                        i += 1
                i += 1
            result[key] = _mi_parse_kv(s[start:i - 1])
        elif s[i] == '[':
            # List — find matching bracket
            depth = 1
            start = i + 1
            i += 1
            while i < len(s) and depth > 0:
                if s[i] == '[':
                    depth += 1
                elif s[i] == ']':
                    depth -= 1
                elif s[i] == '"':
                    i += 1
                    while i < len(s) and s[i] != '"':
                        if s[i] == '\\':
                            i += 1
                        i += 1
                i += 1
            inner = s[start:i - 1].strip()
            result[key] = _mi_parse_list(inner)
        else:
            # Bare word
            j = i
            while j < len(s) and s[j] not in ",}]":
                j += 1
            result[key] = s[i:j].strip()
            i = j
    return result


# Global GDB controller instance (set in main if --gdb-port/--gdb-attach)
gdb_controller = [None]

# ── Main ─────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="oveRTOS dashboard bridge (shmem → WebSocket)")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--dashboard", type=str, default=None,
                        help="Path to dashboard static files")
    parser.add_argument("--log-fd", type=int, default=-1,
                        help="File descriptor to read console log from")
    parser.add_argument("--gdb-port", type=int, default=0,
                        help="GDB remote port (QEMU gdbserver)")
    parser.add_argument("--gdb-attach", type=int, default=0,
                        help="PID to attach GDB to (POSIX mode)")
    parser.add_argument("--gdb-toolchain", type=str, default="gdb",
                        help="GDB binary name (e.g. arm-none-eabi-gdb)")
    parser.add_argument("--elf-path", type=str, default=None,
                        help="Path to ELF for debug symbols")
    parser.add_argument("--build-dir", type=str, default=None,
                        help="CMake build dir (for source file list)")
    args = parser.parse_args()

    ove_dir = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))

    if args.dashboard:
        dashboard_dir = args.dashboard
    else:
        dashboard_dir = os.path.join(ove_dir, "sim", "dashboard")
    if not os.path.isdir(dashboard_dir):
        print(f"ERROR: Dashboard not found at {dashboard_dir}",
              file=sys.stderr)
        sys.exit(1)

    # Build source file list from CMake build objects.
    if args.build_dir:
        build_project_file_list(args.build_dir, ove_dir)

    threading.Thread(target=display_bridge, daemon=True).start()
    threading.Thread(target=audio_bridge, daemon=True).start()
    threading.Thread(target=event_bridge, daemon=True).start()
    if args.log_fd >= 0:
        threading.Thread(target=log_bridge, args=(args.log_fd,),
                         daemon=True).start()

    # Start GDB controller if configured.
    if args.gdb_port or args.gdb_attach:
        gdb = GdbController(
            toolchain=args.gdb_toolchain,
            elf_path=args.elf_path,
            gdb_port=args.gdb_port if args.gdb_port else None,
            attach_pid=args.gdb_attach if args.gdb_attach else None,
        )
        gdb_controller[0] = gdb
        # Start GDB in a thread — connection may take a moment.
        def _start_gdb():
            # Wait for target: QEMU needs time to start gdbserver;
            # local attach is immediate.
            if args.gdb_port:
                time.sleep(1)
            else:
                time.sleep(0.2)
            try:
                gdb.start()
                print(f"GDB connected ({args.gdb_toolchain})", flush=True)
            except Exception as e:
                msg = str(e)
                if "ptrace" in msg.lower() or "Operation not permitted" in msg:
                    print("GDB attach failed: ptrace not permitted. "
                          "Run: sudo sysctl kernel.yama.ptrace_scope=0",
                          flush=True)
                else:
                    print("GDB connection failed:", flush=True)
                    traceback.print_exc()
        threading.Thread(target=_start_gdb, daemon=True).start()

    try:
        serve(args.port, dashboard_dir)
    except KeyboardInterrupt:
        gdb = gdb_controller[0]
        if gdb:
            gdb.stop()


if __name__ == "__main__":
    main()
