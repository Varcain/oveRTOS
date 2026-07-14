#!/usr/bin/env python3
"""Hard-float guest context regression for FreeRTOS on QEMU MPS2-AN500.

The self-test guest repeatedly seeds and verifies s0-s31 plus FPSCR across an
unknown deferred syscall and a blocking poll.
"""

import os
import re
import subprocess
import sys


log = sys.argv[1] if len(sys.argv) > 1 else "/tmp/fpcheck_drive.log"

try:
    env = os.environ.copy()
    env["QEMU_TIMEOUT"] = "75"
    with open(log, "wb") as output:
        subprocess.run([".venv/bin/ove", "run"], stdin=subprocess.DEVNULL,
                       stdout=output, stderr=subprocess.STDOUT, env=env,
                       timeout=100)
except subprocess.TimeoutExpired:
    subprocess.run(["pkill", "-9", "-f", "qemu-system-arm"])

text = (open(log, "rb").read().decode("latin-1", "replace")
        if os.path.exists(log) else "")

passed = "hardfloat-fp-ok loops=100" in text
phase1_ok = "[demo] phase 1 OK" in text
host_completed = "interop demo done (hard-float self-test exited)" in text
no_fp_failure = not re.search(r"hardfloat-fp-(FAIL|context-fail)", text)
no_kernel_fault = not re.search(
    r"HardFault|Default_Handler|PANIC|panic|Assertion|MPU FAULT|FATAL", text)

print(f"fp_passed={passed} phase1_ok={phase1_ok} "
      f"host_completed={host_completed} no_fp_failure={no_fp_failure} "
      f"no_kernel_fault={no_kernel_fault}")
ok = passed and phase1_ok and host_completed and no_fp_failure and no_kernel_fault
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
