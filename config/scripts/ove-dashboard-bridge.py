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
FRAME_TRACE      = 0x0E
FRAME_PROFILE    = 0x0F

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
        # Build poll list: server + active client sockets. Guard against
        # fd=-1 entries — a client whose handshake threw (e.g. browser
        # RST mid-upgrade) already had its socket closed; if it wasn't
        # also marked ST_DONE, select() would blow up on the stale fd
        # and take down the whole server loop.
        rlist = [srv]
        for c in clients:
            if c.state == ST_DONE:
                continue
            try:
                if c.sock.fileno() < 0:
                    c.state = ST_DONE
                    continue
            except Exception:
                c.state = ST_DONE
                continue
            rlist.append(c.sock)
        try:
            readable, _, _ = select.select(rlist, [], [], 0.015)
        except (ValueError, OSError) as e:
            # Belt-and-suspenders: if a race still snuck a bad fd into
            # rlist, drop the offenders and carry on instead of dying.
            sys.stderr.write(f"[bridge] select() error: {e}; purging\n")
            sys.stderr.flush()
            for c in list(clients):
                try:
                    if c.sock.fileno() < 0:
                        c.state = ST_DONE
                except Exception:
                    c.state = ST_DONE
            readable = []

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
                            # Audio-inject short-circuit: cmd_type 0 with
                            # a real plugin ID (skip console sentinel)
                            # writes PCM directly into the audio ring.
                            if (plugin_id != 0xFFFFFFFF
                                    and cmd_type == 0 and pcm):
                                audio_input_write(pcm)
                            else:
                                # Forward to the firmware's shm cmd ring.
                                # The sim-debug pump drains it and
                                # dispatches to the targeted plugin.
                                sim_cmd_send(plugin_id, cmd_type, pcm)
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

            # Profiler symbol map (one-shot; samples that arrive later
            # reference pc values that map into this table).
            if _profiler_symbol_frame:
                c.sock.sendall(_profiler_symbol_frame)

            # Profiler capabilities (max/current Hz) — cached at the
            # bridge so late-joining clients populate their rate dropdown
            # without needing the firmware to re-announce.
            if _profiler_caps_frame:
                c.sock.sendall(_profiler_caps_frame)

        except Exception:
            # Browser tab close / page reload during the initial-state
            # dump RSTs the socket, which breaks our sendall chain.
            # Mark ST_DONE so the main loop removes us next tick; not
            # doing this left the client in the polling list with a
            # closed fd and crashed select() on the next iteration.
            try:
                c.sock.close()
            except Exception:
                pass
            c.state = ST_DONE
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
# Trace plugin event types (see sim/include/ove/sim/ove_sim_trace.h).
# Values are picked to not collide with SIM_DEBUG_EVT_THREADS=0.
SIM_TRACE_EVT_STREAM      = 10
SIM_TRACE_EVT_DESCRIPTORS = 11

# Profiler plugin event types (see sim/include/ove/sim/ove_sim_profiler.h).
SIM_PROFILER_EVT_SAMPLES  = 20
SIM_PROFILER_EVT_CAPS     = 21

# Profiler frame subtypes (bridge-synthesised).
PROFILE_SUB_SAMPLES = 1
PROFILE_SUB_SYMBOLS = 2
PROFILE_SUB_CAPS    = 3

# Sim shm cmd-ring writer (host → guest). Module-level so callers don't
# reopen the file on every command; the header offsets mirror
# sim_shm_header in sim/include/ove/sim/ove_sim_shm.h.
#   offset 24: cmd_write_pos
#   offset 28: cmd_read_pos
SIM_CMD_WRITE_POS_OFF = 24
SIM_CMD_READ_POS_OFF  = 28
SIM_CMD_RING_OFF      = SIM_SHM_HDR_SIZE + SIM_SHM_RING_SIZE

_sim_cmd_lock = threading.Lock()


