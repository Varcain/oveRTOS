#!/usr/bin/env python3
# Negative MPU-isolation test for the Zephyr Linux personality (QEMU mps2-an521, Cortex-M33).
#
# Boots the firmware, logs in, and runs /usr/bin/segv — a tiny UNPRIVILEGED Linux program that
# deliberately writes to kernel SRAM (0x20000000), outside its K_USER MPU domain. The MPU raises a
# fatal fault; Zephyr's default k_sys_fatal_error_handler HALTS the whole system, so the personality
# OVERRIDES it (backends/zephyr/zephyr_lnx.c): a faulting program is marked killed (exit 139) and the
# coordinator reaps it — the program dies like a default-action SIGSEGV and the shell survives, the
# same containment FreeRTOS/NuttX get from their MemManage handlers. Broken containment would instead
# hang the system (no SEGV_RC, QEMU runs to timeout) or let the store return (segv prints "FAILED").
#
# Same shell-pipeline shape as the freertos/nuttx segv_drive.py (piped keystrokes, output redirected
# to a file, parsed afterward — the Cortex-M MPU emulation is timing-fragile and dislikes host-side
# readers). NOTE the FULL board name for the workspace: `qemu.zephyr` prefix-matches the an500 and
# would drop USERSPACE, so the isolation build needs qemu-mps2-an521.zephyr.linux_interop.
#
# Usage: segv_drive.py [logfile]
import subprocess, sys, re, os

log = sys.argv[1] if len(sys.argv) > 1 else "/tmp/zephyr_segv_drive.log"

# Two 'root' lines: the account has a password
# (BR2_TARGET_GENERIC_ROOT_PASSWD), so login prompts for name and password.
# With only the first, the next command is eaten as the password.
seq = (r"printf 'root\n'; sleep 5; "
       r"printf 'root\n'; sleep 5; "
       r"printf 'segv; echo SEGV_RC=$?\n'; sleep 4; "
       r"printf 'uname -a\n'; sleep 3; "
       r"printf 'poweroff\n'; sleep 4")
cmd = f"( {seq} ) | QEMU_TIMEOUT=90 .venv/bin/ove run > {log} 2>&1"

try:
    subprocess.run(["/bin/sh", "-c", cmd], timeout=140)
except subprocess.TimeoutExpired:
    subprocess.run(["pkill", "-9", "-f", "qemu-system-arm"])

text = open(log, "rb").read().decode("latin-1", "replace") if os.path.exists(log) else ""

ran        = "[segv] write kernel SRAM" in text        # the program started
contained  = "[segv] isolation FAILED" not in text     # the kernel-SRAM store did NOT return
killed     = "SEGV_RC=139" in text                      # killed by SIGSEGV (128 + 11)
shell_alive= "Linux overtos" in text                    # uname ran AFTER segv -> the shell survived
# The Zephyr kernel console (UART0) is routed to null by the runner, so a panic would NOT reach this
# stdio log — a hang instead shows up as shell_alive=False + no SEGV_RC (QEMU ran to timeout).
no_kfault  = not re.search(r"MPU FAULT|FATAL|K_ERR|Halting system|>>> ZEPHYR", text)

print(f"ran={ran} contained={contained} killed(139)={killed} "
      f"shell_survived={shell_alive} no_kernel_fault={no_kfault}")
ok = ran and contained and killed and shell_alive and no_kfault
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
