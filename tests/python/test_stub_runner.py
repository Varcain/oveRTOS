# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import unittest
from tempfile import TemporaryDirectory

from ove import test as ove_test


class StubRunnerTest(unittest.TestCase):
    def test_missing_extra_binary_is_a_failure(self):
        results = ove_test.TestResults(suite="stub")
        with TemporaryDirectory() as build:
            ove_test._run_extra_binaries(build, ("ove_test_nvs_full",), "stub", results)

        self.assertEqual(results.failed, 1)
        self.assertEqual(results.failed_names, ["<missing ove_test_nvs_full>"])
