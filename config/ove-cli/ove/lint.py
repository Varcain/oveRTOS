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

from . import lint_backend_struct
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


def _find_compile_db(ove_dir):
    """Locate a compile_commands.json under output/ to feed clang-tidy.

    Preference order (most-to-least portable for host clang-tidy):
      1. `output/tests/stub` — host-compiled tests, always available after
         `make test-stub`. Flags are plain host GCC/clang, no cross-compile
         noise clang-tidy has to dodge.
      2. `output/tests/cpp` — host C++ tests.
      3. `output/host/posix/*` — host POSIX app builds.
      4. Fall back to the active workspace symlink `output/compile_commands.json`.
      5. Most recently modified build otherwise.

    Returns the absolute build directory containing compile_commands.json,
    or None if nothing is configured.  ARM/cross-target builds still work
    but often surface "unknown argument" diagnostics from vendor toolchain
    flags that host clang-tidy doesn't recognise — preferring a host-native
    build avoids that noise.
    """
    output_root = os.path.join(ove_dir, "output")
    if not os.path.isdir(output_root):
        return None

    preferred = (
        os.path.join("tests", "stub"),
        os.path.join("tests", "cpp"),
        os.path.join("host", "posix"),
    )

    buckets = {p: [] for p in preferred}
    other_candidates = []
    for dirpath, _dirs, files in os.walk(output_root):
        if "compile_commands.json" not in files:
            continue
        path = os.path.join(dirpath, "compile_commands.json")
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            continue
        rel = os.path.relpath(dirpath, output_root)
        matched = False
        for p in preferred:
            if rel == p or rel.startswith(p + os.sep):
                buckets[p].append((mtime, dirpath))
                matched = True
                break
        if not matched:
            other_candidates.append((mtime, dirpath))

    for p in preferred:
        if buckets[p]:
            buckets[p].sort(reverse=True)
            return buckets[p][0][1]

    # Active-workspace symlink resolves to whatever was built last.
    symlink = os.path.join(output_root, "compile_commands.json")
    if os.path.isfile(symlink):
        return output_root

    if other_candidates:
        other_candidates.sort(reverse=True)
        return other_candidates[0][1]
    return None


def _clang_tidy(ove_dir, check):
    """Run clang-tidy against sources present in an active build's
    compile_commands.json.  Static analysis only — never rewrites files.

    Scope is the intersection of
        (compile_commands.json ∪ OVE_EXTERNAL_APPS)
    with
        (bindings/cpp, apps/, overtos_apps/, OVE_EXTERNAL_APPS).
    This ensures every file clang-tidy sees has the right -I/-D flags.
    """
    import json
    del check  # clang-tidy has --fix but we deliberately don't wire it
    if not shutil.which("clang-tidy"):
        return ("clang-tidy", "SKIP", "not installed")
    build_dir = _find_compile_db(ove_dir)
    if build_dir is None:
        return ("clang-tidy", "SKIP",
                "no compile_commands.json — run 'make test-stub' or "
                "'make <board>.<rtos>.<app>' first")

    # Parse the compile DB and pick sources whose path lies under our scope.
    db_path = os.path.join(build_dir, "compile_commands.json")
    try:
        with open(db_path, encoding="utf-8") as f:
            entries = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        return ("clang-tidy", "SKIP", f"compile db read failed: {e}")

    scope_prefixes = [
        os.path.abspath(os.path.join(ove_dir, "bindings", "cpp")),
        os.path.abspath(os.path.join(ove_dir, "apps")),
        os.path.abspath(os.path.join(ove_dir, "overtos_apps")),
    ]
    ext = os.environ.get("OVE_EXTERNAL_APPS")
    if ext and os.path.isdir(ext):
        scope_prefixes.append(os.path.abspath(ext))

    files = []
    seen = set()
    for e in entries:
        src = e.get("file")
        if not src or src in seen:
            continue
        src_abs = os.path.abspath(src)
        if not any(src_abs.startswith(p + os.sep) for p in scope_prefixes):
            continue
        if any(part in ("dl", "output", "build", "target", ".venv",
                        "_deps", "zig-cache", ".zig-cache")
               for part in src_abs.split(os.sep)):
            continue
        files.append(src_abs)
        seen.add(src)

    if not files:
        return ("clang-tidy", "OK",
                "no app/binding sources in active compile db")

    cmd = ["clang-tidy", "--quiet", "-p", build_dir] + files
    rc, out = _run(cmd, cwd=ove_dir)
    return ("clang-tidy", "OK" if rc == 0 else "FAIL",
            f"{len(files)} files" if rc == 0 else out.strip()[:200])


