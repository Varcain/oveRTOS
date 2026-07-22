# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ove-fdpic-gdb.py — turnkey source-level GDB debugging of an FDPIC userspace program (plus the
# ld.so and shared libraries it runs on) inside the oveRTOS Linux personality.
#
# An FDPIC object is loaded at RUNTIME addresses: its loadmap relocates each PT_LOAD segment
# independently (text is shared in place from the board's CPIO backing; RW data lives in the
# per-process region / PSRAM), so the on-disk ELF's link addresses do not match memory and a plain
# `file prog.elf` gives wrong line/symbol mappings. Each object therefore needs symbols placed at its
# own text bias — and a dynamic program has THREE of them (the exec, ld-uClibc.so.0, libc.so.0).
#
# The personality publishes the main exec's runtime bases + its _DYNAMIC address in the GDB-readable
# table `g_lxp_dbg[]` (filled by launch() in modules/lxp/src/lxp_run.c). `ove-fdpic-auto`
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
#     segment at a DIFFERENT bias, so EVERY RW section (.data/.bss/.got/.dynamic/.data.rel.ro) is
#     re-mapped there with `-s <sect> <addr>` (see _elf_data_sections) — otherwise a data symbol left
#     at the text bias (e.g. libc's `_fixed_buffers`) lands in the text address space and shadows
#     another object's CODE there (it was masking dbgdemo's `main`).
#   * Text is shared XIP from the CPIO backing (normally flash/QSPI on hardware), so use `hbreak`
#     (Cortex-M hardware breakpoint), not a mutating software breakpoint.
# Requires a Python-enabled, ARM-capable GDB (the distro /usr/bin/gdb 17.x with --enable-targets=all
# works; the bare arm-none-eabi-gdb toolchain builds ship WITHOUT Python and cannot `source` this).
#
# Usage (from a GDB attached to the firmware's gdbstub, e.g. qemu -gdb tcp::1234):
#   (gdb) source config/scripts/ove-fdpic-gdb.py
#   (gdb) ove-fdpic-auto dbgdemo /path/to/dbgdemo   # wait for the next exec of "dbgdemo", load ALL
#                                                   # objects (exec + ld.so + libc) with full source
#   (gdb) hbreak main      # or `hbreak write` to step into libc — safe for shared XIP text
#   (gdb) continue
# Add --userspace to drop the firmware/kernel symbols after the walk, so a bare syscall-stub frame
# resolves as `write ()` (not `?? ()`) — the real-Linux userspace-debugging model (kernel frames then
# show `?? ()`; omit it, the default, to keep the firmware for kernel/cross-boundary debugging):
#   (gdb) ove-fdpic-auto dbgdemo /path/to/dbgdemo --userspace
# The single-object helpers remain for a program already loaded / with no shared libs:
#   (gdb) ove-fdpic-map dbgdemo /path/to/dbgdemo    # map one already-loaded slot's exec symbols
#   (gdb) ove-fdpic-debug dbgdemo /path/to/dbgdemo  # run to the exec, map just its symbols
import gdb
import os
import struct

