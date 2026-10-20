# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Tests for the dashboard bridge's trace-descriptor decoder
(config/scripts/ove-dashboard-bridge.py::_trace_dump_descriptors).

The guest emits a descriptor batch whose envelope version byte selects the
record layout (see sim/include/ove/sim/ove_sim_trace.h):

  v1  : tid(4) | name_len(1) | name
  v2+ : tid(4) | generation(4) | name_len(1) | name

Decoding a v2+ batch with the v1 layout silently yields empty/garbage names
(the name_len byte is read from the low byte of the generation word) and
desynchronises the record loop, so every entry has to be recovered exactly.
"""

import contextlib
import importlib.util
import io
import os
import re
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BRIDGE = os.path.join(ROOT, "config", "scripts", "ove-dashboard-bridge.py")

# "[dump-desc] tid=0x1000 name=alpha gen=0x0"
LINE_RE = re.compile(
    r"^\[dump-desc\] tid=0x([0-9a-fA-F]+) name=(.*?)(?: gen=0x([0-9a-fA-F]+))?$")
WARN_RE = re.compile(r"^\[dump-desc\] trace version (\d+) ")


def load_bridge():
    spec = importlib.util.spec_from_file_location("ove_dashboard_bridge", BRIDGE)
    mod = importlib.util.module_from_spec(spec)
    # Keep the repo clean: the bridge lives outside a package, so importing
    # it would drop a __pycache__ entry next to it.
    saved = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.dont_write_bytecode = saved
    return mod


def envelope(version, records, count=None):
    """Build a FRAME_TRACE descriptor batch (sub_type 0)."""
    n = len(records) if count is None else count
    return struct.pack("<BBHI", 0, version, n, 0) + b"".join(records)


def desc_v1(tid, name):
    """v1 descriptor record: tid(4) | name_len(1) | name."""
    raw = name.encode()
    return struct.pack("<IB", tid, len(raw)) + raw


def desc_v2(tid, generation, name):
    """v2+ descriptor record: tid(4) | generation(4) | name_len(1) | name."""
    raw = name.encode()
    return struct.pack("<IIB", tid, generation, len(raw)) + raw


class DescriptorDecodeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.b = load_bridge()

    def setUp(self):
        # The bridge warns once per unknown trace version; reset the
        # bookkeeping so every test starts from a clean slate.
        warned = getattr(self.b, "_trace_warned_versions", None)
        if warned is not None:
            warned.clear()

    def dump(self, payload):
        """Run the decoder and return (lines, parsed tid/name/gen tuples)."""
        buf = io.StringIO()
        with contextlib.redirect_stderr(buf):
            self.b._trace_dump_descriptors(payload)
        lines = buf.getvalue().splitlines()
        parsed = []
        for line in lines:
            m = LINE_RE.match(line)
            if m:
                parsed.append((int(m.group(1), 16), m.group(2),
                               None if m.group(3) is None else int(m.group(3), 16)))
        return lines, parsed

    def test_v2_layout_recovers_tid_and_name(self):
        # The guest's current layout (v2+, generation word present).
        payload = envelope(2, [
            desc_v2(0x1000, 0x00000000, "alpha"),
            desc_v2(0x2000, 0x00000001, "beta"),
            desc_v2(0x3000, 0x01020304, "gamma"),
        ])
        _lines, parsed = self.dump(payload)
        self.assertEqual(
            [(tid, name) for (tid, name, _gen) in parsed],
            [(0x1000, "alpha"), (0x2000, "beta"), (0x3000, "gamma")])

    def test_v2_layout_reports_the_generation(self):
        payload = envelope(2, [
            desc_v2(0x1000, 0x00000005, "alpha"),
            desc_v2(0x2000, 0x01020304, "beta"),
        ])
        _lines, parsed = self.dump(payload)
        self.assertEqual([gen for (_t, _n, gen) in parsed], [0x5, 0x01020304])

    def test_v1_layout_recovers_tid_and_name(self):
        # An older guest (v1) sends no generation word.
        payload = envelope(1, [
            desc_v1(0x1000, "alpha"),
            desc_v1(0x2000, "beta"),
            desc_v1(0x3000, "gamma"),
        ])
        _lines, parsed = self.dump(payload)
        self.assertEqual(
            [(tid, name) for (tid, name, _gen) in parsed],
            [(0x1000, "alpha"), (0x2000, "beta"), (0x3000, "gamma")])
        # The v1 layout has no generation word to report.
        self.assertEqual([gen for (_t, _n, gen) in parsed], [None, None, None])

    def test_v2_and_v1_payloads_differ_only_in_the_version_byte(self):
        # The layout is taken from the version byte, not guessed.
        v2 = envelope(2, [desc_v2(0x1000, 0x00000000, "alpha")])
        v1 = envelope(1, [desc_v1(0x1000, "alpha")])
        _l2, parsed2 = self.dump(v2)
        _l1, parsed1 = self.dump(v1)
        self.assertEqual([(t, n) for (t, n, _g) in parsed2], [(0x1000, "alpha")])
        self.assertEqual([(t, n) for (t, n, _g) in parsed1], [(0x1000, "alpha")])

    def test_truncated_v2_payload_is_dropped(self):
        # The record header is complete but the name bytes are missing:
        # the decoder must bail out instead of emitting a partial entry.
        full = envelope(2, [desc_v2(0x1000, 0x00000000, "alpha")])
        truncated = full[:-len("alpha")]
        lines, parsed = self.dump(truncated)
        self.assertEqual(parsed, [])
        self.assertEqual([ln for ln in lines if ln.startswith("[dump-desc] tid=")], [])

    def test_truncated_v1_payload_is_dropped(self):
        full = envelope(1, [desc_v1(0x1000, "alpha")])
        truncated = full[:-len("alpha")]
        lines, parsed = self.dump(truncated)
        self.assertEqual(parsed, [])
        self.assertEqual([ln for ln in lines if ln.startswith("[dump-desc] tid=")], [])

    def test_truncated_record_header_is_dropped(self):
        # One byte short of the fixed record header.
        payload = struct.pack("<BBHI", 0, 2, 1, 0) + struct.pack("<II", 0x1000, 0)
        lines, parsed = self.dump(payload)
        self.assertEqual(parsed, [])
        self.assertEqual([ln for ln in lines if ln.startswith("[dump-desc] tid=")], [])

    def test_count_larger_than_the_payload_is_bounded(self):
        # A lying count must not make the decoder walk past the payload.
        payload = envelope(2, [desc_v2(0x1000, 0x00000000, "alpha")], count=1000)
        _lines, parsed = self.dump(payload)
        self.assertEqual([(t, n) for (t, n, _g) in parsed], [(0x1000, "alpha")])

    def test_unknown_version_warns_once(self):
        # The descriptor batch arrives every 500 ms: the warning must not
        # be repeated for every batch.
        payload = envelope(4, [desc_v2(0x1000, 0x00000000, "alpha")])
        warnings = []
        for _ in range(5):
            lines, _parsed = self.dump(payload)
            warnings += [ln for ln in lines if WARN_RE.match(ln)]
        self.assertEqual(len(warnings), 1)
        self.assertEqual(WARN_RE.match(warnings[0]).group(1), "4")

    def test_unknown_version_is_decoded_with_the_v2_layout(self):
        payload = envelope(4, [desc_v2(0x1000, 0x00000000, "alpha")])
        _lines, parsed = self.dump(payload)
        self.assertEqual([(t, n) for (t, n, _g) in parsed], [(0x1000, "alpha")])

    def test_version_zero_is_rejected(self):
        # No version information at all: the layout cannot be guessed.
        payload = envelope(0, [desc_v2(0x1000, 0x00000000, "alpha")])
        lines, parsed = self.dump(payload)
        self.assertEqual(parsed, [])
        self.assertEqual([ln for ln in lines if ln.startswith("[dump-desc] tid=")], [])
        self.assertTrue(any(WARN_RE.match(ln) for ln in lines))

    def test_non_descriptor_subtype_is_ignored(self):
        payload = struct.pack("<BBHI", 1, 2, 1, 0) + desc_v2(0x1000, 0, "alpha")
        lines, parsed = self.dump(payload)
        self.assertEqual(parsed, [])
        self.assertEqual(lines, [])


if __name__ == "__main__":
    unittest.main()
