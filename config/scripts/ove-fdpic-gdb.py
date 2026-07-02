# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ove-fdpic-gdb.py — turnkey source-level GDB debugging of an FDPIC userspace program (plus the
# ld.so and shared libraries it runs on) inside the oveRTOS Linux personality.
#
# An FDPIC object is loaded at RUNTIME addresses: its loadmap relocates each PT_LOAD segment
# independently (text is shared in-place from the embedded cpio, in FLASH; RW data lives in the
# per-process region / PSRAM), so the on-disk ELF's link addresses do not match memory and a plain
# `file prog.elf` gives wrong line/symbol mappings. Each object therefore needs symbols placed at its
# own text bias — and a dynamic program has THREE of them (the exec, ld-uClibc.so.0, libc.so.0).
#
# The personality publishes the main exec's runtime bases + its _DYNAMIC address in the GDB-readable
# table `g_ove_lnx_dbg[]` (filled by launch() in backends/common/ove_lnx_run.c). `ove-fdpic-auto`
# uses that to load the exec, runs to the exec's entry (by when ld.so has linked everything and
# patched the exec's _DYNAMIC[DT_DEBUG]), then walks the standard SVR4/FDPIC rendezvous
# (DT_DEBUG -> struct r_debug -> the uClibc `elf_resolve` link-map chain, whose FDPIC `l_addr` is a
# `{loadmap*, got}` pair) to add-symbol-file EVERY loaded object at its own FDPIC text bias. It then
# breakpoints ld.so's `_dl_debug_state` so a later dlopen re-runs the walk (true auto-solib).
#
# Two gotchas (verified end-to-end against dbgdemo on QEMU + real STM32F746):
#   * Relocate with `-o <text_bias>` (offset every symbol by the text load bias), NOT `-s .text ...`:
#     a dynamically-linked FDPIC ELF's .text SECTION has a non-zero link vaddr (it sits after the
#     dynamic sections), so placing the section AT the bias lands functions at the wrong address.
#     `-o` is correct for code/.rodata/line-info/stack-resident locals. Data globals live in the RW
#     segment at a different bias; `-s .data <data_base>` maps those (best-effort, added too).
#   * The text is shared in-place from the cpio in FLASH, where a SOFTWARE breakpoint cannot be
#     written — use `hbreak` (Cortex-M hardware breakpoint), not `break`.
# Requires a Python-enabled, ARM-capable GDB (the distro /usr/bin/gdb 17.x with --enable-targets=all
# works; the bare arm-none-eabi-gdb toolchain builds ship WITHOUT Python and cannot `source` this).
#
# Usage (from a GDB attached to the firmware's gdbstub, e.g. qemu -gdb tcp::1234):
#   (gdb) source config/scripts/ove-fdpic-gdb.py
#   (gdb) ove-fdpic-auto dbgdemo /path/to/dbgdemo   # wait for the next exec of "dbgdemo", load ALL
#                                                   # objects (exec + ld.so + libc) with full source
#   (gdb) hbreak main      # or `hbreak write` to step into libc — HARDWARE bp (text is in flash)
#   (gdb) continue
# The single-object helpers remain for a program already loaded / with no shared libs:
#   (gdb) ove-fdpic-map dbgdemo /path/to/dbgdemo    # map one already-loaded slot's exec symbols
#   (gdb) ove-fdpic-debug dbgdemo /path/to/dbgdemo  # run to the exec, map just its symbols
import gdb
import os
import struct

# ---- target memory helpers ------------------------------------------------------------------------
def _inf():
    return gdb.selected_inferior()


def _rd(addr, n):
    return bytes(_inf().read_memory(addr & 0xFFFFFFFF, n))


def _u32(addr):
    return struct.unpack("<I", _rd(addr, 4))[0]


def _i32(addr):
    return struct.unpack("<i", _rd(addr, 4))[0]


def _u16(addr):
    return struct.unpack("<H", _rd(addr, 2))[0]


def _cstr(addr, maxn=256):
    if not addr:
        return ""
    try:
        return _rd(addr, maxn).split(b"\0", 1)[0].decode("latin-1", "replace")
    except gdb.MemoryError:
        return "?"


def _basename(s):
    return s.lstrip("-").rsplit("/", 1)[-1]


def _nslot():
    return int(gdb.parse_and_eval("(int)(sizeof(g_ove_lnx_proc)/sizeof(g_ove_lnx_proc[0]))"))


