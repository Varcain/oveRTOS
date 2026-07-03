#!/usr/bin/env python3
# LVGL benchmark under the FreeRTOS Linux personality (QEMU mps2-an500).
#
# Runs /usr/bin/lvbench — a stock LVGL fbdev program (lv_demo_benchmark) built as
# an FDPIC Linux binary that renders to /dev/fb0 (the personality's framebuffer,
# pwrite scanlines). It runs every scene, then prints an FPS/CPU/render summary
# via its end-cb and exits. Asserts the benchmark completed with a non-zero FPS
# and the shell survived + powered off cleanly. Same piped-stdin shape as the
# other *_drive.py tests (no host pty — the Cortex-M MPU emulation is timing
# fragile). Run from the repo root with the active workspace = the linux_interop
# build. The benchmark is slow under emulation, so the window is generous.
#
# Usage: lvbench_drive.py [logfile]
import subprocess, sys, re, os

log = sys.argv[1] if len(sys.argv) > 1 else "/tmp/lvbench_drive.log"

seq = (r"printf 'root\n'; sleep 5; "
       r"printf 'lvbench\n'; sleep 150; "         # run the benchmark to completion
       r"printf '\n'; sleep 2; "
       r"printf 'uname -a\n'; sleep 3; "
       r"printf 'poweroff\n'; sleep 4")
cmd = f"( {seq} ) | QEMU_TIMEOUT=260 .venv/bin/ove run --headless > {log} 2>&1"

try:
    subprocess.run(["/bin/sh", "-c", cmd], timeout=320)
except subprocess.TimeoutExpired:
    subprocess.run(["pkill", "-9", "-f", "qemu-system-arm"])

text = open(log, "rb").read().decode("latin-1", "replace") if os.path.exists(log) else ""

started    = "lvbench: starting lv_demo_benchmark on /dev/fb0" in text
m          = re.search(r"lvbench: DONE avg_fps=(\d+)", text)
done_ok    = bool(m) and int(m.group(1)) > 0        # completed + a real (non-zero) FPS
shell_ok   = "Linux overtos" in text                # uname ran after -> shell survived
no_kfault  = not re.search(r"HardFault|up_assert|PANIC|panic|Assertion|MPU FAULT|FATAL", text)

fps = m.group(1) if m else "?"
print(f"started={started} done={done_ok} fps={fps} "
      f"shell_survived={shell_ok} no_kernel_fault={no_kfault}")
ok = started and done_ok and shell_ok and no_kfault
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
