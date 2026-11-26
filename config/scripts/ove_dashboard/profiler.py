# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import json
import os
import re
import shutil
import struct
import subprocess
import sys

from .protocol import FRAME_PROFILE, PROFILE_SUB_SYMBOLS

# ── Profiler symbol map (nm-based) ──────────────────────────────────

_profiler_symbol_frame = None  # cached payload for PROFILE_SUB_SYMBOLS

# Debug-only: when OVE_PROFILER_DUMP=1 is set, decode each sample batch
# and print the resolved frames per sample to stderr. Used to inspect
# profiler output while iterating on the stack-unwinding heuristics.
_profiler_sym_index = None  # list of (pc_start, pc_end, name)
_LINUX_GUEST_SYMBOL = "[Linux guest]"


def _profiler_resolve(pc):
    if not _profiler_sym_index:
        return f"0x{pc:x}"
    lo, hi = 0, len(_profiler_sym_index) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        ps, pe, nm = _profiler_sym_index[mid]
        if pc < ps:
            hi = mid - 1
        elif pc >= pe:
            lo = mid + 1
        else:
            return nm.split("@", 1)[0]
    return f"0x{pc:x}"


# Profiler sample PCs travel at the target's pointer size (envelope
# word_size): 4 on Cortex-M, 8 on 64-bit hosts.
_PC_FMT = {4: "<I", 8: "<Q"}


def _profiler_dump_samples(payload):
    """Decode a PROFILE_SUB_SAMPLES payload and print per-sample frames."""
    if len(payload) < 8:
        return
    try:
        _ver, word_size, count, dropped = struct.unpack_from(
            "<BBHI", payload, 0)
    except struct.error:
        return
    pc_fmt = _PC_FMT.get(word_size)
    if pc_fmt is None:
        return
    p = 8
    for _ in range(count):
        if p + 16 > len(payload):
            return
        ts, tid, depth, state, isr = struct.unpack_from("<QIBBH", payload, p)
        p += 16
        if p + depth * word_size > len(payload):
            return
        pcs = []
        for _j in range(depth):
            (pc,) = struct.unpack_from(pc_fmt, payload, p)
            pcs.append(pc)
            p += word_size
        frames = " <- ".join(f"{_profiler_resolve(x)}@0x{x:x}" for x in pcs)
        where = f"isr={isr}" if isr else f"tid=0x{tid:x}"
        sys.stderr.write(
            f"[dump] t={ts} {where} st={state} d={depth} {frames}\n")
    sys.stderr.flush()


def _trace_dump_descriptors(payload):
    """Decode a FRAME_TRACE descriptor batch (subType=0) and log tid→name
    pairs so an offline aggregator can filter idle/pump threads the same
    way the dashboard does. Only DESCRIPTORS; stream records are skipped."""
    if len(payload) < 8:
        return
    try:
        sub, ver, count, _dropped = struct.unpack_from("<BBHI", payload, 0)
    except struct.error:
        return
    if sub != 0:
        return
    has_gen = ver >= 2
    p = 8
    for _ in range(count):
        if p + (9 if has_gen else 5) > len(payload):
            return
        (tid,) = struct.unpack_from("<I", payload, p)
        p += 4
        gen_field = ""
        if has_gen:
            (generation,) = struct.unpack_from("<I", payload, p)
            p += 4
            gen_field = f" gen=0x{generation:x}"
        name_len = payload[p]
        p += 1
        if p + name_len > len(payload):
            return
        name = payload[p:p + name_len].decode("utf-8", "replace")
        p += name_len
        sys.stderr.write(f"[dump-desc] tid=0x{tid:x} name={name}{gen_field}\n")
    sys.stderr.flush()


