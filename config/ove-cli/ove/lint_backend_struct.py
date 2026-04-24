# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Lint rule: forbid `struct ove_*` redefinitions inside backend .c files.

Why this exists
---------------
Every `ove_*_storage_t` typedef in a backend storage header (e.g.
`ove_storage_freertos.h`) is a plain alias for the corresponding
`struct ove_X`.  Consumers — the C app, C++ binding, Rust/Zig storage-size
probes — see the size via that typedef.

If a backend .c defines its own `struct ove_X` (typically guarded by a
`#define OVE_X_DEFINED` so the header's fallback stub is skipped), the
local definition is visible only inside that translation unit.  Every
other consumer still sees the header's stub — now silently smaller than
what the backend writes via `_init()`.  The watchdog bug we hit on
STM32F7 was exactly this: 20-byte `struct ove_watchdog` in the backend,
8-byte stub visible elsewhere, BSS corruption on `ove_watchdog_init()`.

Rule: backend .c files must NOT declare `struct ove_*` at top level.
All storage-type layouts live in `ove_storage_<rtos>.h`, where consumers
can see them.

A small allowlist below covers legitimate exceptions (driver handle
types for filesystems).  New entries require an explanation in the
docstring of the file and a peer review.
"""

import os
import re

# path suffix (relative to OVE_DIR) -> set of struct names allowed in that file
ALLOWLIST = {
    # FS backends embed driver handles (FatFS FIL/DIR etc.) that aren't
    # natural for the storage header to declare. They still must match
    # the storage_t typedef; see the _Static_assert in the same file.
    "backends/freertos/freertos_fs.c": {"ove_file", "ove_dir"},
    "backends/nuttx/nuttx_fs.c":       {"ove_file", "ove_dir"},
    "backends/zephyr/zephyr_fs.c":     {"ove_file", "ove_dir"},
    "backends/posix/posix_fs.c":       {"ove_file", "ove_dir"},
}

# A top-level struct definition: `struct ove_<name> {` at the start of a
# line (no identifier between the name and the brace, which would indicate
# a variable declaration / compound literal). We allow a trailing comment.
_STRUCT_RE = re.compile(
    r"^\s*struct\s+(ove_\w+)\s*(?:/\*[^*]*\*/\s*)?\{",
    re.MULTILINE,
)

BACKEND_DIRS = ("backends/freertos", "backends/nuttx", "backends/zephyr",
                "backends/posix")


def _scan_file(path: str, ove_dir: str):
    """Return a list of (rel_path, struct_name) violations."""
    rel = os.path.relpath(path, ove_dir)
    allowed = ALLOWLIST.get(rel, set())
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            src = f.read()
    except OSError:
        return []
    violations = []
    for m in _STRUCT_RE.finditer(src):
        name = m.group(1)
        if name in allowed:
            continue
        violations.append((rel, name))
    return violations


def check(ove_dir: str):
    """Scan every backend .c file. Returns a list of (file, struct) violations."""
    violations = []
    for sub in BACKEND_DIRS:
        root = os.path.join(ove_dir, sub)
        if not os.path.isdir(root):
            continue
        for dirpath, _dirs, files in os.walk(root):
            for fn in files:
                if not fn.endswith(".c"):
                    continue
                violations.extend(_scan_file(os.path.join(dirpath, fn), ove_dir))
    return violations


def format_violations(violations):
    """Render violations as human-readable multi-line text."""
    if not violations:
        return ""
    lines = []
    for rel, name in violations:
        lines.append(
            f"{rel}: backend-local `struct {name}` — move to "
            f"ove_storage_<rtos>.h or add to lint_backend_struct.ALLOWLIST"
        )
    return "\n    " + "\n    ".join(lines)
