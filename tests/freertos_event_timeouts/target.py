# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check initialized ordinary and restricted Event callers on Cortex-M7."""
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


def expected_output(mpu, abort, zero, canary=False):
    workers = (5 + 2 * int(abort)) * (1 + int(mpu))
    checks = (29 + 17 * int(abort)) * (1 + int(mpu)) + 1 + int(mpu) + 2 * int(not zero)
    prefix = "FAIL: early failure survives later native controls\n" if canary else ""
    return (prefix + f"freertos event native: mpu={int(mpu)} abort={int(abort)} "
            f"heap={int(not zero)} ticks=32 workers={workers} checks={checks + int(canary)} "
            f"failures={int(canary)}\n").encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--freertos", required=True, type=Path)
    parser.add_argument("--toolchain", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-arm")
    args = parser.parse_args()
    rows = []
    metadata = args.output.resolve() / "tests/freertos_event_timeouts/target-observations.json"
    metadata.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    explicit = env.get("OVE_FREERTOS_EVENT_TARGET_CANARY") == "1"
    for mpu in (False, True):
        for abort in (False, True):
            for zero in (False, True):
                label = ("v1-" if mpu else "ordinary-") + ("abort-" if abort else "default-")
                label += "zero" if zero else "heap"
                folder = args.output.resolve() / "qemu-mps2-an500/freertos" / ("event_timeouts_" + label)
                folder.mkdir(parents=True, exist_ok=True)
                configure = ["cmake", "-S", str(HERE / "target"), "-B", str(folder / "build"),
                             "-DOVE_TOOLCHAIN_DIR=" + str(args.toolchain.resolve()),
                             "-DCMAKE_TOOLCHAIN_FILE=" + str(ROOT / "boards/qemu-mps2-an500/cmake/arm-none-eabi.cmake"),
                             "-DFREERTOS_PATH=" + str(args.freertos.resolve()),
                             "-DOVE_MPU_V1=" + ("ON" if mpu else "OFF"),
                             "-DOVE_ZERO_HEAP=" + ("ON" if zero else "OFF"),
                             "-DOVE_NATIVE_ABORT=" + ("ON" if abort else "OFF"),
                             "-DOVE_EVENT_TARGET_CANARY=" + ("ON" if explicit else "OFF")]
                for stage, command in (
                    ("configure", configure),
                    ("build", ["cmake", "--build", str(folder / "build"), "--parallel", "8"]),
                ):
                    log = folder / (stage + ".log")
                    with log.open("wb") as stream:
                        status = _run_owned_test_process(command, env=env, timeout=240, stdout=stream)
                    row = {"profile": label, "stage": stage, "command": command, "status": status,
                           "log": str(log), "sha256": hashlib.sha256(log.read_bytes()).hexdigest()}
                    rows.append(row)
                    metadata.write_text(json.dumps(rows, indent=2) + "\n")
                    if status:
                        print(log.read_text(errors="backslashreplace"), end="")
                        return 1
                binary = folder / "build/event_timeout_target"
                digest = hashlib.sha256(binary.read_bytes()).hexdigest()
                command = [args.qemu, "-M", "mps2-an500", "-cpu", "cortex-m7", "-m", "16",
                           "-nographic", "-monitor", "none", "-semihosting-config", "enable=on,target=native",
                           "-kernel", str(binary)]
                log = folder / "run.log"
                with log.open("wb") as stream:
                    status = _run_owned_test_process(command, env=env, timeout=30, stdout=stream)
                data = log.read_bytes()
                valid = not explicit and status == 0 and data == expected_output(mpu, abort, zero)
                row = {"profile": label, "stage": "run", "command": command, "status": status,
                       "valid": valid, "elf_sha256_before_run": digest, "log": str(log),
                       "sha256": hashlib.sha256(data).hexdigest(),
                       "qualifier": "real native32/1k callers;restricted objects statically granted in both allocation configs;heap owner probe privileged;no native64/long-wrap/physicalIRQ claim"}
                rows.append(row)
                log.with_suffix(".status.json").write_text(json.dumps(row, indent=2) + "\n")
                metadata.write_text(json.dumps(rows, indent=2) + "\n")
                print(label, "PASS" if valid else "FAIL", flush=True)
                if not valid:
                    print(data.decode(errors="backslashreplace"), end="")
    observations = [row for row in rows if row["stage"] == "run"]
    if len(rows) != 24 or len(observations) != 8:
        raise ValueError("incomplete native Event observations")
    failed = sum(not row["valid"] for row in observations)
    print(f"freertos event native gate: observations={len(observations)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