def _nm_symbols(path, base, want_dynamic, nm_bin="nm"):
    """Run nm and return a list of (addr, size, name) tuples already
    shifted by @base. On shared libraries, static symbols are stripped
    so pass want_dynamic=True to ask nm for the dynamic symbol table.
    @nm_bin lets the cross-toolchain's nm be used for non-host ELFs
    (e.g. arm-none-eabi-nm on QEMU FreeRTOS firmware)."""
    cmd = [nm_bin, "-n", "-S", "--defined-only", "-C"]
    if want_dynamic:
        cmd.append("-D")
    cmd.append(path)
    try:
        out = subprocess.check_output(
            cmd, stderr=subprocess.DEVNULL).decode("utf-8", "replace")
    except (FileNotFoundError, subprocess.CalledProcessError):
        return []
    syms = []
    for line in out.splitlines():
        parts = line.split(maxsplit=3)
        if len(parts) == 3:
            addr_s, tcode, name = parts
            size_s = None
        elif len(parts) == 4:
            addr_s, size_s, tcode, name = parts
        else:
            continue
        if tcode not in ("T", "t", "W", "w", "i"):
            continue
        try:
            addr = int(addr_s, 16)
            size = int(size_s, 16) if size_s else 0
        except ValueError:
            continue
        syms.append((addr + base, size, name))
    return syms


def profiler_tools(gdb_toolchain):
    """Return (nm, objdump) of the cross toolchain that @gdb_toolchain (a
    `<prefix>-gdb` path or bare name on PATH) belongs to; None for a tool
    that cannot be found."""
    gdb = gdb_toolchain or ""
    if not (gdb.endswith("-gdb") and len(gdb) > len("-gdb")):
        return None, None
    prefix = gdb[:-len("-gdb")]
    found = []
    for tool in ("-nm", "-objdump"):
        cand = prefix + tool
        if os.path.isfile(cand):
            found.append(cand)
        else:
            found.append(shutil.which(cand))
    return found[0], found[1]


def linux_guest_ranges(config):
    """Return half-open address ranges for the configured Linux guest windows."""
    ranges = []
    for window in ("ROOTFS", "GUEST_POOL"):
        prefix = f"CONFIG_OVE_LINUX_{window}"
        try:
            base = int(config.get(f"{prefix}_BASE", "0"), 16)
            size = int(config.get(f"{prefix}_SIZE", "0"), 16)
        except ValueError:
            continue
        if base > 0 and size > 0:
            ranges.append((base, base + size))
    return ranges


def build_profiler_symbols(elf_path, nm_bin="nm", *, guest_ranges=()):
    """Build a PROFILE_SUB_SYMBOLS WebSocket frame for the firmware ELF.

    Payload after the 5-byte outer header is a UTF-8 JSON array of
    [pc_start, pc_end, name] triples sorted by pc_start.

    @nm_bin is forwarded to _nm_symbols so the ARM firmware can be
    indexed with arm-none-eabi-nm.
    """
    global _profiler_symbol_frame
    if not elf_path or not os.path.isfile(elf_path):
        return

    real_main = os.path.realpath(elf_path)
    # The firmware is one statically linked image: path -> (base, pc_start, pc_end).
    segs = {real_main: (0, 0, 0)}

    # Extension cap: never inflate a single symbol past this size, so the
    # last named symbol before a large unnamed region can't swallow it.
    MAX_SYM_EXTEND = 0x10000   # 64 KiB

    entries = []
    for path, (base, pc_start, pc_end) in segs.items():
        # Prefer the static symbol table; fall back to -D if nothing came
        # out (e.g. a stripped build).
        syms = _nm_symbols(path, base, want_dynamic=False, nm_bin=nm_bin)
        if not syms:
            syms = _nm_symbols(path, base, want_dynamic=True, nm_bin=nm_bin)
        if not syms:
            continue

        # Sort + dedup + gap-fill. Extending each symbol's end to the
        # next covers nm holes (veneers, padding).
        syms.sort(key=lambda s: s[0])
        mapping_end_cap = pc_end if pc_end else (
            syms[-1][0] + MAX_SYM_EXTEND)
        seen = set()
        n = len(syms)
        for i, (addr, _size, name) in enumerate(syms):
            if addr in seen:
                continue
            seen.add(addr)
            # Next distinct-address symbol in this mapping.
            next_start = None
            for k in range(i + 1, n):
                na = syms[k][0]
                if na > addr:
                    next_start = na
                    break
            hard_cap = min(
                addr + MAX_SYM_EXTEND,
                mapping_end_cap,
            )
            end = min(next_start, hard_cap) if next_start else hard_cap
            if end <= addr:
                end = addr + 1
            entries.append([addr, end, name])

    entries.extend([start, end, _LINUX_GUEST_SYMBOL] for start, end in guest_ranges)
    if not entries:
        return

    entries.sort(key=lambda e: e[0])

    # Cache for OVE_PROFILER_DUMP resolver. No-op in normal runs.
    global _profiler_sym_index
    _profiler_sym_index = [(e[0], e[1], e[2]) for e in entries]

    payload = json.dumps(entries).encode("utf-8")
    _profiler_symbol_frame = struct.pack("<IB", FRAME_PROFILE, PROFILE_SUB_SYMBOLS) + payload

    sys.stderr.write(
        f"[profiler] {len(entries)} symbols indexed from "
        f"{len(segs)} segment(s) ({len(payload)} B)\n")
    sys.stderr.flush()


