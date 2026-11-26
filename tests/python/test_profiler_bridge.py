# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Tests for profiler tool lookup and phantom-frame pruning."""

import json
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
    from ove_dashboard.protocol import FRAME_PROFILE, PROFILE_SUB_SYMBOLS


class ProfilerSymbolsTest(unittest.TestCase):
    def test_linux_guest_windows(self):
        config = {
            "CONFIG_OVE_LINUX_ROOTFS_BASE": "0x60000000",
            "CONFIG_OVE_LINUX_ROOTFS_SIZE": "0x00800000",
            "CONFIG_OVE_LINUX_GUEST_POOL_BASE": "0x60800000",
            "CONFIG_OVE_LINUX_GUEST_POOL_SIZE": "0x00800000",
        }
        with (patch.object(profiler, "_nm_symbols", return_value=[(0x100, 0x40, "main")]),
              patch.multiple(profiler, _profiler_sym_index=None, _profiler_symbol_frame=None)):
            profiler.build_profiler_symbols(__file__, guest_ranges=profiler.linux_guest_ranges(config))
            frame = profiler._profiler_symbol_frame
            self.assertEqual(frame[:5], struct.pack("<IB", FRAME_PROFILE, PROFILE_SUB_SYMBOLS))
            self.assertEqual(json.loads(frame[5:])[1:], [
                [0x60000000, 0x60800000, "[Linux guest]"],
                [0x60800000, 0x61000000, "[Linux guest]"],
            ])
            for pc in (0x60123456, 0x60900000):
                self.assertEqual(profiler._profiler_resolve(pc), "[Linux guest]")
                self.assertIsNone(profiler._profiler_func_start(pc))
            self.assertEqual(profiler._profiler_resolve(0x61000000), "0x61000000")
            self.assertEqual(profiler._profiler_resolve(0x120), "main")

    def test_no_guest_ranges_without_base_and_size(self):
        self.assertEqual(profiler.linux_guest_ranges({}), [])
        self.assertEqual(profiler.linux_guest_ranges({
            "CONFIG_OVE_LINUX_ROOTFS_BASE": "0x60000000",
            "CONFIG_OVE_LINUX_GUEST_POOL_BASE": "0",
            "CONFIG_OVE_LINUX_GUEST_POOL_SIZE": "0x00800000",
        }), [])


class ProfilerSampleDumpTest(unittest.TestCase):
    def test_distinguishes_isr_and_thread_samples(self):
        payload = struct.pack("<BBHI", 1, 4, 2, 0)
        payload += struct.pack("<QIBBHI", 10, 0, 1, 0, 25, 0x1000)
        payload += struct.pack("<QIBBHI", 20, 0x2000, 1, 0, 0, 0x3000)
        with patch("sys.stderr", new_callable=StringIO) as stderr:
            profiler._profiler_dump_samples(payload)
        self.assertEqual(stderr.getvalue().splitlines(), [
            "[dump] t=10 isr=25 st=0 d=1 0x1000@0x1000",
            "[dump] t=20 tid=0x2000 st=0 d=1 0x3000@0x3000",
        ])


class TraceDescriptorDumpTest(unittest.TestCase):
    def test_v1_after_v3(self):
        current = struct.pack("<BBHIIIB", 0, 3, 1, 0, 7, 3, 6) + b"worker"
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
    (0x900, 0x940, "wrap"),
    (0xA00, 0xA40, "irec"),
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

00000900 <wrap>:
     900:	push	{r7, lr}
     902:	blx	r3
     904:	pop	{r7, pc}