def sim_cmd_send(plugin_id, cmd_type, data):
    """Forward a plugin command to the firmware via the shm cmd ring.

    The firmware's sim-debug pump drains this ring each tick and
    dispatches to plugins. Writing into the ring mirrors the layout
    used by ove_sim_transport_shm_local's local_recv_cmd:
      [uint16 length][ove_sim_cmd header + data]
    """
    if not os.path.exists(SIM_SHM_PATH):
        return False

    hdr_bytes = struct.pack("<III", plugin_id, cmd_type, len(data))
    body = hdr_bytes + data
    total = len(body)
    if total > 0xFFFF:
        return False

    with _sim_cmd_lock:
        try:
            fd = os.open(SIM_SHM_PATH, os.O_RDWR)
        except OSError:
            return False
        try:
            magic_buf = os.pread(fd, 4, 0)
            if len(magic_buf) < 4 or \
                    struct.unpack_from("<I", magic_buf, 0)[0] != SIM_SHM_MAGIC:
                return False

            wpos_buf = os.pread(fd, 4, SIM_CMD_WRITE_POS_OFF)
            rpos_buf = os.pread(fd, 4, SIM_CMD_READ_POS_OFF)
            if len(wpos_buf) < 4 or len(rpos_buf) < 4:
                return False
            wpos = struct.unpack_from("<I", wpos_buf, 0)[0]
            rpos = struct.unpack_from("<I", rpos_buf, 0)[0]
            used = (wpos - rpos) & 0xFFFFFFFF
            if used + 2 + total > SIM_SHM_RING_SIZE:
                return False

            mask = SIM_SHM_RING_SIZE - 1

            # Write length prefix (two bytes, handling wrap).
            lenb = struct.pack("<H", total)
            off = SIM_CMD_RING_OFF + (wpos & mask)
            os.pwrite(fd, lenb[0:1], off)
            off2 = SIM_CMD_RING_OFF + ((wpos + 1) & mask)
            os.pwrite(fd, lenb[1:2], off2)
            wpos = (wpos + 2) & 0xFFFFFFFF

            # Write body (handle wrap).
            ring_off = wpos & mask
            first = min(SIM_SHM_RING_SIZE - ring_off, total)
            os.pwrite(fd, body[:first], SIM_CMD_RING_OFF + ring_off)
            if first < total:
                os.pwrite(fd, body[first:], SIM_CMD_RING_OFF)
            wpos = (wpos + total) & 0xFFFFFFFF

            os.pwrite(fd, struct.pack("<I", wpos), SIM_CMD_WRITE_POS_OFF)
            return True
        finally:
            os.close(fd)


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
    global _profiler_caps_frame
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
            # Forward trace plugin events as FRAME_TRACE. Both subtypes
            # (stream records, descriptor table) share the same outer
            # frame; the dashboard dispatches on the embedded sub_type.
            elif event_type in (SIM_TRACE_EVT_STREAM,
                                SIM_TRACE_EVT_DESCRIPTORS) and payload:
                frame = ws_frame(
                    struct.pack("<I", FRAME_TRACE) + payload)
                mailbox.post_log(frame)
                if (event_type == SIM_TRACE_EVT_DESCRIPTORS
                        and os.environ.get("OVE_PROFILER_DUMP")):
                    _trace_dump_descriptors(payload)
            # Forward profiler sample batches as FRAME_PROFILE with
            # subtype=SAMPLES. The symbol table is synthesised once
            # per ELF and emitted on WebSocket connect — see mailbox.
            elif event_type == SIM_PROFILER_EVT_SAMPLES and payload:
                payload = _prune_profile_payload(payload)
                frame = ws_frame(
                    struct.pack("<IB", FRAME_PROFILE, PROFILE_SUB_SAMPLES)
                    + payload)
                mailbox.post_log(frame)
                if os.environ.get("OVE_PROFILER_DUMP"):
                    _profiler_dump_samples(payload)
            # Forward profiler capability events (max_hz, current_hz).
            # The dashboard uses these to populate its rate dropdown.
            # Also cache for late-joining WS clients — the firmware only
            # emits this on state changes (start + set-rate), so without
            # the cache a browser reload after initial announce would
            # never see the ceiling and the dropdown would stay empty.
            elif event_type == SIM_PROFILER_EVT_CAPS and payload:
                frame = ws_frame(
                    struct.pack("<IB", FRAME_PROFILE, PROFILE_SUB_CAPS)
                    + payload)
                _profiler_caps_frame = frame
                mailbox.post_log(frame)
                if len(payload) >= 8:
                    max_hz, cur_hz = struct.unpack_from("<II", payload, 0)
                    sys.stderr.write(
                        f"[event-bridge] profiler caps: "
                        f"max={max_hz} Hz, current={cur_hz} Hz\n")
                    sys.stderr.flush()
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
            elif ("FreeRTOS" in abs_path or "freertos" in abs_path.lower()
                  or "/nuttx/" in abs_path.lower()
                  or "/dl/nuttx" in abs_path.lower()):
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


# ── Profiler symbol map (nm-based) ──────────────────────────────────

