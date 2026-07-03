#!/usr/bin/env python3
# evdev touch input under the FreeRTOS Linux personality (QEMU mps2-an500).
#
# Runs /usr/bin/evread, which blocking-reads /dev/input/event0 and prints the
# single-touch coordinates the QEMU testpad injector produces — exercising the
# personality's evdev class, input feeder, and the park/retry + coordinator-kick
# path (a blocking read on an empty ring). Asserts a 16-byte input_event layout,
# several touch reports, and shell survival. Same piped-stdin shape as the other
# *_drive.py tests. Run from the repo root with the linux_interop workspace.
#
# Usage: evread_drive.py [logfile]
import subprocess, sys, re, os

log = sys.argv[1] if len(sys.argv) > 1 else "/tmp/evread_drive.log"

seq = (r"printf 'root\n'; sleep 5; "
       r"printf 'evread; echo ERC=$?\n'; sleep 8; "
       r"printf 'uname -a\n'; sleep 3; "
       r"printf 'poweroff\n'; sleep 4")
cmd = f"( {seq} ) | QEMU_TIMEOUT=120 .venv/bin/ove run --headless > {log} 2>&1"

try:
    subprocess.run(["/bin/sh", "-c", cmd], timeout=170)
except subprocess.TimeoutExpired:
    subprocess.run(["pkill", "-9", "-f", "qemu-system-arm"])

text = open(log, "rb").read().decode("latin-1", "replace") if os.path.exists(log) else ""

size_ok    = "evread: input_event size=16" in text          # kernel/uClibc layout agree
touches    = re.findall(r"evread: touch x=(\d+) y=(\d+) down=1", text)
touch_ok   = len(touches) >= 3                               # the injector fed a drag
done_ok    = bool(re.search(r"evread: DONE got=[1-9]", text)) and "ERC=0" in text
shell_ok   = "Linux overtos" in text
no_kfault  = not re.search(r"HardFault|up_assert|PANIC|panic|Assertion|MPU FAULT|FATAL", text)

print(f"size16={size_ok} touches={len(touches)} done={done_ok} "
      f"shell_survived={shell_ok} no_kernel_fault={no_kfault}")
ok = size_ok and touch_ok and done_ok and shell_ok and no_kfault
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
