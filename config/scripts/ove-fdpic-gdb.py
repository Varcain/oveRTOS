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
# that table and does `add-symbol-file <elf> -s .text <text_base> -s .data <data_base>`, which lines
# the program's source/symbols up with the running code — breakpoints by function/line, backtraces,
# and locals then work.
#
# Usage (from a GDB attached to the firmware's gdbstub, e.g. qemu -gdb tcp::1234):
#   (gdb) source config/scripts/ove-fdpic-gdb.py
#   (gdb) ove-fdpic-debug dbgdemo /path/to/dbgdemo        # wait for the next exec of "dbgdemo", map it
#   (gdb) break add
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
    gdb.execute("add-symbol-file %s -s .text 0x%x -s .data 0x%x" % (elf, text, data))
    print("[ove-fdpic] slot %d mapped: .text=0x%08x .data=0x%08x" % (sidx, text, data))


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
        print("[ove-fdpic] %s ready — set breakpoints (e.g. `break main`) and `continue`" % comm)


OveFdpicMap()
OveFdpicDebug()
print("[ove-fdpic] commands: ove-fdpic-debug <comm> <elf> | ove-fdpic-map <comm> <elf>")