_profiler_symbol_frame = None  # cached WS frame for PROFILE_SUB_SYMBOLS
_profiler_caps_frame   = None  # cached WS frame for PROFILE_SUB_CAPS

# Debug-only: when OVE_PROFILER_DUMP=1 is set, decode each sample batch
# and print the resolved frames per sample to stderr. Used to inspect
# profiler output while iterating on the stack-unwinding heuristics.
_profiler_sym_index = None  # list of (pc_start, pc_end, name)


def _profiler_resolve(pc):
    if not _profiler_sym_index:
        return f"0x{pc:x}"
    lo, hi = 0, len(_profiler_sym_index) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        ps, pe, nm = _profiler_sym_index[mid]
        if pc < ps:
            hi = mid - 1
        elif pc >= pe:
            lo = mid + 1
        else:
            return nm.split("@", 1)[0]
    return f"0x{pc:x}"


def _profiler_dump_samples(payload):
    """Decode a PROFILE_SUB_SAMPLES payload and print per-sample frames."""
    if len(payload) < 8:
        return
    try:
        _ver, word_size, count, dropped = struct.unpack_from(
            "<BBHI", payload, 0)
    except struct.error:
        return
    if word_size != 8:
        return
    p = 8
    for _ in range(count):
        if p + 16 > len(payload):
            return
        ts, tid, depth, state = struct.unpack_from("<QIBB", payload, p)
        p += 16
        # skip 2-byte pad
        if p + depth * 8 > len(payload):
            return
        pcs = []
        for _j in range(depth):
            (pc,) = struct.unpack_from("<Q", payload, p)
            pcs.append(pc)
            p += 8
        frames = " <- ".join(f"{_profiler_resolve(x)}@0x{x:x}" for x in pcs)
        sys.stderr.write(
            f"[dump] t={ts} tid=0x{tid:x} st={state} d={depth} {frames}\n")
    sys.stderr.flush()


def _trace_dump_descriptors(payload):
    """Decode a FRAME_TRACE descriptor batch (subType=0) and log tid→name
    pairs so an offline aggregator can filter idle/pump threads the same
    way the dashboard does. Only DESCRIPTORS; stream records are skipped."""
    if len(payload) < 8:
        return
    try:
        sub, _ver, count, _dropped = struct.unpack_from("<BBHI", payload, 0)
    except struct.error:
        return
    if sub != 0:
        return
    p = 8
    for _ in range(count):
        if p + 5 > len(payload):
            return
        (tid,) = struct.unpack_from("<I", payload, p); p += 4
        name_len = payload[p]; p += 1
        if p + name_len > len(payload):
            return
        name = payload[p:p + name_len].decode("utf-8", "replace")
        p += name_len
        sys.stderr.write(f"[dump-desc] tid=0x{tid:x} name={name}\n")
    sys.stderr.flush()


def _is_pie_elf(elf_path):
    """Best-effort: ELF Type = DYN means PIE (or shared library)."""
    try:
        out = subprocess.check_output(
            ["readelf", "-h", elf_path],
            stderr=subprocess.DEVNULL).decode("utf-8", "replace")
        for line in out.splitlines():
            if "Type:" in line:
                return "DYN" in line
    except Exception:
        pass
    return False


def _enumerate_text_segments(pid):
    """Parse /proc/<pid>/maps and return a dict of
    { realpath: (load_base, pc_start, pc_end) } for every unique
    executable file-backed mapping. load_base = (start - file_offset)
    so nm addresses add directly onto it. pc_start/pc_end bound the
    mapping in runtime address space — used downstream to stop
    range-extension from leaking across mappings. Returns {} on
    failure."""
    if not pid:
        return {}
    result = {}
    try:
        with open(f"/proc/{pid}/maps") as f:
            for line in f:
                parts = line.split(maxsplit=5)
                if len(parts) < 6:
                    continue
                perms = parts[1]
                if "x" not in perms:
                    continue
                path = parts[5].strip()
                if not path or path.startswith("["):
                    continue      # anon / [vdso] / [vsyscall] / etc.
                if not os.path.isfile(path):
                    continue
                real = os.path.realpath(path)
                if real in result:
                    continue     # first text mapping wins
                try:
                    start_s, end_s = parts[0].split("-")
                    start = int(start_s, 16)
                    end = int(end_s, 16)
                    off = int(parts[2], 16)
                except ValueError:
                    continue
                result[real] = (start - off, start, end)
    except OSError:
        return {}
    return result


