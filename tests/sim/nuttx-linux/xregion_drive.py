#!/usr/bin/env python3
# Negative INTER-PROGRAM MPU-isolation test for the NuttX Linux personality (QEMU mps2-an500).
#
# Boots the firmware, logs in, and runs /usr/bin/xregion — a tiny unprivileged Linux program that
# writes to a SIBLING program's pool region (the 512K MPU region just above its own, derived from
# the running sp so no pool base is hardcoded). Where segv.c writes kernel SRAM (0x20000000, denied
# by the ARM default map — that proves KERNEL isolation), this exercises the Phase-2 INTER-PROGRAM
# path: the store lands in the privileged-only whole-pool BASE MPU region (unpriv-NO), which the
# per-program regions 2+3 override only for the program's OWN region — so it raises a MemManage
# fault, which the personality CONTAINS (kills xregion like a default-action SIGSEGV, exit 139) and
# the shell survives. Broken inter-program isolation would instead let the store return (xregion
# prints its "FAILED" line), fault the kernel (HardFault / up_assert / panic), or hang — all caught.
#
# Same shell-pipeline shape as segv_drive.py (QEMU's an500 MPU emulation is timing-fragile, so any
# host-side pty/reader perturbs the unrelated phase-1 program; keystrokes go through a plain shell
# pipeline with output redirected to a file, parsed afterward).
#
# Requires the active workspace to be the an500 NuttX linux_interop build (ove run uses it):
#   ove defconfig-fragments qemu.nuttx.linux_interop && ove build
# Run from the repo root.
#
# Usage: xregion_drive.py [logfile]
import subprocess, sys, re, os

log = sys.argv[1] if len(sys.argv) > 1 else "/tmp/xregion_drive.log"

# Two 'root' lines: the account has a password
# (BR2_TARGET_GENERIC_ROOT_PASSWD), so login prompts for name and password.
# With only the first, the next command is eaten as the password.
seq = (r"printf 'root\n'; sleep 5; "
       r"printf 'root\n'; sleep 5; "
       r"printf 'xregion; echo XREG_RC=$?\n'; sleep 4; "
       r"printf 'uname -a\n'; sleep 3; "
       r"printf 'poweroff\n'; sleep 4")
cmd = f"( {seq} ) | QEMU_TIMEOUT=75 .venv/bin/ove run > {log} 2>&1"

try:
    subprocess.run(["/bin/sh", "-c", cmd], timeout=120)
except subprocess.TimeoutExpired:
    subprocess.run(["pkill", "-9", "-f", "qemu-system-arm"])

text = open(log, "rb").read().decode("latin-1", "replace") if os.path.exists(log) else ""

qemu_corrupt = bool(re.search(r"unimplemented syscall nr=\d{5,}|FAIL: phase-1", text))
ran = "[xregion] write sibling pool region" in text    # the program started
contained = "[xregion] isolation FAILED" not in text   # the sibling-region store did NOT return
killed = "XREG_RC=139" in text                          # killed by SIGSEGV (128 + 11)
shell_alive = "Linux overtos" in text                   # uname ran -> the shell survived
no_kfault = not re.search(r"HardFault|up_assert|PANIC|panic|Assertion", text)

print(f"ran={ran} contained={contained} killed(139)={killed} "
      f"shell_survived={shell_alive} no_kernel_fault={no_kfault} qemu_mpu_emul_corrupt={qemu_corrupt}")
ok = ran and contained and killed and shell_alive and no_kfault
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
