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
import signal
import subprocess
import sys
import time

from .workspace import WORKSPACE_DIR_ENV, Workspace

logger = logging.getLogger("ove")


class _TerminationRequested(Exception):
    """Turn SIGTERM into normal cleanup while a child build is running."""

    def __init__(self, signum):
        super().__init__(signum)
        self.signum = signum


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


def _stop_child(proc):
    """Terminate a command and every descendant in its process group."""
    if proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        proc.wait()
        return
    try:
        proc.wait(timeout=5)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        proc.wait()
        return
    proc.wait()


def _run_command(cmd, env, cwd):
    """Run one build stage without allowing descendants to outlive it."""
    proc = subprocess.Popen(cmd, env=env, cwd=cwd, start_new_session=True)
    try:
        return proc.wait()
    except BaseException:
        _stop_child(proc)
        raise


def _run_app(ove_dir, board, rtos, app, ove_bin):
    """Configure and build one isolated <board>.<rtos>.<app> workspace."""
    spec = f"{board}.{rtos}.{app}"
    start = time.time()
    env = os.environ.copy()
    workspace_dir = os.path.join(ove_dir, "output", board, rtos, app)

    configure = [ove_bin, "defconfig-fragments", spec, "--no-activate"]
    if _run_command(configure, env, ove_dir) != 0:
        return False, time.time() - start

    env[WORKSPACE_DIR_ENV] = workspace_dir
    for command in ("download", "configure", "build"):
        if _run_command([ove_bin, command], env, ove_dir) != 0:
            return False, time.time() - start
    return True, time.time() - start


def _cmd_allconfigs(args):
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
    spec_pair = f"{board}.{rtos}"
    for i, (app, incompat) in enumerate(apps, 1):
        print(f"\n{'=' * 60}")
        # Entries are either bare board names (`host`, skip on any rtos)
        # or `<board>.<rtos>` (skip only that pair — used for board/rtos
        # combos that lack a C-side driver, e.g. STM32F7 ETH driver lives
        # in drivers/freertos so the NuttX/Zephyr combos won't link).
        skip_reason = None
        if board in incompat:
            skip_reason = board
        elif spec_pair in incompat:
            skip_reason = spec_pair
        if skip_reason is not None:
            print(f"[{i}/{total}] Skipping {board}.{rtos}.{app}"
                  f" (incompatible_boards includes {skip_reason})")
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


def cmd_allconfigs(args):
    """CLI entry point for 'ove allconfigs <board>.<rtos>'."""
    def _request_termination(signum, _frame):
        raise _TerminationRequested(signum)

    previous = signal.signal(signal.SIGTERM, _request_termination)
    try:
        _cmd_allconfigs(args)
    except _TerminationRequested as exc:
        logger.error("allconfigs interrupted")
        raise SystemExit(128 + exc.signum) from None
    except KeyboardInterrupt:
        logger.error("allconfigs interrupted")
        raise SystemExit(130) from None
    finally:
        signal.signal(signal.SIGTERM, previous)


def add_subcommand(sub):
    p = sub.add_parser("allconfigs",
                       help="Build every app for <board>.<rtos>")
    p.add_argument("spec", help="board.rtos (e.g. host.posix)")
    p.add_argument("--json", action="store_true",
                   help="Emit JSON summary in addition to text")
    return p