# ── Profiler CFG validator (objdump-based) ──────────────────────────
#
# The profiler unwinds by walking saved-{r7, lr} pairs on the task
# stack. Functions with large local frames (e.g. LVGL draw callbacks
# with 200+ B of locals) sometimes contain leftover {r7, lr} pairs
# from previously-completed deep calls — these look identical to a
# live frame and can't be rejected by the r7-invariant check alone.
#
# Bridge-side fix: once at startup, disassemble the firmware ELF with
# objdump and build a per-function call-graph of direct BL targets.
# Each profiler sample arriving as a (leaf, lr1, lr2, ...) chain is
# validated adjacent-pairwise: for each (callee, apparent-caller), if
# the caller has no `bl callee` instruction *and* no `blx reg` /
# indirect branch, the caller frame is dropped as phantom.
#
# Tail calls are handled: `F tail-calls G` via an unconditional `b G`
# makes G inherit F's saved LR (into F's caller). Stack then shows
# `G <- F_caller` even though F_caller never directly called G. We
# compute tail-call closure and include transitive targets.
# The tables are keyed by function start address, not name: static
# functions (and LVGL's per-file inline copies — e.g. 39 x lv_memzero)
# share names, and merging them would refute real edges or validate
# phantom ones. Names are for display only.
_profiler_cfg_calls = {}      # caller_start -> set(callee_start)  direct BLs
_profiler_cfg_reach = {}      # caller_start -> set(callee_start)  direct + tail-closure (2-hop)
_profiler_cfg_indirect = set()  # callers with >=1 `blx reg` / `bx reg` (not lr)
_profiler_cfg_indirect_hub = set()  # callers with >=2 `blx reg` (dispatchers); logged, not used to prune

_CFG_FUNC_RE = re.compile(r'^([0-9a-f]+)\s+<([^>]+)>:')
# `bl HEXTARGET <NAME(+0xOFFSET)?>` — direct call. objdump on ARM emits
# `bl` even for inter-Thumb-calls (no `bl.w` distinction).
_CFG_BL_RE = re.compile(r'\sbl\s+([0-9a-f]+)\s+<([^>]+)>')
# Unconditional branch `b(.w|.n)? HEXTARGET <NAME(+0xOFFSET)?>` —
# possible tail call if target is outside the current function.
_CFG_B_RE = re.compile(r'\sb(?:\.w|\.n)?\s+([0-9a-f]+)\s+<([^>]+)>')
# `blx reg` / `bx reg` (reg != lr) — indirect call that invalidates the
# caller's outgoing edges. `bx lr` is just return; filter it out.
_CFG_INDIRECT_RE = re.compile(
    r'\s(?:blx|bx)\s+(r\d+|ip|sl|sb|fp)\b')


