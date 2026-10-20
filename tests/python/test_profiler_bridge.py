# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Tests for profiler tool lookup and phantom-frame pruning."""

import os
import struct
import sys
import unittest
from io import StringIO
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[2] / "config" / "scripts"
with patch.object(sys, "path", [str(SCRIPTS), *sys.path]):
    from ove_dashboard import profiler


class TraceDescriptorDumpTest(unittest.TestCase):
    def test_v1_after_v2(self):
        current = struct.pack("<BBHIIIB", 0, 2, 1, 0, 7, 3, 6) + b"worker"
        legacy = struct.pack("<BBHIIB", 0, 1, 1, 0, 8, 4) + b"idle"
        with patch("sys.stderr", new_callable=StringIO) as stderr:
            profiler._trace_dump_descriptors(current)
            profiler._trace_dump_descriptors(legacy)
        self.assertEqual(stderr.getvalue().splitlines(), [
            "[dump-desc] tid=0x7 name=worker gen=0x3",
            "[dump-desc] tid=0x8 name=idle",
        ])

    def test_v2_reads_generation_on_each_record(self):
        payload = struct.pack("<BBHI", 0, 2, 2, 0)
        payload += struct.pack("<IIB", 7, 0x12345678, 6) + b"worker"
        payload += struct.pack("<IIB", 8, 0, 4) + b"idle"
        with patch("sys.stderr", new_callable=StringIO) as stderr:
            profiler._trace_dump_descriptors(payload)
        self.assertEqual(stderr.getvalue().splitlines(), [
            "[dump-desc] tid=0x7 name=worker gen=0x12345678",
            "[dump-desc] tid=0x8 name=idle gen=0x0",
        ])


# Synthetic firmware: function start, end, name — including two different
# static functions both named "helper".
SYMBOLS = [
    (0x100, 0x140, "main"),
    (0x200, 0x240, "a"),
    (0x300, 0x340, "b"),
    (0x400, 0x440, "c"),
    (0x500, 0x540, "helper"),
    (0x600, 0x640, "helper"),
    (0x700, 0x740, "r"),
    (0x800, 0x840, "hub"),
]

OBJDUMP = """
00000100 <main>:
     100:	push	{r7, lr}
     102:	bl	200 <a>
     106:	bl	700 <r>
     10a:	bl	800 <hub>
     10e:	pop	{r7, pc}

00000200 <a>:
     200:	push	{r7, lr}
     202:	bl	300 <b>
     206:	pop	{r7, pc}

00000300 <b>:
     300:	bx	lr

00000400 <c>:
     400:	push	{r7, lr}
     402:	bl	300 <b>
     406:	pop	{r7, pc}

00000500 <helper>:
     500:	push	{r7, lr}
     502:	bl	300 <b>
     506:	pop	{r7, pc}

00000600 <helper>:
     600:	push	{r7, lr}
     602:	nop
     604:	pop	{r7, pc}

00000700 <r>:
     700:	push	{r7, lr}
     702:	bl	700 <r>
     706:	bl	300 <b>
     70a:	pop	{r7, pc}

00000800 <hub>:
     800:	push	{r7, lr}
     802:	blx	r3
     804:	blx	r2
     806:	pop	{r7, pc}
"""


