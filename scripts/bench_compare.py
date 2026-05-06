#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Cross-binding benchmark comparison report.

Reads benchmark stdout dumps that contain JSON envelopes
(`###BENCH_JSON_BEGIN` / `###BENCH_JSON_END` sentinels emitted when
CONFIG_OVE_BENCHMARK_OUTPUT_JSON=y) and produces a Markdown table
joining cases across the C / C++ / Rust / Zig bindings.

When a `native_posix` (or other native_*) suite is present, its rows
are emitted alongside the wrapper rows so the reader can see at a
glance "wrapper vs raw API: ±X ns".

Usage:
  bench_compare.py --input <bench-stdout-or-json-file> ...
                   [--output <report.md>] [--threshold-pct N]

Each input file may be:
  - The raw stdout of one bench run (sentinels found and parsed); OR
  - A bare JSON file containing one suite per object on its own line.
The "binding" field of each JSON envelope determines which column the
case lands in; "rtos" is required to be consistent across all inputs.
"""

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path


JSON_BLOCK_RE = re.compile(
    r"###BENCH_JSON_BEGIN\s*\n(.+?)\n###BENCH_JSON_END",
    re.DOTALL,
)


def extract_envelopes(text):
    """Yield parsed JSON suite envelopes from a stdout dump."""
    for m in JSON_BLOCK_RE.finditer(text):
        try:
            yield json.loads(m.group(1).strip())
        except json.JSONDecodeError as e:
            print(f"warning: malformed JSON envelope skipped: {e}",
                  file=sys.stderr)


def load_inputs(paths):
    """Return dict: rtos -> {binding -> {(suite, case) -> case_dict}}."""
    by_rtos = defaultdict(lambda: defaultdict(dict))
    for path in paths:
        text = Path(path).read_text()
        for suite in extract_envelopes(text):
            rtos = suite.get("rtos", "?")
            binding = suite.get("binding", "?")
            sname = suite.get("suite", "?")
            for case in suite.get("cases", []):
                key = (sname, case.get("name", "?"))
                by_rtos[rtos][binding][key] = case
    return by_rtos


def fmt_ns(n):
    if n is None:
        return "—"
    if n >= 1_000_000:
        return f"{n / 1_000_000:.2f} ms"
    if n >= 1_000:
        return f"{n / 1_000:.1f} µs"
    return f"{n} ns"


def fmt_delta_pct(wrapper, baseline):
    if wrapper is None or baseline is None or baseline == 0:
        return "—"
    pct = 100.0 * (wrapper - baseline) / baseline
    sign = "+" if pct >= 0 else ""
    return f"{sign}{pct:.1f}%"


def case_metric(case):
    """Return the comparison metric for a case (trimmed_mean preferred,
    avg_ns fallback). For memory cases, use heap_delta."""
    if case is None:
        return None
    if case.get("type") == "memory":
        return case.get("heap_delta")
    return case.get("trimmed_mean_ns") or case.get("avg_ns")


# Page-specific RTOS metadata used when --page-mode is passed.  Keeps
# the per-RTOS doc page header (display name, native bench filename,
# representative native APIs) in one place so each new bench run drops
# straight into docs-site/docs/benchmarks/<rtos>-<mode>.md without any
# manual header restoration.
_PAGE_RTOS_META = {
    "FreeRTOS": {
        "display":    "FreeRTOS",
        "key":        "freertos",
        "long":       "FreeRTOS",
        "native_src": "bench_native_freertos.c",
        "native_apis": "`xSemaphoreTake`, `xQueueSend`, `xTaskCreateStatic`",
    },
    "NuttX": {
        "display":    "NuttX",
        "key":        "nuttx",
        "long":       "Apache NuttX",
        "native_src": "bench_native_nuttx.c",
        "native_apis": "`nxsem_*`, `pthread_*`",
    },
    "Zephyr": {
        "display":    "Zephyr",
        "key":        "zephyr",
        "long":       "Zephyr RTOS",
        "native_src": "bench_native_zephyr.c",
        "native_apis": "`k_mutex_*`, `k_sem_*`, `k_msgq_*`",
    },
    "POSIX": {
        "display":    "POSIX",
        "key":        "posix",
        "long":       "POSIX (host)",
        "native_src": "bench_native_posix.c",
        "native_apis": "raw pthread/sem APIs",
    },
}


def _page_header(by_rtos, mode, threshold_pct):
    """Render a docs-site page-specific header (RTOS + mode aware).

    Used when bench_compare is invoked with --page-mode {heap,zeroheap}
    so the resulting report.md drops into docs-site/docs/benchmarks/
    without any manual header restoration step.  Falls back to the
    generic cross-binding header if the inputs span multiple RTOSes
    (page model assumes one RTOS per file).

    Page contents are intentionally raw: setup + tables + outlier list.
    Interpretation lives on dedicated pages (heap-vs-zeroheap.md,
    per-binding.md, wrapper-vs-native-notes.md) so the data here can be
    regenerated from a fresh bench run without trampling prose.
    """
    rtos_keys = list(by_rtos.keys())
    if len(rtos_keys) != 1 or rtos_keys[0] not in _PAGE_RTOS_META:
        return None
    meta = _PAGE_RTOS_META[rtos_keys[0]]
    name, long_, native_src, native_apis = (
        meta["display"], meta["long"], meta["native_src"], meta["native_apis"])
    key = meta["key"]
    out = []
    if mode == "heap":
        out.append(f"# {name} — heap mode\n")
        out.append(
            f"Raw cross-binding benchmark results measured on "
            f"**STM32F746G-DISCOVERY** (Cortex-M7 @ 216 MHz) running "
            f"{long_} in default heap-allocation mode "
            f"(`_create()` / `_destroy()` API), with "
            f"`CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING=y` — caches and "
            f"flash accelerators disabled to approximate cacheless "
            f"ARM-MCU timing.  See [benchmarks overview](index.md) "
            f"for the worst-case-timing methodology.\n"
        )
        out.append(
            f"Methodology and reproduction steps: "
            f"[benchmarks overview](index.md).  Same numbers under "
            f"`CONFIG_OVE_ZERO_HEAP=y`: "
            f"[{name} zero-heap]({key}-zeroheap.md).  Interpretation: "
            f"[heap vs zero-heap](heap-vs-zeroheap.md), "
            f"[per-binding analysis](per-binding.md), "
            f"[wrapper-vs-native notes](wrapper-vs-native-notes.md).\n"
        )
    elif mode == "zeroheap":
        out.append(f"# {name} — zero-heap mode\n")
        out.append(
            f"Raw cross-binding benchmark results measured on "
            f"**STM32F746G-DISCOVERY** (Cortex-M7 @ 216 MHz) running "
            f"{long_} with `CONFIG_OVE_ZERO_HEAP=y` "
            f"(`_init()` / `_deinit()` API, caller-supplied static "
            f"storage, heap locked at `ove_run()`) and "
            f"`CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING=y` — caches and "
            f"flash accelerators disabled to approximate cacheless "
            f"ARM-MCU timing.  See [benchmarks overview](index.md) "
            f"for the worst-case-timing methodology.\n"
        )
        out.append(
            f"Under zero-heap, `*_create_destroy` and `*_memory` cases "
            f"are gated out — the create/destroy API isn't generated.\n"
        )
        out.append(
            f"Methodology and reproduction steps: "
            f"[benchmarks overview](index.md).  Heap-mode counterpart: "
            f"[{name} heap mode]({key}-heap.md).  Interpretation: "
            f"[heap vs zero-heap](heap-vs-zeroheap.md), "
            f"[per-binding analysis](per-binding.md), "
            f"[wrapper-vs-native notes](wrapper-vs-native-notes.md).\n"
        )
    else:
        return None
    out.append(
        f"> *Generated by `scripts/bench_compare.py`. Trimmed-mean "
        f"(top 1% dropped) when available, else avg. Delta column is "
        f"`(binding − C) / C` — values within ±{threshold_pct}% are "
        f"within typical measurement noise.*\n"
    )
    out.append(
        f"**`native_*` rows.** `{native_src}` is C code calling raw "
        f"{name} APIs ({native_apis}), compiled identically into every "
        f"binary; the CPP/RUST/ZIG columns for those rows are the same "
        f"C code measured in three different processes.\n"
    )
    # Section heading replaces the legacy "## RTOS: <name>" so the
    # downstream table renderer doesn't re-emit it (suppressed via
    # page_mode flag in build_report).
    out.append(f"## {name}\n")
    return "\n".join(out)


def _generic_header(threshold_pct):
    """Cross-RTOS header (legacy path, used when --page-mode not passed
    or when inputs span multiple RTOSes)."""
    return "\n".join([
        "# oveRTOS Cross-Binding Benchmark Comparison\n",
        f"Generated by `scripts/bench_compare.py`. "
        f"Trimmed-mean (top 1% dropped) when available, else avg. "
        f"Delta column is `(binding − C) / C` — values within "
        f"±{threshold_pct}% are within typical measurement noise.\n",
        "**`native_*` rows.** `bench_native_<rtos>.c` is C code calling "
        "raw RTOS APIs, compiled identically into every binary; the "
        "CPP/RUST/ZIG columns for those rows are the same C code "
        "measured in three different processes.\n",
    ])


def build_report(by_rtos, threshold_pct, page_mode=None):
    lines = []
    page_hdr = _page_header(by_rtos, page_mode, threshold_pct) if page_mode else None
    if page_hdr is not None:
        lines.append(page_hdr)
    else:
        lines.append(_generic_header(threshold_pct))

    BINDING_ORDER = ["c", "cpp", "rust", "zig"]

    for rtos in sorted(by_rtos.keys()):
        by_binding = by_rtos[rtos]
        bindings_present = [b for b in BINDING_ORDER if b in by_binding]
        if not bindings_present:
            continue
        # The page-mode header already emitted "## <RTOS>"; don't double-print.
        if not (page_mode and page_hdr is not None):
            lines.append(f"\n## RTOS: {rtos}\n")

        # Determine the union of (suite, case) keys, preserving suite/case
        # ordering as observed in C (the canonical baseline) when present.
        all_keys = []
        seen = set()
        c_cases = by_binding.get("c", {})
        for k in c_cases.keys():
            all_keys.append(k)
            seen.add(k)
        for b in bindings_present:
            for k in by_binding[b].keys():
                if k not in seen:
                    all_keys.append(k)
                    seen.add(k)

        # Header: binding columns + delta columns (vs C, only for non-C).
        header = ["Suite", "Case"]
        for b in bindings_present:
            header.append(b.upper())
            if b != "c":
                header.append(f"Δ {b.upper()}")
        lines.append("| " + " | ".join(header) + " |")
        lines.append("|" + "|".join("---" for _ in header) + "|")

        for suite_name, case_name in all_keys:
            row = [suite_name, case_name]
            c_metric = case_metric(c_cases.get((suite_name, case_name)))
            for b in bindings_present:
                cdict = by_binding[b].get((suite_name, case_name))
                metric = case_metric(cdict)
                if cdict is None:
                    row.append("—")
                elif cdict.get("type") == "memory":
                    row.append(f"{metric} B" if metric is not None else "—")
                else:
                    row.append(fmt_ns(metric))
                if b != "c":
                    if cdict is None or cdict.get("type") == "memory":
                        row.append("—")
                    else:
                        row.append(fmt_delta_pct(metric, c_metric))
            lines.append("| " + " | ".join(row) + " |")

        # Summary block: outliers vs C (>threshold or <-threshold).
        notable = []
        for k in all_keys:
            c_dict = c_cases.get(k)
            if c_dict is None or c_dict.get("type") == "memory":
                continue
            c_v = case_metric(c_dict)
            if not c_v:
                continue
            for b in bindings_present:
                if b == "c":
                    continue
                wd = by_binding[b].get(k)
                wv = case_metric(wd)
                if not wv:
                    continue
                pct = 100.0 * (wv - c_v) / c_v
                if abs(pct) > threshold_pct:
                    notable.append((b.upper(), k[0], k[1], pct, wv, c_v))

        if notable:
            lines.append(
                f"\n**Cases with |Δ| > {threshold_pct}% vs C:**\n"
            )
            for b, suite, case, pct, wv, cv in sorted(
                    notable, key=lambda r: -abs(r[3])):
                lines.append(
                    f"- **{b}** `{suite}/{case}` "
                    f"{wv} vs {cv} ({'+' if pct >= 0 else ''}{pct:.1f}%)"
                )
        else:
            lines.append(
                f"\nAll non-memory cases within ±{threshold_pct}% of C.\n"
            )

        # Wrapper-vs-native within-run deltas — each row is one
        # (binding × operation) pair from a single benchmark process,
        # so per-run scheduler noise cancels and the Δ column is the
        # raw "wrapper code path − raw API code path" in nanoseconds.
        # Per-RTOS interpretation (IPC caveats, lifecycle costs) lives
        # in docs-site/docs/benchmarks/wrapper-vs-native-notes.md so it
        # isn't trampled when this script regenerates the page.
        #
        # Categories: threading, mutex, semaphore, condvar/event, IPC.
        # Native baseline suite name is detected per binding (one of
        # native_posix / native_freertos / native_nuttx / native_zephyr).
        # All share identical case-name suffixes so a single template
        # works for every backend.  Cases without a meaningful raw
        # equivalent (event groups, workqueues) are absent.
        _WRAPPER_NATIVE_TEMPLATES = [
            # (label, wrapper_key, native_case_suffix)
            ("Thread yield",                ("thread", "yield"),                          "thread_yield"),
            ("Thread sleep 1ms",            ("thread", "sleep_1ms"),                      "thread_sleep_1ms"),
            ("Thread create+destroy",       ("thread", "create_destroy"),                 "thread_create_destroy"),
            ("Thread context_switch (2t)",  ("thread", "context_switch"),                 "thread_context_switch"),
            ("Mutex lock+unlock",           ("sync",   "mutex_lock_unlock"),              "mutex_lock_unlock"),
            ("Mutex create+destroy",        ("sync",   "mutex_create_destroy"),           "mutex_create_destroy"),
            ("Mutex contention (2t)",       ("sync",   "mutex_contention_2t"),            "mutex_contention_2t"),
            ("Recursive mutex lock+unlock", ("sync",   "recursive_mutex_lock_unlock"),    "recursive_mutex_lock_unlock"),
            ("Sem take+give",               ("sync",   "sem_take_give"),                  "sem_take_give"),
            ("Sem create+destroy",          ("sync",   "sem_create_destroy"),             "sem_create_destroy"),
            ("Condvar signal+wait",         ("sync",   "condvar_signal_wait"),            "condvar_signal_wait"),
            ("Event signal+wait",           ("sync",   "event_signal_wait"),              "event_signal_wait"),
            ("Queue send+receive",          ("queue",  "send_receive"),                   "queue_send_receive"),
            ("Queue create+destroy",        ("queue",  "create_destroy"),                 "queue_create_destroy"),
            ("Stream send+recv 64B",        ("stream", "send_recv_64B"),                  "stream_send_recv_64B"),
        ]
        # Detect which native suite is present per binding (one of
        # native_posix, native_freertos) and build the concrete pair
        # list.  All bindings in a single RTOS report share the same
        # native suite name, so we resolve it from any binding's keys.
        native_suite = None
        for b in bindings_present:
            for sname, _ in by_binding[b].keys():
                if sname.startswith("native_"):
                    native_suite = sname
                    break
            if native_suite:
                break
        WRAPPER_NATIVE_PAIRS = [
            (label, w_key, (native_suite, "native_" + suffix))
            for (label, w_key, suffix) in _WRAPPER_NATIVE_TEMPLATES
        ] if native_suite else []
        # Per-binding wrapper labels (rendered in the "Wrapper called"
        # column).  Keyed by (suite, case) so create_destroy /
        # send_receive don't collide between thread vs queue vs stream.
        WRAPPER_LABELS = {
            ("sync",   "mutex_lock_unlock"):           {"c": "ove_mutex_lock+unlock",       "cpp": "ove::Mutex::lock+unlock",         "rust": "ove::Mutex::lock+unlock",       "zig": "ove.Mutex.lock+unlock"},
            ("sync",   "mutex_create_destroy"):        {"c": "ove_mutex_create+destroy",    "cpp": "ove::Mutex (ctor+dtor)",          "rust": "ove::Mutex (new+drop)",         "zig": "ove.Mutex.create+destroy"},
            ("sync",   "mutex_contention_2t"):         {"c": "ove_mutex_lock+unlock (×2t)", "cpp": "ove::Mutex::lock+unlock (×2t)",   "rust": "ove::Mutex::lock+unlock (×2t)", "zig": "ove.Mutex.lock+unlock (×2t)"},
            ("sync",   "recursive_mutex_lock_unlock"): {"c": "ove_rmtx_lock+unlock",        "cpp": "ove::RMutex::lock+unlock",        "rust": "ove::RMutex::lock+unlock",      "zig": "ove.RMutex.lock+unlock"},
            ("sync",   "sem_take_give"):               {"c": "ove_sem_take+give",           "cpp": "ove::Sem::take+give",             "rust": "ove::Sem::take+give",           "zig": "ove.Sem.take+give"},
            ("sync",   "sem_create_destroy"):          {"c": "ove_sem_create+destroy",      "cpp": "ove::Sem (ctor+dtor)",            "rust": "ove::Sem (new+drop)",           "zig": "ove.Sem.create+destroy"},
            ("sync",   "condvar_signal_wait"):         {"c": "ove_condvar_signal+wait",     "cpp": "ove::Condvar::signal+wait",       "rust": "ove::Condvar::signal+wait",     "zig": "ove.Condvar.signal+wait"},
            ("sync",   "event_signal_wait"):           {"c": "ove_event_signal+wait",       "cpp": "ove::Event::signal+wait",         "rust": "ove::Event::signal+wait",       "zig": "ove.Event.signal+wait"},
            ("thread", "yield"):                       {"c": "ove_thread_yield",            "cpp": "ove::Thread::yield",              "rust": "ove::Thread::yield",            "zig": "ove.Thread.yield"},
            ("thread", "sleep_1ms"):                   {"c": "ove_thread_sleep_ms(1)",      "cpp": "ove::Thread::sleep_ms(1)",        "rust": "ove::Thread::sleep_ms(1)",      "zig": "ove.Thread.sleepMs(1)"},
            ("thread", "create_destroy"):              {"c": "ove_thread_create+destroy",   "cpp": "ove::Thread (ctor+dtor)",         "rust": "ove::Thread::spawn+join",       "zig": "ove.Thread.spawn+join"},
            ("thread", "context_switch"):              {"c": "ove ping-pong (2t)",          "cpp": "ove ping-pong (2t)",              "rust": "ove ping-pong (2t)",            "zig": "ove ping-pong (2t)"},
            ("queue",  "send_receive"):                {"c": "ove_queue_send+receive",      "cpp": "ove::Queue::send+recv",           "rust": "ove::Queue::send+recv",         "zig": "ove.Queue.send+recv"},
            ("queue",  "create_destroy"):              {"c": "ove_queue_create+destroy",    "cpp": "ove::Queue (ctor+dtor)",          "rust": "ove::Queue (new+drop)",         "zig": "ove.Queue.create+destroy"},
            ("stream", "send_recv_64B"):               {"c": "ove_stream_send+recv 64B",    "cpp": "ove::Stream::send+recv 64B",      "rust": "ove::Stream::send+recv 64B",    "zig": "ove.Stream.send+recv 64B"},
        }
        wn_rows = []
        for label, w_key, n_key in WRAPPER_NATIVE_PAIRS:
            for b in bindings_present:
                w = case_metric(by_binding[b].get(w_key))
                n = case_metric(by_binding[b].get(n_key))
                if w is None or n is None:
                    continue
                delta = w - n
                sign = "+" if delta >= 0 else ""
                lbl_map = WRAPPER_LABELS.get(w_key, {})
                wrapper_label = lbl_map.get(b, w_key[1])
                wn_rows.append([
                    label,
                    b.upper(),
                    wrapper_label,
                    f"{w} ns",
                    f"{n} ns",
                    f"{sign}{delta} ns",
                ])

        if wn_rows:
            native_label = {
                "native_posix":    "pthread",
                "native_freertos": "FreeRTOS API",
                "native_nuttx":    "NuttX API",
                "native_zephyr":   "Zephyr API",
            }.get(native_suite, "native API")
            lines.append(
                f"\n### Wrapper vs native {native_label} (within-run delta)\n"
            )
            lines.append(
                f"Each row pairs one binding's wrapper measurement against "
                f"the raw {native_label} baseline measured in the same "
                f"process.  See "
                f"[wrapper-vs-native notes](wrapper-vs-native-notes.md) "
                f"for IPC caveats, lifecycle/intrinsic-cost interpretation, "
                f"and notes on cross-process baseline variance.\n"
            )
            header = ["Operation", "Binding", "Wrapper called", "Wrapper ns", "Native ns", "Δ"]
            lines.append("| " + " | ".join(header) + " |")
            lines.append("|" + "|".join("---" for _ in header) + "|")
            for row in wn_rows:
                lines.append("| " + " | ".join(row) + " |")

    return "\n".join(lines) + "\n"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--input", nargs="+", required=True,
                   help="bench stdout dumps to compare")
    p.add_argument("--output", default=None,
                   help="markdown report output path (default: stdout)")
    p.add_argument("--threshold-pct", type=float, default=10.0,
                   help="highlight cases where binding-vs-C exceeds this")
    p.add_argument("--page-mode", choices=["heap", "zeroheap"], default=None,
                   help="emit a docs-site/docs/benchmarks/<rtos>-<mode>.md "
                        "page header (RTOS+mode-specific) instead of the "
                        "generic cross-binding header.  Use when the input "
                        "spans exactly one RTOS — e.g. when called from the "
                        "bench harness for a single platform.")
    args = p.parse_args()

    by_rtos = load_inputs(args.input)
    if not by_rtos:
        print("no JSON envelopes found in inputs", file=sys.stderr)
        sys.exit(1)

    report = build_report(by_rtos, args.threshold_pct,
                          page_mode=args.page_mode)

    if args.output:
        Path(args.output).write_text(report)
        print(f"wrote {args.output}")
    else:
        print(report)


if __name__ == "__main__":
    main()