def _profiler_func_start(pc):
    """Start address of the symbol containing @pc, or None."""
    if not _profiler_sym_index:
        return None
    lo, hi = 0, len(_profiler_sym_index) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        ps, pe, name = _profiler_sym_index[mid]
        if pc < ps:
            hi = mid - 1
        elif pc >= pe:
            lo = mid + 1
        else:
            # The guest windows lack ELF functions for CFG pruning.
            return None if name == _LINUX_GUEST_SYMBOL else ps
    return None


def _cfg_from_objdump(text):
    """Build the call-graph tables from `objdump -d` output (requires the
    symbol index). Returns (calls, reach, indirect, indirect_hub)."""
    calls = {}      # start -> set of direct-BL callee starts
    tails = {}      # start -> set of tail-call target starts (external B)
    indirect_count = {}  # start -> number of `blx reg` instructions
    cur = None

    for line in text.splitlines():
        m = _CFG_FUNC_RE.match(line)
        if m:
            cur = _profiler_func_start(int(m.group(1), 16))
            if cur is not None:
                calls.setdefault(cur, set())
                tails.setdefault(cur, set())
            continue
        if cur is None:
            continue
        m = _CFG_BL_RE.search(line)
        if m:
            tgt = _profiler_func_start(int(m.group(1), 16))
            if tgt is not None:
                calls[cur].add(tgt)
            continue
        m = _CFG_INDIRECT_RE.search(line)
        if m:
            indirect_count[cur] = indirect_count.get(cur, 0) + 1
            continue
        m = _CFG_B_RE.search(line)
        if m:
            tgt = _profiler_func_start(int(m.group(1), 16))
            # Intra-function branch: control flow, not a tail call.
            if tgt is not None and tgt != cur:
                tails[cur].add(tgt)

    # Tail-call closure: when caller BL's some X, and X chains through
    # `b Y; b Z;` tail-calls, any of {X, Y, Z, ...} can appear on the stack
    # below caller.
    tc_cache = {}

    def tc_close(f, stk):
        if f in tc_cache:
            return tc_cache[f]
        if f in stk:
            return {f}
        stk.add(f)
        r = {f}
        for g in tails.get(f, ()):
            r |= tc_close(g, stk)
        stk.discard(f)
        tc_cache[f] = r
        return r

    # reach_1[F] = direct calls of F ∪ tail closures of direct callees.
    reach_1 = {}
    for f in calls:
        r = set(calls[f])
        for x in calls[f]:
            r |= tc_close(x, set())
        for g in tc_close(f, set()):
            r |= calls.get(g, set())
        reach_1[f] = r

    # Expand to 2-hop: covers the "unwinder missed a frame" case (e.g. a
    # large-locals frame drops out of the capture). Callbacks dispatch via
    # `blx reg`, which reach doesn't traverse, so 2-hop stays tight.
    reach = {}
    for f in reach_1:
        r = set(reach_1[f])
        for x in list(reach_1[f]):
            r |= reach_1.get(x, set())
        reach[f] = r

    indirect = set(indirect_count)
    indirect_hub = {f for f, n in indirect_count.items() if n >= 2}
    return calls, reach, indirect, indirect_hub


def _build_profiler_cfg(elf_path, objdump_bin="objdump"):
    """Disassemble @elf_path and populate module-global CFG tables used
    by _prune_profile_payload(). Must be called AFTER
    build_profiler_symbols() so BL targets resolve to function starts."""
    global _profiler_cfg_calls, _profiler_cfg_reach
    global _profiler_cfg_indirect, _profiler_cfg_indirect_hub
    if not elf_path or not os.path.isfile(elf_path):
        return
    if not _profiler_sym_index:
        return
    try:
        out = subprocess.check_output(
            [objdump_bin, "-d", "--no-show-raw-insn", elf_path],
            stderr=subprocess.DEVNULL).decode("utf-8", "replace")
    except (FileNotFoundError, subprocess.CalledProcessError):
        return

    calls, reach, indirect, indirect_hub = _cfg_from_objdump(out)
    _profiler_cfg_calls = calls
    _profiler_cfg_reach = reach
    _profiler_cfg_indirect = indirect
    _profiler_cfg_indirect_hub = indirect_hub

    sys.stderr.write(
        f"[profiler] CFG: {len(calls)} functions, "
        f"{len(indirect)} with indirect calls "
        f"({len(indirect_hub)} hubs ≥2 blx), "
        f"{sum(len(v) for v in calls.values())} direct-call edges, "
        f"{sum(len(v) for v in reach.values())} 2-hop reach edges\n")
    sys.stderr.flush()


