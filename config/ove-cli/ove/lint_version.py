# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Lint rule: every package manifest, and the Rust bindings stub, carries the
version in include/ove/version.h.

Why this exists
---------------
include/ove/version.h is the single source of truth for the oveRTOS version.
The packages built from this tree each state it again: the ove CLI
(pyproject.toml), the Rust crates (Cargo.toml) and the Zig module
(build.zig.zon).  Nothing else ties them together — cargo notices only a
bump that leaves ove's requirement on ove-macros unsatisfiable, and the
firmware build compiles the Zig module without reading its zon — so a
release bump that misses one goes unseen.  The Rust crate's docs.rs stub
(bindings_stub.rs) holds a copy of the header's constants as well, which
only a docs.rs build would trip over.

Parsed with regular expressions, not tomllib: the CLI supports Python 3.8.
"""

import os
import re

_HEADER = "include/ove/version.h"

# (manifest, section the version line lives in, or None for the zon)
_MANIFESTS = (
    ("config/ove-cli/pyproject.toml", "project"),
    ("bindings/rust/ove/Cargo.toml", "package"),
    ("bindings/rust/ove-macros/Cargo.toml", "package"),
    ("bindings/rust/ove-allocator/Cargo.toml", "package"),
    ("bindings/zig/ove/build.zig.zon", None),
)

# ove's dependency on ove-macros, whose requirement must admit this version.
_MACROS_DEP = ("bindings/rust/ove/Cargo.toml", "ove-macros")

# The Rust crate's stand-in for bindgen output in docs.rs builds, which keeps
# its own copy of the header's constants.
_STUB = "bindings/rust/ove/src/bindings_stub.rs"
_STUB_INT_RE = re.compile(r"^pub const OVE_VERSION_(MAJOR|MINOR|PATCH): u32 = (\d+);", re.MULTILINE)
_STUB_STR_RE = re.compile(r'^pub const OVE_VERSION_STRING: &\[u8; \d+\] = b"([^"\\]*)\\0";',
                          re.MULTILINE)
_STUB_PACKED_RE = re.compile(r"^pub const OVE_VERSION: u32 = (\d+);", re.MULTILINE)

_INT_RE = re.compile(r"^\s*#define\s+OVE_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)\b", re.MULTILINE)
_STR_RE = re.compile(r'^\s*#define\s+OVE_VERSION_STRING\s+"([^"]*)"', re.MULTILINE)
_ZON_RE = re.compile(r'^\s*\.version\s*=\s*"([^"]*)"', re.MULTILINE)


def _read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def _toml_section_version(text, section):
    """The `version = "..."` of a [section] table, or None."""
    m = re.search(r"^\[" + re.escape(section) + r"\]\s*$(.*?)(?=^\[|\Z)", text,
                  re.MULTILINE | re.DOTALL)
    if not m:
        return None
    v = re.search(r'^\s*version\s*=\s*"([^"]*)"', m.group(1), re.MULTILINE)
    return v.group(1) if v else None


def check(ove_dir: str):
    """Compare each manifest's version with include/ove/version.h.

    Returns a list of (file, problem) pairs; empty when everything agrees.
    """
    header = _read(os.path.join(ove_dir, _HEADER))
    if header is None:
        return [(_HEADER, "missing")]
    parts = dict(_INT_RE.findall(header))
    if set(parts) != {"MAJOR", "MINOR", "PATCH"}:
        return [(_HEADER, "OVE_VERSION_MAJOR/MINOR/PATCH not all defined")]
    major, minor, patch = (int(parts[k]) for k in ("MAJOR", "MINOR", "PATCH"))
    version = f"{major}.{minor}.{patch}"

    violations = []
    s = _STR_RE.search(header)
    if not s:
        violations.append((_HEADER, "OVE_VERSION_STRING not defined"))
    elif s.group(1) != version:
        violations.append((_HEADER, f'OVE_VERSION_STRING "{s.group(1)}", '
                                    f"the numbers say {version}"))

    for rel, section in _MANIFESTS:
        text = _read(os.path.join(ove_dir, rel))
        if text is None:
            violations.append((rel, "missing"))
            continue
        if section is None:
            m = _ZON_RE.search(text)
            found = m.group(1) if m else None
        else:
            found = _toml_section_version(text, section)
        if found is None:
            violations.append((rel, "no version found"))
        elif found != version:
            violations.append((rel, f"version {found}, {_HEADER} says {version}"))

    stub = _read(os.path.join(ove_dir, _STUB))
    if stub is None:
        violations.append((_STUB, "missing"))
    else:
        sparts = dict(_STUB_INT_RE.findall(stub))
        sstr = _STUB_STR_RE.search(stub)
        spacked = _STUB_PACKED_RE.search(stub)
        found = (f"{sparts.get('MAJOR')}.{sparts.get('MINOR')}.{sparts.get('PATCH')}",
                 sstr.group(1) if sstr else None,
                 int(spacked.group(1)) if spacked else None)
        expected = (version, version, (major << 16) | (minor << 8) | patch)
        if found != expected:
            violations.append((_STUB, f"OVE_VERSION_* constants say "
                                      f"{found[0]} / \"{found[1]}\" / {found[2]}, "
                                      f"{_HEADER} says {version} / {expected[2]}"))

    rel, dep = _MACROS_DEP
    text = _read(os.path.join(ove_dir, rel)) or ""
    m = re.search(r"^\s*" + re.escape(dep) + r'\s*=\s*\{[^}]*\bversion\s*=\s*"([^"]*)"',
                  text, re.MULTILINE)
    allowed = (f"{major}.{minor}", version)
    if not m:
        violations.append((rel, f"no version requirement on {dep}"))
    elif m.group(1) not in allowed:
        violations.append((rel, f'{dep} requirement "{m.group(1)}", '
                                f'expected "{allowed[0]}" or "{allowed[1]}"'))
    return violations


def format_violations(violations):
    """Render violations as human-readable multi-line text."""
    if not violations:
        return ""
    return "\n    " + "\n    ".join(f"{rel}: {problem}" for rel, problem in violations)
