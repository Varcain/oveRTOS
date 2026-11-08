# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reuse the native context observer with Queue-only feature selection."""
import argparse
import sys
from copy import deepcopy
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "rust_scoped" / "context_flags"))
from profiles import profiles

from build import build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    selection = []
    for name in ("f-arm7em-sync", "z-native-up", "n-sim-flat-up", "p-sync"):
        profile = deepcopy(next(row for row in profiles if row["profile"] == name))
        profile["profile"] = "queue-" + name
        profile["defines"].pop("CONFIG_OVE_SYNC")
        profile["defines"]["CONFIG_OVE_QUEUE"] = 1
        selection.append(profile)
    build(args.output.resolve(), args.sanitize, selected_profiles=selection)


if __name__ == "__main__":
    main()
