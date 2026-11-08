# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check actual SysTick/daemon delivery and native MPU V1 teardown."""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--freertos", required=True, type=Path)
    parser.add_argument("--toolchain", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-arm")
    args = parser.parse_args()
    rows = []
    manifest = args.output.resolve() / "tests/eventgroup_deferred/target-observations.json"
    manifest.parent.mkdir(parents=True, exist_ok=True)

    def command(argv, folder, name, expected=None):
        result = subprocess.run(argv, capture_output=True,
                                timeout=30 if expected else 240, check=False)
        data = result.stdout + result.stderr
        log = folder / (name + ".log")
        log.write_bytes(data)
        valid = result.returncode == 0 and (expected is None or data == expected.encode())
        row = {"name": name, "command": argv, "status": result.returncode,
               "valid": valid, "log": str(log), "sha256": hashlib.sha256(data).hexdigest()}
        if expected is not None:
            row["elf_sha256"] = hashlib.sha256((folder / "build/eventgroup_target").read_bytes()).hexdigest()
        rows.append(row)
        log.with_suffix(".status.json").write_text(json.dumps(row, indent=2) + "\n")
        manifest.write_text(json.dumps(rows, indent=2) + "\n")
        if not valid:
            print(data.decode(errors="backslashreplace"), end="")
            raise RuntimeError("EventGroup target check failed: " + name)

    for mpu in (False, True):
        for zero in (False, True):
            label = ("mpu-" if mpu else "daemon-") + ("zero" if zero else "heap")
            folder = args.output.resolve() / "qemu-mps2-an500/freertos" / ("eventgroup_deferred_" + label)
            folder.mkdir(parents=True, exist_ok=True)
            configure = ["cmake", "-S", str(HERE / "target"), "-B", str(folder / "build"),
                         "-DOVE_TOOLCHAIN_DIR=" + str(args.toolchain.resolve()),
                         "-DCMAKE_TOOLCHAIN_FILE=" + str(ROOT / "boards/qemu-mps2-an500/cmake/arm-none-eabi.cmake"),
                         "-DFREERTOS_PATH=" + str(args.freertos.resolve()),
                         "-DOVE_MPU_V1=" + ("ON" if mpu else "OFF"),
                         "-DOVE_DEFERRED_CANARY=" + ("ON" if os.environ.get("OVE_EVENTGROUP_DEFERRED_TARGET_CANARY") == "1" else "OFF"),
                         "-DOVE_ZERO_HEAP=" + ("ON" if zero else "OFF")]
            command(configure, folder, label + "-configure")
            command(["cmake", "--build", str(folder / "build"), "--parallel", "8"], folder, label + "-build")
            expected = ("eventgroup deferred mpu: checks=8 failures=0 entry=1 exit=1 regions=16\n" if mpu
                        else f"eventgroup deferred target: checks={18 if zero else 22} failures=0 "
                             f"ipsr=15 early=0 delivered=1 fences={3 if zero else 4}\n")
            command([args.qemu, "-M", "mps2-an500", "-cpu", "cortex-m7", "-m", "16",
                     "-nographic", "-monitor", "none", "-semihosting-config", "enable=on,target=native",
                     "-kernel", str(folder / "build/eventgroup_target")], folder, label + "-run", expected)
            print(label, "PASS", flush=True)
    if len(rows) != 12:
        raise ValueError("incomplete EventGroup target observations")
    print("eventgroup deferred target: observations=4 failed=0", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
