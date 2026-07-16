#!/usr/bin/env python3
# Negative MPU-isolation test for the FreeRTOS-MPU Linux personality (QEMU mps2-an500).
#
# Boots the firmware, logs in, and runs /usr/bin/segv — a tiny unprivileged Linux program that
# deliberately writes to kernel SRAM (0x20000000), which lies OUTSIDE its per-task MPU regions.
# Under the ARM_CM4_MPU port this raises a MemManage fault, which the personality must CONTAIN:
# segv is killed like a default-action SIGSEGV (exit 139) and the shell survives. A broken
# isolation would instead let the store return (segv prints its "FAILED" line), fault the kernel
# (HardFault / Default_Handler / panic), or hang — all of which this test catches.
#
# The keystrokes are fed by a plain shell pipeline (printf ... | ove run > log), NOT a Python
# pty/subprocess reader: QEMU's mps2-an500 ARM-MPU *emulation* is timing-fragile, and any
# host-side reader (a pty's line discipline, a draining thread, even a Python parent's
# GIL/scheduler jitter) perturbs it enough to corrupt the unrelated phase-1 program. The shell
# pipeline with output redirected to a file is the only stable shape. Output is parsed afterward.
#
# Requires the active workspace to be the an500 FreeRTOS linux_interop build (ove run uses it):
#   ove defconfig-fragments qemu.freertos.linux_interop && ove build
# Run from the repo root.
#
# Usage: segv_drive.py [logfile]
import subprocess, sys, re, os

log = sys.argv[1] if len(sys.argv) > 1 else "/tmp/segv_drive.log"

# Two 'root' lines: the account has a password
# (BR2_TARGET_GENERIC_ROOT_PASSWD), so login prompts for name and password.
# With only the first, the next command is eaten as the password.
seq = (r"printf 'root\n'; sleep 5; "
       r"printf 'root\n'; sleep 5; "
       r"printf 'segv; echo SEGV_RC=$?\n'; sleep 4; "
       r"printf 'uname -a\n'; sleep 3; "
       r"printf 'poweroff\n'; sleep 4")
cmd = f"( {seq} ) | QEMU_TIMEOUT=75 .venv/bin/ove run > {log} 2>&1"

try:
    subprocess.run(["/bin/sh", "-c", cmd], timeout=120)
except subprocess.TimeoutExpired:
    subprocess.run(["pkill", "-9", "-f", "qemu-system-arm"])

text = open(log, "rb").read().decode("latin-1", "replace") if os.path.exists(log) else ""

qemu_corrupt = bool(re.search(r"unimplemented syscall nr=\d{5,}|FAIL: phase-1", text))
ran = "[segv] write kernel SRAM" in text             # the program started
contained = "[segv] isolation FAILED" not in text    # the kernel-SRAM store did NOT return
killed = "SEGV_RC=139" in text                        # killed by SIGSEGV (128 + 11)
shell_alive = "Linux overtos" in text                 # uname ran -> the shell survived
no_kfault = not re.search(r"HardFault|Default_Handler|panic|FAULT", text)

print(f"ran={ran} contained={contained} killed(139)={killed} "
      f"shell_survived={shell_alive} no_kernel_fault={no_kfault} qemu_mpu_emul_corrupt={qemu_corrupt}")
ok = ran and contained and killed and shell_alive and no_kfault
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
