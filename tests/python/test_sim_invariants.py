# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Exercise the QEMU trace and profiler checker with synthetic wire streams."""

import importlib.util
import struct
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[2] / "config" / "scripts"
spec = importlib.util.spec_from_file_location("sim_invariants", SCRIPTS / "ove-sim-invariants.py")
checker = importlib.util.module_from_spec(spec)
with patch.object(sys, "path", [str(SCRIPTS), *sys.path]):
    spec.loader.exec_module(checker)

RUN, READY, BLOCKED = 0, 1, 2
STATE, MARK = 1, 2
TEXT = [(0x1000, 0x2000)]
NAMES = (b"producer.send", b"consumer.recv", b"graphics.frame", b"ui.update")


def thread_records(first_run_duration=0):
    records = []
    for ts in range(100_000, 2_100_000, 100_000):
        records.extend([
            (ts, 1, STATE, RUN, READY),
            (ts + 10_000, 2, STATE, RUN, READY),
            (ts + first_run_duration, 1, STATE, BLOCKED, RUN),
            (ts + 80_000, 2, STATE, BLOCKED, RUN),
            (ts + first_run_duration, 1, STATE, READY, BLOCKED),
            (ts + 90_000, 2, STATE, READY, BLOCKED),
        ])
    return records


def capture(records, *, version=3, names=NAMES, sample_step=1_000, pc=0x1001,
            dropped=0, trace_dropped=0):
    cap = checker.Capture()
    records = [*records, (10, 1, MARK, 0xF5, 1), (20, 1, MARK, 0xF6, 1)]
    trace = struct.pack("<BBHI", 1, version, len(records), trace_dropped)
    for ts, tid, kind, code, obj in records:
        if version >= 3:
            trace += struct.pack("<QIBBHII", ts, tid, kind, code, 0, obj, 0)
        else:
            trace += struct.pack("<QIBBH", ts, tid, kind, code, obj)
    cap.read_trace(trace)

    name_entries = b"".join(struct.pack("<HB", nid, len(name)) + name
                            for nid, name in enumerate(names, 1))
    cap.read_trace(struct.pack("<BBHI", 2, 3, len(names), 0) + name_entries)

    profile = struct.pack("<BBHI", 1, 4, 50, dropped)
    for n in range(50):
        profile += struct.pack("<QIBBH", n * sample_step, 1 + n % 2, int(pc is not None), 0, 0)
        if pc is not None:
            profile += struct.pack("<I", pc)
    cap.read_profiler(profile)
    return cap


class SimInvariantsTest(unittest.TestCase):
    def validate(self, cap, rtos="freertos"):
        return checker.validate(cap, TEXT, rtos)

    def assert_failure(self, cap, message, rtos="freertos"):
        errors = self.validate(cap, rtos)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn(message, errors[0])

    def test_same_timestamp_preserves_emission_order(self):
        self.assertEqual(self.validate(capture(thread_records())), [])

    def test_one_lost_or_garbled_record(self):
        records = thread_records()
        for case, index, replacement in (
            ("lost RUNNING", 6, []),
            ("lost BLOCKED", 8, []),
            ("garbled old state", 8, [(*records[8][:-1], BLOCKED)]),
        ):
            with self.subTest(case=case):
                cap = capture([*records[:index], *replacement, *records[index + 1:]])
                self.assert_failure(cap, "1 old_state discontinuities")

    def test_timestamp_step_back(self):
        records = thread_records()
        records[2] = (records[2][0] - 1, *records[2][1:])
        self.assert_failure(capture(records), "non-monotonic per-thread timestamps")

    def test_early_qemu_exit(self):
        cap = capture(thread_records())
        cap.qemu_exit = 0
        self.assert_failure(cap, "QEMU exited with status 0")

    def test_capture_waits_for_truncated_header(self):
        with TemporaryDirectory() as tmp, patch.object(checker.subprocess, "Popen") as popen:
            sim = Path(tmp) / "ove-sim"
            header = struct.pack("<I28xI", checker.shm.SIM_SHM_MAGIC, 1)
            qemu = popen.return_value.__enter__.return_value
            qemu.poll.return_value = qemu.returncode = None
            qemu.communicate.return_value = b"", b""
            with patch.object(checker, "SHM_SIZES", {sim: 0}), \
                    patch.object(checker.shm, "SIM_SHM_PATH", sim), \
                    patch.object(checker.time, "monotonic", side_effect=[0, 0, 0, 1]), \
                    patch.object(checker.time, "sleep", side_effect=lambda _: sim.write_bytes(header)):
                cap = checker.capture(Path("firmware.elf"), 1)
            self.assertEqual(cap.event_dropped, 1)

    def test_marks_alone_are_not_enough(self):
        self.assertIn("only 0 thread state records", self.validate(capture([])))

    def test_samples_without_a_pc(self):
        self.assert_failure(capture(thread_records(), pc=None), "50/50 profiler samples have no PC")

    def test_chain_check_is_freertos_only(self):
        records = thread_records()
        del records[6]
        cap = capture(records)
        self.assert_failure(cap, "1 old_state discontinuities")
        self.assertEqual(self.validate(cap, "nuttx"), [])
        self.assertEqual(self.validate(cap, "zephyr"), [])

    def test_double_running(self):
        cap = capture(thread_records(first_run_duration=80_000))
        self.assert_failure(cap, "double-RUNNING")
        self.assertEqual(self.validate(cap, "nuttx"), [])

    def test_other_invariants(self):
        for options, event_dropped, message in (
            ({"names": NAMES[:3]}, 0, "missing names: [4]"),
            ({"pc": 0x2001}, 0, "outside executable segments"),
            ({"sample_step": 100_000}, 0, "profiler sample rate"),
            ({"dropped": 6}, 0, "profiler dropped 6"),
            ({"version": 2}, 0, "trace wire version is 2"),
            ({"trace_dropped": 1}, 0, "trace dropped 1 records"),
            ({}, 1, "sim ring dropped 1 events"),
        ):
            with self.subTest(message=message):
                cap = capture(thread_records(), **options)
                cap.event_dropped = event_dropped
                self.assert_failure(cap, message)
