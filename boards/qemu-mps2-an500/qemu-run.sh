#!/bin/bash
# Run an ELF binary on QEMU MPS2-AN500 with semihosting and display viewer.
#
# Usage: qemu-run.sh <elf-file> [--headless] [--machine <name>] [extra-qemu-args...]
#
# The display viewer launches automatically. Pass --headless to disable it.
#
# Options:
#   --headless       Run without the display viewer.
#   --machine <name> QEMU machine (default: mps2-an500).
#
# Exit code is propagated from the guest via semihosting.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OVE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VENV_PYTHON="${OVE_DIR}/.venv/bin/python"
VIEWER="${OVE_DIR}/config/scripts/qemu-display-viewer.py"

ELF="${1:?Usage: $0 <elf-file> [--headless] [--machine <name>] [extra-qemu-args...]}"
shift

QEMU_MACHINE="mps2-an500"
HEADLESS=0
NO_NET=0
QEMU_TIMEOUT=""
EXTRA_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --headless)
            HEADLESS=1
            ;;
        --no-net)
            NO_NET=1
            ;;
        --timeout)
            shift
            QEMU_TIMEOUT="${1:?--timeout requires seconds}"
            ;;
        --machine)
            shift
            QEMU_MACHINE="${1:?--machine requires an argument}"
            ;;
        *)
            EXTRA_ARGS+=("$1")
            ;;
    esac
    shift
done

QEMU_ARGS=(
    -M "${QEMU_MACHINE}"
    -nographic
    -kernel "${ELF}"
    -monitor none
)

VIEWER_PID=""
NET_BRIDGE_PID=""
AUDIO_PATH="/dev/shm/ove-audio"
NET_PATH="/dev/shm/ove-net"
NET_BRIDGE="${OVE_DIR}/config/scripts/qemu-net-bridge.py"
cleanup() {
    if [ -n "${VIEWER_PID}" ]; then
        kill "${VIEWER_PID}" 2>/dev/null || true
        sleep 0.2 2>/dev/null || true
        wait "${VIEWER_PID}" 2>/dev/null || true
    fi
    if [ -n "${NET_BRIDGE_PID}" ]; then
        kill "${NET_BRIDGE_PID}" 2>/dev/null || true
        wait "${NET_BRIDGE_PID}" 2>/dev/null || true
    fi
    rm -f "${AUDIO_PATH}" "${NET_PATH}"
}
trap cleanup EXIT

if [ "${HEADLESS}" -eq 0 ]; then
    FB_PATH="/dev/shm/ove-fb"

    : > "${FB_PATH}"
    truncate -s 512K "${FB_PATH}"

    # Audio shared-memory ringbuffer (header 64B + 2x 128KB rings)
    : > "${AUDIO_PATH}"
    truncate -s 262208 "${AUDIO_PATH}"

    QEMU_ARGS+=(
        -semihosting-config "enable=on,target=native,arg=${FB_PATH}"
    )

    "${VENV_PYTHON}" "${VIEWER}" &
    VIEWER_PID=$!
else
    QEMU_ARGS+=(
        -semihosting-config "enable=on,target=native"
    )
fi

# Network shared-memory bridge (runs in both headless and non-headless modes)
if [ "${NO_NET}" -eq 0 ] && [ -e /dev/net/tun ]; then
    : > "${NET_PATH}"
    truncate -s 131136 "${NET_PATH}"  # 64B hdr + 2x 64KB rings
    "${VENV_PYTHON}" "${NET_BRIDGE}" &
    NET_BRIDGE_PID=$!
fi

if [ -n "${QEMU_TIMEOUT}" ]; then
    timeout --foreground "${QEMU_TIMEOUT}" qemu-system-arm "${QEMU_ARGS[@]}" "${EXTRA_ARGS[@]}"
    QEMU_EXIT=$?
    if [ ${QEMU_EXIT} -eq 124 ]; then
        echo "ERROR: QEMU timed out after ${QEMU_TIMEOUT}s" >&2
    fi
else
    qemu-system-arm "${QEMU_ARGS[@]}" "${EXTRA_ARGS[@]}"
    QEMU_EXIT=$?
fi

cleanup
trap - EXIT
exit ${QEMU_EXIT}
