# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""The sim drain thread must outrank every application thread.

The consolidated sim-debug pump (`sim/hal/sim_debug.c`) is the only reader of
the profiler and trace rings.  A busy application thread - the LVGL render
loop runs at `OVE_PRIO_HIGH` - starves the pump if the pump does not preempt
it, and the rings overflow.

Preemption needs a STRICTLY higher priority, not an equal one:

  * NuttX   - two equal-priority tasks do not preempt each other (the ready
              list is FIFO within a priority level, `sched.h`); with
              `CONFIG_RR_INTERVAL=200` they alternate in 200 ms slices, so
              the pump and the application split the CPU.
  * FreeRTOS- `configUSE_TIME_SLICING=1` makes equal-priority tasks share
              the processor 50/50, halving the drain rate and skewing the
              app CPU-share figures the dashboard reports.
  * Zephyr  - equal priorities do not preempt each other.

So the invariant this test pins down is:

    the drain thread's priority is STRICTLY above the priority of every
    application thread in the tree.

The test is static: it reads the priority the drain is created with out of
`sim/hal/sim_debug.c`, reads the priority of every application thread out of
`apps/`, resolves both to numbers on the portable scale
(`include/ove/thread.h`) and compares the numbers.  No build, no toolchain,
no network.
"""

import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

THREAD_H = os.path.join(ROOT, "include", "ove", "thread.h")
LXP_HOST_H = os.path.join(ROOT, "include", "ove", "lxp_host.h")
SIM_DEBUG_C = os.path.join(ROOT, "sim", "hal", "sim_debug.c")
APPS_DIR = os.path.join(ROOT, "apps")

DRAIN_THREAD_NAME = "sim_debug"

# The C thread-creation entry points, with the position of the thread name
# and of the priority inside the argument list.
C_CALLS = (
    ("ove_thread_create", 1),
    ("ove_thread_init", 2),
    ("OVE_THREAD_INIT_DEFINED", 2),
    ("OVE_THREAD_DEFINE_STATIC", 5),
)

# The C++ spellings: the priority is the third argument of the Thread
# constructor, whether it is built with make_unique or constructed directly.
CPP_CALLS = (
    re.compile(r"std::make_unique\s*<\s*ove::Thread\s*<[^<>]*>\s*>\s*\("),
    re.compile(r"(?<![:\w])ove::Thread\s*<[^<>]*>\s+[A-Za-z_]\w*\s*\("),
)

RUST_CALL = re.compile(r"\.priority\s*\(\s*([^()]*?)\s*\)")
ZIG_CALL = re.compile(r"\.priority\s*=\s*\.(\w+)")

RUST_PRIORITY_NAMES = {
    "Idle": "OVE_PRIO_IDLE",
    "Low": "OVE_PRIO_LOW",
    "BelowNormal": "OVE_PRIO_BELOW_NORMAL",
    "Normal": "OVE_PRIO_NORMAL",
    "AboveNormal": "OVE_PRIO_ABOVE_NORMAL",
    "High": "OVE_PRIO_HIGH",
    "Realtime": "OVE_PRIO_REALTIME",
    "Critical": "OVE_PRIO_CRITICAL",
}

ZIG_PRIORITY_NAMES = {
    "idle": "OVE_PRIO_IDLE",
    "low": "OVE_PRIO_LOW",
    "below_normal": "OVE_PRIO_BELOW_NORMAL",
    "normal": "OVE_PRIO_NORMAL",
    "above_normal": "OVE_PRIO_ABOVE_NORMAL",
    "high": "OVE_PRIO_HIGH",
    "realtime": "OVE_PRIO_REALTIME",
    "critical": "OVE_PRIO_CRITICAL",
}


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def strip_comments(text, block=True):
    """Remove C/C++ comments so a priority mentioned in prose is not read
    as a priority.  Line comments only, plus block comments when asked.

    The newlines are kept, so a line number computed on the result is the
    line number in the original file."""
    out = []
    i = 0
    n = len(text)
    in_block = False
    while i < n:
        if in_block:
            end = text.find("*/", i)
            if end < 0:
                out.append(text[i:])
                break
            in_block = False
            out.append("\n" * text.count("\n", i, end))
            i = end + 2
            continue
        if text.startswith("/*", i) and block:
            in_block = True
            i += 2
            continue
        if text.startswith("//", i):
            end = text.find("\n", i)
            if end < 0:
                break
            i = end
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def parse_scale():
    """The portable priority anchors, from the ove_prio_t enum."""
    text = read(THREAD_H)
    m = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*ove_prio_t\s*;", text, re.S)
    if not m:
        raise AssertionError("no ove_prio_t enum in include/ove/thread.h")
    scale = {}
    for name, value in re.findall(r"(OVE_PRIO_\w+)\s*=\s*(\d+)", m.group(1)):
        scale[name] = int(value)
    if "OVE_PRIO_MAX" not in scale:
        scale["OVE_PRIO_MAX"] = scale["OVE_PRIO_CRITICAL"]
    if "OVE_PRIO_IDLE" not in scale:
        scale["OVE_PRIO_IDLE"] = 0
    return scale


def parse_defines(text):
    """The single-line #define aliases of a translation unit."""
    defines = {}
    for m in re.finditer(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s+([^\n/]+)$",
                         text, re.M):
        name, value = m.group(1), m.group(2).strip()
        if "(" in name:
            continue
        defines[name] = value
    return defines


