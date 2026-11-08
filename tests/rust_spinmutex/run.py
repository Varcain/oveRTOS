# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the actual SpinMutex family and defined reference-free admission spies."""
import argparse
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "config/ove-cli"))
from ove.test import _run_owned_test_process


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    original = (ROOT / "bindings/rust/ove/src/sync.rs").read_text()
    start = original.index("#[cfg(has_async)]\npub use spin_mutex")
    family = original[start:]
    end = family.rfind("}")
    projected = family[:end] + (HERE / "adapters.rs").read_text() + family[end:]
    source = out / "source.rs"
    source.write_text(projected)
    env = dict(os.environ, OVE_SPIN_SOURCE=str(source))
    if args.sanitize:
        env.update(RUSTC_BOOTSTRAP="1", ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", LSAN_OPTIONS="exitcode=23")
    passed = failed = 0
    for zero in (False, True):
        for real in (False, True):
            label = ("zero" if zero else "heap") + ("-posix" if real else "-inert")
            folder = out / label
            folder.mkdir(parents=True, exist_ok=True)
            config = "#pragma once\n#define CONFIG_OVE_RTOS_POSIX 1\n#define CONFIG_OVE_ASYNC 1\n#define CONFIG_OVE_SYNC 1\n"
            if zero:
                config += "#define CONFIG_OVE_ZERO_HEAP 1\n"
            (folder / "ove_config.h").write_text(config)
            cflags = ["-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                      "-ffunction-sections", "-fdata-sections"]
            if args.sanitize:
                cflags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            commands = [["cc", *cflags, "-DOVE_SPIN_REAL_PROVIDER=" + str(int(real)),
                         "-c", str(HERE / "native.c"), "-o", str(folder / "native.o")]]
            objects = [folder / "native.o"]
            if real:
                aliases = ["-Dove_irq_lock=spin_real_irq_lock", "-Dove_irq_unlock=spin_real_irq_unlock"]
                aliases += ["-D" + name + "=spin_checked_" + name for name in (
                    "pthread_once", "pthread_mutexattr_init", "pthread_mutexattr_settype",
                    "pthread_mutexattr_destroy", "pthread_mutex_init", "pthread_mutex_lock", "pthread_mutex_unlock")]
                commands.append(["cc", *cflags, *aliases, "-I" + str(folder), "-I" + str(ROOT / "include"),
                                 "-c", str(ROOT / "tests/host/posix/posix_irq.c"), "-o", str(folder / "posix.o")])
                objects.append(folder / "posix.o")
            rust = ["rustc", "--edition=2024", "--cfg", "has_async", "--cfg", "has_sync",
                    "--check-cfg", "cfg(has_async)", "--check-cfg", "cfg(has_sync)", "--check-cfg", "cfg(zero_heap)",
                    "-Dwarnings", "-Copt-level=1", "-g", str(HERE / "main.rs"),
                    "-o", str(folder / "ownership"), "-Clink-arg=-pthread"]
            rust += ["-Clink-arg=" + str(obj) for obj in objects]
            if zero:
                rust += ["--cfg", "zero_heap"]
            if args.sanitize:
                rust += ["-Zsanitizer=address", "-Cforce-frame-pointers=yes", "-Clink-arg=-lubsan"]
            commands.append(rust)
            for command in commands:
                if _run_owned_test_process(command, env=env, timeout=180):
                    return 1
            ok = _run_owned_test_process([str(folder / "ownership")], env=env, timeout=20) == 0
            print(label, "PASS" if ok else "FAIL", flush=True)
            passed += ok
            failed += not ok
    failed += passed == 0
    print(f"spinmutex ownership: passed={passed} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
