# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check actual Rust entry paths against the initialized C register model."""
import argparse
import itertools
import os
import shutil
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
    env = dict(os.environ)
    env.pop("DOCS_RS", None)
    env.update(OVE_DIR=str(ROOT), RUST_IS_NATIVE="1", CARGO_TARGET_DIR=str(out / "cargo"),
               LVGL_INCLUDE_PATH=str(ROOT / "tests/backends/stub/lvgl"),
               LVGL_PARENT_PATH=str(ROOT / "tests/backends/stub"))
    if args.sanitize:
        env.update(RUSTC_BOOTSTRAP="1", RUSTFLAGS="-Zsanitizer=address -Cforce-frame-pointers=yes -Clink-arg=-lubsan",
                   ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                   LSAN_OPTIONS="exitcode=23")

    def run(command, log, timeout=240):
        with log.open("wb") as stream:
            status = _run_owned_test_process(command, env=env, timeout=timeout, stdout=stream)
        if status:
            print(log.read_text(errors="backslashreplace"), end="")
        return status

    # These are actual host opaque-storage measurements for bindgen. This
    # fixture uses only a u64 key and a fresh Rust u32; no storage ABI is tested.
    # The probe also measures public struct layouts, so it must see the
    # profiles' feature set (on the host backend) rather than tests/ove_config.h.
    storage = out / "storage"
    (out / "stub_config").mkdir(exist_ok=True)
    storage.mkdir(exist_ok=True)
    (out / "stub_config/ove_config.h").write_text("#pragma once\n" + "".join(
        "#define CONFIG_OVE_" + item + " 1\n" for item in ("RTOS_POSIX", "SYNC", "ASYNC", "TIME", "TIMER")))
    for stage, command in (("configure", ["cmake", "-S", str(ROOT / "tests/rust/stub_cmake"), "-B", str(storage),
                                          "-DOVE_STUB_CONFIG_DIR=" + str(out / "stub_config")]),
                           ("build", ["cmake", "--build", str(storage), "--target", "storage_sizes", "--parallel", "8"])):
        if run(command, storage / (stage + ".log")):
            return 1
    env["OVE_STORAGE_SIZES"] = str(storage / "ove_storage_sizes.env")
    passed = failed = 0
    for (mpu, allow), zero in itertools.product(((0, 0), (1, 0), (1, 1)), (0, 1)):
        folder = out / f"mpu{mpu}-allow{allow}-zero{zero}"
        folder.mkdir(exist_ok=True)
        config = "#pragma once\n" + "".join("#define CONFIG_OVE_" + item + " 1\n" for item in
                                             ("RTOS_FREERTOS", "SYNC", "ASYNC", "TIME", "TIMER"))
        if zero:
            config += "#define CONFIG_OVE_ZERO_HEAP 1\n"
        (folder / "ove_config.h").write_text(config)
        obj = folder / "model.o"
        command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                   "-DOVE_MODEL_MPU=" + str(mpu), "-DOVE_MODEL_ALLOW=" + str(allow), "-DOVE_MODEL_ERRATUM=1",
                   "-DOVE_FREERTOS_IRQ_PORT_SUPPORTED=1",
                   '-DOVE_IRQ_SOURCE="' + str(ROOT / "backends/freertos/freertos_irq.c") + '"',
                   "-I" + str(folder), "-I" + str(HERE / "sdk"), "-I" + str(ROOT / "include"),
                   "-I" + str(ROOT / "backends/common"), "-I" + str(ROOT / "backends/freertos/include"),
                   "-c", str(HERE / "rust_model.c"), "-o", str(obj)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        env.update(OVE_GEN_DIR=str(folder), OVE_IRQ_MODEL_OBJECT=str(obj))
        cargo = ["cargo", "build", "--offline", "--locked", "--release", "--manifest-path", str(HERE / "rust/Cargo.toml")]
        if args.sanitize:
            cargo += ["--target", "x86_64-unknown-linux-gnu"]
        if run(command, folder / "native.log") or run(cargo, folder / "rust.log"):
            return 1
        built = out / "cargo" / ("x86_64-unknown-linux-gnu" if args.sanitize else "") / "release/irq-boundary"
        binary = folder / "irq-boundary"
        shutil.copy2(built, binary)
        for kind, api in itertools.product(range(14), ("spin", "global")):
            ok = run([str(binary), str(kind), api], folder / f"{kind}-{api}.log", timeout=10) == 0
            passed += ok
            failed += not ok
            print(f"{folder.name}/{kind}/{api}", "PASS" if ok else "FAIL", flush=True)
    print(f"freertos irq Rust gate: passed={passed} failed={failed}", flush=True)
    return int(failed != 0 or passed == 0)


if __name__ == "__main__":
    raise SystemExit(main())
