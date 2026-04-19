# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""`ove lint` / `ove format` — multi-language formatter / static check.

Tools per language:
  C / C++:    clang-format
  Rust:       cargo fmt --check / cargo fmt
  Zig:        zig fmt --check / zig fmt
  Python:     ruff check (lint only — no autoformatter assumed)

Each tool is invoked only when the corresponding binary is on PATH; we
treat absence as a SKIP, not a failure. Exit code 1 if any invoked tool
reports diagnostics.
"""

import logging
import os
import shutil
import subprocess
import sys

from .workspace import find_ove_dir

logger = logging.getLogger("ove")


def _run(cmd, cwd=None):
    """Return (rc, output) running cmd; rc=127 if binary missing."""
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except FileNotFoundError:
        return 127, ""


def _glob(root, *exts):
    """Walk root and yield files matching any of *exts."""
    for d, _dirs, files in os.walk(root):
        # Skip generated / vendored trees
        parts = set(d.split(os.sep))
        if parts & {".git", "output", "dl", ".venv", "target", "build",
                    "zig-cache", ".zig-cache", "node_modules", "__pycache__"}:
            continue
        for f in files:
            if any(f.endswith(e) for e in exts):
                yield os.path.join(d, f)


def _clang_format(ove_dir, check):
    if not shutil.which("clang-format"):
        return ("clang-format", "SKIP", "not installed")
    files = list(_glob(ove_dir, ".c", ".h", ".cpp", ".hpp"))
    if not files:
        return ("clang-format", "OK", "no sources")
    cmd = ["clang-format", "--Werror"] + (
        ["-n"] if check else ["-i"]) + files
    rc, out = _run(cmd, cwd=ove_dir)
    return ("clang-format", "OK" if rc == 0 else "FAIL",
            f"{len(files)} files" if rc == 0 else out.strip()[:200])


def _cargo_fmt(ove_dir, check):
    if not shutil.which("cargo"):
        return ("cargo fmt", "SKIP", "not installed")
    # Format the binding crate plus every rust app crate found under apps/.
    crates = [os.path.join(ove_dir, "bindings", "rust", "ove")]
    apps_rust = os.path.join(ove_dir, "apps", "rust")
    if os.path.isdir(apps_rust):
        for entry in sorted(os.listdir(apps_rust)):
            cargo = os.path.join(apps_rust, entry, "Cargo.toml")
            if os.path.isfile(cargo):
                crates.append(os.path.dirname(cargo))
    crates = [c for c in crates if os.path.isfile(
        os.path.join(c, "Cargo.toml"))]
    if not crates:
        return ("cargo fmt", "SKIP", "no rust crates")
    failures = []
    for crate in crates:
        cmd = ["cargo", "fmt", "--all"] + (["--check"] if check else [])
        rc, out = _run(cmd, cwd=crate)
        if rc != 0:
            failures.append(os.path.relpath(crate, ove_dir))
    if failures:
        return ("cargo fmt", "FAIL", " ".join(failures)[:200])
    return ("cargo fmt", "OK", f"{len(crates)} crates")


def _zig_fmt(ove_dir, check):
    if not shutil.which("zig"):
        return ("zig fmt", "SKIP", "not installed")
    files = list(_glob(ove_dir, ".zig"))
    if not files:
        return ("zig fmt", "OK", "no sources")
    cmd = ["zig", "fmt"] + (["--check"] if check else []) + files
    rc, out = _run(cmd, cwd=ove_dir)
    return ("zig fmt", "OK" if rc == 0 else "FAIL",
            f"{len(files)} files" if rc == 0 else out.strip()[:200])


def _ruff(ove_dir, check):
    if not shutil.which("ruff"):
        return ("ruff", "SKIP", "not installed (pip install ruff)")
    py_root = os.path.join(ove_dir, "config", "ove-cli")
    cmd = ["ruff", "check", py_root]
    rc, out = _run(cmd, cwd=ove_dir)
    return ("ruff", "OK" if rc == 0 else "FAIL",
            "" if rc == 0 else out.strip()[:200])


def _run_all(check):
    ove_dir = find_ove_dir()
    return [
        _clang_format(ove_dir, check),
        _cargo_fmt(ove_dir, check),
        _zig_fmt(ove_dir, check),
        _ruff(ove_dir, check),
    ]


def _print(results):
    name_w = max(len(n) for n, _, _ in results)
    for name, status, detail in results:
        marker = {"OK": "\u2713", "FAIL": "\u2717", "SKIP": "-"}[status]
        print(f"  [{marker}] {name:<{name_w}}  {detail}")
    failures = [n for n, s, _ in results if s == "FAIL"]
    if failures:
        print(f"FAIL: {', '.join(failures)}")
        return 1
    return 0


def cmd_lint(args):
    """CLI entry point for 'ove lint'. Read-only check."""
    sys.exit(_print(_run_all(check=True)))


def cmd_format(args):
    """CLI entry point for 'ove format'. Rewrites files in place."""
    sys.exit(_print(_run_all(check=False)))