# ---- backtrace terminator for the FDPIC entry stub -----------------------------------------------
# The FDPIC `_start` crt stub carries no CFI. In --userspace mode the firmware is dropped so `_start`
# resolves — and GDB's prologue-based unwinder then reads a bogus "return address" that points back
# into `_start`, looping forever (each iteration nudges SP, so GDB's same-frame detection never trips).
# Register an unwinder that recognises a frame sitting exactly on a known program entry PC and hands
# GDB a caller PC of 0, so `_start` is treated as the outermost frame and the backtrace stops cleanly.
# Defensive: any failure (a non-Python / older GDB, an API change) degrades to the harmless loop.
try:
    from gdb.unwinder import Unwinder as _Unwinder, FrameId as _FrameId
    import gdb.unwinder as _gdb_unwinder

    class _OveEntryStop(_Unwinder):
        def __init__(self):
            super().__init__("ove-fdpic-entry-stop")
            self.ranges = []  # [lo, hi) runtime spans of _start stubs (populated by --userspace)

        def __call__(self, pending_frame):
            if not self.ranges:
                return None
            try:
                pcv = pending_frame.read_register("pc")
                pc = int(pcv) & 0xFFFFFFFE
                # The FDPIC _start never returns, so ANY frame within its stub is the outermost —
                # the looping "return address" is a slot a few instructions PAST the entry, so match
                # the whole stub span, not just the exact entry PC.
                if not any(lo <= pc < hi for lo, hi in self.ranges):
                    return None
                # Make the caller frame IDENTICAL to this one (same sp+pc) so GDB's own cycle detector
                # trips and ends the backtrace ("previous frame identical") — the default unwinder
                # instead keeps nudging sp, so the ids differ and it loops. This avoids synthesising a
                # pc=0 sentinel frame (which makes GDB demand a target-specific status reg it errors on).
                spv = pending_frame.read_register("sp")
                info = pending_frame.create_unwind_info(_FrameId(spv, pcv))
                info.add_saved_register("sp", spv)
                info.add_saved_register("pc", pcv)
                return info
            except Exception:  # noqa: BLE001 — never let the unwinder throw mid-backtrace
                return None

    _ENTRY_STOP = _OveEntryStop()
    _gdb_unwinder.register_unwinder(None, _ENTRY_STOP, replace=True)
except Exception:  # noqa: BLE001
    _ENTRY_STOP = None


def _stop_bt_at_entry(entry, span=0x100):
    """Terminate the backtrace anywhere inside this program's _start stub (see _OveEntryStop). `entry`
    is the runtime ELF entry (== _start's start); `span` covers the whole stub (a crt _start is tiny —
    tens of bytes — and never returns, so the whole span maps to the outermost frame)."""
    if _ENTRY_STOP is not None and entry:
        lo = entry & 0xFFFFFFFE
        _ENTRY_STOP.ranges.append((lo, lo + span))


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
    return int(gdb.parse_and_eval("(int)(sizeof(g_lxp_proc)/sizeof(g_lxp_proc[0]))"))


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


def _elf_data_sections(elf, data_base):
    """Parse `elf`'s ELF32 headers; return [(section_name, runtime_addr), ...] for EVERY SHF_ALLOC
    section in the RW (data) PT_LOAD segment, placed at its true runtime address.

    add-symbol-file's `-o <text_bias>` offsets every section by the TEXT bias — right for the RX
    segment, wrong for the RW segment (.data/.bss/.got/.dynamic/.data.rel.ro), which the FDPIC loadmap
    relocates INDEPENDENTLY to `data_base`. A data symbol left at the text bias (e.g. libc's
    `_fixed_buffers` in .bss, link vaddr ~0x5f730 + text bias 0x80fe0 ≈ 0xe0710) lands in the text
    address space and SHADOWS another object's code there (it masked dbgdemo's `main`). Re-map each RW
    section to sh_addr + (data_base − data_seg_vaddr) so every data symbol sits where it really is and
    the text address space holds only code."""
    try:
        with open(elf, "rb") as f:
            blob = f.read()
    except OSError:
        return []
    if len(blob) < 52 or blob[:4] != b"\x7fELF" or blob[4] != 1:  # ELF32 little-endian only
        return []

    def u16(o):
        return struct.unpack_from("<H", blob, o)[0]

    def u32(o):
        return struct.unpack_from("<I", blob, o)[0]

    e_phoff, e_shoff = u32(0x1C), u32(0x20)
    e_phentsize, e_phnum = u16(0x2A), u16(0x2C)
    e_shentsize, e_shnum, e_shstrndx = u16(0x2E), u16(0x30), u16(0x32)
    # The RW (data) PT_LOAD segment — the one the loadmap's data base relocates (PF_W set).
    seg = None
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        if u32(o) != 1:  # PT_LOAD
            continue
        if u32(o + 24) & 0x2:  # p_flags & PF_W -> the RW/data segment
            seg = (u32(o + 8), u32(o + 20))  # (p_vaddr, p_memsz)
            break
    if not seg or e_shstrndx >= e_shnum:
        return []
    d_vaddr, d_memsz = seg
    data_bias = (data_base - d_vaddr) & 0xFFFFFFFF
    strtab_off = u32(e_shoff + e_shstrndx * e_shentsize + 16)  # shstrtab sh_offset
    out = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        sh_name, sh_flags, sh_addr = u32(o), u32(o + 8), u32(o + 12)
        if not (sh_flags & 0x2):  # SHF_ALLOC
            continue
        if sh_addr < d_vaddr or sh_addr >= d_vaddr + d_memsz:  # only the RW segment
            continue
        end = blob.find(b"\0", strtab_off + sh_name)
        nm = blob[strtab_off + sh_name:end].decode("latin-1", "replace")
        if nm:
            out.append((nm, (sh_addr + data_bias) & 0xFFFFFFFF))
    return out


