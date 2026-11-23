# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""`ove lint` / `ove format` — multi-language formatter / static check.

Tools per language:
  C / C++:    clang-format, clang-tidy
  Rust:       cargo fmt --check / cargo fmt
  Zig:        zig fmt --check / zig fmt
  Python:     ruff check (lint only — no autoformatter assumed)

Missing tools normally skip their checks. The host clang-tidy check
requires its tools in CI. Exit code 1 indicates a failed check.
"""

import logging
import os
import shutil
import subprocess
import sys

from . import lint_backend_struct
from . import lint_doc_snippets
from . import lint_version
from .lint_backend_commands import BackendCommands, source_path
from .utils import CARGO_CLIPPY_COMMAND
from .workspace import find_ove_dir

logger = logging.getLogger("ove")


def _run(cmd, cwd=None):
    """Return (rc, output) running cmd; rc=127 if binary missing."""
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except FileNotFoundError:
        return 127, ""


def _git_ignored(root):
    """Paths under root that git ignores (.gitignore, .git/info/exclude),
    or an empty set when root is not a git checkout."""
    try:
        out = subprocess.run(
            ["git", "-C", root, "ls-files", "--others", "--ignored",
             "--exclude-standard", "--directory"],
            capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return set()
    return {os.path.normpath(os.path.join(root, p)) for p in out.splitlines()}


def _glob(root, *exts):
    """Walk root and yield files matching any of *exts, skipping what git
    ignores."""
    # Generated / vendored trees, and modules/ (git submodules such as lxp,
    # which follow their own project's style and which CI's lint job does not
    # check out).
    skip = {".git", "output", "dl", ".venv", "target", "build", "zig-cache",
            ".zig-cache", "node_modules", "__pycache__", "modules"}
    ignored = _git_ignored(root)
    for d, dirs, files in os.walk(root):
        # Prune in place so os.walk never descends into skipped trees.
        dirs[:] = [n for n in dirs if n not in skip and
                   os.path.normpath(os.path.join(d, n)) not in ignored]
        for f in files:
            path = os.path.join(d, f)
            if any(f.endswith(e) for e in exts) and \
                    os.path.normpath(path) not in ignored:
                yield path


def _external_app_roots():
    """Return absolute paths advertised via the OVE_EXTERNAL_APPS env var.

    Supports a single path or PATH-style `os.pathsep`-separated list so a
    sibling group of external apps can be linted together (e.g. when CI
    walks `overtos_apps/{hiroic, hiroic_cpp, hiroic_rust, hiroic_zig}`).
    Empty entries, nonexistent paths, and duplicates are filtered out so
    callers can blindly extend `_glob()` results without further checks.

    The variable is set by `config/make/ove_app.mk` when an external
    app's Makefile delegates `make lint` / `make format` to `ove`.
    """
    raw = os.environ.get("OVE_EXTERNAL_APPS", "")
    seen = set()
    roots = []
    for p in raw.split(os.pathsep):
        p = p.strip()
        if not p:
            continue
        ap = os.path.abspath(p)
        if ap in seen or not os.path.isdir(ap):
            continue
        seen.add(ap)
        roots.append(ap)
    return roots


def _clang_format(ove_dir, check):
    if not shutil.which("clang-format"):
        return ("clang-format", "SKIP", "not installed")
    files = list(_glob(ove_dir, ".c", ".h", ".cpp", ".hpp"))
    # External apps advertised via OVE_EXTERNAL_APPS are formatted with
    # the same .clang-format from ove_dir (clang-format walks up from
    # each input file to find the nearest config; rooted at ove_dir's
    # dotfile when the external tree has none, which is the convention).
    for ext_root in _external_app_roots():
        files.extend(_glob(ext_root, ".c", ".h", ".cpp", ".hpp"))
    # Doxyfile.cpp is a doxygen config (key=value), not C++ source — skip
    # it despite the .cpp suffix so clang-format doesn't try to reformat
    # it as code.
    files = [f for f in files
             if os.path.basename(f) not in ("Doxyfile.cpp",)]
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
    # Format the binding crate plus every rust app crate found under
    # apps/rust/ and the rust benchmark crate under tests/benchmarks/.
    crates = [os.path.join(ove_dir, "bindings", "rust", "ove")]
    rust_crate_roots = [
        os.path.join(ove_dir, "apps", "rust"),
        os.path.join(ove_dir, "tests", "benchmarks"),
    ]
    for root in rust_crate_roots:
        if not os.path.isdir(root):
            continue
        for entry in sorted(os.listdir(root)):
            cargo = os.path.join(root, entry, "Cargo.toml")
            if os.path.isfile(cargo):
                crates.append(os.path.dirname(cargo))
    # External apps: each OVE_EXTERNAL_APPS entry may itself be a Rust
    # crate (single-app case) or contain Rust crates as immediate
    # subdirectories (sibling-group case, e.g. overtos_apps/hiroic_rust).
    for ext_root in _external_app_roots():
        if os.path.isfile(os.path.join(ext_root, "Cargo.toml")):
            crates.append(ext_root)
        try:
            entries = sorted(os.listdir(ext_root))
        except OSError:
            entries = []
        for entry in entries:
            sub = os.path.join(ext_root, entry, "Cargo.toml")
            if os.path.isfile(sub):
                crates.append(os.path.dirname(sub))
    # De-duplicate while preserving order (a path could be reached via
    # both the in-tree and external-app code paths in unusual layouts).
    _seen = set()
    crates = [c for c in crates
              if not (c in _seen or _seen.add(c))]
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


def _find_zig(ove_dir):
    """Resolve the zig binary path.

    Prefers the in-tree toolchain at output/toolchains/zig-*-linux-*/zig
    (downloaded by `ove download` per manifest.yaml), so lint matches the
    same compiler the build uses. Falls back to PATH so contributors with
    a system zig don't have to bootstrap the workspace just to lint.
    """
    toolchains = os.path.join(ove_dir, "output", "toolchains")
    if os.path.isdir(toolchains):
        for name in sorted(os.listdir(toolchains)):
            if name.startswith("zig-"):
                p = os.path.join(toolchains, name, "zig")
                if os.path.isfile(p) and os.access(p, os.X_OK):
                    return p
    return shutil.which("zig")


def _zig_fmt(ove_dir, check):
    zig = _find_zig(ove_dir)
    if not zig:
        return ("zig fmt", "SKIP", "not installed (run `ove download` first)")
    files = list(_glob(ove_dir, ".zig"))
    for ext_root in _external_app_roots():
        files.extend(_glob(ext_root, ".zig"))
    if not files:
        return ("zig fmt", "OK", "no sources")
    cmd = [zig, "fmt"] + (["--check"] if check else []) + files
    rc, out = _run(cmd, cwd=ove_dir)
    return ("zig fmt", "OK" if rc == 0 else "FAIL",
            f"{len(files)} files" if rc == 0 else out.strip()[:200])


def _clang_tidy_missing_tool(tool):
    status = "FAIL" if os.environ.get("CI") else "SKIP"
    return ("clang-tidy", status, f"{tool} not installed")


def _clang_tidy(ove_dir, check):
    """Analyze in-scope C/C++ sources from the host stub compile database."""
    import json

    del check
    if not shutil.which("clang-tidy"):
        return _clang_tidy_missing_tool("clang-tidy")

    # Configure the host stub build on bare checkouts.
    stub_build = os.path.join(ove_dir, "output", "tests", "stub")
    stub_db = os.path.join(stub_build, "compile_commands.json")
    if not os.path.isfile(stub_db):
        cmake = shutil.which("cmake")
        if not cmake:
            return _clang_tidy_missing_tool("cmake")
        os.makedirs(stub_build, exist_ok=True)
        rc, out = _run([cmake, "-S", os.path.join(ove_dir, "tests"),
                        "-B", stub_build, "-DCMAKE_BUILD_TYPE=Debug"],
                       cwd=ove_dir)
        if rc != 0:
            if out.strip():
                logger.error("%s", out)
            detail = out.strip()[-200:] or f"exit status {rc}"
            return ("clang-tidy", "FAIL", f"stub CMake configure failed ({detail})")

    try:
        with open(stub_db, encoding="utf-8") as f:
            entries = json.load(f)
    except (OSError, ValueError) as e:
        return ("clang-tidy", "FAIL", f"stub compile database read failed ({e})")
    if not isinstance(entries, list):
        return ("clang-tidy", "FAIL", "stub compile database must be an array")

    # Limit analysis to project sources in the stub build.
    scope_prefixes = [
        os.path.abspath(os.path.join(ove_dir, "src")),
        os.path.abspath(os.path.join(ove_dir, "include")),
        os.path.abspath(os.path.join(ove_dir, "backends")),
        # The host test backend the stub/C++ suites link.
        os.path.abspath(os.path.join(ove_dir, "tests", "host", "posix")),
        os.path.abspath(os.path.join(ove_dir, "bindings", "cpp")),
        os.path.abspath(os.path.join(ove_dir, "apps")),
        os.path.abspath(os.path.join(ove_dir, "overtos_apps")),
    ]
    ext = os.environ.get("OVE_EXTERNAL_APPS")
    if ext and os.path.isdir(ext):
        scope_prefixes.append(os.path.abspath(ext))

    files = []
    seen = set()
    for entry in entries:
        src = entry.get("file") if isinstance(entry, dict) else None
        if not isinstance(src, str) or not src or src in seen:
            continue
        src_abs = os.path.abspath(src)
        if not any(src_abs.startswith(p + os.sep) for p in scope_prefixes):
            continue
        if any(part in ("dl", "output", "build", "target", ".venv",
                        "_deps", "zig-cache", ".zig-cache")
               for part in os.path.relpath(src_abs, ove_dir).split(os.sep)):
            continue
        files.append(src_abs)
        seen.add(src)

    if not files:
        return ("clang-tidy", "FAIL", "no in-scope sources in stub compile database")

    # Match backend analysis: Clang rejects the zero-heap allocator's
    # error-attribute redeclarations. Analysis never emits a binary.
    cmd = ["clang-tidy", "--quiet", "-p", stub_build,
           "--extra-arg=-D__OVE_LINT__"] + files
    rc, out = _run(cmd, cwd=ove_dir)
    return ("clang-tidy", "OK" if rc == 0 else "FAIL",
            f"{len(files)} files" if rc == 0 else out.strip()[:200])


def _clang_tidy_backends(ove_dir, check):
    """Run clang-tidy against each RTOS backend's C code via cross-compile
    compile_commands.json artefacts.

    The host `_clang_tidy` only sees TUs in the stub-backed test build, so
    files under `backends/{freertos,nuttx,zephyr}/*.c` never get linted.
    This check finds existing cross-compile DBs at predictable locations
    under output/, scopes to backend-specific files, strips gcc-private
    flags Clang's driver rejects, and preserves the supported target and ABI
    of each command. Unsupported commands fail explicitly.

    Filtered DBs are written to `output/tests/lint_backend_dbs/<rtos>/`
    so the original firmware-build artefacts aren't disturbed.

    SKIP cleanly if no firmware build is present — bootstrapping firmware
    requires the cross toolchain, board configure, and Zephyr/NuttX
    workspace download, none of which the lint pipeline should fight with.
    Run after `make stm32f746.freertos.<app>` (or NuttX/Zephyr equivalent)
    or via the dedicated `make lint-backends` target.
    """
    import json
    import glob
    del check
    if not shutil.which("clang-tidy"):
        return ("clang-tidy-backends", "SKIP", "not installed")

    # Predictable build-output locations (per ove configure conventions):
    #   - Firmware app builds:  output/<board>/<rtos>/<app>/build/firmware/
    #                           (NuttX: .../build/nuttx-cmake/)
    #   - QEMU/Renode test:     output/tests/{qemu,renode-stm32f746}-<rtos>*/build/
    #   - Plain test build:     output/tests/<rtos>/build/
    # Select the freshest available database for each RTOS below.
    candidate_globs = [
        os.path.join(ove_dir, "output", "*", "*", "*", "build",
                     "firmware", "compile_commands.json"),
        # NuttX app builds generate theirs inside the NuttX CMake tree.
        os.path.join(ove_dir, "output", "*", "*", "*", "build",
                     "nuttx-cmake", "compile_commands.json"),
        os.path.join(ove_dir, "output", "tests", "*", "build",
                     "compile_commands.json"),
        os.path.join(ove_dir, "output", "tests", "*",
                     "compile_commands.json"),
    ]
    found = []
    for pat in candidate_globs:
        for db_path in glob.glob(pat):
            found.append(db_path)
    if not found:
        return ("clang-tidy-backends", "SKIP",
                "no firmware compile_commands.json — run "
                "'make stm32f746.<rtos>.<app>' or 'make lint-backends'")

    # Pick the freshest DB per RTOS so we lint against the same toolchain
    # the latest build used (avoids stale flag drift if toolchain bumped).
    rtos_db = {}  # rtos -> (mtime, db_path)
    for db_path in found:
        # Detect RTOS from the path: prefer explicit /<rtos>/ directory
        # markers (output/<board>/<rtos>/...) over qemu-<rtos> / renode-<rtos>.
        norm = db_path.replace(os.sep, "/")
        rtos = None
        for candidate in ("freertos", "nuttx", "zephyr"):
            if f"/{candidate}/" in norm or f"-{candidate}/" in norm \
                    or f"-{candidate}-" in norm:
                rtos = candidate
                break
        if not rtos:
            continue
        try:
            mt = os.path.getmtime(db_path)
        except OSError:
            continue
        if rtos not in rtos_db or mt > rtos_db[rtos][0]:
            rtos_db[rtos] = (mt, db_path)

    if not rtos_db:
        return ("clang-tidy-backends", "SKIP",
                "no RTOS-tagged compile_commands.json found")

    failures = []
    commands = BackendCommands()
    files_total = 0
    filtered_root = os.path.join(ove_dir, "output", "tests",
                                 "lint_backend_dbs")
    for rtos in sorted(rtos_db):
        _, db_path = rtos_db[rtos]
        backend_root = os.path.abspath(
            os.path.join(ove_dir, "backends", rtos))
        try:
            with open(db_path, encoding="utf-8") as f:
                entries = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            failures.append(f"{rtos}: db read failed ({e})")
            continue

        # Validate selected commands before writing or invoking any filtered DB.
        filtered = []
        seen = set()
        invalid = False
        if not isinstance(entries, list):
            failures.append(f"{rtos}: compile database must be an array")
            continue
        for entry in entries:
            try:
                source = source_path(entry)
                if not source.startswith(backend_root + os.sep):
                    continue
                normalized = commands.normalize(entry)
                if source in seen:
                    continue
                filtered.append(normalized)
                seen.add(source)
            except ValueError as error:
                failures.append(f"{rtos}: {error}")
                invalid = True
        if invalid:
            continue

        if not filtered:
            continue
        files_total += len(filtered)

        # Write filtered DB to a stable location so clang-tidy can read it.
        filt_dir = os.path.join(filtered_root, rtos)
        os.makedirs(filt_dir, exist_ok=True)
        filt_path = os.path.join(filt_dir, "compile_commands.json")
        try:
            with open(filt_path, "w", encoding="utf-8") as f:
                json.dump(filtered, f)
        except OSError as e:
            failures.append(f"{rtos}: filtered db write failed ({e})")
            continue

        # Each filtered command carries its qualified compiler target.
        # Unknown options remain visible as driver errors; unused codegen
        # options need not obscure findings in analysis-only invocations.
        #
        # `--checks` overrides the repo .clang-tidy: keep the bug-finding
        # families (bugprone / cert / concurrency / clang-analyzer) and
        # drop readability/misc/portability/performance which generate
        # noise on third-party-influenced cross-compile code (vendored
        # STM32 HAL macros, Zephyr/NuttX libc shims).  The host
        # `_clang_tidy` keeps the full check set; this is the
        # cross-compile-specific scope.
        cmd = [
            "clang-tidy",
            "--quiet",
            "-p", filt_dir,
            # Mirror the per-check exclusions from the repo `.clang-tidy`
            # so cross-compile lint matches host-lint posture.
            "--checks=-*,bugprone-*,cert-*,concurrency-*,clang-analyzer-*,"
            "-bugprone-easily-swappable-parameters,"
            "-bugprone-reserved-identifier,"
            "-bugprone-macro-parentheses,"
            "-bugprone-multi-level-implicit-pointer-conversion,"
            "-bugprone-implicit-widening-of-multiplication-result,"
            "-bugprone-narrowing-conversions,"
            "-bugprone-branch-clone,"  # errno mapping noise on backends
            "-cert-dcl37-c,"
            "-cert-dcl51-cpp,"
            "-clang-analyzer-security.insecureAPI."
            "DeprecatedOrUnsafeBufferHandling,"
            "-clang-analyzer-security.insecureAPI.strcpy,"
            # Optin EnumCastOutOfRange fires in vendored Zephyr drivers/gpio.h
            # when our backend casts an int to gpio_flags_t — it can't see
            # that the int IS a flag combination; third-party-induced noise.
            "-clang-analyzer-optin.core.EnumCastOutOfRange",
            # Scope header analysis to our own backend headers while preserving
            # include search order, including third-party paths.
            "--header-filter=backends/",
            "--extra-arg=-Qunused-arguments",
            "--extra-arg=-Wno-unknown-warning-option",
            "--extra-arg=-Wno-unused-command-line-argument",
            "--extra-arg=-ferror-limit=200",
            # Sentinel honoured by ove/heap_assert.h to skip its libc
            # allocator redeclarations — clang rejects the `error`
            # attribute on a redeclaration (see header for rationale).
            "--extra-arg=-D__OVE_LINT__",
        ] + [e["file"] for e in filtered]
        rc, out = _run(cmd, cwd=ove_dir)
        if rc != 0:
            failures.append(
                f"{rtos} ({len(filtered)} files): {out.strip()[:200]}")

    if failures:
        return ("clang-tidy-backends", "FAIL",
                "; ".join(failures))
    if files_total == 0:
        return ("clang-tidy-backends", "OK",
                "no backend files in compile dbs")
    return ("clang-tidy-backends", "OK",
            f"{len(rtos_db)} backend(s), {files_total} files")


def _cargo_clippy(ove_dir, check):
    """Run cargo clippy against the `bindings/rust/ove` crate.

    Reuse the `ove test rust` host configuration and storage probe.
    `make test-lvgl-compile` lints the LVGL-enabled configuration.

    Rust *apps* (apps/rust/*, overtos_apps/*_rust) need a fully
    configured CMake workspace and per-board paths; lint them
    manually after `make <board>.<rtos>.<app>`.
    """
    del check
    if not shutil.which("cargo"):
        return ("cargo clippy", "SKIP", "not installed")
    binding_crate = os.path.join(ove_dir, "bindings", "rust", "ove")
    if not os.path.isfile(os.path.join(binding_crate, "Cargo.toml")):
        return ("cargo clippy", "SKIP", "no binding crate")

    # Build the rust-stub CMake project to materialise the storage-sizes
    # probe + bring up a real workspace for bindgen to chew through.
    # Imported lazily so `ove lint` doesn't pull in test.py at module load.
    from .test import _rust_test_env  # noqa: PLC0415
    output_dir = os.path.join(ove_dir, "output")
    target_dir = os.path.join(output_dir, "tests", "rust_lint", "target")
    try:
        _, rust_env = _rust_test_env(ove_dir, output_dir, target_dir)
    except subprocess.CalledProcessError as e:
        return ("cargo clippy", "FAIL",
                f"workspace setup failed: {e}"[:200])

    env = os.environ.copy()
    env.update(rust_env)
    # Override CARGO_TARGET_DIR so lint artefacts don't collide with the
    # test harness target dir — clippy and `cargo build` use different
    # rustc flags and a shared dir would invalidate both caches.
    env["CARGO_TARGET_DIR"] = os.path.join(
        output_dir, "tests", "rust_lint", "clippy_target")

    try:
        r = subprocess.run(CARGO_CLIPPY_COMMAND, cwd=binding_crate, env=env,
                           capture_output=True, text=True)
    except FileNotFoundError:
        return ("cargo clippy", "SKIP", "cargo disappeared mid-run")
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        return ("cargo clippy", "FAIL", "bindings/rust/ove")
    return ("cargo clippy", "OK", "bindings/rust/ove")


def _zig_ast_check(ove_dir, check):
    """Run `zig ast-check` on every .zig source — parse + semantic check
    without codegen.  Zig has no standalone linter; this is the closest
    built-in equivalent.
    """
    del check
    zig = _find_zig(ove_dir)
    if not zig:
        return ("zig ast-check", "SKIP", "not installed (run `ove download` first)")
    files = list(_glob(ove_dir, ".zig"))
    for ext_root in _external_app_roots():
        files.extend(_glob(ext_root, ".zig"))
    if not files:
        return ("zig ast-check", "OK", "no sources")
    failures = []
    for f in files:
        rc, out = _run([zig, "ast-check", f], cwd=ove_dir)
        if rc != 0:
            failures.append((os.path.relpath(f, ove_dir), out.strip()[:120]))
    if failures:
        detail = "; ".join(f"{p}: {e}" for p, e in failures[:3])
        return ("zig ast-check", "FAIL", detail[:200])
    return ("zig ast-check", "OK", f"{len(files)} files")


def _ruff(ove_dir, check):
    if not shutil.which("ruff"):
        return ("ruff", "SKIP", "not installed (pip install ruff)")
    py_roots = [os.path.join(ove_dir, path)
                for path in ("config/ove-cli", "config/scripts", "tests/python")]
    py_count = sum(1 for root in py_roots for _ in _glob(root, ".py"))
    cmd = ["ruff", "check", *py_roots]
    rc, out = _run(cmd, cwd=ove_dir)
    return ("ruff", "OK" if rc == 0 else "FAIL",
            f"{py_count} Python files" if rc == 0 else out.strip()[:200])


def _backend_struct_guard(ove_dir, check):
    """Forbid `struct ove_*` redefinition inside backend .c files."""
    del check  # read-only; format is n/a for this rule
    violations = lint_backend_struct.check(ove_dir)
    if not violations:
        return ("backend-struct", "OK", "no redefinitions")
    detail = lint_backend_struct.format_violations(violations)
    return ("backend-struct", "FAIL", detail)


def _version_pinning(ove_dir, check):
    """Every package manifest carries the version in include/ove/version.h."""
    del check  # read-only
    violations = lint_version.check(ove_dir)
    if not violations:
        return ("version-pin", "OK", "manifests and stub match include/ove/version.h")
    detail = lint_version.format_violations(violations)
    return ("version-pin", "FAIL", detail)


def _nonnull_public(ove_dir, check):
    """No public declaration may carry OVE_NONNULL.

    The public API rejects NULL at run time with OVE_ERR_INVALID_PARAM, and
    GCC deletes those checks in the implementation once the declaration says
    the parameter is non-NULL.  -Werror=nonnull-compare catches only a compare
    written in the function body, not one inside an inlined helper such as
    ove_check_param(), so the rule is enforced here instead.
    """
    del check  # read-only
    hits = []
    inc = os.path.join(ove_dir, "include", "ove")
    for path in sorted(_glob(inc, ".h")):
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                code = line.split("//", 1)[0].strip()
                if "OVE_NONNULL(" not in code or code.startswith(("#define", "*", "/*")):
                    continue
                hits.append(f"{os.path.relpath(path, ove_dir)}:{n}")
    if not hits:
        return ("nonnull-public", "OK", "no OVE_NONNULL on public declarations")
    return ("nonnull-public", "FAIL", "OVE_NONNULL deletes NULL checks: " + ", ".join(hits))


def _doc_snippets(ove_dir, check):
    """Every C example in the docs compiles against the real headers."""
    del check  # read-only
    status, detail = lint_doc_snippets.check(ove_dir)
    return ("doc-snippets", status, detail)


def _doc_names(ove_dir, check):
    """Every backticked ove_/OVE_ name in the docs exists in the tree."""
    del check  # read-only
    status, detail = lint_doc_snippets.check_names(ove_dir)
    return ("doc-names", status, detail)


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
            _clang_tidy_backends(ove_dir, check),
            _cargo_clippy(ove_dir, check),
            _zig_ast_check(ove_dir, check),
            _backend_struct_guard(ove_dir, check),
            _version_pinning(ove_dir, check),
            _nonnull_public(ove_dir, check),
            _doc_snippets(ove_dir, check),
            _doc_names(ove_dir, check),
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
    """CLI entry point for 'ove lint'. Read-only correctness + format check.

    `--only NAME` filters the result list to a single check by its
    reported name (the first element in each (name, status, msg)
    tuple) — useful for `make lint-backends` and similar focused
    one-off runs.
    """
    only = getattr(args, "only", None)
    results = _run_all(check=True, include_lint=True)
    if only:
        results = [r for r in results if r[0] == only]
        if not results:
            print(f"no check named {only!r}; "
                  f"see `make lint` output for valid names",
                  file=sys.stderr)
            sys.exit(2)
    sys.exit(_print(results))


def cmd_format(args):
    """CLI entry point for 'ove format'. Rewrites files via the formatters
    only — correctness linters (clang-tidy, clippy, zig ast-check) are
    skipped here to keep the fix-in-place semantics narrow and predictable.
    """
    del args
    sys.exit(_print(_run_all(check=False, include_lint=False)))
