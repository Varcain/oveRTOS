# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and run the Rust EventGroup contracts and their compiler and macro controls."""
import argparse
import os
import subprocess
import sys
from pathlib import Path

from macro_controls import verify as macro_controls
from type_controls import verify as type_controls

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TARGET = "x86_64-unknown-linux-gnu"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    results = []
    for zero in (False, True):
        for optional in (False, True):
            base = args.output.resolve() / (("zero" if zero else "heap") + ("-optional" if optional else "-minimal"))
            native, target = base / "native", base / "cargo"
            env = {key: value for key, value in os.environ.items() if key not in (
                "DOCS_RS", "CARGO_ENCODED_RUSTFLAGS", "RUSTFLAGS", "CARGO_BUILD_TARGET", "CMSIS_DSP_INCLUDE",
                "CMSIS_CORE_INCLUDE", "BINDGEN_EXTRA_CLANG_ARGS", "EVENTGROUP_TEST_SANITIZE")}
            env.update(OVE_DIR=str(ROOT), OVE_GEN_DIR=str(native), RUST_IS_NATIVE="1",
                       OVE_STORAGE_SIZES=str(native / "ove_storage_sizes.env"),
                       EVENTGROUP_TEST_LIB_DIR=str(native), CARGO_TARGET_DIR=str(target),
                       LVGL_INCLUDE_PATH=str(ROOT / "tests/backends/stub/lvgl"),
                       LVGL_PARENT_PATH=str(ROOT / "tests/backends/stub"), LV_CONF_PATH=str(ROOT / "tests/host"))
            if args.sanitize:
                env.update(RUSTC_BOOTSTRAP="1", EVENTGROUP_TEST_SANITIZE="1",
                           RUSTFLAGS="-Zsanitizer=address -Cforce-frame-pointers=yes",
                           ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", LSAN_OPTIONS="exitcode=23")
            for command in (["cmake", "-S", str(HERE), "-B", str(native), "-DOVE_ZERO_HEAP=" + ("ON" if zero else "OFF"),
                             "-DOVE_EVENTGROUP_OPTIONAL=" + ("ON" if optional else "OFF"),
                             "-DOVE_SANITIZE=" + ("ON" if args.sanitize else "OFF")],
                            ["cmake", "--build", str(native), "--parallel", "8"],
                            ["cargo", "build", "--locked", "--target", TARGET, "--manifest-path",
                             str(HERE / "Cargo.toml"), "--bin", "eventgroup-contracts"]):
                subprocess.run(command, env=env, timeout=240, check=True)
            binary = target / TARGET / "debug/eventgroup-contracts"
            results.append(subprocess.run([str(binary)], env=env, stdin=subprocess.DEVNULL, timeout=30, check=False).returncode == 0)
            print(base.name, "PASS" if results[-1] else "FAIL", flush=True)
            if not args.sanitize:
                subprocess.run(["cargo", "build", "--locked", "--lib", "--target", TARGET, "--manifest-path",
                                str(ROOT / "bindings/rust/ove/Cargo.toml"), "--no-default-features",
                                "--features", "std,async" if optional else "std"], env=env, timeout=240, check=True)
                library = target / TARGET / "debug/libove.rlib"
                results += type_controls(library, base / "types", env, zero=zero, asynchronous=optional)
    if not args.sanitize:
        results += macro_controls(args.output.resolve() / "macro", dict(os.environ))
    failed = results.count(False) + (not results)
    print(f"rust eventgroup: passed={results.count(True)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    sys.exit(main())
