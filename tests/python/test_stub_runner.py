# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import os
import re
import subprocess
import time
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from shlex import quote
from tempfile import TemporaryDirectory
from unittest import mock

from ove import test as ove_test
from ove.test import NUTTX_SANITIZER_REPORT


_ZERO_FAILURE_SUMMARY = "=== Summary: 0 test group(s) had failures ===\n"
_PASSING_OUTPUT = "[  PASSED  ] 3 test(s).\n" + _ZERO_FAILURE_SUMMARY


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
    def test_missing_clang_skips_msan_locally(self):
        with (
            TemporaryDirectory() as directory,
            mock.patch.dict(os.environ, clear=True),
            mock.patch.object(ove_test.shutil, "which", return_value=None),
            mock.patch.object(ove_test, "_cmake_build") as build,
        ):
            results = ove_test.test_stub_msan(directory, Path(directory) / "output")

        self.assertEqual(results, ove_test.TestResults(suite="stub-msan", skipped=1))
        build.assert_not_called()

    def test_missing_clang_fails_msan_in_ci(self):
        with (
            TemporaryDirectory() as directory,
            mock.patch.dict(os.environ, {"CI": "true"}),
            mock.patch.object(ove_test.shutil, "which", return_value=None),
            mock.patch.object(ove_test, "_cmake_build") as build,
        ):
            results = ove_test.test_stub_msan(directory, Path(directory) / "output")

        self.assertEqual(results, ove_test.TestResults(
            suite="stub-msan", failed=1, failed_names=["<clang not installed>"],
        ))
        build.assert_not_called()

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

    def test_kasan_report_fails_despite_successful_summary(self):
        results = _run_fake_binary(
            "kasan_report: kasan detected a read access error\n" + _PASSING_OUTPUT,
            0, summary=True, fail_on=NUTTX_SANITIZER_REPORT,
        )
        self.assertEqual(results.failed, 1)
        self.assertIn("<sanitizer report>", results.failed_names)

    def test_ubsan_report_fails_despite_successful_summary(self):
        results = _run_fake_binary(
            "UBSAN: add-overflow in foo\n" + _PASSING_OUTPUT,
            0, summary=True, fail_on=re.compile(NUTTX_SANITIZER_REPORT),
        )
        self.assertEqual(results.failed, 1)
        self.assertIn("<sanitizer report>", results.failed_names)

    def test_clean_output_passes_with_sanitizer_check(self):
        results = _run_fake_binary(
            _PASSING_OUTPUT, 0, summary=True, fail_on=NUTTX_SANITIZER_REPORT,
        )
        self.assertEqual(results.failed, 0)

    def test_silent_binary_without_required_summary_passes(self):
        results = _run_fake_binary("", 0)
        self.assertEqual(results.passed, 1)
        self.assertEqual(results.failed, 0)

    def test_binary_exit_124_is_not_a_runner_timeout(self):
        results = _run_fake_binary("[ RUN      ] test_exits\n", 124)
        self.assertEqual(results.failed, 1)
        self.assertEqual(results.failed_names, ["<exit 124>"])

    def test_binary_timeout_preserves_output_and_reaps_process(self):
        script = r'printf "%s\n" "$$" "[ RUN      ] test_hangs" >&2; exec sleep 60'
        start = time.monotonic()
        with redirect_stdout(StringIO()) as stdout:
            results = ove_test._run_test_binary(["sh", "-c", script], "stub", timeout=1)

        self.assertLess(time.monotonic() - start, 5)
        self.assertEqual(results.failed, 1)
        self.assertEqual(results.failed_names, ["<timeout after 1 s>"])
        pid, run_line = stdout.getvalue().splitlines()
        self.assertEqual(run_line, "[ RUN      ] test_hangs")
        with self.assertRaises(ProcessLookupError):
            os.kill(int(pid), 0)

    def test_binary_uses_default_deadline(self):
        with mock.patch.object(ove_test, "_run_owned_test_process", return_value=0) as run:
            _run_fake_binary("", 0)

        run.assert_called_once()
        self.assertEqual(run.call_args.kwargs["timeout"], 600)


