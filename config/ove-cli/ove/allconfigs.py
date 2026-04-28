# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Build every app config for a given <board>.<rtos> pair.

Replaces the inline shell loop that used to live in `allconfigs-%` Makefile
rule. Discovers app names from `config_name:` entries in app.yaml files,
then runs the standard configure → download → build pipeline for each.
"""

import json
import logging
import os
import re
import subprocess
import sys
import time

from .workspace import Workspace

logger = logging.getLogger("ove")

_CONFIG_NAME_RE = re.compile(r"^\s*config_name\s*:\s*(\S+)", re.MULTILINE)


def _discover_apps(ove_dir):
    """Return sorted unique list of `config_name` values from app.yaml files."""
    apps_dir = os.path.join(ove_dir, "apps")
    names = set()
    for root, _dirs, files in os.walk(apps_dir):
        if "app.yaml" in files:
            with open(os.path.join(root, "app.yaml")) as f:
                for m in _CONFIG_NAME_RE.finditer(f.read()):
                    names.add(m.group(1))
    return sorted(names)


def _run_app(ove_dir, board, rtos, app, ove_bin):
    """Configure and build one <board>.<rtos>.<app>. Returns (ok, elapsed)."""
    spec = f"{board}.{rtos}.{app}"
    start = time.time()
    env = os.environ.copy()
    cmds = [
        [ove_bin, "defconfig-fragments", spec],
        [ove_bin, "download"],
        [ove_bin, "configure"],
        [ove_bin, "build"],
    ]
    for cmd in cmds:
        if subprocess.run(cmd, env=env).returncode != 0:
            return False, time.time() - start
    return True, time.time() - start


def cmd_allconfigs(args):
    """CLI entry point for 'ove allconfigs <board>.<rtos>'."""
    spec = args.spec
    if "." not in spec:
        logger.error("usage: ove allconfigs <board>.<rtos>")
        sys.exit(2)
    board, rtos = spec.split(".", 1)
    if not board or not rtos:
        logger.error("usage: ove allconfigs <board>.<rtos>")
        sys.exit(2)

    ws = Workspace()
    apps = _discover_apps(ws.ove_dir)
    if not apps:
        logger.error("no app.yaml files found under apps/")
        sys.exit(1)

    ove_bin = os.path.join(ws.venv_dir, "bin", "ove")
    if not os.path.isfile(ove_bin):
        ove_bin = "ove"

    results = []
    total = len(apps)
    for i, app in enumerate(apps, 1):
        print(f"\n{'=' * 60}")
        print(f"[{i}/{total}] Building {board}.{rtos}.{app}")
        print(f"{'=' * 60}")
        ok, elapsed = _run_app(ws.ove_dir, board, rtos, app, ove_bin)
        results.append({"app": app, "ok": ok, "seconds": round(elapsed, 1)})
        status = "OK" if ok else "FAILED"
        print(f"[{i}/{total}] {board}.{rtos}.{app}: {status} ({elapsed:.1f}s)")

    failed = [r["app"] for r in results if not r["ok"]]
    print(f"\n{'=' * 60}")
    print(f"allconfigs-{board}.{rtos}: {total} configurations processed")
    if args.json:
        json.dump({"board": board, "rtos": rtos, "results": results,
                   "failed": failed}, sys.stdout, indent=2)
        print()
    if failed:
        print(f"FAILED: {' '.join(failed)}")
        sys.exit(1)
    print("All configurations built successfully")


def add_subcommand(sub):
    p = sub.add_parser("allconfigs",
                       help="Build every app for <board>.<rtos>")
    p.add_argument("spec", help="board.rtos (e.g. host.posix)")
    p.add_argument("--json", action="store_true",
                   help="Emit JSON summary in addition to text")
    return p
