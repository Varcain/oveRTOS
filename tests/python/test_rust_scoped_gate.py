# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""The gate must reject lost early failures and partial ownership observations."""
import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ove import test as ove_test
from ove.rust_scoped import CANARIES, cases, expected, observation_valid


class ScopedRustGateTest(unittest.TestCase):
    def test_closed_profiles_and_semantic_fields(self):
        for zero in (False, True):
            for optional in (False, True):
                names = cases(zero, optional)
                self.assertEqual(len(names), len(set(names)))
                self.assertEqual("value_into_inner" in names, not zero)
                self.assertEqual("actual_masked_release" in names, optional)
                for name in names:
                    rc, text = expected(name, zero, optional)
                    self.assertTrue(observation_valid(text, name, zero, optional, rc))
                    for field in ("checks", "init", "deinit", "locks", "unlocks", "duplicates",
                                  "forbidden", "retirements", "retained", "harness_cleanup"):
                        prefix, tail = text.rsplit(" " + field + "=", 1)
                        _value, *rest = tail.split(" ", 1)
                        altered = prefix + " " + field + "=999" + (" " + rest[0] if rest else "\n")
                        self.assertFalse(observation_valid(altered, name, zero, optional, rc))
                    for altered in (text.rstrip(), text + text, "extra\n" + text, text + "trailing\n"):
                        self.assertFalse(observation_valid(altered, name, zero, optional, rc))
                    self.assertFalse(observation_valid(text, name, zero, optional, 1))

    def test_persistent_canary_rejects_reset_even_with_successful_status(self):
        for name in CANARIES:
            rc, text = expected(name, False, False)
            self.assertTrue(observation_valid(text, name, False, False, rc, expected_failure=True))
            self.assertFalse(observation_valid(text, name, False, False, rc))
            reset = text.replace("failures=1", "failures=0")
            self.assertFalse(observation_valid(reset, name, False, False, 0, expected_failure=True))
            self.assertFalse(observation_valid(text, name, False, False, 0, expected_failure=True))

    def test_unknown_case_has_no_observation(self):
        self.assertFalse(observation_valid("", "unknown", False, False, 0))

    def test_cli_records_reset_as_failure(self):
        name = CANARIES[0]
        _, text = expected(name, False, False)
        reset = text.replace("failures=1", "failures=0").encode()
        with tempfile.TemporaryDirectory() as folder:
            def launch(command, **kwargs):
                if command[0] == "cmake" and "-B" in command:
                    Path(command[command.index("-B") + 1]).mkdir(parents=True, exist_ok=True)
                stream = kwargs.get("stdout")
                if stream is not None:
                    stream.write(reset)
                return 0
            with (mock.patch.object(ove_test, "_run_owned_test_process", side_effect=launch),
                  mock.patch.object(ove_test, "_scoped_private_units", return_value=(0, 0)),
                  mock.patch.object(ove_test, "_scoped_cases", return_value=()),
                  mock.patch.object(ove_test, "_scoped_canaries", (name,)),
                  contextlib.redirect_stdout(io.StringIO()) as output):
                result = ove_test.test_rust_scoped(str(Path(folder)), str(Path(folder)))
            self.assertEqual(result.passed, 1)
            self.assertEqual(result.failed, 4)
            self.assertEqual(output.getvalue().count("rc=0"), 4)


if __name__ == "__main__":
    unittest.main()
