# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run shared public binding waits against the actual FreeRTOS Event adapter."""
import argparse
import hashlib
import json
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
    parser.add_argument("--zig", required=True, type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--backend-include", type=Path)
    parser.add_argument("--legacy-slot", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    for key in ("DOCS_RS", "RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS", "CARGO_BUILD_TARGET",
                "BINDGEN_EXTRA_CLANG_ARGS", "CMSIS_DSP_INCLUDE", "CMSIS_CORE_INCLUDE"):
        env.pop(key, None)
    commands, rows = [], []

    def build(name, command, environment):
        log = out / (name + ".log")
        with log.open("wb") as stream:
            status = _run_owned_test_process(command, env=environment, timeout=240, stdout=stream)
        commands.append({"command": command, "status": status, "log": str(log),
                         "sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if status:
            print(log.read_text(errors="backslashreplace"), end="")
        return status == 0

    native = out / "build"
    configure = ["cmake", "-S", str(HERE), "-B", str(native),
                 "-DOVE_MODEL_EVENT_LIST=" + ("OFF" if args.legacy_slot else "ON")]
    if args.source:
        configure.append("-DOVE_EVENT_SOURCE=" + str(args.source.resolve()))
    if args.backend_include:
        configure.append("-DOVE_BACKEND_INCLUDE=" + str(args.backend_include.resolve()))
    if not build("configure", configure, env):
        return 1
    if not build("native", ["cmake", "--build", str(native), "--parallel", "8", "--target",
                            "shared_cpp_heap", "shared_cpp_zero", "binding_storage_heap",
                            "binding_storage_zero"], env):
        return 1
    explicit = env.get("OVE_FREERTOS_EVENT_BINDINGS_CANARY") == "1"
    for mode in ("heap", "zero"):
        generated = native / ("bindings_" + mode)
        target = out / ("cargo_" + mode)
        child = dict(env, OVE_DIR=str(ROOT), OVE_GEN_DIR=str(generated), RUST_IS_NATIVE="1",
                     OVE_STORAGE_SIZES=str(generated / "ove_storage_sizes.env"),
                     EVENT_WAITER_TEST_LIB_DIR=str(native),
                     EVENT_WAITER_TEST_LIB="provider_threaded_32_0_0_" + mode,
                     CARGO_TARGET_DIR=str(target),
                     LVGL_INCLUDE_PATH=str(ROOT / "tests/backends/stub/lvgl"),
                     LVGL_PARENT_PATH=str(ROOT / "tests/backends/stub"),
                     LV_CONF_PATH=str(ROOT / "tests/host"))
        if not build(mode + "-rust", ["cargo", "build", "--locked", "--target",
                                      "x86_64-unknown-linux-gnu", "--manifest-path",
                                      str(HERE / "rust/Cargo.toml")], child):
            return 1
        sizes = "".join("#define OVE_" + line.replace("=", " ", 1) + "\n"
                        for line in (generated / "ove_storage_sizes.env").read_text().splitlines()
                        if "=" in line)
        (generated / "zig_storage_sizes.h").write_text(sizes)
        zig = out / ("zig_" + mode)
        child.update(ZIG_GLOBAL_CACHE_DIR=str(out / "zig-cache"),
                     ZIG_LOCAL_CACHE_DIR=str(out / ("zig-cache-" + mode)))
        if not build(mode + "-zig", [str(args.zig.resolve()), "build-exe", "-target",
                                     "native-native-gnu", "-OReleaseSafe", "--global-cache-dir",
                                     str(out / "zig-cache"), "--cache-dir",
                                     str(out / ("zig-cache-" + mode)), "--dep", "ove",
                                     "-Mroot=" + str(HERE / "bindings.zig"),
                                     "-Move=" + str(ROOT / "bindings/zig/ove/src/root.zig"),
                                     "-I" + str(generated), "-I" + str(ROOT / "include"),
                                     str(native / ("libprovider_threaded_32_0_0_" + mode + ".a")),
                                     "-lpthread", "-lc", "-femit-bin=" + str(zig)], child):
            return 1
        programs = [("cpp", native / ("shared_cpp_" + mode)),
                    ("rust", target / "x86_64-unknown-linux-gnu/debug/event-shared-rust"),
                    ("zig", zig)]
        for language, binary in programs:
            digest = hashlib.sha256(binary.read_bytes()).hexdigest()
            for canary in (False, True):
                active = canary or explicit
                child["OVE_EVENT_BINDING_CANARY"] = str(int(active))
                command = [str(binary)]
                log = out / (mode + "-" + language + ("-canary" if canary else "-normal") + ".log")
                with log.open("wb") as stream:
                    status = _run_owned_test_process(command, env=child, timeout=20, stdout=stream)
                prefix = "FAIL: early failure survives later binding controls\n" if active else ""
                expected = (prefix + f"event shared {language}: heap={int(mode == 'heap')} "
                            f"completed=2 failures={int(active)}\n").encode()
                data = log.read_bytes()
                valid = not explicit and status == int(active) and data == expected
                row = {"command": command, "status": status, "valid": valid,
                       "elf_sha256_before_run": digest, "log": str(log),
                       "sha256": hashlib.sha256(data).hexdigest(),
                       "qualifier": "actual binding+adapter;initialized host observer entries/context;no native RTOS or MPU/IRQ execution"}
                rows.append(row)
                log.with_suffix(".status.json").write_text(json.dumps(row, indent=2) + "\n")
                (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
                print(mode, language, "canary" if canary else "normal",
                      "PASS" if valid else "FAIL", flush=True)
                if not valid:
                    print(data.decode(errors="backslashreplace"), end="")
    if len(rows) != 12:
        raise ValueError("incomplete shared Event binding observations")
    failed = sum(not row["valid"] for row in rows)
    print(f"freertos event bindings: observations={len(rows)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