def _nm_symbols(path, base, want_dynamic, nm_bin="nm"):
    """Run nm and return a list of (addr, size, name) tuples already
    shifted by @base. On shared libraries, static symbols are stripped
    so pass want_dynamic=True to ask nm for the dynamic symbol table.
    @nm_bin lets the cross-toolchain's nm be used for non-host ELFs
    (e.g. arm-none-eabi-nm on QEMU FreeRTOS firmware)."""
    cmd = [nm_bin, "-n", "-S", "--defined-only", "-C"]
    if want_dynamic:
        cmd.append("-D")
    cmd.append(path)
    try:
        out = subprocess.check_output(
            cmd, stderr=subprocess.DEVNULL).decode("utf-8", "replace")
    except (FileNotFoundError, subprocess.CalledProcessError):
        return []
    syms = []
    for line in out.splitlines():
        parts = line.split(maxsplit=3)
        if len(parts) == 3:
            addr_s, tcode, name = parts
            size_s = None
        elif len(parts) == 4:
            addr_s, size_s, tcode, name = parts
        else:
            continue
        if tcode not in ("T", "t", "W", "w", "i"):
            continue
        try:
            addr = int(addr_s, 16)
            size = int(size_s, 16) if size_s else 0
        except ValueError:
            continue
        syms.append((addr + base, size, name))
    return syms


def build_profiler_symbols(elf_path, pid=None, nm_bin="nm"):
    """Build a PROFILE_SUB_SYMBOLS WebSocket frame covering the main ELF
    *and* every shared library mapped into the process.

    Payload after the 5-byte outer header is a UTF-8 JSON array of
    [pc_start, pc_end, name] triples sorted by pc_start. Without shared
    libs included, samples that land in libc/libpthread/ld-linux show
    up as raw 0x7f… addresses in the dashboard.

    @nm_bin is forwarded to _nm_symbols so cross-compiled firmware (QEMU
    FreeRTOS) can be indexed with arm-none-eabi-nm; the ARM ELF doesn't
    have shared-library segments, so the main-ELF fallback path covers it.
    """
    global _profiler_symbol_frame
    if not elf_path or not os.path.isfile(elf_path):
        return

    segs = _enumerate_text_segments(pid)  # dict path -> (base, pc_start, pc_end)
    real_main = os.path.realpath(elf_path)

    # Fall back to nm-only on the main ELF if /proc/maps was unreadable
    # (no pid — QEMU mode — or an unreadable /proc entry).
    if not segs:
        segs = {real_main: (0, 0, 0)}

    # Per-mapping extension cap: never inflate a single symbol past this
    # size, so e.g. the last named symbol in libc before a big stripped
    # internal region can't swallow everything up to the mapping end.
    MAX_SYM_EXTEND = 0x10000   # 64 KiB

    entries = []
    for path, (base, pc_start, pc_end) in segs.items():
        is_main = (path == real_main)
        # Shared libs: .symtab (-T) is usually stripped → use -D.
        # The main ELF often has both; prefer -T for completeness, but
        # fall back to -D if nothing came out (e.g. stripped builds).
        syms = _nm_symbols(path, base, want_dynamic=False, nm_bin=nm_bin)
        if not syms:
            syms = _nm_symbols(path, base, want_dynamic=True, nm_bin=nm_bin)
        elif not is_main:
            # Always augment shared libs with dynamic symbols — static
            # nm on many distro libs returns nothing even if the file
            # isn't stripped.
            syms += _nm_symbols(path, base, want_dynamic=True, nm_bin=nm_bin)
        if not syms:
            continue

        # Per-mapping sort + dedup + gap-fill. Extending each symbol's
        # end to the next within the same mapping covers nm holes
        # (trampolines, PLT stubs, padding) without leaking across
        # mappings — which would happen if we sorted globally and
        # let `data_start` extend into libc's ASLR space.
        syms.sort(key=lambda s: s[0])
        mapping_end_cap = pc_end if pc_end else (
            syms[-1][0] + MAX_SYM_EXTEND)
        seen = set()
        n = len(syms)
        for i, (addr, _size, name) in enumerate(syms):
            if addr in seen:
                continue
            seen.add(addr)
            # Next distinct-address symbol in this mapping.
            next_start = None
            for k in range(i + 1, n):
                na = syms[k][0]
                if na > addr:
                    next_start = na
                    break
            hard_cap = min(
                addr + MAX_SYM_EXTEND,
                mapping_end_cap,
            )
            end = min(next_start, hard_cap) if next_start else hard_cap
            if end <= addr:
                end = addr + 1
            entries.append([addr, end, name])

    if not entries:
        return

    entries.sort(key=lambda e: e[0])

    # Cache for OVE_PROFILER_DUMP resolver. No-op in normal runs.
    global _profiler_sym_index
    _profiler_sym_index = [(e[0], e[1], e[2]) for e in entries]

    payload = json.dumps(entries).encode("utf-8")
    _profiler_symbol_frame = ws_frame(
        struct.pack("<IB", FRAME_PROFILE, PROFILE_SUB_SYMBOLS) + payload)

    sys.stderr.write(
        f"[profiler] {len(entries)} symbols indexed from "
        f"{len(segs)} segment(s) ({len(payload)} B)\n")
    sys.stderr.flush()


