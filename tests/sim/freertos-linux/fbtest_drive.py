#!/usr/bin/env python3
# Framebuffer /dev/fb0 smoke for the FreeRTOS Linux personality (QEMU mps2-an500).
#
# Runs /usr/bin/fbtest, which opens /dev/fb0, reads its geometry via
# FBIOGET_VSCREENINFO/FSCREENINFO, fills an RGB565 gradient, and verifies a
# pwrite()/pread() round-trip through a scanline (the exact positioned-write path
# LVGL's fbdev driver uses with LV_LINUX_FBDEV_MMAP=0). The shell must survive and
# poweroff cleanly. Same piped-stdin shape as segv_drive.py (the Cortex-M MPU
# emulation is timing-fragile — no host-side pty/reader). Run from the repo root
# with the active workspace = this engine's linux_interop build.
#
# Usage: fbtest_drive.py [logfile]
import subprocess, sys, re, os

log = sys.argv[1] if len(sys.argv) > 1 else "/tmp/fbtest_drive.log"

# Two 'root' lines: the account has a password
# (BR2_TARGET_GENERIC_ROOT_PASSWD), so login prompts for name and password.
# With only the first, the next command is eaten as the password.
seq = (r"printf 'root\n'; sleep 5; "
       r"printf 'root\n'; sleep 5; "
       r"printf 'fbtest; echo FRC=$?\n'; sleep 6; "
       r"printf 'ls -la /dev/fb0\n'; sleep 3; "
       r"printf 'uname -a\n'; sleep 3; "
       r"printf 'poweroff\n'; sleep 4")
cmd = f"( {seq} ) | QEMU_TIMEOUT=150 .venv/bin/ove run --headless > {log} 2>&1"

try:
    subprocess.run(["/bin/sh", "-c", cmd], timeout=210)
except subprocess.TimeoutExpired:
    subprocess.run(["pkill", "-9", "-f", "qemu-system-arm"])

text = open(log, "rb").read().decode("latin-1", "replace") if os.path.exists(log) else ""

geom_ok    = "fbtest: 480x272 16bpp" in text and "id=ovefb" in text
pass_ok    = "fbtest: PASS" in text and "FRC=0" in text        # ioctls + pwrite/pread round-trip
node_ok    = bool(re.search(r"crw.*29,\s*0.*/dev/fb0", text))  # /dev/fb0 is a char dev major 29
shell_ok   = "Linux overtos" in text                           # uname ran after -> shell survived
no_kfault  = not re.search(r"HardFault|up_assert|PANIC|panic|Assertion|MPU FAULT|FATAL", text)

print(f"geom={geom_ok} fbtest={pass_ok} devnode={node_ok} "
      f"shell_survived={shell_ok} no_kernel_fault={no_kfault}")
ok = geom_ok and pass_ok and node_ok and shell_ok and no_kfault
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