# ---- FDPIC loadmap → per-object text/data bias ---------------------------------------------------
def _loadmap_bases(loadmap):
    """Return (text_bias, data_base) from an elf32_fdpic_loadmap at address `loadmap`.

    Layout: u16 version, u16 nsegs, then nsegs × {u32 addr, u32 p_vaddr, u32 p_memsz}. By the FDPIC
    convention seg[0] is the RX text segment and seg[1] the RW data segment. text_bias relocates
    every symbol (`-o`); data_base is where the RW segment landed (`-s .data`)."""
    if not loadmap:
        return None, None
    nsegs = _u16(loadmap + 2)
    segs = []
    for i in range(min(nsegs, 16)):
        b = loadmap + 4 + i * 12
        segs.append((_u32(b), _u32(b + 4), _u32(b + 8)))  # (addr, p_vaddr, p_memsz)
    if not segs:
        return None, None
    text_bias = (segs[0][0] - segs[0][1]) & 0xFFFFFFFF
    data_base = segs[1][0] if len(segs) > 1 else None
    return text_bias, data_base


def _add_symbols(elf, text_bias, data_base):
    if not os.path.exists(elf):
        print("[ove-fdpic] MISSING symbol file: %s" % elf)
        return False
    cmd = "add-symbol-file %s -o 0x%x" % (elf, text_bias)
    if data_base:
        cmd += " -s .data 0x%x" % (data_base & 0xFFFFFFFF)
    gdb.execute(cmd)
    return True


# ---- target path -> host ELF (with symbols) ------------------------------------------------------
def _host_elf(target_path, exec_comm, exec_elf, sysroot):
    """Resolve a link-map object name to a host ELF that carries debug info."""
    name = _basename(target_path)
    if name == exec_comm or target_path == exec_comm:
        return exec_elf  # the exec itself — the ELF the user handed us
    if not sysroot:
        return None
    # target paths look like "/lib/libc.so.0"; try sysroot + path, then follow into libuClibc*.so.
    cand = os.path.join(sysroot, target_path.lstrip("/"))
    if os.path.islink(cand):
        cand = os.path.realpath(cand)
    if os.path.exists(cand):
        return cand
    # bare basename fallback
    cand = os.path.join(sysroot, "lib", name)
    if os.path.exists(os.path.realpath(cand)):
        return os.path.realpath(cand)
    return None


def _default_sysroot(exec_elf):
    """Best-effort: a Buildroot exec at .../output-fdpic/target/... implies libs (with debug info)
    live under .../output-fdpic/staging."""
    p = os.path.abspath(exec_elf)
    marker = os.sep + "target" + os.sep
    if "output-fdpic" in p and marker in p:
        staging = p[: p.index(marker)] + os.sep + "staging"
        if os.path.isdir(staging):
            return staging
    return None


# ---- the rendezvous walk (the heart of auto-solib) -----------------------------------------------
def _walk_and_load(dyn_addr, exec_comm, exec_elf, sysroot):
    """Given the exec's _DYNAMIC runtime address (after ld.so has run), load every loaded object."""
    # exec's _DYNAMIC -> DT_DEBUG (tag 21) -> struct r_debug
    p, r_debug = dyn_addr, 0
    for _ in range(256):
        tag = _i32(p)
        if tag == 0:
            break
        if tag == 21:
            r_debug = _u32(p + 4)
            break
        p += 8
    if not r_debug:
        print("[ove-fdpic] DT_DEBUG empty — ld.so has not published the link map yet")
        return
    r_map = _u32(r_debug + 4)  # struct r_debug { int r_version; struct link_map *r_map; ... }
    lm, i, loaded = r_map, 0, 0
    while lm and i < 64:
        # elf_resolve head: FDPIC l_addr = {loadmap*@0, got@4}, l_name@8, l_ld@12, l_next@16
        loadmap = _u32(lm)
        l_name = _u32(lm + 8)
        l_next = _u32(lm + 16)
        name = _cstr(l_name)
        text_bias, data_base = _loadmap_bases(loadmap)
        elf = _host_elf(name, exec_comm, exec_elf, sysroot)
        tag = _basename(name) or "(exec)"
        if text_bias is None:
            print("[ove-fdpic]   %-22s no loadmap — skipped" % tag)
        elif not elf:
            print("[ove-fdpic]   %-22s text=0x%08x  (no host ELF found in sysroot — skipped)"
                  % (tag, text_bias))
        elif _add_symbols(elf, text_bias, data_base):
            print("[ove-fdpic]   %-22s text=0x%08x data=0x%08x  <- %s"
                  % (tag, text_bias, data_base or 0, elf))
            loaded += 1
        lm = l_next
        i += 1
    print("[ove-fdpic] loaded symbols for %d object(s). Text is flash-resident -> use `hbreak`." % loaded)


