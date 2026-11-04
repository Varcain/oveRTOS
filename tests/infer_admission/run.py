# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run native admission cases with distinct intentional-retention policy."""

import argparse
import json
import os
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
parser.add_argument("--zero-heap", action="store_true")
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
controlled = [
    "busy-tiny-static",
    "pair-static-static",
    "model-failure",
    "register-failure",
    "setup-failure",
    "static-tails",
    "static-failure-tails",
    "old-static-deinit",
    "consume",
    "invalid",
    "parallel",
]
if not args.zero_heap:
    controlled += [
        "busy-tiny-heap",
        "pair-heap-heap",
        "pair-heap-static",
        "pair-static-heap",
        "header-failure",
        "arena-failure",
        "heap-model-failure",
        "heap-register-failure",
        "heap-setup-failure",
        "heap-tails",
        "heap-failure-tails",
        "old-static-destroy",
        "old-heap-destroy",
    ]
sequential = ["sequential-static"] + ([] if args.zero_heap else ["sequential-heap"])
retained = ["static-static"] + (
    [] if args.zero_heap else ["heap-heap", "heap-static", "static-heap"]
)
rows = []
for binary, cases, retention in [
    ("infer_controlled", controlled, False),
    ("infer_real", sequential, False),
    ("infer_real", retained, True),
]:
    for case in cases:
        env = dict(os.environ)
        if args.sanitize:
            env["ASAN_OPTIONS"] = f"detect_leaks={int(not retention)}:halt_on_error=1"
            env["UBSAN_OPTIONS"] = "halt_on_error=1"
        # The binaries create threads but no subprocesses. Inherit the outer
        # owned process group so its deadline also covers any active case.
        try:
            result = subprocess.run(
                [str(args.build / binary), case], env=env, timeout=30, check=False
            )
            rc = result.returncode
        except subprocess.TimeoutExpired:
            rc = 124
        rows.append(
            {
                "binary": binary,
                "case": case,
                "intentional_retention": retention,
                "rc": rc,
            }
        )
        (args.build / "admission-results.json").write_text(
            json.dumps(rows, indent=2) + "\n"
        )
        if rc:
            raise SystemExit(1)
print(f"Native inference admission: {len(rows)} cases passed")
