# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import json
import os
import re
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


def _profiler_dump_samples(payload):
    """Decode a PROFILE_SUB_SAMPLES payload and print per-sample frames."""
    if len(payload) < 8:
        return
    try:
        _ver, word_size, count, dropped = struct.unpack_from(
            "<BBHI", payload, 0)
    except struct.error:
        return
    if word_size != 8:
        return
    p = 8
    for _ in range(count):
        if p + 16 > len(payload):
            return
        ts, tid, depth, state = struct.unpack_from("<QIBB", payload, p)
        p += 16
        # skip 2-byte pad
        if p + depth * 8 > len(payload):
            return
        pcs = []
        for _j in range(depth):
            (pc,) = struct.unpack_from("<Q", payload, p)
            pcs.append(pc)
            p += 8
        frames = " <- ".join(f"{_profiler_resolve(x)}@0x{x:x}" for x in pcs)
        sys.stderr.write(
            f"[dump] t={ts} tid=0x{tid:x} st={state} d={depth} {frames}\n")
    sys.stderr.flush()


def _trace_dump_descriptors(payload):
    """Decode a FRAME_TRACE descriptor batch (subType=0) and log tid→name
    pairs so an offline aggregator can filter idle/pump threads the same
    way the dashboard does. Only DESCRIPTORS; stream records are skipped."""
    if len(payload) < 8:
        return
    try:
        sub, _ver, count, _dropped = struct.unpack_from("<BBHI", payload, 0)
    except struct.error:
        return
    if sub != 0:
        return
    p = 8
    for _ in range(count):
        if p + 5 > len(payload):
            return
        (tid,) = struct.unpack_from("<I", payload, p); p += 4
        name_len = payload[p]; p += 1
        if p + name_len > len(payload):
            return
        name = payload[p:p + name_len].decode("utf-8", "replace")
        p += name_len
        sys.stderr.write(f"[dump-desc] tid=0x{tid:x} name={name}\n")
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


def build_profiler_symbols(elf_path, nm_bin="nm"):
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
_profiler_cfg_calls = {}      # caller_name -> set(callee_name)  direct BLs
_profiler_cfg_reach = {}      # caller_name -> set(callee_name)  direct + tail-closure
_profiler_cfg_indirect = set()  # callers with ≥1 `blx reg` / `bx reg` (not lr)
_profiler_cfg_indirect_hub = set()  # callers with ≥2 `blx reg` — real dispatchers (event_send_core, etc.)

_CFG_FUNC_RE = re.compile(r'^[0-9a-f]+\s+<([^>]+)>:')
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


def _build_profiler_cfg(elf_path, objdump_bin="objdump"):
    """Disassemble @elf_path and populate module-global CFG tables used
    by _prune_profile_payload(). Must be called AFTER
    build_profiler_symbols() so BL targets can be resolved to names via
    _profiler_resolve()."""
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

    calls = {}      # name -> set of direct-BL callees
    tails = {}      # name -> set of tail-call targets (external B)
    indirect_count = {}  # name -> number of `blx reg` instructions
    cur = None

    for line in out.splitlines():
        m = _CFG_FUNC_RE.match(line)
        if m:
            cur = m.group(1)
            calls.setdefault(cur, set())
            tails.setdefault(cur, set())
            continue
        if cur is None:
            continue
        m = _CFG_BL_RE.search(line)
        if m:
            try:
                target = int(m.group(1), 16)
            except ValueError:
                continue
            tgt = _profiler_resolve(target)
            if tgt and not tgt.startswith("0x"):
                calls[cur].add(tgt)
            continue
        m = _CFG_INDIRECT_RE.search(line)
        if m:
            indirect_count[cur] = indirect_count.get(cur, 0) + 1
            continue
        m = _CFG_B_RE.search(line)
        if m:
            try:
                target = int(m.group(1), 16)
            except ValueError:
                continue
            tgt = _profiler_resolve(target)
            # Intra-function branch: target resolves to the same function
            # we're currently parsing. Skip — it's control flow, not a
            # tail call.
            if tgt and not tgt.startswith("0x") and tgt != cur:
                tails[cur].add(tgt)

    # Compute tail-call closure: when caller BL's some X, and X chains
    # through `b Y; b Z;` tail-calls, any of {X, Y, Z, ...} can appear
    # on the stack below caller.
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

    # Expand to 2-hop: reach[F] = reach_1[F] ∪ ⋃ reach_1[X] for X in
    # reach_1[F]. Covers the "unwinder missed a frame" case — e.g. an
    # LVGL draw_sw_fill call into blend where fill's large locals
    # corrupt the saved-r7 chain and fill drops out of the capture;
    # we still want to accept execute_drawing -> blend as a valid
    # (hole-containing) edge. 2-hop stays tight in this firmware
    # because callbacks dispatch via `blx reg`, which reach doesn't
    # traverse — event-dispatch functions don't reach draw code via
    # reach_2. Sampled separately at build time before relying on it.
    reach = {}
    for f in reach_1:
        r = set(reach_1[f])
        for x in list(reach_1[f]):
            r |= reach_1.get(x, set())
        reach[f] = r

    indirect = set(indirect_count)
    indirect_hub = {f for f, n in indirect_count.items() if n >= 2}

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
    """True when `caller` could plausibly be the direct-or-transitive
    ancestor of `callee`. Allows when either side is unresolved. When
    strict=False (default, used for the primary adjacent-edge check),
    an indirect-dispatch hub caller (≥2 `blx reg` sites — e.g.
    event_send_core) also permits arbitrary callees. When strict=True
    (used for validator-scan targets that want to drop intermediate
    frames), hub-permit is disabled — only direct/reach edges count.
    Hub-permit at the scan target would wrongly accept a hub as the
    validator across several REAL intermediate frames, dropping them."""
    if caller not in _profiler_cfg_reach or callee.startswith("0x"):
        return True
    if callee in _profiler_cfg_reach[caller]:
        return True
    if strict:
        return False
    return caller in _profiler_cfg_indirect_hub


