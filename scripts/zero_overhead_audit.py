#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Static symbol audit for oveRTOS "zero runtime overhead" claims.

Runs nm on a final ELF and fails if forbidden symbols are present:

  - C++:   vtable / typeinfo (any C++ polymorphism leaked into a binding)
  - Rust:  dyn-trait vtable patterns (no_std + no trait objects → zero)
  - All:   ove_* data symbols whose name matches a dispatch-table
           convention (`ove_*_ops`, `ove_*_dispatch`, `ove_*_vtable`,
           `ove_*_funcs`, `ove_*_jumptable`, `ove_*_callbacks`) — these
           would be runtime function-pointer dispatch tables in the
           kernel API surface, contradicting the "compile-time backend
           dispatch" claim.

Plain data descriptors (board configs, LED arrays, fixed register tables)
are NOT flagged: they hold compile-time constants, not function pointers.
Sim/dashboard plugin op-tables (`debug_ops`, `profiler_ops`, etc.) are
also NOT flagged because they live in host-side development infrastructure
that is outside the kernel "zero overhead" claim.

The audit enumerates ALL ove_* symbols (text + data) into
<output_dir>/<target>.txt for human review across builds.

Usage:
  zero_overhead_audit.py --elf <elf> --binding <c|cpp|rust|zig>
                         --target <name> [--nm <nm>] [--cppfilt <c++filt>]
                         [--output-dir <dir>] [--strict-data]
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


CPP_FORBIDDEN_DEMANGLED = (
    "vtable for ",
    "typeinfo for ",
    "typeinfo name for ",
)

# Mangled forms — caught even if demangling is unavailable.
CPP_FORBIDDEN_MANGLED_PREFIXES = ("_ZTV", "_ZTI", "_ZTS")

# Rust trait-object vtables use the C++ ABI conventions (legacy v0 mangling
# emits _ZTV-style symbols for &dyn Trait objects). Pure no_std crates with
# zero trait objects produce zero such symbols; any presence is a regression.
RUST_FORBIDDEN_DEMANGLED_SUBSTRS = ("<dyn ", "::vtable")

# Rust panic / formatter machinery — only checked when --no-panic-symbols
# is passed.  These are red flags in size-critical embedded ELFs where the
# crate sets `panic = "abort"` AND the apps avoid the std-fmt machinery
# (typically via a panic_immediate_abort nightly build-std).  In a default
# stable build with `panic = "abort"`, `core::panicking::panic_fmt` still
# survives as the panic entry symbol; that's expected and the gate is
# opt-in, not always-on.
RUST_FORBIDDEN_PANIC_SUBSTRS = (
    "core::panicking::",
    "core::fmt::Arguments::new_v1",
)

# Zig panic / runtime machinery — only checked when --no-panic-symbols
# is passed.  Mirrors Rust gate.  Caught by default Zig binding once
# `-fno-stack-check` is set and `pub fn panic` is overridden to abort;
# any survival in a release ELF means a runtime branch reached the
# panic path or the stack-probe runtime leaked through LTO.
ZIG_FORBIDDEN_PANIC_SUBSTRS = (
    "std.builtin.default_panic",
    "__zig_probe_stack",
    "panic_handler",
)

# Symbol-name suffixes that indicate a function-pointer dispatch table.
# Combined with an `ove_` prefix and a data nm-type, these mark a regression
# of the "compile-time backend dispatch" claim.  Plain data descriptors
# (ove_board_leds, ove_board_descriptor) don't match these suffixes; sim
# plugin op-tables (debug_ops, profiler_ops, etc.) lack the ove_ prefix
# and are out of scope.
DISPATCH_TABLE_NAME_RE = re.compile(
    r"^ove_.*_(ops|dispatch|vtable|jumptable|funcs|callbacks)$"
)


def run(cmd):
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"command failed ({' '.join(cmd)}):\n{res.stderr}", file=sys.stderr)
        sys.exit(2)
    return res.stdout


