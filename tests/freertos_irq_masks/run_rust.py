# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check actual Rust entry paths against the initialized C register model."""
import argparse
import hashlib
import itertools
import json
import os
import re
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "config/ove-cli"))
from ove.test import _run_owned_test_process


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--provider", type=Path, default=ROOT / "backends/freertos/freertos_irq.c")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--one-profile", action="store_true")
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
    deadline = time.monotonic() + 600
    def run(command, log, maximum=120):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Rust IRQ gate exhausted its cumulative budget")
        with log.open("wb") as stream:
            return _run_owned_test_process(command, env=env, timeout=min(maximum, remaining), stdout=stream)
    # These are actual host opaque-storage measurements for bindgen. This
    # fixture uses only a u64 key and a fresh Rust u32; no storage ABI is tested.
    storage = out / "storage"
    storage.mkdir(exist_ok=True)
    for stage, command in (("configure", ["cmake", "-S", str(ROOT / "tests/rust/stub_cmake"), "-B", str(storage)]),
                           ("build", ["cmake", "--build", str(storage), "--target", "storage_sizes", "--parallel", "8"])):
        log = storage / (stage + ".log")
        if run(command, log):
            print(log.read_text(errors="backslashreplace"), end="")
            return 1
    env["OVE_STORAGE_SIZES"] = str(storage / "ove_storage_sizes.env")
    profiles = [(mpu, allow, zero) for (mpu, allow), zero in itertools.product(((0, 0), (1, 0), (1, 1)), (0, 1))]
    if args.one_profile:
        profiles = profiles[:1]
    rows = []
    explicit = env.get("OVE_FREERTOS_IRQ_RUST_CANARY") == "1"
    for mpu, allow, zero in profiles:
        label = f"mpu{mpu}-allow{allow}-zero{zero}"
        folder = out / label
        folder.mkdir(exist_ok=True)
        config = "#pragma once\n" + "".join("#define CONFIG_OVE_" + item + " 1\n" for item in
                                             ("RTOS_FREERTOS", "SYNC", "ASYNC", "TIME", "TIMER"))
        if zero:
            config += "#define CONFIG_OVE_ZERO_HEAP 1\n"
        (folder / "ove_config.h").write_text(config)
        obj = folder / "model.o"
        command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                   "-DOVE_MODEL_MPU=" + str(mpu), "-DOVE_MODEL_ALLOW=" + str(allow), "-DOVE_MODEL_ERRATUM=1",
                   "-DOVE_FREERTOS_IRQ_PORT_SUPPORTED=1", "-DOVE_IRQ_SOURCE=" + json.dumps(str(args.provider.resolve())),
                   "-I" + str(folder), "-I" + str(HERE / "sdk"), "-I" + str(ROOT / "include"),
                   "-I" + str(ROOT / "backends/common"), "-I" + str(ROOT / "backends/freertos/include"),
                   "-c", str(HERE / "rust_model.c"), "-o", str(obj)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        env.update(OVE_GEN_DIR=str(folder), OVE_IRQ_MODEL_OBJECT=str(obj))
        cargo = ["cargo", "build", "--offline", "--locked", "--release", "--manifest-path", str(HERE / "rust/Cargo.toml")]
        if args.sanitize:
            cargo += ["--target", "x86_64-unknown-linux-gnu"]
        for stage, stage_command in (("native", command), ("rust", cargo)):
            log = folder / (stage + ".log")
            if run(stage_command, log):
                print(log.read_text(errors="backslashreplace"), end="")
                return 1
        built = out / "cargo"
        if args.sanitize:
            built /= "x86_64-unknown-linux-gnu"
        binary = folder / "irq-boundary"
        shutil.copy2(built / "release/irq-boundary", binary)
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        for kind, api, canary in itertools.product(range(14), ("spin", "global"), (False, True)):
            rejection = kind in (2, 3, 5, 6, 8, 11, 13) or kind == 4 and not mpu
            command = [str(binary), str(kind), api, "canary" if canary or explicit else "normal"]
            log = folder / f"{kind}-{api}-{'canary' if canary else 'normal'}.log"
            status = run(command, log, 10)
            data = log.read_bytes()
            expected = b"FAIL: early failure survives every later IRQ control\n" if canary else b""
            expected += f"irq masks: cases=1 checks={(3 if rejection else 6)+int(canary)} failures={int(canary)} rejected={int(rejection)}\n".encode()
            valid = status == int(canary) and data == expected
            rows.append({"profile": label, "kind": kind, "api": api, "canary": canary, "status": status,
                         "valid": valid, "command": command, "log": str(log), "elf_sha256_before_run": digest,
                         "log_sha256": hashlib.sha256(data).hexdigest()})
            (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
            print(f"{label}/{kind}/{api}/{'canary' if canary else 'normal'}", "PASS" if valid else "FAIL", flush=True)
            if not valid:
                print(data.decode(errors="backslashreplace"), end="")
            if status not in (0, 1) or not re.search(rb"^irq masks: cases=[0-9]+ checks=[0-9]+ failures=[0-9]+ rejected=[01]\n\Z", data, re.MULTILINE):
                return 1
    failed = sum(not row["valid"] for row in rows)
    print(f"freertos irq Rust gate: observations={len(rows)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
