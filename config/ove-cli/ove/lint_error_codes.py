# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Lint rule: every OVE_ERR_* defined in the C master header must be pinned
by a compile-time assertion in every language binding.

Why this exists
---------------
`include/ove/types.h` is the single source of truth for the OVE_ERR_* numeric
values.  Each binding has its own compile-time assertion block that pins those
values so a future renumber in the C header breaks every TU instead of
silently miscompiling callers:

  - C master block      include/ove/types.h          (OVE_STATIC_ASSERT)
  - C++ binding         bindings/cpp/ove/types.hpp   (static_assert)
  - Rust binding        bindings/rust/ove/src/error.rs (const fn _assert_codes_match)
  - Zig binding         bindings/zig/ove/src/error.zig (comptime std.debug.assert)

When a new OVE_ERR_* is added to the C header, all four blocks must be
extended.  Forgetting one creates a silent-drift hole: the omitted binding
still compiles when the code is renumbered, and miscompiles callers that
catch by value.  This rule catches the omission at lint time.

The lint compares *sets* of asserted names, not just counts — easier to
explain in the failure message.
"""

import os
import re

# Patterns extract the OVE_ERR_* names asserted in each block.
_DEFINE_RE = re.compile(r"^\s*#define\s+(OVE_ERR_\w+)\s*\(-?\d+\)", re.MULTILINE)
_C_ASSERT_RE = re.compile(r"OVE_STATIC_ASSERT\(\s*(OVE_ERR_\w+)\s*==")
_CPP_ASSERT_RE = re.compile(r"static_assert\(\s*(OVE_ERR_\w+)\s*==")
_RUST_ASSERT_RE = re.compile(r"bindings::(OVE_ERR_\w+)\s*==")
_ZIG_ASSERT_RE = re.compile(r"c\.(OVE_ERR_\w+)\s*==")

# Files inspected (relative to OVE_DIR).
_C_HEADER = "include/ove/types.h"
_BINDINGS = (
    ("cpp",  "bindings/cpp/ove/types.hpp",        _CPP_ASSERT_RE),
    ("rust", "bindings/rust/ove/src/error.rs",    _RUST_ASSERT_RE),
    ("zig",  "bindings/zig/ove/src/error.zig",    _ZIG_ASSERT_RE),
)


def _read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def check(ove_dir: str):
    """Compare OVE_ERR_* defined in the C header against each binding's
    assertion block.  Returns a list of (binding, missing_set, extra_set).
    Empty list means every binding asserts exactly the master set.
    """
    header = _read(os.path.join(ove_dir, _C_HEADER))
    defined = set(_DEFINE_RE.findall(header))
    if not defined:
        # Header missing or unparseable — surface this so a refactor that
        # moves the OVE_ERR_* #defines doesn't silently neuter the lint.
        return [("c-header", {"<no OVE_ERR_* defines found in include/ove/types.h>"}, set())]

    # The C master also has its own assertion block — verify that too.
    bindings = (("c-master", _C_HEADER, _C_ASSERT_RE),) + _BINDINGS

    violations = []
    for name, rel, pattern in bindings:
        src = _read(os.path.join(ove_dir, rel))
        asserted = set(pattern.findall(src))
        missing = defined - asserted
        extra = asserted - defined
        if missing or extra:
            violations.append((name, missing, extra))
    return violations


def format_violations(violations):
    """Render violations as human-readable multi-line text."""
    if not violations:
        return ""
    lines = []
    for name, missing, extra in violations:
        if missing:
            lines.append(
                f"{name}: missing assertions for {sorted(missing)} — "
                f"add to the pinning block"
            )
        if extra:
            lines.append(
                f"{name}: stale assertions for {sorted(extra)} — "
                f"code no longer #defined in include/ove/types.h"
            )
    return "\n    " + "\n    ".join(lines)