def demangle(names, cppfilt):
    """Return list of demangled names, parallel to input list."""
    if not names:
        return []
    proc = subprocess.run(
        [cppfilt, "-p"],
        input="\n".join(names),
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        # Demangling failed — fall back to raw names; the mangled-prefix
        # check still catches C++ vtables/typeinfo via _ZTV/_ZTI/_ZTS.
        return list(names)
    return proc.stdout.splitlines()


def parse_nm(nm_output):
    """Yield (addr, sym_type, name) for each symbol line."""
    pattern = re.compile(r"^([0-9a-fA-F]*)\s+([A-Za-z?])\s+(\S.*)$")
    for line in nm_output.splitlines():
        m = pattern.match(line)
        if m:
            yield m.group(1), m.group(2), m.group(3)


def audit(elf, binding, target, nm, cppfilt, output_dir,
          strict_data=False, no_panic_symbols=False):
    nm_out = run([nm, str(elf)])
    symbols = list(parse_nm(nm_out))

    raw_names = [name for _, _, name in symbols]
    demangled = demangle(raw_names, cppfilt)

    # Track findings.
    forbidden_cpp_vtable = []
    forbidden_cpp_typeinfo = []
    forbidden_rust_dyn = []
    forbidden_lang_panic = []
    forbidden_dispatch_tables = []
    forbidden_ove_data_strict = []
    ove_text_symbols = []
    ove_data_symbols = []

    for (addr, stype, raw), dem in zip(symbols, demangled):
        # ove_* symbols — split into text vs data for the artifact and
        # apply the dispatch-table-name rule on the data side.
        if raw.startswith("ove_"):
            if stype in ("T", "t", "U"):
                ove_text_symbols.append((stype, raw))
            elif stype in ("D", "d", "B", "b", "R", "r"):
                ove_data_symbols.append((stype, raw))
                if DISPATCH_TABLE_NAME_RE.match(raw):
                    forbidden_dispatch_tables.append((stype, raw))
                if strict_data:
                    forbidden_ove_data_strict.append((stype, raw))

        # C++ vtable / typeinfo — check both mangled prefix and demangled form.
        if any(raw.startswith(p) for p in CPP_FORBIDDEN_MANGLED_PREFIXES):
            if raw.startswith("_ZTV"):
                forbidden_cpp_vtable.append((stype, dem if dem != raw else raw))
            else:
                forbidden_cpp_typeinfo.append((stype, dem if dem != raw else raw))
            continue

        if "vtable for " in dem:
            forbidden_cpp_vtable.append((stype, dem))
        elif "typeinfo for " in dem or "typeinfo name for " in dem:
            forbidden_cpp_typeinfo.append((stype, dem))

        # Rust dyn-trait vtables (rare in no_std crates; any hit is a regression).
        if binding == "rust":
            if any(s in dem for s in RUST_FORBIDDEN_DEMANGLED_SUBSTRS):
                # Avoid false positives on harmless symbols that happen to
                # contain "::vtable" as a substring (e.g. internal symbols
                # in libcore for trait object support).  Require BOTH a
                # `<dyn` and `::vtable` to register as a vtable.
                if "<dyn " in dem and "::vtable" in dem:
                    forbidden_rust_dyn.append((stype, dem))
            if no_panic_symbols and any(
                s in dem for s in RUST_FORBIDDEN_PANIC_SUBSTRS
            ):
                forbidden_lang_panic.append((stype, dem))

        # Zig panic / stack-probe machinery — opt-in mirror of the Rust
        # gate.  Demangling does nothing for Zig (its symbols are already
        # human-readable), but checking against `dem` works because c++filt
        # passes through unrecognized formats unchanged.
        if binding == "zig" and no_panic_symbols:
            if any(s in dem or s in raw for s in ZIG_FORBIDDEN_PANIC_SUBSTRS):
                forbidden_lang_panic.append((stype, dem if dem != raw else raw))

    # Write the symbols artifact for review.
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    artifact = output_dir / f"{target}.txt"
    with artifact.open("w") as f:
        f.write(f"# zero-overhead audit: ove_* symbols in {elf}\n")
        f.write(f"# binding={binding} target={target}\n")
        f.write(f"# ove_* text symbols: {len(ove_text_symbols)} "
                f"(T/U = compile-time-resolved, no runtime dispatch)\n")
        f.write(f"# ove_* data symbols: {len(ove_data_symbols)} "
                f"(D/B/R = constants/state, NOT function pointers)\n")
        f.write("#\n")
        f.write("# === ove_* text/extern (T/t/U) ===\n")
        for stype, name in sorted(ove_text_symbols, key=lambda x: x[1]):
            f.write(f"{stype}\t{name}\n")
        f.write("#\n")
        f.write("# === ove_* data (D/d/B/b/R/r) ===\n")
        for stype, name in sorted(ove_data_symbols, key=lambda x: x[1]):
            f.write(f"{stype}\t{name}\n")

    # Print one-line summary to stdout (visible in build log).
    n_vt = len(forbidden_cpp_vtable)
    n_ti = len(forbidden_cpp_typeinfo)
    n_rd = len(forbidden_rust_dyn)
    n_rp = len(forbidden_lang_panic)
    n_dt = len(forbidden_dispatch_tables)
    n_sd = len(forbidden_ove_data_strict)
    n_txt = len(ove_text_symbols)
    n_dat = len(ove_data_symbols)
    fail_total = n_vt + n_ti + n_rd + n_rp + n_dt + n_sd
    status = "OK" if fail_total == 0 else "FAIL"
    print(
        f"[zero-overhead audit] target={target} binding={binding} "
        f"vtables={n_vt} typeinfo={n_ti} rust_dyn={n_rd} "
        f"lang_panic={n_rp} dispatch_tables={n_dt} "
        f"ove_text={n_txt} ove_data={n_dat} {status}"
    )

    if status == "FAIL":
        print(f"[zero-overhead audit] FAIL: forbidden symbols in {elf}", file=sys.stderr)
        for stype, name in forbidden_cpp_vtable:
            print(f"  C++ vtable          [{stype}] {name}", file=sys.stderr)
        for stype, name in forbidden_cpp_typeinfo:
            print(f"  C++ typeinfo        [{stype}] {name}", file=sys.stderr)
        for stype, name in forbidden_rust_dyn:
            print(f"  Rust dyn-vt         [{stype}] {name}", file=sys.stderr)
        for stype, name in forbidden_lang_panic:
            print(f"  panic/fmt symbol    [{stype}] {name}", file=sys.stderr)
        for stype, name in forbidden_dispatch_tables:
            print(f"  dispatch-table-name [{stype}] {name}", file=sys.stderr)
        for stype, name in forbidden_ove_data_strict:
            print(f"  ove_* data (strict) [{stype}] {name}", file=sys.stderr)
        return 1
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--elf", required=True, help="path to final ELF")
    p.add_argument("--binding", required=True, choices=("c", "cpp", "rust", "zig"))
    p.add_argument("--target", required=True, help="logical target name for artifact filename")
    p.add_argument("--nm", default=None, help="nm binary (default: from PATH)")
    p.add_argument("--cppfilt", default=None, help="c++filt binary (default: from PATH)")
    p.add_argument(
        "--output-dir",
        default="output/audit/symbols",
        help="directory for the ove_* externals list",
    )
    p.add_argument(
        "--strict-data",
        action="store_true",
        help="also fail on any ove_* data symbol (production-strict mode; "
             "default off because legitimate data descriptors exist)",
    )
    p.add_argument(
        "--no-panic-symbols",
        action="store_true",
        help="also fail on Rust core::panicking::* / fmt::Arguments::new_v1 "
             "(opt-in; only meaningful with `panic = \"abort\"` plus "
             "panic_immediate_abort, otherwise the panic entry survives)",
    )
    args = p.parse_args()

    nm = args.nm or shutil.which("nm")
    if not nm:
        print("nm not found", file=sys.stderr)
        sys.exit(2)
    cppfilt = args.cppfilt or shutil.which("c++filt") or "c++filt"

    elf = Path(args.elf)
    if not elf.is_file():
        print(f"ELF not found: {elf}", file=sys.stderr)
        sys.exit(2)

    sys.exit(audit(elf, args.binding, args.target, nm, cppfilt,
                   args.output_dir, args.strict_data, args.no_panic_symbols))


if __name__ == "__main__":
    main()
