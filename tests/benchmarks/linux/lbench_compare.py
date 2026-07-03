#!/usr/bin/env python3
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Personality-tax report: pair the native bench (tests/benchmarks/c) against the
# FDPIC lbench (buildroot board/overtos/progs/lbench.c) per axis and print the
# overhead of running work as an unprivileged Linux process vs a native thread.
#
# Both sides emit ###BENCH_JSON_BEGIN/END envelopes.  The native side runs as an
# ove app (binding "c", suites compute/thread/stream/...); the Linux side is the
# "personality" suite.  On real STM32F746 both time off DWT->CYCCNT, so the ns
# figures are directly comparable; --mhz converts ns→cycles.
#
# Usage:
#   lbench_compare.py --native <native-stdout> --linux <lbench-stdout> [--mhz 216]
# Each input is a raw stdout capture (envelopes are extracted); pass the same
# file twice if one capture contains both.
import argparse
import json
import re
import sys

# Axis  ->  (native suite, native case, linux case, note).  The native case is
# the closest same-world analogue; the delta/ratio is the personality tax.
AXES = [
    ("B1 compute",      "compute", "compute_mix",   "compute_mix",      "same kernel; ratio≈1+FDPIC-PIC"),
    ("B2 syscall",      "compute", "null_call",     "null_syscall",     "SVC trap vs plain call"),
    ("B2 getpid",       "compute", "null_call",     "getpid_cached",    "libc getpid (uClibc: uncached)"),
    ("B3 write",        "stream",  None,            "write_devnull",    "write() syscall (abs)"),
    ("B5 ctx-switch",   "thread",  "context_switch","ctx_switch",       "pipe RT vs sem ping-pong"),
    ("B6 IPC 4KiB",     "stream",  None,            "pipe_wr_4k",        "pipe drain vs native stream"),
    ("B7 spawn",        "thread",  "create_destroy","spawn_vfork_exec", "process load+MPU vs thread create"),
]


def extract(text):
    """dict: (suite, case) -> case_obj, from every JSON envelope in `text`."""
    out = {}
    for m in re.finditer(r"###BENCH_JSON_BEGIN\s*(\{.*?\})\s*###BENCH_JSON_END", text, re.S):
        try:
            env = json.loads(m.group(1))
        except json.JSONDecodeError:
            continue
        suite = env.get("suite", "?")
        for c in env.get("cases", []):
            out[(suite, c.get("name"))] = c
    return out


def cyc(ns, mhz):
    return None if ns is None else round(ns * mhz / 1000.0)


def fmt(ns, mhz):
    if ns is None:
        return "—"
    c = cyc(ns, mhz)
    if ns >= 1000:
        return f"{ns/1000:.1f}µs/{c}c"
    return f"{ns}ns/{c}c"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--native", required=True)
    ap.add_argument("--linux", required=True)
    ap.add_argument("--mhz", type=float, default=216.0, help="CPU MHz for ns→cycles (STM32F746=216)")
    ap.add_argument("--metric", default="min_ns", choices=["min_ns", "p50_ns", "avg_ns"])
    a = ap.parse_args()

    nat = extract(open(a.native).read())
    lnx = extract(open(a.linux).read())
    if not lnx:
        print("no personality (lbench) envelope found in --linux input", file=sys.stderr)
        sys.exit(1)

    ck = next((c for (s, n), c in lnx.items()), {})
    print(f"# Personality tax  (metric={a.metric}, {a.mhz:g} MHz; ns/cycles)\n")
    print(f"{'axis':<14} {'native':>14} {'personality':>16} {'tax':>10}   note")
    print("-" * 88)
    for label, nsuite, ncase, lcase, note in AXES:
        lo = lnx.get(("personality", lcase))
        ln_ns = lo.get(a.metric) if lo else None
        na = nat.get((nsuite, ncase)) if ncase else None
        na_ns = na.get(a.metric) if na else None
        if ln_ns and na_ns:
            ratio = ln_ns / na_ns
            tax = f"{ratio:.1f}x" if ratio >= 1 else f"{ratio:.2f}x"
        elif ln_ns and na_ns == 0:
            tax = "∞ (nat 0)"
        else:
            tax = "—"
        print(f"{label:<14} {fmt(na_ns, a.mhz):>14} {fmt(ln_ns, a.mhz):>16} {tax:>10}   {note}")
    print("-" * 88)
    print("native '—' = no directly-comparable native case (B3/B6 are absolute personality costs;")
    print("their native analogue is the stream/queue suite — compare against those rows by hand).")


if __name__ == "__main__":
    main()
