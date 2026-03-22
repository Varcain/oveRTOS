#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Extract ove storage type sizes and alignments from a compiled object file.

Usage: extract_storage_sizes.py <object_file> <output_file>

Compiles a C file with sizeof/alignof arrays, then parses the nm output
to produce KEY=VALUE pairs suitable for passing as env vars to cargo.
"""

import re
import subprocess
import sys


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <object_file> <output_file>", file=sys.stderr)
        sys.exit(1)

    obj_file = sys.argv[1]
    out_file = sys.argv[2]

    # Run nm --print-size on the object file
    result = subprocess.run(
        ["nm", "--print-size", obj_file],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print(f"nm failed: {result.stderr}", file=sys.stderr)
        sys.exit(1)

    # Parse lines like: 00000000 000000c4 B _sizeof_ove_thread_storage_t
    pattern = re.compile(
        r'[0-9a-f]+\s+([0-9a-f]+)\s+[A-Z]\s+_(sizeof|alignof)_(\w+)'
    )

    sizes = {}
    for line in result.stdout.splitlines():
        m = pattern.match(line)
        if m:
            value = int(m.group(1), 16)
            kind = m.group(2).upper()  # SIZEOF or ALIGNOF
            type_name = m.group(3).upper()  # e.g. OVE_THREAD_STORAGE_T
            # Remove _T suffix for cleaner env var names
            type_name = re.sub(r'_T$', '', type_name)
            key = f"{kind}_{type_name}"
            sizes[key] = value

    if not sizes:
        print("warning: no storage sizes found in object file", file=sys.stderr)

    with open(out_file, 'w') as f:
        for key, value in sorted(sizes.items()):
            f.write(f"{key}={value}\n")


if __name__ == '__main__':
    main()
