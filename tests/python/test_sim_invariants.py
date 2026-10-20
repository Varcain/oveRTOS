# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Tests for the QEMU invariant suite (config/scripts/ove-sim-invariants.py).

The suite is a standalone script, so it is imported from its path.  The
tests below drive it with synthetic trace streams - no QEMU, no network -
and pin down the old_state chain semantics:

  * the chain is checked per thread, in the order the guest emitted the
    records, so two transitions of one thread that carry the SAME
    timestamp (the guest's clock is clamped, not strictly monotonic) must
    not be reported as a chain break;
  * a genuine chain break (a lost or garbled record) is still reported;
  * the double-RUNNING check is still armed.
"""

import ast
import importlib.util
import os
import struct
import unittest
from pathlib import Path
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SUITE = os.environ.get(
    "OVE_SIM_INVARIANTS",
    os.path.join(ROOT, "config", "scripts", "ove-sim-invariants.py"),
)

# Suite state codes (config/scripts/ove-sim-invariants.py: STATE_NAMES)
RUN, RDY, BLK, SUS, TERM = 0, 1, 2, 3, 4
KIND_STATE, KIND_MARK = 1, 2
TID_A = 0x20005418
TID_B = 0x20004338


def load_suite(path=SUITE, name="ove_sim_invariants"):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# --- wire format (sim/include/ove/sim/ove_sim_trace.h, version 3) ------

def stream_payload(records, ver=3, dropped=0):
    out = bytearray()
    out += struct.pack("<BBHI", 1, ver, len(records), dropped)
    for ts, tid, kind, code, obj in records:
        out += struct.pack("<QIBBH", ts, tid, kind, code, obj & 0xFFFF)
        out += struct.pack("<I", obj & 0xFFFFFFFF)
        out += b"\0\0\0\0"  # sizeof(struct ove_trace_record) == 24
    return bytes(out)


def names_payload(names, ver=3):
    out = bytearray()
    out += struct.pack("<BBHI", 2, ver, len(names), 0)
    for nid, name in names:
        raw = name.encode()
        out += struct.pack("<HB", nid, len(raw)) + raw
    return bytes(out)


def profiler_payload(samples, word=4, ver=1):
    out = bytearray()
    out += struct.pack("<BBHI", ver, word, len(samples), 0)
    for ts, tid, depth, pcs in samples:
        out += struct.pack("<QIBBH", ts, tid, depth, 0, 0)
        for pc in pcs:
            out += struct.pack("<I" if word == 4 else "<Q", pc)
    return bytes(out)


# --- synthetic guest stream --------------------------------------------

def chain_records(tid, new_states, timestamps, first_old=RDY):
    """Records of one thread with an intact chain: old[i] == new[i-1],
    exactly how the guest builds them (old_state is the state the tracker
    replaced)."""
    out = []
    prev = first_old
    for ts, new in zip(timestamps, new_states):
        out.append((ts, tid, KIND_STATE, new, prev))
        prev = new
    return out


def legit_stream():
    """A legitimate guest-like stream.

    Thread A alternates RUN -> BLK -> RDY.  The RUN and the following BLK
    carry the SAME timestamp - the guest's clock is clamped, not strictly
    monotonic, so a transition that happens inside one tick repeats the
    previous timestamp.  Thread B runs in the gap in between, so the two
    threads never run at the same time.
    """
    a_states, a_ts = [], []
    b_states, b_ts = [], []
    t = 10_000
    for _ in range(10):
        a_states += [RUN, BLK, RDY]
        a_ts += [t, t, t + 500_000]  # RUN and BLK share a timestamp
        b_states += [RUN, BLK, RDY]
        b_ts += [t + 100, t + 400_000, t + 500_000]
        t += 500_000
    return chain_records(TID_A, a_states, a_ts) + chain_records(TID_B, b_states, b_ts)


def make_capture(mod, state_records):
    """A capture that satisfies every other invariant of the suite, so
    that the only possible complaint is about the state records."""
    cap = mod.Capture()
    recs = list(state_records)
    for i in range(60):
        recs.append((1000 + i * 100, TID_A, KIND_MARK, 0xF5, 0))  # zone begin
        recs.append((1050 + i * 100, TID_A, KIND_MARK, 0xF6, 0))  # zone end
    mod.parse_event(stream_payload(recs), cap)
    mod.parse_event(
        names_payload([(1, "idle"), (2, "main"), (3, "worker"), (4, "timer")]), cap
    )
    samples = []
    for i in range(2000):
        tid = TID_A if i % 2 == 0 else TID_B
        samples.append((1_000_000 + i * 1000, tid, 1, [0x8000000]))
    mod.parse_profiler(profiler_payload(samples[:1000]), cap)
    mod.parse_profiler(profiler_payload(samples[1000:]), cap)
    mod.parse_profiler(profiler_payload(samples[:10]), cap)
    return cap


def chain_breaks(ordered):
    """The suite's chain rule: the old_state of a record must equal the
    new_state of the previous record of the same thread."""
    prev = None
    breaks = []
    for i, (ts, _tid, _kind, code, old) in enumerate(ordered):
        if prev is not None and old != prev:
            breaks.append((i, ts, code, old, prev))
        prev = code
    return breaks


def per_thread(records):
    out = {}
    for rec in records:
        out.setdefault(rec[1], []).append(rec)
    return out


class SimInvariantsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_suite()
        cls.records = legit_stream()
        cls.cap = make_capture(cls.mod, cls.records)

    def validate(self, cap, strict=True):
        return self.mod.validate(
            cap, [(0x8000000, 0x8001000)], max_depth=32, min_samples=40,
            strict_running=strict,
        )

    @staticmethod
    def chain_errors(errors):
        return [e for e in errors if "discontinu" in e]

    # --- the regression: same-timestamp transitions ---------------------

    def test_same_timestamp_transitions_are_not_a_chain_break(self):
        """Two transitions of one thread with the same timestamp are
        legitimate: the guest's clock is clamped, so the second one
        repeats the timestamp of the first one.  The chain is intact in
        emission order and must not be reported as a break."""
        errors = self.validate(self.cap)
        self.assertEqual(
            self.chain_errors(errors), [],
            "same-timestamp transitions reported as a chain break",
        )
        # ... and no invented RUNNING period either.
        self.assertEqual(
            [e for e in errors if "double-RUNNING" in e], [],
            "same-timestamp transitions invented a RUNNING period",
        )

    def test_old_tie_break_would_break_the_chain(self):
        """Sensitivity check: the pre-fix suite pushed a RUNNING record
        behind every other record with the same timestamp, which breaks
        the chain of this very stream.  If this ever stops holding, the
        test above no longer pins the ordering fix down."""
        breaks = 0
        for recs in per_thread(self.records).values():
            ordered = sorted(recs, key=lambda r: (r[0], 1 if r[3] == 0 else 0))
            breaks += len(chain_breaks(ordered))
        self.assertGreater(breaks, 0, "the synthetic stream no longer exercises "
                                      "the equal-timestamp ordering")

    # --- the chain check must stay per thread --------------------------

    def test_chain_check_is_per_thread(self):
        """The chain is a per-thread property: the check has to run on a
        per-thread list (records of different threads interleave), the
        previous state has to be reset for every thread, and the records
        are followed in emission order."""
        with open(SUITE, encoding="utf-8") as f:
            tree = ast.parse(f.read())
        func = next(
            n for n in ast.walk(tree)
            if isinstance(n, ast.FunctionDef) and n.name == "validate"
        )
        per_thread_loops = [
            n for n in ast.walk(func)
            if isinstance(n, ast.For) and "by_tid.values()" in ast.unparse(n.iter)
        ]
        self.assertEqual(len(per_thread_loops), 1,
                         "no per-thread loop in validate()")
        loop = per_thread_loops[0]
        body = ast.unparse(loop.body)
        self.assertIn("old != prev", body,
                      "the chain check does not run inside the per-thread loop")
        self.assertIn("prev = None", body,
                      "the previous state is not reset per thread")
        self.assertNotIn(".sort(", body,
                         "the per-thread records are re-ordered: the chain "
                         "and the timestamp check must follow emission order")

    # --- a genuine break must still be caught ---------------------------

    def drop_one(self, code):
        """The stream with the first matching record of thread A removed."""
        recs = []
        dropped = False
        for r in self.records:
            if (not dropped and r[1] == TID_A and r[2] == KIND_STATE
                    and r[3] == code and r[0] > 2_000_000):
                dropped = True
                continue
            recs.append(r)
        self.assertTrue(dropped, "no record to drop")
        return recs

    def test_dropped_record_is_reported(self):
        errors = self.validate(make_capture(self.mod, self.drop_one(BLK)))
        self.assertEqual(len(self.chain_errors(errors)), 1)
        self.assertIn("1 old_state discontinuities", self.chain_errors(errors)[0])

    def test_dropped_run_record_is_reported(self):
        errors = self.validate(make_capture(self.mod, self.drop_one(RUN)))
        self.assertEqual(len(self.chain_errors(errors)), 1)
        self.assertIn("1 old_state discontinuities", self.chain_errors(errors)[0])

    def test_garbled_old_state_is_reported(self):
        recs = []
        done = False
        for r in self.records:
            if (not done and r[1] == TID_A and r[2] == KIND_STATE
                    and r[3] == BLK and r[0] > 2_000_000):
                r = (r[0], r[1], r[2], r[3], TERM)
                done = True
            recs.append(r)
        errors = self.validate(make_capture(self.mod, recs))
        self.assertEqual(len(self.chain_errors(errors)), 1)
        self.assertIn("1 old_state discontinuities", self.chain_errors(errors)[0])

    # --- the other checks must stay armed -------------------------------

    def test_double_running_is_still_reported(self):
        a = chain_records(TID_A, [RUN, BLK, RDY], [10_000, 400_000, 500_000])
        b = chain_records(TID_B, [RUN, BLK, RDY], [20_000, 400_000, 500_000])
        errors = self.validate(make_capture(self.mod, a + b))
        self.assertTrue(
            [e for e in errors if "double-RUNNING" in e],
            f"the double-RUNNING check did not fire: {errors}",
        )

    def test_legit_stream_does_not_trip_double_running(self):
        errors = self.validate(self.cap)
        self.assertEqual([e for e in errors if "double-RUNNING" in e], [])

    def test_timestamp_stepping_back_is_reported(self):
        """A thread's records must not go back in time in the order the
        guest pushed them; sorting them first would hide that."""
        recs = list(self.records)
        idx = next(i for i, r in enumerate(recs)
                   if r[1] == TID_A and r[0] > 2_000_000)
        ts, tid, kind, code, old = recs[idx]
        recs[idx] = (ts - 1_500_000, tid, kind, code, old)
        errors = self.validate(make_capture(self.mod, recs), strict=False)
        self.assertTrue([e for e in errors if "non-monotonic" in e], errors)

    def test_early_qemu_exit_is_reported(self):
        """QEMU that stops before the capture ends is a failure, even
        when the records it sent until then pass every other check."""
        cap = make_capture(self.mod, self.records)
        cap.qemu_exit = 1
        errors = self.validate(cap, strict=False)
        self.assertTrue([e for e in errors if "QEMU exited" in e], errors)
        self.assertEqual(self.validate(self.cap, strict=False), [])

    def test_exit_after_the_last_poll_is_recorded(self):
        """QEMU that stops between the loop's last poll and the deadline
        is seen only by the cleanup; it must still count as an exit."""
        mod = self.mod

        class FakeQemu:
            returncode = None

            def __init__(self, *args, **kwargs):
                self.polls = 0

            def poll(self):
                # Running for the loop's two polls, gone at the cleanup.
                self.polls += 1
                if self.polls > 2:
                    self.returncode = 1
                return self.returncode

            def terminate(self):
                raise AssertionError("an exited QEMU must not be terminated")

            def communicate(self, timeout=None):
                return b"", b""

        clock = iter(i * 0.4 for i in range(100))
        with mock.patch.object(mod.subprocess, "Popen", FakeQemu), \
                mock.patch.object(mod, "prep_shm", lambda: None), \
                mock.patch.object(mod.os, "open", mock.Mock(side_effect=OSError)), \
                mock.patch.object(mod.time, "monotonic", lambda: next(clock)), \
                mock.patch.object(mod.time, "sleep", lambda s: None), \
                mock.patch.object(mod.sys, "stderr", mock.Mock()):
            cap = mod.capture(Path("fw.elf"), 1.0)
        self.assertEqual(cap.qemu_exit, 1)

    def test_markers_alone_are_not_enough(self):
        """Names, zones and profiler samples without any thread state
        record mean the scheduler hooks are not reporting."""
        errors = self.validate(make_capture(self.mod, []), strict=False)
        self.assertTrue([e for e in errors if "state records" in e], errors)

    def test_samples_without_a_pc_are_reported(self):
        cap = make_capture(self.mod, self.records)
        cap.prof_pcs = []
        errors = self.validate(cap, strict=False)
        self.assertTrue([e for e in errors if "have no PC" in e], errors)

    def test_chain_check_is_freertos_only(self):
        """The chain is checked on FreeRTOS only."""
        recs = []
        done = False
        for r in self.records:
            if (not done and r[1] == TID_A and r[2] == KIND_STATE
                    and r[3] == BLK and r[0] > 2_000_000):
                r = (r[0], r[1], r[2], r[3], TERM)
                done = True
            recs.append(r)
        errors = self.validate(make_capture(self.mod, recs), strict=False)
        self.assertEqual(self.chain_errors(errors), [])


if __name__ == "__main__":
    unittest.main()
