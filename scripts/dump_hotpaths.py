#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Dump and check oveRTOS binding hotpaths from a benchmark ELF.

Companion to scripts/zero_overhead_audit.py.  The audit proves no
function-pointer dispatch *exists* in the binary; this tool proves the
hot binding wrappers (Mutex::lock, MutexGuard::drop, etc.) compile to
the same instructions a hand-written C call site would.

Disassembles selected symbols via objdump, summarises each (instruction
count, list of call/jmp targets), and optionally compares against a
golden YAML at tests/audit/hotpath_expected.yaml — fails on hard
violations (forbidden patterns, unexpected callees, instruction counts
beyond the max).

Usage:
  dump_hotpaths.py --elf <elf> --binding <c|cpp|rust|zig> [--target <name>]
                   [--config tests/audit/hotpath_expected.yaml]
                   [--output-dir output/audit/disasm]
                   [--objdump <objdump>] [--cppfilt <c++filt>]

The config schema:
  <target_key>:                     # e.g. host_posix_benchmark_cpp
    symbols:
      - name: <human label>          # printed in the report
        pattern: <regex>             # matched against nm output (mangled)
        max_instructions: <int>      # soft cap (warning if exceeded)
        allowed_calls: [<callee>...] # call/jmp must resolve to these
        forbid_demangled: [<sub>...] # hard-fail substrings post-c++filt
