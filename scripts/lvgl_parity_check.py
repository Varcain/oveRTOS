#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""LVGL binding parity check across Rust, Zig, and C++ wrappers.

The three lvgl bindings (`bindings/rust/ove/src/lvgl.rs`,
`bindings/zig/ove/src/lvgl.zig`, `bindings/cpp/ove/lvgl.hpp`) are
hand-curated wrappers around the LVGL C API.  A widget method added
to one binding can drift out of the others over time.  This tool
extracts the per-binding method surface, normalises names, and
reports gaps.

The tool intentionally focuses on **method names on widget types** —
the cross-binding signal that matters most for an LVGL app porter.
It does NOT try to assert signature parity (argument types diverge
naturally because each language has different idioms — Rust takes
`&[u8]` where C++ takes `const char *`, etc.).

Usage:
    scripts/lvgl_parity_check.py            # report, exit 0
    scripts/lvgl_parity_check.py --strict   # exit 1 if any non-whitelisted gap

Wired into `make lint` non-blockingly: lint runs the tool without
`--strict`, prints any drift as a warning, but never fails the lint.
"""

import argparse
import re
import sys
from pathlib import Path

OVE_DIR = Path(__file__).resolve().parent.parent

BINDINGS = {
    "rust": OVE_DIR / "bindings/rust/ove/src/lvgl.rs",
    "zig":  OVE_DIR / "bindings/zig/ove/src/lvgl.zig",
    "cpp":  OVE_DIR / "bindings/cpp/ove/lvgl.hpp",
}

# Whitelist file — YAML-like simple format.  Empty by default;
# populate as real intentional gaps surface.
WHITELIST_FILE = OVE_DIR / "tests/audit/lvgl_parity_whitelist.txt"


# ── Name normalisation ────────────────────────────────────────────

# Base-object carrier types per binding.  Each binding factors the
# *shared* widget base surface (size / position / flags / style / event
# callbacks) differently — Rust uses blanket-impl'd traits, Zig comptime
# mixin functions, C++ inherited mixin classes — and attributes those
# methods to a carrier type whose name differs per binding (and, in Zig,
# is copied onto every widget).  A per-`(type, method)` comparison can
# therefore never reconcile a base method across bindings.  We collapse
# every carrier below to the synthetic canonical type `obj`, so a base
# method compares equal regardless of which carrier hosts it.  Concrete
# widget types (label, slider, …) and genuinely distinct types (style,
# state, component, …) are intentionally left as-is.
CANON_BASE = "obj"
BASE_CARRIERS = {
    "rust": {"widget", "layout", "styleable", "event_target"},
    "zig":  {"object_mixin", "style_mixin", "event_mixin"},
    "cpp":  {"object_mixin", "object_view", "style_mixin", "event_mixin"},
}


# Method names that are binding lifetime / raw-handle plumbing, not part
# of the comparable LVGL widget surface.  They differ per language by
# design (pointer access, FFI handle conversion) and are never a parity
# signal, so they are dropped from every binding's surface.
IGNORE_METHODS = {
    "raw", "as_raw", "from_raw", "raw_obj",
    "as_mut_ptr", "as_ptr", "ptr", "param_raw",
}


def canonicalise(surface: set, binding: str) -> set:
    """Collapse base-object carrier types to the synthetic `obj` type and
    drop raw-handle plumbing methods.

    See `BASE_CARRIERS` / `IGNORE_METHODS`.  `Obj`/`obj` (the concrete
    root object) already snake-cases to `obj`, so its own methods merge
    cleanly with the canonicalised base surface.
    """
    carriers = BASE_CARRIERS.get(binding, set())
    out = set()
    for type_name, method in surface:
        if method in IGNORE_METHODS:
            continue
        out.add((CANON_BASE if type_name in carriers else type_name, method))
    return out


_CAMEL_RE = re.compile(r"([a-z0-9])([A-Z])")


def snake(name: str) -> str:
    """camelCase / PascalCase → snake_case.

    Only splits on a lower/digit → upper boundary, so consecutive
    capitals are left fused (the LVGL bindings don't use ALLCAPS method
    names, so this is fine in practice).

    >>> snake("setText")
    'set_text'
    >>> snake("alignTo")
    'align_to'
    >>> snake("HTTPRequest")
    'httprequest'

    A trailing underscore is stripped: it is a keyword-escape idiom
    (Zig's `resume_` / `init_` for the `resume` / `init` keywords, Rust's
    `type_`), so it must compare equal to the un-escaped name in another
    binding.

    >>> snake("resume_")
    'resume'
    """
    return _CAMEL_RE.sub(r"\1_\2", name).lower().rstrip("_")


# ── Per-binding extractors ────────────────────────────────────────

def _close_brace_pos(text: str, start: int) -> int:
    """From position `start` (one past an opening `{`), find the position
    of the matching `}`.  Naive: ignores string/char literals and
    comments — fine for the binding files which don't contain `{`/`}`
    inside strings."""
    depth = 1
    for i in range(start, len(text)):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(text)


def extract_rust(path: Path) -> set:
    """Extract (type, method) tuples from `lvgl.rs`.

    Captures two surfaces:
      * **inherent** impls — `impl T { ... }` blocks — for the
        widget-specific API (skips `impl Trait for T`, whose body is the
        trait's surface, not the type's).
      * **trait** definitions — `trait T { ... }` — whose default
        methods form the *shared base surface*.  The LVGL Rust binding
        factors the base-object API into the blanket-impl'd `Widget` /
        `Layout` / `Styleable` / `EventTarget` traits; `canonicalise`
        later collapses those carrier names to `obj`.
    """
    text = path.read_text()
    surface = set()
    # Inherent impl: `impl [<...>] T [<...>] {` with no `for` before `{`.
    pat = re.compile(
        r"\bimpl(?:<[^>]*>)?\s+(\w+)(?:<[^>]*>)?\s*\{",
        re.MULTILINE,
    )
    for m in pat.finditer(text):
        # Re-check that this isn't `impl Trait for T`: scan back from
        # the match start to confirm no `for` keyword appears between
        # `impl` and `{`.  The regex already excludes `for X` after
        # the typename, but be defensive against edge cases.
        head = text[m.start():m.end()]
        if " for " in head:
            continue
        type_name = m.group(1)
        body_start = m.end()
        body_end = _close_brace_pos(text, body_start)
        body = text[body_start:body_end]
        for fn in re.finditer(r"\bpub\s+(?:const\s+|async\s+|unsafe\s+)*fn\s+(\w+)", body):
            surface.add((snake(type_name), snake(fn.group(1))))
    # Trait definitions: `[pub] trait Name[: bounds] { ... }`.  Default
    # methods (with or without bodies) are the base-object surface.
    trait_pat = re.compile(
        r"\b(?:pub\s+)?trait\s+(\w+)\s*(?::[^{]*)?\{",
        re.MULTILINE,
    )
    for m in trait_pat.finditer(text):
        type_name = m.group(1)
        body_start = m.end()
        body_end = _close_brace_pos(text, body_start)
        body = text[body_start:body_end]
        for fn in re.finditer(r"\bfn\s+(\w+)", body):
            surface.add((snake(type_name), snake(fn.group(1))))
    return surface


def extract_zig(path: Path) -> set:
    """Extract (type, method) tuples from `lvgl.zig`.

    Captures two surfaces:
      * `pub const TypeName = struct { ... pub fn method(...) ... };` —
        the widget-specific API (one struct per widget).
      * `pub fn NameMixin(comptime Self: type) type { return struct {
        ... pub fn method(...) ... }; }` — the *shared base surface*.
        Zig factors the base-object API into comptime mixin functions
        (`ObjectMixin` / `StyleMixin` / `EventMixin`) which each widget
        re-exports per method (`pub const width = ObjectMixin(W).width;`).
        Those `pub const` re-exports are intentionally NOT matched (they
        would duplicate the base surface onto every widget type); we read
        the mixin definitions once instead, and `canonicalise` collapses
        the mixin carrier names to `obj`.
    """
    text = path.read_text()
    surface = set()
    pat = re.compile(
        r"\bpub\s+const\s+(\w+)\s*=\s*struct\s*\{",
        re.MULTILINE,
    )
    for m in pat.finditer(text):
        type_name = m.group(1)
        body_start = m.end()
        body_end = _close_brace_pos(text, body_start)
        body = text[body_start:body_end]
        for fn in re.finditer(r"\bpub\s+(?:inline\s+|extern\s+)?fn\s+(\w+)", body):
            surface.add((snake(type_name), snake(fn.group(1))))
    # Comptime mixin functions: `pub fn NameMixin(comptime ...) type { ... }`.
    mixin_pat = re.compile(
        r"\bpub\s+fn\s+(\w*Mixin)\s*\(\s*comptime[^)]*\)\s*type\s*\{",
        re.MULTILINE,
    )
    for m in mixin_pat.finditer(text):
        type_name = m.group(1)
        body_start = m.end()
        body_end = _close_brace_pos(text, body_start)
        body = text[body_start:body_end]
        for fn in re.finditer(r"\bpub\s+(?:inline\s+|extern\s+)?fn\s+(\w+)", body):
            surface.add((snake(type_name), snake(fn.group(1))))
    return surface


_CPP_TEMPLATE_PREFIX = re.compile(r"^\s*template\s*<[^>]*>\s*")


def extract_cpp(path: Path) -> set:
    """Extract (type, method) tuples from `lvgl.hpp`.

    Walks each `class T { ... };` / `struct T { ... };` body, blanks
    out anything nested deeper than the immediate class body (so
    function calls inside inline method bodies are ignored), and
    matches likely method-declaration lines by their signature shape.

    Skips constructors, destructors, operator overloads, control-flow
    keywords, primitive type tokens, and any name starting with `lv_`
    (those are LVGL C-API calls that occasionally bleed through).
    """
    text = path.read_text()
    surface = set()
    class_pat = re.compile(
        r"\b(?:class|struct)\s+(\w+)(?:\s*:\s*[^{]+?)?\s*\{",
        re.MULTILINE,
    )
    # Per-line declaration shape — kept simple to avoid catastrophic
    # backtracking.  We require the line to contain `Name (` where the
    # part before `Name` looks like a return type or qualifier set.
    decl_pat = re.compile(
        r"^\s*"
        r"(?P<qual>(?:\[\[[^]]+\]\]\s*)?"
        r"(?:static|inline|virtual|explicit|constexpr|friend|noexcept|"
        r"const|mutable|volatile)?\s*"
        r"(?:[\w:]+(?:<[^>]*>)?\s+\*?\s*&?\s*)+)"  # return-type-ish tokens
        r"(?P<name>\w+)\s*\("
    )

    KEYWORDS = {
        "if", "for", "while", "switch", "return", "throw", "delete",
        "sizeof", "alignof", "typeid", "static_cast", "dynamic_cast",
        "reinterpret_cast", "const_cast", "noexcept", "decltype",
        "new", "do", "case", "default", "catch", "try",
    }
    # Type tokens that the return-type-greedy regex can miscapture as a
    # method name.  NB: do NOT add real method names here — `size` was
    # previously listed and silently dropped the legitimate
    # `size()` (lv_obj_set_size) wrapper, manufacturing a phantom gap.
    NOT_METHODS = {
        "bool", "int", "uint", "void", "char", "float", "double",
        "size_t", "uint8_t", "uint16_t", "uint32_t", "int8_t", "int16_t",
        "int32_t", "auto", "explicit", "this", "self",
    }

    for m in class_pat.finditer(text):
        type_name = m.group(1)
        body_start = m.end()
        body_end = _close_brace_pos(text, body_start)
        body = text[body_start:body_end]

        # Blank out content nested deeper than class-body depth 1.
        out = []
        depth = 0
        for ch in body:
            if depth == 0:
                out.append(ch)
            else:
                out.append(" " if ch != "\n" else "\n")
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
        flat = "".join(out)

        for line in flat.splitlines():
            # Strip a leading `template <...>` prefix so templated member
            # functions (e.g. EventMixin's `template <...> Derived &on(...)`)
            # are matched by the return-type-anchored decl pattern below.
            line = _CPP_TEMPLATE_PREFIX.sub("", line)
            mm = decl_pat.match(line)
            if not mm:
                continue
            name = mm.group("name")
            if name == type_name:
                continue
            if name in KEYWORDS or name in NOT_METHODS:
                continue
            if name.startswith("operator"):
                continue
            if name.startswith("lv_"):
                continue
            surface.add((snake(type_name), snake(name)))
    return surface


# ── Whitelist loading ─────────────────────────────────────────────

def load_whitelist(path: Path) -> set:
    """Read intentional-gaps whitelist.

    Format: one entry per line, `binding type method` (whitespace
    separated).  Blank lines and lines starting with `#` are ignored, and
    a trailing `# ...` comment on an entry line is stripped (so each gap
    can carry an inline rationale).  Returns a set of
    (binding, type_snake, method_snake) tuples.
    """
    out = set()
    if not path.is_file():
        return out
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 3:
            print(
                f"lvgl_parity_check: malformed whitelist line: {line!r}",
                file=sys.stderr,
            )
            continue
        binding, type_name, method = parts
        out.add((binding, snake(type_name), snake(method)))
    return out


# ── Report ────────────────────────────────────────────────────────

def live_gaps(surfaces: dict) -> set:
    """All `(binding, type, method)` tuples that are *only* in `binding`
    relative to at least one other binding (before whitelist filtering).

    This is the ground truth a whitelist entry must match to be useful:
    an entry naming a tuple that is no longer only-in-one-binding (the
    method got added everywhere, or removed from its own binding) is
    stale and should be deleted.
    """
    names = sorted(surfaces.keys())
    out = set()
    for a in names:
        for b in names:
            if a == b:
                continue
            for tm in surfaces[a] - surfaces[b]:
                out.add((a, *tm))
    return out


def report(surfaces: dict, whitelist: set, strict: bool, verbose: bool) -> int:
    """Print cross-binding gaps, return exit code."""
    names = sorted(surfaces.keys())
    any_gap = False
    print("# LVGL binding parity report")
    print()
    for name, surface in surfaces.items():
        print(f"## {name}: {len(surface)} method(s) on {len(set(t for t, _ in surface))} type(s)")
    print()

    # Pairwise gaps.
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            sa, sb = surfaces[a], surfaces[b]
            only_a = {x for x in sa - sb if (a, *x) not in whitelist}
            only_b = {x for x in sb - sa if (b, *x) not in whitelist}

            if not only_a and not only_b:
                print(f"## {a} ↔ {b}: in sync")
                continue
            any_gap = True
            print(f"## {a} ↔ {b}: drift "
                  f"({len(only_a)} only-in-{a}, {len(only_b)} only-in-{b})")
            if verbose:
                if only_a:
                    print(f"  In {a} but not {b}:")
                    for tn, mn in sorted(only_a):
                        print(f"    - {tn}::{mn}")
                if only_b:
                    print(f"  In {b} but not {a}:")
                    for tn, mn in sorted(only_b):
                        print(f"    - {tn}::{mn}")

    # Whitelist-staleness guard: every whitelisted tuple must still name a
    # real only-in-one-binding gap.  A stale entry silently suppresses
    # nothing and rots — flag it (and fail under --strict so the gate
    # forces the whitelist to track reality).
    stale = sorted(whitelist - live_gaps(surfaces))
    print()
    if stale:
        print(f"## stale whitelist entries ({len(stale)})")
        print("  These no longer match any gap (method added everywhere, "
              "renamed, or removed) — delete them from "
              f"{WHITELIST_FILE.relative_to(OVE_DIR)}:")
        for binding, tn, mn in stale:
            print(f"    - {binding} {tn} {mn}")
        print()

    if any_gap:
        if not verbose:
            print(
                "Tip: re-run with `--verbose` for the per-method gap list, "
                "or `--strict` to fail on any non-whitelisted gap."
            )
        print(
            "Note: cross-binding name normalisation is heuristic (snake_case "
            "of the symbol name; no signature comparison).  False positives are "
            "expected when a method has different names across bindings — add a "
            f"line to {WHITELIST_FILE.relative_to(OVE_DIR)} when an entry is intentional."
        )
    return 1 if (strict and (any_gap or stale)) else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--strict",
        action="store_true",
        help="Exit non-zero if any non-whitelisted gap is found.",
    )
    ap.add_argument(
        "--verbose",
        action="store_true",
        help="Print the per-method gap list (default: only summary counts).",
    )
    args = ap.parse_args()

    surfaces = {}
    for name, path in BINDINGS.items():
        if not path.is_file():
            print(
                f"lvgl_parity_check: {name} binding not found at {path} — skipping",
                file=sys.stderr,
            )
            continue
        if name == "rust":
            surfaces[name] = extract_rust(path)
        elif name == "zig":
            surfaces[name] = extract_zig(path)
        elif name == "cpp":
            surfaces[name] = extract_cpp(path)
        surfaces[name] = canonicalise(surfaces[name], name)

    if len(surfaces) < 2:
        print(
            "lvgl_parity_check: need at least 2 bindings present to compare; skipping",
            file=sys.stderr,
        )
        return 0

    whitelist = load_whitelist(WHITELIST_FILE)
    return report(surfaces, whitelist, args.strict, args.verbose)


if __name__ == "__main__":
    sys.exit(main())