# ── Profiler CFG validator (objdump-based) ──────────────────────────
#
# The profiler unwinds by walking saved-{r7, lr} pairs on the task
# stack. Functions with large local frames (e.g. LVGL draw callbacks
# with 200+ B of locals) sometimes contain leftover {r7, lr} pairs
# from previously-completed deep calls — these look identical to a
# live frame and can't be rejected by the r7-invariant check alone.
#
# Bridge-side fix: once at startup, disassemble the firmware ELF with
# objdump and build a per-function call-graph of direct BL targets.
# Each profiler sample arriving as a (leaf, lr1, lr2, ...) chain is
# validated adjacent-pairwise: for each (callee, apparent-caller), if
# the caller has no `bl callee` instruction *and* no `blx reg` /
# indirect branch, the caller frame is dropped as phantom.
#
# Tail calls are handled: `F tail-calls G` via an unconditional `b G`
# makes G inherit F's saved LR (into F's caller). Stack then shows
# `G <- F_caller` even though F_caller never directly called G. We
# compute tail-call closure and include transitive targets.
_profiler_cfg_calls = {}      # caller_name -> set(callee_name)  direct BLs
_profiler_cfg_reach = {}      # caller_name -> set(callee_name)  direct + tail-closure
_profiler_cfg_indirect = set()  # callers with ≥1 `blx reg` / `bx reg` (not lr)
_profiler_cfg_indirect_hub = set()  # callers with ≥2 `blx reg` — real dispatchers (event_send_core, etc.)

_CFG_FUNC_RE = re.compile(r'^[0-9a-f]+\s+<([^>]+)>:')
# `bl HEXTARGET <NAME(+0xOFFSET)?>` — direct call. objdump on ARM emits
# `bl` even for inter-Thumb-calls (no `bl.w` distinction).
_CFG_BL_RE = re.compile(r'\sbl\s+([0-9a-f]+)\s+<([^>]+)>')
# Unconditional branch `b(.w|.n)? HEXTARGET <NAME(+0xOFFSET)?>` —
# possible tail call if target is outside the current function.
_CFG_B_RE = re.compile(r'\sb(?:\.w|\.n)?\s+([0-9a-f]+)\s+<([^>]+)>')
# `blx reg` / `bx reg` (reg != lr) — indirect call that invalidates the
# caller's outgoing edges. `bx lr` is just return; filter it out.
_CFG_INDIRECT_RE = re.compile(
    r'\s(?:blx|bx)\s+(r\d+|ip|sl|sb|fp)\b')