def _prune_profile_payload(payload):
    """Rewrite a PROFILE_SUB_SAMPLES payload, dropping phantom frames.
    For each (callee, apparent-caller) pair, if the edge is refuted AND
    a valid caller for the same callee exists within _PRUNE_WINDOW
    frames above, drop every refuted frame between and promote the
    validator. If no validator is reachable, keep the chain as-is —
    better to show a bogus caller than to shred a chain we can't
    confidently repair. No-op when CFG tables are empty."""
    if not _profiler_cfg_reach or len(payload) < 8:
        return payload
    try:
        ver, word_size, count, dropped = struct.unpack_from(
            "<BBHI", payload, 0)
    except struct.error:
        return payload
    if word_size != 8:
        return payload

    out = bytearray()
    out += struct.pack("<BBHI", ver, word_size, count, dropped)
    p = 8
    for _ in range(count):
        if p + 16 > len(payload):
            return payload
        ts, tid, depth, state = struct.unpack_from("<QIBB", payload, p)
        pad = struct.unpack_from("<H", payload, p + 14)[0]
        p += 16
        if p + depth * 8 > len(payload):
            return payload
        pcs = []
        for _j in range(depth):
            (pc,) = struct.unpack_from("<Q", payload, p)
            pcs.append(pc)
            p += 8

        names = [_profiler_resolve(pc) for pc in pcs]
        keep = [True] * len(pcs)

        # Duplicate-phantom pass: if the same function name appears at
        # multiple positions AND the CFG says the function is not a hub
        # and cannot reach itself (no recursion path in 2-hop), every
        # copy except the outermost is a stale-LR phantom. Drops the
        # very common `finalize_task_creation @ depth 6 AND 10` pattern
        # from the LVGL draw dispatch path.
        seen_at = {}
        for idx, nm in enumerate(names):
            if nm.startswith("0x"):
                continue
            seen_at.setdefault(nm, []).append(idx)
        for nm, positions in seen_at.items():
            if len(positions) < 2:
                continue
            if nm in _profiler_cfg_indirect_hub:
                continue
            if nm in _profiler_cfg_reach.get(nm, ()):
                continue  # real recursion
            # Keep only the outermost occurrence.
            for idx in positions[:-1]:
                keep[idx] = False

        # Walk from leaf. i = current validated callee index.
        i = 0
        while i < len(keep):
            if not keep[i]:
                i += 1
                continue
            # Find next kept neighbour.
            j = i + 1
            while j < len(keep) and not keep[j]:
                j += 1
            if j >= len(keep):
                break
            callee = names[i]
            if _edge_allowed(callee, names[j]):
                i = j
                continue
            # Edge refuted. Scan up to _PRUNE_WINDOW frames higher for
            # a validator. If found, mark every refuted intermediate
            # frame as phantom. strict=True because we don't want a
            # hub caller 3 frames up to hub-permit across real
            # intermediate frames (e.g. finalize_task_creation
            # hub-permitting `dispatch` would drop real lv_draw_dispatch
            # and lv_draw_dispatch_layer frames in between).
            k = j + 1
            hops = 0
            found = -1
            while k < len(keep) and hops < _PRUNE_WINDOW:
                if not keep[k]:
                    k += 1
                    continue
                if _edge_allowed(callee, names[k], strict=True):
                    found = k
                    break
                k += 1
                hops += 1
            if found >= 0:
                for m in range(j, found):
                    if keep[m]:
                        keep[m] = False
                i = found
            else:
                # No validator — keep j as an unvalidated caller.
                i = j

        # Firm-refuted up-edge pass: drop a frame whose UP edge
        # (current-frame called-by next-up-frame) is firmly refuted —
        # the up-caller has a resolvable reach set, callee is not in
        # it, AND the up-caller has ZERO indirect call sites. With no
        # indirect calls, the CFG reach is COMPLETE (direct bl/b +
        # tail-call closure); missing callee means the call is truly
        # impossible. Catches stale-LR phantoms wedged between real
        # frames where the down-edge happens to be CFG-valid so the
        # isolated-phantom pass (which needs BOTH edges refuted) can't
        # touch them. Example: in LVGL label draw, a previous rect
        # draw leaves `finalize_task_creation @ lv_draw_rect` on the
        # stack, so the chain shows
        # `iterate_characters <- finalize_task_creation <- lv_draw_rect
        # <- lv_draw_sw_label`; `sw_label` has 0 indirect callsites
        # and doesn't reach `lv_draw_rect`, so that edge is firm-
        # refuted and `lv_draw_rect` is dropped. Iterating lets the
        # now-exposed `finalize_task_creation <- lv_draw_sw_label`
        # edge also get firm-refuted and dropped.
        changed = True
        while changed:
            changed = False
            kept_idx = [k for k, v in enumerate(keep) if v]
            for pos in range(len(kept_idx) - 1):
                cur = kept_idx[pos]
                up = kept_idx[pos + 1]
                up_name = names[up]
                cur_name = names[cur]
                if (up_name in _profiler_cfg_reach
                        and not cur_name.startswith("0x")
                        and cur_name not in _profiler_cfg_reach[up_name]
                        and up_name not in _profiler_cfg_indirect):
                    keep[cur] = False
                    changed = True
                    break

        # Isolated-phantom pass: a frame whose edges to BOTH its
        # surviving upstream and downstream neighbours refute (loose)
        # is almost certainly a stale-LR phantom wedged between real
        # frames. The window-validator pass can miss these when the
        # phantom is an indirect non-hub (permits nothing strict, but
        # not refuted-enough to trigger the scan). Example:
        # `draw_letter_cb <- lv_draw_buf_width_to_stride_ex <-
        # lv_draw_unit_draw_letter` — buf_width is indirect but not a
        # hub, and sits where the real chain is
        # unit_draw_letter -> draw_letter_cb via blx.
        changed = True
        while changed:
            changed = False
            kept_idx = [k for k, v in enumerate(keep) if v]
            for pos in range(1, len(kept_idx) - 1):
                mid = kept_idx[pos]
                down = kept_idx[pos - 1]    # closer to leaf (callee)
                up = kept_idx[pos + 1]      # closer to outer (caller)
                if (not _edge_allowed(names[down], names[mid])
                        and not _edge_allowed(names[mid], names[up])):
                    keep[mid] = False
                    changed = True
                    break

        # Top-of-chain trim: if the outermost surviving frame has a
        # refuted edge to the frame below, drop it and keep trimming
        # until the top validates or we run out. Catches phantoms at
        # the end of the unwound chain with nothing above to validate.
        kept_idx = [i for i, k in enumerate(keep) if k]
        while len(kept_idx) >= 2:
            top = kept_idx[-1]
            below = kept_idx[-2]
            if _edge_allowed(names[below], names[top]):
                break
            keep[top] = False
            kept_idx.pop()

        new_pcs = [pc for pc, k in zip(pcs, keep) if k]
        new_depth = len(new_pcs)
        out += struct.pack("<QIBBH", ts, tid, new_depth, state, pad)
        for pc in new_pcs:
            out += struct.pack("<Q", pc)
    return bytes(out)
