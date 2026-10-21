# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Lint rule: every C example in the docs compiles against the real API.

Why this exists
---------------
The cookbook, API reference and getting-started pages show C code, but
nothing ever built it, so it drifted onto functions that do not exist. This
rule pulls out each ```c block under _PAGE_DIRS, prepends
`#include "ove/ove.h"`, and compiles it to assembly against include/ with
tests/docs/ove_config.h (every module the docs use switched on). LVGL comes
from dl/lvgl when it has been downloaded, else from the stub header the host
tests use.

The compile generates code (-S) at -O0 on purpose. GCC reports an ignored
OVE_NODISCARD result only while generating code, so -fsyntax-only never sees
one, and at -O1 and above it drops unreferenced static functions before
warning, so ignored results inside them go unreported. GCC also ignores a
(void) cast here, so a snippet has to use the result.

The line before a fence can mark the block:
  <!-- ove:body -->      statements only: compiled inside a function body
  <!-- ove:fragment -->  an excerpt that cannot stand alone: skipped

The compile is for the host, so a format mismatch that only exists on the
32-bit targets (uint32_t is unsigned long on arm-none-eabi) is not caught;
snippets cast printf arguments for that reason.

_PENDING lists pages still being rewritten. A pending page that compiles is
reported too, so the list can only shrink.
"""

import os
import re
import shutil
import subprocess
import tempfile

_PAGE_DIRS = ("docs-site/docs/cookbook", "docs-site/docs/api",
              "docs-site/docs/getting-started", "docs-site/docs/examples")
_FRAGMENT_MARK = "<!-- ove:fragment -->"
_BODY_MARK = "<!-- ove:body -->"
_BLOCK_RE = re.compile(r"^```c[ \t]*\n(.*?)^```[ \t]*$", re.MULTILINE | re.DOTALL)

# page (relative to docs-site/docs) -> why it does not compile yet
_PENDING = {}


def _blocks(text):
    """Yield (line, code) for each ```c block not marked as a fragment; a
    block marked as a body comes back wrapped in a function."""
    for m in _BLOCK_RE.finditer(text):
        line = text.count("\n", 0, m.start()) + 1
        before = text[:m.start()].rstrip("\n").rsplit("\n", 1)[-1].strip()
        if before == _FRAGMENT_MARK:
            continue
        code = m.group(1)
        if before == _BODY_MARK:
            code = "void ove_doc_excerpt(void)\n{\n" + code + "}\n"
        yield line, code


def _include_args(ove_dir):
    def inc(*parts):
        return "-I" + os.path.join(ove_dir, *parts)

    args = [inc("tests", "docs"), inc("include"), inc("tests", "host", "posix", "include")]
    if os.path.isfile(os.path.join(ove_dir, "dl", "lvgl", "lvgl.h")):
        args += [inc("dl", "lvgl"), inc("dl"), inc("tests", "host"), "-DLV_CONF_INCLUDE_SIMPLE"]
    else:
        args += [inc("tests", "backends", "stub", "lvgl"), inc("tests", "backends", "stub")]
    return args


def _compile(cc, args, code):
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write('#include "ove/ove.h"\n')
        f.write(code)
        path = f.name
    try:
        r = subprocess.run([cc, "-std=c11", "-S", "-O0", "-o", os.devnull, "-Wall", "-Werror",
                            "-Wno-unused-function", "-Wno-unused-variable"] + args + [path],
                           capture_output=True, text=True)
    finally:
        os.unlink(path)
    out = (r.stdout + r.stderr).replace(path, "<snippet>")
    first = next((ln for ln in out.splitlines() if "error" in ln), out.strip()[:200])
    return r.returncode == 0, first


def check(ove_dir):
    """Returns (status, detail): status is "OK", "FAIL" or "SKIP"."""
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        return "SKIP", "no host C compiler"
    docs_root = os.path.join(ove_dir, "docs-site", "docs")
    args = _include_args(ove_dir)
    problems = []
    compiled = 0
    pages = []
    for d in _PAGE_DIRS:
        full = os.path.join(ove_dir, d)
        if os.path.isdir(full):
            pages += [os.path.join(full, p) for p in sorted(os.listdir(full)) if p.endswith(".md")]
    for path in pages:
        page = os.path.relpath(path, docs_root)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        page_ok = True
        for line, code in _blocks(text):
            ok, err = _compile(cc, args, code)
            compiled += 1
            if not ok:
                page_ok = False
                if page not in _PENDING:
                    problems.append(f"{page}:{line}: {err}")
        if page in _PENDING and page_ok:
            problems.append(f"{page}: compiles now; remove it from _PENDING")
    if problems:
        return "FAIL", "; ".join(problems)
    return "OK", f"{compiled} blocks, {len(_PENDING)} pages pending"


# ── doc-names: API names quoted in prose and tables ─────────────────────────
#
# Text outside code fences is not compiled, so a renamed or invented
# `ove_*` / `OVE_*` name there goes unnoticed. Every such name in backticks
# must appear somewhere in the source tree. Prefixes (`ove_mutex_`) and the
# `ove_X_*` placeholder used for API families are exempt.

_SOURCE_ROOTS = ("include", "src", "backends", "bindings", "drivers", "boards", "config",
                 "cmake", "sim", "tests", "scripts", "apps", "models", "CMakeLists.txt",
                 "Makefile")
_SKIP_DIRS = {"output", "dl", "target", ".git", "__pycache__", "node_modules"}
_NAME_RE = re.compile(r"\b(?:ove|OVE)_[A-Za-z0-9_]+\b")
_TICKED_RE = re.compile(r"`[^`\n]*`")
_FENCE_RE = re.compile(r"^```.*?^```", re.MULTILINE | re.DOTALL)


def _known_names(ove_dir):
    names = set()
    for root in _SOURCE_ROOTS:
        path = os.path.join(ove_dir, root)
        files = [path] if os.path.isfile(path) else []
        for dp, dirs, fs in os.walk(path):
            dirs[:] = [d for d in dirs if d not in _SKIP_DIRS]
            files += [os.path.join(dp, f) for f in fs if not f.endswith(".md")]
        for f in files:
            try:
                with open(f, encoding="utf-8", errors="ignore") as fh:
                    names.update(_NAME_RE.findall(fh.read()))
            except OSError:
                pass
    return names


def check_names(ove_dir):
    """Returns (status, detail) for backticked API names in the docs."""
    known = _known_names(ove_dir)
    docs_root = os.path.join(ove_dir, "docs-site", "docs")
    problems = []
    checked = 0
    for dp, _dirs, fs in os.walk(docs_root):
        for f in sorted(fs):
            if not f.endswith(".md"):
                continue
            path = os.path.join(dp, f)
            with open(path, encoding="utf-8") as fh:
                text = _FENCE_RE.sub("", fh.read())
            for span in _TICKED_RE.findall(text):
                for name in _NAME_RE.findall(span):
                    if name.endswith("_") or name.startswith("ove_X") or "_X_" in name:
                        continue
                    checked += 1
                    if name not in known:
                        problems.append(f"{os.path.relpath(path, docs_root)}: {name}")
    if problems:
        return "FAIL", "unknown names: " + "; ".join(sorted(set(problems)))
    return "OK", f"{checked} names"