00000a00 <irec>:
     a00:	push	{r7, lr}
     a02:	blx	r3
     a04:	bl	300 <b>
     a08:	pop	{r7, pc}
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

    def test_single_indirect_caller_permits_edge(self):
        # wrap has exactly one blx, so its reach set is incomplete. The
        # old hub-only rule (>=2 blx) trimmed it from the top of the chain.
        self.assertTrue(self.b._edge_allowed(0x300, 0x900))
        self.assertFalse(self.b._edge_allowed(0x300, 0x900, strict=True))
        self.assertEqual(self.kept([0x302, 0x902]), [0x302, 0x902])

    def test_indirect_recursion_keeps_duplicates(self):
        # irec could call itself through the function pointer, so two
        # copies are not provably stale LR values.
        chain = [0x302, 0xA04, 0xA04, 0xDEAD0]
        self.assertEqual(self.kept(chain), chain)

    def test_leaf_is_never_dropped(self):
        # The sampled PC is real even when the frame above refutes it.
        chain = [0x402, 0x206, 0x302]  # c <- a <- b; a never calls c
        self.assertEqual(self.kept(chain)[0], 0x402)

    def test_leaf_survives_a_duplicate_above_it(self):
        # a <- stale a <- main: the sampled PC is in a, so the copy above
        # it is the phantom, not the sampled PC.
        self.assertEqual(self.kept([0x202, 0x206, 0x106]), [0x202, 0x106])

    def test_leaf_duplicate_under_an_indirect_caller_is_dropped(self):
        # b <- stale b <- wrap: b cannot recurse, so the copy above the
        # sampled PC goes even though wrap's blx could explain either one.
        self.assertEqual(self.kept([0x302, 0x304, 0x902]), [0x302, 0x902])

    def test_unresolved_frame_does_not_validate_a_drop(self):
        # b <- ? <- a <- helper@0x600: the unresolved frame cannot show that
        # dropping a repairs the chain; the refuted top goes instead.
        chain = [0x302, 0xDEAD0, 0x206, 0x606]
        self.assertEqual(self.kept(chain), [0x302, 0xDEAD0, 0x206])

    def test_phantom_top_does_not_erase_the_chain(self):
        # b <- a <- main <- c: a phantom c above the real root refutes main,
        # but must not cascade down and remove main, a and b.
        chain = [0x302, 0x206, 0x106, 0x406]
        self.assertEqual(self.kept(chain)[:3], [0x302, 0x206, 0x106])

    def test_refuted_top_is_trimmed(self):
        # helper@0x600 calls nothing and has no indirect call site, so it
        # cannot be main's caller and nothing above can validate it.
        chain = [0x302, 0x206, 0x106, 0x606]
        self.assertEqual(self.kept(chain), [0x302, 0x206, 0x106])

    def test_indirect_caller_does_not_validate_across_a_frame(self):
        # wrap could have called b through its pointer, but the scan for a
        # validator is strict: helper@0x600 between them stays (unvalidated).
        chain = [0x302, 0x606, 0x902]
        self.assertEqual(self.kept(chain), chain)

    def test_frame_permitted_by_an_indirect_call_is_not_isolated(self):
        # b <- wrap is permitted (wrap's reach is incomplete), so wrap is
        # not an isolated phantom; helper@0x600 above it is, and main is a
        # refuted top.
        chain = [0x302, 0x902, 0x606, 0x106]
        self.assertEqual(self.kept(chain), [0x302, 0x902])

    def test_duplicate_of_non_recursive_function_is_dropped(self):
        # a appears twice; only the outermost copy is real.
        chain = [0x302, 0x202, 0x206, 0x106]
        self.assertEqual(self.kept(chain), [0x302, 0x206, 0x106])

    def test_unresolved_frames_are_never_refuted(self):
        chain = [0x302, 0xDEAD0, 0x106]
        self.assertEqual(self.kept(chain), chain)

    def round_trip(self, word_size):
        fmt = {4: "<I", 8: "<Q"}[word_size]
        # The second sample is a handler-mode one: tid 0, exception 15.
        samples = [(1000, 0x2000, 0, 0, [0x302, 0x406, 0x206, 0x106]),
                   (2000, 0, 2, 15, [0x302, 0x206, 0x106])]
        payload = struct.pack("<BBHI", 1, word_size, len(samples), 7)
        for ts, tid, state, isr, pcs in samples:
            payload += struct.pack("<QIBBH", ts, tid, len(pcs), state, isr)
            payload += b"".join(struct.pack(fmt, pc) for pc in pcs)
        out = self.b._prune_profile_payload(payload)
        ver, ws, count, dropped = struct.unpack_from("<BBHI", out, 0)
        self.assertEqual((ver, ws, count, dropped), (1, word_size, 2, 7))
        p = 8
        got = []
        for _ in range(count):
            ts, tid, depth, state, isr = struct.unpack_from("<QIBBH", out, p)
            p += 16
            pcs = [struct.unpack_from(fmt, out, p + word_size * k)[0] for k in range(depth)]
            p += word_size * depth
            got.append((ts, tid, state, isr, pcs))
        self.assertEqual(p, len(out))
        self.assertEqual(got[0], (1000, 0x2000, 0, 0, [0x302, 0x206, 0x106]))
        self.assertEqual(got[1], (2000, 0, 2, 15, [0x302, 0x206, 0x106]))

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
