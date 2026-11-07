# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject lost failures, incomplete cleanup observations and incorrect statuses."""

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ove import test as ove_test
from ove.rust_sync import FAMILIES, cases, expected, observation_valid


class RustSyncGateTest(unittest.TestCase):
    def test_closed_feature_and_family_matrix(self):
        total = 0
        for zero in (False, True):
            for optional in (False, True):
                for family in FAMILIES:
                    names = cases(family, zero, optional)
                    self.assertEqual(len(names), len(set(names)))
                    self.assertEqual("raw_origin" in names, zero)
                    self.assertEqual("deadline_isr" in names, optional)
                    self.assertEqual("notify" in names, optional and family == "semaphore")
                    self.assertEqual("actual_waker" in names, optional and family == "event")
                    total += len(names) + 1
        self.assertEqual(total, 456)

    def test_complete_transcript_and_native_counters_are_required(self):
        code, text = expected("semaphore", "normal", False, False)
        self.assertEqual(code, 0)
        self.assertEqual(text, (
            "sync heap/semaphore normal: checks=17 failures=0 retained=0 cleanup=0 workers=0,0\n"
            "native before: [2, 2, 2, 2, 0, 0, 2, 2, 4, 2, 0, 0, 0]\n"
            "native after: [2, 2, 2, 2, 0, 0, 2, 2, 4, 2, 0, 0, 0]\n"
            "clock: calls=0 forbidden=0\n"))
        for family in FAMILIES:
            for zero in (False, True):
                for optional in (False, True):
                    for name in cases(family, zero, optional):
                        code, text = expected(family, name, zero, optional)
                        self.assertTrue(observation_valid(text, family, name, zero, optional, code))
                        for altered in (text.rstrip(), text + text, "unexpected\n" + text,
                                        text + "runtime error: invalid state\n",
                                        text.replace("failures=0", "failures=1"),
                                        text.replace("workers=0,0", "workers=0,1").replace("workers=1,1", "workers=1,0")):
                            self.assertFalse(observation_valid(altered, family, name, zero, optional, code))
                        for status in (1, 2, -9, 124):
                            self.assertFalse(observation_valid(text, family, name, zero, optional, status))
                        lines = text.splitlines(keepends=True)
                        for index in (1, 2):
                            prefix, array = lines[index].split(": ", 1)
                            values = json.loads(array)
                            for counter in range(13):
                                changed = list(values)
                                changed[counter] += 1
                                changed_lines = list(lines)
                                changed_lines[index] = prefix + ": " + str(changed) + "\n"
                                self.assertFalse(observation_valid("".join(changed_lines), family, name, zero, optional, code))

    def test_canary_requires_early_line_live_failure_and_nonzero_status(self):
        for family in FAMILIES:
            code, text = expected(family, "canary", True, True)
            self.assertTrue(observation_valid(text, family, "canary", True, True, code, expected_failure=True))
            self.assertFalse(observation_valid(text, family, "canary", True, True, code))
            for changed, status in ((text, 0), (text.replace("failures=1", "failures=0"), 0),
                                    (text.replace("failures=1", "failures=0"), 1),
                                    (text.split("\n", 1)[1], 1)):
                self.assertFalse(observation_valid(changed, family, "canary", True, True, status, expected_failure=True))
        self.assertFalse(observation_valid("", "unknown", "normal", False, False, 0))
        self.assertFalse(observation_valid("", "event", "notify", False, True, 0))
        self.assertFalse(observation_valid("", "semaphore", "raw_origin", False, True, 0))

    def test_cli_records_actual_canary_status_and_rejects_reset(self):
        for reset in (False, True):
            with self.subTest(reset=reset), tempfile.TemporaryDirectory() as folder:
                def launch(command, *, reset=reset, **kwargs):
                    if command[0] == "cmake" and "-B" in command:
                        Path(command[command.index("-B") + 1]).mkdir(parents=True, exist_ok=True)
                    stream = kwargs.get("stdout")
                    if stream is not None:
                        profile = Path(command[0]).parents[2].name
                        code, text = expected(command[1], command[2], "zero" in profile, "optional" in profile)
                        if reset:
                            text = text.replace("failures=1", "failures=0")
                            code = 0
                        stream.write(text.encode())
                        return code
                    return 0

                with (mock.patch.dict(os.environ, {"OVE_RUST_SYNC_CANARY": "0"}),
                      mock.patch.object(ove_test, "_run_owned_test_process", side_effect=launch),
                      mock.patch.object(ove_test, "_rust_sync_cases", return_value=()),
                      contextlib.redirect_stdout(io.StringIO()) as output):
                    result = ove_test.test_rust_sync(folder, folder)
                self.assertEqual((result.passed, result.failed), (1, 8) if reset else (9, 0))
                self.assertEqual(output.getvalue().count("rc=0"), 8 if reset else 0)
                sidecars = list(Path(folder).rglob("*.status.json"))
                self.assertEqual(len(sidecars), 8)
                self.assertTrue(all(json.loads(path.read_text())["status"] == int(not reset) for path in sidecars))


if __name__ == "__main__":
    unittest.main()
