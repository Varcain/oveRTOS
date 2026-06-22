#!/usr/bin/env bash
#
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build + run the RTOS<->Linux interop demo on QEMU mps2/an521 (Cortex-M33).
# Phase 1 runs automatically; phase 2 drops into an interactive BusyBox shell —
# type commands (ls /, echo hi, cat /etc/hostname, pwd, ...) and `exit` to quit.
#
# Reuses the Zephyr workspace the personality test fetches; if it is missing,
# run `make test-qemu-zephyr-linux` (or `ove test qemu-zephyr-linux`) once first.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OVE="$(cd "$HERE/../../.." && pwd)"
WS="$(ls -d "$OVE"/dl/zephyr-workspace-*/zephyr 2>/dev/null | head -1)"
[ -d "$WS" ] || { echo "Zephyr workspace not found; run 'make test-qemu-zephyr-linux' once."; exit 1; }
BUILD="$OVE/output/apps/linux_interop"

ZEPHYR_BASE="$WS" "$OVE/.venv/bin/west" build -b mps2/an521/cpu0 -d "$BUILD" "$HERE"

# Interactive phase 2 needs each keystroke (not whole lines) and lets the shell
# do the echo: put the terminal in raw-ish mode and restore it on exit. The
# console reaches the firmware through the semihosting chardev (a dedicated
# stdio chardev so SYS_READC/SYS_WRITEC use this terminal's stdin/stdout).
SAVED=""
if [ -t 0 ]; then
	SAVED="$(stty -g)"
	stty -icanon -echo -icrnl min 1 time 0
fi
restore() { [ -n "$SAVED" ] && stty "$SAVED" || true; }
trap restore EXIT INT TERM

qemu-system-arm -cpu cortex-m33 -machine mps2-an521 -m 16 \
	-semihosting-config enable=on,chardev=c0 -chardev stdio,id=c0 \
	-serial none -monitor none -display none \
	-kernel "$BUILD/zephyr/zephyr.elf"