def _add_symbols(elf, text_bias, data_base):
    if not os.path.exists(elf):
        print("[ove-fdpic] MISSING symbol file: %s" % elf)
        return False
    cmd = "add-symbol-file %s -o 0x%x" % (elf, text_bias)
    if data_base:
        # Place every RW-segment section at its real data-segment address (NOT the text bias), so no
        # data symbol shadows code in the text space (see _elf_data_sections).
        for name, addr in _elf_data_sections(elf, data_base):
            cmd += " -s %s 0x%x" % (name, addr)
    gdb.execute(cmd, to_string=True)
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


_unshadowed = [False]


def _unshadow_cpio():
    """When the firmware embeds the rootfs cpio, it is one big OBJECT symbol
    (ove_test_rootfs_cpio) in executable .text — the FDPIC code is shared in place from it — and that
    symbol spans the whole shared-XIP region. For a libc/exec address that has no debug-info
    function (a syscall stub like write), GDB's msymbol lookup returns the cpio blob instead of the
    real symbol, so the frame shows `ove_test_rootfs_cpio` not `write`. Strip JUST that one symbol
    from a debug copy of the firmware ELF and reload it — the array data + every other symbol
    (launch, g_lxp_dbg, ...) stay, so nothing else changes. Afterwards a debug-info function (the
    program, libc C functions) resolves fully (name + source); a bare syscall stub, which GDB's
    section-filtered frame lookup can't name once the blob is gone, shows `?? ()` — its real name is
    one `info symbol $pc` away (that global lookup now returns the lib symbol uniquely)."""
    if _unshadowed[0]:
        return
    _unshadowed[0] = True
    objs = gdb.objfiles()
    if not objs or not objs[0].filename or not os.path.exists(objs[0].filename):
        return
    fw = objs[0].filename
    # (_unshadowed guards this to once per session; objcopy is a harmless no-op if the symbol is
    # absent, e.g. a non-personality firmware — so we don't need a fragile presence pre-check.)
    import subprocess
    import shutil
    import glob
    # Prefer the ARM toolchain objcopy (the host /usr/bin/objcopy often can't rewrite an arm-fdpic
    # ELF). Look on PATH, then in the oveRTOS toolchains dir relative to the firmware.
    objcopy = shutil.which("arm-none-eabi-objcopy")
    if not objcopy and (os.sep + "output" + os.sep) in fw:
        root = fw.split(os.sep + "output" + os.sep)[0]
        hits = glob.glob(os.path.join(root, "output", "toolchains", "*", "bin", "arm-none-eabi-objcopy"))
        objcopy = hits[0] if hits else None
    if not objcopy:
        objcopy = shutil.which("objcopy")  # last resort
    if not objcopy:
        print("[ove-fdpic] no ARM objcopy found — libc/exec frames may show the cpio blob symbol")
        return
    dbg = fw + ".fdpic-dbg"
    try:
        subprocess.run([objcopy, "--strip-symbol=ove_test_rootfs_cpio", fw, dbg],
                       check=True, capture_output=True)
        gdb.execute("symbol-file %s" % dbg)
        print("[ove-fdpic] stripped the rootfs-cpio blob symbol: it no longer mislabels libc/exec "
              "frames (a debug-info function shows its name+source; a bare syscall stub shows "
              "`?? ()` — use `info symbol $pc` for its real name).")
    except Exception as e:  # noqa: BLE001 — best-effort cosmetic; never abort debugging over it
        print("[ove-fdpic] could not strip the cpio symbol (%s) — frames may show the blob" % e)


