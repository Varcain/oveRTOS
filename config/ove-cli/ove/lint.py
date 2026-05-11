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
    # Auto-bootstrap test-stub so lint runs end-to-end on a bare checkout
    # (CI calls `make lint` without a prior workspace build). Always
    # prefer this over older firmware-build compile dbs that may be
    # symlinked into output/ — those reference cross-compiled flags
    # clang-tidy can't always parse and exclude the kernel/backend
    # sources that test-stub covers.
    stub_build = os.path.join(ove_dir, "output", "tests", "stub")
    stub_db = os.path.join(stub_build, "compile_commands.json")
    if not os.path.isfile(stub_db):
        cmake = shutil.which("cmake")
        if cmake:
            os.makedirs(stub_build, exist_ok=True)
            try:
                subprocess.run(
                    [cmake, "-S", os.path.join(ove_dir, "tests"),
                     "-B", stub_build, "-DCMAKE_BUILD_TYPE=Debug"],
                    cwd=ove_dir, capture_output=True, text=True, check=True)
            except subprocess.CalledProcessError:
                pass
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

    # Lint the project's own C/C++ surface — kernel core, backends, the
    # C++ wrapper, plus any apps the active build pulled in. Excludes
    # vendored/external trees (cmocka, lwip, FreeRTOS, etc.) via the
    # path filter below so noise from upstream code stays out.
    scope_prefixes = [
        os.path.abspath(os.path.join(ove_dir, "src")),
        os.path.abspath(os.path.join(ove_dir, "include")),
        os.path.abspath(os.path.join(ove_dir, "backends")),
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


# gcc-only flags that clang's driver rejects with `unknown argument` errors
# when consumed via a cross-compile compile_commands.json.  We strip these
# before invoking clang-tidy so the parser accepts the rest of the cmdline.
# Anything starting with one of these prefixes (or matching exactly) is
# dropped.  Keep narrow — over-stripping risks silently masking warnings.
_GCC_ONLY_PREFIXES = (
    "-fno-defer-pop",
    "-fno-printf-return-value",
    "-fno-reorder-functions",
    "-fno-allow-store-data-races",
    "-mfp16-format=",
    "-mtp=",
    "-mfix-cmse-cve-2021-35465",
    "--param=",
    "-Wno-pointer-sign",  # clang lacks this; gcc-only spelling
    "-fdiagnostics-color=",  # clang uses different syntax; harmless to drop
    # NuttX cross builds inject `-fprofile-abs-path` (gcc 9+) which
    # clang doesn't accept.  Stripping it has no effect on diagnostics
    # since coverage instrumentation is irrelevant to clang-tidy.
    "-fprofile-abs-path",
)


def _strip_gcc_only(args):
    return [a for a in args if not any(
        a == p or a.startswith(p) for p in _GCC_ONLY_PREFIXES)]


# Path fragments that mark third-party / vendored include trees.  When
# we see `-I<path>` whose path contains one of these, we rewrite it to
# `-isystem<path>` so clang treats the headers as system headers and
# suppresses diagnostics inside them.  This kills the noise from
# vendored Zephyr SDK headers / STM32 HAL macros / NuttX libc shims
# without losing findings in our own backend code.
_THIRDPARTY_PATH_FRAGMENTS = (
    "/dl/",
    "/zephyr-workspace",
    "/zephyr-sdk",
    "/cmocka-src/",
    "/_deps/",
    "/picolibc-",
    "arm-zephyr-eabi/",
    "arm-none-eabi/",
)


def _demote_thirdparty_includes(args):
    out = []
    for arg in args:
        if arg.startswith("-I") and len(arg) > 2:
            path = arg[2:]
            if any(frag in path for frag in _THIRDPARTY_PATH_FRAGMENTS):
                out.append("-isystem" + path)
                continue
        out.append(arg)
    return out


def _clang_tidy_backends(ove_dir, check):
    """Run clang-tidy against each RTOS backend's C code via cross-compile
    compile_commands.json artefacts.

    The host `_clang_tidy` only sees TUs in the stub-backed test build, so
    files under `backends/{freertos,nuttx,zephyr}/*.c` never get linted.
    This check finds existing cross-compile DBs at predictable locations
    under output/, scopes to backend-specific files, strips gcc-private
    flags clang's driver rejects, and runs clang-tidy with
    `--target=arm-none-eabi`.

    Filtered DBs are written to `output/tests/lint_backend_dbs/<rtos>/`
    so the original firmware-build artefacts aren't disturbed.

    SKIP cleanly if no firmware build is present — bootstrapping firmware
    requires the cross toolchain, board configure, and Zephyr/NuttX
    workspace download, none of which the lint pipeline should fight with.
    Run after `make stm32f746.freertos.<app>` (or NuttX/Zephyr equivalent)
    or via the dedicated `make lint-backends` target.
    """
    import json
    import shlex
    import glob
    del check
    if not shutil.which("clang-tidy"):
        return ("clang-tidy-backends", "SKIP", "not installed")

    # Predictable build-output locations (per ove configure conventions):
    #   - Firmware app builds:  output/<board>/<rtos>/<app>/build/firmware/
    #   - QEMU/Renode test:     output/tests/{qemu,renode-stm32f746}-<rtos>*/build/
    #   - Plain test build:     output/tests/<rtos>/build/
    # Order matters — prefer app builds (full surface) over test builds
    # (slimmer surface) so we lint the maximum amount of backend code.
    candidate_globs = [
        os.path.join(ove_dir, "output", "*", "*", "*", "build",
                     "firmware", "compile_commands.json"),
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
    files_total = 0
    filtered_root = os.path.join(ove_dir, "output", "tests",
                                 "lint_backend_dbs")
    # Per-RTOS extra clang-tidy args.  Zephyr needs a force-included
    # shim header (scripts/lint/zephyr_clang_compat.h) that pre-sets
    # the include guard for the SDK's gcc arm_acle.h — the header's
    # wrapper inlines call `__builtin_arm_cdp` etc. with non-constant
    # parameters that clang's stricter signature rejects.  The shim
    # short-circuits arm_acle.h cleanly; backends/zephyr code doesn't
    # call those intrinsics directly so nothing real is masked.
    per_rtos_extra_args = {
        "zephyr": [
            "--extra-arg=-include" + os.path.abspath(os.path.join(
                ove_dir, "scripts", "lint", "zephyr_clang_compat.h")),
        ],
    }
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

        # Build a filtered DB: only backend TUs, with gcc-only flags
        # stripped.  Each entry's `command` (or `arguments`) is rewritten
        # so clang-tidy's invocation goes through clang's driver cleanly.
        filtered = []
        seen = set()
        for e in entries:
            src = e.get("file")
            if not src or src in seen:
                continue
            src_abs = os.path.abspath(src)
            if not src_abs.startswith(backend_root + os.sep):
                continue
            seen.add(src)
            new_entry = dict(e)
            if "arguments" in e and e["arguments"]:
                new_entry["arguments"] = _demote_thirdparty_includes(
                    _strip_gcc_only(e["arguments"]))
            elif "command" in e and e["command"]:
                tokens = shlex.split(e["command"])
                new_entry["command"] = shlex.join(
                    _demote_thirdparty_includes(_strip_gcc_only(tokens)))
            filtered.append(new_entry)

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

        # `--target=arm-none-eabi` keeps clang's parser happy with the
        # remaining -mcpu / -mthumb / -mfpu flags.  -Qunused-arguments
        # silences any residual unrecognised flags so findings dominate
        # the output rather than driver chatter.
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
            # Scope header analysis to our own backend headers; the
            # demote-to-isystem pass above silences vendored headers.
            "--header-filter=backends/",
            "--extra-arg-before=--target=arm-none-eabi",
            "--extra-arg=-Qunused-arguments",
            "--extra-arg=-Wno-unknown-warning-option",
            "--extra-arg=-Wno-unused-command-line-argument",
            "--extra-arg=-ferror-limit=200",
            # Sentinel honoured by ove/heap_assert.h to skip its libc
            # allocator redeclarations — clang rejects the `error`
            # attribute on a redeclaration (see header for rationale).
            "--extra-arg=-D__OVE_LINT__",
        ] + per_rtos_extra_args.get(rtos, []) + [e["file"] for e in filtered]
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

    The crate's build.rs needs LVGL/CMSIS headers and a generated
    storage-sizes env file, none of which exist on a bare checkout.
    Rather than skip the lib (and lint only build.rs), we reuse the
    same workspace prep that `ove test rust` does — building the
    `tests/rust/stub_cmake` project produces ove_storage_sizes.env,
    and `tests/backends/stub/lvgl/` ships a stub LVGL header tree.
    With both present, clippy compiles the crate the same way the
    Rust test harness does and lints every target end-to-end.

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

    # `--features std` matches the test/docs.rs build shape — without
    # it the crate's `extern crate alloc` paths are no_std-only, leading
    # to E0433 "no std" / missing-symbol noise rather than real lint
    # findings.  (The old `bench` feature was retired when bench code
    # moved out of the binding crate into tests/benchmarks/rust/.)
    cmd = [
        "cargo", "clippy", "--all-targets", "--locked",
        "--features", "std", "--",
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
    py_root = os.path.join(ove_dir, "config", "ove-cli")
    py_files = list(_glob(py_root, ".py"))
    cmd = ["ruff", "check", py_root]
    rc, out = _run(cmd, cwd=ove_dir)
    return ("ruff", "OK" if rc == 0 else "FAIL",
            f"{len(py_files)} files" if rc == 0 else out.strip()[:200])


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
            _clang_tidy_backends(ove_dir, check),
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
