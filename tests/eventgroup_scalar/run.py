# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check event masks, admission ordering and native snapshot exclusion."""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
COUNTS = {"posix": 20, "nuttx": 21, "zephyr": 23,
          "freertos16": 41, "freertos32": 41, "freertos64": 20}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    explicit = os.environ.get("OVE_EVENTGROUP_SCALAR_CANARY") == "1"
    rows, commands = [], []
    for zero in (False, True):
        mode = "zero" if zero else "heap"
        build = out / mode
        for stage, command in (
            ("configure", ["cmake", "-S", str(HERE), "-B", str(build),
                           "-DOVE_ZERO_HEAP=" + ("ON" if zero else "OFF"),
                           "-DOVE_SANITIZE=" + ("ON" if args.sanitize else "OFF")]),
            ("build", ["cmake", "--build", str(build), "--parallel", "8"]),
        ):
            result = subprocess.run(command, capture_output=True, timeout=180, check=False)
            log = out / (mode + "-" + stage + ".log")
            log.write_bytes(result.stdout + result.stderr)
            commands.append({"command": command, "status": result.returncode,
                             "log": str(log), "sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
            (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
            if result.returncode:
                print(log.read_text(errors="backslashreplace"), end="")
                return 1
        for name, count in COUNTS.items():
            binary = build / ("scalar_" + name)
            for canary in (False, True):
                active = canary or explicit
                command = [str(binary)] + (["canary"] if active else [])
                result = subprocess.run(command, capture_output=True, timeout=15, check=False)
                output = result.stdout + result.stderr
                expected = ("FAIL: early failure survives controls\n" if active else "")
                expected += f"eventgroup scalar: checks={count + int(active)} failures={int(active)}\n"
                valid = not explicit and result.returncode == int(active) and output == expected.encode()
                stem = mode + "-" + name + ("-canary" if canary else "-normal")
                log = out / (stem + ".log")
                log.write_bytes(output)
                row = {"name": stem, "command": command, "status": result.returncode,
                       "valid": valid, "log": str(log),
                       "log_sha256": hashlib.sha256(output).hexdigest(),
                       "elf_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                       "qualifier": "inert native model; 64-bit FreeRTOS is host arithmetic only"}
                log.with_suffix(".status.json").write_text(json.dumps(row, indent=2) + "\n")
                rows.append(row)
                (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
                print(stem, "PASS" if valid else "FAIL", flush=True)
                if not valid:
                    print(output.decode(errors="backslashreplace"), end="")
    failed = sum(not row["valid"] for row in rows)
    if len(rows) != 24:
        raise ValueError("incomplete eventgroup scalar observations")
    print(f"eventgroup scalar: observations={len(rows)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
