# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Read finite ELF/config artifacts without loading or executing ARM code."""

import shutil
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ove import test as ove_test
from ove.nuttx_startup import _reset_transfer, inspect_arm_startup, require_arm_startup


class BoundaryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.elf = self.root / "nuttx"
        self.config = self.root / ".config"
        self.script = self.root / "flash.ld.tmp"

    def image(self, board="mps2-an500", region=0x20000000, length=4 * 1024**2,
              bss_size=8, idle=2048, load_extra=0, payload_delta=0):
        flash = 0 if board == "mps2-an500" else 0x08000000
        boundary = region + 8 + bss_size
        data = bytearray(0x500)
        data[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
        names = b"\0.text\0.data\0.bss\0.strtab\0.symtab\0.shstrtab\0"
        strings = b"\0_vectors\0g_idle_topstack\0_sbss\0_ebss\0start\0__start\0"
        data[0x180:0x180 + len(strings)] = strings
        data[0x280:0x280 + len(names)] = names
        struct.pack_into("<HHIIIIIHHHHHH", data, 16,
                         2, 40, 1, flash + 17, 52, 0x300, 0x05000000,
                         52, 32, 2, 40, 7, 6)
        struct.pack_into("<IIIIIIII", data, 52,
                         1, 0x100, flash, flash, 32, 32, 5, 4)
        struct.pack_into("<IIIIIIII", data, 84,
                         1, 0x120, region, region, 8, 8 + bss_size + load_extra, 6, 4)
        struct.pack_into("<II", data, 0x100, boundary + idle, flash + 9)
        # Encodings: mov.w lr,#0; b.w to the immediately following __start.
        struct.pack_into("<HHHHH", data, 0x108, 0xf04f, 0x0e00, 0xf000, 0xb800, 0x4770)
        struct.pack_into("<I", data, 0x11c, boundary + idle + payload_delta)
        symbols = [(b"start", flash + 9, 8, 2, 1),
                   (b"_vectors", flash, 8, 17, 1),
                   (b"g_idle_topstack", flash + 28, 4, 17, 1),
                   (b"_sbss", region + 8, 0, 16, 0xfff1),
                   (b"_ebss", boundary, 0, 16, 0xfff1),
                   (b"__start", flash + 17, 2, 18, 1)]
        self.symbol_offsets = {}
        for index, (name, value, size, info, section) in enumerate(symbols, 1):
            self.symbol_offsets[name.decode()] = 0x200 + 16 * index
            struct.pack_into("<IIIBBH", data, 0x200 + 16 * index,
                             strings.index(name), value, size, info, 0, section)
        sections = [(0,) * 10,
                    (names.index(b".text"), 1, 6, flash, 0x100, 32, 0, 0, 4, 0),
                    (names.index(b".data"), 1, 3, region, 0x120, 8, 0, 0, 4, 0),
                    (names.index(b".bss"), 8, 3, region + 8, 0x128, bss_size, 0, 0, 4, 0),
                    (names.index(b".strtab"), 3, 0, 0, 0x180, len(strings), 0, 0, 1, 0),
                    (names.index(b".symtab"), 2, 0, 0, 0x200, 112, 4, 2, 4, 16),
                    (names.index(b".shstrtab"), 3, 0, 0, 0x280, len(names), 0, 0, 1, 0)]
        for index, section in enumerate(sections):
            struct.pack_into("<IIIIIIIIII", data, 0x300 + 40 * index, *section)
        self.elf.write_bytes(data[:0x300 + 40 * len(sections)])
        option = "MPS2_AN500" if board == "mps2-an500" else "STM32F746G_DISCO"
        self.config.write_text(
            f'CONFIG_BUILD_FLAT=y\nCONFIG_ARCH_BOARD="{board}"\n'
            f'CONFIG_ARCH_BOARD_{option}=y\nCONFIG_RAM_START={region:#x}\n'
            f'CONFIG_RAM_SIZE={length}\nCONFIG_IDLETHREAD_STACKSIZE={idle}\n')
        self.script.write_text(f"sram1 (rwx) : ORIGIN = {region:#x}, LENGTH = {length}\n")
        return board

    def inspect(self, board="mps2-an500"):
        return inspect_arm_startup(self.elf, self.config, self.script, board)

    def test_supported_primary_geometries(self):
        for board, region, length in (("mps2-an500", 0x20000000, 4 * 1024**2),
                                      ("mps2-an500", 0x60000000, 16 * 1024**2),
                                      ("stm32f746g-disco", 0x20010000, 240 * 1024)):
            with self.subTest(board=board, region=region):
                self.image(board, region, length)
                self.assertEqual(self.inspect(board)["failed_checks"], [])
                require_arm_startup(self.elf, self.config, self.script, board)

    def test_four_byte_bss_tail_rejects_initial_stack(self):
        self.image(bss_size=4)
        self.assertEqual(self.inspect()["failed_checks"], ["bss_end_aligned8", "entry_sp_aligned8"])
        with self.assertRaisesRegex(RuntimeError, "bss_end_aligned8"):
            require_arm_startup(self.elf, self.config, self.script, "mps2-an500")

    def test_writable_segment_cannot_overlap_idle_stack(self):
        self.image(load_extra=8)
        self.assertEqual(self.inspect()["failed_checks"], ["all_primary_writable_loads_before_idle"])

    def test_idle_payload_must_match_vector_and_bss(self):
        self.image(payload_delta=8)
        self.assertEqual(self.inspect()["failed_checks"], ["vector_payload_agree"])

    def test_invalid_idle_geometry_is_named(self):
        for idle, expected in ((0, "positive_idle_size"), (2052, "idle_size_aligned8"),
                               (4 * 1024**2, "idle_interval_fits_ram")):
            with self.subTest(idle=idle):
                self.image(idle=idle)
                self.assertIn(expected, self.inspect()["failed_checks"])

    def test_test_ram_aperture_is_recorded_without_resizing_native_heap(self):
        self.image(region=0x60000000, length=16 * 1024**2)
        self.config.write_text(self.config.read_text().replace("16777216", "2097152"))
        evidence = self.inspect()
        self.assertEqual(evidence["failed_checks"], [])
        self.assertEqual(evidence["configured_ram_size"], 2 * 1024**2)
        self.assertEqual(evidence["primary_ram_size"], 16 * 1024**2)

    def test_conflicting_modes_and_board_identity_are_rejected(self):
        for option in ("CONFIG_ARCH_SIM", "CONFIG_BUILD_PROTECTED", "CONFIG_BUILD_KERNEL", "CONFIG_BUILD_PIC",
                       "CONFIG_SMP", "CONFIG_ARCH_BOARD_STM32F746G_DISCO"):
            with self.subTest(option=option):
                self.image()
                with self.config.open("a") as stream:
                    stream.write(option + "=y\n")
                with self.assertRaises(ValueError):
                    self.inspect()
        self.image()
        with self.assertRaisesRegex(ValueError, "configuration does not match"):
            self.inspect("stm32f746g-disco")

    def test_undefined_wrong_type_and_oversized_objects_are_rejected(self):
        for name, delta, pattern, value in (("_vectors", 14, "<H", 0),
                                           ("_vectors", 12, "<B", 18),
                                           ("g_idle_topstack", 8, "<I", 32)):
            offset = {"_vectors": 0x220, "g_idle_topstack": 0x230}[name] + delta
            with self.subTest(offset=offset):
                self.image()
                data = bytearray(self.elf.read_bytes())
                struct.pack_into(pattern, data, offset, value)
                self.elf.write_bytes(data)
                with self.assertRaises(ValueError):
                    self.inspect()

    def test_truncated_elf_is_rejected_before_reading_objects(self):
        self.image()
        self.elf.write_bytes(self.elf.read_bytes()[:100])
        with self.assertRaises(RuntimeError):
            require_arm_startup(self.elf, self.config, self.script, "mps2-an500")

    def test_stackcheck_reserve_is_board_specific(self):
        self.image("stm32f746g-disco", 0x20010000, 240 * 1024, idle=64)
        with self.config.open("a") as stream:
            stream.write("CONFIG_ARMV7M_STACKCHECK=y\n")
        self.assertEqual(self.inspect("stm32f746g-disco")["failed_checks"], ["stackcheck_reserve"])

    def test_known_reset_rejects_spills_calls_conditional_and_wrong_targets(self):
        for halfwords in ((0xb480, 0xaf00, 0xf04f, 0x0e00),
                          (0xb580, 0xaf00, 0xf04f, 0x0e00),
                          (0xf04f, 0x0e00, 0xf000, 0xf800),
                          (0xf04f, 0x0e00, 0xf000, 0x8000),
                          (0xf04f, 0x0e00, 0xd000, 0xbf00),
                          (0xf04f, 0x0e00, 0xf000, 0xb801)):
            with self.subTest(halfwords=halfwords):
                self.image()
                data = bytearray(self.elf.read_bytes())
                struct.pack_into("<HHHH", data, 0x108, *halfwords)
                self.elf.write_bytes(data)
                with self.assertRaisesRegex(RuntimeError, "known_reset_transfer"):
                    require_arm_startup(self.elf, self.config, self.script, "mps2-an500")

    def test_reset_vector_must_select_the_known_thumb_stub(self):
        self.image()
        data = bytearray(self.elf.read_bytes())
        struct.pack_into("<I", data, 0x104, 17)
        self.elf.write_bytes(data)
        with self.assertRaisesRegex(RuntimeError, "known_reset_transfer"):
            require_arm_startup(self.elf, self.config, self.script, "mps2-an500")

    def test_malformed_or_truncated_reset_function_metadata_is_rejected(self):
        for name, delta, pattern, value in (("start", 14, "<H", 0),
                                           ("start", 8, "<I", 5),
                                           ("start", 8, "<I", 7),
                                           ("start", 8, "<I", 64),
                                           ("__start", 4, "<I", 16),
                                           ("__start", 12, "<B", 17)):
            with self.subTest(name=name, delta=delta, value=value):
                self.image()
                data = bytearray(self.elf.read_bytes())
                struct.pack_into(pattern, data, self.symbol_offsets[name] + delta, value)
                self.elf.write_bytes(data)
                with self.assertRaisesRegex(RuntimeError, "Cannot admit native ARM startup"):
                    require_arm_startup(self.elf, self.config, self.script, "mps2-an500")

    def test_ambiguous_c_startup_function_is_rejected(self):
        self.image()
        data = bytearray(self.elf.read_bytes())
        original = self.symbol_offsets["__start"]
        data[0x270:0x280] = data[original:original + 16]
        struct.pack_into("<I", data, 0x300 + 5 * 40 + 20, 128)
        self.elf.write_bytes(data)
        with self.assertRaisesRegex(RuntimeError, "ambiguous native function"):
            require_arm_startup(self.elf, self.config, self.script, "mps2-an500")

    def test_reset_functions_require_executable_sections_and_loads(self):
        for offset, flags in ((52 + 24, 4), (0x300 + 40 + 8, 2)):
            with self.subTest(offset=offset):
                self.image()
                data = bytearray(self.elf.read_bytes())
                struct.pack_into("<I", data, offset, flags)
                self.elf.write_bytes(data)
                with self.assertRaisesRegex(RuntimeError, "Cannot admit native ARM startup"):
                    require_arm_startup(self.elf, self.config, self.script, "mps2-an500")

    def test_arm_source_change_preserves_apps_and_admits_only_after_build(self):
        self.exercise_arm_build_admission()

    def test_arm_build_does_not_return_or_launch_a_malformed_image(self):
        self.exercise_arm_build_admission(malformed=True)

    def test_arm_build_does_not_return_or_launch_a_misaligned_image(self):
        self.exercise_arm_build_admission(bad_boundary=True)

    def test_reused_source_cannot_leave_stale_admission_after_failure(self):
        self.exercise_arm_build_admission(bad_boundary=True, reuse_source=True)

    def exercise_arm_build_admission(self, malformed=False, bad_boundary=False, reuse_source=False):
        self.image(bss_size=4 if bad_boundary else 8)
        if malformed:
            self.elf.write_bytes(self.elf.read_bytes()[:100])
        build = self.root / "build"
        apps = self.root / "nuttx-apps"
        apps.mkdir()
        (apps / "preserved").write_text("external test app")
        build.mkdir()
        home = "/prepared" if reuse_source else "/raw"
        (build / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=" + home + "\n")
        (build / "stale").write_text("generated")
        (build / "ove-startup-boundary.json").write_text("old admission evidence")
        record = self.root / ".ove-source-trees"

        def prepare(workspace, source, layers):
            self.assertEqual(source, "/raw")
            self.assertEqual(layers, [("global patch", "/ove/config/patches/nuttx")])
            self.assertEqual(Path(workspace.build_dir), self.root)
            record.write_text("/prepared\n")
            return "/prepared"

        def run(command, **kwargs):
            self.assertTrue(record.exists())
            self.assertEqual((build / "stale").exists(), reuse_source)
            self.assertFalse((build / "ove-startup-boundary.json").exists())
            if "--build" in command:
                for source, target in ((self.elf, "nuttx"), (self.config, ".config"),
                                       (self.script, "flash.ld.tmp")):
                    shutil.copyfile(source, build / target)
            else:
                if "-S" in command:
                    self.assertEqual(command[command.index("-S") + 1], "/prepared")
                build.mkdir(exist_ok=True)

        with mock.patch("ove.build._shared_patched_tree", side_effect=prepare), \
                mock.patch("ove.build._check_source_trees_unchanged") as unchanged, \
                mock.patch.object(ove_test, "run", side_effect=run):
            launcher = mock.Mock()

            def build_then_launch():
                ove_test._nuttx_cmake_build("/ove", nuttx_src="/raw", apps_build=str(apps),
                    build_dir=str(build), board_config="mps2-an500:nsh", defconfig_overlays=[],
                    coverage=False, variant_dir="/variant")
                launcher()

            if malformed or bad_boundary:
                message = "Cannot admit native ARM startup" if malformed else "Native ARM startup boundary failed"
                with self.assertRaisesRegex(RuntimeError, message):
                    build_then_launch()
                launcher.assert_not_called()
            else:
                build_then_launch()
                launcher.assert_called_once()
        self.assertEqual((apps / "preserved").read_text(), "external test app")
        self.assertEqual(record.read_text(), "/prepared\n")
        self.assertEqual((build / "ove-startup-boundary.json").exists(), not (malformed or bad_boundary))
        unchanged.assert_called_once()


class NativeTestBuildTest(unittest.TestCase):
    def test_sim_preserves_raw_source_and_skips_arm_admission(self):
        with mock.patch("ove.build._shared_patched_tree") as prepare, \
                mock.patch("ove.nuttx_startup.require_arm_startup") as admit, \
                mock.patch.object(ove_test, "run"), mock.patch.object(ove_test.os.path, "isfile", return_value=False):
            ove_test._nuttx_cmake_build("/ove", nuttx_src="/raw", apps_build="/apps",
                build_dir="/build", board_config="sim:nsh", defconfig_overlays=[],
                coverage=False, variant_dir="/variant")
        prepare.assert_not_called()
        admit.assert_not_called()


class ResetEncodingTest(unittest.TestCase):
    def test_branch_widths_signs_and_independent_wide_immediate_bits(self):
        # Control encodings are emitted by reset/branch-facts.S. No encoder
        # inverse is used here: literals exercise the decoder independently.
        facts = [((0xe000,), 0), ((0xe7f8,), -16),
                 ((0xf000, 0xb800), 0), ((0xf7ff, 0xbff8), -16),
                 ((0xf000, 0xb000), 0x400000), ((0xf000, 0x9800), 0x800000),
                 ((0xf400, 0xb800), -0x400000), ((0xf400, 0xb000), -0x800000)]
        start = 0x1000000
        for halfwords, displacement in facts:
            with self.subTest(halfwords=halfwords):
                code = struct.pack("<HH", 0xf04f, 0x0e00) + struct.pack("<" + "H" * len(halfwords), *halfwords)
                image = mock.Mock()
                image.symbols = {"_vectors": (0, 8, 17, 1)}
                image.word.return_value = start + 1
                target = start + 8 + displacement
                image.function_bytes.side_effect = [(start, code), (target, b"\x70\x47")]
                evidence = _reset_transfer(image)
                self.assertTrue(evidence["known_transfer"])
                self.assertEqual(evidence["branch_bytes"], 2 * len(halfwords))
                self.assertEqual(evidence["branch_target"], target)


if __name__ == "__main__":
    unittest.main()
