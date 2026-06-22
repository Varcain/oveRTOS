#!/usr/bin/env bash
#
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Run a QEMU MPS2-AN521 (Cortex-M33) firmware. This board targets the Linux
# personality, so it wires up an interactive semihosting console: the firmware's
# SYS_WRITEC/SYS_READC reach this terminal's stdout/stdin through a dedicated
# stdio chardev (the default -nographic semihosting does NOT route stdin), and
# the terminal is put in raw-ish mode so a personality shell sees each keystroke.
#
# Usage: qemu-run.sh <elf-file> [--headless] [--timeout <s>] [extra-qemu-args...]
set -u
ELF="${1:?Usage: $0 <elf-file> [--headless] [--timeout <s>] [extra-qemu-args...]}"
shift

QEMU_TIMEOUT=""
EXTRA_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --headless|--no-net|--no-gdb) ;;             # no display/net/gdb on this board
        --gdb-port) shift ;;
        --timeout) shift; QEMU_TIMEOUT="${1:-}" ;;
        --machine) shift ;;                          # machine is fixed (mps2-an521)
        *) EXTRA_ARGS+=("$1") ;;
    esac
    shift
done

QEMU_ARGS=(
    -cpu cortex-m33 -machine mps2-an521 -m 16
    -semihosting-config enable=on,chardev=c0 -chardev stdio,id=c0
    -serial none -monitor none -display none
    -kernel "${ELF}"
)

# Interactive personality shell: deliver each keystroke and let the guest echo.
SAVED=""
if [ -t 0 ]; then
    SAVED="$(stty -g 2>/dev/null || true)"
    stty -icanon -echo -icrnl min 1 time 0 2>/dev/null || true
fi
restore() { [ -n "${SAVED}" ] && stty "${SAVED}" 2>/dev/null || true; }
trap restore EXIT INT TERM

if [ -n "${QEMU_TIMEOUT}" ]; then
    timeout --foreground "${QEMU_TIMEOUT}" qemu-system-arm "${QEMU_ARGS[@]}" "${EXTRA_ARGS[@]}"
else
    qemu-system-arm "${QEMU_ARGS[@]}" "${EXTRA_ARGS[@]}"
fi
