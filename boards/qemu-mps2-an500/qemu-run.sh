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
VIEWER="${OVE_DIR}/config/scripts/ove-dashboard-bridge.py"

# Resolve ARM toolchain path for GDB.
TC_SENTINEL="${OVE_DIR}/output/toolchains/path.txt"
if [ -f "${TC_SENTINEL}" ]; then
    TC_DIR="$(cat "${TC_SENTINEL}")"
    ARM_GDB="${TC_DIR}/bin/arm-none-eabi-gdb"
else
    ARM_GDB="arm-none-eabi-gdb"
fi

ELF="${1:?Usage: $0 <elf-file> [--headless] [--machine <name>] [extra-qemu-args...]}"
shift

QEMU_MACHINE="mps2-an500"
HEADLESS=0
NO_NET=0
NO_GDB=0
GDB_PORT=1234
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
        --no-gdb)
            NO_GDB=1
            ;;
        --gdb-port)
            shift
            GDB_PORT="${1:?--gdb-port requires a port number}"
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

# ── Linux personality build: headless, interactive semihosting console ───────
# The personality is no-sim; route SYS_READC/SYS_WRITEC to a dedicated stdio
# chardev (serial/monitor/display off) so the shell can read keystrokes —
# `target=native` + `-nographic` (the sim path below) does NOT route stdin to
# semihosting. Detected via CONFIG_OVE_LINUX in the workspace .config.
PERSONALITY_CFG="$(dirname "$(realpath "${ELF}")")/../.config"
if [ -f "${PERSONALITY_CFG}" ] && grep -q '^CONFIG_OVE_LINUX=y' "${PERSONALITY_CFG}"; then
    # The rootfs cpio is XIP'd from PSRAM @ 0x60000000 (not embedded in the ELF — keeps the
    # 11 MB rootfs off the 4 MB internal flash; the QEMU analog of the STM32 QSPI-NOR rootfs).
    # Inject the raw cpio into PSRAM at reset with -device loader. Resolve its path the way
    # cmake/OveLinuxFixtures.cmake does: <OVE_BUILDROOT>/<OVE_LINUX_ROOTFS_OUTPUT>/images/.
    _br="$(sed -n 's/^CONFIG_OVE_BUILDROOT="\(.*\)"$/\1/p' "${PERSONALITY_CFG}")"; _br="${_br:-../buildroot}"
    _ro="$(sed -n 's/^CONFIG_OVE_LINUX_ROOTFS_OUTPUT="\(.*\)"$/\1/p' "${PERSONALITY_CFG}")"; _ro="${_ro:-output}"
    case "${_br}" in
        /*) ROOTFS_CPIO="${_br}/${_ro}/images/rootfs.cpio" ;;
        *)  ROOTFS_CPIO="${OVE_DIR}/${_br}/${_ro}/images/rootfs.cpio" ;;
    esac
    if [ ! -f "${ROOTFS_CPIO}" ]; then
        echo "[qemu-run] ERROR: rootfs.cpio not found at ${ROOTFS_CPIO} (build Buildroot first)" >&2
        exit 1
    fi
    PERS_ARGS=(
        -machine "${QEMU_MACHINE}" -m 16
        # Program console = CMSDK UART1 on stdio (non-blocking-pollable: interactive
        # top's 'q' quit). UART0 (serial0) = the engine's own console, discarded.
        # Semihosting → null (kept only for the clean SYS_EXIT).
        -semihosting-config enable=on,chardev=c0 -chardev null,id=c0
        -serial none -serial stdio -monitor none -display none
        -kernel "${ELF}"
        # Rootfs XIP'd from PSRAM: raw cpio loaded to 0x60000000 at reset.
        -device "loader,file=${ROOTFS_CPIO},addr=0x60000000,force-raw=on"
    )
    # GDB server (enabled by default; --no-gdb turns it off): just opens the port, the firmware
    # still boots normally. Enables turnkey FDPIC source-level debugging via
    # config/scripts/ove-fdpic-gdb.py (`target remote :GDB_PORT` + `ove-fdpic-auto <comm> <elf>`).
    if [ "${NO_GDB}" -eq 0 ]; then
        PERS_ARGS+=( -gdb "tcp::${GDB_PORT}" )
    fi
    SAVED_TTY=""
    if [ -t 0 ]; then
        SAVED_TTY="$(stty -g 2>/dev/null || true)"
        stty -icanon -echo -icrnl min 1 time 0 2>/dev/null || true
    fi
    pers_restore() { [ -n "${SAVED_TTY}" ] && stty "${SAVED_TTY}" 2>/dev/null || true; }
    trap pers_restore EXIT INT TERM
    if [ -n "${QEMU_TIMEOUT}" ]; then
        timeout --foreground "${QEMU_TIMEOUT}" qemu-system-arm "${PERS_ARGS[@]}" "${EXTRA_ARGS[@]}"
    else
        qemu-system-arm "${PERS_ARGS[@]}" "${EXTRA_ARGS[@]}"
    fi
    exit $?
fi

# Kill any stale QEMU instance writing to the same shm files.
STALE_QEMU=$(pgrep -f "qemu-system-arm.*ove-fb" 2>/dev/null || true)
if [ -n "${STALE_QEMU}" ]; then
    echo "[qemu-run] Killing stale QEMU process(es): ${STALE_QEMU}"
    kill ${STALE_QEMU} 2>/dev/null || true
    sleep 0.3
fi

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
SIM_PATH="/dev/shm/ove-sim"
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
    rm -f "${AUDIO_PATH}" "${NET_PATH}" "${SIM_PATH}" "${LOG_FIFO:-}"
}
trap cleanup EXIT

if [ "${HEADLESS}" -eq 0 ]; then
    FB_PATH="/dev/shm/ove-fb"

    : > "${FB_PATH}"
    truncate -s 1M "${FB_PATH}"  # header 20B + XRGB8888 pixels (480x272x4 = 522KB)

    # Audio shared-memory ringbuffer: 2x ove_sim_audio_ring (32B hdr + 64KB buf each)
    : > "${AUDIO_PATH}"
    truncate -s 131136 "${AUDIO_PATH}"

    # Plugin events/commands SHM (header 64B + 2x 64KB rings)
    : > "${SIM_PATH}"
    truncate -s 131136 "${SIM_PATH}"

    QEMU_ARGS+=(
        -semihosting-config "enable=on,target=native,arg=${FB_PATH}"
    )

    # GDB server: enabled by default (firmware boots normally, dashboard can pause).
    if [ "${NO_GDB}" -eq 0 ]; then
        QEMU_ARGS+=( -gdb "tcp::${GDB_PORT}" )
    fi

    # Kill any stale dashboard bridge on the same port.
    DASHBOARD_PORT=8081
    STALE_PID=$(lsof -ti tcp:${DASHBOARD_PORT} 2>/dev/null || true)
    if [ -n "${STALE_PID}" ]; then
        kill ${STALE_PID} 2>/dev/null || true
        sleep 0.3
    fi

    # Named pipe: QEMU stdout → tee → terminal + bridge log.
    LOG_FIFO=$(mktemp -u -t ove-log.XXXXXX)
    mkfifo "${LOG_FIFO}"

    # Derive CMake build dir from ELF path for source file indexing.
    # ELF is in .../images/firmware.elf, build objects are in:
    #   FreeRTOS: .../build/firmware/
    #   Zephyr:   .../build/firmware/
    #   NuttX:    .../build/nuttx-cmake/
    ELF_DIR="$(dirname "$(realpath "${ELF}")")"
    WS_DIR="${ELF_DIR%/images}"
    BUILD_DIR="${WS_DIR}/build/firmware"
    if [ ! -d "${BUILD_DIR}" ]; then
        BUILD_DIR="${WS_DIR}/build/nuttx-cmake"
    fi
    if [ ! -d "${BUILD_DIR}" ]; then
        BUILD_DIR="${WS_DIR}/build"
    fi

    VIEWER_ARGS=( --port ${DASHBOARD_PORT} --log-fd 0 --build-dir "${BUILD_DIR}" )
    if [ "${NO_GDB}" -eq 0 ]; then
        VIEWER_ARGS+=( --gdb-port "${GDB_PORT}" --gdb-toolchain "${ARM_GDB}" --elf-path "${ELF}" )
    fi

    "${VENV_PYTHON}" "${VIEWER}" "${VIEWER_ARGS[@]}" < "${LOG_FIFO}" &
    VIEWER_PID=$!

    # Wait for the dashboard HTTP server to be ready before starting QEMU.
    for i in $(seq 1 30); do
        if curl -s -o /dev/null http://localhost:${DASHBOARD_PORT}/ 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
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
    if [ -n "${LOG_FIFO:-}" ]; then
        timeout --foreground "${QEMU_TIMEOUT}" qemu-system-arm "${QEMU_ARGS[@]}" "${EXTRA_ARGS[@]}" 2>&1 | tee "${LOG_FIFO}"
        QEMU_EXIT=${PIPESTATUS[0]}
    else
        timeout --foreground "${QEMU_TIMEOUT}" qemu-system-arm "${QEMU_ARGS[@]}" "${EXTRA_ARGS[@]}"
        QEMU_EXIT=$?
    fi
    if [ ${QEMU_EXIT} -eq 124 ]; then
        echo "ERROR: QEMU timed out after ${QEMU_TIMEOUT}s" >&2
    fi
else
    if [ -n "${LOG_FIFO:-}" ]; then
        qemu-system-arm "${QEMU_ARGS[@]}" "${EXTRA_ARGS[@]}" 2>&1 | tee "${LOG_FIFO}"
        QEMU_EXIT=${PIPESTATUS[0]}
    else
        qemu-system-arm "${QEMU_ARGS[@]}" "${EXTRA_ARGS[@]}"
        QEMU_EXIT=$?
    fi
fi

cleanup
trap - EXIT
exit ${QEMU_EXIT}