def _build_profiler_cfg(elf_path, objdump_bin="objdump"):
    """Disassemble @elf_path and populate module-global CFG tables used
    by _prune_profile_payload(). Must be called AFTER
    build_profiler_symbols() so BL targets can be resolved to names via
    _profiler_resolve()."""
    global _profiler_cfg_calls, _profiler_cfg_reach
    global _profiler_cfg_indirect, _profiler_cfg_indirect_hub
    if not elf_path or not os.path.isfile(elf_path):
        return
    if not _profiler_sym_index:
        return
    try:
        out = subprocess.check_output(
            [objdump_bin, "-d", "--no-show-raw-insn", elf_path],
            stderr=subprocess.DEVNULL).decode("utf-8", "replace")
    except (FileNotFoundError, subprocess.CalledProcessError):
        return

    calls = {}      # name -> set of direct-BL callees
    tails = {}      # name -> set of tail-call targets (external B)
    indirect_count = {}  # name -> number of `blx reg` instructions
    cur = None

    for line in out.splitlines():
        m = _CFG_FUNC_RE.match(line)
        if m:
            cur = m.group(1)
            calls.setdefault(cur, set())
            tails.setdefault(cur, set())
            continue
        if cur is None:
            continue
        m = _CFG_BL_RE.search(line)
        if m:
            try:
                target = int(m.group(1), 16)
            except ValueError:
                continue
            tgt = _profiler_resolve(target)
            if tgt and not tgt.startswith("0x"):
                calls[cur].add(tgt)
            continue
        m = _CFG_INDIRECT_RE.search(line)
        if m:
            indirect_count[cur] = indirect_count.get(cur, 0) + 1
            continue
        m = _CFG_B_RE.search(line)
        if m:
            try:
                target = int(m.group(1), 16)
            except ValueError:
                continue
            tgt = _profiler_resolve(target)
            # Intra-function branch: target resolves to the same function
            # we're currently parsing. Skip — it's control flow, not a
            # tail call.
            if tgt and not tgt.startswith("0x") and tgt != cur:
                tails[cur].add(tgt)

    # Compute tail-call closure: when caller BL's some X, and X chains
    # through `b Y; b Z;` tail-calls, any of {X, Y, Z, ...} can appear
    # on the stack below caller.
    tc_cache = {}

    def tc_close(f, stk):
        if f in tc_cache:
            return tc_cache[f]
        if f in stk:
            return {f}
        stk.add(f)
        r = {f}
        for g in tails.get(f, ()):
            r |= tc_close(g, stk)
        stk.discard(f)
        tc_cache[f] = r
        return r

    # reach_1[F] = direct calls of F ∪ tail closures of direct callees.
    reach_1 = {}
    for f in calls:
        r = set(calls[f])
        for x in calls[f]:
            r |= tc_close(x, set())
        for g in tc_close(f, set()):
            r |= calls.get(g, set())
        reach_1[f] = r

    # Expand to 2-hop: reach[F] = reach_1[F] ∪ ⋃ reach_1[X] for X in
    # reach_1[F]. Covers the "unwinder missed a frame" case — e.g. an
    # LVGL draw_sw_fill call into blend where fill's large locals
    # corrupt the saved-r7 chain and fill drops out of the capture;
    # we still want to accept execute_drawing -> blend as a valid
    # (hole-containing) edge. 2-hop stays tight in this firmware
    # because callbacks dispatch via `blx reg`, which reach doesn't
    # traverse — event-dispatch functions don't reach draw code via
    # reach_2. Sampled separately at build time before relying on it.
    reach = {}
    for f in reach_1:
        r = set(reach_1[f])
        for x in list(reach_1[f]):
            r |= reach_1.get(x, set())
        reach[f] = r

    indirect = set(indirect_count)
    indirect_hub = {f for f, n in indirect_count.items() if n >= 2}

    _profiler_cfg_calls = calls
    _profiler_cfg_reach = reach
    _profiler_cfg_indirect = indirect
    _profiler_cfg_indirect_hub = indirect_hub

    sys.stderr.write(
        f"[profiler] CFG: {len(calls)} functions, "
        f"{len(indirect)} with indirect calls "
        f"({len(indirect_hub)} hubs ≥2 blx), "
        f"{sum(len(v) for v in calls.values())} direct-call edges, "
        f"{sum(len(v) for v in reach.values())} 2-hop reach edges\n")
    sys.stderr.flush()


_PRUNE_WINDOW = 4   # max consecutive phantoms we'll try to bypass


def _edge_allowed(callee, caller, strict=False):
    """True when `caller` could plausibly be the direct-or-transitive
    ancestor of `callee`. Allows when either side is unresolved. When
    strict=False (default, used for the primary adjacent-edge check),
    an indirect-dispatch hub caller (≥2 `blx reg` sites — e.g.
    event_send_core) also permits arbitrary callees. When strict=True
    (used for validator-scan targets that want to drop intermediate
    frames), hub-permit is disabled — only direct/reach edges count.
    Hub-permit at the scan target would wrongly accept a hub as the
    validator across several REAL intermediate frames, dropping them."""
    if caller not in _profiler_cfg_reach or callee.startswith("0x"):
        return True
    if callee in _profiler_cfg_reach[caller]:
        return True
    if strict:
        return False
    return caller in _profiler_cfg_indirect_hub


