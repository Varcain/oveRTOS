# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Benchmark names retain the API that produced each measurement."""

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
path = Path(os.environ.get("OVE_BENCH_COMPARE_SCRIPT", ROOT / "scripts/bench_compare.py"))
spec = importlib.util.spec_from_file_location("scoped_bench_compare", path)
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)
sys.path.pop(0)


class BenchmarkApiLabelsTest(unittest.TestCase):
    def report(self, name):
        suites = [
            {"binding": "rust", "rtos": "FreeRTOS", "suite": "sync",
             "cases": [{"name": name, "avg_ns": 200}]},
            {"binding": "rust", "rtos": "FreeRTOS", "suite": "native_freertos",
             "cases": [{"name": "native_mutex_lock_unlock", "avg_ns": 150}]},
        ]
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "stdout.txt"
            source.write_text("".join("###BENCH_JSON_BEGIN\n" + json.dumps(suite)
                                      + "\n###BENCH_JSON_END\n" for suite in suites))
            data = compare.load_inputs([source])
        return data, compare.build_report(data, 10)

    def test_legacy_records_keep_their_measured_guard_api(self):
        data, report = self.report("mutex_lock_unlock")
        self.assertEqual(data["FreeRTOS"]["rust"][("sync", "mutex_lock_unlock")]["avg_ns"], 200)
        self.assertIn("ove::Mutex::lock+unlock", report)
        self.assertNotIn("ove::Mutex::with_lock", report)

    def test_scoped_records_join_the_native_baseline_with_the_new_label(self):
        data, report = self.report("mutex_with_lock")
        self.assertIn(("sync", "mutex_lock_unlock"), data["FreeRTOS"]["rust"])
        self.assertIn("ove::Mutex::with_lock", report)
        self.assertNotIn("ove::Mutex::lock+unlock", report)
        self.assertIn("200 ns | 150 ns | +50 ns", report)


if __name__ == "__main__":
    unittest.main()