def _arm_dl_debug_state():
    """After ld.so symbols are loaded, breakpoint its notifier so a later dlopen re-walks."""
    try:
        gdb.execute("hbreak _dl_debug_state", to_string=True)
        return True
    except gdb.error:
        return False


# ---- single-object mapping (legacy / no-shared-libs) ---------------------------------------------
def _map_slot(sidx, elf):
    d = gdb.parse_and_eval("g_ove_lnx_dbg[%d]" % sidx)
    text = int(d["text_base"]) & 0xFFFFFFFF
    data = int(d["data_base"]) & 0xFFFFFFFF
    _add_symbols(elf, text, data)
    print("[ove-fdpic] slot %d mapped: text bias=0x%08x (RW data base=0x%08x)" % (sidx, text, data))
    print("[ove-fdpic] text is flash-resident (shared cpio) -> use `hbreak`, not `break`.")


def _find_launch_slot(comm):
    """continue until launch() is hit for a program whose argv[0] basename == comm; return sidx."""
    bp = gdb.Breakpoint("launch", internal=True)
    try:
        for _ in range(64):
            gdb.execute("continue")
            fr = gdb.selected_frame()
            if fr is None or fr.name() != "launch":
                print("[ove-fdpic] stopped outside launch(); aborting")
                return None
            try:
                a0 = gdb.parse_and_eval("argv[0]").string()
            except gdb.error:
                a0 = ""
            if _basename(a0) == comm:
                return int(gdb.parse_and_eval("sidx"))
        return None
    finally:
        gdb.execute("finish", to_string=True)  # let launch return: g_ove_lnx_dbg[sidx] is filled
        bp.delete()


class OveFdpicAuto(gdb.Command):
    """ove-fdpic-auto <comm> <exec-elf> [sysroot]: run until <comm> execs, then auto-load the exec +
    ld.so + every shared library with full source (walks the FDPIC DT_DEBUG rendezvous)."""

    def __init__(self):
        super().__init__("ove-fdpic-auto", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        args = arg.split()
        if len(args) < 2:
            print("usage: ove-fdpic-auto <comm> <exec-elf> [sysroot]")
            return
        comm, exec_elf = args[0], args[1]
        sysroot = args[2] if len(args) > 2 else _default_sysroot(exec_elf)
        sidx = _find_launch_slot(comm)
        if sidx is None:
            print("[ove-fdpic] never saw %r exec" % comm)
            return
        d = gdb.parse_and_eval("g_ove_lnx_dbg[%d]" % sidx)
        text = int(d["text_base"]) & 0xFFFFFFFF
        entry = int(d["entry"]) & 0xFFFFFFFF
        dyn = int(d["dynamic"]) & 0xFFFFFFFF
        interp = int(d["interp_base"]) & 0xFFFFFFFF
        if not interp or not dyn:
            # static exec (no interpreter / rendezvous): just map the one object.
            _add_symbols(exec_elf, text, int(d["data_base"]) & 0xFFFFFFFF)
            print("[ove-fdpic] static exec (no interpreter) — only the exec is mapped.")
            return
        # Run to the exec's own entry (a raw-address bp needs no symbols yet): by then ld.so has
        # linked libc + patched _DYNAMIC[DT_DEBUG]. Then the walk loads the exec + every library.
        gdb.execute("hbreak *0x%x" % (entry & ~1), to_string=True)
        gdb.execute("continue")
        print("[ove-fdpic] ld.so finished; walking the link map:")
        _walk_and_load(dyn, comm, exec_elf, sysroot)
        _arm_dl_debug_state()
        print("[ove-fdpic] %s ready — `hbreak main` (or `hbreak write` to step into libc), `continue`."
              % comm)


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
    """ove-fdpic-debug <comm> <elf>: run until <comm> is exec'd, then map its (exec-only) symbols."""

    def __init__(self):
        super().__init__("ove-fdpic-debug", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        comm, elf = arg.split()
        sidx = _find_launch_slot(comm)
        if sidx is None:
            print("[ove-fdpic] never saw %r exec" % comm)
            return
        _map_slot(sidx, elf)
        print("[ove-fdpic] %s ready — set HARDWARE breakpoints (e.g. `hbreak main`) and `continue`" % comm)


OveFdpicAuto()
OveFdpicMap()
OveFdpicDebug()
print("[ove-fdpic] commands: ove-fdpic-auto <comm> <exec-elf> [sysroot] | ove-fdpic-map | ove-fdpic-debug")
