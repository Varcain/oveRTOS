# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Shared helpers for the benchmark-report scripts.

`bench_audit.py` (iteration-count calibration) and `bench_compare.py`
(cross-binding comparison report) both consume the same JSON envelopes
emitted by oveRTOS benchmarks built with
`CONFIG_OVE_BENCHMARK_OUTPUT_JSON=y`.  Each envelope is bracketed by
``###BENCH_JSON_BEGIN`` / ``###BENCH_JSON_END`` sentinels in the
benchmark stdout dump and contains one suite-worth of cases.

Keeping the extraction logic here means a future format change (a
versioned sentinel, single-line JSON, an extra envelope-level field)
needs only one update.
"""

import json
import re
import sys


# Matches one envelope.  `re.DOTALL` so the JSON body may span lines;
# non-greedy so adjacent envelopes do not get merged.
JSON_BLOCK_RE = re.compile(
    r"###BENCH_JSON_BEGIN\s*\n(.+?)\n###BENCH_JSON_END",
    re.DOTALL,
)


def extract_envelopes(text):
    """Yield parsed JSON suite envelopes from a stdout dump.

    Malformed envelopes are reported to stderr and skipped — callers
    typically want the remaining well-formed envelopes rather than an
    all-or-nothing failure on a single corrupted block.
    """
    for m in JSON_BLOCK_RE.finditer(text):
        try:
            yield json.loads(m.group(1).strip())
        except json.JSONDecodeError as e:
            print(f"warning: malformed JSON envelope skipped: {e}",
                  file=sys.stderr)
