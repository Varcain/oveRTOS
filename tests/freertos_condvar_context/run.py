# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe actual CV adapter mode, ownership and masks with initialized spies."""
import argparse
import hashlib
import itertools
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "config/ove-cli"))
from ove.test import _run_owned_test_process


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--provider", type=Path, default=ROOT / "backends/freertos/freertos_sync.c")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if args.sanitize:
        env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", LSAN_OPTIONS="exitcode=23")
    commands = [("configure", ["cmake", "-S", str(HERE), "-B", str(out / "build"),
                              "-DOVE_CV_SOURCE=" + str(args.provider.resolve()),
                              "-DOVE_SANITIZE=" + str(int(args.sanitize))]),
                ("build", ["cmake", "--build", str(out / "build"), "--parallel", "8"])]
    rows = []
    for stage, command in commands:
        log = out / (stage + ".log")
        with log.open("wb") as stream:
            status = _run_owned_test_process(command, env=env, timeout=240, stdout=stream)
        if status:
            print(log.read_text(errors="backslashreplace"), end="")
            return 1
    explicit = env.get("OVE_FREERTOS_CV_CONTEXT_CANARY") == "1"
    for bits, context, erratum, mode, canary in itertools.product(
        (16, 32), ("ordinary", "v1allow0", "v1allow1"), (0, 1), ("heap", "zero"), (False, True)
    ):
        name = f"context_{bits}_{context}_{erratum}_{mode}"
        binary = out / "build" / name
        log = out / (name + ("-canary" if canary else "") + ".log")
        command = [str(binary), "canary" if canary or explicit else "normal"]
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        with log.open("wb") as stream:
            status = _run_owned_test_process(command, env=env, timeout=15, stdout=stream)
        data = log.read_bytes()
        cases = 24 if context == "ordinary" else 34
        prefix = b"FAIL: early canary: failure survives all later controls\n" if canary else b""
        pattern = prefix + f"freertos cv context: cases={cases} checks=([0-9]+) failures={int(canary)}\n".encode()
        match = re.fullmatch(pattern, data)
        valid = status == int(canary) and match is not None and int(match[1]) >= 180
        rows.append({"profile": name, "canary": canary, "command": command,
                     "status": status, "valid": valid, "elf_sha256_before_run": digest,
                     "log": str(log), "sha256": hashlib.sha256(data).hexdigest()})
        (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(name + ("-canary" if canary else ""), "PASS" if valid else "FAIL", flush=True)
        if not valid:
            print(data.decode(errors="backslashreplace"), end="")
        # A setup rejection, crash or incomplete footer invalidates the cohort.
        if status not in (0, 1) or not re.search(rb"^freertos cv context: cases=[0-9]+ checks=[0-9]+ failures=[0-9]+\n\Z", data, re.MULTILINE):
            return 1
    failed = sum(not row["valid"] for row in rows)
    print(f"freertos cv context gate: observations={len(rows)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
