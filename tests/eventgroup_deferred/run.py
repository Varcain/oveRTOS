# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check bounded deferred ownership, publication and native task access."""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROFILES = {"heap": (10, False), "zero": (10, True), "mpu_heap": (10, False),
            "mpu_zero": (10, True), "minimum": (1, False), "nondefault": (3, True)}
COUNTS = {"queued": 11, "delivery": 8, "reuse": 8, "pressure": 7, "tail": 8,
          "acquisition": 5, "zero": 11, "privilege": 7, "owners": 6, "reentry": 6}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    rows, commands = [], []
    explicit = os.environ.get("OVE_EVENTGROUP_DEFERRED_CANARY") == "1"
    for name, command in (
        ("configure", ["cmake", "-S", str(HERE), "-B", str(out / "build"),
                       "-DOVE_SANITIZE=" + ("ON" if args.sanitize else "OFF")]),
        ("build", ["cmake", "--build", str(out / "build"), "--parallel", "8"]),
    ):
        result = subprocess.run(command, capture_output=True, timeout=180, check=False)
        log = out / (name + ".log")
        log.write_bytes(result.stdout + result.stderr)
        commands.append({"command": command, "status": result.returncode,
                         "log": str(log), "sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode:
            print(log.read_text(errors="backslashreplace"), end="")
            return 1
    for profile, (depth, zero) in PROFILES.items():
        counts = dict(COUNTS, rollback=2 * depth + 9, heap=11 if zero else 6)
        binary = out / "build" / ("deferred_" + profile)
        for case, count in counts.items():
            for canary in (False, True):
                active = canary or explicit
                command = [str(binary), case] + (["canary"] if active else [])
                result = subprocess.run(command, capture_output=True, timeout=15, check=False)
                data = result.stdout + result.stderr
                text = "FAIL: early failure survives later controls\n" if active else ""
                text += f"eventgroup deferred {case}: checks={count + int(active)} failures={int(active)}\n"
                valid = not explicit and result.returncode == int(active) and data == text.encode()
                stem = profile + "-" + case + ("-canary" if canary else "-normal")
                log = out / (stem + ".log")
                log.write_bytes(data)
                row = {"name": stem, "command": command, "status": result.returncode,
                       "valid": valid, "log": str(log),
                       "log_sha256": hashlib.sha256(data).hexdigest(),
                       "elf_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                       "qualifier": "inert queue/native observers; logical retirement precedes physical cleanup"}
                log.with_suffix(".status.json").write_text(json.dumps(row, indent=2) + "\n")
                rows.append(row)
                (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
                print(stem, "PASS" if valid else "FAIL", flush=True)
                if not valid:
                    print(data.decode(errors="backslashreplace"), end="")
    if len(rows) != 144:
        raise ValueError("incomplete deferred EventGroup observations")
    failed = sum(not row["valid"] for row in rows)
    print(f"eventgroup deferred: observations={len(rows)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
