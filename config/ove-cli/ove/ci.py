# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""`ove ci` — pre-merge umbrella.

Runs the standard pre-merge gates in order, stopping on the first
failure unless ``--keep-going`` is passed:

  1. doctor   — host environment check (required tools present)
  2. lint     — clang-format / cargo fmt / zig fmt / ruff
  3. test all — sim + qemu test suites
"""

import logging
import shutil
import subprocess
import sys

logger = logging.getLogger("ove")


_STAGES = [
    ("doctor", ["doctor"]),
    ("lint",   ["lint"]),
    ("test",   ["test", "all"]),
]


def cmd_ci(args):
    """CLI entry point for 'ove ci'."""
    ove = shutil.which("ove") or sys.argv[0]
    failed = []
    for label, argv in _STAGES:
        print(f"\n==> ove {' '.join(argv)}")
        rc = subprocess.call([ove] + argv)
        if rc != 0:
            failed.append(label)
            if not args.keep_going:
                print(f"\nci: {label} FAILED — stopping (use --keep-going "
                      "to run remaining stages)")
                sys.exit(1)

    if failed:
        print(f"\nci: FAILED stages: {', '.join(failed)}")
        sys.exit(1)
    print("\nci: all stages passed")
