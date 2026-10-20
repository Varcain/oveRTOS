#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract the NuttX suspend/resume handshake from the backend.

The handshake (the SIGUSR1 suspend loop, the suspend request/release
protocol and the public ``ove_thread_suspend()`` / ``ove_thread_resume()``)
is compiled *verbatim* into the host regression test
``tests/obs/nuttx_suspend/`` so the test always exercises the real source
text of ``backends/nuttx/nuttx_thread.c`` - never a copy that can drift
away from it.

Three regions are extracted:

1. the suspend request/release helpers and the ``sigusr1_handler()`` (the
   helpers only exist in the fixed backend; the pre-fix backend starts the
   region at the handler),
2. ``task_wrapper()`` (the task entry that honours a suspend request that
   was published before the task's first dispatch),
3. ``ove_thread_suspend()`` and ``ove_thread_resume()``.

The regions are located by their function signatures, so a reworded
comment above them cannot break the extraction.

Usage::

    extract_nuttx_suspend_block.py <nuttx_thread.c> <output.inc>
"""

import sys

# Region 1: the handshake helpers plus the signal handler.
R1_OPTIONAL_START = "#define OVE_NUTTX_SUSPEND_ACK_US"
R1_START = "static void sigusr1_handler(int sig)"
R1_END = "static void sigusr1_handler(int sig)"

# Region 2: the task entry.
R2_START = "static int task_wrapper(int argc, char *argv[])"
R2_END = "static int task_wrapper(int argc, char *argv[])"

# Region 3: the public suspend/resume pair.
R3_START = "void ove_thread_suspend(ove_thread_t handle)"
R3_END = "void ove_thread_resume(ove_thread_t handle)"


def _strip(line):
    """Return *line* without comments and string/char literals."""
    out = []
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c == "/" and i + 1 < n and line[i + 1] == "*":
            end = line.find("*/", i + 2)
            if end < 0:
                break
            i = end + 2
            out.append(" ")
            continue
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            break
        if c in "\"'":
            quote = c
            i += 1
            while i < n:
                if line[i] == "\\":
                    i += 2
                    continue
                if line[i] == quote:
                    i += 1
                    break
                i += 1
            out.append(" ")
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _find(lines, prefix, start=0):
    """Index of the first line at or after *start* that starts with *prefix*."""
    for index in range(start, len(lines)):
        if lines[index].startswith(prefix):
            return index
    return -1


def _block_end(lines, start):
    """Index of the line that closes the brace block opened at *start*."""
    depth = 0
    opened = False
    for index in range(start, len(lines)):
        for char in _strip(lines[index]):
            if char == "{":
                depth += 1
                opened = True
            elif char == "}":
                depth -= 1
                if opened and depth == 0:
                    return index
    return -1


def _region(lines, start_prefix, end_prefix, optional_start=None):
    """The source lines of the region between the two anchors."""
    start = -1
    if optional_start is not None:
        start = _find(lines, optional_start)
    if start < 0:
        start = _find(lines, start_prefix)
    if start < 0:
        raise SystemExit(f"cannot locate {start_prefix!r} in the NuttX backend")
    end_anchor = _find(lines, end_prefix, start)
    if end_anchor < 0:
        raise SystemExit(f"cannot locate {end_prefix!r} in the NuttX backend")
    end = _block_end(lines, end_anchor)
    if end < 0:
        raise SystemExit(f"cannot locate the end of {end_prefix!r} in the NuttX backend")
    return lines[start:end + 1]


def main(argv):
    if len(argv) != 3:
        raise SystemExit(
            "usage: extract_nuttx_suspend_block.py <nuttx_thread.c> <output.inc>")
    source, destination = argv[1], argv[2]
    with open(source, encoding="utf-8") as handle:
        lines = handle.readlines()

    body = []
    body += _region(lines, R1_START, R1_END, optional_start=R1_OPTIONAL_START)
    body.append("\n")
    body += _region(lines, R2_START, R2_END)
    body.append("\n")
    body += _region(lines, R3_START, R3_END)

    with open(destination, "w", encoding="utf-8") as handle:
        handle.write("/* Generated from backends/nuttx/nuttx_thread.c "
                     "- do not edit. */\n")
        handle.write("".join(body))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