_PRUNE_WINDOW = 4   # max consecutive phantoms we'll try to bypass


def _edge_allowed(callee, caller, strict=False):
    """True when function `caller` (start address) could plausibly be the
    direct-or-transitive ancestor of `callee`. Allows when either side is
    unresolved (None). When strict=False (default, used for the primary
    adjacent-edge check), a caller with ANY indirect call site (`blx reg` /
    `bx reg`) also permits arbitrary callees: its reach set is incomplete,
    so absence from it does not refute the edge. This is what keeps the
    thread wrapper (one `blx` to the entry) and LVGL's function-pointer
    dispatch. When strict=True (used for validator-scan targets that want
    to drop intermediate frames), indirect-permit is disabled — only
    direct/reach edges count. Indirect-permit at the scan target would
    wrongly accept such a caller as the validator across several REAL
    intermediate frames, dropping them."""
    if caller is None or callee is None or caller not in _profiler_cfg_reach:
        return True
    if callee in _profiler_cfg_reach[caller]:
        return True
    if strict:
        return False
    return caller in _profiler_cfg_indirect


def _prune_chain(pcs):
    """Return the keep-mask for one sample's PC chain (leaf first)."""
    funcs = [_profiler_func_start(pc) for pc in pcs]
    keep = [True] * len(pcs)

    # Duplicate-phantom pass: if the same function appears at multiple
    # positions AND the CFG says it cannot reach itself (no recursion path
    # in 2-hop) and it has no indirect call site that could hide such a
    # path, every copy except the outermost is a stale-LR phantom (e.g.
    # `finalize_task_creation` at depth 6 AND 10 in the LVGL draw dispatch
    # path). Compared by function address, so two different static
    # functions sharing a name are not merged.
    seen_at = {}
    for idx, f in enumerate(funcs):
        if f is None:
            continue
        seen_at.setdefault(f, []).append(idx)
    for f, positions in seen_at.items():
        if len(positions) < 2:
            continue
        if f in _profiler_cfg_indirect:
            continue  # an indirect call may hide the recursion
        if f in _profiler_cfg_reach.get(f, ()):
            continue  # real recursion
        # pcs[0] is the sampled PC, never a stale LR: when it is one of the
        # copies, every other copy is the phantom.
        stale = positions[1:] if positions[0] == 0 else positions[:-1]
        for idx in stale:
            keep[idx] = False

    # Walk from leaf. i = current validated callee index.
    i = 0
    while i < len(keep):
        if not keep[i]:
            i += 1
            continue
        j = i + 1
        while j < len(keep) and not keep[j]:
            j += 1
        if j >= len(keep):
            break
        callee = funcs[i]
        if _edge_allowed(callee, funcs[j]):
            i = j
            continue
        # Edge refuted. Scan up to _PRUNE_WINDOW frames higher for a
        # validator; if found, every refuted intermediate frame is a
        # phantom. strict=True so a caller with an indirect call site
        # several frames up cannot permit across real intermediate frames.
        k = j + 1
        hops = 0
        found = -1
        while k < len(keep) and hops < _PRUNE_WINDOW:
            if not keep[k]:
                k += 1
                continue
            if _edge_allowed(callee, funcs[k], strict=True):
                found = k
                break
            k += 1
            hops += 1
        if found >= 0:
            for m in range(j, found):
                keep[m] = False
            i = found
        else:
            i = j  # no validator — keep j as an unvalidated caller

    # Firm-refuted up-edge pass: drop a frame whose UP edge is firmly
    # refuted — the up-caller has a resolvable reach set, the frame is not
    # in it, AND the up-caller has ZERO indirect call sites (so its reach
    # is complete) — but only when the up-caller validates the frame below
    # it (an unresolved frame below validates nothing), so the drop repairs
    # the chain. Without that condition one phantom high in a chain refuted
    # the real frame under it, then the next one, and erased the chain down
    # to the sampled PC. Catches stale-LR phantoms wedged between real
    # frames whose down-edge happens to be CFG-valid. The sampled PC (pos 0)
    # is never dropped. Iterate: removing one can expose another.
    changed = True
    while changed:
        changed = False
        kept_idx = [k for k, v in enumerate(keep) if v]
        for pos in range(1, len(kept_idx) - 1):
            cur = funcs[kept_idx[pos]]
            up = funcs[kept_idx[pos + 1]]
            down = funcs[kept_idx[pos - 1]]
            if (up in _profiler_cfg_reach and cur is not None
                    and down is not None
                    and cur not in _profiler_cfg_reach[up]
                    and up not in _profiler_cfg_indirect
                    and _edge_allowed(down, up, strict=True)):
                keep[kept_idx[pos]] = False
                changed = True
                break

    # Isolated-phantom pass: a frame whose edges to BOTH surviving
    # neighbours refute (loose) is a stale-LR phantom wedged between real
    # frames.
    changed = True
    while changed:
        changed = False
        kept_idx = [k for k, v in enumerate(keep) if v]
        for pos in range(1, len(kept_idx) - 1):
            mid = kept_idx[pos]
            down = kept_idx[pos - 1]    # closer to leaf (callee)
            up = kept_idx[pos + 1]      # closer to outer (caller)
            if (not _edge_allowed(funcs[down], funcs[mid])
                    and not _edge_allowed(funcs[mid], funcs[up])):
                keep[mid] = False
                changed = True
                break

    # Top-of-chain trim: drop outermost frames whose edge to the frame
    # below is refuted (nothing above to validate them).
    kept_idx = [k for k, v in enumerate(keep) if v]
    while len(kept_idx) >= 2:
        top = kept_idx[-1]
        below = kept_idx[-2]
        if _edge_allowed(funcs[below], funcs[top]):
            break
        keep[top] = False
        kept_idx.pop()
    return keep