def _cargo_clippy(ove_dir, check):
    """Run cargo clippy against Rust crates that don't require a fully
    configured workspace.

    Only the `bindings/rust/ove` crate is linted by default: the
    crate's `build.rs` needs `OVE_DIR` + `OVE_GEN_DIR` pointing at a
    minimal ove_config.h, which the `tests/` subtree already ships.

    Rust *apps* (apps/rust/*, overtos_apps/*_rust) additionally need
    `LVGL_INCLUDE_PATH`, `LVGL_PARENT_PATH`, `CMSIS_DSP_INCLUDE` etc.
    set by an active CMake workspace.  Linting those requires a prior
    `make <board>.<rtos>.<app>` and equivalent env; rather than
    duplicate the build wiring here, we skip them with a pointer.

    To lint an app crate manually after an active build:

        OVE_DIR=$(pwd) OVE_GEN_DIR=output/<ws>/generated \\
        LVGL_INCLUDE_PATH=... LVGL_PARENT_PATH=... \\
            cargo clippy --manifest-path overtos_apps/hiroic_rust/Cargo.toml
    """
    del check
    if not shutil.which("cargo"):
        return ("cargo clippy", "SKIP", "not installed")
    binding_crate = os.path.join(ove_dir, "bindings", "rust", "ove")
    if not os.path.isfile(os.path.join(binding_crate, "Cargo.toml")):
        return ("cargo clippy", "SKIP", "no binding crate")

    env = os.environ.copy()
    env.setdefault("OVE_DIR", ove_dir)
    env.setdefault("OVE_GEN_DIR", os.path.join(ove_dir, "tests"))

    cmd = [
        "cargo", "clippy", "--all-targets", "--locked", "--",
        "-D", "warnings",
        "-W", "clippy::pedantic",
        "-W", "clippy::nursery",
        "-A", "clippy::module-name-repetitions",
        "-A", "clippy::missing-errors-doc",
    ]
    try:
        r = subprocess.run(cmd, cwd=binding_crate, env=env,
                           capture_output=True, text=True)
    except FileNotFoundError:
        return ("cargo clippy", "SKIP", "cargo disappeared mid-run")
    if r.returncode != 0:
        # Surface just the first error line so the summary stays readable.
        detail = ""
        for line in (r.stdout + r.stderr).splitlines():
            if line.startswith("error:"):
                detail = line[:200]
                break
        return ("cargo clippy", "FAIL", detail or "bindings/rust/ove")
    return ("cargo clippy", "OK", "bindings/rust/ove")


def _zig_ast_check(ove_dir, check):
    """Run `zig ast-check` on every .zig source — parse + semantic check
    without codegen.  Zig has no standalone linter; this is the closest
    built-in equivalent.
    """
    del check
    if not shutil.which("zig"):
        return ("zig ast-check", "SKIP", "not installed")
    files = list(_glob(ove_dir, ".zig"))
    if not files:
        return ("zig ast-check", "OK", "no sources")
    failures = []
    for f in files:
        rc, out = _run(["zig", "ast-check", f], cwd=ove_dir)
        if rc != 0:
            failures.append((os.path.relpath(f, ove_dir), out.strip()[:120]))
    if failures:
        detail = "; ".join(f"{p}: {e}" for p, e in failures[:3])
        return ("zig ast-check", "FAIL", detail[:200])
    return ("zig ast-check", "OK", f"{len(files)} files")


def _ruff(ove_dir, check):
    if not shutil.which("ruff"):
        return ("ruff", "SKIP", "not installed (pip install ruff)")
    py_root = os.path.join(ove_dir, "config", "ove-cli")
    cmd = ["ruff", "check", py_root]
    rc, out = _run(cmd, cwd=ove_dir)
    return ("ruff", "OK" if rc == 0 else "FAIL",
            "" if rc == 0 else out.strip()[:200])


def _backend_struct_guard(ove_dir, check):
    """Forbid `struct ove_*` redefinition inside backend .c files."""
    del check  # read-only; format is n/a for this rule
    violations = lint_backend_struct.check(ove_dir)
    if not violations:
        return ("backend-struct", "OK", "no redefinitions")
    detail = lint_backend_struct.format_violations(violations)
    return ("backend-struct", "FAIL", detail)


def _run_all(check, include_lint=True):
    """Run all checks. `check=True` means read-only mode; `check=False`
    rewrites files (only the formatters honour the distinction — the
    correctness linters are always read-only).

    When `include_lint=False` only the formatters run, matching the
    existing `ove format` semantics.
    """
    ove_dir = find_ove_dir()
    results = [
        _clang_format(ove_dir, check),
        _cargo_fmt(ove_dir, check),
        _zig_fmt(ove_dir, check),
        _ruff(ove_dir, check),
    ]
    if include_lint:
        results += [
            _clang_tidy(ove_dir, check),
            _cargo_clippy(ove_dir, check),
            _zig_ast_check(ove_dir, check),
            _backend_struct_guard(ove_dir, check),
        ]
    return results


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
    """CLI entry point for 'ove lint'. Read-only correctness + format check."""
    del args
    sys.exit(_print(_run_all(check=True, include_lint=True)))


def cmd_format(args):
    """CLI entry point for 'ove format'. Rewrites files via the formatters
    only — correctness linters (clang-tidy, clippy, zig ast-check) are
    skipped here to keep the fix-in-place semantics narrow and predictable.
    """
    del args
    sys.exit(_print(_run_all(check=False, include_lint=False)))
