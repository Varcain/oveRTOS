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
    """Return sorted list of (config_name, incompatible_boards) tuples
    discovered from app.yaml files.

    `incompatible_boards` is a list of board directory names (e.g.
    `["host", "wasm"]`); empty when the app sets no such field. Used to
    skip board/rtos combos an app cannot satisfy (e.g. apps that need
    CONFIG_OVE_ASYNC on a board that has no irq backend).
    """
    import yaml
    apps_dir = os.path.join(ove_dir, "apps")
    found = {}
    for root, _dirs, files in os.walk(apps_dir):
        if "app.yaml" not in files:
            continue
        path = os.path.join(root, "app.yaml")
        with open(path) as f:
            data = yaml.safe_load(f) or {}
        name = data.get("config_name")
        if not name:
            continue
        incompat = data.get("incompatible_boards") or []
        if not isinstance(incompat, list):
            incompat = []
        found[name] = sorted(str(b) for b in incompat)
    return sorted(found.items())


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
    for i, (app, incompat) in enumerate(apps, 1):
        print(f"\n{'=' * 60}")
        if board in incompat:
            print(f"[{i}/{total}] Skipping {board}.{rtos}.{app}"
                  f" (incompatible_boards includes {board})")
            print(f"{'=' * 60}")
            results.append({"app": app, "ok": True, "skipped": True,
                            "seconds": 0.0})
            print(f"[{i}/{total}] {board}.{rtos}.{app}: SKIPPED")
            continue
        print(f"[{i}/{total}] Building {board}.{rtos}.{app}")
        print(f"{'=' * 60}")
        ok, elapsed = _run_app(ws.ove_dir, board, rtos, app, ove_bin)
        results.append({"app": app, "ok": ok, "skipped": False,
                        "seconds": round(elapsed, 1)})
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
