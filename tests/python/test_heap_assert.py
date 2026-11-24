# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADERS = "#include <cstdlib>\n#include <ove/ove.h>\n"


class HeapAssertTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("clang++"), "no host clang++")
    def test_clang(self):
        self.check_allocator_guard("clang++")

    @unittest.skipUnless(shutil.which("g++"), "no host g++")
    def test_gcc(self):
        self.check_allocator_guard("g++")

    def check_allocator_guard(self, compiler):
        cmd = [
            compiler, "-std=c++17", "-DCONFIG_OVE_ZERO_HEAP=1", "-Werror",
            "-I" + str(ROOT / "include"),
            "-I" + str(ROOT / "tests"),
            "-I" + str(ROOT / "tests/host/posix/include"),
            "-O0", "-x", "c++", "-c", "-", "-o", os.devnull,
        ]
        result = subprocess.run(
            cmd,
            input=HEADERS + "#include <memory>\n"
            "void release(void *p) { std::free(p); ::free(nullptr); }\n"
            "int *address(int &value) { return std::addressof(value); }\n",
            capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

        calls = ["malloc(17)"]
        # Clang's guard misses std:: names imported by <cstdlib> before it.
        if compiler == "g++":
            calls.extend(("std::malloc(17)", "std::calloc(1, 17)",
                          "std::realloc(p, 17)", "std::aligned_alloc(16, 32)"))
        for call in calls:
            with self.subTest(call=call):
                result = subprocess.run(
                    cmd, input=HEADERS + f"void *allocate(void *p) {{ return {call}; }}\n",
                    capture_output=True, text=True, timeout=10,
                )
                name = call.split("(")[0].rsplit("::", 1)[-1]
                self.assertNotEqual(result.returncode, 0, f"{call} compiled")
                self.assertIn(f"zero-heap mode forbids libc {name}()", result.stderr)