def global_defines():
    """The portable aliases every translation unit can see."""
    defines = {}
    for path in (THREAD_H, LXP_HOST_H):
        if os.path.exists(path):
            defines.update(parse_defines(strip_comments(read(path))))
    return defines


def resolve(expr, scale, defines, depth=0):
    """The portable value of a priority expression, or None.

    Handles the enum anchors, the OVE_PRIO_ABOVE()/OVE_PRIO_BELOW()
    derivation macros and the #define aliases of the translation unit."""
    if depth > 16 or expr is None:
        return None
    e = expr.strip().rstrip(",")
    if not e:
        return None
    m = re.fullmatch(r"OVE_PRIO_ABOVE\s*\(\s*(.+?)\s*,\s*(\d+)\s*\)", e)
    if m:
        base = resolve(m.group(1), scale, defines, depth + 1)
        if base is None:
            return None
        return min(base + int(m.group(2)), scale["OVE_PRIO_MAX"])
    m = re.fullmatch(r"OVE_PRIO_BELOW\s*\(\s*(.+?)\s*,\s*(\d+)\s*\)", e)
    if m:
        base = resolve(m.group(1), scale, defines, depth + 1)
        if base is None:
            return None
        return max(base - int(m.group(2)), scale["OVE_PRIO_IDLE"])
    if e in scale:
        return scale[e]
    if e in defines:
        return resolve(defines[e], scale, defines, depth + 1)
    return None


