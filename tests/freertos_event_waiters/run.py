# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check actual Event bodies with initialized nested and threaded observers."""
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

PROFILES = ["event_heap", "event_zero"] + [
    f"threaded_{bits}_{mpu}_{abort}_{mode}" for bits in (16, 32, 64)
    for mpu in (0, 1) for abort in (0, 1) for mode in ("heap", "zero")]
MASK_PROFILES = [f"masks_{erratum}_{mpu}_{mode}" for erratum in (0, 1)
                 for mpu in (0, 1) for mode in ("heap", "zero")]


def expected_output(name, canary):
    fields = name.split("_")
    if fields[0] == "event":
        prefix = "FAIL: setup: early failure survives all later controls\n" if canary else ""
        footer = (f"freertos event waiters: bits=32 heap={int(fields[1] == 'heap')} "
                  f"cases=6 checks={54 + int(canary)} failures={int(canary)}\n")
    elif fields[0] == "masks":
        _, erratum, mpu, mode = fields
        prefix = ("FAIL: canary: controlled mask-observer failure reaches the caller\n"
                  if canary else "")
        footer = (f"freertos event masks: erratum={erratum} mpu={mpu} "
                  f"heap={int(mode == 'heap')} cases={34 + 2 * int(mpu)} "
                  f"checks={368 + 22 * int(mpu) + int(canary)} failures={int(canary)}\n")
    else:
        _, bits, mpu, abort, mode = fields
        prefix = "FAIL: early failure survives later threaded controls\n" if canary else ""
        footer = (f"freertos event threaded: bits={bits} mpu_model={mpu} abort={abort} "
                  f"heap={int(mode == 'heap')} cases={12 + 3 * int(abort)} "
                  f"failures={int(canary)}\n")
    return (prefix + footer).encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source", type=Path)
    parser.add_argument("--backend-include", type=Path)
    parser.add_argument("--legacy-slot", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if args.sanitize:
        env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", LSAN_OPTIONS="exitcode=23")
    configure = ["cmake", "-S", str(HERE), "-B", str(out / "build"),
                 "-DOVE_SANITIZE=" + ("ON" if args.sanitize else "OFF"),
                 "-DOVE_MODEL_EVENT_LIST=" + ("OFF" if args.legacy_slot else "ON")]
    if args.source:
        configure.append("-DOVE_EVENT_SOURCE=" + str(args.source.resolve()))
    if args.backend_include:
        configure.append("-DOVE_BACKEND_INCLUDE=" + str(args.backend_include.resolve()))
    commands, rows = [], []
    for label, command, timeout in (
        ("configure", configure, 180),
        ("build", ["cmake", "--build", str(out / "build"), "--parallel", "8"], 300),
    ):
        log = out / (label + ".log")
        with log.open("wb") as stream:
            status = _run_owned_test_process(command, env=env, timeout=timeout, stdout=stream)
        commands.append({"command": command, "status": status, "log": str(log),
                         "sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if status:
            print(log.read_text(errors="backslashreplace"), end="")
            return 1
    explicit = env.get("OVE_FREERTOS_EVENT_WAITERS_CANARY") == "1"
    profiles = PROFILES + ([] if args.legacy_slot else MASK_PROFILES)
    for name in profiles:
        binary = out / "build" / name
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        for canary in (False, True):
            active = canary or explicit
            command = [str(binary)] + (["canary"] if active else [])
            log = out / (name + ("-canary" if canary else "-normal") + ".log")
            with log.open("wb") as stream:
                status = _run_owned_test_process(command, env=env, timeout=20, stdout=stream)
            data = log.read_bytes()
            valid = not explicit and status == int(active) and data == expected_output(name, active)
            row = {"command": command, "status": status, "valid": valid,
                   "elf_sha256_before_run": digest, "log": str(log),
                   "sha256": hashlib.sha256(data).hexdigest(),
                   "qualifier": "actual adapters/common owners;initialized host observer entries;model MPU/ISR only;64 model;no FreeRTOS scheduler/tasks/blocking linked"}
            rows.append(row)
            log.with_suffix(".status.json").write_text(json.dumps(row, indent=2) + "\n")
            (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
            print(name, "canary" if canary else "normal", "PASS" if valid else "FAIL", flush=True)
            if not valid:
                print(data.decode(errors="backslashreplace"), end="")
    if len(rows) != 2 * len(profiles):
        raise ValueError("incomplete Event waiter observations")
    failed = sum(not row["valid"] for row in rows)
    print(f"freertos event waiters: observations={len(rows)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
