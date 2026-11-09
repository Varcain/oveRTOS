# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile native bindings with real headers in no_std, alloc and std modes."""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main(out):
    env = {key: value for key, value in os.environ.items() if key not in (
        "DOCS_RS", "CARGO_BUILD_TARGET", "RUSTFLAGS") and not key.startswith("BINDGEN_EXTRA_CLANG_ARGS")}
    # Empty encoded flags override target-specific/config-file cfg injections.
    env.update(CARGO_ENCODED_RUSTFLAGS="", OVE_DIR=str(ROOT), RUST_IS_NATIVE="1",
               LVGL_INCLUDE_PATH=str(ROOT / "tests/backends/stub/lvgl"), LVGL_PARENT_PATH=str(ROOT / "tests/backends/stub"))
    host = subprocess.run(["rustc", "--print", "host-tuple"], capture_output=True, text=True, check=True).stdout.strip()
    passed = failed = 0
    for zero in (0, 1):
        for feature in ("no_std", "alloc", "std"):
            folder = out / (("zero" if zero else "heap") + "-" + feature)
            (folder / "storage").mkdir(parents=True, exist_ok=True)
            (folder / "ove_config.h").write_text("#pragma once\n#define CONFIG_OVE_RTOS_POSIX 1\n"
                                                 + ("#define CONFIG_OVE_ZERO_HEAP 1\n" if zero else ""))
            env.update(OVE_GEN_DIR=str(folder), OVE_STORAGE_SIZES=str(folder / "storage/ove_storage_sizes.env"),
                       CARGO_TARGET_DIR=str(folder / "cargo"))
            commands = (["cmake", "-S", str(ROOT / "tests/rust/stub_cmake"), "-B", str(folder / "storage"),
                         "-DOVE_STUB_CONFIG_DIR=" + str(folder)],
                        ["cmake", "--build", str(folder / "storage"), "--target", "storage_sizes", "--parallel", "8"],
                        ["cargo", "check", "--locked", "--lib", "--no-default-features", "--target", host,
                         "--manifest-path", str(ROOT / "bindings/rust/ove/Cargo.toml")]
                        + ([] if feature == "no_std" else ["--features", feature]))
            ok = all(subprocess.run(command, env=env, timeout=180, check=False).returncode == 0 for command in commands)
            print(folder.name, "PASS" if ok else "FAIL", flush=True)
            passed += ok
            failed += not ok
    failed += passed == 0
    print(f"native core bindgen gate: passed={passed} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    sys.exit(main(Path(sys.argv[1]).resolve()))
