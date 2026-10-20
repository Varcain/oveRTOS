# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Regression test for the Rust binding's ``has_trace`` Kconfig gate.

It pins down a defect that broke the Rust binding in a way no C-level
test could see, plus a related check:

1. ``bindings/rust/ove/build.rs`` gates the ``has_trace`` cfg on a Kconfig
   symbol that is *not* the one guarding the C declarations the Rust
   ``trace`` module calls.  ``include/ove/trace.h`` declares
   ``ove_trace_register_name`` / ``ove_trace_mark`` / ``ove_trace_zone_begin``
   / ``ove_trace_zone_end`` under ``#ifdef CONFIG_OVE_TRACE_MARKERS``; with
   ``CONFIG_OVE_TRACE_STREAM=y`` and ``CONFIG_OVE_TRACE_MARKERS=n`` the
   header falls back to ``static inline`` no-ops, which bindgen does not
   emit -- so a gate on ``CONFIG_OVE_TRACE_STREAM`` made the crate call
   symbols that did not exist (E0425).

2. The ``ove_thread_info`` struct literal in ``thread.rs``'s ``empty()``
   must initialise every field the C record (and therefore the bindgen
   output) carries, or the crate fails with E0063 "missing field".

The test is pure Python: it parses the real headers, the real ``build.rs``
gate and the real Rust sources.  It needs no compiler, no network and no
configured workspace, and it runs in well under a second.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
# tests/python/<file> -> tests/python -> tests -> <repo root>
DEFAULT_ROOT = os.path.dirname(os.path.dirname(HERE))
# Escape hatch so the same test can be pointed at a pre-fix copy of the
# tree (used to prove that it actually fails there).
ROOT = os.path.abspath(os.environ.get("OVE_TEST_ROOT", DEFAULT_ROOT))

BUILD_RS = os.path.join(ROOT, "bindings", "rust", "ove", "build.rs")
TRACE_RS = os.path.join(ROOT, "bindings", "rust", "ove", "src", "trace.rs")
THREAD_RS = os.path.join(ROOT, "bindings", "rust", "ove", "src", "thread.rs")
STUB_RS = os.path.join(ROOT, "bindings", "rust", "ove", "src", "bindings_stub.rs")
LIB_RS = os.path.join(ROOT, "bindings", "rust", "ove", "src", "lib.rs")
TRACE_H = os.path.join(ROOT, "include", "ove", "trace.h")
THREAD_H = os.path.join(ROOT, "include", "ove", "thread.h")
CONFIG_IN_DEBUG = os.path.join(ROOT, "config", "Config.in.debug")

STREAM = "CONFIG_OVE_TRACE_STREAM"
MARKERS = "CONFIG_OVE_TRACE_MARKERS"

# The C functions the Rust `trace` module needs; they are declared (with
# external linkage) only under `#ifdef CONFIG_OVE_TRACE_MARKERS`.
MARKER_FUNCTIONS = (
    "ove_trace_register_name",
    "ove_trace_mark",
    "ove_trace_zone_begin",
    "ove_trace_zone_end",
)


def read(path):
    with open(path) as fh:
        return fh.read()


# ---------------------------------------------------------------------------
# a tiny C preprocessor evaluator (only the constructs trace.h uses)
# ---------------------------------------------------------------------------

_DEFINED_RE = re.compile(r"\bdefined\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")
_IFDEF_RE = re.compile(r"^\s*#\s*ifdef\s+([A-Za-z_][A-Za-z0-9_]*)")
_IFNDEF_RE = re.compile(r"^\s*#\s*ifndef\s+([A-Za-z_][A-Za-z0-9_]*)")
_IF_RE = re.compile(r"^\s*#\s*if\s+(.*)$")


def eval_c_expr(expr, defines):
    """Evaluate a `#if` expression over a set of defined macro names."""
    py = _DEFINED_RE.sub(lambda m: repr(m.group(1)) + " in defines", expr)
    py = py.replace("&&", " and ").replace("||", " or ")
    py = re.sub(r"!(?!=)", " not ", py)
    return bool(eval(py, {"__builtins__": {}}, {"defines": defines}))


