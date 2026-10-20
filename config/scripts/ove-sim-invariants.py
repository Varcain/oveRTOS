#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Check the trace and profiler streams from an AN500 example firmware.

Run only with the QEMU dashboard closed because this uses and removes ove run's /dev/shm/ove-* files.
"""

import argparse
import math
import os
import signal
import struct
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

from elftools.elf.constants import P_FLAGS
from elftools.elf.elffile import ELFFile
from ove_dashboard import shm

SHM_SIZES = {shm.SIM_SHM_PATH: shm.SIM_SHM_TOTAL_SIZE, shm.FB_PATH: 1 << 20,
             shm.AUDIO_PATH: shm.AUDIO_SHM_TOTAL, shm.INPUT_PATH: shm.INPUT_SHM_SIZE}
MIN_SAMPLES = 40
MAX_DEPTH = 32


class Capture:
    def __init__(self):
        self.trace = []  # timestamp, tid, kind, code, object
        self.names = set()
        self.trace_version = None
        self.trace_dropped = 0
        self.samples = []  # timestamp, tid, depth, leaf PC
        self.prof_dropped = 0
        self.event_dropped = 0
        self.qemu_exit = None

    def read_trace(self, payload):
        subtype, version, count, dropped = struct.unpack_from("<BBHI", payload)
        offset = 8
        if subtype == 2:  # NAMES
            for _ in range(count):
                name_id, size = struct.unpack_from("<HB", payload, offset)
                offset += 3 + size
                if offset <= len(payload):
                    self.names.add(name_id)
        elif subtype == 1:  # STREAM
            if self.trace_version is None or version < self.trace_version:
                self.trace_version = version
            self.trace_dropped += dropped
            for _ in range(count):
                ts, tid, kind, code, obj = struct.unpack_from("<QIBBH", payload, offset)
                if version >= 3:
                    obj, = struct.unpack_from("<I4x", payload, offset + 16)
                self.trace.append((ts, tid, kind, code, obj))
                offset += 24 if version >= 3 else 16

    def read_profiler(self, payload):
        _, word, count, dropped = struct.unpack_from("<BBHI", payload)
        self.prof_dropped += dropped
        pc_format = {4: "<I", 8: "<Q"}[word]
        offset = 8
        for _ in range(count):
            ts, tid, depth = struct.unpack_from("<QIB3x", payload, offset)
            offset += 16
            pc = struct.unpack_from(pc_format, payload, offset)[0] if depth else None
            self.samples.append((ts, tid, depth, pc))
            offset += depth * word
        if offset > len(payload):
            raise ValueError("truncated profiler sample")


def prep_shm():
    for path, size in SHM_SIZES.items():
        with open(path, "wb") as stream:
            if path == shm.INPUT_PATH:
                stream.write(struct.pack("<I", shm.INPUT_MAGIC))
            stream.truncate(size)


def read_ring(fd, pos, size):
    offset = pos % shm.SIM_SHM_RING_SIZE
    first = min(size, shm.SIM_SHM_RING_SIZE - offset)
    data = os.pread(fd, first, shm.SIM_SHM_EVENT_OFF + offset)
    if first < size:
        data += os.pread(fd, size - first, shm.SIM_SHM_EVENT_OFF)
    return data


def capture(elf, seconds):
    cap = Capture()
    command = ["qemu-system-arm", "-M", "mps2-an500", "-display", "none",
               "-monitor", "none", "-serial", "null", "-kernel", str(elf),
               "-semihosting-config", f"enable=on,target=native,arg={shm.FB_PATH}"]
    try:
        prep_shm()
        with open(shm.SIM_SHM_PATH, "r+b") as sim, subprocess.Popen(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        ) as qemu:
            fd, rpos = sim.fileno(), 0
            deadline = time.monotonic() + seconds
            try:
                while time.monotonic() < deadline and qemu.poll() is None:
                    header = os.pread(fd, 20, 0)
                    if len(header) < 20:  # The guest truncates the file on open.
                        time.sleep(0.01)
                        continue
                    magic, wpos = struct.unpack("<I12xI", header)
                    if magic != shm.SIM_SHM_MAGIC or wpos == rpos:
                        time.sleep(0.01)
                        continue
                    size, = struct.unpack("<H", read_ring(fd, rpos, 2))
                    event = read_ring(fd, rpos + 2, size)
                    rpos = (rpos + 2 + size) & 0xFFFFFFFF
                    os.pwrite(fd, struct.pack("<I", rpos), 20)
                    _, event_type, _, data_len = struct.unpack_from("<IIII", event)
                    payload = event[shm.SIM_EVENT_HDR_SIZE:shm.SIM_EVENT_HDR_SIZE + data_len]
                    if event_type in (shm.SIM_TRACE_EVT_STREAM, shm.SIM_TRACE_EVT_DESCRIPTORS):
                        cap.read_trace(payload)
                    elif event_type == shm.SIM_PROFILER_EVT_SAMPLES:
                        cap.read_profiler(payload)
                cap.qemu_exit = qemu.poll()
                dropped = os.pread(fd, 4, shm.SIM_EVENT_DROPPED_OFF)
                if len(dropped) == 4:
                    cap.event_dropped, = struct.unpack("<I", dropped)
            finally:
                if qemu.returncode is None:
                    qemu.terminate()
                try:
                    _, stderr = qemu.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    qemu.kill()
                    _, stderr = qemu.communicate()
                if cap.qemu_exit:
                    sys.stderr.write(stderr.decode(errors="replace"))
    finally:
        for path in SHM_SIZES:
            Path(path).unlink(missing_ok=True)
    return cap


def validate(cap, ranges, rtos):
    errors = []

    def check(condition, message):
        if not condition:
            errors.append(message)

    check(cap.qemu_exit is None, f"QEMU exited with status {cap.qemu_exit} during capture")
    check((cap.trace_version or 0) >= 3, f"trace wire version is {cap.trace_version}")
    check(cap.trace_dropped == 0, f"trace dropped {cap.trace_dropped} records")
    check(cap.event_dropped == 0, f"sim ring dropped {cap.event_dropped} events")
    states = [record for record in cap.trace if record[2] == 1]
    by_tid = defaultdict(list)
    for record in states:
        by_tid[record[1]].append(record)
    check(len(states) >= 50, f"only {len(states)} thread state records")
    check(len(by_tid) >= 2, f"state records from {len(by_tid)} thread(s)")

    points = []
    discontinuities = backwards = 0
    for records in by_tid.values():
        start = prev = last_ts = None
        # Equal timestamps are valid. Follow the transitions in emission order.
        for ts, _, _, code, old in records:
            backwards += last_ts is not None and ts < last_ts
            discontinuities += prev is not None and old != prev
            prev, last_ts = code, ts
            if code == 0:  # RUNNING
                if start is None:
                    start = ts
            elif start is not None:
                if ts > start:
                    points.extend(((start, 1), (ts, -1)))
                start = None
        if start is not None and last_ts > start:
            points.extend(((start, 1), (last_ts, -1)))
    check(backwards == 0, f"{backwards} non-monotonic per-thread timestamps")
    if rtos == "freertos":
        check(discontinuities == 0, f"{discontinuities} old_state discontinuities")
        if points:
            overlap = running = previous = 0
            for ts, delta in sorted(points):
                if running > 1:
                    overlap += ts - previous
                previous, running = ts, running + delta
            span = max(states)[0] - min(states)[0]
            check(overlap * 100 <= span, f"{overlap / 1000:.1f} ms double-RUNNING (max 1%)")

    missing = {1, 2, 3, 4} - cap.names
    check(not missing, f"missing names: {sorted(missing)}")
    marks = [code for _, _, kind, code, _ in cap.trace if kind == 2]
    begins, ends = marks.count(0xF5), marks.count(0xF6)
    check(begins > 0, "no user zone begin markers")
    check(abs(begins - ends) <= 4, f"unbalanced zones: {begins} begin, {ends} end")

    count = len(cap.samples)
    times = {ts for ts, _, _, _ in cap.samples}
    span = max(times) - min(times) if times else 0
    rate = len(times) * 1e6 / span if span else 0
    depth = max((depth for _, _, depth, _ in cap.samples), default=0)
    pcs = [pc & ~1 for _, _, _, pc in cap.samples if pc is not None]
    bad = sum(not any(lo <= pc < hi for lo, hi in ranges) for pc in pcs)
    check(count >= MIN_SAMPLES, f"only {count} profiler samples")
    check(len({tid for _, tid, _, _ in cap.samples}) >= 2, "profiler sampled fewer than two threads")
    check(rate >= 20, f"profiler sample rate {rate:.1f}/s")
    check(cap.prof_dropped * 10 <= count, f"profiler dropped {cap.prof_dropped} of {count} samples")
    check(depth <= MAX_DEPTH, f"profiler stack depth {depth} exceeds {MAX_DEPTH}")
    check(len(pcs) == count, f"{count - len(pcs)}/{count} profiler samples have no PC")
    check(bad == 0, f"{bad}/{len(pcs)} sample PCs outside executable segments")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--rtos", required=True, choices=("freertos", "nuttx", "zephyr"))
    parser.add_argument("--seconds", type=float, default=5)
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0:
        parser.error("--seconds must be positive and finite")
    with args.elf.open("rb") as elf:
        ranges = [(seg["p_vaddr"], seg["p_vaddr"] + seg["p_memsz"])
                  for seg in ELFFile(elf).iter_segments(type="PT_LOAD") if seg["p_flags"] & P_FLAGS.PF_X]

    signal.signal(signal.SIGTERM, signal.default_int_handler)
    cap = capture(args.elf, args.seconds)
    errors = validate(cap, ranges, args.rtos)
    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"sim invariants: {len(cap.trace)} trace records (v{cap.trace_version}, "
          f"{cap.trace_dropped} dropped), {len(cap.names)} names, "
          f"{len(cap.samples)} profiler samples ({cap.prof_dropped} dropped), "
          f"{cap.event_dropped} event drops: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
