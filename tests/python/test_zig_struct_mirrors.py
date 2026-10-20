#!/usr/bin/env python3
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Regression test: the Zig binding's `extern struct` mirrors must stay
layout-identical to the C structs they mirror.

`bindings/zig/ove/src/thread.zig` declares `ThreadInfo` as a hand-written
`extern struct` that shadows the translate-c view `c.struct_ove_thread_info`
of `struct ove_thread_info` (`include/ove/thread.h`).  Every change to the C
struct has to be mirrored in the Zig declaration, otherwise

  * the Zig build stops on the `comptime` layout assertions in that file, or
  * - worse - the binding silently reads the wrong bytes at runtime and the
    C side overruns the caller's buffer (the mirror used to be 88 bytes while
    `struct ove_thread_info` had grown to 96).

This test parses both sides, computes the two layouts under the host C ABI
and compares them field by field:

  * every C field must have a counterpart in the Zig mirror, at the same
    offset and with the same size and alignment;
  * the field order must agree;
  * the totals (size, alignment) must agree;
  * every `std.debug.assert(...)` layout/enum pin in the Zig source must
    hold under the computed layouts.

The layouts come from a small C-ABI model that is cross-checked against
`ctypes` (the host C ABI) and, when a C compiler is available, against the
compiler itself.  No network, no build system and no Zig toolchain needed.
"""

import ctypes
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.environ.get("OVE_REPO_ROOT") or os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

ZIG_SRC_DIR = os.path.join(ROOT, "bindings", "zig", "ove", "src")
HEADER_ROOTS = [
    os.path.join(ROOT, "include"),
    os.path.join(ROOT, "sim", "include"),
]


# ---------------------------------------------------------------------------
# C ABI model
# ---------------------------------------------------------------------------

PTR_SIZE = ctypes.sizeof(ctypes.c_void_p)


def _align_up(value, align):
    return (value + align - 1) // align * align


def _c_primitives():
    """size/alignment of the C scalar types, for the host ABI."""
    table = {
        "char": (1, 1),
        "signed char": (1, 1),
        "unsigned char": (1, 1),
        "_Bool": (1, 1),
        "bool": (1, 1),
        "short": (2, 2),
        "signed short": (2, 2),
        "unsigned short": (2, 2),
        "int": (4, 4),
        "signed int": (4, 4),
        "unsigned int": (4, 4),
        "unsigned": (4, 4),
        "int8_t": (1, 1),
        "uint8_t": (1, 1),
        "int16_t": (2, 2),
        "uint16_t": (2, 2),
        "int32_t": (4, 4),
        "uint32_t": (4, 4),
        "float": (4, 4),
        "double": (8, 8),
    }
    if PTR_SIZE == 8:
        table.update({
            "long": (8, 8),
            "unsigned long": (8, 8),
            "long long": (8, 8),
            "unsigned long long": (8, 8),
            "int64_t": (8, 8),
            "uint64_t": (8, 8),
            "size_t": (8, 8),
            "ssize_t": (8, 8),
            "ptrdiff_t": (8, 8),
            "uintptr_t": (8, 8),
            "intptr_t": (8, 8),
        })
    else:
        table.update({
            "long": (4, 4),
            "unsigned long": (4, 4),
            "long long": (8, 4),
            "unsigned long long": (8, 4),
            "int64_t": (8, 4),
            "uint64_t": (8, 4),
            "size_t": (4, 4),
            "ssize_t": (4, 4),
            "ptrdiff_t": (4, 4),
            "uintptr_t": (4, 4),
            "intptr_t": (4, 4),
        })
    return table


C_PRIMS = _c_primitives()

ZIG_PRIMS = {
    "u8": (1, 1),
    "i8": (1, 1),
    "u16": (2, 2),
    "i16": (2, 2),
    "u32": (4, 4),
    "i32": (4, 4),
    "u64": (8, 8),
    "i64": (8, 8),
    "usize": (PTR_SIZE, PTR_SIZE),
    "isize": (PTR_SIZE, PTR_SIZE),
    "c_char": (1, 1),
    "c_short": (2, 2),
    "c_ushort": (2, 2),
    "c_int": (4, 4),
    "c_uint": (4, 4),
    "c_long": C_PRIMS["long"],
    "c_ulong": C_PRIMS["unsigned long"],
    "c_longlong": C_PRIMS["long long"],
    "c_ulonglong": C_PRIMS["unsigned long long"],
    "bool": (1, 1),
}


class Field(object):
    """One struct member: its name, type, size, alignment and offset."""

    def __init__(self, name, type_str, size, align, offset):
        self.name = name
        self.type_str = type_str
        self.size = size
        self.align = align
        self.offset = offset

    @property
    def end(self):
        return self.offset + self.size

    def __repr__(self):
        return "Field(%s @%d size=%d)" % (self.name, self.offset, self.size)


class Layout(object):
    """The computed layout of a C struct or of a Zig `extern struct`."""

    def __init__(self, name, fields, size, align):
        self.name = name
        self.fields = fields
        self.size = size
        self.align = align

    def field(self, name):
        for f in self.fields:
            if f.name == name:
                return f
        return None

    def offsets(self):
        return dict((f.name, f.offset) for f in self.fields)

    def describe(self):
        rows = ["%s: size=%d align=%d" % (self.name, self.size, self.align)]
        for f in self.fields:
            rows.append("    %-22s %-24s off=%-4d size=%d" %
                        (f.name, f.type_str, f.offset, f.size))
        return "\n".join(rows)


# ---------------------------------------------------------------------------
# source parsing helpers
# ---------------------------------------------------------------------------


def _read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def strip_preprocessor(text):
    """Drop `#`-directives; they never contribute struct members."""
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))


def brace_body(text, start):
    """`text[start]` is '{'; return (body, index just past the matching '}')."""
    depth = 0
    i = start
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i], i + 1
        i += 1
    raise ValueError("unbalanced braces")


def eval_int(expr):
    """Evaluate a simple integer constant; None when it is not one."""
    expr = expr.strip()
    if not expr:
        return None
    try:
        return int(expr, 0)
    except ValueError:
        return None


def parse_enum_members(body):
    """[(name, value)] for the body of a C or Zig enum."""
    members = []
    value = None
    for part in body.split(","):
        part = " ".join(part.split())
        if not part:
            continue
        if "=" in part:
            name, _, expr = part.partition("=")
            name = name.strip()
            parsed = eval_int(expr)
            if parsed is None:
                parsed = 0 if value is None else value + 1
            value = parsed
        else:
            name = part
            value = 0 if value is None else value + 1
        members.append((name, value))
    return members


def split_c_decls(body):
    """[(type, name, dims)] for the members of a C struct body."""
    decls = []
    depth = 0
    chunk = ""
    for ch in body:
        if ch in "{[(":
            depth += 1
        elif ch in "}])":
            depth -= 1
        if ch == ";" and depth == 0:
            text = " ".join(chunk.split())
            chunk = ""
            parsed = split_c_decl(text)
            if parsed:
                decls.append(parsed)
            continue
        chunk += ch
    return decls


def split_c_decl(text):
    """One C member declaration -> (type, name, dims); None when not a member."""
    if not text:
        return None
    for keyword in ("typedef", "static_assert", "_Static_assert"):
        if text.startswith(keyword):
            return None
    fnptr = re.match(r"^(.*)\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(.*\)$", text)
    if fnptr:
        return (fnptr.group(1).strip() + " *", fnptr.group(2), [])
    m = re.match(r"^(?P<type>.*?)\s*(?P<name>[A-Za-z_]\w*)\s*"
                 r"(?P<dims>(?:\[[^\]]*\])*)$", text)
    if not m:
        return None
    dims = [int(d) for d in re.findall(r"\[(\d+)\]", m.group("dims"))]
    return (m.group("type").strip(), m.group("name"), dims)


# ---------------------------------------------------------------------------
# C headers
# ---------------------------------------------------------------------------


class CHeaders(object):
    """Index of every struct/enum declared in the repository's headers."""

    def __init__(self, roots):
        self.structs = {}        # struct name -> (path, body)
        self.unions = {}         # union name -> (path, body)
        self.enum_typedefs = {}  # typedef name -> [(member, value)]
        self.constants = {}      # enumerator name -> value
        self._layouts = {}
        self._union_layouts = {}
        for root in roots:
            if not os.path.isdir(root):
                continue
            for dirpath, dirnames, filenames in os.walk(root):
                dirnames[:] = [d for d in dirnames
                               if d not in (".git", "output", "node_modules")]
                for name in sorted(filenames):
                    if name.endswith(".h"):
                        self._scan(os.path.join(dirpath, name))

    # -- scanning ---------------------------------------------------------

    def _scan(self, path):
        text = strip_preprocessor(strip_comments(_read(path)))
        for m in re.finditer(r"\btypedef\s+enum\b[^{;]*\{", text):
            body, end = brace_body(text, m.end() - 1)
            members = parse_enum_members(body)
            tail = re.match(r"\s*([A-Za-z_]\w*)\s*;", text[end:])
            if tail:
                self.enum_typedefs.setdefault(tail.group(1), members)
            for name, value in members:
                self.constants.setdefault(name, value)
        for m in re.finditer(r"\benum\s+([A-Za-z_]\w*)\s*\{", text):
            body, _ = brace_body(text, m.end() - 1)
            for name, value in parse_enum_members(body):
                self.constants.setdefault(name, value)
        for m in re.finditer(r"\bstruct\s+([A-Za-z_]\w*)\s*\{", text):
            name = m.group(1)
            if name in self.structs:
                continue
            body, _ = brace_body(text, m.end() - 1)
            self.structs[name] = (path, body)
        for m in re.finditer(r"\btypedef\s+struct\s*\{", text):
            body, end = brace_body(text, m.end() - 1)
            tail = re.match(r"\s*([A-Za-z_]\w*)\s*;", text[end:])
            if tail:
                self.structs.setdefault(tail.group(1), (path, body))
        for m in re.finditer(r"\bunion\s+([A-Za-z_]\w*)\s*\{", text):
            name = m.group(1)
            if name in self.unions:
                continue
            body, _ = brace_body(text, m.end() - 1)
            self.unions[name] = (path, body)

    # -- layout -----------------------------------------------------------

    def layout(self, struct_name):
        if struct_name in self._layouts:
            return self._layouts[struct_name]
        if struct_name not in self.structs:
            raise KeyError("unknown C struct: %s" % struct_name)
        _path, body = self.structs[struct_name]
        fields = []
        offset = 0
        max_align = 1
        for type_str, name, dims in split_c_decls(body):
            size, align = self.type_size_align(type_str)
            for dim in dims:
                size *= dim
            offset = _align_up(offset, align)
            fields.append(Field(name, type_str, size, align, offset))
            offset += size
            max_align = max(max_align, align)
        lay = Layout(struct_name, fields, _align_up(offset, max_align), max_align)
        self._layouts[struct_name] = lay
        return lay

    def union_layout(self, union_name):
        if union_name in self._union_layouts:
            return self._union_layouts[union_name]
        if union_name not in self.unions:
            raise KeyError("unknown C union: %s" % union_name)
        _path, body = self.unions[union_name]
        size = 0
        max_align = 1
        for type_str, _name, dims in split_c_decls(body):
            msize, align = self.type_size_align(type_str)
            for dim in dims:
                msize *= dim
            size = max(size, msize)
            max_align = max(max_align, align)
        lay = Layout(union_name, [], _align_up(size, max_align), max_align)
        self._union_layouts[union_name] = lay
        return lay

    def type_size_align(self, type_str):
        t = " ".join(type_str.split())
        if "*" in t:
            return (PTR_SIZE, PTR_SIZE)
        if t.startswith("struct "):
            sub = self.layout(t.split()[1])
            return (sub.size, sub.align)
        if t.startswith("union "):
            sub = self.union_layout(t.split()[1])
            return (sub.size, sub.align)
        if t in self.enum_typedefs:
            return (4, 4)  # a C enum without -fshort-enums is an int
        if t in C_PRIMS:
            return C_PRIMS[t]
        for prefix in ("const ", "volatile ", "struct "):
            if t.startswith(prefix):
                return self.type_size_align(t[len(prefix):])
        raise KeyError("unknown C type: %r" % type_str)

    def header_of(self, struct_name):
        """(include dir, include path) of the header declaring the struct."""
        path = self.structs[struct_name][0]
        parts = path.split(os.sep)
        for i in range(len(parts) - 1, -1, -1):
            if parts[i] == "include":
                return os.sep.join(parts[:i + 1]), os.sep.join(parts[i + 1:])
        return os.path.dirname(path), os.path.basename(path)