class NuttxSimRunnerTest(unittest.TestCase):
    def test_sanitizer_overlay_and_environment(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            apps = root / "nuttx-apps"
            apps.mkdir()
            output = root / "output"
            with (
                mock.patch.dict(os.environ, {"PATH": "/bin", "ASAN_OPTIONS": "old"}, clear=True),
                mock.patch.object(ove_test, "_nuttx_fetch_sources", return_value=("nuttx", apps, None)),
                mock.patch.object(ove_test, "_nuttx_register_test_app"),
                mock.patch.object(ove_test, "_nuttx_cmake_build") as build,
                mock.patch.object(ove_test.subprocess, "run") as run,
                mock.patch("sys.stdout"),
                mock.patch("sys.stderr") as stderr,
            ):
                run.side_effect = subprocess.TimeoutExpired([], 60, output=_PASSING_OUTPUT.encode())
                self.assertEqual(ove_test.test_nuttx(root, output).failed, 0)
                run.side_effect = subprocess.TimeoutExpired(
                    [], 60, output=_PASSING_OUTPUT.encode(), stderr=b"simulator stderr\n",
                )
                result = ove_test.test_nuttx_sanitize(root, output)

        overlays = root / "tests" / "sim" / "nuttx"
        base = str(overlays / "nuttx_sim_defconfig")
        plain_build, sanitized_build = build.call_args_list
        self.assertEqual(plain_build.kwargs["defconfig_overlays"], [base])
        self.assertEqual(sanitized_build.kwargs["defconfig_overlays"], [
            base, str(overlays / "nuttx_sim_asan_defconfig"),
        ])
        plain_run, sanitized_run = run.call_args_list
        self.assertIsNone(plain_run.kwargs.get("env"))
        self.assertEqual(sanitized_run.kwargs["env"], {
            "PATH": "/bin",
            "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=0",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        })
        self.assertEqual(result, ove_test.TestResults(suite="nuttx-sim-sanitize", passed=3))
        stderr.write.assert_any_call("simulator stderr\n")

    def test_sim_exit_after_summary_fails(self):
        for rc, stderr in ((-6, "ERROR: AddressSanitizer: heap-use-after-free\n"), (0, "")):
            with self.subTest(returncode=rc):
                result = self._run_sanitize(return_value=subprocess.CompletedProcess(
                    ["nuttx"], rc, _PASSING_OUTPUT, stderr,
                ))
                self.assertGreaterEqual(result.failed, 1)
                self.assertIn(f"<sim exited: rc {rc}>", result.failed_names)

    def test_sim_timeout_after_summary_passes(self):
        result = self._run_sanitize(side_effect=subprocess.TimeoutExpired(
            ["nuttx"], 60, output=_PASSING_OUTPUT.encode(),
        ))
        self.assertEqual(result.failed, 0)

    def _run_sanitize(self, **run_kwargs):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            apps = root / "nuttx-apps"
            apps.mkdir()
            with (
                mock.patch.object(ove_test, "_nuttx_fetch_sources", return_value=("nuttx", apps, None)),
                mock.patch.object(ove_test, "_nuttx_register_test_app"),
                mock.patch.object(ove_test, "_nuttx_cmake_build"),
                mock.patch.object(ove_test.subprocess, "run", **run_kwargs),
                mock.patch("sys.stdout"),
                mock.patch("sys.stderr"),
            ):
                return ove_test.test_nuttx_sanitize(root, root / "output")


class RenodeRunnerTest(unittest.TestCase):
    def test_renode_unavailable(self):
        cases = (
            ({"CI": "true"}, ove_test.TestResults(
                suite="renode", failed=1, failed_names=["<Renode unavailable>"],
            )),
            ({}, ove_test.TestResults(suite="renode", skipped=1)),
        )
        with TemporaryDirectory() as directory, mock.patch.object(
            ove_test, "_ensure_renode", return_value=None,
        ):
            for env, expected in cases:
                with self.subTest(ci=bool(env)), mock.patch.dict(os.environ, env, clear=True):
                    result = ove_test._renode_run_elf(
                        directory, Path(directory) / "output", label="renode",
                        elf="test.elf", resc="test.resc",
                    )
                    self.assertEqual(result, expected)

    def test_renode_counts_tests_in_completed_run(self):
        cases = (
            ("", ove_test.TestResults(suite="renode", failed=1, failed_names=["<no tests ran>"])),
            ("[  PASSED  ] 1 test(s).\n", ove_test.TestResults(suite="renode", passed=1)),
        )
        with TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            uart = output / "tests" / "renode" / "uart.log"
            uart.parent.mkdir(parents=True)

            for pass_record, expected in cases:
                def run_renode(cmd, **kwargs):
                    uart.write_text(pass_record + _ZERO_FAILURE_SUMMARY, encoding="utf-8")
                    return subprocess.CompletedProcess(cmd, 0)

                with (
                    self.subTest(passed=expected.passed),
                    mock.patch.object(ove_test, "_ensure_renode", return_value="renode"),
                    mock.patch.object(ove_test.subprocess, "run", side_effect=run_renode),
                    redirect_stdout(StringIO()),
                ):
                    result = ove_test._renode_run_elf(
                        directory, output, label="renode", elf="test.elf", resc="test.resc",
                    )
                    self.assertEqual(result, expected)


class ZephyrSimRunnerTest(unittest.TestCase):
    def test_native_sim_partial_run_fails(self):
        def run_binary(cmd, *, stdout, **kwargs):
            stdout.write("[  PASSED  ] 1 test(s).\n")
            return 0

        manifest = {"rtos": {"zephyr": {"version": "test-rev", "url": "unused"}}}
        with (
            TemporaryDirectory() as directory,
            mock.patch.object(ove_test, "_require_zephyr_python"),
            mock.patch.object(ove_test, "load_manifest", return_value=manifest),
            mock.patch.object(ove_test, "run"),
            mock.patch.object(ove_test, "_run_owned_test_process", side_effect=run_binary),
            redirect_stdout(StringIO()),
        ):
            result = ove_test.test_zephyr(directory, Path(directory) / "output")

        self.assertEqual(result, ove_test.TestResults(
            suite="zephyr-native-sim", passed=1, failed=1, failed_names=["<no summary line>"],
        ))


class LvglCompileRunnerTest(unittest.TestCase):
    def test_lvgl_header_missing(self):
        with TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            with self.subTest(ci=True), mock.patch.dict(os.environ, {"CI": "true"}):
                result = ove_test.test_lvgl_bindings_compile(directory, output)
                self.assertEqual(result, ove_test.TestResults(
                    suite="lvgl-compile", failed=1, failed_names=["<missing dl/lvgl/lvgl.h>"],
                ))

            with (
                self.subTest(ci=False),
                mock.patch.dict(os.environ, clear=True),
                self.assertLogs("ove", level="WARNING") as logs,
            ):
                result = ove_test.test_lvgl_bindings_compile(directory, output)
                self.assertEqual(result, ove_test.TestResults(suite="lvgl-compile", skipped=3))
                self.assertIn("skipping", logs.output[0])