def _make_firmware_removable():
    """Return the firmware ELF path, re-loaded as a REMOVABLE add-symbol-file objfile.

    `--userspace` mode drops the firmware after the walk so the shared-XIP frames resolve like real
    Linux (where the kernel is never a userspace objfile). But GDB's `remove-symbol-file` cannot touch
    the main `file` objfile — only add-symbol-file'd ones. So discard the main file and re-add the SAME
    ELF via add-symbol-file (at its own link addresses — a firmware is fixed-address, so no offset).
    MUST be called BEFORE any FDPIC object is loaded: `symbol-file` with no arg discards ALL symbol
    tables, so there must be nothing else to lose yet. `launch`/`g_lxp_dbg`/argv/sidx all still
    resolve afterwards (add-symbol-file provides them), so the launch-slot walk is unaffected."""
    objs = gdb.objfiles()
    if not objs or not objs[0].filename:
        return None
    fw = objs[0].filename
    was_on = gdb.parameter("confirm")
    try:
        gdb.execute("set confirm off")
        gdb.execute("symbol-file")             # drop the main file (nothing else is loaded yet)
        gdb.execute("add-symbol-file %s" % fw, to_string=True)  # re-add, now removable
    finally:
        gdb.execute("set confirm on" if was_on else "set confirm off")
    return fw


def _drop_firmware(fw):
    """Remove the firmware/kernel objfile so only the userspace (exec + ld.so + libs) objects remain —
    every shared-XIP address then belongs to exactly one objfile and frames resolve cleanly, exactly
    as when debugging userspace on real Linux (gdbserver never loads the kernel). Trade-off: kernel
    symbols are gone, so a backtrace that crosses into the SVC handler shows `?? ()` for the kernel
    frames — re-`add-symbol-file` the firmware to inspect the kernel side again."""
    if not fw:
        return
    try:
        gdb.execute("remove-symbol-file %s" % fw, to_string=True)
        print("[ove-fdpic] userspace-only view: dropped the firmware/kernel symbols, so libc/exec "
              "frames resolve cleanly (like real-Linux userspace debugging). Kernel frames are now "
              "`?? ()`; `add-symbol-file %s` to inspect the kernel again." % fw)
    except gdb.error as e:  # noqa: BLE001
        print("[ove-fdpic] could not drop the firmware (%s) — frames may show the cpio blob" % e)


def _default_sysroot(exec_elf):
    """Best-effort: a Buildroot exec at .../output/target/... implies libs (with debug info)
    live under .../output/staging."""
    p = os.path.abspath(exec_elf)
    marker = os.sep + "target" + os.sep
    if "output" in p and marker in p:
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
    print("[ove-fdpic] loaded symbols for %d object(s). Text is shared XIP -> use `hbreak`." % loaded)


def _arm_dl_debug_state():
    """After ld.so symbols are loaded, breakpoint its notifier so a later dlopen re-walks."""
    try:
        gdb.execute("hbreak _dl_debug_state", to_string=True)
        return True
    except gdb.error:
        return False


# ---- single-object mapping (legacy / no-shared-libs) ---------------------------------------------
def _map_slot(sidx, elf):
    d = gdb.parse_and_eval("g_lxp_dbg[%d]" % sidx)
    text = int(d["text_base"]) & 0xFFFFFFFF
    data = int(d["data_base"]) & 0xFFFFFFFF
    _add_symbols(elf, text, data)
    print("[ove-fdpic] slot %d mapped: text bias=0x%08x (RW data base=0x%08x)" % (sidx, text, data))
    print("[ove-fdpic] text is shared XIP -> use `hbreak`, not `break`.")


def _find_launch_slot(comm):
    """continue until launch() is hit for a program whose argv[0] basename == comm; return sidx."""
    print("[ove-fdpic] waiting for %r to exec on the target — run it now (the shell is live)..." % comm)
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
        gdb.execute("finish", to_string=True)  # let launch return: g_lxp_dbg[sidx] is filled
        bp.delete()


