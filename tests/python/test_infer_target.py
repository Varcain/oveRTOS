# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Target qualification must reject incomplete, mismatched or unqualified results."""

import copy
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ove import appgen
from ove import infer_target as target
from ove import test as test_cli
from ove.build import _prepare_freertos_build_dir

ROOT = Path(__file__).resolve().parents[2]


def complete_log(zero_heap=True):
    lines = []
    for mode in (("init",) if zero_heap else ("init", "create")):
        lines.append("IT_BEGIN " + mode)
        for seed in target.SEEDS:
            lines.extend(f"IT_FRAME {seed} {frame} " + "00" * 40 for frame in range(49))
            lines.extend([
                f"IT_ARENA {seed} preprocessor 32768 20000 0",
                f"IT_SCORES {seed} " + "00" * 4,
                f"IT_ARENA {seed} classifier 32768 21000 0",
            ])
        lines.append("IT_END")
    return "\n".join(lines + ["IT_RESULT 0"]) + "\n"


def expected_outputs():
    return {str(seed): {"features": ["00" * 40] * 49, "scores": "00" * 4}
            for seed in target.SEEDS}


def model_symbol_elf():
    """A metadata-only ARM ELF container; it has no executable sections."""
    payload, strings, symbols = bytearray(), bytearray(b"\0"), bytearray(16)
    for name in target.MODEL_NAMES:
        payload.extend(bytes((-len(payload)) % 16))
        original = (ROOT / "models/keyword" / (name + ".tflite")).read_bytes()
        symbols.extend(struct.pack("<IIIBBH", len(strings), 0x1000 + len(payload),
                                   len(original), 0x11, 0, 1))
        strings.extend(("g_" + name + "_model_data\0").encode())
        payload.extend(original)
    string_start = 512 + len(payload)
    symbol_start = (string_start + len(strings) + 3) & ~3
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    header = struct.pack("<16sHHIIIIIHHHHHH", ident, 1, 40, 1, 0, 0, 52, 0,
                         52, 0, 0, 40, 4, 0)
    sections = bytes(40)
    sections += struct.pack("<10I", 0, 1, 2, 0x1000, 512, len(payload), 0, 0, 16, 0)
    sections += struct.pack("<10I", 0, 3, 0, 0, string_start, len(strings), 0, 0, 1, 0)
    sections += struct.pack("<10I", 0, 2, 0, 0, symbol_start, len(symbols), 2, 1, 4, 16)
    result = bytearray(header + sections)
    result.extend(bytes(512 - len(result)))
    result.extend(payload + strings)
    result.extend(bytes(symbol_start - len(result)))
    result.extend(symbols)
    return result


