# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import unittest
from pathlib import Path
from shlex import quote
from tempfile import TemporaryDirectory

from ove import test as ove_test


def _run_fake_binary(output, returncode, **kwargs):
    with TemporaryDirectory() as directory:
        binary = Path(directory) / "test-suite"
        binary.write_text(
            f"#!/bin/sh\nprintf '%s' {quote(output)}\nexit {returncode}\n",
            encoding="utf-8",
        )
        binary.chmod(0o700)
        return ove_test._run_test_binary([binary], "stub", **kwargs)


class StubRunnerTest(unittest.TestCase):
    def test_missing_extra_binary_is_a_failure(self):
        results = ove_test.TestResults(suite="stub")
        with TemporaryDirectory() as build:
            ove_test._run_extra_binaries(build, ("ove_test_nvs_full",), "stub", results)

        self.assertEqual(results.failed, 1)
        self.assertEqual(results.failed_names, ["<missing ove_test_nvs_full>"])

    def test_silent_binary_with_required_summary_fails(self):
        results = _run_fake_binary("", 0, summary=True)
        self.assertEqual(results.passed, 0)
        self.assertEqual(results.failed, 1)
        self.assertIn("<no summary line>", results.failed_names)

    def test_partial_cmocka_output_with_required_summary_fails(self):
        results = _run_fake_binary("[  PASSED  ] 3 test(s).\n", 0, summary=True)
        self.assertEqual(results.passed, 3)
        self.assertEqual(results.failed, 1)

    def test_c_summary_completes_run(self):
        results = _run_fake_binary(
            "[  PASSED  ] 3 test(s).\n\n=== Summary: 0 test group(s) had failures ===\n",
            0, summary=True,
        )
        self.assertEqual(results.failed, 0)

    def test_rust_zig_summary_completes_run(self):
        results = _run_fake_binary("=== Summary: 5 passed, 0 failed ===\n", 0, summary=True)
        self.assertEqual(results.failed, 0)

    def test_silent_binary_without_required_summary_passes(self):
        results = _run_fake_binary("", 0)
        self.assertEqual(results.passed, 1)
        self.assertEqual(results.failed, 0)
