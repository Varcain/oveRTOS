# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import array
import codecs
import mmap
import os
import select
import struct
import sys
import threading
import time
import traceback

from . import frames, profiler
from .protocol import (
    FRAME_AUDIO,
    FRAME_FB,
    FRAME_LOG,
    FRAME_PROFILE,
    FRAME_SERIAL,
    FRAME_THREAD,
    FRAME_TRACE,
    PROFILE_SUB_CAPS,
    PROFILE_SUB_SAMPLES,
)

# ── Shared-memory constants ──────────────────────────────────────────

FB_PATH = "/dev/shm/ove-fb"
FB_MAGIC = 0x42465854
FB_HEADER_SIZE = 16

AUDIO_PATH = "/dev/shm/ove-audio"

# Pointer input — 16-byte struct qemu-run.sh creates before the firmware
# starts (the guest reads it through semihosting). We mmap it lazily on
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
                    frames._last_fb_frame = payload
                    frames.post_frame(None)
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
                frames.post_frame(payload)

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
SIM_EVENT_DROPPED_OFF = 32  # struct sim_shm_header.event_dropped

# ove_sim_event header: plugin_id(4) + event_type(4) + timestamp_ms(4) + data_len(4)
SIM_EVENT_HDR_SIZE = 16
# Per-plugin event and command ranges are defined in sim/include/ove/sim/ove_sim_plugin.h.
SIM_DEBUG_EVT_THREADS = 0
SIM_TRACE_EVT_STREAM      = 10
SIM_TRACE_EVT_DESCRIPTORS = 11
SIM_PROFILER_EVT_SAMPLES  = 20
SIM_PROFILER_EVT_CAPS     = 21

SIM_AUDIO_CMD_INJECT = 10
SIM_PROFILER_CMD_SET_RATE = 20

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
    sim/src/ove_sim_transport_shm_guest.c reads:
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


_profiler_caps_frame   = None  # cached payload for PROFILE_SUB_CAPS
_learned_profiler_id = None


