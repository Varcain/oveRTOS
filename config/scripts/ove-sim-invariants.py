#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""
Host-side trace/profiler invariant checks for a sim firmware.

Boots the ELF under QEMU (mps2-an500), drains the /dev/shm/ove-sim event
ring like the dashboard bridge does, and asserts the properties the
dashboard relies on:

  * QEMU runs for the whole capture
  * trace records arrive, wire version >= 3, nothing dropped, with state
    records from at least two threads
  * each thread's timestamps never step back in the order the guest
    pushed its records
  * the example app's NAMES table arrives and zones begin/end
  * profiler samples arrive at a sane rate, respect the depth cap, each
    carry a PC and point into executable ELF segments
  * the ring never overruns / resyncs

On FreeRTOS the suite also asserts an intact old_state chain (every
record's old state is the state the previous one set) and at most 1%
double-RUNNING time.  The chain is checked in the order the guest
emitted the records: the guest's clock is clamped, not strictly
monotonic, so two transitions of one thread can carry the same timestamp
and must not be re-ordered.  NuttX and Zephyr are not held to the chain
and double-RUNNING checks.

Usage:
  ove-sim-invariants.py --elf <firmware.elf> --rtos freertos [--seconds 5]
"""

import argparse
import os
import struct
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

SIM = "/dev/shm/ove-sim"
FB = "/dev/shm/ove-fb"
AUDIO = "/dev/shm/ove-audio"
INPUT = "/dev/shm/ove-input"

RING = 1 << 16
EVT_OFF = 64
EVT_HDR = 16

TRACE_EVT = 10  # OVE_SIM_EVT_BASE_TRACE (+1 for descriptors/names)
PROFILER_EVT = 20

STATE_NAMES = {0: "RUN", 1: "RDY", 2: "BLK", 3: "SUS", 4: "TERM"}

# The trace ring is multi-producer: independent threads can enqueue their
# state records in an order that does not match the timestamps.  Validate
# reconstructed per-thread timelines instead of raw ring order, and allow
# a small double-RUNNING budget to absorb switch-edge races.
OVERLAP_MAX_PCT = 1.0


def prep_shm() -> None:
    for path, size in ((FB, 1 << 20), (AUDIO, 131136), (SIM, 131136)):
        with open(path, "wb") as f:
            f.truncate(size)
    with open(INPUT, "wb") as f:
        f.write(b"INPT")
        f.truncate(16)


def exec_ranges(elf: Path) -> list[tuple[int, int]]:
    """PT_LOAD + PF_X virtual address ranges from a 32/64-bit ELF."""
    data = elf.read_bytes()
    if data[:4] != b"\x7fELF":
        raise ValueError(f"{elf} is not an ELF file")
    is64 = data[4] == 2
    endian = "<" if data[5] == 1 else ">"
    if is64:
        phoff = struct.unpack_from(endian + "Q", data, 32)[0]
        phentsize = struct.unpack_from(endian + "H", data, 54)[0]
        phnum = struct.unpack_from(endian + "H", data, 56)[0]
        fmt, size = endian + "IIQQQQQQ", 56
    else:
        phoff = struct.unpack_from(endian + "I", data, 28)[0]
        phentsize = struct.unpack_from(endian + "H", data, 42)[0]
        phnum = struct.unpack_from(endian + "H", data, 44)[0]
        fmt, size = endian + "IIIIIIII", 32

    ranges = []
    for i in range(phnum):
        off = phoff + i * phentsize
        if off + size > len(data):
            break
        fields = struct.unpack_from(fmt, data, off)
        if is64:
            p_type, p_flags, _p_off, p_vaddr, _p_paddr, _p_filesz, p_memsz, _align = (
                fields
            )
        else:
            p_type, _p_off, p_vaddr, _p_paddr, _p_filesz, p_memsz, p_flags, _align = (
                fields
            )
        if p_type == 1 and (p_flags & 1) and p_memsz:
            ranges.append((p_vaddr, p_vaddr + p_memsz))
    return ranges


class Capture:
    def __init__(self) -> None:
        self.trace = []  # (ts, tid, kind, code, obj)
        self.names = {}
        self.trace_batches = 0
        self.trace_version = 0
        self.trace_dropped = 0
        self.prof_samples = 0
        self.prof_dropped = 0
        self.prof_pcs = []
        self.prof_depths = []
        self.prof_ts = []
        self.prof_tids = set()
        self.prof_events = 0
        self.resyncs = 0
        self.oversize = 0
        # Exit status of a QEMU that stopped before the capture ended.
        self.qemu_exit = None


def parse_event(pl: bytes, cap: Capture) -> None:
    if len(pl) < 8:
        return
    sub, ver, count, dropped = struct.unpack_from("<BBHI", pl, 0)
    if sub == 0:  # thread descriptors; not needed here
        return
    if sub == 2:  # NAMES
        p = 8
        for _ in range(count):
            if p + 3 > len(pl):
                break
            nid, nlen = struct.unpack_from("<HB", pl, p)
            p += 3
            if p + nlen > len(pl):
                break
            cap.names[nid] = pl[p : p + nlen].decode(errors="replace")
            p += nlen
        return

    cap.trace_batches += 1
    cap.trace_version = max(cap.trace_version, ver)
    cap.trace_dropped += dropped
    rec = 24 if ver >= 3 else 16
    p = 8
    for _ in range(count):
        if p + rec > len(pl):
            break
        ts, tid, kind, code, reserved = struct.unpack_from("<QIBBH", pl, p)
        obj = struct.unpack_from("<I", pl, p + 16)[0] if rec == 24 else reserved
        cap.trace.append((ts, tid, kind, code, obj))
        p += rec


def parse_profiler(pl: bytes, cap: Capture) -> None:
    if len(pl) < 8:
        return
    _ver, word, count, dropped = struct.unpack_from("<BBHI", pl, 0)
    cap.prof_dropped += dropped
    cap.prof_events += 1
    p = 8
    for _ in range(count):
        if p + 16 > len(pl):
            break
        ts, tid, depth, _state, _isr = struct.unpack_from("<QIBBH", pl, p)
        if p + 16 + depth * word > len(pl):
            break
        cap.prof_samples += 1
        cap.prof_ts.append(ts)
        cap.prof_tids.add(tid)
        cap.prof_depths.append(depth)
        if depth:
            cap.prof_pcs.append(
                struct.unpack_from("<I" if word == 4 else "<Q", pl, p + 16)[0]
            )
        p += 16 + depth * word


def capture(elf: Path, seconds: float) -> Capture:
    prep_shm()
    qemu = subprocess.Popen(
        [
            "qemu-system-arm",
            "-M",
            "mps2-an500",
            "-display",
            "none",
            "-monitor",
            "none",
            "-serial",
            "null",
            "-kernel",
            str(elf),
            "-semihosting-config",
            f"enable=on,target=native,arg={FB}",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    cap = Capture()
    fd = None
    rpos = 0
    deadline = time.monotonic() + seconds
    try:
        while time.monotonic() < deadline:
            if qemu.poll() is not None:
                cap.qemu_exit = qemu.returncode
                break
            if fd is None:
                try:
                    fd = os.open(SIM, os.O_RDWR)
                except OSError:
                    time.sleep(0.05)
                    continue

            hdr = os.pread(fd, 24, 0)
            if len(hdr) < 24 or struct.unpack_from("<I", hdr, 0)[0] == 0:
                time.sleep(0.05)
                continue
            wpos = struct.unpack_from("<I", hdr, 16)[0]
            avail = (wpos - rpos) & 0xFFFFFFFF
            if avail > RING:
                cap.resyncs += 1
                rpos = wpos
                continue
            if avail < 2:
                time.sleep(0.01)
                continue

            lo = os.pread(fd, 1, EVT_OFF + (rpos & (RING - 1)))[0]
            hi = os.pread(fd, 1, EVT_OFF + ((rpos + 1) & (RING - 1)))[0]
            length = lo | (hi << 8)
            rpos += 2
            off = rpos & (RING - 1)
            first = min(RING - off, length)
            buf = os.pread(fd, first, EVT_OFF + off)
            if first < length:
                buf += os.pread(fd, length - first, EVT_OFF)
            rpos += length
            os.pwrite(fd, struct.pack("<I", rpos & 0xFFFFFFFF), 20)

            if len(buf) < EVT_HDR:
                continue
            _pid, event_type, _ts, data_len = struct.unpack_from("<IIII", buf, 0)
            if length > 4096:
                cap.oversize += 1
                continue
            payload = buf[EVT_HDR : EVT_HDR + data_len]
            if event_type in (TRACE_EVT, TRACE_EVT + 1):
                parse_event(payload, cap)
            elif event_type == PROFILER_EVT:
                parse_profiler(payload, cap)
    finally:
        if qemu.poll() is None:
            qemu.terminate()
        elif cap.qemu_exit is None:
            # It stopped after the last poll of the loop (for instance
            # during its final sleep): that is an early exit too.
            cap.qemu_exit = qemu.returncode
        try:
            _out, err = qemu.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()
            _out, err = qemu.communicate()
        if fd is not None:
            os.close(fd)
        if qemu.returncode and qemu.returncode > 0:
            sys.stderr.write(err.decode(errors="replace")[-2000:])

    return cap


def validate(
    cap: Capture,
    ranges: list[tuple[int, int]],
    max_depth: int,
    min_samples: int,
    strict_running: bool,
) -> list[str]:
    errors = []

    def check(cond: bool, msg: str) -> None:
        if not cond:
            errors.append(msg)

    states = [r for r in cap.trace if r[2] == 1]
    marks = [r for r in cap.trace if r[2] == 2]
    check(len(cap.trace) > 100, f"only {len(cap.trace)} trace records")
    check(cap.trace_version >= 3, f"trace wire version is {cap.trace_version}")
    check(cap.trace_dropped == 0, f"trace dropped {cap.trace_dropped} records")
    check(cap.resyncs == 0, f"sim ring resynced {cap.resyncs} times")
    check(cap.oversize == 0, f"{cap.oversize} oversize events")
    check(cap.qemu_exit is None,
          f"QEMU exited with status {cap.qemu_exit} before the capture ended")
    # The markers and zones alone could pass the record count: the
    # scheduler hooks must report state changes of several threads too.
    check(len(states) >= 50, f"only {len(states)} thread state records")
    state_tids = {r[1] for r in states}
    check(len(state_tids) >= 2, f"state records from {len(state_tids)} thread(s)")

    # Reconstruct each thread's timeline independently, in ring order: the
    # ring keeps the order the guest pushed the records in, and a thread's
    # records must already be in timestamp order there.
    by_tid = defaultdict(list)
    for rec in states:
        by_tid[rec[1]].append(rec)

    nonmono = 0
    discontinued = 0
    intervals = []
    for recs in by_tid.values():
        # Follow the records in the order the guest pushed them.  The
        # guest's clock is clamped, not strictly monotonic
        # (backends/freertos/include/ove_freertos_systick.h), so two
        # transitions of one thread can carry the same timestamp - any
        # tie-break that re-orders them breaks the old_state chain and
        # invents RUNNING periods the thread never had.
        start = None
        prev = None
        last_ts = None
        for ts, _tid, _kind, code, old in recs:
            if last_ts is not None and ts < last_ts:
                nonmono += 1
            last_ts = ts
            if prev is not None and old != prev:
                discontinued += 1
            prev = code
            if code == 0:
                if start is None:
                    start = ts
            elif start is not None:
                if ts > start:
                    intervals.append((start, ts))
                start = None
        if start is not None:
            intervals.append((start, last_ts))

    check(nonmono == 0, f"{nonmono} non-monotonic per-thread timestamps")
    if strict_running:
        # The backend emits old_state = the state the tracker replaced, so
        # the chain is exact by construction in the guest's emission order:
        # a discontinuity means a record was lost or garbled - which the
        # suite also reports as a dropped record or a ring resync.  No
        # budget on purpose: a budget would mask exactly the loss this
        # check exists to catch.
        check(discontinued == 0, f"{discontinued} old_state discontinuities")

    ts_values = [r[0] for r in states]
    span = max(ts_values) - min(ts_values) if ts_values else 0
    if span > 0 and intervals:
        points = []
        for iv_start, iv_end in intervals:
            points.append((iv_start, 1))
            points.append((iv_end, -1))
        points.sort(key=lambda p: (p[0], p[1]))  # ends before starts on ties
        running = 0
        prev_ts = points[0][0]
        overlap = 0
        for ts, delta in points:
            if running > 1:
                overlap += ts - prev_ts
            prev_ts = ts
            running += delta
        if strict_running:
            max_overlap = int(span * OVERLAP_MAX_PCT / 100)
            check(
                overlap <= max_overlap,
                f"{overlap / 1000:.1f} ms double-RUNNING "
                f"(max {max_overlap / 1000:.1f} ms)",
            )

    check({1, 2, 3, 4}.issubset(cap.names), f"missing names: {cap.names}")
    begins = sum(1 for r in marks if r[3] == 0xF5)
    ends = sum(1 for r in marks if r[3] == 0xF6)
    check(begins > 0, "no user zone begin markers")
    check(ends >= begins - 4, f"unbalanced zones: {begins} begin, {ends} end")

    check(cap.prof_samples >= min_samples, f"only {cap.prof_samples} profiler samples")
    check(cap.prof_events >= 3, f"only {cap.prof_events} profiler batches")
    check(
        cap.prof_dropped <= cap.prof_samples // 10,
        f"profiler dropped {cap.prof_dropped} of {cap.prof_samples} samples",
    )
    if cap.prof_depths:
        check(max(cap.prof_depths) <= max_depth, f"stack depth {max(cap.prof_depths)}")
    check(len(cap.prof_tids) >= 2, "profiler attributed samples to <2 threads")
    if cap.prof_ts:
        distinct = sorted(set(cap.prof_ts))
        span = (distinct[-1] - distinct[0]) / 1e6
        rate = len(distinct) / span if span > 0 else 0
        check(rate >= 20, f"profiler sample rate {rate:.1f}/s")

    def interactive(pc: int) -> bool:
        pc &= ~1
        return any(lo <= pc < hi for lo, hi in ranges)

    # Every sample carries at least the interrupted PC.
    empty = cap.prof_samples - len(cap.prof_pcs)
    check(empty == 0, f"{empty}/{cap.prof_samples} profiler samples have no PC")
    bad = [pc for pc in cap.prof_pcs if not interactive(pc)]
    check(not bad, f"{len(bad)}/{len(cap.prof_pcs)} sample PCs outside .text")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument(
        "--rtos", required=True, choices=("freertos", "nuttx", "zephyr")
    )
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--max-depth", type=int, default=32)
    parser.add_argument("--min-samples", type=int, default=40)
    args = parser.parse_args()

    try:
        ranges = exec_ranges(args.elf)
    except (OSError, ValueError) as exc:
        sys.exit(f"ove-sim-invariants: {exc}")

    cap = capture(args.elf, args.seconds)
    errors = validate(
        cap,
        ranges,
        args.max_depth,
        args.min_samples,
        strict_running=args.rtos == "freertos",
    )

    print(
        f"sim invariants: {len(cap.trace)} trace records ({cap.trace_batches} batches, "
        f"v{cap.trace_version}, {cap.trace_dropped} dropped), "
        f"{len(cap.names)} names, {cap.prof_samples} profiler samples "
        f"({cap.prof_dropped} dropped, max depth "
        f"{max(cap.prof_depths) if cap.prof_depths else 0})"
    )
    if errors:
        for msg in errors:
            print(f"FAIL: {msg}", file=sys.stderr)
        return 1
    print("sim invariants: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
