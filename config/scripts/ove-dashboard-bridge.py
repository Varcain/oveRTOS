#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""
oveRTOS dashboard bridge — shared-memory to WebSocket.

Reads the framebuffer from /dev/shm/ove-fb and audio ring from
/dev/shm/ove-audio (written by the QEMU guest through semihosting),
and streams them to the browser dashboard over WebSocket.

Started by qemu-run.sh for native QEMU runs.  In the browser the page
does this job itself (sim/dashboard/qemu-wasm.js).

Shared-memory and reader threads post frames to the asyncio server.
"""

import argparse
import asyncio
import os
import sys
import threading
import time
import traceback

# Find the sibling package when Python omits the script directory.
sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))

import ove_manifest
from ove_dashboard import gdb as gdb_bridge
from ove_dashboard import profiler, server, shm, sources

# ── Main ─────────────────────────────────────────────────────────────

def main():
    # Parent may hand us stdout/stderr in non-blocking mode (observed
    # under qemu-run.sh where `qemu | tee fifo` shares the terminal
    # with this process and Zephyr's boot chatter fills the pipe).
    # Progress lines must not abort startup with BlockingIOError.
    for _std in (sys.stdout, sys.stderr):
        try:
            os.set_blocking(_std.fileno(), True)
        except (AttributeError, OSError, ValueError):
            pass

    parser = argparse.ArgumentParser(
        description="oveRTOS dashboard bridge (shmem → WebSocket)")
    parser.add_argument("--host", default=server.DEFAULT_HOST,
                        help="Dashboard bind address (default: %(default)s)")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--dashboard", type=str, default=None,
                        help="Path to dashboard static files")
    parser.add_argument("--log-fd", type=int, default=-1,
                        help="File descriptor to read console log from")
    parser.add_argument("--console-fifo", type=str, default=None,
                        help="FIFO feeding the firmware's console UART; "
                             "dashboard console input is written to it")
    parser.add_argument("--gdb-port", type=int, default=0,
                        help="GDB remote port (QEMU gdbserver)")
    parser.add_argument("--gdb-toolchain", type=str, default="gdb",
                        help="GDB binary name (e.g. arm-none-eabi-gdb)")
    parser.add_argument("--elf-path", type=str, default=None,
                        help="Path to ELF for debug symbols")
    parser.add_argument("--build-dir", type=str, default=None,
                        help="CMake build dir (for source file list)")
    args = parser.parse_args()

    if args.console_fifo:
        # O_RDWR: opening a FIFO write-only would block until QEMU opens it.
        server.console_fd[0] = os.open(args.console_fifo, os.O_RDWR)

    if args.dashboard:
        dashboard_dir = args.dashboard
    else:
        dashboard_dir = os.path.join(sources.OVE_DIR, "sim", "dashboard")
    if not os.path.isdir(dashboard_dir):
        print(f"ERROR: Dashboard not found at {dashboard_dir}",
              file=sys.stderr)
        sys.exit(1)

    # Build source file list from CMake build objects.
    if args.build_dir:
        sources.build_project_file_list(args.build_dir, sources.OVE_DIR)

    # Board and feature manifest the dashboard draws, served at /manifest.json.
    workspace = ove_manifest.find_workspace(args.build_dir) if args.build_dir else None
    server.manifest.update(ove_manifest.build(
        workspace, sources.OVE_DIR, debugger=bool(args.gdb_port),
        console=bool(args.console_fifo), log=lambda msg: print(msg, flush=True)))

    # Build profiler symbol map from the ELF. The firmware is ARM and
    # system nm may not handle the architecture on every host — when
    # --gdb-toolchain points at arm-none-eabi-gdb, derive the matching
    # arm-none-eabi-nm alongside it and feed it to build_profiler_symbols.
    if args.elf_path:
        nm_bin, objdump_bin = profiler.profiler_tools(args.gdb_toolchain)
        if nm_bin is None:
            sys.stderr.write(
                "[profiler] WARNING: no cross nm next to --gdb-toolchain "
                f"({args.gdb_toolchain or 'not given'}); trying host nm — "
                "profiler symbols may be missing\n")
            nm_bin = "nm"
        profiler.build_profiler_symbols(args.elf_path, nm_bin=nm_bin)
        # CFG validator: reject phantom frames left over from stale
        # saved-{r7,lr} pairs in functions with large local frames, for the
        # stack-scan unwinder. Needs the cross-toolchain objdump.
        if objdump_bin:
            profiler._build_profiler_cfg(args.elf_path, objdump_bin=objdump_bin)
        else:
            sys.stderr.write(
                "[profiler] WARNING: no cross objdump next to --gdb-toolchain; "
                "phantom-frame pruning is disabled\n")
        sys.stderr.flush()

    threading.Thread(target=shm.display_bridge, daemon=True).start()
    threading.Thread(target=shm.audio_bridge, daemon=True).start()
    threading.Thread(target=shm.event_bridge, daemon=True).start()
    if args.log_fd >= 0:
        threading.Thread(target=shm.log_bridge, args=(args.log_fd,),
                         daemon=True).start()

    # Start GDB controller if configured.
    if args.gdb_port:
        gdb = gdb_bridge.GdbController(
            toolchain=args.gdb_toolchain,
            elf_path=args.elf_path,
            gdb_port=args.gdb_port,
        )
        gdb_bridge.gdb_controller[0] = gdb
        # Start GDB in a thread — connection may take a moment.
        def _start_gdb():
            # Wait for target: QEMU needs time to start its gdbserver.
            time.sleep(1)
            try:
                gdb.start()
                print(f"GDB connected ({args.gdb_toolchain})", flush=True)
            except Exception:
                print("GDB connection failed:", flush=True)
                traceback.print_exc()
        threading.Thread(target=_start_gdb, daemon=True).start()

    try:
        asyncio.run(server.serve(args.port, dashboard_dir, host=args.host))
    except KeyboardInterrupt:
        gdb = gdb_bridge.gdb_controller[0]
        if gdb:
            gdb.stop()


if __name__ == "__main__":
    main()
