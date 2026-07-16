#!/usr/bin/env python3
# Kernel-hardening regression for the Zephyr Linux personality (QEMU mps2-an521, Cortex-M33). Workspace = the FULL board name qemu-mps2-an521.zephyr.linux_interop.
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

# Two 'root' lines: the account has a password
# (BR2_TARGET_GENERIC_ROOT_PASSWD), so login prompts for name and password.
# With only the first, the next command is eaten as the password.
seq = (r"printf 'root\n'; sleep 5; "
       r"printf 'root\n'; sleep 5; "
       r"printf 'kstress ptr; echo KRC=$?\n'; sleep 5; "
       r"printf 'kstress str; echo SRC=$?\n'; sleep 4; "
       r"printf 'kstress udf; echo URC=$?\n'; sleep 5; "
       r"printf 'kstress div0; echo DRC=$?\n'; sleep 4; "
       r"printf 'kstress rsrc; echo RRC=$?\n'; sleep 4; "
       r"printf 'kstress elf; echo ERC=$?\n'; sleep 4; "
       r"printf 'uname -a\n'; sleep 3; "
       r"printf 'poweroff\n'; sleep 4")
cmd = f"( {seq} ) | QEMU_TIMEOUT=150 .venv/bin/ove run > {log} 2>&1"

try:
    subprocess.run(["/bin/sh", "-c", cmd], timeout=210)
except subprocess.TimeoutExpired:
    subprocess.run(["pkill", "-9", "-f", "qemu-system-arm"])

text = open(log, "rb").read().decode("latin-1", "replace") if os.path.exists(log) else ""

ptr_ok      = "[kstress] ptr: PASS" in text and "KRC=0" in text  # every bad pointer -> EFAULT
str_ok      = "[kstress] str: PASS" in text and "SRC=0" in text  # unterminated path -> EFAULT
udf_ran     = "[kstress] udf: executing an undefined instruction" in text
udf_ok      = "URC=139" in text and "[kstress] udf: NOT CONTAINED" not in text  # UsageFault contained
div0_ok     = ("DRC=0" in text or "DRC=139" in text)             # trapped(139) OR benign(0): either policy
rsrc_ok     = "[kstress] rsrc: PASS" in text and "RRC=0" in text # fd table exhausted -> EMFILE, no OOB
# execve of a non-ELF either returns -ENOEXEC (-> "elf: PASS", ERC=0) or the personality tears the
# caller down as a rejected load (ERC=127); both leave the kernel + shell intact.
elf_ok      = ("[kstress] elf: PASS" in text and "ERC=0" in text) or ("ERC=127" in text)
shell_alive = "Linux overtos" in text                            # uname ran after all -> survived
# Kernel-console panic markers differ per engine (Zephyr's go to a null UART); shell_alive + the RCs
# are the definitive signal — a crash shows as shell_alive=False + missing RCs (QEMU timed out).
no_kfault   = not re.search(r"HardFault|up_assert|PANIC|panic|Assertion|MPU FAULT|FATAL", text)

print(f"ptr={ptr_ok} str={str_ok} udf={udf_ran and udf_ok} div0={div0_ok} "
      f"rsrc={rsrc_ok} elf={elf_ok} shell_survived={shell_alive} no_kernel_fault={no_kfault}")
ok = (ptr_ok and str_ok and udf_ran and udf_ok and div0_ok and rsrc_ok and elf_ok
      and shell_alive and no_kfault)
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