def split_args(text, open_paren):
    """The top-level arguments of the call whose '(' is at @open_paren."""
    assert text[open_paren] == "("
    depth = 0
    args = []
    start = open_paren + 1
    i = open_paren
    n = len(text)
    in_str = None
    while i < n:
        c = text[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == in_str:
                in_str = None
            i += 1
            continue
        if c in "\"'":
            in_str = c
            i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(text[start:i].strip())
                return args
        elif c == "," and depth == 1:
            args.append(text[start:i].strip())
            start = i + 1
        i += 1
    raise AssertionError("unbalanced parenthesis")


def priority_in_args(args, scale, defines):
    """The one priority argument of a call, or None.

    Exactly one argument of a thread-creation call is a priority, so the
    call is skipped when none or more than one of them resolves - the
    test then fails loudly instead of silently checking nothing."""
    found = [a for a in args if resolve(a, scale, defines) is not None]
    if len(found) == 1:
        return found[0]
    return None


def c_sites(text):
    """(call name, line, arguments) of every C/C++ thread creation."""
    text = strip_comments(text)
    sites = []
    for name, name_index in C_CALLS:
        for m in re.finditer(r"(?<![A-Za-z0-9_])" + name + r"\s*\(", text):
            args = split_args(text, m.end() - 1)
            if name_index >= len(args):
                continue
            if not args[name_index].startswith('"'):
                continue
            sites.append((name, text.count("\n", 0, m.start()) + 1, args))
    for pattern in CPP_CALLS:
        for m in pattern.finditer(text):
            args = split_args(text, m.end() - 1)
            sites.append(("ove::Thread", text.count("\n", 0, m.start()) + 1, args))
    return sites


def rust_sites(text):
    text = strip_comments(text, block=False)
    sites = []
    for m in RUST_CALL.finditer(text):
        expr = m.group(1).strip()
        name = expr.rsplit("::", 1)[-1]
        mapped = RUST_PRIORITY_NAMES.get(name)
        sites.append(("rust", text.count("\n", 0, m.start()) + 1,
                      [mapped if mapped else expr]))
    return sites


def zig_sites(text):
    text = strip_comments(text, block=False)
    sites = []
    for m in ZIG_CALL.finditer(text):
        name = m.group(1)
        mapped = ZIG_PRIORITY_NAMES.get(name)
        sites.append(("zig", text.count("\n", 0, m.start()) + 1,
                      [mapped if mapped else name]))
    return sites


def app_sources():
    for dirpath, dirnames, filenames in os.walk(APPS_DIR):
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", "target")]
        for name in sorted(filenames):
            if name.endswith((".c", ".h", ".cpp", ".hpp", ".rs", ".zig")):
                yield os.path.join(dirpath, name)


def drain_creation_sites(scale, defines):
    """The priority the drain thread is created with, at every site."""
    text = read(SIM_DEBUG_C)
    local = dict(defines)
    local.update(parse_defines(strip_comments(text)))
    out = []
    for name, name_index in C_CALLS:
        if name not in ("ove_thread_create", "ove_thread_init"):
            continue
        for m in re.finditer(r"(?<![A-Za-z0-9_])" + name + r"\s*\(", text):
            args = split_args(text, m.end() - 1)
            if name_index >= len(args):
                continue
            if args[name_index] != '"%s"' % DRAIN_THREAD_NAME:
                continue
            prio = priority_in_args(args, scale, local)
            out.append((name, prio, text.count("\n", 0, m.start()) + 1, args))
    return out


class SimDrainPriorityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scale = parse_scale()
        cls.defines = global_defines()
        cls.drain = drain_creation_sites(cls.scale, cls.defines)

    # --- the drain thread itself ----------------------------------------

    def test_drain_thread_has_creation_sites(self):
        """Both creation paths exist: the heap one and the zero-heap one.

        A drain that is only created on one of the two paths would run at
        the wrong priority on the other."""
        names = [name for name, _prio, _line, _args in self.drain]
        self.assertIn("ove_thread_create", names,
                      "the heap path does not create the drain thread")
        self.assertIn("ove_thread_init", names,
                      "the zero-heap path does not create the drain thread")

    def test_every_drain_creation_site_uses_a_resolvable_priority(self):
        """Every creation site names a priority the scale can resolve.

        A raw number, or an unknown alias, would silently escape the
        invariant check below."""
        for name, prio, line, _args in self.drain:
            self.assertIsNotNone(
                prio,
                "%s at sim/hal/sim_debug.c:%d does not name a resolvable "
                "priority" % (name, line))

    def test_all_drain_creation_sites_use_the_same_priority(self):
        """The heap and the zero-heap path must not drift apart."""
        values = {(prio, resolve(prio, self.scale, self.defines))
                  for _name, prio, _line, _args in self.drain}
        self.assertEqual(len(values), 1,
                         "the drain thread is created at different priorities: %s"
                         % sorted(str(v) for v in values))

    def test_drain_priority_is_a_named_portable_anchor(self):
        """The drain runs at a named anchor, not at a hand-picked number."""
        for _name, prio, line, _args in self.drain:
            self.assertIn(prio, self.scale,
                          "the drain priority %r at sim/hal/sim_debug.c:%d is "
                          "not a portable anchor" % (prio, line))

    # --- the invariant ---------------------------------------------------

    def test_drain_is_strictly_above_every_application_thread(self):
        """The core invariant: drain > every application thread.

        A starvation test would assert this against the native priorities;
        on the portable scale the relation is the same, because every
        backend maps the scale monotonically."""
        drain_value = resolve(self.drain[0][1], self.scale, self.defines)
        self.assertIsNotNone(drain_value)

        offenders = []
        checked = 0
        for path in app_sources():
            text = read(path)
            rel = os.path.relpath(path, ROOT)
            if path.endswith(".rs"):
                sites = rust_sites(text)
            elif path.endswith(".zig"):
                sites = zig_sites(text)
            else:
                sites = c_sites(text)
            local = dict(self.defines)
            local.update(parse_defines(strip_comments(text)))
            for name, line, args in sites:
                prio = priority_in_args(args, self.scale, local)
                value = None
                if prio is not None:
                    value = resolve(prio, self.scale, local)
                if value is None:
                    offenders.append(
                        "%s:%d: %s has no resolvable priority (%s) - the "
                        "invariant cannot be checked here"
                        % (rel, line, name, ", ".join(args)))
                    continue
                checked += 1
                if not drain_value > value:
                    offenders.append(
                        "%s:%d: %s runs at %d (%s), the drain runs at %d"
                        % (rel, line, name, value, prio, drain_value))
        self.assertGreater(checked, 0, "no application thread was found")
        self.assertEqual(offenders, [], "\n".join(offenders))

    def test_drain_is_strictly_above_the_highest_application_anchor(self):
        """The drain outranks OVE_PRIO_HIGH, the band the LVGL graphics
        threads use.  This keeps the invariant anchored even if the
        application tree changes."""
        drain_value = resolve(self.drain[0][1], self.scale, self.defines)
        self.assertGreater(
            drain_value, self.scale["OVE_PRIO_HIGH"],
            "the drain runs at %d, not above OVE_PRIO_HIGH (%d)"
            % (drain_value, self.scale["OVE_PRIO_HIGH"]))


if __name__ == "__main__":
    unittest.main()
