#!/usr/bin/env bash
#
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build + run the RTOS<->Linux interop demo on QEMU mps2/an521 (Cortex-M33).
# Reuses the Zephyr workspace the personality test fetches; if it is missing,
# run `make test-qemu-zephyr-linux` (or `ove test qemu-zephyr-linux`) once first.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OVE="$(cd "$HERE/../../.." && pwd)"
WS="$(ls -d "$OVE"/dl/zephyr-workspace-*/zephyr 2>/dev/null | head -1)"
[ -d "$WS" ] || { echo "Zephyr workspace not found; run 'make test-qemu-zephyr-linux' once."; exit 1; }
BUILD="$OVE/output/apps/linux_interop"

ZEPHYR_BASE="$WS" "$OVE/.venv/bin/west" build -b mps2/an521/cpu0 -d "$BUILD" "$HERE"
exec qemu-system-arm -cpu cortex-m33 -machine mps2-an521 -m 16 -nographic -semihosting \
	-kernel "$BUILD/zephyr/zephyr.elf"
