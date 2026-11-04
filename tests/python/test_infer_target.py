# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Target qualification must reject incomplete or mismatched guest output."""

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ove import appgen
from ove import infer_target as target
from ove.build import _prepare_freertos_build_dir

ROOT = Path(__file__).resolve().parents[2]
GOLDENS = {str(seed): {"features": ["00" * 40] * 49, "scores": "00" * 4} for seed in target.SEEDS}


def complete_log(zero_heap=True):
    lines = []
    for mode in ("init",) if zero_heap else ("init", "create"):
        lines.append("IT_BEGIN " + mode)
        for seed in target.SEEDS:
            lines.extend(f"IT_FRAME {seed} {frame} " + "00" * 40 for frame in range(49))
            lines.extend([f"IT_ARENA {seed} preprocessor 32768 20000 0",
                          f"IT_SCORES {seed} " + "00" * 4,
                          f"IT_ARENA {seed} classifier 32768 21000 0"])
        lines.append("IT_END")
    return "\n".join(lines + ["IT_RESULT 0"]) + "\n"


class TargetOutputTest(unittest.TestCase):
    def test_complete_output_matches_and_can_be_captured(self):
        for zero in (False, True):
            arenas = target.check_output(complete_log(zero), GOLDENS, zero_heap=zero)
            self.assertEqual(len(arenas), 4 if zero else 8)
        self.assertEqual(target.capture(complete_log()), GOLDENS)

    def test_changed_missing_extra_or_failed_records_are_rejected(self):
        good = complete_log()
        last = "IT_FRAME 17 48 " + "00" * 40 + "\n"
        bad = [good.replace(last, ""), good.replace(last, last * 2), good + "failure\n",
               good.replace("IT_FRAME 0 0 00", "IT_FRAME 0 0 01"),
               good.replace("IT_SCORES 17 00", "IT_SCORES 17 01"),
               good.replace("IT_RESULT 0", "IT_RESULT 1"),
               good.replace("classifier 32768 21000 0", "classifier 16384 21000 0"),
               good.replace("classifier 32768 21000 0", "classifier 32768 21000 8")]
        for text in bad:
            with self.subTest(text=text[-80:]), self.assertRaises(ValueError):
                target.check_output(text, GOLDENS, zero_heap=True)


class TargetBuildTest(unittest.TestCase):
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
