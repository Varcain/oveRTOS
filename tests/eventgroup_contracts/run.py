# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build real eventgroup providers and verify complete native observations."""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXPECTED = {
    "eventgroup_posix": ("eventgroup provider posix", (53, 41)),
    "eventgroup_freertos": ("eventgroup provider freertos-declared-null", (31, 23)),
    "eventgroup_nuttx": ("eventgroup provider nuttx-declared-error", (31, 23)),
    "eventgroup_zephyr": ("eventgroup provider zephyr-void-init", (9, 5)),
    "eventgroup_context_posix": ("eventgroup context", (6, 6)),
    "eventgroup_context_freertos": ("eventgroup context", (5, 5)),
    "eventgroup_context_nuttx": ("eventgroup context", (5, 5)),
    "eventgroup_context_zephyr": ("eventgroup context", (5, 5)),
    "eventgroup_integration": ("eventgroup integration", (49, 49)),
}


def command(args):
    subprocess.run(args, check=True, timeout=180)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    explicit = os.environ.get("OVE_EVENTGROUP_CONTRACTS_CANARY") == "1"
    rows = []

    def observe(binary, name, canary, expected, qualifier):
        active = canary or explicit
        argv = [str(binary)] + (["canary"] if active else [])
        result = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=15, check=False)
        stem = name + ("-canary" if canary else "-normal")
        log = out / (stem + ".log")
        log.write_bytes(result.stdout)
        record = {"command": argv, "status": result.returncode,
                  "log_sha256": hashlib.sha256(result.stdout).hexdigest(),
                  "elf_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                  "qualifier": qualifier}
        log.with_suffix(".status.json").write_text(json.dumps(record, indent=2) + "\n")
        wanted = expected[active]
        valid = not explicit and result.returncode == int(active) and result.stdout == wanted
        rows.append({"name": stem, "valid": valid, **record})
        print(stem, "PASS" if valid else "FAIL", flush=True)
        if not valid:
            print(result.stdout.decode(errors="backslashreplace"), end="")
        (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")

    for zero in (False, True):
        mode = "zero" if zero else "heap"
        build = out / mode
        command(["cmake", "-S", str(HERE), "-B", str(build),
                 "-DOVE_ZERO_HEAP=" + ("ON" if zero else "OFF"),
                 "-DOVE_SANITIZE=" + ("ON" if args.sanitize else "OFF")])
        command(["cmake", "--build", str(build), "--parallel", "8"])
        for name, (label, counts) in EXPECTED.items():
            count = counts[int(zero)]
            texts = [(f"{label}: checks={count} failures=0\n").encode(),
                     (f"FAIL: early failure survives controls\n{label}: "
                      f"checks={count + 1} failures=1\n").encode()]
            for canary in (False, True):
                observe(build / name, mode + "-" + name, canary, texts,
                        "real POSIX integration" if name == "eventgroup_integration"
                        else "inert native observer; FreeRTOS/NuttX errors are declared results")

    models = out / "context-models"
    build_args = [sys.executable, str(HERE / "context_models.py"), str(models)]
    command(build_args + (["--sanitize"] if args.sanitize else []))
    for image in json.loads((models / "images.json").read_text()):
        texts = [image["normal_text"].encode(), image["canary_text"].encode()]
        for canary in (False, True):
            observe(Path(image["binary"]), image["profile"], canary, texts,
                    "metadata context model; architectural reads substituted explicitly")
    if len(rows) != 84:
        raise ValueError("incomplete eventgroup observation set")
    failed = sum(not row["valid"] for row in rows)
    print(f"eventgroup contracts: observations={len(rows)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
