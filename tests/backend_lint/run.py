# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check backend lint commands using compiler assertions and real Clang analysis."""

import argparse
import json
import os
import re
import shutil
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "config/ove-cli"))
from ove import lint
from ove.test import _run_owned_test_process

OUTPUT = None


class BackendLint(unittest.TestCase):
    def setUp(self):
        self.folder = OUTPUT / self.id().rsplit(".", 1)[-1]
        self.folder.mkdir(parents=True, exist_ok=True)
        self.root = self.folder / "fixture"
        self.build = self.root / "output/tests/zephyr/build"
        self.build.mkdir(parents=True, exist_ok=True)
        self.source = self.root / "backends/zephyr/probe.c"
        self.source.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / ".clang-tidy", self.root / ".clang-tidy")
        self.calls = []
        self.serial = 0

    def invoke(self, source, flags=(), *, compiler="gcc", relative=False,
               command=False, edit=None):
        self.source.write_text(source)
        name = os.path.relpath(self.source, self.build) if relative else str(self.source)
        entry = {"directory": str(self.build), "file": name,
                     "arguments": [compiler, "-std=c11", *flags, "-c", name]}
        if command:
            import shlex
            entry["command"] = shlex.join(entry.pop("arguments"))
        if edit:
            edit(entry)
        original = self.build / "compile_commands.json"
        original.write_text(json.dumps([entry]))
        before = original.read_bytes()

        def run(args, cwd=None):
            log = self.folder / ("clang-tidy-" + str(self.serial) + ".log")
            self.serial += 1
            with log.open("wb") as stream:
                status = _run_owned_test_process(args, cwd=cwd, env=dict(os.environ),
                                                 timeout=30, stdout=stream)
            text = log.read_text(errors="backslashreplace")
            self.calls.append({"command": args, "status": status, "output": text})
            return status, text

        with patch.object(lint, "_run", run):
            result = lint._clang_tidy_backends(str(self.root), False)
        self.assertEqual(original.read_bytes(), before)
        (self.folder / "result.json").write_text(json.dumps({"result": result, "calls": self.calls}, indent=2))
        return result

    def positive(self, *args, **kwargs):
        result = self.invoke(*args, **kwargs)
        self.assertEqual(result[1], "OK", result)
        self.assertEqual(len(self.calls), 1, result)
        self.assertEqual(self.calls[0]["status"], 0, self.calls)

    def negative(self, *args, diagnostic=None, **kwargs):
        result = self.invoke(*args, **kwargs)
        self.assertEqual(result[1], "FAIL", result)
        if diagnostic:
            self.assertIn(diagnostic, result[2] + "\n" + "\n".join(c["output"] for c in self.calls))

    def test_native_hosted_layout(self):
        self.positive('''
_Static_assert(sizeof(void *) == 8 && sizeof(long) == 8, "LP64 layout");
_Static_assert(__STDC_HOSTED__ == 1, "hosted");
_Static_assert(__x86_64__ && __LP64__, "native macros");
#ifdef __arm__
#error wrong architecture
#endif
#ifdef __SOFTFP__
#error ARM float ABI on native
#endif
struct probe { char first; long second; };
_Static_assert(__builtin_offsetof(struct probe, second) == 8, "offset");
_Static_assert(sizeof(_Atomic(long)) == 8, "atomic layout");
_Static_assert(sizeof(long double) == 16 && _Alignof(long double) == 16, "native long double");
void probe(void) { __asm__ volatile("" ::: "rax"); }
''', ["-m64", "-ffreestanding", "-fno-freestanding"])
        filtered = self.root / "output/tests/lint_backend_dbs/zephyr/compile_commands.json"
        arguments = json.loads(filtered.read_text())[0]["arguments"]
        # Clang18 can ignore an ARM float flag on x86. Check this input contract
        # as well as the compiler-observed native layout and macro assertions.
        self.assertFalse(any(a.startswith("-mfloat-abi=") for a in arguments))

    def test_native_without_width_preserves_undefines(self):
        self.positive('''
_Static_assert(sizeof(void *) == 8 && sizeof(long) == 8, "native LP64");
#if defined(__linux__) || defined(__unix__)
#error unexpected OS macro
#endif
int probe(void) { return 0; }
''', ["-U__linux__", "-U__unix__"], compiler="cc", command=True)

    def test_freestanding_order(self):
        self.positive('_Static_assert(__STDC_HOSTED__ == 0, "freestanding");',
                      ["-fno-freestanding", "-ffreestanding", "-fno-builtin"])

    def test_relative_source(self):
        self.positive('_Static_assert(sizeof(void *) == 8, "native");', relative=True)

    def test_arm_float_abis(self):
        for abi in ("default", "soft", "softfp", "hard"):
            with self.subTest(abi=abi):
                self.calls = []
                extra = [] if abi == "default" else ["-mfloat-abi=" + abi]
                soft = abi in ("default", "soft")
                self.positive('''
_Static_assert(sizeof(void *) == 4 && sizeof(long) == 4, "ARM layout");
_Static_assert(sizeof(long double) == 8, "ARM long double");
_Static_assert(__arm__ && __thumb__, "ARM macros");
_Static_assert(sizeof(__fp16) == 2, "half layout");
_Static_assert(__ARM_FP16_FORMAT_IEEE == 1, "IEEE half format");
''' + ("#ifndef __SOFTFP__\n#error missing soft ABI\n#endif\n" if soft else
       "#ifdef __SOFTFP__\n#error wrong soft ABI\n#endif\n") +
       ("#ifndef __ARM_PCS_VFP\n#error missing hard ABI\n#endif\n" if abi == "hard" else
        "#ifdef __ARM_PCS_VFP\n#error wrong hard ABI\n#endif\n"),
                    ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mtp=soft",
                     "-mfp16-format=ieee", *extra], compiler="arm-none-eabi-gcc")
                (self.folder / (abi + ".json")).write_text(json.dumps(self.calls, indent=2))

    def test_single_precision_types_and_suffixes(self):
        self.positive('''
#define TYPE(x, t) _Generic((x), t: 1, default: 0)
_Static_assert(TYPE(1.0, float) && TYPE(0x1.2p0, float), "literal types");
_Static_assert(sizeof(1.0) == sizeof(float), "literal size");
_Static_assert(TYPE(1.0f, float) && TYPE(1.0F, float), "float suffixes");
_Static_assert(TYPE(1.0l, long double) && TYPE(1.0L, long double), "long suffixes"); // NOLINT(cert-dcl16-c)
_Static_assert(0.5 == 0.5f && 0x1.8p1 == 3.0f, "exact values");
''', ["-fsingle-precision-constant"])

    def test_single_precision_option_order(self):
        for active in (False, True):
            with self.subTest(active=active):
                self.calls = []
                flags = ["-fsingle-precision-constant", "-fno-single-precision-constant"]
                if active:
                    flags.reverse()
                self.positive('_Static_assert(_Generic(1.0, ' +
                              ("float" if active else "double") + ': 1, default: 0), "option order");', flags)

    def test_single_precision_rounding_limit(self):
        # Clang18 casts a parsed double to float; GCC rounds directly to float.
        # These two boundaries intentionally record Clang's known limitation.
        self.positive('''
_Static_assert(_Generic(1.0, float: 1, default: 0), "literal type");
_Static_assert(0x1.00000100000001p0 == 1.0f, "Clang hex rounding limit");
_Static_assert(1.00000005960464477539062501 == 1.0f, "Clang decimal rounding limit");
''', ["-fsingle-precision-constant"])

    def test_include_precedence(self):
        vendor = self.root / "dl/vendor"
        own = self.root / "include"
        for directory, value in ((vendor, 111), (own, 222)):
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "choice.h").write_text("#define CHOICE " + str(value) + "\n")
        for split in (False, True):
            with self.subTest(split=split):
                self.calls = []
                flags = ["-I", str(vendor), "-I", str(own)] if split else ["-I" + str(vendor), "-I" + str(own)]
                self.positive('#include <choice.h>\n_Static_assert(CHOICE == 111, "include precedence");', flags)

    def test_replace_only_gcc_builtin_headers(self):
        private = self.root / "lib/gcc/arm-none-eabi/15.2.1/include"
        private.mkdir(parents=True, exist_ok=True)
        (private / "stdatomic.h").write_text('#error GCC private header retained\n')
        self.positive('''
#include <stdatomic.h>
_Atomic(int) value;
int probe(void) { atomic_store(&value, 3); return atomic_load(&value); }
''', ["-nostdinc", "-ffreestanding", "-isystem", str(private)], compiler="arm-none-eabi-gcc")

    def test_deliberate_nostdinc_stays_closed(self):
        self.negative('#include <stddef.h>\n', ["-nostdinc"], diagnostic="'stddef.h' file not found")

    def test_own_source_diagnostic(self):
        self.negative('int probe(void) { int value; return value; }', diagnostic="clang-analyzer-core.uninitialized.UndefReturn")

    def test_own_header_diagnostic(self):
        header = self.source.parent / "bad.h"
        header.write_text('static inline int bad(void) { int value; return value; }\n')
        self.negative('#include "bad.h"\nint probe(void) { return bad(); }',
                      diagnostic="clang-analyzer-core.uninitialized.UndefReturn")

    def test_unsupported_target(self):
        for compiler, flags in (("riscv64-unknown-elf-gcc", []),
                                ("gcc", ["--target="]),
                                ("clang", ["--target=aarch64-none-elf"]),
                                ("ccache", ["gcc"]),
                                ("gcc", ["@options.rsp"])):
            with self.subTest(compiler=compiler, flags=flags):
                self.calls = []
                self.negative('int probe(void) { return 0; }', flags,
                              compiler=compiler, diagnostic="unsupported")

    def test_incompatible_width(self):
        self.negative('int probe(void) { return 0; }', ["-m32"], diagnostic="unsupported")

    def test_unsupported_half_format(self):
        self.negative('int probe(void) { return 0; }', ["-mfp16-format=alternative"],
                      compiler="arm-none-eabi-gcc", diagnostic="unsupported")

    def test_missing_command(self):
        self.negative('int probe(void) { return 0; }', edit=lambda e: e.pop("arguments"), diagnostic="command")

    def test_malformed_command(self):
        def edit(entry):
            entry.pop("arguments")
            entry["command"] = "gcc 'unterminated"
        self.negative('int probe(void) { return 0; }', edit=edit, diagnostic="command")

    def test_missing_selected_source(self):
        self.negative('int probe(void) { return 0; }', edit=lambda e: self.source.unlink(), diagnostic="missing")


