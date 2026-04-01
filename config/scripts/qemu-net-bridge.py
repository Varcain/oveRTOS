#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""
QEMU shared-memory Ethernet bridge for oveRTOS.

Reads Ethernet frames from the TX ring in /dev/shm/ove-net (written by
the QEMU guest via semihosting) and forwards them to a TAP interface.
Incoming frames from the TAP are written to the RX ring for the guest.

Uses pread/pwrite for all shared-memory access (not mmap) to guarantee
coherency with QEMU's semihosting read()/write() syscalls.

The TAP interface must be created beforehand with qemu-net-setup.sh.

Usage:
    python3 qemu-net-bridge.py [--tap TAP_DEV] [--shm SHM_PATH]
"""

import argparse
import fcntl
import os
import select
import signal
import struct
import sys
import time

# ── Shared-memory protocol constants (must match qemu_net_shm.h) ────

NET_SHM_MAGIC = 0x4F564E54
NET_SHM_HDR_SIZE = 64
NET_SHM_RING_SIZE = 1 << 16  # 64 KB
NET_SHM_TX_RING_OFF = NET_SHM_HDR_SIZE
NET_SHM_RX_RING_OFF = NET_SHM_HDR_SIZE + NET_SHM_RING_SIZE
NET_SHM_TOTAL_SIZE = NET_SHM_HDR_SIZE + 2 * NET_SHM_RING_SIZE
NET_SHM_MTU = 1518

# Header field offsets
OFF_MAGIC = 0
OFF_MAC = 4
OFF_MTU = 10
OFF_RING_SIZE = 12
OFF_TX_WPOS = 16
OFF_TX_RPOS = 20
OFF_RX_WPOS = 24
OFF_RX_RPOS = 28
OFF_LINK_UP = 32

# ── TAP interface constants ──────────────────────────────────────────

TUNSETIFF = 0x400454CA
IFF_TAP = 0x0002
IFF_NO_PI = 0x1000

DEFAULT_TAP = "tap-ove"
DEFAULT_SHM = "/dev/shm/ove-net"

running = True


def signal_handler(sig, frame):
    global running
    running = False


def open_tap(dev_name):
    """Open a TAP device and return the file descriptor."""
    fd = os.open("/dev/net/tun", os.O_RDWR)
    ifr = struct.pack("16sH", dev_name.encode(), IFF_TAP | IFF_NO_PI)
    fcntl.ioctl(fd, TUNSETIFF, ifr)
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
    return fd


# ── pread/pwrite helpers for shared memory ───────────────────────────

def shm_read_u32(fd, offset):
    data = os.pread(fd, 4, offset)
    return struct.unpack("<I", data)[0] if len(data) == 4 else 0


def shm_write_u32(fd, offset, val):
    os.pwrite(fd, struct.pack("<I", val), offset)


def shm_write_u8(fd, offset, val):
    os.pwrite(fd, struct.pack("<B", val), offset)


def shm_ring_write(fd, ring_off, wpos, data):
    """Write data into a ring buffer via pwrite, handling wrap-around."""
    mask = NET_SHM_RING_SIZE - 1
    pos = wpos & mask
    first = NET_SHM_RING_SIZE - pos
    dlen = len(data)

    if first >= dlen:
        os.pwrite(fd, data, ring_off + pos)
    else:
        os.pwrite(fd, data[:first], ring_off + pos)
        os.pwrite(fd, data[first:], ring_off)

    return wpos + dlen


def shm_ring_read(fd, ring_off, rpos, length):
    """Read length bytes from a ring buffer via pread, handling wrap."""
    mask = NET_SHM_RING_SIZE - 1
    pos = rpos & mask
    first = NET_SHM_RING_SIZE - pos

    if first >= length:
        data = os.pread(fd, length, ring_off + pos)
    else:
        data = os.pread(fd, first, ring_off + pos)
        data += os.pread(fd, length - first, ring_off)

    return data, rpos + length


def main():
    global running

    parser = argparse.ArgumentParser(description="QEMU Ethernet bridge")
    parser.add_argument("--tap", default=DEFAULT_TAP, help="TAP device name")
    parser.add_argument("--shm", default=DEFAULT_SHM, help="SHM file path")
    args = parser.parse_args()

    signal.signal(signal.SIGTERM, signal_handler)
    signal.signal(signal.SIGINT, signal_handler)

    shm_path = args.shm

    # Wait for the SHM file to exist and have content
    while running:
        if os.path.exists(shm_path):
            sz = os.path.getsize(shm_path)
            if sz >= NET_SHM_TOTAL_SIZE:
                break
        time.sleep(0.05)

    if not running:
        return

    fd_shm = os.open(shm_path, os.O_RDWR)

    # Wait for guest to write the magic number
    print("[net-bridge] Waiting for guest...", flush=True)
    while running:
        magic = shm_read_u32(fd_shm, OFF_MAGIC)
        if magic == NET_SHM_MAGIC:
            break
        time.sleep(0.05)

    if not running:
        os.close(fd_shm)
        return

    mac = os.pread(fd_shm, 6, OFF_MAC)
    print(f"[net-bridge] Guest MAC: {mac.hex(':')}", flush=True)

    # Open TAP device
    try:
        tap_fd = open_tap(args.tap)
    except OSError as e:
        print(f"[net-bridge] Cannot open TAP '{args.tap}': {e}", flush=True)
        print("[net-bridge] Run 'sudo config/scripts/qemu-net-setup.sh' first",
              flush=True)
        os.close(fd_shm)
        sys.exit(1)

    print(f"[net-bridge] TAP '{args.tap}' opened", flush=True)

    # Signal link-up to the guest
    shm_write_u8(fd_shm, OFF_LINK_UP, 1)
    print("[net-bridge] Link up — bridging frames", flush=True)

    # Local copies of ring positions
    local_tx_rpos = 0
    local_rx_wpos = 0

    tx_frames = 0
    rx_frames = 0
    last_status = time.monotonic()

    while running:
        # ── TX ring → TAP (guest → host) ────────────────────────────
        tx_wpos = shm_read_u32(fd_shm, OFF_TX_WPOS)
        while local_tx_rpos != tx_wpos:
            avail = tx_wpos - local_tx_rpos
            if avail > NET_SHM_RING_SIZE:
                local_tx_rpos = tx_wpos
                break
            if avail < 2:
                break

            # Read frame length
            len_data, _ = shm_ring_read(fd_shm, NET_SHM_TX_RING_OFF,
                                        local_tx_rpos, 2)
            frame_len = struct.unpack("<H", len_data)[0]

            if frame_len == 0 or frame_len > NET_SHM_MTU:
                local_tx_rpos = tx_wpos
                break

            if avail < 2 + frame_len:
                break

            local_tx_rpos += 2

            # Read frame data
            frame_data, local_tx_rpos = shm_ring_read(
                fd_shm, NET_SHM_TX_RING_OFF, local_tx_rpos, frame_len)

            # Forward to TAP
            try:
                os.write(tap_fd, frame_data)
                tx_frames += 1
            except OSError:
                pass

        # Update read position
        shm_write_u32(fd_shm, OFF_TX_RPOS, local_tx_rpos)

        # ── TAP → RX ring (host → guest) ────────────────────────────
        try:
            ready, _, _ = select.select([tap_fd], [], [], 0.001)
        except (select.error, ValueError):
            break

        while ready:
            try:
                frame = os.read(tap_fd, NET_SHM_MTU)
            except OSError:
                break

            if not frame or len(frame) > NET_SHM_MTU:
                break

            rx_rpos = shm_read_u32(fd_shm, OFF_RX_RPOS)
            used = local_rx_wpos - rx_rpos
            needed = 2 + len(frame)

            if used + needed > NET_SHM_RING_SIZE:
                break  # Ring full — drop frame

            # Write [length][data] to RX ring
            len_prefix = struct.pack("<H", len(frame))
            local_rx_wpos = shm_ring_write(
                fd_shm, NET_SHM_RX_RING_OFF, local_rx_wpos, len_prefix)
            local_rx_wpos = shm_ring_write(
                fd_shm, NET_SHM_RX_RING_OFF, local_rx_wpos, frame)
            shm_write_u32(fd_shm, OFF_RX_WPOS, local_rx_wpos)
            rx_frames += 1

            # Check for more TAP frames (non-blocking)
            try:
                ready, _, _ = select.select([tap_fd], [], [], 0)
            except (select.error, ValueError):
                ready = []

        # Periodic status
        now = time.monotonic()
        if now - last_status >= 5.0:
            print(f"[net-bridge] TX: {tx_frames}  RX: {rx_frames}",
                  flush=True)
            last_status = now

    print(f"\n[net-bridge] Shutting down (TX: {tx_frames}, RX: {rx_frames})",
          flush=True)
    shm_write_u8(fd_shm, OFF_LINK_UP, 0)
    os.close(fd_shm)
    os.close(tap_fd)


if __name__ == "__main__":
    main()
