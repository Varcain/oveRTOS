#!/usr/bin/env python3
# Kernel-hardening regression for the NuttX Linux personality (QEMU mps2-an500).
#
# Runs /usr/bin/kstress, which attacks the KERNEL through paths the MPU isolation does not cover:
#   ptr - feeds write() a kernel / device / unmapped pointer; the syscall layer's access_ok must
#         reject each (-EFAULT) so the PRIVILEGED handler never dereferences it. Prints "ptr: PASS".
#         An unhardened kernel crashes here (the privileged deref of an unmapped pointer faults).
#   udf - executes an undefined instruction (UsageFault); the fault handler must CONTAIN it (exit
#         139), not escalate to a HardFault / panic. A MemManage-only handler crashes here.
# The shell must survive both and poweroff cleanly. Same shell-pipeline shape as segv_drive.py (the
# Cortex-M MPU emulation is timing-fragile — no host-side pty/reader). Run from the repo root with the
# active workspace = this engine's linux_interop build (ove run uses it).
#
# Usage: kstress_drive.py [logfile]
import subprocess, sys, re, os

log = sys.argv[1] if len(sys.argv) > 1 else "/tmp/kstress_drive.log"

seq = (r"printf 'root\n'; sleep 5; "
       r"printf 'kstress ptr; echo KRC=$?\n'; sleep 5; "
       r"printf 'kstress udf; echo URC=$?\n'; sleep 5; "
       r"printf 'uname -a\n'; sleep 3; "
       r"printf 'poweroff\n'; sleep 4")
cmd = f"( {seq} ) | QEMU_TIMEOUT=90 .venv/bin/ove run > {log} 2>&1"

try:
    subprocess.run(["/bin/sh", "-c", cmd], timeout=150)
except subprocess.TimeoutExpired:
    subprocess.run(["pkill", "-9", "-f", "qemu-system-arm"])

text = open(log, "rb").read().decode("latin-1", "replace") if os.path.exists(log) else ""

ptr_ok      = "[kstress] ptr: PASS" in text and "KRC=0" in text  # every bad pointer -> EFAULT
udf_ran     = "[kstress] udf: executing an undefined instruction" in text
udf_ok      = "URC=139" in text and "[kstress] udf: NOT CONTAINED" not in text  # UsageFault contained
shell_alive = "Linux overtos" in text                            # uname ran after both -> survived
# Kernel-console panic markers differ per engine (Zephyr's go to a null UART); shell_alive + the RCs
# are the definitive signal — a crash shows as shell_alive=False + missing KRC/URC (QEMU timed out).
no_kfault   = not re.search(r"HardFault|up_assert|PANIC|panic|Assertion|MPU FAULT|FATAL", text)

print(f"ptr_rejected={ptr_ok} udf_contained={udf_ran and udf_ok} "
      f"shell_survived={shell_alive} no_kernel_fault={no_kfault}")
ok = ptr_ok and udf_ran and udf_ok and shell_alive and no_kfault
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
