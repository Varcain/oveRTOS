# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Tests for the dashboard bridge's profiler symbolication and
phantom-frame pruning (config/scripts/ove-dashboard-bridge.py)."""

import importlib.util
import os
import struct
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BRIDGE = os.path.join(ROOT, "config", "scripts", "ove-dashboard-bridge.py")
TOOLCHAIN_BIN = os.path.join(
    ROOT, "output", "toolchains", "arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi", "bin")
EXAMPLE_ELF = os.path.join(
    ROOT, "output", "qemu", "freertos", "example_c", "build", "firmware", "firmware.elf")


def load_bridge():
    spec = importlib.util.spec_from_file_location("ove_dashboard_bridge", BRIDGE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


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
    @classmethod
    def setUpClass(cls):
        cls.b = load_bridge()
        cls.b._profiler_sym_index = list(SYMBOLS)
        (calls, reach, indirect, hub) = cls.b._cfg_from_objdump(OBJDUMP)
        cls.b._profiler_cfg_calls = calls
        cls.b._profiler_cfg_reach = reach
        cls.b._profiler_cfg_indirect = indirect
        cls.b._profiler_cfg_indirect_hub = hub

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
        b = load_bridge()
        self.assertEqual(b.profiler_tools(None), (None, None))
        self.assertEqual(b.profiler_tools("gdb-multiarch"), (None, None))

    @unittest.skipUnless(os.path.isdir(TOOLCHAIN_BIN), "ARM toolchain not installed")
    def test_profiler_tools_finds_cross_binutils(self):
        b = load_bridge()
        nm, objdump = b.profiler_tools(os.path.join(TOOLCHAIN_BIN, "arm-none-eabi-gdb"))
        self.assertTrue(nm.endswith("arm-none-eabi-nm"))
        self.assertTrue(objdump.endswith("arm-none-eabi-objdump"))


@unittest.skipUnless(os.path.isdir(TOOLCHAIN_BIN) and os.path.isfile(EXAMPLE_ELF),
                     "ARM toolchain or example_c firmware not built")
class RealFirmwareTest(unittest.TestCase):
    def test_symbols_and_cfg_from_firmware(self):
        b = load_bridge()
        nm, objdump = b.profiler_tools(os.path.join(TOOLCHAIN_BIN, "arm-none-eabi-gdb"))
        b.build_profiler_symbols(EXAMPLE_ELF, nm_bin=nm)
        self.assertGreater(len(b._profiler_sym_index), 100)
        names = {e[2] for e in b._profiler_sym_index}
        self.assertIn("ove_main", names)
        b._build_profiler_cfg(EXAMPLE_ELF, objdump_bin=objdump)
        self.assertGreater(len(b._profiler_cfg_reach), 100)
        # Per-file copies of a static helper stay separate CFG nodes.
        starts = [e[0] for e in b._profiler_sym_index if e[2] == "lv_memzero"]
        if len(starts) > 1:
            self.assertGreater(len(set(starts) & set(b._profiler_cfg_calls)), 1)


if __name__ == "__main__":
    unittest.main()
