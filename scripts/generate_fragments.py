#!/usr/bin/env python3
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Generate config/fragments/ from existing defconfigs/.

Reads all defconfig files, decomposes them into hierarchical layers
(global, board, rtos, board+rtos, app, board+rtos+app, variant),
writes fragment files, and optionally verifies that merging fragments
reproduces the original defconfig for every combination.

Usage:
    python3 scripts/generate_fragments.py              # Generate fragments
    python3 scripts/generate_fragments.py --verify     # Verify only (no write)
    python3 scripts/generate_fragments.py --dry-run    # Show what would be written
"""

import argparse
import os
import re
import sys
from collections import defaultdict
from pathlib import Path


VALID_RTOS = ("freertos", "nuttx", "zephyr", "posix")
OVE_DIR = Path(__file__).resolve().parent.parent
DEFCONFIGS_DIR = OVE_DIR / "defconfigs"
FRAGMENTS_DIR = OVE_DIR / "config" / "fragments"


def parse_defconfig(path):
    """Parse a defconfig file into an ordered list of lines.

    Returns list of strings (including comments like '# CONFIG_X is not set').
    Blank lines and pure-comment lines (not 'is not set') are skipped.
    """
    lines = []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            # Keep 'CONFIG_X=value' and '# CONFIG_X is not set' lines
            if line.startswith("CONFIG_") or line.startswith("# CONFIG_"):
                lines.append(line)
            # Keep comment lines that precede '# CONFIG_X is not set' (context)
            elif line.startswith("#") and "is not set" not in line:
                # Keep informational comments (e.g., "# QEMU has no I2S...")
                lines.append(line)
    return lines


def parse_defconfig_as_set(path):
    """Parse a defconfig into a set of config lines for set operations."""
    return set(parse_defconfig(path))


def parse_name(filename):
    """Parse defconfig filename into (board, rtos, app, zeroheap) tuple.

    Format: <board>_<rtos>_<app>[_zeroheap]_defconfig
    """
    stem = filename.replace("_defconfig", "")

    # Find RTOS
    board = rtos = app = None
    for r in VALID_RTOS:
        idx = stem.find(f"_{r}_")
        if idx >= 0:
            board = stem[:idx]
            rtos = r
            app = stem[idx + 1 + len(r) + 1:]
            break

    if not board or not rtos or not app:
        return None

    zeroheap = False
    if app.endswith("_zeroheap"):
        zeroheap = True
        app = app[:-len("_zeroheap")]

    return board, rtos, app, zeroheap


def load_all_defconfigs():
    """Load all defconfig files, return list of (board, rtos, app, zeroheap, lines_set, path)."""
    results = []
    for path in sorted(DEFCONFIGS_DIR.rglob("*_defconfig")):
        parsed = parse_name(path.name)
        if not parsed:
            print(f"WARNING: cannot parse {path.name}, skipping")
            continue
        board, rtos, app, zeroheap = parsed
        lines = parse_defconfig_as_set(path)
        results.append((board, rtos, app, zeroheap, lines, path))
    return results


def _get_kconfig_defaults(ove_dir):
    """Parse Config.in files to extract default values for int/string/hex configs."""
    defaults = {}
    config_files = [
        ove_dir / "config" / "Config.in.rtos",
        ove_dir / "config" / "Config.in.hardware",
        ove_dir / "config" / "Config.in.modules",
        ove_dir / "config" / "Config.in.toolchain",
    ]
    for cfg_file in config_files:
        if not cfg_file.is_file():
            continue
        current_key = None
        with open(cfg_file) as f:
            for line in f:
                line = line.strip()
                m = re.match(r'config\s+(\w+)', line)
                if m:
                    current_key = f"CONFIG_{m.group(1)}"
                elif current_key and line.startswith("default "):
                    # Extract simple defaults (no conditionals)
                    val = line.split("default ", 1)[1].strip()
                    # Stop at 'if' clause
                    if " if " in val:
                        val = val.split(" if ")[0].strip()
                    defaults[current_key] = val
                    current_key = None
    return defaults


def extract_fragments(all_configs):
    """Decompose defconfigs into hierarchical fragment layers.

    Returns dict of fragment_path -> set of config lines.
    """
    fragments = {}

    # Group configs by various dimensions (exclude zeroheap variants for base extraction)
    base_configs = [(b, r, a, lines) for b, r, a, zh, lines, _ in all_configs if not zh]

    if not base_configs:
        print("ERROR: No base (non-zeroheap) defconfigs found")
        sys.exit(1)

    # 1. Global: intersection of ALL base defconfigs
    all_sets = [lines for _, _, _, lines in base_configs]
    global_lines = set.intersection(*all_sets) if all_sets else set()
    fragments["global.defconfig"] = global_lines

    # Identify RTOS-selection and board-selection configs to force into correct layers.
    # These patterns MUST go to their respective layers regardless of set math.
    rtos_reserved = set()
    board_reserved = set()
    for line in set().union(*all_sets):
        if re.match(r'CONFIG_OVE_RTOS_\w+=y', line):
            rtos_reserved.add(line)
        elif re.match(r'CONFIG_OVE_BOARD_\w+=y', line):
            board_reserved.add(line)

    # 2. Board fragments: intersection of all configs for each board, minus global
    #    Exclude RTOS-selection configs (they belong in rtos layer).
    boards = sorted(set(b for b, _, _, _ in base_configs))
    for board in boards:
        board_sets = [lines for b, _, _, lines in base_configs if b == board]
        if board_sets:
            board_common = set.intersection(*board_sets) - global_lines - rtos_reserved
            if board_common:
                fragments[f"board/{board}.defconfig"] = board_common

    # 3. RTOS fragments: intersection of all configs for each RTOS, minus global,
    #    minus all board fragments. Exclude board-selection configs.
    all_board_lines = set()
    for k, v in fragments.items():
        if k.startswith("board/"):
            all_board_lines |= v

    rtoses = sorted(set(r for _, r, _, _ in base_configs))
    for rtos in rtoses:
        rtos_sets = [lines for _, r, _, lines in base_configs if r == rtos]
        if rtos_sets:
            rtos_common = (set.intersection(*rtos_sets)
                           - global_lines - all_board_lines - board_reserved)
            if rtos_common:
                fragments[f"rtos/{rtos}.defconfig"] = rtos_common

    # 4. Board+RTOS fragments: intersection of all configs for each (board,rtos) combo,
    #    minus global, board, rtos
    all_rtos_lines = set()
    for k, v in fragments.items():
        if k.startswith("rtos/"):
            all_rtos_lines |= v

    board_rtos_combos = sorted(set((b, r) for b, r, _, _ in base_configs))
    for board, rtos in board_rtos_combos:
        combo_sets = [lines for b, r, _, lines in base_configs if b == board and r == rtos]
        if combo_sets:
            board_frag = fragments.get(f"board/{board}.defconfig", set())
            rtos_frag = fragments.get(f"rtos/{rtos}.defconfig", set())
            combo_common = set.intersection(*combo_sets) - global_lines - board_frag - rtos_frag
            if combo_common:
                fragments[f"board+rtos/{board}+{rtos}.defconfig"] = combo_common

    # 5. App fragments: intersection of all configs for each app (across all boards/RTOSes),
    #    minus all higher layers
    higher = set()
    for k, v in fragments.items():
        higher |= v

    apps = sorted(set(a for _, _, a, _ in base_configs))
    for app in apps:
        app_sets = [lines for _, _, a, lines in base_configs if a == app]
        if app_sets:
            app_common = set.intersection(*app_sets) - higher
            if app_common:
                fragments[f"app/{app}.defconfig"] = app_common

    # 6. Board+RTOS+App overrides: remainder for each specific combination
    all_app_lines = set()
    for k, v in fragments.items():
        if k.startswith("app/"):
            all_app_lines |= v

    for board, rtos, app, lines in base_configs:
        # Reconstruct what the merged config would be from higher layers
        merged = set()
        merged |= global_lines
        merged |= fragments.get(f"board/{board}.defconfig", set())
        merged |= fragments.get(f"rtos/{rtos}.defconfig", set())
        merged |= fragments.get(f"board+rtos/{board}+{rtos}.defconfig", set())
        merged |= fragments.get(f"app/{app}.defconfig", set())

        remainder = lines - merged
        if remainder:
            fragments[f"board+rtos+app/{board}+{rtos}+{app}.defconfig"] = remainder

    # 7. Zeroheap variant fragment (just CONFIG_OVE_ZERO_HEAP=y)
    fragments["variant/zeroheap.defconfig"] = {"CONFIG_OVE_ZERO_HEAP=y"}

    # 8. Zeroheap-specific overrides for combos where zeroheap differs from base
    #    beyond just adding ZERO_HEAP=y.
    #    We use the actual zeroheap defconfig values to get correct overrides,
    #    including for int/string configs where '# is not set' doesn't work.
    kconfig_defaults = _get_kconfig_defaults(OVE_DIR)

    for board, rtos, app, zeroheap, zh_lines, path in all_configs:
        if not zeroheap:
            continue
        base_match = [lines for b, r, a, zh, lines, _ in all_configs
                      if b == board and r == rtos and a == app and not zh]
        if not base_match:
            continue
        base_lines = base_match[0]
        zh_extra = zh_lines - base_lines - {"CONFIG_OVE_ZERO_HEAP=y"}
        zh_removed = base_lines - zh_lines
        if zh_extra or zh_removed:
            override_lines = set()
            override_lines |= zh_extra
            # For removed configs: use '# is not set' for booleans,
            # explicit default values for int/string/hex.
            for line in zh_removed:
                if line.startswith("CONFIG_"):
                    key = line.split("=")[0]
                    val = line.split("=", 1)[1] if "=" in line else ""
                    if val == "y":
                        override_lines.add(f"# {key} is not set")
                    else:
                        # Int/string/hex: use the Kconfig default
                        default = kconfig_defaults.get(key)
                        if default is not None:
                            override_lines.add(f"{key}={default}")
                        else:
                            override_lines.add(f"# {key} is not set")
            if override_lines:
                fragments[f"override/{board}+{rtos}+{app}+zeroheap.defconfig"] = override_lines

    return fragments


def format_fragment(lines_set):
    """Format a set of config lines into a readable fragment file.

    Groups: comments first, then CONFIG_ lines sorted.
    """
    comments = sorted(l for l in lines_set if l.startswith("#"))
    configs = sorted(l for l in lines_set if l.startswith("CONFIG_"))
    not_set = sorted(l for l in lines_set
                     if l.startswith("# CONFIG_") and "is not set" in l)

    # Remove 'not set' from comments (they were caught by both filters)
    comments = [c for c in comments if "is not set" not in c]

    parts = []
    if comments:
        parts.extend(comments)
    if configs:
        if parts:
            parts.append("")
        parts.extend(configs)
    if not_set:
        if parts:
            parts.append("")
        parts.extend(not_set)

    return "\n".join(parts) + "\n"


def write_fragments(fragments, dry_run=False):
    """Write fragment files to config/fragments/."""
    for frag_path, lines_set in sorted(fragments.items()):
        full_path = FRAGMENTS_DIR / frag_path
        content = format_fragment(lines_set)

        if dry_run:
            print(f"\n--- {frag_path} ---")
            print(content)
            continue

        full_path.parent.mkdir(parents=True, exist_ok=True)
        with open(full_path, "w") as f:
            f.write(content)
        print(f"  Written: config/fragments/{frag_path} ({len(lines_set)} lines)")


def _apply_override(merged, override):
    """Apply override fragment with last-writer-wins for same CONFIG_ keys."""
    result = set(merged)
    for line in override:
        if line.startswith("CONFIG_"):
            key = line.split("=")[0]
            result = {l for l in result if not l.startswith(key + "=")}
            result.add(line)
        elif line.startswith("# CONFIG_") and "is not set" in line:
            key = line.split("# ")[1].split(" is not set")[0]
            result = {l for l in result if not l.startswith(key + "=")}
        else:
            result.add(line)
    return result


def verify_fragments(all_configs, fragments):
    """Verify that merging fragments reproduces each original defconfig."""
    errors = 0
    checked = 0

    for board, rtos, app, zeroheap, original_lines, path in all_configs:
        # Reconstruct by merging fragments in order (simple set union)
        merged = set()
        merged |= fragments.get("global.defconfig", set())
        merged |= fragments.get(f"board/{board}.defconfig", set())
        merged |= fragments.get(f"rtos/{rtos}.defconfig", set())
        merged |= fragments.get(f"board+rtos/{board}+{rtos}.defconfig", set())
        merged |= fragments.get(f"app/{app}.defconfig", set())
        merged |= fragments.get(f"board+rtos+app/{board}+{rtos}+{app}.defconfig", set())
        if zeroheap:
            merged |= fragments.get("variant/zeroheap.defconfig", set())
            # Apply override with key-level replacement
            override = fragments.get(
                f"override/{board}+{rtos}+{app}+zeroheap.defconfig", set())
            if override:
                merged = _apply_override(merged, override)

        if merged != original_lines:
            missing = original_lines - merged
            extra = merged - original_lines
            # Filter out "extra" lines that are explicit defaults matching
            # Kconfig defaults — these are semantically equivalent to absent.
            kconfig_defaults = _get_kconfig_defaults(OVE_DIR)
            real_extra = set()
            for line in extra:
                if line.startswith("CONFIG_") and "=" in line:
                    key = line.split("=")[0]
                    val = line.split("=", 1)[1]
                    default = kconfig_defaults.get(key)
                    if default is not None and str(default) == str(val):
                        continue  # Explicit default, OK
                real_extra.add(line)
            if missing or real_extra:
                print(f"  MISMATCH: {path.name}")
                if missing:
                    print(f"    Missing from fragments: {missing}")
                if real_extra:
                    print(f"    Extra in fragments:     {real_extra}")
                errors += 1
        checked += 1

    print(f"\nVerification: {checked} defconfigs checked, {errors} mismatches")
    return errors == 0


def main():
    parser = argparse.ArgumentParser(description="Generate config fragments from defconfigs")
    parser.add_argument("--verify", action="store_true",
                        help="Verify fragments match defconfigs (no write)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show what would be written without writing")
    args = parser.parse_args()

    print("Loading defconfigs...")
    all_configs = load_all_defconfigs()
    print(f"  Found {len(all_configs)} defconfig files")

    print("\nExtracting fragments...")
    fragments = extract_fragments(all_configs)
    print(f"  Extracted {len(fragments)} fragments")

    if args.verify:
        ok = verify_fragments(all_configs, fragments)
        sys.exit(0 if ok else 1)

    print("\nWriting fragments...")
    write_fragments(fragments, dry_run=args.dry_run)

    print("\nVerifying...")
    ok = verify_fragments(all_configs, fragments)

    if ok:
        print("\nAll defconfigs verified successfully!")
    else:
        print("\nWARNING: Some defconfigs do not match fragments!")
        sys.exit(1)


if __name__ == "__main__":
    main()
