#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Query a value from manifest.yaml by dotted path.

Replaces the inline ``python3 -c "import yaml; ..."`` patterns scattered
across .github/workflows/.  Those inline forms shell-interpolate values
into Python source (``b'${REV}'``) — a latent syntax-error footgun if
the manifest ever grows a value containing a quote.  This helper reads
the value directly from YAML, never interpolating into source text.

Usage:
    scripts/manifest_query.py PATH            # print the raw value
    scripts/manifest_query.py --hash PATH     # print sha256(value)[:8]
    scripts/manifest_query.py --help

Examples:
    $ scripts/manifest_query.py toolchains.zephyr-sdk.version
    0.17.0
    $ scripts/manifest_query.py --hash rtos.zephyr.version
    a1b2c3d4

Exit code 1 with a clear stderr message if the path does not resolve.

The dotted-path grammar splits on '.' only — manifest keys may safely
contain hyphens (e.g. ``toolchains.zephyr-sdk.version``) but not dots.
"""

import argparse
import hashlib
import sys
from pathlib import Path

import yaml


def resolve(manifest: dict, dotted_path: str):
    """Walk *manifest* by splitting *dotted_path* on '.'.

    Returns the resolved leaf value.  Raises KeyError with the full
    failing path on missing keys so the caller can produce a useful
    diagnostic.
    """
    node = manifest
    walked = []
    for key in dotted_path.split("."):
        walked.append(key)
        if not isinstance(node, dict) or key not in node:
            raise KeyError(".".join(walked))
        node = node[key]
    return node


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Query a value from manifest.yaml by dotted path.",
    )
    parser.add_argument(
        "path",
        help='Dotted path into manifest.yaml (e.g. "rtos.zephyr.version").',
    )
    parser.add_argument(
        "--hash",
        action="store_true",
        help="Print the first 8 hex chars of sha256(value) instead of "
             "the raw value.  Matches the cache-key convention used in "
             "the GitHub Actions workflows.",
    )
    parser.add_argument(
        "--manifest",
        default="manifest.yaml",
        help="Path to the manifest file (default: manifest.yaml in CWD).",
    )
    args = parser.parse_args()

    manifest_path = Path(args.manifest)
    if not manifest_path.is_file():
        print(
            f"manifest_query: manifest not found: {manifest_path}",
            file=sys.stderr,
        )
        return 1

    try:
        manifest = yaml.safe_load(manifest_path.read_text())
    except yaml.YAMLError as exc:
        print(f"manifest_query: invalid YAML in {manifest_path}: {exc}",
              file=sys.stderr)
        return 1

    try:
        value = resolve(manifest, args.path)
    except KeyError as failing_path:
        print(
            f"manifest_query: path not found in {manifest_path}: "
            f"{failing_path.args[0]}",
            file=sys.stderr,
        )
        return 1

    if args.hash:
        text = str(value).encode("utf-8")
        print(hashlib.sha256(text).hexdigest()[:8])
    else:
        print(value)
    return 0


if __name__ == "__main__":
    sys.exit(main())