# ---------------------------------------------------------------------------
# Zig sources
# ---------------------------------------------------------------------------


def strip_zig_fns(body):
    """Remove `pub fn ... { ... }` members (methods) from a struct body."""
    out = body
    while True:
        m = re.search(r"\bpub\s+fn\b", out)
        if not m:
            break
        brace = out.find("{", m.end())
        if brace == -1:
            break
        depth = 0
        j = brace
        while j < len(out):
            if out[j] == "{":
                depth += 1
            elif out[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out = out[:m.start()] + " " + out[j + 1:]
    return out


def parse_zig_fields(body):
    """[(name, type)] for the members of a Zig `extern struct` body."""
    body = strip_comments(strip_zig_fns(body))
    fields = []
    depth = 0
    chunk = ""
    segments = []
    for ch in body:
        if ch in "{[(":
            depth += 1
        elif ch in "}])":
            depth -= 1
        if ch == "," and depth == 0:
            segments.append(chunk)
            chunk = ""
            continue
        chunk += ch
    segments.append(chunk)
    for segment in segments:
        seg = " ".join(segment.split())
        if not seg or ":" not in seg:
            continue
        name, _, type_str = seg.partition(":")
        name = name.strip()
        if not re.match(r"^[A-Za-z_]\w*$", name):
            continue
        fields.append((name, type_str.strip()))
    return fields


ZIG_STRUCT_RE = re.compile(
    r"pub\s+const\s+([A-Za-z_]\w*)\s*=\s*extern\s+struct\s*\{")
ZIG_ENUM_RE = re.compile(
    r"pub\s+const\s+([A-Za-z_]\w*)\s*=\s*enum\s*(?:\(([^)]*)\))?\s*\{")


class ZigFile(object):
    """The `extern struct` mirrors and enums declared in one Zig source."""

    def __init__(self, path):
        self.path = path
        self.text = _read(path)
        self.structs = []   # [(name, [(field, type)])]
        self.enums = {}     # name -> (tag type, [(member, value)])
        for m in ZIG_STRUCT_RE.finditer(self.text):
            body, _ = brace_body(self.text, m.end() - 1)
            self.structs.append((m.group(1), parse_zig_fields(body)))
        for m in ZIG_ENUM_RE.finditer(self.text):
            body, _ = brace_body(self.text, m.end() - 1)
            # doc comments live inside the enum bodies - drop them first
            body = strip_comments(body)
            self.enums[m.group(1)] = (m.group(2), parse_enum_members(body))

    def enum_size_align(self, name):
        tag, members = self.enums[name]
        if tag:
            return ZIG_PRIMS.get(tag.strip(), (4, 4))
        biggest = max([v for _n, v in members] or [0])
        for candidate in ("u8", "u16", "u32", "u64"):
            if biggest < (1 << (8 * int(candidate[1:]))):
                return ZIG_PRIMS[candidate]
        return ZIG_PRIMS["u64"]

    def enum_value(self, enum_name, member):
        _tag, members = self.enums[enum_name]
        for name, value in members:
            if name == member:
                return value
        return None


# ---------------------------------------------------------------------------
# comptime assertions
# ---------------------------------------------------------------------------

ASSERT_RE = re.compile(r"std\.debug\.assert\((?P<expr>.*?)\)\s*;", re.S)
RE_SIZE_EQ = re.compile(
    r"^@sizeOf\(\s*([A-Za-z_][\w.]*)\s*\)\s*==\s*@sizeOf\(\s*([A-Za-z_][\w.]*)\s*\)$")
RE_ALIGN_EQ = re.compile(
    r"^@alignOf\(\s*([A-Za-z_][\w.]*)\s*\)\s*==\s*@alignOf\(\s*([A-Za-z_][\w.]*)\s*\)$")
RE_OFFSET_EQ = re.compile(
    r"^@offsetOf\(\s*([A-Za-z_][\w.]*)\s*,\s*\"([A-Za-z_]\w*)\"\s*\)\s*==\s*"
    r"@offsetOf\(\s*([A-Za-z_][\w.]*)\s*,\s*\"([A-Za-z_]\w*)\"\s*\)$")
RE_SIZE_INT = re.compile(
    r"^@sizeOf\(\s*([A-Za-z_][\w.]*)\s*\)\s*==\s*(-?\d+)$")
RE_ENUM_CONST = re.compile(
    r"^@intFromEnum\(\s*([A-Za-z_]\w*)\.([A-Za-z_]\w*)\s*\)\s*==\s*c\.([A-Za-z_]\w*)$")
RE_ENUM_INT = re.compile(
    r"^@intFromEnum\(\s*([A-Za-z_]\w*)\.([A-Za-z_]\w*)\s*\)\s*==\s*(-?\d+)$")
RE_C_CONST_INT = re.compile(r"^c\.([A-Za-z_]\w*)\s*==\s*(-?\d+)$")


class MirrorChecker(object):
    """Compares one Zig `extern struct` mirror with the C struct it mirrors."""

    def __init__(self, zig_file, headers):
        self.zig = zig_file
        self.headers = headers
        self.assertions = self._collect_assertions()
        self.problems = []
        self.notes = []

    # -- assertions -------------------------------------------------------

    def _collect_assertions(self):
        out = []
        for m in ASSERT_RE.finditer(self.zig.text):
            expr = " ".join(m.group("expr").split())
            line = self.zig.text[:m.start()].count("\n") + 1
            out.append((line, expr))
        return out

    def _resolve_type(self, name):
        """(size, align, layout) of a Zig or C type named in an assertion."""
        if name.startswith("c."):
            rest = name[2:]
            if rest.startswith("struct_"):
                lay = self.headers.layout(rest[len("struct_"):])
                return (lay.size, lay.align, lay)
            if rest in self.headers.enum_typedefs:
                return (4, 4, None)
            if rest in C_PRIMS:
                size, align = C_PRIMS[rest]
                return (size, align, None)
            raise KeyError("unknown C type: %s" % name)
        for struct_name, fields in self.zig.structs:
            if struct_name == name:
                lay = self.zig_layout(struct_name, fields)
                return (lay.size, lay.align, lay)
        if name in self.zig.enums:
            size, align = self.zig.enum_size_align(name)
            return (size, align, None)
        raise KeyError("unknown Zig type: %s" % name)

    def _offset_of(self, type_name, field_name):
        _size, _align, lay = self._resolve_type(type_name)
        if lay is None:
            raise KeyError("no layout for %s" % type_name)
        field = lay.field(field_name)
        if field is None:
            raise KeyError("no field %r in %s" % (field_name, type_name))
        return field.offset

    # -- layouts ----------------------------------------------------------

    def zig_type_size_align(self, type_str):
        t = " ".join(type_str.split())
        if t.startswith("[*") or t.startswith("*"):
            return (PTR_SIZE, PTR_SIZE)
        if t in ZIG_PRIMS:
            return ZIG_PRIMS[t]
        if t.startswith("c.struct_"):
            lay = self.headers.layout(t[len("c.struct_"):])
            return (lay.size, lay.align)
        if t.startswith("c."):
            rest = t[2:]
            if rest in self.headers.enum_typedefs:
                return (4, 4)
            if rest in C_PRIMS:
                return C_PRIMS[rest]
        if t in self.zig.enums:
            return self.zig.enum_size_align(t)
        raise KeyError("unknown Zig type: %r" % type_str)

    def zig_layout(self, struct_name, fields):
        out = []
        offset = 0
        max_align = 1
        for field_name, type_str in fields:
            size, align = self.zig_type_size_align(type_str)
            offset = _align_up(offset, align)
            out.append(Field(field_name, type_str, size, align, offset))
            offset += size
            max_align = max(max_align, align)
        return Layout(struct_name, out, _align_up(offset, max_align), max_align)

    # -- the comparison ---------------------------------------------------

    def c_counterpart(self, struct_name):
        """The C struct a Zig mirror shadows, from the comptime assertions."""
        candidates = {}
        for _line, expr in self.assertions:
            types = []
            m = RE_OFFSET_EQ.match(expr)
            if m:
                types = [m.group(1), m.group(3)]
            else:
                m = RE_SIZE_EQ.match(expr) or RE_ALIGN_EQ.match(expr)
                if m:
                    types = [m.group(1), m.group(2)]
            if len(types) != 2:
                continue
            lhs, rhs = types
            if lhs != struct_name:
                continue
            if rhs.startswith("c.struct_"):
                c_name = rhs[len("c.struct_"):]
                candidates[c_name] = candidates.get(c_name, 0) + 1
        if not candidates:
            return None
        return sorted(candidates.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]

    def field_mapping(self, struct_name, zig_layout, c_layout):
        """{zig field: c field}, from the offset assertions plus name identity."""
        mapping = {}
        for _line, expr in self.assertions:
            m = RE_OFFSET_EQ.match(expr)
            if not m:
                continue
            lhs, lhs_field, rhs, rhs_field = m.groups()
            if lhs != struct_name:
                continue
            if rhs.startswith("c.struct_") and rhs[len("c.struct_"):] == c_layout.name:
                mapping[lhs_field] = rhs_field
        c_names = set(f.name for f in c_layout.fields)
        for f in zig_layout.fields:
            if f.name not in mapping and f.name in c_names:
                mapping[f.name] = f.name
        return mapping

    def check(self):
        for struct_name, fields in self.zig.structs:
            zig_layout = self.zig_layout(struct_name, fields)
            c_name = self.c_counterpart(struct_name)
            if c_name is None:
                self.notes.append(
                    "%s: no comptime assertion ties `%s` to a C struct - skipped"
                    % (os.path.relpath(self.zig.path, ROOT), struct_name))
                continue
            c_layout = self.headers.layout(c_name)
            where = "%s: Zig `%s` vs C `struct %s`" % (
                os.path.relpath(self.zig.path, ROOT), struct_name, c_name)
            if c_layout.size != zig_layout.size:
                self.problems.append(
                    "%s: size mismatch - C %d bytes, Zig %d bytes"
                    % (where, c_layout.size, zig_layout.size))
            if c_layout.align != zig_layout.align:
                self.problems.append(
                    "%s: alignment mismatch - C %d, Zig %d"
                    % (where, c_layout.align, zig_layout.align))
            mapping = self.field_mapping(struct_name, zig_layout, c_layout)
            c_fields = c_layout.fields
            z_fields = zig_layout.fields
            unmapped_c = [f.name for f in c_fields if f.name not in mapping.values()]
            unmapped_z = [f.name for f in z_fields if f.name not in mapping]
            if unmapped_c:
                self.problems.append(
                    "%s: C field(s) not mirrored in Zig: %s"
                    % (where, ", ".join(unmapped_c)))
            if unmapped_z:
                self.problems.append(
                    "%s: Zig field(s) with no C counterpart: %s"
                    % (where, ", ".join(unmapped_z)))
            for zf in z_fields:
                c_name_of = mapping.get(zf.name)
                if c_name_of is None:
                    continue
                cf = c_layout.field(c_name_of)
                if cf is None:
                    continue
                if cf.offset != zf.offset:
                    self.problems.append(
                        "%s: Zig `%s` sits at offset %d, C `%s` at offset %d"
                        % (where, zf.name, zf.offset, cf.name, cf.offset))
                if cf.size != zf.size or cf.align != zf.align:
                    self.problems.append(
                        "%s: Zig `%s` is %d bytes/align %d, C `%s` is %d bytes/"
                        "align %d - the mirror's field type is wrong"
                        % (where, zf.name, zf.size, zf.align,
                           cf.name, cf.size, cf.align))
            # the mirrors must list the fields in the same order
            c_offsets = c_layout.offsets()
            c_order = []
            for zf in z_fields:
                mapped = mapping.get(zf.name)
                if mapped is None or mapped not in c_offsets:
                    c_order = None
                    break
                c_order.append(c_offsets[mapped])
            if c_order is not None and c_order != sorted(c_order):
                self.problems.append(
                    "%s: the mirrored fields are not in the C declaration order"
                    % where)
        self.check_assertions()

    def check_assertions(self):
        for line, expr in self.assertions:
            ok, detail = self.eval_assertion(expr)
            if ok is None:
                self.notes.append(
                    "%s:%d: assertion not evaluated: %s"
                    % (os.path.relpath(self.zig.path, ROOT), line, expr))
                continue
            if not ok:
                self.problems.append(
                    "%s:%d: comptime assertion is false: %s (%s)"
                    % (os.path.relpath(self.zig.path, ROOT), line, expr, detail))

    def eval_assertion(self, expr):
        """(ok, detail) for a supported assertion form; (None, None) if not."""
        m = RE_SIZE_EQ.match(expr)
        if m:
            left, right = m.groups()
            l_size = self._resolve_type(left)[0]
            r_size = self._resolve_type(right)[0]
            return (l_size == r_size, "%s=%d vs %s=%d"
                    % (left, l_size, right, r_size))
        m = RE_ALIGN_EQ.match(expr)
        if m:
            left, right = m.groups()
            l_align = self._resolve_type(left)[1]
            r_align = self._resolve_type(right)[1]
            return (l_align == r_align, "%s=%d vs %s=%d"
                    % (left, l_align, right, r_align))
        m = RE_OFFSET_EQ.match(expr)
        if m:
            left, left_field, right, right_field = m.groups()
            l_off = self._offset_of(left, left_field)
            r_off = self._offset_of(right, right_field)
            return (l_off == r_off, "%s.%s=%d vs %s.%s=%d"
                    % (left, left_field, l_off, right, right_field, r_off))
        m = RE_SIZE_INT.match(expr)
        if m:
            name, expected = m.group(1), int(m.group(2))
            size = self._resolve_type(name)[0]
            return (size == expected, "%s=%d vs %d" % (name, size, expected))
        m = RE_ENUM_CONST.match(expr)
        if m:
            enum_name, member, const = m.groups()
            zig_value = self.zig.enum_value(enum_name, member)
            c_value = self.headers.constants.get(const)
            if zig_value is None or c_value is None:
                return (None, None)
            return (zig_value == c_value,
                    "%s.%s=%s vs c.%s=%s" % (enum_name, member, zig_value,
                                             const, c_value))
        m = RE_ENUM_INT.match(expr)
        if m:
            enum_name, member, expected = m.groups()
            zig_value = self.zig.enum_value(enum_name, member)
            if zig_value is None:
                return (None, None)
            return (zig_value == int(expected),
                    "%s.%s=%s vs %s" % (enum_name, member, zig_value, expected))
        m = RE_C_CONST_INT.match(expr)
        if m:
            const, expected = m.group(1), int(m.group(2))
            c_value = self.headers.constants.get(const)
            if c_value is None:
                return (None, None)
            return (c_value == expected, "c.%s=%s vs %s" % (const, c_value, expected))
        return (None, None)


# ---------------------------------------------------------------------------
# independent cross-checks
# ---------------------------------------------------------------------------


CTYPES_PRIMS = {
    "char": ctypes.c_char,
    "signed char": ctypes.c_byte,
    "unsigned char": ctypes.c_ubyte,
    "_Bool": ctypes.c_bool,
    "bool": ctypes.c_bool,
    "short": ctypes.c_short,
    "signed short": ctypes.c_short,
    "unsigned short": ctypes.c_ushort,
    "int": ctypes.c_int,
    "signed int": ctypes.c_int,
    "unsigned int": ctypes.c_uint,
    "unsigned": ctypes.c_uint,
    "int8_t": ctypes.c_int8,
    "uint8_t": ctypes.c_uint8,
    "int16_t": ctypes.c_int16,
    "uint16_t": ctypes.c_uint16,
    "int32_t": ctypes.c_int32,
    "uint32_t": ctypes.c_uint32,
    "int64_t": ctypes.c_int64,
    "uint64_t": ctypes.c_uint64,
    "long": ctypes.c_long,
    "unsigned long": ctypes.c_ulong,
    "long long": ctypes.c_longlong,
    "unsigned long long": ctypes.c_ulonglong,
    "size_t": ctypes.c_size_t,
    "ssize_t": ctypes.c_ssize_t,
    "ptrdiff_t": ctypes.c_ssize_t,
    "uintptr_t": ctypes.c_size_t,
    "intptr_t": ctypes.c_ssize_t,
    "float": ctypes.c_float,
    "double": ctypes.c_double,
}


def ctypes_type(headers, type_str, cache):
    """The `ctypes` spelling of a C type - the host C ABI, independently."""
    t = " ".join(type_str.split())
    if "*" in t:
        return ctypes.c_void_p
    if t.startswith("struct "):
        return ctypes_struct(headers, t.split()[1], cache)
    if t in headers.enum_typedefs:
        return ctypes.c_int
    if t in CTYPES_PRIMS:
        return CTYPES_PRIMS[t]
    for prefix in ("const ", "volatile ", "struct "):
        if t.startswith(prefix):
            return ctypes_type(headers, t[len(prefix):], cache)
    raise KeyError("unknown C type: %r" % type_str)


def ctypes_struct(headers, struct_name, cache):
    """A `ctypes.Structure` mirroring the C struct - the host C ABI."""
    if struct_name in cache:
        return cache[struct_name]
    members = []
    for type_str, name, dims in split_c_decls(headers.structs[struct_name][1]):
        ctype = ctypes_type(headers, type_str, cache)
        for dim in dims:
            ctype = ctype * dim
        members.append((name, ctype))

    class Struct(ctypes.Structure):
        _fields_ = members

    cache[struct_name] = Struct
    return Struct


def gcc_layout_facts(struct_names, headers):
    """sizeof/alignof/offsetof straight from the C compiler, or None."""
    cc = os.environ.get("CC", "gcc")
    if not shutil.which(cc):
        return None
    include_dirs = [d for d in (os.path.join(ROOT, "include"),
                                os.path.join(ROOT, "sim", "include"),
                                os.path.join(ROOT, "tests"),
                                os.path.join(ROOT, "tests", "host", "posix", "include"))
                    if os.path.isdir(d)]
    headers_used = []
    lines = ["#include <stdio.h>", "#include <stddef.h>"]
    for name in struct_names:
        inc_dir, rel = headers.header_of(name)
        header = '"%s"' % rel.replace(os.sep, "/")
        if (inc_dir, header) not in headers_used:
            headers_used.append((inc_dir, header))
    for _inc_dir, header in headers_used:
        lines.append("#include %s" % header)
    lines.append("int main(void) {")
    for name in struct_names:
        lines.append('  printf("size|%s=%%zu\\n", sizeof(struct %s));' % (name, name))
        lines.append('  printf("align|%s=%%zu\\n", _Alignof(struct %s));' % (name, name))
        for field in headers.layout(name).fields:
            lines.append('  printf("off|%s|%s=%%zu\\n", offsetof(struct %s, %s));'
                         % (name, field.name, name, field.name))
    lines.append("  return 0;")
    lines.append("}")
    tmpdir = tempfile.mkdtemp(prefix="zig_mirror_probe_")
    try:
        src = os.path.join(tmpdir, "probe.c")
        exe = os.path.join(tmpdir, "probe")
        with open(src, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        cmd = [cc]
        for inc_dir in include_dirs:
            cmd += ["-I", inc_dir]
        cmd += ["-o", exe, src]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            return None
        run = subprocess.run([exe], capture_output=True, text=True)
        if run.returncode != 0:
            return None
        facts = {}
        for line in run.stdout.splitlines():
            if "|" in line and "=" in line:
                key, value = line.split("=", 1)
                facts[key.strip()] = int(value.strip())
        return facts
    except OSError:
        return None
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


# ---------------------------------------------------------------------------
# the test
# ---------------------------------------------------------------------------


class ZigStructMirrorTest(unittest.TestCase):
    """Every hand-written Zig `extern struct` must match its C struct."""

    @classmethod
    def setUpClass(cls):
        cls.headers = CHeaders(HEADER_ROOTS)
        cls.zig_files = []
        if os.path.isdir(ZIG_SRC_DIR):
            for name in sorted(os.listdir(ZIG_SRC_DIR)):
                if name.endswith(".zig"):
                    cls.zig_files.append(ZigFile(os.path.join(ZIG_SRC_DIR, name)))

    def mirrored_structs(self):
        """[(zig file, zig mirror name, C struct name)] for every mirror."""
        out = []
        for zig_file in self.zig_files:
            checker = MirrorChecker(zig_file, self.headers)
            for struct_name, _fields in zig_file.structs:
                c_name = checker.c_counterpart(struct_name)
                if c_name:
                    out.append((zig_file, struct_name, c_name))
        return out

    def test_zig_extern_struct_mirrors_match_c(self):
        self.assertTrue(self.zig_files, "no Zig sources found under %s" % ZIG_SRC_DIR)
        problems = []
        notes = []
        checked = 0
        for zig_file in self.zig_files:
            checker = MirrorChecker(zig_file, self.headers)
            checker.check()
            problems.extend(checker.problems)
            notes.extend(checker.notes)
            checked += len(zig_file.structs)
        self.assertGreater(checked, 0, "no Zig `extern struct` mirror found")
        if problems:
            self.fail("Zig/C struct mirror drift:\n  " + "\n  ".join(problems))
        for note in notes:
            print("note: %s" % note)

    def test_c_layout_model_matches_ctypes(self):
        """The layout model used above must agree with the host C ABI."""
        problems = []
        cache = {}
        for _zig_file, _struct_name, c_name in self.mirrored_structs():
            lay = self.headers.layout(c_name)
            try:
                struct = ctypes_struct(self.headers, c_name, cache)
            except KeyError as exc:
                problems.append("struct %s: %s" % (c_name, exc))
                continue
            if ctypes.sizeof(struct) != lay.size:
                problems.append("sizeof(struct %s): ctypes %d, model %d"
                                % (c_name, ctypes.sizeof(struct), lay.size))
            if ctypes.alignment(struct) != lay.align:
                problems.append("alignof(struct %s): ctypes %d, model %d"
                                % (c_name, ctypes.alignment(struct), lay.align))
            for field in lay.fields:
                got = getattr(struct, field.name)
                offset = got.offset
                if offset != field.offset:
                    problems.append("offsetof(%s, %s): ctypes %d, model %d"
                                    % (c_name, field.name, offset, field.offset))
        self.assertEqual(problems, [], "\n".join(problems))

    def test_zig_mirror_layouts_agree_with_the_c_compiler(self):
        """The layout model used above must agree with the C compiler."""
        struct_names = []
        for zig_file in self.zig_files:
            checker = MirrorChecker(zig_file, self.headers)
            for struct_name, _fields in zig_file.structs:
                c_name = checker.c_counterpart(struct_name)
                if c_name and c_name not in struct_names:
                    struct_names.append(c_name)
        self.assertTrue(struct_names, "no C struct is mirrored by the Zig binding")
        facts = gcc_layout_facts(struct_names, self.headers)
        if facts is None:
            self.skipTest("no usable C compiler for the layout cross-check")
        problems = []
        for name in struct_names:
            lay = self.headers.layout(name)
            if facts.get("size|" + name) != lay.size:
                problems.append("sizeof(struct %s): compiler %s, model %d" %
                                (name, facts.get("size|" + name), lay.size))
            if facts.get("align|" + name) != lay.align:
                problems.append("alignof(struct %s): compiler %s, model %d" %
                                (name, facts.get("align|" + name), lay.align))
            for field in lay.fields:
                key = "off|%s|%s" % (name, field.name)
                if facts.get(key) != field.offset:
                    problems.append("offsetof(%s, %s): compiler %s, model %d" %
                                    (name, field.name, facts.get(key), field.offset))
        self.assertEqual(problems, [], "\n".join(problems))

    def test_zig_mirror_field_widths_match_the_c_struct(self):
        """Field widths (not just offsets) must match, so a wrong Zig type
        such as `u16` for a C `uint32_t` cannot slip through the padding."""
        problems = []
        for zig_file in self.zig_files:
            checker = MirrorChecker(zig_file, self.headers)
            for struct_name, fields in zig_file.structs:
                c_name = checker.c_counterpart(struct_name)
                if c_name is None:
                    continue
                zig_layout = checker.zig_layout(struct_name, fields)
                c_layout = self.headers.layout(c_name)
                mapping = checker.field_mapping(struct_name, zig_layout, c_layout)
                for zf in zig_layout.fields:
                    cf = c_layout.field(mapping.get(zf.name, ""))
                    if cf is None:
                        continue
                    if (zf.size, zf.align) != (cf.size, cf.align):
                        problems.append(
                            "%s.%s: Zig %d bytes/align %d vs C %s %d bytes/align %d"
                            % (struct_name, zf.name, zf.size, zf.align,
                               cf.name, cf.size, cf.align))
        self.assertEqual(problems, [], "\n".join(problems))


if __name__ == "__main__":
    unittest.main()
