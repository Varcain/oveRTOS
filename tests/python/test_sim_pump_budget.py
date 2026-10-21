# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""The sim pump's per-iteration event budget must be DERIVED
from the shared-memory event ring, the producers that share the ring must
fit into it together, and the profiler drain must keep up with the SAMPLER,
not merely with the ring.

The consolidated sim-debug pump (`sim/hal/sim_debug.c`) is the only writer
of the shared-memory event ring (`sim/include/ove/sim/ove_sim_shm.h`).  Two
of its drains write to that ring: the profiler sample drain
(`sim/hal/sim_profiler.c`) and the trace stream drain (`sim/hal/sim_trace.c`).
An unbounded drain loop turns a producer backlog into a burst that does not
fit in the ring, and the transport's backpressure then DROPS the events
instead of deferring them - the backlog is destroyed rather than delayed.

The fix bounds both loops with a per-pump-iteration batch cap that is
derived from the ring size, and it splits the ring between the producers
that can land in the same pump iteration:

  * every drain must derive its cap from `SIM_SHM_RING_SIZE` (a magic
    number is not a derivation - it breaks the moment the ring grows),
  * the ring is SHARED: the profiler drain, the trace drain, the trace's
    own descriptor/name events and the debug plugin's thread/heap
    snapshot can all land in one pump iteration, and their worst cases
    must fit in the ring together,
  * the drain loop must actually be bounded by that cap,
  * the profiler's capabilities replies must be coalesced into the one
    event the budget reserves, however many rate commands arrive,
  * the profiler drain must keep up with the sampling rate, i.e. it must
    move at least as many samples per drain period as the sampler
    produces - otherwise the profiler ring fills up, its own drop
    counter starts reporting lost samples, and the backlog is destroyed
    instead of deferred, which is the defect this budget is meant to
    fix.  The NuttX wall-clock backend samples EVERY registered thread
    once per period, so the producer rate is `hz * threads`, not `hz`;
    the drain covers the single-sample-per-period backends statically
    and a rate guard keeps the per-thread backend inside the transport's
    capacity.

