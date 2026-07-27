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
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OVE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
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

ove_ws_dir() {
    local d
    d="$(dirname "$(realpath "$1")")"
    while [ "${d}" != "/" ]; do
        if [ -f "${d}/.config" ]; then
            printf '%s\n' "${d}"
            return 0
        fi
        d="$(dirname "${d}")"
    done
    return 1
}

OVE_WS_DIR="$(ove_ws_dir "${ELF}" || true)"
PERSONALITY_CFG="${OVE_WS_DIR:-$(dirname "$(realpath "${ELF}")")}/.config"
PERS_ARGS=()
if [ -f "${PERSONALITY_CFG}" ] && grep -q '^CONFIG_OVE_LINUX=y' "${PERSONALITY_CFG}"; then
    _br="$(sed -n 's/^CONFIG_OVE_BUILDROOT="\(.*\)"$/\1/p' "${PERSONALITY_CFG}")"
    _br="${_br:-../buildroot}"
    _ro="$(sed -n 's/^CONFIG_OVE_LINUX_ROOTFS_OUTPUT="\(.*\)"$/\1/p' "${PERSONALITY_CFG}")"
    _ro="${_ro:-output}"
    case "${_br}" in
        /*) ROOTFS_CPIO="${_br}/${_ro}/images/rootfs.cpio" ;;
        *)  ROOTFS_CPIO="${OVE_DIR}/${_br}/${_ro}/images/rootfs.cpio" ;;
    esac
    if [ ! -f "${ROOTFS_CPIO}" ]; then
        echo "[qemu-run] ERROR: rootfs.cpio not found at ${ROOTFS_CPIO} (build Buildroot first)" >&2
        exit 1
    fi
    ROOTFS_SIZE="$(stat -c %s "${ROOTFS_CPIO}")"
    if [ "${ROOTFS_SIZE}" -gt $((0x00ef0000)) ]; then
        echo "[qemu-run] ERROR: rootfs.cpio is ${ROOTFS_SIZE} bytes; AN521 PSRAM window is $((0x00ef0000))" >&2
        exit 1
    fi
    PERS_ARGS=(-device "loader,file=${ROOTFS_CPIO},addr=0x80000000,force-raw=on")
fi

QEMU_ARGS=(
    -cpu cortex-m33 -machine mps2-an521 -m 16
    # Program console = CMSDK UART1 on stdio (non-blocking-pollable: interactive top's
    # 'q' quit). UART0 (serial0) = the engine's own console, discarded. Semihosting →
    # null (kept only for the clean SYS_EXIT).
    -semihosting-config enable=on,chardev=c0 -chardev null,id=c0
    -serial none -serial stdio -monitor none -display none
    -kernel "${ELF}"
    "${PERS_ARGS[@]}"
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
