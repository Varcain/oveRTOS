# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and run every context metadata model; a model exits non-zero on any failed check."""

import argparse
import subprocess
from pathlib import Path

from build import build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    binaries = build(args.output.resolve(), args.sanitize)
    failed = 0
    for binary in binaries:
        completed = subprocess.run([str(binary)], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=15, check=False)
        if completed.returncode:
            print(completed.stdout.decode(errors="backslashreplace"), end="")
        print(binary.parent.name, "FAIL" if completed.returncode else "PASS", flush=True)
        failed += int(completed.returncode != 0)
    return int(failed != 0 or not binaries)


if __name__ == "__main__":
    raise SystemExit(main())