The test is static: it reads the C sources and the headers they use and
recomputes the arithmetic for the default configuration
(`CONFIG_OVE_PROFILER_MAX_DEPTH` = 32) on the Cortex-M target (4-byte
pointers), and for the deepest stack the Kconfig allows (64).  No build,
no toolchain, no network.
"""

import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SHM_H = os.path.join(ROOT, "sim", "include", "ove", "sim", "ove_sim_shm.h")
BUDGET_H = os.path.join(ROOT, "sim", "include", "ove", "sim", "ove_sim_budget.h")
PLUGIN_H = os.path.join(ROOT, "sim", "include", "ove", "sim", "ove_sim_plugin.h")
PROFILER_H = os.path.join(ROOT, "sim", "include", "ove", "sim", "ove_sim_profiler.h")
TRACE_H = os.path.join(ROOT, "sim", "include", "ove", "sim", "ove_sim_trace.h")
DEBUG_H = os.path.join(ROOT, "sim", "include", "ove", "sim", "ove_sim_debug.h")
PROFILER_RING_H = os.path.join(ROOT, "backends", "common", "ove_profiler_ring.h")
TRACE_RING_H = os.path.join(ROOT, "backends", "common", "ove_trace_ring.h")
CONFIG_DEBUG = os.path.join(ROOT, "config", "Config.in.debug")
SIM_DEBUG_C = os.path.join(ROOT, "sim", "hal", "sim_debug.c")
NUTTX_PROFILER_C = os.path.join(ROOT, "backends", "nuttx", "nuttx_profiler.c")

# The two files under test.  The environment overrides exist so the test
# can be pointed at a pre-fix copy (e.g. `git show HEAD:<path>`) without
# touching the working tree.
PROFILER_C = os.environ.get(
    "OVE_SIM_PROFILER_C", os.path.join(ROOT, "sim", "hal", "sim_profiler.c")
)
TRACE_C = os.environ.get(
    "OVE_SIM_TRACE_C", os.path.join(ROOT, "sim", "hal", "sim_trace.c")
)

# The target is Cortex-M: a pointer (and therefore a PC on the wire) is
# 4 bytes wide.  The C code derives the wire width from sizeof(uintptr_t).
PC_WIRE_BYTES = 4

# The two-byte length prefix that precedes every event in the ring.
LENGTH_PREFIX_BYTES = 2


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def _defines(text):
    """Every `#define` of a C header/source as (name, params, body).

    `params` is None for an object-like macro, the parameter list for a
    function-like one.
    """
    pattern = re.compile(
        r"^#define\s+([A-Za-z_]\w*)(\(([^)]*)\))?[ \t]*(.*(?:\n[ \t]+.*)*)", re.M
    )
    for m in pattern.finditer(text):
        name = m.group(1)
        params = m.group(3)
        body = m.group(4)
        body = body.replace("\\", " ")                   # line continuation
        body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)   # block comment
        body = re.sub(r"//[^\n]*", " ", body)               # line comment
        body = " ".join(body.split())
        if params is not None:
            params = [p.strip() for p in params.split(",") if p.strip()]
        yield name, params, body


def macros(path):
    """Every object-like `#define` of a C header/source as {name: definition}."""
    out = {}
    for name, params, body in _defines(read(path)):
        if params is None:
            out[name] = body
    return out


def func_macros(path):
    """Every function-like `#define` as {name: (params, body)}."""
    out = {}
    for name, params, body in _defines(read(path)):
        if params is not None:
            out[name] = (params, body)
    return out


def c_int(expr):
    """Evaluate a C integer constant expression (no identifiers)."""
    expr = expr.strip()
    expr = re.sub(r"(\d)[uUlL]+\b", r"\1", expr)
    if not re.fullmatch(r"[0-9\s()+\-*/%<>|&^~]+", expr):
        raise ValueError("not a constant expression: %r" % expr)
    return int(eval(expr, {"__builtins__": {}}, {}))


# The sizeof() forms the budget macros use, and where their value comes
# from: the event header is four uint32_t, the trace record is 24 bytes
# (asserted by sim_trace.c itself) and a pointer is 4 bytes on Cortex-M.
SIZEOF_FORMS = (
    (r"sizeof\s*\(\s*struct\s+ove_sim_event\s*\)", "sizeof(struct ove_sim_event)"),
    (r"sizeof\s*\(\s*struct\s+ove_trace_record\s*\)", "sizeof(struct ove_trace_record)"),
    (r"sizeof\s*\(\s*uintptr_t\s*\)", "sizeof(uintptr_t)"),
)


def eval_expr(expr, table, base):
    """Evaluate a C integer constant expression that may use macros.

    `table` holds the macro definitions (object-like), `base` the leaf
    constants that come from headers (ring size, event size, ...) or
    from the target's data model (sizeof).
    """
    # Expand the macros first: the sizeof() forms live inside the macro
    # definitions, not in the expression that names them.
    names = sorted((n for n in table if n), key=len, reverse=True)
    for _ in range(16):
        before = expr
        for other in names:
            expr = re.sub(r"\b%s\b" % re.escape(other), "(" + table[other] + ")", expr)
        if expr == before:
            break
    # sizeof() next: its argument is not an identifier.
    for pattern, key in SIZEOF_FORMS:
        if key in base:
            expr = re.sub(pattern, str(base[key]), expr)
    expr = re.sub(r"\(\s*unsigned\s*(?:int|long)?\s*\)", "", expr)
    for other, value in base.items():
        if not other.startswith("sizeof("):
            expr = re.sub(r"\b%s\b" % re.escape(other), str(value), expr)
    expr = re.sub(r"(\d)[uUlL]+\b", r"\1", expr)
    # C integer division truncates; Python's "/" does not.  Protect the
    # shift operators, which contain a slash too.
    expr = expr.replace("<<", "\x01").replace(">>", "\x02")
    expr = expr.replace("/", "//")
    expr = expr.replace("\x01", "<<").replace("\x02", ">>")
    if not re.fullmatch(r"[0-9\s()+\-*/%<>|&^~]+", expr):
        raise ValueError("cannot evaluate %r" % expr)
    return int(eval(expr, {"__builtins__": {}}, {}))


def eval_macro(name, table, base):
    """Evaluate a C macro from `table`, resolving other macros recursively."""
    if name in base:
        return base[name]
    if name not in table:
        raise KeyError(name)
    return eval_expr(table[name], table, base)


def call_macro(name, args, table, base, funcs):
    """Evaluate a function-like macro with numeric arguments."""
    params, body = funcs[name]
    if len(params) != len(args):
        raise ValueError("%s takes %d arguments, %d given" % (name, len(params), len(args)))
    expr = body
    for param, arg in zip(params, args):
        expr = re.sub(r"\b%s\b" % re.escape(param), "(" + str(arg) + ")", expr)
    return eval_expr(expr, table, base)


def strip_comments(src):
    """C source without its comments."""
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def function_body(src, func):
    """The body of a C function, braces balanced."""
    m = re.search(r"^[A-Za-z_][^\n;]*\b%s\s*\([^)]*\)\s*\{" % re.escape(func), src, re.M)
    if not m:
        return None
    i = src.index("{", m.end() - 1)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[m.start(): j + 1]
    return None


def struct_ove_sim_event_bytes(path):
    """sizeof(struct ove_sim_event): four uint32_t members, no padding."""
    text = read(path)
    m = re.search(r"struct\s+ove_sim_event\s*\{(.*?)\}", text, re.S)
    if not m:
        raise AssertionError("struct ove_sim_event not found in %s" % path)
    body = m.group(1)
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    members = [line for line in body.split(";") if line.strip()]
    total = 0
    for member in members:
        member = " ".join(member.split())
        if member.startswith("uint8_t data"):
            continue  # flexible array member: not part of sizeof()
        if member.startswith("uint32_t"):
            total += 4
        else:
            raise AssertionError("unexpected member in struct ove_sim_event: %r" % member)
    return total


class SimPumpBudgetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.shm = macros(SHM_H)
        cls.budget = macros(BUDGET_H)
        cls.plugin = macros(PLUGIN_H)
        cls.profiler_h = macros(PROFILER_H)
        cls.trace_h = macros(TRACE_H)
        cls.debug_h = macros(DEBUG_H)
        cls.profiler_ring = macros(PROFILER_RING_H)
        cls.trace_ring = macros(TRACE_RING_H)
        cls.debug_c = macros(SIM_DEBUG_C)
        cls.nuttx_profiler = macros(NUTTX_PROFILER_C)

        cls.ring_size = c_int(cls.shm["SIM_SHM_RING_SIZE"])
        cls.event_max = c_int(cls.plugin["OVE_SIM_EVENT_MAX_BYTES"])
        cls.event_hdr = struct_ove_sim_event_bytes(PLUGIN_H)
        cls.depth = c_int(cls.profiler_ring["CONFIG_OVE_PROFILER_MAX_DEPTH"])
        cls.trace_batch = c_int(cls.trace_h["OVE_SIM_TRACE_MAX_BATCH"])
        cls.record_bytes = 24  # asserted by sim_trace.c itself
        cls.drain_ms = c_int(cls.profiler_h["OVE_SIM_PROFILER_DRAIN_MS"])
        cls.trace_drain_ms = c_int(cls.trace_h["OVE_SIM_TRACE_DRAIN_MS"])
        cls.pump_tick_ms = c_int(cls.debug_c["PUMP_TICK_MS"])

        # The Kconfig ceiling of the sampling rate and the Kconfig
        # defaults of the two ring capacities.
        text = read(CONFIG_DEBUG)
        m = re.search(r"config\s+OVE_PROFILER_HZ\b.*?range\s+\d+\s+(\d+)", text, re.S)
        cls.max_hz = int(m.group(1)) if m else 1000

        # The Kconfig ceiling of the stack depth.
        m = re.search(r"config\s+OVE_PROFILER_MAX_DEPTH\b.*?range\s+\d+\s+(\d+)", text, re.S)
        cls.max_depth = int(m.group(1)) if m else 64

        m = re.search(r"config\s+OVE_TRACE_RING_SIZE\b.*?default\s+(\d+)", text, re.S)
        cls.trace_ring_size = int(m.group(1)) if m else 4096

        # The number of threads the NuttX wall-clock backend walks once
        # per sampling period (backends/nuttx/nuttx_profiler.c).
        cls.nuttx_threads = c_int(cls.nuttx_profiler["PROFILER_MAX_THREADS"])

    # -- helpers --------------------------------------------------------

    def base(self, depth=None):
        """The leaf constants the budget macros are evaluated against."""
        return {
            "SIM_SHM_RING_SIZE": self.ring_size,
            "OVE_SIM_EVENT_MAX_BYTES": self.event_max,
            "sizeof(struct ove_sim_event)": self.event_hdr,
            "sizeof(uintptr_t)": PC_WIRE_BYTES,
            "sizeof(struct ove_trace_record)": self.record_bytes,
            "CONFIG_OVE_PROFILER_MAX_DEPTH": self.depth if depth is None else depth,
            "OVE_SIM_TRACE_MAX_BATCH": self.trace_batch,
            "OVE_TRACE_NAME_MAX": c_int(self.trace_ring["OVE_TRACE_NAME_MAX"]),
            "CONFIG_OVE_PROFILER_HZ": self.max_hz,
            "CONFIG_OVE_TRACE_RING_SIZE": self.trace_ring_size,
            "OVE_SIM_DEBUG_MAX_THREADS": c_int(self.debug_h["OVE_SIM_DEBUG_MAX_THREADS"]),
            "OVE_SIM_DEBUG_MAX_NAME_LEN": c_int(self.debug_h["OVE_SIM_DEBUG_MAX_NAME_LEN"]),
        }

    def drain_macros(self, path, prefix, depth=None):
        """The budget macros of one drain, and their values.

        The shared budget header is merged into the macro table: the
        split of the ring between the producers lives there.  The two
        plugin headers are merged as well, because the budget macros
        are expressed in terms of their limits.
        """
        table = dict(macros(path))
        table.update(self.budget)
        table.update(self.profiler_h)
        table.update(self.trace_h)
        base = self.base(depth)
        budget = "%s_EVENT_BUDGET_BYTES" % prefix
        per_event = "%s_EVENT_RING_BYTES" % prefix
        cap = "%s_MAX_BATCHES_PER_TICK" % prefix
        for name in (budget, per_event, cap):
            self.assertIn(name, table, "%s does not define %s" % (path, name))
        return table, base, budget, per_event, cap

    def trace_meta_bytes(self):
        """Bytes the trace's descriptor and name events take in the ring.

        Recomputed from the wire-format limits (sim/include/ove/sim/
        ove_sim_trace.h and backends/common/ove_trace_ring.h) rather
        than from the drain's own macros, so that a tree which does not
        charge them to the trace's share is measured, not excused.
        """
        desc_per = 4 + 4 + 1 + c_int(self.trace_h["OVE_SIM_TRACE_MAX_NAME"])
        desc_buf = 8 + c_int(self.trace_h["OVE_SIM_TRACE_MAX_DESC"]) * desc_per
        name_per = 2 + 1 + c_int(self.trace_h["OVE_SIM_TRACE_MAX_NAME"])
        names_buf = 8 + c_int(self.trace_ring["OVE_TRACE_NAME_MAX"]) * name_per
        overhead = LENGTH_PREFIX_BYTES + self.event_hdr
        return overhead + desc_buf + overhead + names_buf

    def assert_bounded_loop(self, src, func, cap, path):
        body = function_body(src, func)
        self.assertIsNotNone(body, "%s: cannot find %s()" % (path, func))
        m = re.search(r"while\s*\(([^)]*)\)\s*;", body)
        self.assertIsNotNone(m, "%s: %s() has no drain loop" % (path, func))
        cond = " ".join(m.group(1).split())
        self.assertIn(cap, cond,
                      "%s: the drain loop is not bounded by %s (condition: %s)"
                      % (path, cap, cond))
        # The cap must be a counter compared against a limit, not the
        # "the ring was full" test that made the loop unbounded.
        self.assertRegex(cond, r"[<>]=?\s*" + re.escape(cap),
                         "%s: the drain loop does not compare against %s"
                         % (path, cap))

    # -- the tests ------------------------------------------------------

    def test_ring_size_is_the_real_event_ring(self):
        """SIM_SHM_RING_SIZE is the 64 KiB event ring, not the whole shm."""
        self.assertEqual(self.ring_size, 65536)
        # The event ring and the command ring are two separate rings of the
        # same size; the budget must be derived from the event ring.
        event_off = eval_macro("SIM_SHM_EVENT_RING_OFF", self.shm, {})
        cmd_off = eval_macro("SIM_SHM_CMD_RING_OFF", self.shm, {})
        self.assertEqual(event_off + self.ring_size, cmd_off)

    def test_the_ring_is_split_between_the_pump_producers(self):
        """The shares of the ring must add up - and leave room for the
        debug plugin's snapshot, which is emitted from the same pump
        iteration."""
        base = self.base()
        ring = eval_macro("SIM_EVENT_RING_BUDGET_BYTES", self.budget, base)
        reserve = eval_macro("SIM_EVENT_SNAPSHOT_RESERVE_BYTES", self.budget, base)
        drain = eval_macro("SIM_EVENT_DRAIN_BUDGET_BYTES", self.budget, base)
        prof = eval_macro("SIM_EVENT_PROFILER_BUDGET_BYTES", self.budget, base)
        trace = eval_macro("SIM_EVENT_TRACE_BUDGET_BYTES", self.budget, base)

        self.assertEqual(ring, self.ring_size,
                         "the budget must be the event ring itself")
        self.assertEqual(drain + reserve, ring,
                         "the drain budget and the snapshot reserve must "
                         "account for the whole ring")
        self.assertEqual(prof + trace, drain,
                         "the two drains' shares must account for the whole "
                         "drain budget")
        self.assertGreater(prof, 0)
        self.assertGreater(trace, 0)

        # The reserve must cover the snapshot the debug plugin emits.
        snapshot = eval_macro("SNAPSHOT_BUF_SIZE", self.debug_c, base)
        snapshot_event = LENGTH_PREFIX_BYTES + self.event_hdr + snapshot
        self.assertLessEqual(snapshot_event, reserve,
                             "the reserved %d B must cover the %d B the debug "
                             "plugin's snapshot can take"
                             % (reserve, snapshot_event))

    def test_profiler_budget_is_derived_from_the_ring(self):
        path = PROFILER_C
        table, base, budget, per_event, cap = self.drain_macros(path, "PROFILE")

        # Derived, not guessed: the budget comes from the shared split of
        # the ring, which is derived from SIM_SHM_RING_SIZE.
        self.assertIn("SIM_EVENT_PROFILER_BUDGET_BYTES", table[budget],
                      "%s: %s does not come from the shared ring split (%s)"
                      % (path, budget, table[budget]))
        share = eval_macro("SIM_EVENT_PROFILER_BUDGET_BYTES", self.budget, base)
        self.assertLessEqual(share, self.ring_size)

        # ... and the cap is that budget divided by the worst case of one
        # of this file's own batch events.
        self.assertIn(budget, table[cap])
        self.assertIn(per_event, table[cap])
        self.assertIn("PROFILE_BUF_BYTES", table[per_event])

        cap_value = eval_macro(cap, table, base)
        budget_value = eval_macro(budget, table, base)
        event_bytes = eval_macro(per_event, table, base)

        self.assertGreaterEqual(cap_value, 1,
                                "%s: the cap must allow at least one batch" % path)
        self.assertLessEqual(cap_value * event_bytes, budget_value,
                             "%s: one pump iteration may emit %d B, more than "
                             "the %d B budget"
                             % (path, cap_value * event_bytes, budget_value))
        # The cap must be the tightest fit: one more batch would not fit.
        self.assertGreater((cap_value + 1) * event_bytes, budget_value,
                           "%s: the cap is looser than the budget" % path)
        self.assert_bounded_loop(read(path), "ove_sim_profiler_tick", cap, path)

    def test_trace_budget_is_derived_from_the_ring(self):
        path = TRACE_C
        table, base, budget, per_event, cap = self.drain_macros(path, "TRACE")

        self.assertIn("SIM_EVENT_TRACE_BUDGET_BYTES", table[budget],
                      "%s: %s does not come from the shared ring split (%s)"
                      % (path, budget, table[budget]))
        self.assertIn(budget, table[cap])
        self.assertIn(per_event, table[cap])
        self.assertIn("STREAM_BUF_BYTES", table[per_event])

        cap_value = eval_macro(cap, table, base)
        budget_value = eval_macro(budget, table, base)
        event_bytes = eval_macro(per_event, table, base)

        self.assertGreaterEqual(cap_value, 1,
                                "%s: the cap must allow at least one batch" % path)
        self.assertLessEqual(cap_value * event_bytes, budget_value,
                             "%s: one pump iteration may emit %d B, more than "
                             "the %d B budget"
                             % (path, cap_value * event_bytes, budget_value))
        self.assertGreater((cap_value + 1) * event_bytes, budget_value,
                           "%s: the cap is looser than the budget" % path)
        self.assert_bounded_loop(read(path), "ove_sim_trace_tick", cap, path)

    def test_the_trace_charges_its_own_events_to_its_share(self):
        """The descriptor and name events are emitted from the same tick
        as the stream batches: they must be paid for out of the trace's
        share, not out of the ring's slack."""
        path = TRACE_C
        table, base, budget, per_event, cap = self.drain_macros(path, "TRACE")

        self.assertIn("TRACE_META_EVENT_BYTES", table,
                      "%s: the trace does not charge its descriptor and name "
                      "events to its share" % path)
        share = eval_macro("SIM_EVENT_TRACE_BUDGET_BYTES", self.budget, base)
        meta = eval_macro("TRACE_META_EVENT_BYTES", table, base)
        cap_value = eval_macro(cap, table, base)
        event_bytes = eval_macro(per_event, table, base)

        self.assertLessEqual(meta, share,
                             "the trace's own events (%d B) do not fit in its "
                             "share (%d B)" % (meta, share))
        self.assertLessEqual(cap_value * event_bytes + meta, share,
                             "the trace's worst case exceeds its share")
        # The descriptor event is always emitted; the name table only
        # with CONFIG_OVE_TRACE_MARKERS.
        desc = eval_macro("TRACE_DESC_EVENT_BYTES", table, base)
        self.assertGreaterEqual(desc, LENGTH_PREFIX_BYTES + self.event_hdr)

    def test_the_two_drains_fit_together(self):
        """Profiler + trace + the trace's own events + the debug snapshot
        in one pump iteration must fit in the ring."""
        p_table, p_base, p_budget, p_event, p_cap = self.drain_macros(PROFILER_C, "PROFILE")
        t_table, t_base, t_budget, t_event, t_cap = self.drain_macros(TRACE_C, "TRACE")

        prof_bytes = eval_macro(p_cap, p_table, p_base) * eval_macro(
            p_event, p_table, p_base)
        trace_bytes = eval_macro(t_cap, t_table, t_base) * eval_macro(
            t_event, t_table, t_base)
        # The trace's own descriptor and name events, recomputed from the
        # wire-format limits so that a tree that does not charge them to
        # the trace's share is measured, not excused.
        meta_bytes = self.trace_meta_bytes()
        reserve = eval_macro("SIM_EVENT_SNAPSHOT_RESERVE_BYTES", self.budget, t_base)

        self.assertLessEqual(
            prof_bytes + trace_bytes + meta_bytes + reserve, self.ring_size,
            "one pump iteration may hand %d B to the transport, more than the "
            "%d B event ring"
            % (prof_bytes + trace_bytes + meta_bytes + reserve, self.ring_size),
        )

    def test_the_drain_spends_the_whole_pump_iteration_budget(self):
        """The budget is a PER PUMP ITERATION budget, so the drain has to
        run at least as often as the pump ticks - otherwise most of the
        ring's capacity is left unspent and the drain cannot keep up
        with the sampler."""
        self.assertLessEqual(self.drain_ms, self.pump_tick_ms,
                             "the profiler drain period (%d ms) is longer "
                             "than the pump tick (%d ms): most of the event "
                             "budget stays unspent"
                             % (self.drain_ms, self.pump_tick_ms))

    def test_profiler_cap_keeps_up_with_the_sampling_rate(self):
        """The cap must not throttle the profiler ring itself.

        A backend that takes ONE sample per sampling period (FreeRTOS,
        Zephyr) produces `hz` samples/s, so the drain has to move
        `hz * drain_period` samples per tick.  If the cap is below
        that, the profiler ring fills up and its own drop counter starts
        reporting lost samples - the backlog would be destroyed instead
        of deferred, which is the defect this budget is meant to fix.

        The NuttX wall-clock backend takes ONE sample per REGISTERED
        THREAD per period, so it produces `hz * threads` samples/s -
        far more than the ring can carry at the Kconfig ceiling.  The
        drain covers that case with a rate guard, which is checked
        below.
        """
        for depth in (self.depth, self.max_depth):
            table, base, _, _, cap = self.drain_macros(PROFILER_C, "PROFILE", depth)
            cap_value = eval_macro(cap, table, base)
            max_batch = eval_macro("PROFILE_MAX_BATCH", table, base)
            capacity = cap_value * max_batch        # samples per drain period
            rate = capacity * 1000 // self.drain_ms

            self.assertGreaterEqual(
                rate, self.max_hz,
                "at depth %d the profiler drain moves %d samples/s, but a "
                "single-sample-per-period backend can produce %d samples/s"
                % (depth, rate, self.max_hz),
            )

    def test_the_rate_guard_bounds_the_per_thread_producer(self):
        """The NuttX wall-clock backend produces `hz * threads` samples/s.

        That is more than the ring can carry at the Kconfig ceiling, so
        the drain cannot keep up with it statically: the plugin has to
        lower the rate it asks the backend for.  The guard scales the
        rate by the ratio of what the transport can carry to what the
        sampler produced, which converges.
        """
        path = PROFILER_C
        table, base, _, _, cap = self.drain_macros(path, "PROFILE")
        funcs = func_macros(path)

        cap_value = eval_macro(cap, table, base)
        max_batch = eval_macro("PROFILE_MAX_BATCH", table, base)
        capacity = cap_value * max_batch            # samples per drain period

        self.assertIn("PROFILE_RATE_AFTER_OVERLOAD", funcs,
                      "%s: the rate guard has no formula" % path)

        hz = self.max_hz
        for _ in range(64):
            produced = hz * self.nuttx_threads * self.drain_ms // 1000
            if produced <= capacity:
                break
            hz = call_macro("PROFILE_RATE_AFTER_OVERLOAD", [hz, produced],
                            table, base, funcs)
        produced = hz * self.nuttx_threads * self.drain_ms // 1000
        self.assertLessEqual(produced, capacity,
                             "the rate guard does not converge: %d threads "
                             "at %d Hz still produce %d samples per drain "
                             "period, the transport carries %d"
                             % (self.nuttx_threads, hz, produced, capacity))
        self.assertGreaterEqual(hz, 1)

        # The guard must actually be wired into the drain, and it must
        # react to the samples the profiler ring lost.
        src = read(path)
        tick = function_body(src, "ove_sim_profiler_tick")
        self.assertIsNotNone(tick, "%s: no ove_sim_profiler_tick()" % path)
        self.assertIn("profiler_rate_guard", tick,
                      "%s: the drain does not run the rate guard" % path)
        self.assertIn("ove_profiler_ring_dropped_get", tick,
                      "%s: the drain does not read the ring's drop counter"
                      % path)
        guard = function_body(src, "profiler_rate_guard")
        self.assertIsNotNone(guard, "%s: no profiler_rate_guard()" % path)
        self.assertIn("ove_backend_profiler_set_rate", guard,
                      "%s: the rate guard does not lower the sampling rate"
                      % path)
        self.assertIn("PROFILE_CAPACITY_PER_DRAIN", guard,
                      "%s: the rate guard does not use the transport's "
                      "capacity" % path)

    def test_the_trace_ring_size_is_not_a_rate_limit(self):
        """The trace ring's size is backlog, not rate: no build-time check
        may tie the drain's ceiling to it (a larger ring is a valid
        configuration), and the documented ceiling must match the
        arithmetic."""
        src = strip_comments(read(TRACE_C))
        for m in re.finditer(r"_Static_assert\s*\((.*?)\);", src, re.S):
            self.assertNotIn("CONFIG_OVE_TRACE_RING_SIZE", m.group(1),
                             "a build-time check ties the trace drain to the "
                             "trace ring's size")
        table, base, _, _, cap = self.drain_macros(TRACE_C, "TRACE")
        cap_value = eval_macro(cap, table, base)
        rate = cap_value * self.trace_batch * 1000 // self.trace_drain_ms
        documented = re.search(r"(\d+) \* (\d+) / 0\.1 s = ([\d ]+) records/s",
                               read(TRACE_C))
        self.assertTrue(documented, "sim_trace.c no longer states its drain ceiling")
        self.assertEqual(
            (int(documented.group(1)), int(documented.group(2)),
             int(documented.group(3).replace(" ", ""))),
            (cap_value, self.trace_batch, rate),
            "the drain ceiling documented in sim_trace.c differs from the "
            "arithmetic")

    def test_caps_replies_cost_one_event_per_tick(self):
        """A burst of rate commands must not defeat the budget: every caps
        request only marks one pending, the tick sends at most one, and
        the budget reserves exactly that one."""
        src = strip_comments(read(PROFILER_C))
        self.assertRegex(
            src, r"#define\s+PROFILE_EVENT_BUDGET_BYTES\s+\(\s*"
                 r"SIM_EVENT_PROFILER_BUDGET_BYTES\s*-\s*PROFILE_CAPS_EVENT_BYTES\s*\)",
            "the profiler budget must reserve exactly one caps event")
        calls = [m.start() for m in re.finditer(r"\bemit_caps\s*\(", src)]
        definition = re.search(r"static\s+void\s+emit_caps\s*\(", src)
        self.assertTrue(definition, "emit_caps() not found")
        calls = [c for c in calls if c != src.index("emit_caps", definition.start())]
        self.assertEqual(len(calls), 1,
                         "caps events must be sent from one place only (the tick)")
        tick = function_body(src, "ove_sim_profiler_tick")
        self.assertTrue(tick, "ove_sim_profiler_tick() not found")
        self.assertRegex(tick, r"if\s*\(\s*caps_pending\s*\)\s*emit_caps\s*\(",
                         "the tick must send a caps event only when one is pending")
        for func in ("profiler_handle_cmd", "profiler_rate_guard",
                     "ove_sim_profiler_announce_caps"):
            body = function_body(src, func)
            self.assertTrue(body, f"{func}() not found")
            self.assertNotRegex(body, r"\bemit_caps\s*\(",
                                f"{func}() sends a caps event itself")
            self.assertRegex(body, r"caps_pending\s*=\s*true",
                             f"{func}() does not queue a caps event")


if __name__ == "__main__":
    unittest.main()