def main():
    global OUTPUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    OUTPUT = args.output.resolve()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for program in ("gcc", "clang-tidy"):
        path = shutil.which(program)
        if not path:
            raise RuntimeError(program + " is required for backend lint regression")
        log = OUTPUT / (program + "-version.log")
        with log.open("wb") as stream:
            status = _run_owned_test_process([path, "--version"], env=dict(os.environ), timeout=15, stdout=stream)
        text = log.read_text()
        if status or (program == "clang-tidy" and not re.search(r"LLVM version 18\.1\.8\b", text)):
            raise RuntimeError("backend lint regression requires GCC and clang-tidy 18.1.8: " + text)
    reference = OUTPUT / "gcc-reference.c"
    reference.write_text('''
_Static_assert(_Generic(1.0, float: 1, default: 0), "GCC float literal");
_Static_assert(0x1.00000100000001p0 == 0x1.000002p0f, "GCC direct hex rounding");
_Static_assert(1.00000005960464477539062501 == 0x1.000002p0f, "GCC direct decimal rounding");
''')
    with (OUTPUT / "gcc-reference.log").open("wb") as stream:
        status = _run_owned_test_process(["gcc", "-std=c11", "-fsingle-precision-constant",
                                          "-fsyntax-only", str(reference)], env=dict(os.environ),
                                         timeout=15, stdout=stream)
    if status:
        raise RuntimeError("GCC reference literal assertions failed")
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(BackendLint)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    failed = not result.wasSuccessful() or bool(result.skipped) or result.testsRun != 19
    failed |= os.environ.get("OVE_BACKEND_LINT_CANARY") == "1"
    if not failed:
        print("BACKEND_LINT_GATE_PASS tests=19", flush=True)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
