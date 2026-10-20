#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract the NuttX scheduler-note observation block from the backend.

The observation code (the ``obs_tasks[]`` table and the ``obs_note_*()``
handlers that implement the scheduler-note driver) is compiled *verbatim*
into the host regression test ``tests/obs/nuttx_note/`` so the test always
exercises the real source text of ``backends/nuttx/nuttx_thread.c`` - never
a copy that can drift away from it.

The extracted block is the ``#ifdef CONFIG_OVE_THREAD_STATE_STATS`` region
that holds ``obs_tasks[]``; it is located by its ``OBS_MAX_TASKS`` anchor so
a reworded comment above it cannot break the extraction.

Usage::

    extract_nuttx_obs_block.py <nuttx_thread.c> <output.inc>
"""

import sys


def _extract(lines):
    """Return the observation block (list of lines) of nuttx_thread.c."""
    anchors = [i for i, line in enumerate(lines)
               if line.startswith("#define OBS_MAX_TASKS")]
    if not anchors:
        raise SystemExit("cannot locate OBS_MAX_TASKS in the NuttX backend")
    anchor = anchors[0]

    start = None
    for i in range(anchor, -1, -1):
        if lines[i].strip() == "#ifdef CONFIG_OVE_THREAD_STATE_STATS":
            start = i
            break
    if start is None:
        raise SystemExit("cannot locate the observation block in the NuttX backend")

    depth = 0
    for i in range(start, len(lines)):
        stripped = lines[i].strip()
        if stripped.startswith("#if"):
            depth += 1
        elif stripped.startswith("#endif"):
            depth -= 1
            if depth == 0:
                return lines[start:i + 1]
    raise SystemExit("unterminated #ifdef block in the NuttX backend")


def main(argv):
    if len(argv) != 3:
        raise SystemExit(
            "usage: extract_nuttx_obs_block.py <nuttx_thread.c> <output.inc>")
    source, destination = argv[1], argv[2]
    with open(source, encoding="utf-8") as handle:
        lines = handle.readlines()
    body = "".join(_extract(lines))
    with open(destination, "w", encoding="utf-8") as handle:
        handle.write("/* Generated from backends/nuttx/nuttx_thread.c "
                     "- do not edit. */\n")
        handle.write(body)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