"""

import argparse
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


# Lines that look like one decoded instruction: `<hex>: <mnemonic> ...`.
# The mnemonic captured here is then matched against the unconditional-
# control-transfer set below.
INSN_RE = re.compile(r"^\s*[0-9a-f]+:\s+(\S+)")

# Unconditional control transfers that resolve to an external symbol.
# Conditional jumps (je/jne/jl/jeq/bne/...) are intentionally absent —
# they're either intra-function loops (filtered out via the +0x suffix
# heuristic in summarise()) or, very rarely, cross-function flow that
# we do NOT want to charge as a wrapper call.
#
# x86: call/callq direct + jmp/jmpq tail
# ARM: bl/blx for calls, b/b.w/b.n unconditional branch (tail jump),
#      bx for branch-and-exchange.  bl.w / bl.n exist on Thumb-2.
CALL_OPCODES = {
    "call", "callq", "jmp", "jmpq",
    "bl", "blx", "bl.w", "bl.n",
    "b", "b.w", "b.n", "bx",
}

# Last `<name>` on the disasm line is always the resolved callee, both
# for direct calls (`call <addr> <name>`) and indirect calls
# (`call *<expr> # <addr> <_GLOBAL_OFFSET_TABLE_+0xNNN>`).
TARGET_RE = re.compile(r"<([^>]+)>\s*$")


def run(cmd):
    res = subprocess.run(cmd, capture_output=True, text=True)
    return res.stdout, res.returncode


def list_symbols(nm, elf):
    out, rc = run([nm, str(elf)])
    if rc != 0:
        print(f"nm failed: rc={rc}", file=sys.stderr)
        sys.exit(2)
    pat = re.compile(r"^[0-9a-fA-F]*\s+([A-Za-z?])\s+(\S.*)$")
    for line in out.splitlines():
        m = pat.match(line)
        if m:
            yield m.group(1), m.group(2)


def addr_to_symbol(nm, elf):
    """Map symbol VAs to names. Includes text symbols (so a resolved
    GOT slot can be mapped back to a function) and the special data
    symbol `_GLOBAL_OFFSET_TABLE_` used as the GOT-relative base."""
    out, rc = run([nm, str(elf)])
    if rc != 0:
        return {}
    addr_map = {}
    pat = re.compile(r"^([0-9a-fA-F]+)\s+([A-Za-z?])\s+(\S.*)$")
    for line in out.splitlines():
        m = pat.match(line)
        if not m:
            continue
        stype = m.group(2).lower()
        name = m.group(3)
        if stype == "t" or name == "_GLOBAL_OFFSET_TABLE_":
            addr_map[int(m.group(1), 16)] = name
    return addr_map


def get_got_slot(elf_path, slot_va):
    """Resolve a GOT slot virtual address to the qword stored there.

    PIE binaries reach internal functions via GOT slots; the slot VA
    appears in objdump output as `_GLOBAL_OFFSET_TABLE_+0xNNN`. To map
    the slot back to its target function, read the qword at that VA
    and look it up in the symbol table.

    Returns the resolved virtual address, or None on failure.
    """
    out, rc = run(["readelf", "-S", "-W", str(elf_path)])
    if rc != 0:
        return None
    got_va = got_off = None
    for line in out.splitlines():
        m = re.search(
            r"\.got\s+\S+\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)", line
        )
        if m and " .got " in line + " ":
            got_va = int(m.group(1), 16)
            got_off = int(m.group(2), 16)
            break
    if got_va is None:
        return None
    if not (got_va <= slot_va):
        return None
    file_off = got_off + (slot_va - got_va)
    with open(elf_path, "rb") as f:
        f.seek(file_off)
        raw = f.read(8)
    if len(raw) != 8:
        return None
    return int.from_bytes(raw, byteorder="little", signed=False)


def resolve_got_callee(target, elf_path, addr_map):
    """If target looks like `_GLOBAL_OFFSET_TABLE_+0xNNN`, resolve to
    the function name behind that GOT slot. Otherwise return target
    unchanged."""
    m = re.match(r"^_GLOBAL_OFFSET_TABLE_\+0x([0-9a-fA-F]+)$", target)
    if not m:
        return target
    got_base = next((va for va, n in addr_map.items()
                     if n == "_GLOBAL_OFFSET_TABLE_"), None)
    if got_base is None:
        return target
    slot_va = got_base + int(m.group(1), 16)
    target_va = get_got_slot(elf_path, slot_va)
    if target_va is None:
        return target
    return addr_map.get(target_va, target)


def demangle_one(name, cppfilt):
    proc = subprocess.run(
        [cppfilt, "-p"],
        input=name,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        return name
    return proc.stdout.strip() or name


def disassemble(objdump, elf, sym):
    out, rc = run([
        objdump,
        f"--disassemble={sym}",
        "--no-show-raw-insn",
        str(elf),
    ])
    if rc != 0:
        return None
    # objdump prints the function header `<sym>:` then instructions
    # until a blank line.  Slice the section that belongs to `sym`.
    lines = out.splitlines()
    body = []
    in_target = False
    header_re = re.compile(rf"^[0-9a-f]+\s+<{re.escape(sym)}>:")
    for line in lines:
        if header_re.match(line):
            in_target = True
            body.append(line)
            continue
        if in_target:
            if line.strip() == "":
                break
            body.append(line)
    return "\n".join(body) if body else None


def summarise(disasm, current_sym, elf_path=None, addr_map=None):
    """Return (instruction_count, list_of_call_targets).

    Intra-function jumps (e.g. `je 12bac <current_sym+0x34>`) are
    excluded — their target ends with `+0x...`, indicating a jump to
    an offset inside `current_sym` itself.

    GOT-relative indirect calls (e.g. `_GLOBAL_OFFSET_TABLE_+0x288`)
    are resolved to the underlying function name when elf_path and
    addr_map are provided; otherwise reported verbatim.
    """
    insns = 0
    calls = []
    for line in disasm.splitlines():
        m = INSN_RE.match(line)
        if not m:
            continue
        insns += 1
        opcode = m.group(1).lower()
        if opcode not in CALL_OPCODES:
            continue
        tm = TARGET_RE.search(line)
        if not tm:
            continue
        target = tm.group(1)
        if target.startswith(current_sym + "+0x"):
            continue
        if elf_path and addr_map:
            target = resolve_got_callee(target, elf_path, addr_map)
        calls.append(target)
    return insns, calls


def find_matches(symbols, pattern_re):
    return [n for stype, n in symbols if stype.lower() in ("t",) and pattern_re.search(n)]


def _callee_matches(callee, patterns):
    """True if `callee` matches any pattern.

    Supported pattern forms:
      `prefix*`   — callee starts with `prefix`
      `*suffix`   — callee ends with `suffix`
      `*middle*`  — callee contains `middle`
      `exact`     — callee equals `exact`

    The `*X*` form matters for std/core symbols that rustc emits under
    legacy v0 mangling (`_ZN4core6result13unwrap_failed17h<hash>E`) on
    user code but new v0 mangling (`_RNvNtCs..._4core6result13unwrap_failed`)
    on pre-built rust-std after a stable bump — the encoded inner path
    is identical, so a substring match on the path component is stable
    across mangling-format changes."""
    for p in patterns:
        starts = p.startswith("*")
        ends = p.endswith("*")
        if starts and ends:
            if p[1:-1] in callee:
                return True
        elif ends:
            if callee.startswith(p[:-1]):
                return True
        elif starts:
            if callee.endswith(p[1:]):
                return True
        elif callee == p:
            return True
    return False


def check_one(sym_label, sym, disasm, expected, cppfilt, elf_path=None, addr_map=None):
    """Return (status, list_of_messages). status in {OK, WARN, FAIL}."""
    insns, calls = summarise(disasm, sym, elf_path, addr_map)
    msgs = [f"{sym_label}: {insns} insns, {len(calls)} call/jmp -> {calls}"]
    status = "OK"

    max_insns = expected.get("max_instructions") if expected else None
    allowed = expected.get("allowed_calls") if expected else None
    forbid_dem = expected.get("forbid_demangled", []) if expected else []

    if max_insns is not None and insns > max_insns:
        msgs.append(f"  WARN: {insns} > max_instructions={max_insns}")
        if status == "OK":
            status = "WARN"

    if allowed is not None:
        unexpected = [c for c in calls if not _callee_matches(c, allowed)]
        if unexpected:
            msgs.append(f"  FAIL: unexpected callees: {unexpected} "
                        f"(allowed: {sorted(allowed)})")
            status = "FAIL"

    if forbid_dem:
        dem = demangle_one(sym, cppfilt)
        for sub in forbid_dem:
            if sub in dem:
                msgs.append(f"  FAIL: demangled name contains '{sub}': {dem}")
                status = "FAIL"
    return status, msgs


def load_config(path):
    if not path or not Path(path).is_file():
        return {}
    # Avoid hard PyYAML dependency — accept JSON-equivalent or a tiny
    # YAML parser via the stdlib.  Try yaml first if available; fall
    # back to JSON for simple configs.
    text = Path(path).read_text()
    try:
        import yaml  # type: ignore
        return yaml.safe_load(text) or {}
    except ImportError:
        import json
        try:
            return json.loads(text)
        except json.JSONDecodeError as e:
            print(f"hotpath config requires PyYAML or JSON-compatible "
                  f"format ({path}): {e}", file=sys.stderr)
            sys.exit(2)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--elf", required=True)
    p.add_argument("--binding", required=True, choices=("c", "cpp", "rust", "zig"))
    p.add_argument("--target", default=None,
                   help="config key (default: derived from --binding)")
    p.add_argument("--config", default="tests/audit/hotpath_expected.yaml")
    p.add_argument("--output-dir", default="output/audit/disasm",
                   help="directory for per-symbol .S dumps")
    p.add_argument("--nm", default=None)
    p.add_argument("--objdump", default=None)
    p.add_argument("--cppfilt", default=None)
    p.add_argument("--strict", action="store_true",
                   help="treat WARN as FAIL (instruction-count overrun)")
    args = p.parse_args()

    nm = args.nm or shutil.which("nm")
    objdump = args.objdump or shutil.which("objdump")
    cppfilt = args.cppfilt or shutil.which("c++filt") or "c++filt"
    if not nm or not objdump:
        print("nm or objdump not found", file=sys.stderr)
        sys.exit(2)

    elf = Path(args.elf)
    if not elf.is_file():
        print(f"ELF not found: {elf}", file=sys.stderr)
        sys.exit(2)

    target_key = args.target or f"host_posix_benchmark_{args.binding}"
    full_config = load_config(args.config)
    # A missing/renamed/typo'd target key (or a missing config file, which
    # load_config returns as {}) must FAIL, not silently pass: with an empty
    # expected-symbol list the audit loop below runs zero times and reports
    # status=OK, so a wholly un-audited target would score green.  The audit
    # only runs for benchmark builds and every such target has an entry, so
    # this can only trip on genuine misconfiguration (e.g. a new
    # binding/board/variant added without a hotpath_expected.yaml entry).
    if target_key not in full_config:
        print(f"[hotpath dump] FAIL: target '{target_key}' not found in "
              f"{args.config} — add an entry or fix --target/--binding.",
              file=sys.stderr)
        sys.exit(2)
    config = full_config[target_key]
    expected_symbols = config.get("symbols", [])
    if not expected_symbols:
        print(f"[hotpath dump] FAIL: target '{target_key}' lists no "
              f"'symbols' to audit in {args.config}.", file=sys.stderr)
        sys.exit(2)

    symbols = list(list_symbols(nm, elf))
    addr_map = addr_to_symbol(nm, elf)
    output_dir = Path(args.output_dir) / target_key
    output_dir.mkdir(parents=True, exist_ok=True)

    overall = "OK"
    summary_lines = []
    for entry in expected_symbols:
        pat = re.compile(entry["pattern"])
        matches = find_matches(symbols, pat)
        label = entry.get("name", entry["pattern"])
        if not matches:
            summary_lines.append(f"{label}: NOT FOUND (pattern={entry['pattern']})")
            # Missing symbol is informational — likely fully inlined,
            # which is GOOD evidence of zero overhead.  Don't fail.
            continue
        for sym in matches:
            disasm = disassemble(objdump, elf, sym)
            if not disasm:
                summary_lines.append(f"{label}: {sym}: DISASSEMBLY FAILED")
                overall = "FAIL"
                continue
            # Persist disassembly artifact.
            safe_name = re.sub(r"[^A-Za-z0-9._-]+", "_", sym)[:120]
            (output_dir / f"{safe_name}.S").write_text(disasm + "\n")

            status, msgs = check_one(
                label, sym, disasm, entry, cppfilt,
                elf_path=elf, addr_map=addr_map,
            )
            summary_lines.extend(msgs)
            if status == "FAIL":
                overall = "FAIL"
            elif status == "WARN" and overall == "OK":
                overall = "WARN"

    print(f"[hotpath dump] target={target_key} status={overall}")
    for line in summary_lines:
        print(line)

    if args.strict and overall == "WARN":
        return 1
    return 1 if overall == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