def _prune_profile_payload(payload):
    """Rewrite a PROFILE_SUB_SAMPLES payload, dropping phantom frames
    (see _prune_chain). If no validator is reachable for a refuted edge the
    chain is kept as-is — better to show a bogus caller than to shred a
    chain we can't confidently repair. No-op when CFG tables are empty."""
    if not _profiler_cfg_reach or len(payload) < 8:
        return payload
    try:
        ver, word_size, count, dropped = struct.unpack_from(
            "<BBHI", payload, 0)
    except struct.error:
        return payload
    pc_fmt = _PC_FMT.get(word_size)
    if pc_fmt is None:
        return payload

    out = bytearray()
    out += struct.pack("<BBHI", ver, word_size, count, dropped)
    p = 8
    for _ in range(count):
        if p + 16 > len(payload):
            return payload
        ts, tid, depth, state, isr = struct.unpack_from("<QIBBH", payload, p)
        p += 16
        if p + depth * word_size > len(payload):
            return payload
        pcs = []
        for _j in range(depth):
            (pc,) = struct.unpack_from(pc_fmt, payload, p)
            pcs.append(pc)
            p += word_size

        keep = _prune_chain(pcs)
        new_pcs = [pc for pc, k in zip(pcs, keep) if k]
        out += struct.pack("<QIBBH", ts, tid, len(new_pcs), state, isr)
        for pc in new_pcs:
            out += struct.pack(pc_fmt, pc)
    return bytes(out)
