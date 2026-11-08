# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check finite Event budgets with initialized inert native observers."""
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

PROFILES = [(bits, abort, mode, 1) for bits in (16, 32, 64) for abort in (0, 1)
            for mode in ("heap", "zero")] + [(32, 0, "heap", 0)]


def expected_output(profile, canary):
    bits, abort, mode, suspend = profile
    prefix = "FAIL: setup: early failure survives all later controls\n" if canary else ""
    return (prefix + f"freertos event accounting: bits={bits} abort={abort} "
            f"suspend={suspend} heap={int(mode == 'heap')} cases=26 "
            f"checks={243 + int(canary)} failures={int(canary)}\n").encode()


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
    commands, rows = [], []
    explicit = env.get("OVE_FREERTOS_EVENT_TIMEOUTS_CANARY") == "1"
    for label, command in (
        ("configure", ["cmake", "-S", str(HERE), "-B", str(out / "build"),
                       "-DOVE_SANITIZE=" + ("ON" if args.sanitize else "OFF")]),
        ("build", ["cmake", "--build", str(out / "build"), "--parallel", "8"]),
    ):
        log = out / (label + ".log")
        with log.open("wb") as stream:
            status = _run_owned_test_process(command, env=env, timeout=180, stdout=stream)
        commands.append({"command": command, "status": status, "log": str(log),
                         "sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if status:
            print(log.read_text(errors="backslashreplace"), end="")
            return 1
    for profile in PROFILES:
        name = "event_" + "_".join(map(str, profile))
        binary = out / "build" / name
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        for canary in (False, True):
            active = canary or explicit
            command = [str(binary)] + (["canary"] if active else [])
            log = out / (name + ("-canary" if canary else "-normal") + ".log")
            with log.open("wb") as stream:
                status = _run_owned_test_process(command, env=env, timeout=10, stdout=stream)
            data = log.read_bytes()
            valid = not explicit and status == int(active) and data == expected_output(profile, active)
            row = {"command": command, "status": status, "valid": valid,
                   "elf_sha256_before_run": digest, "log": str(log),
                   "sha256": hashlib.sha256(data).hexdigest(),
                   "qualifier": "actual Event/common heap with initialized inert entries;64 model only;suspend0 argument only;continuous native counter lifetime"}
            rows.append(row)
            log.with_suffix(".status.json").write_text(json.dumps(row, indent=2) + "\n")
            (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
            print(name, "canary" if canary else "normal", "PASS" if valid else "FAIL", flush=True)
            if not valid:
                print(data.decode(errors="backslashreplace"), end="")
    if len(rows) != 26:
        raise ValueError("incomplete Event accounting observations")
    failed = sum(not row["valid"] for row in rows)
    print(f"freertos event timeouts: observations={len(rows)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
