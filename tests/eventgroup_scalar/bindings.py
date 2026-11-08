# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run public C++, Rust and Zig scalar APIs against real POSIX event groups."""
import argparse
import os
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--zig", default=shutil.which("zig"))
    args = parser.parse_args()
    if not args.zig:
        parser.error("--zig must name the installed Zig compiler")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    def command(argv, env, name):
        print(name, flush=True)
        subprocess.run(argv, env=env, timeout=240, check=True)

    for zero in (False, True):
        mode = "zero" if zero else "heap"
        native = out / mode
        generated = native / "bindings"
        env = dict(os.environ)
        for key in ("DOCS_RS", "RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS", "CARGO_BUILD_TARGET",
                    "BINDGEN_EXTRA_CLANG_ARGS"):
            env.pop(key, None)
        env.update(OVE_DIR=str(ROOT), OVE_GEN_DIR=str(generated), RUST_IS_NATIVE="1",
                   OVE_STORAGE_SIZES=str(generated / "ove_storage_sizes.env"),
                   EVENTGROUP_TEST_LIB_DIR=str(native), CARGO_TARGET_DIR=str(native / "cargo"),
                   LVGL_INCLUDE_PATH=str(ROOT / "tests/backends/stub/lvgl"),
                   LVGL_PARENT_PATH=str(ROOT / "tests/backends/stub"),
                   LV_CONF_PATH=str(ROOT / "tests/host"))
        command(["cmake", "-S", str(HERE), "-B", str(native),
                 "-DOVE_ZERO_HEAP=" + ("ON" if zero else "OFF")], env, mode + "-configure")
        command(["cmake", "--build", str(native), "--parallel", "8"], env, mode + "-native")
        includes = ["-I" + str(generated), "-I" + str(ROOT / "include"),
                    "-I" + str(ROOT / "tests/host/posix/include")]
        cpp = native / "cpp"
        command(["g++", "-std=c++23", "-Wall", "-Wextra", "-Werror", *includes,
                 "-I" + str(ROOT / "bindings/cpp"), str(HERE / "bindings.cpp"),
                 str(native / "libeventgroup_binding.a"), "-lpthread", "-o", str(cpp)],
                env, mode + "-cpp-build")
        command([str(cpp)], env, mode + "-cpp-run")
        command(["cargo", "build", "--locked", "--manifest-path", str(HERE / "Cargo.toml")],
                env, mode + "-rust-build")
        command([str(native / "cargo/debug/eventgroup-bindings")], env, mode + "-rust-run")
        sizes = "\n".join("#define OVE_" + line.replace("=", " ")
                          for line in (generated / "ove_storage_sizes.env").read_text().splitlines()
                          if "=" in line and not line.startswith("#")) + "\n"
        storage = generated / "zig_storage_sizes.h"
        storage.write_text(sizes)
        zig = native / "zig"
        env.update(ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"),
                   ZIG_LOCAL_CACHE_DIR=str(native / "zig-cache"))
        command([args.zig, "build-exe", "-target", "native-native-gnu", "-OReleaseSafe",
                 "--global-cache-dir", str(out / "zig-cache"),
                 "--cache-dir", str(native / "zig-cache"), "--dep", "ove",
                 "-Mroot=" + str(HERE / "bindings.zig"),
                 "-Move=" + str(ROOT / "bindings/zig/ove/src/root.zig"), *includes,
                 str(native / "libeventgroup_binding.a"),
                 "-lpthread", "-lc", "-femit-bin=" + str(zig)], env, mode + "-zig-build")
        command([str(zig)], env, mode + "-zig-run")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
