# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Version manifest loader and integrity checker.

All external component versions are defined in manifest.yaml at the project
root.  This module loads the manifest, provides accessors, and checks whether
the working-tree copy has been modified relative to the last commit.
"""

import logging
import os
import subprocess
import sys

import yaml

logger = logging.getLogger("ove")

_MANIFEST_FILE = "manifest.yaml"
_manifest_cache = {}


def load_manifest(ove_dir):
    """Load and parse manifest.yaml from *ove_dir*.

    Results are cached per *ove_dir* so repeated calls are cheap.
    Returns the parsed dict.  Raises ``FileNotFoundError`` when the file is
    missing and ``SystemExit`` on invalid YAML.
    """
    ove_dir = os.path.abspath(ove_dir)
    if ove_dir in _manifest_cache:
        return _manifest_cache[ove_dir]

    path = os.path.join(ove_dir, _MANIFEST_FILE)
    with open(path) as fh:
        try:
            data = yaml.safe_load(fh)
        except yaml.YAMLError as exc:
            logger.error(f"{_MANIFEST_FILE} is not valid YAML: {exc}")
            sys.exit(1)

    _manifest_cache[ove_dir] = data
    return data


def get_component(manifest, *path, default=None):
    """Traverse *manifest* dict by key *path*.

    >>> m = {"rtos": {"nuttx": {"kernel": {"version": "12.12.0"}}}}
    >>> get_component(m, "rtos", "nuttx", "kernel", "version")
    '12.12.0'
    """
    node = manifest
    for key in path:
        if not isinstance(node, dict):
            return default
        node = node.get(key)
        if node is None:
            return default
    return node


def _diff_dicts(old, new, prefix=""):
    """Recursively compare two dicts, returning dotted paths that differ."""
    changes = []
    all_keys = set()
    if isinstance(old, dict):
        all_keys |= old.keys()
    if isinstance(new, dict):
        all_keys |= new.keys()

    for key in sorted(all_keys):
        full = f"{prefix}.{key}" if prefix else key
        old_val = old.get(key) if isinstance(old, dict) else None
        new_val = new.get(key) if isinstance(new, dict) else None

        if isinstance(old_val, dict) or isinstance(new_val, dict):
            changes.extend(_diff_dicts(
                old_val if isinstance(old_val, dict) else {},
                new_val if isinstance(new_val, dict) else {},
                full,
            ))
        elif old_val != new_val:
            changes.append(full)

    return changes


def check_manifest_integrity(ove_dir):
    """Compare working-tree manifest against the last commit.

    Returns:
        ``None``  – git is unavailable or *ove_dir* is not a git repo.
        ``[]``    – manifest is clean (matches committed version).
        ``[str]`` – list of dotted paths that differ.
    """
    manifest_path = os.path.join(ove_dir, _MANIFEST_FILE)
    if not os.path.isfile(manifest_path):
        return None

    try:
        ret = subprocess.run(
            ["git", "show", f"HEAD:{_MANIFEST_FILE}"],
            capture_output=True, text=True, cwd=ove_dir,
        )
    except FileNotFoundError:
        return None

    if ret.returncode != 0:
        return None

    try:
        committed = yaml.safe_load(ret.stdout)
    except yaml.YAMLError:
        return None

    try:
        with open(manifest_path) as fh:
            working = yaml.safe_load(fh)
    except (OSError, yaml.YAMLError):
        return None

    return _diff_dicts(committed or {}, working or {})


def warn_if_dirty(ove_dir):
    """Print a warning banner if manifest.yaml has uncommitted changes."""
    changes = check_manifest_integrity(ove_dir)

    if changes is None:
        logger.debug("manifest integrity check skipped (not a git repository)")
        return

    if not changes:
        return

    sep = "=" * 60
    logger.warning(sep)
    logger.warning("manifest.yaml has uncommitted changes!")
    logger.warning("The following component versions differ from the")
    logger.warning("committed (tested/supported) manifest:")
    for c in changes:
        logger.warning(f"  - {c}")
    logger.warning("")
    logger.warning("These versions are UNTESTED and UNSUPPORTED.")
    logger.warning("Commit manifest.yaml to bless these version changes.")
    logger.warning(sep)


def cmd_manifest(args):
    """CLI entry point for 'ove manifest'."""
    from .workspace import find_ove_dir

    ove_dir = find_ove_dir()
    manifest = load_manifest(ove_dir)

    print("oveRTOS Component Manifest")
    print("=" * 40)

    for category in ("toolchains", "rtos", "libraries"):
        data = manifest.get(category)
        if not data:
            continue
        print(f"\n{category}:")
        _print_tree(data, indent=2)

    changes = check_manifest_integrity(ove_dir)
    print()
    if changes is None:
        print("Integrity: skipped (not a git repository)")
    elif not changes:
        print("Integrity: clean (committed)")
    else:
        print(f"Integrity: DIRTY ({len(changes)} uncommitted change(s))")
        for c in changes:
            print(f"  - {c}")

    if getattr(args, "check", False) and changes:
        sys.exit(1)


def _print_tree(data, indent=0):
    """Pretty-print a nested dict as an indented tree."""
    prefix = " " * indent
    for key, val in data.items():
        if isinstance(val, dict):
            print(f"{prefix}{key}:")
            _print_tree(val, indent + 2)
        else:
            print(f"{prefix}{key}: {val}")