def _learn_profiler_id(plugin_id):
    global _learned_profiler_id
    if _learned_profiler_id is None:
        _learned_profiler_id = plugin_id
        sys.stderr.write(f"[event-bridge] profiler plugin id = {plugin_id}\n")
        sys.stderr.flush()


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
        last_dropped = 0
        ring_mask = SIM_SHM_RING_SIZE - 1
        sys.stderr.write("[event-bridge] SIM SHM ready\n")
        sys.stderr.flush()

        while True:
            # The guest refuses an event that does not fit instead of
            # lapping this reader; report the loss it counted.
            dbuf = os.pread(fd, 4, SIM_EVENT_DROPPED_OFF)
            if len(dbuf) == 4:
                dropped = struct.unpack_from("<I", dbuf, 0)[0]
                if dropped < last_dropped:  # the guest reset its header
                    last_dropped = 0
                if dropped != last_dropped:
                    text = f"[sim] {dropped - last_dropped} event(s) dropped: host behind\n"
                    sys.stderr.write(text)
                    sys.stderr.flush()
                    frames.post_frame(struct.pack("<I", FRAME_LOG) + text.encode("utf-8"))
                    last_dropped = dropped

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
            # Positions are free-running u32 counters: wrap like the guest.
            rpos = (rpos + 2) & 0xFFFFFFFF

            if msg_len < SIM_EVENT_HDR_SIZE or msg_len > 4096:
                rpos = (rpos + msg_len) & 0xFFFFFFFF
                os.pwrite(fd, struct.pack("<I", rpos), 20)
                continue

            # Read event from ring (handle wrap-around)
            ring_off = rpos & ring_mask
            first = min(SIM_SHM_RING_SIZE - ring_off, msg_len)
            buf = os.pread(fd, first, SIM_SHM_EVENT_OFF + ring_off)
            if first < msg_len:
                buf += os.pread(fd, msg_len - first, SIM_SHM_EVENT_OFF)
            rpos = (rpos + msg_len) & 0xFFFFFFFF

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
                frames.post_frame(struct.pack("<I", FRAME_THREAD) + payload)
            # Forward trace plugin events as FRAME_TRACE. All sub_types
            # (stream records, thread descriptors, user names) share the
            # same outer frame; the dashboard dispatches on the embedded
            # sub_type.
            elif event_type in (SIM_TRACE_EVT_STREAM,
                                SIM_TRACE_EVT_DESCRIPTORS) and payload:
                frames.post_frame(struct.pack("<I", FRAME_TRACE) + payload)
                if (event_type == SIM_TRACE_EVT_DESCRIPTORS
                        and os.environ.get("OVE_PROFILER_DUMP")):
                    profiler._trace_dump_descriptors(payload)
            # Forward profiler sample batches as FRAME_PROFILE with
            # subtype=SAMPLES. The symbol table is synthesised once
            # per ELF and emitted on WebSocket connect.
            elif event_type == SIM_PROFILER_EVT_SAMPLES and payload:
                _learn_profiler_id(plugin_id)
                payload = profiler._prune_profile_payload(payload)
                frames.post_frame(struct.pack("<IB", FRAME_PROFILE, PROFILE_SUB_SAMPLES)
                           + payload)
                if os.environ.get("OVE_PROFILER_DUMP"):
                    profiler._profiler_dump_samples(payload)
            # Forward profiler capability events (max_hz, current_hz, flags).
            # The dashboard uses these to populate its rate dropdown.
            # Also cache for late-joining WS clients — the firmware only
            # emits this on state changes (start + set-rate), so without
            # the cache a browser reload after initial announce would
            # never see the ceiling and the dropdown would stay empty.
            elif event_type == SIM_PROFILER_EVT_CAPS and payload:
                _learn_profiler_id(plugin_id)
                _profiler_caps_frame = (
                    struct.pack("<IB", FRAME_PROFILE, PROFILE_SUB_CAPS) + payload)
                frames.post_frame(_profiler_caps_frame)
                if len(payload) >= 8:
                    max_hz, cur_hz = struct.unpack_from("<II", payload, 0)
                    flags = (struct.unpack_from("<I", payload, 8)[0]
                             if len(payload) >= 12 else 0)
                    mode = "wall-clock" if flags & 1 else "on-CPU"
                    sys.stderr.write(
                        f"[event-bridge] profiler caps: "
                        f"max={max_hz} Hz, current={cur_hz} Hz, "
                        f"sampling={mode}\n")
                    sys.stderr.flush()
    finally:
        os.close(fd)

# ── Log and serial bridges ──────────────────────────────────────────

def log_bridge(read_fd):
    """Broadcast log lines and partial lines as FRAME_LOG."""
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    header = struct.pack("<I", FRAME_LOG)
    pending = ""
    with open(read_fd, "rb", buffering=0) as f:
        while True:
            if pending and (len(pending) >= 4096 or not select.select([f], [], [], 0.05)[0]):
                frames.post_frame(header + pending.encode())
                pending = ""
            data = f.read(4096)
            pending += decoder.decode(data, final=not data)
            end = pending.rfind("\n") + 1 if data else len(pending)
            if end:
                frames.post_frame(header + pending[:end].encode())
                pending = pending[end:]
            if not data:
                break


def serial_bridge(read_fd):
    """Broadcast raw console UART output as FRAME_SERIAL."""
    header = struct.pack("<I", FRAME_SERIAL)
    try:
        while data := os.read(read_fd, 4096):
            deadline = time.monotonic() + 0.01
            chunk = data
            while data and len(chunk) < 4096:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not select.select([read_fd], [], [], remaining)[0]:
                    break
                data = os.read(read_fd, 4096 - len(chunk))
                chunk += data
            frames.post_frame(header + chunk)
            if not data:
                return
    finally:
        os.close(read_fd)