def active_lines(text, defines):
    """Return the lines of a header that survive conditional inclusion.

    Returns a list of (lineno, text) tuples for the lines that are part of
    the translation unit for the given set of defined macros.
    """
    defines = set(defines)
    stack = []          # entries: [active, taken, parent_active]
    out = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        m = _IFDEF_RE.match(line)
        if m:
            cond = m.group(1) in defines
            parent = all(e[0] for e in stack)
            stack.append([parent and cond, cond, parent])
            continue
        m = _IFNDEF_RE.match(line)
        if m:
            cond = m.group(1) not in defines
            parent = all(e[0] for e in stack)
            stack.append([parent and cond, cond, parent])
            continue
        m = _IF_RE.match(line)
        if m:
            cond = eval_c_expr(m.group(1), defines)
            parent = all(e[0] for e in stack)
            stack.append([parent and cond, cond, parent])
            continue
        if stripped.startswith("#else"):
            if stack:
                entry = stack[-1]
                entry[0] = entry[2] and not entry[1]
                entry[1] = True
            continue
        if stripped.startswith("#elif"):
            if stack:
                entry = stack[-1]
                cond = eval_c_expr(stripped[len("#elif"):], defines)
                entry[0] = entry[2] and cond and not entry[1]
                entry[1] = entry[1] or cond
            continue
        if stripped.startswith("#endif"):
            if stack:
                stack.pop()
            continue
        if stripped.startswith("#define"):
            mm = re.match(r"\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)", line)
            if mm and all(e[0] for e in stack):
                defines.add(mm.group(1))
            continue
        if all(e[0] for e in stack):
            out.append((lineno, line))
    return out


def external_declarations(text, defines, symbol):
    """Is `symbol` declared with external linkage in this configuration?

    A `static inline` fallback has internal linkage: bindgen does not emit
    it, so the Rust binding cannot call it.
    """
    pattern = re.compile(r"(static\s+inline\s+|static\s+)?\b[A-Za-z_][A-Za-z0-9_]*\s+" +
                         re.escape(symbol) + r"\s*\(")
    for _lineno, line in active_lines(text, defines):
        m = pattern.search(line)
        if m and not m.group(1):
            return True
    return False


