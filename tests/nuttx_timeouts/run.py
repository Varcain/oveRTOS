# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe finite NuttX wait budgets without executing native blocking code."""
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


def expected_output(name, canary):
    fields = name.split("_")
    bits, tick = fields[1:3]
    count = 81 if fields[0] == "arith" else (499 if fields[3] == "heap" else 482)
    if fields[0] == "arith":
        prefix = f"nuttx timeout arithmetic: clock={bits} tick={tick}"
        failure = "early failure survives later controls"
    else:
        prefix = f"nuttx timeout native observers: clock={bits} tick={tick} heap={int(fields[3] == 'heap')}"
        failure = "early semantic failure survives later controls"
    return ((f"FAIL: {failure}\n" if canary else "") +
            f"{prefix} checks={count + int(canary)} failures={int(canary)}\n").encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if args.sanitize:
        env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", LSAN_OPTIONS="exitcode=23")
    rows, commands = [], []
    explicit = env.get("OVE_NUTTX_TIMEOUTS_CANARY") == "1"
    for name, command in (
        ("configure", ["cmake", "-S", str(HERE), "-B", str(out / "build"),
                       "-DOVE_TEST_SAFE_WAIT=ON",
                       "-DOVE_SANITIZE=" + ("ON" if args.sanitize else "OFF")]),
        ("build", ["cmake", "--build", str(out / "build"), "--parallel", "8"]),
    ):
        log = out / (name + ".log")
        with log.open("wb") as stream:
            status = _run_owned_test_process(command, env=env, timeout=180, stdout=stream)
        commands.append({"command": command, "status": status, "log": str(log),
                         "sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if status:
            print(log.read_text(errors="backslashreplace"), end="")
            return 1
    names = [f"arith_{bits}_{tick}" for bits in (32, 64)
             for tick in (100000, 1000000, 1500000, 10000000)]
    names += [f"native_{bits}_{tick}_{mode}" for bits in (32, 64)
              for tick in (1000000, 10000000) for mode in ("heap", "zero")]
    for name in names:
        binary = out / "build" / name
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        for canary in (False, True):
            active = canary or explicit
            command = [str(binary)] + (["canary"] if active else [])
            log = out / (name + ("-canary" if canary else "-normal") + ".log")
            with log.open("wb") as stream:
                status = _run_owned_test_process(command, env=env, timeout=15, stdout=stream)
            data = log.read_bytes()
            valid = not explicit and status == int(active) and data == expected_output(name, active)
            row = {"command": command, "status": status, "valid": valid,
                   "elf_sha256": digest, "log": str(log),
                   "sha256": hashlib.sha256(data).hexdigest(),
                   "qualifier": "explicit clock models and inert native argument observers"}
            rows.append(row)
            log.with_suffix(".status.json").write_text(json.dumps(row, indent=2) + "\n")
            (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
            print(name, "canary" if canary else "normal", "PASS" if valid else "FAIL", flush=True)
            if not valid:
                print(data.decode(errors="backslashreplace"), end="")
    failed = sum(not row["valid"] for row in rows)
    if len(rows) != 32:
        raise ValueError("incomplete NuttX timeout observations")
    print(f"nuttx timeouts: observations={len(rows)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