class PrunerTest(unittest.TestCase):
    def setUp(self):
        self.b = profiler
        symbols = patch.multiple(
            self.b, _profiler_sym_index=list(SYMBOLS), _profiler_symbol_frame=None)
        symbols.start()
        self.addCleanup(symbols.stop)
        calls, reach, indirect, hub = self.b._cfg_from_objdump(OBJDUMP)
        cfg = patch.multiple(
            self.b, _profiler_cfg_calls=calls, _profiler_cfg_reach=reach,
            _profiler_cfg_indirect=indirect, _profiler_cfg_indirect_hub=hub)
        cfg.start()
        self.addCleanup(cfg.stop)

    def kept(self, pcs):
        keep = self.b._prune_chain(pcs)
        return [pc for pc, k in zip(pcs, keep) if k]

    def test_cfg_is_keyed_by_function_address(self):
        calls = self.b._profiler_cfg_calls
        self.assertEqual(calls[0x500], {0x300})
        self.assertEqual(calls[0x600], set())
        self.assertIn(0x800, self.b._profiler_cfg_indirect_hub)

    def test_same_named_statics_are_distinct(self):
        # helper@0x500 calls b; helper@0x600 does not. Name-keyed tables
        # merged them and accepted b <- helper@0x600.
        self.assertTrue(self.b._edge_allowed(0x300, 0x500))
        self.assertFalse(self.b._edge_allowed(0x300, 0x600))

    def test_real_chain_is_kept(self):
        chain = [0x302, 0x206, 0x106]  # b <- a <- main
        self.assertEqual(self.kept(chain), chain)

    def test_wedged_phantom_is_dropped(self):
        # c called b once and left its {r7, lr} behind: b <- c <- a <- main.
        chain = [0x302, 0x406, 0x206, 0x106]
        self.assertEqual(self.kept(chain), [0x302, 0x206, 0x106])

    def test_recursion_is_kept(self):
        chain = [0x302, 0x70a, 0x706, 0x706, 0x10a]  # b <- r <- r <- r <- main
        self.assertEqual(self.kept(chain), chain)

    def test_duplicate_of_non_recursive_function_is_dropped(self):
        # a appears twice; only the outermost copy is real.
        chain = [0x302, 0x206, 0x206, 0x106]
        self.assertEqual(self.kept(chain), [0x302, 0x206, 0x106])

    def test_unresolved_frames_are_never_refuted(self):
        chain = [0x302, 0xDEAD0, 0x106]
        self.assertEqual(self.kept(chain), chain)

    def round_trip(self, word_size):
        fmt = {4: "<I", 8: "<Q"}[word_size]
        samples = [(1000, 0x2000, 0, [0x302, 0x406, 0x206, 0x106]),
                   (2000, 0x3000, 2, [0x302, 0x206, 0x106])]
        payload = struct.pack("<BBHI", 1, word_size, len(samples), 7)
        for ts, tid, state, pcs in samples:
            payload += struct.pack("<QIBBH", ts, tid, len(pcs), state, 0)
            payload += b"".join(struct.pack(fmt, pc) for pc in pcs)
        out = self.b._prune_profile_payload(payload)
        ver, ws, count, dropped = struct.unpack_from("<BBHI", out, 0)
        self.assertEqual((ver, ws, count, dropped), (1, word_size, 2, 7))
        p = 8
        got = []
        for _ in range(count):
            ts, tid, depth, state = struct.unpack_from("<QIBB", out, p)
            p += 16
            pcs = [struct.unpack_from(fmt, out, p + word_size * k)[0] for k in range(depth)]
            p += word_size * depth
            got.append((ts, tid, state, pcs))
        self.assertEqual(p, len(out))
        self.assertEqual(got[0], (1000, 0x2000, 0, [0x302, 0x206, 0x106]))
        self.assertEqual(got[1], (2000, 0x3000, 2, [0x302, 0x206, 0x106]))

    def test_payload_round_trip_4_byte_pcs(self):
        self.round_trip(4)

    def test_payload_round_trip_8_byte_pcs(self):
        self.round_trip(8)

    def test_unknown_word_size_passes_through(self):
        payload = struct.pack("<BBHI", 1, 2, 0, 0)
        self.assertEqual(self.b._prune_profile_payload(payload), payload)


class ToolsTest(unittest.TestCase):
    def test_profiler_tools_needs_a_gdb_name(self):
        self.assertEqual(profiler.profiler_tools(None), (None, None))
        self.assertEqual(profiler.profiler_tools("gdb-multiarch"), (None, None))

    def test_profiler_tools_finds_cross_binutils(self):
        with TemporaryDirectory() as tmp:
            tools = [Path(tmp) / f"arm-none-eabi-{tool}" for tool in ("nm", "objdump")]
            for tool in tools:
                tool.touch()
                tool.chmod(0o755)
            expected = tuple(map(str, tools))
            gdb = "arm-none-eabi-gdb"
            self.assertEqual(profiler.profiler_tools(str(Path(tmp) / gdb)), expected)
            with patch.dict(os.environ, {"PATH": tmp}):
                self.assertEqual(profiler.profiler_tools(gdb), expected)


if __name__ == "__main__":
    unittest.main()