def enclosing_guard(text, defines, symbol):
    """The innermost `#ifdef` symbol that guards `symbol`'s declaration."""
    pattern = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\s+" + re.escape(symbol) + r"\s*\(")
    stack = []
    for line in text.splitlines():
        stripped = line.strip()
        m = _IFDEF_RE.match(line)
        if m:
            stack.append(m.group(1))
            continue
        if stripped.startswith("#else") or stripped.startswith("#elif"):
            if stack:
                stack[-1] = None
            continue
        if stripped.startswith("#endif"):
            if stack:
                stack.pop()
            continue
        if not stripped.startswith("#"):
            if pattern.search(line):
                guards = [g for g in stack if g]
                if guards:
                    return guards[-1]
    return None


# ---------------------------------------------------------------------------
# the build.rs gate
# ---------------------------------------------------------------------------


def extract_gate(build_rs_src, cfg_name):
    """Return (condition, needles) of the `if` that emits `cargo:rustc-cfg`.

    The condition is the raw Rust source text of the guarding `if`, the
    needles are the config strings it looks for.  Nothing about the
    predicate is hard-coded here, so a changed gate is picked up.
    """
    lines = build_rs_src.splitlines()
    emit_idx = None
    for i, line in enumerate(lines):
        if ("cargo:rustc-cfg=" + cfg_name) in line and "check-cfg" not in line:
            emit_idx = i
            break
    if emit_idx is None:
        return None, []
    emit_indent = len(lines[emit_idx]) - len(lines[emit_idx].lstrip())
    if_idx = None
    for j in range(emit_idx - 1, -1, -1):
        stripped = lines[j].strip()
        if not stripped.startswith("if "):
            continue
        indent = len(lines[j]) - len(lines[j].lstrip())
        if indent < emit_indent:
            if_idx = j
            break
    if if_idx is None:
        return None, []
    cond_lines = [lines[if_idx].strip()]
    while not cond_lines[-1].rstrip().endswith("{") and len(cond_lines) < 8:
        cond_lines.append(lines[if_idx + len(cond_lines)].strip())
    cond = " ".join(cond_lines)
    cond = re.sub(r"^if\s+", "", cond)
    cond = re.sub(r"\s*\{\s*$", "", cond)
    needles = re.findall(r'contains\(\s*"([^"]*)"\s*\)', cond)
    return cond, needles


def eval_gate(cond, config_text):
    """Evaluate the build.rs gate condition against a config header text."""
    py = re.sub(r'(\w+)\.contains\(\s*"([^"]*)"\s*\)',
                lambda m: "contains(%r)" % (m.group(2),), cond)
    py = py.replace("&&", " and ").replace("||", " or ")
    py = re.sub(r"!(?!=)", " not ", py)
    if re.search(r"[A-Za-z_]\w*\s*\(", py.replace("contains(", "")):
        raise AssertionError("unsupported gate expression: %r" % cond)
    return bool(eval(py, {"__builtins__": {}},
                     {"contains": lambda needle: needle in config_text}))


def config_text_for(stream, markers):
    """A synthetic ove_config.h for the given trace Kconfig values."""
    lines = ["#ifndef OVE_CONFIG_H", "#define OVE_CONFIG_H"]
    if stream:
        lines.append("#define %s 1" % STREAM)
    if markers:
        lines.append("#define %s 1" % MARKERS)
    lines.append("#endif /* OVE_CONFIG_H */")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# the Kconfig dependency graph
# ---------------------------------------------------------------------------


def kconfig_depends_on(path, symbol):
    """The symbols `symbol` depends on (config/Config.in.debug)."""
    graph = {}
    current = None
    in_help = False
    for raw in read(path).splitlines():
        line = raw.split("#")[0].strip()
        if not line:
            continue
        match = re.match(r"^config\s+([A-Za-z0-9_]+)$", line)
        if match:
            current = "CONFIG_" + match.group(1)
            graph.setdefault(current, [])
            in_help = False
            continue
        if line in ("endmenu", "endchoice"):
            current = None
            in_help = False
            continue
        if line == "help":
            in_help = True
            continue
        if in_help or current is None:
            continue
        if line.startswith("depends on"):
            expr = line[len("depends on"):]
            for ident in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", expr):
                if ident in ("and", "or", "not"):
                    continue
                dep = "CONFIG_" + ident
                if dep not in graph[current]:
                    graph[current].append(dep)
    return graph.get(symbol, [])


# ---------------------------------------------------------------------------
# Rust source helpers
# ---------------------------------------------------------------------------


def rust_struct_fields(text, struct_name):
    """The field names of a `pub struct <name> { ... }` in Rust source."""
    m = re.search(r"pub struct " + re.escape(struct_name) + r"\s*\{", text)
    if not m:
        return None
    start = text.index("{", m.start())
    depth = 0
    end = start
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    body = text[start + 1:end]
    return re.findall(r"^\s*pub\s+([A-Za-z_][A-Za-z0-9_]*)\s*:", body, re.M)


def rust_literal_entries(body):
    """{field: value} of a Rust struct-literal body."""
    entries = {}
    i = 0
    n = len(body)
    while i < n:
        ch = body[i]
        if ch in "{([":
            i += 1
            continue
        if ch in "})]":
            i += 1
            continue
        m = re.match(r"[A-Za-z_][A-Za-z0-9_]*\s*:(?!:)", body[i:])
        if m:
            name = m.group(0).split(":")[0].strip()
            j = i + m.end()
            depth = 1
            k = j
            while k < n:
                c = body[k]
                if c in "{([":
                    depth += 1
                elif c in "})]":
                    depth -= 1
                    if depth == 0:
                        break
                elif c == "," and depth == 1:
                    break
                k += 1
            entries[name] = body[j:k].strip()
            i = k
            continue
        i += 1
    return entries


def rust_empty_literal(thread_rs_src):
    """The field->value map of the `ove_thread_info` literal in empty().

    Anchored on the `impl bindings::ove_thread_info` block: other types in
    the file have their own `empty()` constructor.
    """
    impl = re.search(r"impl\s+bindings::ove_thread_info\s*\{", thread_rs_src)
    if not impl:
        return None
    impl_start = thread_rs_src.index("{", impl.start())
    depth = 0
    impl_end = impl_start
    for i in range(impl_start, len(thread_rs_src)):
        if thread_rs_src[i] == "{":
            depth += 1
        elif thread_rs_src[i] == "}":
            depth -= 1
            if depth == 0:
                impl_end = i
                break
    impl_body = thread_rs_src[impl_start:impl_end]
    m = re.search(r"pub const fn empty\(\)\s*->\s*Self\s*\{", impl_body)
    if not m:
        return None
    start = impl_body.index("{", m.start())
    depth = 0
    end = start
    for i in range(start, len(impl_body)):
        if impl_body[i] == "{":
            depth += 1
        elif impl_body[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    body = impl_body[start + 1:end]
    lit = re.search(r"Self\s*\{", body)
    if not lit:
        return None
    inner_start = body.index("{", lit.start())
    depth = 0
    inner_end = inner_start
    for i in range(inner_start, len(body)):
        if body[i] == "{":
            depth += 1
        elif body[i] == "}":
            depth -= 1
            if depth == 0:
                inner_end = i
                break
    return rust_literal_entries(body[inner_start + 1:inner_end])


def c_struct_fields(text, struct_name):
    """The field names of `struct <name> { ... }` in a C header."""
    m = re.search(r"struct\s+" + re.escape(struct_name) + r"\s*\{", text)
    if not m:
        return None
    start = text.index("{", m.start())
    depth = 0
    end = start
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    body = text[start + 1:end]
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    body = re.sub(r"//[^\n]*", " ", body)
    fields = []
    for decl in body.split(";"):
        decl = decl.strip()
        if not decl:
            continue
        names = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", decl)
        if not names:
            continue
        fields.append(names[-1])
    return fields


# ---------------------------------------------------------------------------
# the tests
# ---------------------------------------------------------------------------


class RustTraceCfgTest(unittest.TestCase):
    """The `has_trace` gate must match the C declarations it guards."""

    @classmethod
    def setUpClass(cls):
        cls.build_rs = read(BUILD_RS)
        cls.trace_h = read(TRACE_H)
        cls.cond, cls.needles = extract_gate(cls.build_rs, "has_trace")

    def test_gate_is_found_and_readable(self):
        self.assertIsNotNone(self.cond,
                             "no `cargo:rustc-cfg=has_trace` emission found in "
                             "build.rs")
        self.assertTrue(self.needles,
                        "the has_trace gate does not look at any config symbol")

    def test_gate_symbol_guards_the_c_declarations(self):
        """The gate must use the symbol that guards the C declarations."""
        needles = " ".join(self.needles)
        for symbol in MARKER_FUNCTIONS:
            guard = enclosing_guard(self.trace_h, set(), symbol)
            self.assertIsNotNone(
                guard,
                "no #ifdef guard found for %s in include/ove/trace.h" % symbol)
            self.assertIn(guard, needles,
                          "build.rs gates has_trace on %r, but %s is declared "
                          "under #ifdef %s" % (needles, symbol, guard))
        self.assertIn(MARKERS, needles,
                      "build.rs gates has_trace on %r; the marker API is "
                      "declared under #ifdef %s" % (needles, MARKERS))

    def test_gate_matches_c_declarations_for_every_legal_config(self):
        """has_trace must be on exactly when the C symbols are visible.

        For every Kconfig-legal (TRACE_STREAM, TRACE_MARKERS) combination
        the Rust `trace` module may only be compiled when bindgen can emit
        the four functions it calls.
        """
        deps = kconfig_depends_on(CONFIG_IN_DEBUG, MARKERS)
        self.assertIn(STREAM, deps,
                      "%s no longer depends on %s -- the gate may then be "
                      "wrong for the (stream=off, markers=on) combination"
                      % (MARKERS, STREAM))
        for stream in (False, True):
            for markers in (False, True):
                if markers and not stream:
                    continue        # illegal per the Kconfig graph
                cfg = config_text_for(stream, markers)
                has_trace = eval_gate(self.cond, cfg)
                defines = set()
                if stream:
                    defines.add(STREAM)
                if markers:
                    defines.add(MARKERS)
                visible = all(
                    external_declarations(self.trace_h, defines, symbol)
                    for symbol in MARKER_FUNCTIONS)
                self.assertEqual(
                    has_trace, visible,
                    "TRACE_STREAM=%s TRACE_MARKERS=%s: has_trace=%s but the "
                    "C marker declarations are %s"
                    % (stream, markers, has_trace,
                       "externally linked" if visible else "static inline only"))

    def test_trace_module_only_uses_marker_functions(self):
        """The `trace` module must not need a stream-only symbol."""
        used = set(re.findall(r"bindings::([A-Za-z_][A-Za-z0-9_]*)\s*\(",
                               read(TRACE_RS)))
        for symbol in used:
            self.assertIn(symbol, MARKER_FUNCTIONS,
                          "src/trace.rs calls %s, which is not part of the "
                          "marker API guarded by %s" % (symbol, MARKERS))
        self.assertTrue(used, "src/trace.rs calls no binding at all -- the "
                              "test would be vacuous")

    def test_docs_build_enables_has_trace(self):
        """The docs.rs build must keep the marker declarations available.

        build.rs's DOCS_RS branch emits `has_trace` for every module, so
        the hand-written stub declarations stay compiled and src/trace.rs
        still resolves.
        """
        m = re.search(r'DOCS_RS"\)\.is_ok\(\)\s*\{', self.build_rs)
        self.assertIsNotNone(m, "build.rs has no DOCS_RS branch")
        start = self.build_rs.index("{", m.start())
        depth = 0
        end = start
        for i in range(start, len(self.build_rs)):
            if self.build_rs[i] == "{":
                depth += 1
            elif self.build_rs[i] == "}":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        branch = self.build_rs[start:end]
        modules = re.search(r"let modules = \[(.*?)\];", branch, re.S)
        self.assertIsNotNone(modules, "the DOCS_RS branch has no module list")
        self.assertIn('"trace"', modules.group(1),
                      "the DOCS_RS branch does not enable the trace module")
        self.assertIn("cargo:rustc-cfg=has_{m}", branch,
                      "the DOCS_RS branch does not emit the has_<module> cfgs")

    def test_trace_module_is_always_present(self):
        """`ove::trace` exists in every build, like the C no-ops."""
        lib_rs = read(LIB_RS)
        self.assertRegex(lib_rs, r"(?m)^pub mod trace;",
                         "src/lib.rs does not declare `pub mod trace;`")
        self.assertNotRegex(
            lib_rs, r"#\[cfg\([^)]*\)\]\s*\n\s*pub mod trace;",
            "src/lib.rs gates `pub mod trace;`: callers would not compile "
            "with markers off")

    def test_every_binding_call_is_gated_on_has_trace(self):
        """Each call into the marker API needs has_trace, with a no-op
        branch for the other builds."""
        trace_rs = read(TRACE_RS)
        calls = re.findall(r"bindings::[A-Za-z_][A-Za-z0-9_]*\s*\(", trace_rs)
        gated = re.findall(r"#\[cfg\(has_trace\)\]", trace_rs)
        fallbacks = re.findall(r"#\[cfg\(not\(has_trace\)\)\]", trace_rs)
        self.assertTrue(calls)
        # One gate per call, plus the gated `use crate::bindings`.
        self.assertEqual(len(gated), len(calls) + 1,
                         "a binding call in src/trace.rs is not gated")
        self.assertEqual(len(fallbacks), len(calls),
                         "a gated call in src/trace.rs has no no-op branch")


class ThreadInfoLiteralTest(unittest.TestCase):
    """`ove_thread_info::empty()` must initialise every field."""

    @classmethod
    def setUpClass(cls):
        cls.thread_h = read(THREAD_H)
        cls.stub = read(STUB_RS)
        cls.thread_rs = read(THREAD_RS)

    def test_c_and_stub_structs_agree(self):
        c_fields = c_struct_fields(self.thread_h, "ove_thread_info")
        self.assertIsNotNone(c_fields, "struct ove_thread_info not found in "
                                       "include/ove/thread.h")
        stub_fields = rust_struct_fields(self.stub, "ove_thread_info")
        self.assertIsNotNone(stub_fields,
                             "struct ove_thread_info not found in the stub")
        self.assertEqual(sorted(c_fields), sorted(stub_fields),
                         "the stub's ove_thread_info does not match the C "
                         "record")

    def test_empty_initialises_every_field(self):
        entries = rust_empty_literal(self.thread_rs)
        self.assertIsNotNone(entries, "empty() not found in src/thread.rs")
        stub_fields = rust_struct_fields(self.stub, "ove_thread_info")
        missing = [f for f in stub_fields if f not in entries]
        self.assertEqual([], missing,
                         "ove_thread_info::empty() does not initialise %s "
                         "(E0063: missing field)" % missing)
        self.assertIn("generation", stub_fields,
                      "the C record no longer carries a generation field")

    def test_empty_initialises_every_state_times_field(self):
        entries = rust_empty_literal(self.thread_rs)
        self.assertIsNotNone(entries, "empty() not found in src/thread.rs")
        nested = entries.get("state_times")
        self.assertIsNotNone(nested,
                             "empty() does not initialise state_times")
        inner = nested[nested.index("{") + 1:nested.rindex("}")]
        fields = rust_struct_fields(self.stub, "ove_thread_state_times")
        present = rust_literal_entries(inner)
        missing = [f for f in fields if f not in present]
        self.assertEqual([], missing,
                         "ove_thread_state_times is not fully initialised: "
                         "missing %s" % missing)


if __name__ == "__main__":
    unittest.main()
