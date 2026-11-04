# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run bounded trusted-model arena checks and optional safe mutations."""

import argparse
import hashlib
import json
import os
import signal
import subprocess
import time
from pathlib import Path


def stop(_signal, _frame):
    # Unwind the active case's finally block if an outer deadline ends the run.
    raise SystemExit(143)


signal.signal(signal.SIGTERM, stop)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
parser.add_argument("--zero-heap", action="store_true")
parser.add_argument("--sanitize", action="store_true")
parser.add_argument("--mutations", action="store_true")
args = parser.parse_args()
build = args.build.resolve()
rows = []
assertions = {}
cases = [("infer_arena_cpp", [], 0)]
for case in [
    "overlap",
    "persistent",
    "temp",
    "resizable",
    "resize",
    "reserve",
    "entries",
    "offline-entries",
    "plan",
    "live-scratch",
    "layout",
]:
    cases.append(("infer_arena_allocator", [case], 0))
for case in ["minimum", "alignment", "schema", "zero", "range"]:
    cases.append(("infer_arena_frontiers", [case], 0))
cases.append(("infer_arena_models", ["tiny-static"], 0))
if not args.zero_heap:
    cases.append(("infer_arena_models", ["tiny-heap"], 0))
for mode in ["static"] + ([] if args.zero_heap else ["heap"]):
    for model in ["sine", "preprocessor", "classifier"]:
        cases.append(("infer_arena_models", ["measure", model, mode], 0))
    for model in ["preprocessor", "classifier"]:
        cases.append(("infer_arena_models", ["candidate", model, mode], 0))
    for case in ["aligned"] + (["unaligned"] if mode == "static" else []):
        cases.append(("infer_arena_models", ["query", case, mode], 0))
    for case in ["unaligned", "version", "unregistered"]:
        cases.append(("infer_arena_models", ["contract", case, mode], 0))
if args.mutations:
    for variant in json.loads((build / "arena-mutations/variants.json").read_text()):
        for case in variant["cases"] + (
            [] if args.zero_heap else variant.get("heap_cases", [])
        ):
            binary = "infer_arena_mut_" + variant["name"]
            cases.append((binary, case.split(), 1))
            assertions[(binary, case)] = variant["assertions"][case]

out = build / "arena-results"
out.mkdir(exist_ok=True)
for index, (binary, command_args, expected) in enumerate(cases):
    env = dict(os.environ)
    # Override inherited sanitizer settings: every new case must release backing.
    env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
    env["LSAN_OPTIONS"] = "exitcode=23"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    command = [str(build / binary), *command_args]
    log = out / f"{index:03d}.log"
    start = time.monotonic()
    timed_out = False
    with log.open("wb") as stream:
        process = subprocess.Popen(
            command,
            env=env,
            stdout=stream,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            rc = process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            timed_out = True
            rc = 124
        finally:
            # Kill only the process group created for this case, then reap it.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    output = log.read_text(errors="replace")
    diagnostics = any(
        tag in output
        for tag in [
            "AddressSanitizer",
            "LeakSanitizer",
            "runtime error:",
            "UndefinedBehaviorSanitizer",
        ]
    )
    expected_marker = (
        "assertion failed:" in output
        or "frontier failed:" in output
        or "FAIL busy_tiny:" in output
    )
    good = (
        rc == expected
        and not timed_out
        and not diagnostics
        and (
            expected == 0
            or (
                expected_marker
                and assertions[(binary, " ".join(command_args))] in output
            )
        )
    )
    rows.append(
        {
            "command": command,
            "pid": process.pid,
            "rc": rc,
            "reaped_rc": process.returncode,
            "expected": expected,
            "pass": good,
            "sanitizers": args.sanitize,
            "seconds": time.monotonic() - start,
            "log": str(log),
            "log_sha256": hashlib.sha256(log.read_bytes()).hexdigest(),
        }
    )
    (out / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    print(index, binary, " ".join(command_args), "PASS" if good else "FAIL", flush=True)
    if not good:
        print(output)
        raise SystemExit(1)
print(f"Trusted-model arena checks: {len(rows)} passed")
