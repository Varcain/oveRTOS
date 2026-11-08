# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reuse the native context observer with EventGroup feature selection."""
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
        original = next(row for row in profiles if row["profile"] == name)
        for zero in (False, True):
            for kind in ("only", "time", "off"):
                profile = deepcopy(original)
                profile["profile"] = ("eventgroup-" + name + "-" + kind
                                      + ("-zero" if zero else "-heap"))
                profile["defines"].pop("CONFIG_OVE_SYNC")
                if zero:
                    profile["defines"]["CONFIG_OVE_ZERO_HEAP"] = 1
                if kind == "off":
                    profile["config_fallback"] = True
                    profile["provider"] = "header_only"
                    for case in profile["cases"]:
                        case["expected_flags"] = 0
                        case["expected_resource_blocking"] = 0
                else:
                    profile["defines"]["CONFIG_OVE_EVENTGROUP"] = 1
                    if kind == "time":
                        profile["defines"]["CONFIG_OVE_TIME"] = 1
                selection.append(profile)
    build(args.output.resolve(), args.sanitize, selected_profiles=selection)


if __name__ == "__main__":
    main()