class OveFdpicAuto(gdb.Command):
    """ove-fdpic-auto <comm> <exec-elf> [sysroot] [--userspace]: run until <comm> execs, then auto-load
    the exec + ld.so + every shared library with full source (walks the FDPIC DT_DEBUG rendezvous).

    --userspace (-u): after loading the userspace objects, DROP the firmware/kernel symbols so every
    shared-XIP address belongs to exactly one objfile and frames resolve cleanly (a bare syscall stub
    shows `write ()`, not `?? ()`) — mirroring how real Linux debugs userspace (the kernel is never a
    userspace objfile). Trade-off: kernel frames become `?? ()`; omit the flag (the default) to keep
    the firmware for cross-boundary/kernel debugging."""

    def __init__(self):
        super().__init__("ove-fdpic-auto", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        userspace = False
        pos = []
        for a in arg.split():
            if a in ("--userspace", "-u"):
                userspace = True
            else:
                pos.append(a)
        if len(pos) < 2:
            print("usage: ove-fdpic-auto <comm> <exec-elf> [sysroot] [--userspace]")
            return
        comm, exec_elf = pos[0], pos[1]
        sysroot = pos[2] if len(pos) > 2 else _default_sysroot(exec_elf)
        # --userspace drops the firmware after the walk (clean frames); it must be REMOVABLE first, so
        # re-load it via add-symbol-file now (before any FDPIC object exists). The default keeps the
        # firmware and instead strips just the cpio blob symbol so it stops mislabeling frames.
        fw = _make_firmware_removable() if userspace else None
        if not userspace:
            _unshadow_cpio()
        sidx = _find_launch_slot(comm)
        if sidx is None:
            print("[ove-fdpic] never saw %r exec" % comm)
            return
        d = gdb.parse_and_eval("g_lxp_dbg[%d]" % sidx)
        text = int(d["text_base"]) & 0xFFFFFFFF
        entry = int(d["entry"]) & 0xFFFFFFFF
        dyn = int(d["dynamic"]) & 0xFFFFFFFF
        interp = int(d["interp_base"]) & 0xFFFFFFFF
        if userspace:
            _stop_bt_at_entry(entry)  # firmware is dropped -> stop the bt at _start (no CFI, else loops)
        if not interp or not dyn:
            # static exec (no interpreter / rendezvous): just map the one object.
            _add_symbols(exec_elf, text, int(d["data_base"]) & 0xFFFFFFFF)
            _drop_firmware(fw)  # no-op unless --userspace
            print("[ove-fdpic] static exec (no interpreter) — only the exec is mapped.")
            return
        # Run to the exec's own entry (a raw-address bp needs no symbols yet): by then ld.so has
        # linked libc + patched _DYNAMIC[DT_DEBUG]. Then the walk loads the exec + every library.
        gdb.execute("hbreak *0x%x" % (entry & ~1), to_string=True)
        gdb.execute("continue")
        print("[ove-fdpic] ld.so finished; walking the link map:")
        _walk_and_load(dyn, comm, exec_elf, sysroot)
        _arm_dl_debug_state()  # ld.so symbol — survives dropping the firmware
        _drop_firmware(fw)     # no-op unless --userspace
        print("[ove-fdpic] %s ready — `hbreak main` (or `hbreak write` to step into libc), `continue`."
              % comm)


class OveFdpicMap(gdb.Command):
    """ove-fdpic-map <comm> <elf>: map an ALREADY-loaded FDPIC program's symbols at its runtime bases."""

    def __init__(self):
        super().__init__("ove-fdpic-map", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        comm, elf = arg.split()
        _unshadow_cpio()
        for s in range(_nslot()):
            p = gdb.parse_and_eval("g_lxp_proc[%d]" % s)
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
        _unshadow_cpio()
        sidx = _find_launch_slot(comm)
        if sidx is None:
            print("[ove-fdpic] never saw %r exec" % comm)
            return
        _map_slot(sidx, elf)
        print("[ove-fdpic] %s ready — set HARDWARE breakpoints (e.g. `hbreak main`) and `continue`" % comm)


OveFdpicAuto()
OveFdpicMap()
OveFdpicDebug()
print("[ove-fdpic] commands: ove-fdpic-auto <comm> <exec-elf> [sysroot] [--userspace] | "
      "ove-fdpic-map | ove-fdpic-debug")
