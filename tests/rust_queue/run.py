# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and validate real Rust Queue ownership and native-entry contracts."""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

from macro_controls import verify as macro_controls
from type_controls import verify as type_controls

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CASES = ("live_and_reuse", "initialization_failure", "competing_initialization",
         "competing_teardown", "construction_contexts", "operation_contexts",
         "transfer_routing", "retained_owners", "native_integration",
         "dimension_rejection", "local_storage_payload")


def expected(zero, optional, canary=False):
    cases = [*CASES, "raw_ownership" if zero else "borrowed_payloads"]
    if optional:
        cases.append("notifications")
    cases.append("macro_reuse")
    checks = (167 if zero else 170) - (0 if optional else 23) + int(canary)
    text = "FAIL: early_failure_survives_controls\n" if canary else ""
    text += "".join("case: " + name + "\n" for name in cases)
    text += (f"queue contracts: checks={checks} failures={int(canary)} "
             f"workers=3/3 retained={5 if zero else 6}\n")
    return text.encode()


def profile(output, zero, optional, sanitize):
    name = ("zero" if zero else "heap") + ("-optional" if optional else "-minimal")
    base = output / name
    native, target = base / "native", base / "cargo"
    env = dict(os.environ)
    for key in ("DOCS_RS", "CARGO_ENCODED_RUSTFLAGS", "RUSTFLAGS", "CARGO_BUILD_TARGET",
                "CMSIS_DSP_INCLUDE", "CMSIS_CORE_INCLUDE", "BINDGEN_EXTRA_CLANG_ARGS"):
        env.pop(key, None)
    env.update(OVE_DIR=str(ROOT), OVE_GEN_DIR=str(native), RUST_IS_NATIVE="1",
               OVE_STORAGE_SIZES=str(native / "ove_storage_sizes.env"),
               QUEUE_TEST_LIB_DIR=str(native), CARGO_TARGET_DIR=str(target),
               LVGL_INCLUDE_PATH=str(ROOT / "tests/backends/stub/lvgl"),
               LVGL_PARENT_PATH=str(ROOT / "tests/backends/stub"),
               LV_CONF_PATH=str(ROOT / "tests/host"))
    if sanitize:
        env.update(RUSTC_BOOTSTRAP="1", QUEUE_TEST_SANITIZE="1",
                   RUSTFLAGS="-Zsanitizer=address -Cforce-frame-pointers=yes",
                   ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", LSAN_OPTIONS="exitcode=23")
    else:
        env.pop("QUEUE_TEST_SANITIZE", None)
    configure = ["cmake", "-S", str(HERE), "-B", str(native),
                 "-DOVE_ZERO_HEAP=" + ("ON" if zero else "OFF"),
                 "-DOVE_QUEUE_OPTIONAL=" + ("ON" if optional else "OFF"),
                 "-DOVE_SANITIZE=" + ("ON" if sanitize else "OFF")]
    build = ["cmake", "--build", str(native), "--parallel", "8"]
    cargo = ["cargo", "build", "--locked", "--target", "x86_64-unknown-linux-gnu",
             "--manifest-path", str(HERE / "Cargo.toml"), "--bin", "queue-contracts"]
    binary = target / "x86_64-unknown-linux-gnu/debug/queue-contracts"
    return base, env, (configure, build, cargo), binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    explicit = os.environ.get("OVE_RUST_QUEUE_CANARY") == "1"
    rows = []
    commands = []
    type_count = 0
    failed = 0

    def build(command, env, folder, name):
        result = subprocess.run(command, env=env, capture_output=True, timeout=240, check=False)
        log = folder / (name + ".log")
        log.write_bytes(result.stdout + result.stderr)
        commands.append({"command": command, "status": result.returncode,
                         "log": str(log), "log_sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode:
            print(log.read_text(errors="backslashreplace"), end="")
            raise RuntimeError("Queue build failed: " + name)
        return result

    for zero in (False, True):
        for optional in (False, True):
            base, env, build_commands, binary = profile(output, zero, optional, args.sanitize)
            base.mkdir(parents=True, exist_ok=True)
            for name, command in zip(("configure", "native", "rust"), build_commands, strict=True):
                build(command, env, base, name)
            for canary in (False, True):
                active = canary or explicit
                command = [str(binary)] + (["canary"] if active else [])
                p = subprocess.run(command, env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=False)
                log = base / ("canary.log" if canary else "normal.log")
                log.write_bytes(p.stdout)
                row = {"command": command, "status": p.returncode,
                       "elf_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                       "log_sha256": hashlib.sha256(p.stdout).hexdigest(),
                       "zero": zero, "optional": optional, "sanitize": args.sanitize}
                log.with_suffix(".status.json").write_text(json.dumps(row, indent=2) + "\n")
                valid = not explicit and p.returncode == int(active) and p.stdout == expected(zero, optional, active)
                rows.append({"valid": valid, **row})
                (output / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
                failed += not valid
                print(base.name, "canary" if canary else "normal", "PASS" if valid else "FAIL", flush=True)
                if not valid:
                    print(p.stdout.decode(errors="backslashreplace"), end="")
            if not args.sanitize:
                command = ["cargo", "build", "--locked", "--lib", "--target", "x86_64-unknown-linux-gnu",
                           "--manifest-path", str(ROOT / "bindings/rust/ove/Cargo.toml"),
                           "--no-default-features", "--features", "std,async" if optional else "std",
                           "--message-format=json"]
                p = build(command, env, base, "type-library")
                artifacts = [json.loads(line) for line in p.stdout.splitlines() if line.startswith(b"{")]
                artifact = next(row for row in artifacts if row.get("reason") == "compiler-artifact"
                                and row["target"]["name"] == "ove")
                library = Path(next(name for name in artifact["filenames"] if name.endswith(".rlib")))
                valid = type_controls(library, base / "types", env, zero=zero, asynchronous=optional)
                type_count += len(json.loads((base / "types/observations.json").read_text()))
                failed += not valid
    macro_count = 0
    if not args.sanitize:
        valid = macro_controls(output / "macro", dict(os.environ))
        failed += not valid
        macro_count = 2
    if len(rows) != 8 or type_count != (0 if args.sanitize else 42):
        raise ValueError("incomplete Queue observation set")
    print(f"rust queue: native=8 types={type_count} macro={macro_count} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    sys.exit(main())