class TargetOraclesTest(unittest.TestCase):
    def test_complete_both_modes(self):
        for zero in (False, True):
            results = target.parse_output(complete_log(zero), zero_heap=zero)
            target.compare_goldens(results, expected_outputs())
            self.assertEqual(set(results), {"init"} if zero else {"init", "create"})

    def test_qemu_diagnostics_are_separate_from_the_strict_guest_protocol(self):
        warning = "qemu-system-arm: warning: nic lan9118.0 has no peer\n"
        with tempfile.TemporaryDirectory() as directory:
            workspace = Path(directory)

            def execute(command, *, stdout, **_kwargs):
                if "-chardev" in command:
                    option = command[command.index("-chardev") + 1]
                    path = Path(option.split(",path=", 1)[1].replace(",,", ","))
                    path.write_text(complete_log())
                    stdout.write(warning)
                else:
                    stdout.write(warning + complete_log())
                return 0

            with mock.patch.object(test_cli, "_run_owned_test_process", side_effect=execute):
                text = target.run_qemu("/usr/bin/qemu-system-arm", workspace / "image.elf", workspace)
            self.assertEqual(text, complete_log())
            target.parse_output(text, zero_heap=True)
            self.assertEqual((workspace / "qualification-qemu.log").read_text(), warning)
        with self.assertRaisesRegex(ValueError, "unexpected record"):
            target.parse_output(warning + complete_log(), zero_heap=True)

    def test_only_the_exact_nonfatal_board_diagnostic_is_allowed(self):
        qemu = "/usr/bin/qemu-system-arm"
        target.qemu_diagnostics("", qemu)
        warning = "qemu-system-arm: warning: nic lan9118.0 has no peer\n"
        target.qemu_diagnostics(warning, qemu)
        for text in (warning * 2, warning + "error\n", "unknown diagnostic\n"):
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, "QEMU diagnostics"):
                target.qemu_diagnostics(text, qemu)

    def test_feature_first_middle_and_last_bytes_are_compared(self):
        actual = target.parse_output(complete_log(), zero_heap=True)
        for seed, frame, index in ((0, 0, 0), (17, 24, 20), (17, 48, 39)):
            with self.subTest(seed=seed, frame=frame, index=index):
                expected = expected_outputs()
                raw = bytearray.fromhex(expected[str(seed)]["features"][frame])
                raw[index] = 1
                expected[str(seed)]["features"][frame] = raw.hex()
                with self.assertRaisesRegex(ValueError, f"seed={seed} frame={frame} feature={index}"):
                    target.compare_goldens(actual, expected)

    def test_every_score_is_compared(self):
        actual = target.parse_output(complete_log(), zero_heap=True)
        for seed in target.SEEDS:
            for index in range(4):
                expected = expected_outputs()
                raw = bytearray(4)
                raw[index] = 1
                expected[str(seed)]["scores"] = raw.hex()
                with self.assertRaisesRegex(ValueError, f"seed={seed} score={index}"):
                    target.compare_goldens(actual, expected)

    def test_partial_duplicate_reordered_extra_and_failed_logs_are_rejected(self):
        good = complete_log()
        last = "IT_FRAME 17 48 " + "00" * 40 + "\n"
        first = "IT_FRAME 0 0 " + "00" * 40 + "\n"
        bad = [good.replace(last, ""), good.replace(last, last * 2),
               good.replace("IT_FRAME 0 0", "IT_FRAME 0 1"),
               good.replace(first, first[:-4] + "\n"), good + "failure\n",
               good.replace("IT_RESULT 0", "IT_RESULT 1"), "IT_RESULT 0\n",
               good.replace("IT_ARENA 0 preprocessor 32768 20000 0",
                            "IT_ARENA 0 preprocessor 32768 32753 0")]
        for text in bad:
            with self.subTest(text=text[-80:]), self.assertRaises(ValueError):
                target.parse_output(text, zero_heap=True)

    def test_usage_requires_nonzero_and_exact_sixteen_byte_margin(self):
        target.headroom(32768, 32752, 0)
        for cap, used, padding in ((32768, 32753, 0), (32768, 0, 0), (32768, 32769, 0),
                                   (1 << 64, 100, 0), (32768, 100, 16), (32768, -1, 0)):
            with self.subTest(cap=cap, used=used, padding=padding), self.assertRaises(ValueError):
                target.headroom(cap, used, padding)

    def test_radix_rule_and_actual_model_linkage(self):
        for n in (1, 2, 3, 5, 30, 512):
            self.assertTrue(target.radix_235(n))
        for n in (0, -2, 7, 14, 514, 2.0, True):
            self.assertFalse(target.radix_235(n))
        source = ROOT / "models/keyword/audio_preprocessor_int8.tflite"
        data = source.read_bytes()
        self.assertEqual(target.rfft_metadata(data, 1), [512])
        classifier = ROOT / "models/keyword/micro_speech_quantized.tflite"
        self.assertEqual(target.rfft_metadata(classifier.read_bytes(), 0), [])
        reader = target.ModelBytes(data)
        for graph in reader.vector(reader.root, 2):
            for op in reader.vector(graph, 3):
                raw = reader.blob(op, 5)
                if b"fft_length\0" not in raw:
                    continue
                parent_width = raw[-1]
                width = 1 << (raw[-2] & 3)
                root = len(raw) - 2 - parent_width
                values = root - int.from_bytes(raw[root:root + parent_width], "little")
                offset = reader.target(reader.field(op, 5)) + 4 + values + width
                changed = bytearray(data)
                changed[offset] = 2  # Checker only: 512 -> 514; never handed to TFLM.
                with self.assertRaisesRegex(ValueError, "unsupported RFFT"):
                    target.rfft_metadata(changed, 1)
        with self.assertRaisesRegex(ValueError, "RFFT operator count"):
            target.rfft_metadata(data.replace(b"SignalRfft", b"SignalXfft"), 1)

    def test_reference_label_cannot_hide_cmsis_sources_or_host_build(self):
        rows = [{"file": "/sdk/tensorflow/lite/micro/kernels/" + name,
                 "command": "arm-g++ -mcpu=cortex-m7 -DTF_LITE_STATIC_MEMORY -c " + name}
                for name in ("depthwise_conv.cc", "fully_connected.cc", "softmax.cc")]
        rows.append({"file": "/sdk/tensorflow/lite/micro/micro_allocator.cc",
                     "command": "arm-g++ -mcpu=cortex-m7 -DTF_LITE_STATIC_MEMORY -c micro_allocator.cc"})
        self.assertEqual(target.selected_provider(rows, "reference"), "arm-g++")
        changed = copy.deepcopy(rows)
        changed[0]["file"] = changed[0]["file"].replace("kernels/", "kernels/cmsis_nn/")
        with self.assertRaisesRegex(ValueError, "compiled provider"):
            target.selected_provider(changed, "reference")
        changed = copy.deepcopy(rows)
        changed[-1]["command"] = "g++ -DTF_LITE_STATIC_MEMORY -c micro_allocator.cc"
        with self.assertRaisesRegex(ValueError, "Cortex-M7"):
            target.selected_provider(changed, "reference")

    def test_shipped_arena_inventory(self):
        budgets = target.app_budgets(ROOT)
        self.assertEqual(len(budgets), 8)
        self.assertTrue(all(row["capacity"] == 32768 for row in budgets.values()))

    def test_linked_arrays_require_original_bytes_alignment_and_arm(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "metadata.elf"
            original = model_symbol_elf()
            path.write_bytes(original)
            self.assertEqual(set(target.model_arrays(ROOT, path)), set(target.MODEL_NAMES))
            for offset, value in ((512, original[512] ^ 1), (18, 62)):
                changed = original.copy()
                changed[offset] = value
                path.write_bytes(changed)
                with self.assertRaises(ValueError):
                    target.model_arrays(ROOT, path)
            changed = original.copy()
            symbol_start = struct.unpack_from("<I", original, 52 + 3 * 40 + 16)[0]
            struct.pack_into("<I", changed, symbol_start + 16 + 4, 0x1001)
            path.write_bytes(changed)
            with self.assertRaisesRegex(ValueError, "alignment"):
                target.model_arrays(ROOT, path)

    def test_sdk_selection_rejects_a_foreign_copy_and_missing_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "dl/tflite-micro/tensorflow/lite/micro/micro_allocator.cc"
            source.parent.mkdir(parents=True)
            source.write_text("canonical")
            rows = [{"file": str(source)}]
            self.assertEqual(len(target.sdk_sources(root, rows, "cmsis")), 1)
            with self.assertRaisesRegex(ValueError, "source selection"):
                target.sdk_sources(root, rows, "reference")
            foreign = root / "foreign/tensorflow/lite/micro/micro_allocator.cc"
            foreign.parent.mkdir(parents=True)
            foreign.write_bytes(source.read_bytes())
            with self.assertRaisesRegex(ValueError, "outside the clean"):
                target.sdk_sources(root, [{"file": str(foreign)}], "cmsis")

    def test_changed_leased_input_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input"
            path.write_text("before")
            lease = {str(path): target.sha(path)}
            target.check_lease(lease)
            path.write_text("after")
            with self.assertRaisesRegex(ValueError, "input changed"):
                target.check_lease(lease)

    def test_cmsis_cannot_regenerate_reference_goldens(self):
        with mock.patch("sys.argv", ["qualify", "--regenerate-goldens",
                                    "--cmsis-nn-dir", "/supplied/NN",
                                    "--cmsis-core-include", "/supplied/Core/Include"]), \
             mock.patch.object(target, "build_profile", side_effect=AssertionError("must not build")), \
             self.assertRaisesRegex(ValueError, "only reference"):
            target.main()

    def test_cli_propagates_controller_failure_with_cleanup_grace(self):
        with mock.patch.object(test_cli, "_run_owned_test_process", return_value=1) as run:
            result = test_cli.test_infer_target("/ove", "/out")
        self.assertEqual((result.passed, result.failed), (0, 1))
        self.assertEqual(run.call_args.args[0][1], "/ove/tests/infer_target/run.py")
        self.assertGreater(run.call_args.kwargs["timeout"], 45)
        self.assertGreater(run.call_args.kwargs["cleanup_timeout"], 5)

    def test_new_shipping_provider_in_apps_or_ci_requires_new_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(target.shipped_providers(root), ["reference"])
            for name in ("apps/model/app.yaml", ".github/workflows/extra.yml"):
                path = root / name
                path.parent.mkdir(parents=True)
                path.write_text("CONFIG_OVE_INFER_CMSIS_NN=y\n")
                with self.assertRaisesRegex(ValueError, "extending the mandatory lane"):
                    target.shipped_providers(root)
                path.unlink()

    def test_new_qualification_apps_are_discovered(self):
        parent = ROOT / "output/tests/infer_target_oracles"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as directory:
            root = Path(directory)
            for mode in ("heap", "zeroheap"):
                app = root / "tests/infer_target" / mode
                app.mkdir(parents=True)
                (app / "app.yaml").write_bytes((ROOT / "tests/infer_target" / mode / "app.yaml").read_bytes())
            with mock.patch.dict("os.environ", {"OVE_EXTERNAL_APPS": ""}):
                appgen.generate_app_kconfig(directory)
            paths = json.loads((root / "output/kconfig/app_paths.json").read_text())
            self.assertEqual(set(paths), {"infer_target_heap", "infer_target_zh"})

    def test_switching_cmake_entry_discards_only_its_owned_build_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "firmware"
            build.mkdir()
            (build / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/old\n")
            (build / "stale.o").write_text("stale")
            preserved = root / "other"
            preserved.write_text("preserve")
            _prepare_freertos_build_dir(str(build), "hard", "/new")
            self.assertTrue(build.is_dir())
            self.assertFalse((build / "stale.o").exists())
            self.assertEqual(preserved.read_text(), "preserve")


if __name__ == "__main__":
    unittest.main()