def _prune_profile_payload(payload):
    """Rewrite a PROFILE_SUB_SAMPLES payload, dropping phantom frames.
    For each (callee, apparent-caller) pair, if the edge is refuted AND
    a valid caller for the same callee exists within _PRUNE_WINDOW
    frames above, drop every refuted frame between and promote the
    validator. If no validator is reachable, keep the chain as-is —
    better to show a bogus caller than to shred a chain we can't
    confidently repair. No-op when CFG tables are empty."""
    if not _profiler_cfg_reach or len(payload) < 8:
        return payload
    try:
        ver, word_size, count, dropped = struct.unpack_from(
            "<BBHI", payload, 0)
    except struct.error:
        return payload
    if word_size != 8:
        return payload

    out = bytearray()
    out += struct.pack("<BBHI", ver, word_size, count, dropped)
    p = 8
    for _ in range(count):
        if p + 16 > len(payload):
            return payload
        ts, tid, depth, state = struct.unpack_from("<QIBB", payload, p)
        pad = struct.unpack_from("<H", payload, p + 14)[0]
        p += 16
        if p + depth * 8 > len(payload):
            return payload
        pcs = []
        for _j in range(depth):
            (pc,) = struct.unpack_from("<Q", payload, p)
            pcs.append(pc)
            p += 8

        names = [_profiler_resolve(pc) for pc in pcs]
        keep = [True] * len(pcs)

        # Duplicate-phantom pass: if the same function name appears at
        # multiple positions AND the CFG says the function is not a hub
        # and cannot reach itself (no recursion path in 2-hop), every
        # copy except the outermost is a stale-LR phantom. Drops the
        # very common `finalize_task_creation @ depth 6 AND 10` pattern
        # from the LVGL draw dispatch path.
        seen_at = {}
        for idx, nm in enumerate(names):
            if nm.startswith("0x"):
                continue
            seen_at.setdefault(nm, []).append(idx)
        for nm, positions in seen_at.items():
            if len(positions) < 2:
                continue
            if nm in _profiler_cfg_indirect_hub:
                continue
            if nm in _profiler_cfg_reach.get(nm, ()):
                continue  # real recursion
            # Keep only the outermost occurrence.
            for idx in positions[:-1]:
                keep[idx] = False

        # Walk from leaf. i = current validated callee index.
        i = 0
        while i < len(keep):
            if not keep[i]:
                i += 1
                continue
            # Find next kept neighbour.
            j = i + 1
            while j < len(keep) and not keep[j]:
                j += 1
            if j >= len(keep):
                break
            callee = names[i]
            if _edge_allowed(callee, names[j]):
                i = j
                continue
            # Edge refuted. Scan up to _PRUNE_WINDOW frames higher for
            # a validator. If found, mark every refuted intermediate
            # frame as phantom. strict=True because we don't want a
            # hub caller 3 frames up to hub-permit across real
            # intermediate frames (e.g. finalize_task_creation
            # hub-permitting `dispatch` would drop real lv_draw_dispatch
            # and lv_draw_dispatch_layer frames in between).
            k = j + 1
            hops = 0
            found = -1
            while k < len(keep) and hops < _PRUNE_WINDOW:
                if not keep[k]:
                    k += 1
                    continue
                if _edge_allowed(callee, names[k], strict=True):
                    found = k
                    break
                k += 1
                hops += 1
            if found >= 0:
                for m in range(j, found):
                    if keep[m]:
                        keep[m] = False
                i = found
            else:
                # No validator — keep j as an unvalidated caller.
                i = j

        # Firm-refuted up-edge pass: drop a frame whose UP edge
        # (current-frame called-by next-up-frame) is firmly refuted —
        # the up-caller has a resolvable reach set, callee is not in
        # it, AND the up-caller has ZERO indirect call sites. With no
        # indirect calls, the CFG reach is COMPLETE (direct bl/b +
        # tail-call closure); missing callee means the call is truly
        # impossible. Catches stale-LR phantoms wedged between real
        # frames where the down-edge happens to be CFG-valid so the
        # isolated-phantom pass (which needs BOTH edges refuted) can't
        # touch them. Example: in LVGL label draw, a previous rect
        # draw leaves `finalize_task_creation @ lv_draw_rect` on the
        # stack, so the chain shows
        # `iterate_characters <- finalize_task_creation <- lv_draw_rect
        # <- lv_draw_sw_label`; `sw_label` has 0 indirect callsites
        # and doesn't reach `lv_draw_rect`, so that edge is firm-
        # refuted and `lv_draw_rect` is dropped. Iterating lets the
        # now-exposed `finalize_task_creation <- lv_draw_sw_label`
        # edge also get firm-refuted and dropped.
        changed = True
        while changed:
            changed = False
            kept_idx = [k for k, v in enumerate(keep) if v]
            for pos in range(len(kept_idx) - 1):
                cur = kept_idx[pos]
                up = kept_idx[pos + 1]
                up_name = names[up]
                cur_name = names[cur]
                if (up_name in _profiler_cfg_reach
                        and not cur_name.startswith("0x")
                        and cur_name not in _profiler_cfg_reach[up_name]
                        and up_name not in _profiler_cfg_indirect):
                    keep[cur] = False
                    changed = True
                    break

        # Isolated-phantom pass: a frame whose edges to BOTH its
        # surviving upstream and downstream neighbours refute (loose)
        # is almost certainly a stale-LR phantom wedged between real
        # frames. The window-validator pass can miss these when the
        # phantom is an indirect non-hub (permits nothing strict, but
        # not refuted-enough to trigger the scan). Example:
        # `draw_letter_cb <- lv_draw_buf_width_to_stride_ex <-
        # lv_draw_unit_draw_letter` — buf_width is indirect but not a
        # hub, and sits where the real chain is
        # unit_draw_letter -> draw_letter_cb via blx.
        changed = True
        while changed:
            changed = False
            kept_idx = [k for k, v in enumerate(keep) if v]
            for pos in range(1, len(kept_idx) - 1):
                mid = kept_idx[pos]
                down = kept_idx[pos - 1]    # closer to leaf (callee)
                up = kept_idx[pos + 1]      # closer to outer (caller)
                if (not _edge_allowed(names[down], names[mid])
                        and not _edge_allowed(names[mid], names[up])):
                    keep[mid] = False
                    changed = True
                    break

        # Top-of-chain trim: if the outermost surviving frame has a
        # refuted edge to the frame below, drop it and keep trimming
        # until the top validates or we run out. Catches phantoms at
        # the end of the unwound chain with nothing above to validate.
        kept_idx = [i for i, k in enumerate(keep) if k]
        while len(kept_idx) >= 2:
            top = kept_idx[-1]
            below = kept_idx[-2]
            if _edge_allowed(names[below], names[top]):
                break
            keep[top] = False
            kept_idx.pop()

        new_pcs = [pc for pc, k in zip(pcs, keep) if k]
        new_depth = len(new_pcs)
        out += struct.pack("<QIBBH", ts, tid, new_depth, state, pad)
        for pc in new_pcs:
            out += struct.pack("<Q", pc)
    return bytes(out)


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

        # Don't stop/report the real-time signals the sampling profiler
        # uses internally (SIGRTMIN..SIGRTMAX). Without this, every sample
        # pauses the inferior and floods the event stream. SIGRTMIN is
        # 34 on glibc/Linux; covering 34..64 is safe for any backend.
        for sig in range(34, 65):
            self._send_mi_sync(
                f'-interpreter-exec console "handle SIG{sig} nostop noprint pass"')

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

    # Build profiler symbol map from the ELF. For POSIX (native nm on a
    # PIE binary) we need the runtime load base; read it from the attached
    # PID's /proc/<pid>/maps. For QEMU FreeRTOS firmware the ELF is ARM
    # and system nm may not handle the architecture on every host — when
    # --gdb-toolchain points at arm-none-eabi-gdb, derive the matching
    # arm-none-eabi-nm alongside it and feed it to build_profiler_symbols.
    if args.elf_path:
        nm_bin = "nm"
        objdump_bin = None
        gdb = args.gdb_toolchain or ""
        if gdb.endswith("-gdb") and len(gdb) > len("-gdb"):
            nm_candidate = gdb[:-len("-gdb")] + "-nm"
            if os.path.isfile(nm_candidate):
                nm_bin = nm_candidate
            od_candidate = gdb[:-len("-gdb")] + "-objdump"
            if os.path.isfile(od_candidate):
                objdump_bin = od_candidate
        build_profiler_symbols(
            args.elf_path,
            pid=args.gdb_attach if args.gdb_attach else None,
            nm_bin=nm_bin)
        # CFG validator: reject phantom frames left over from stale
        # saved-{r7,lr} pairs in functions with large local frames. Only
        # meaningful for the stack-scan unwinder on QEMU/FreeRTOS — POSIX
        # uses backtrace(3) (DWARF-based, phantom-free) and disassembling
        # libc-linked host binaries would cost startup time for zero gain.
        # Gated on a cross-toolchain objdump actually resolving.
        if objdump_bin:
            _build_profiler_cfg(args.elf_path, objdump_bin=objdump_bin)

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
