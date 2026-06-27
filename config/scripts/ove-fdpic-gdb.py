# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ove-fdpic-gdb.py — source-level GDB debugging of an FDPIC userspace program running inside the
# oveRTOS Linux personality.
#
# An FDPIC exec is loaded at RUNTIME addresses: the loadmap relocates each segment independently
# (text is shared in-place from the embedded cpio; RW data lives in the per-process region), so the
# on-disk ELF's link addresses do not match memory and plain `file prog.elf` gives wrong line/symbol
# mappings. The personality publishes each live slot's runtime segment bases in the GDB-readable
# table `g_ove_lnx_dbg[]` (filled by launch() in backends/common/ove_lnx_run.c). This script reads
# that table and does `add-symbol-file <elf> -o <text_base>`, which lines the program's
# source/symbols up with the running code — breakpoints by function/line, backtraces, and locals
# then work.
#
# Two gotchas (verified end-to-end against dbgdemo on QEMU):
#   * Relocate with `-o <text_base>` (offset every symbol by the text load bias), NOT
#     `-s .text <text_base>`: a dynamically-linked FDPIC exec's .text *section* has a non-zero link
#     vaddr (it sits after the dynamic sections), so placing the section AT text_base lands functions
#     at the wrong address. `-o` is correct for code, .rodata and stack-resident locals. Data globals
#     live in the RW segment at a *different* bias (data_base) — map those separately if you need them
#     (`add-symbol-file <elf> -s .data <data_base>`).
#   * The text is shared in-place from the cpio in FLASH, where a SOFTWARE breakpoint cannot be
#     written — use `hbreak` (Cortex-M hardware breakpoint), not `break`.
# Requires a Python-enabled, ARM-capable GDB (e.g. gdb-multiarch); the bare arm-none-eabi-gdb
# toolchain builds ship without Python and cannot `source` this file.
#
# Usage (from a GDB attached to the firmware's gdbstub, e.g. qemu -gdb tcp::1234):
#   (gdb) source config/scripts/ove-fdpic-gdb.py
#   (gdb) ove-fdpic-debug dbgdemo /path/to/dbgdemo        # wait for the next exec of "dbgdemo", map it
#   (gdb) hbreak add                                      # HARDWARE bp (text is in flash)
#   (gdb) continue                                        # hit add() with full source + backtrace
# or, if the program is already loaded and you know nothing is execing:
#   (gdb) ove-fdpic-map dbgdemo /path/to/dbgdemo          # map whatever live slot is named "dbgdemo"
import gdb


def _nslot():
    return int(gdb.parse_and_eval("(int)(sizeof(g_ove_lnx_proc)/sizeof(g_ove_lnx_proc[0]))"))


def _basename(s):
    return s.lstrip("-").rsplit("/", 1)[-1]


def _map_slot(sidx, elf):
    d = gdb.parse_and_eval("g_ove_lnx_dbg[%d]" % sidx)
    text = int(d["text_base"]) & 0xFFFFFFFF
    data = int(d["data_base"]) & 0xFFFFFFFF
    # `-o text_base` shifts every symbol by the text segment's load bias — correct for code/.rodata/
    # stack locals even when the .text section's link vaddr is non-zero (dynamically-linked execs).
    # Data globals sit at a different FDPIC bias (data_base); map them on demand with `-s .data`.
    gdb.execute("add-symbol-file %s -o 0x%x" % (elf, text))
    print("[ove-fdpic] slot %d mapped: text bias=0x%08x (RW data segment base=0x%08x)" % (sidx, text, data))
    print("[ove-fdpic] text is flash-resident (shared cpio) -> use `hbreak`, not `break`.")


class OveFdpicMap(gdb.Command):
    """ove-fdpic-map <comm> <elf>: map an ALREADY-loaded FDPIC program's symbols at its runtime bases."""

    def __init__(self):
        super().__init__("ove-fdpic-map", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        comm, elf = arg.split()
        for s in range(_nslot()):
            p = gdb.parse_and_eval("g_ove_lnx_proc[%d]" % s)
            if not int(p["alive"]):
                continue
            if _basename(p["comm"].string()) == comm:
                _map_slot(s, elf)
                return
        print("[ove-fdpic] no live slot named %r" % comm)


class OveFdpicDebug(gdb.Command):
    """ove-fdpic-debug <comm> <elf>: run until <comm> is exec'd, then map its symbols for source debug."""

    def __init__(self):
        super().__init__("ove-fdpic-debug", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        comm, elf = arg.split()
        bp = gdb.Breakpoint("launch", internal=True)
        sidx = None
        try:
            while True:
                gdb.execute("continue")
                fr = gdb.selected_frame()
                if fr is None or fr.name() != "launch":
                    print("[ove-fdpic] stopped outside launch(); aborting")
                    return
                if _basename(gdb.parse_and_eval("argv[0]").string()) == comm:
                    sidx = int(gdb.parse_and_eval("sidx"))
                    break
        finally:
            pass
        gdb.execute("finish")  # run launch to its return: g_ove_lnx_dbg[sidx] is now filled
        bp.delete()
        _map_slot(sidx, elf)
        print("[ove-fdpic] %s ready — set HARDWARE breakpoints (e.g. `hbreak main`) and `continue`" % comm)


OveFdpicMap()
OveFdpicDebug()
print("[ove-fdpic] commands: ove-fdpic-debug <comm> <elf> | ove-fdpic-map <comm> <elf>")
