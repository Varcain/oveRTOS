# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Every queue backend must mark its blocking queue waits.

The trace stream is the only cross-backend view of what a program did, so
every backend has to describe the same event the same way.  A queue send
that blocks on a full queue and a queue receive that blocks on an empty
queue are the two waits a backend can park its caller in; both have to be
bracketed by

    OVE_TRACE_PRIM_QUEUE / OVE_TRACE_ACT_WAIT_ENTER
    OVE_TRACE_PRIM_QUEUE / OVE_TRACE_ACT_WAIT_EXIT

otherwise the very same application produces two different traces (one
backend shows the throttled producer, the other does not), and any tooling
that derives "blocked time" or "queue contention" from the stream reports
different numbers per backend.

The checks below are source-level and backend-agnostic in shape:

  * every C file in the tree that implements ove_queue_send() must be
    known to this test - a new backend cannot be added without deciding
    how it marks its waits;
  * every blocking queue-API call of a backend's ove_queue_send() and
    ove_queue_receive() must be preceded by a QUEUE/WAIT_ENTER marker and
    followed by a QUEUE/WAIT_EXIT marker;
  * the QUEUE/POST marker of a send must come after that send's
    QUEUE/WAIT_EXIT marker, so a consumer of the stream that pairs a POST
    with the waiter whose window contains it can never attribute a
    producer's own POST to the producer's own wait;
  * the ISR variants never block, so they must not mark a blocking wait.

Run: python3 tests/python/test_queue_trace_markers.py
"""

import os
import re
import unittest

#: Repository root.  Overridable so the same checks can be run against a
#: tree that is not the checkout (e.g. a pre-fix copy of the backends).
ROOT = os.environ.get(
    "OVE_QUEUE_MARKERS_ROOT",
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
)

#: Where a queue backend may live.  Only these roots are scanned: the
#: rest of the tree is either not C or not a backend.
SCAN_ROOTS = ("backends", "src", "tests/host")

#: The queue-API calls of each backend that can park the caller, split per
#: operation.  A backend that is added to the tree has to be added here
#: too - that is the point of the table.
BACKENDS = {
    "backends/freertos/freertos_queue.c": {
        "send": ("xQueueSend",),
        "receive": ("xQueueReceive",),
        "isr_send": ("xQueueSendFromISR",),
        "isr_receive": ("xQueueReceiveFromISR",),
        "rtos": True,
    },
    "backends/zephyr/zephyr_queue.c": {
        "send": ("k_msgq_put",),
        "receive": ("k_msgq_get",),
        "isr_send": ("k_msgq_put",),
        "isr_receive": ("k_msgq_get",),
        "rtos": True,
    },
    "backends/nuttx/nuttx_queue.c": {
        "send": ("nxsem_wait_uninterruptible", "nxsem_tickwait_uninterruptible"),
        "receive": ("nxsem_wait_uninterruptible", "nxsem_tickwait_uninterruptible"),
        "isr_send": ("nxsem_trywait",),
        "isr_receive": ("nxsem_trywait",),
        "rtos": True,
    },
    # The host backend is not an RTOS: its ISR variants delegate to the
    # blocking API, so only the marker shape is checked for it.
    "tests/host/posix/posix_queue.c": {
        "send": ("pthread_cond_wait", "pthread_cond_timedwait"),
        "receive": ("pthread_cond_wait", "pthread_cond_timedwait"),
        "isr_send": (),
        "isr_receive": (),
        "rtos": False,
    },
}

#: OVE_TRACE_MARK_CURRENT(OVE_TRACE_PRIM_QUEUE, OVE_TRACE_ACT_XXX, obj)
#: and its OVE_TRACE_MARK() sibling.
MARKER_RE = re.compile(
    r"OVE_TRACE_MARK(?:_CURRENT)?\s*\(\s*OVE_TRACE_PRIM_QUEUE\s*,\s*"
    r"OVE_TRACE_ACT_(?P<act>WAIT_ENTER|WAIT_EXIT|POST)\s*,",
)


def strip_comments_and_strings(src):
    """Blank out comment and string bodies, keeping every offset."""
    out = list(src)
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c in "\"'":
            quote = c
            j = i + 1
            while j < n and src[j] != quote:
                if src[j] == "\\":
                    j += 1
                j += 1
            for k in range(i + 1, min(j, n)):
                if out[k] != "\n":
                    out[k] = " "
            i = min(j + 1, n)
        else:
            i += 1
    return "".join(out)


def find_function(clean, name):
    """Return (start, end) offsets of the body of `name`, or None."""
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\([^;{]*?\)\s*\{", clean):
        brace = clean.index("{", m.end() - 1)
        depth = 0
        i = brace
        while i < len(clean):
            if clean[i] == "{":
                depth += 1
            elif clean[i] == "}":
                depth -= 1
                if depth == 0:
                    return brace, i
            i += 1
    return None


def markers_in(clean, start, end):
    """[(offset, act)] of the QUEUE markers inside [start, end)."""
    return [(m.start(), m.group("act")) for m in MARKER_RE.finditer(clean, start, end)]


def calls_in(clean, start, end, names):
    """[(offset, name)] of the calls of `names` inside [start, end)."""
    out = []
    for name in names:
        for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", clean[start:end]):
            out.append((start + m.start(), name))
    out.sort()
    return out


def discover_backends(root):
    """Relative paths of every C file that implements ove_queue_send()."""
    found = []
    for top in SCAN_ROOTS:
        base = os.path.join(root, top)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in (".git", "output", "build")]
            for name in filenames:
                if not name.endswith(".c"):
                    continue
                path = os.path.join(dirpath, name)
                try:
                    with open(path, "r", errors="replace") as f:
                        src = f.read()
                except OSError:
                    continue
                if re.search(r"\bint\s+ove_queue_send\s*\(", src):
                    found.append(os.path.relpath(path, root))
    return sorted(found)


def backend_source(root, rel):
    with open(os.path.join(root, rel), "r", errors="replace") as f:
        return f.read()


class QueueTraceMarkersTest(unittest.TestCase):
    """Every queue backend brackets its blocking queue waits."""

    def test_every_queue_backend_is_known(self):
        """A new backend must be added to the table above, markers and all."""
        found = discover_backends(ROOT)
        self.assertTrue(found, "no queue backend found - the scan roots are wrong")
        unknown = [rel for rel in found if rel not in BACKENDS]
        self.assertEqual(
            unknown, [],
            "new queue backend(s) without a marker contract: %s - add them to "
            "BACKENDS in %s and mark their blocking waits" % (unknown, __file__))
        missing = [rel for rel in BACKENDS if rel not in found]
        self.assertEqual(
            missing, [],
            "queue backend(s) listed in the table but not found in the tree: %s" % missing)

    def _check_wait_is_marked(self, rel, operation, apis):
        src = backend_source(ROOT, rel)
        clean = strip_comments_and_strings(src)
        body = find_function(clean, operation)
        self.assertIsNotNone(body, "%s: no %s() found" % (rel, operation))
        start, end = body
        marks = markers_in(clean, start, end)
        calls = calls_in(clean, start, end, apis)
        self.assertTrue(calls, "%s: %s() calls none of %s - is the backend table "
                                 "out of date?" % (rel, operation, list(apis)))
        for off, api in calls:
            # The bracket must close on every path out of the wait, so the
            # WAIT_EXIT has to come before the first return after the call.
            ret = re.search(r"\breturn\b", clean[off:end])
            limit = off + ret.start() if ret else end
            enters = [o for o, act in marks if act == "WAIT_ENTER" and o < off]
            exits = [o for o, act in marks if act == "WAIT_EXIT" and off < o < limit]
            self.assertTrue(
                enters,
                "%s: %s() calls %s() but emits no "
                "OVE_TRACE_PRIM_QUEUE/OVE_TRACE_ACT_WAIT_ENTER before it - the "
                "blocking wait is invisible in the trace" % (rel, operation, api))
            self.assertTrue(
                exits,
                "%s: %s() calls %s() but emits no "
                "OVE_TRACE_PRIM_QUEUE/OVE_TRACE_ACT_WAIT_EXIT after it, before "
                "the next return - the wait never ends in the trace"
                % (rel, operation, api))

    def test_send_marks_the_blocking_wait(self):
        """The producer-side blocking wait is marked on every backend."""
        for rel, spec in sorted(BACKENDS.items()):
            with self.subTest(backend=rel):
                self._check_wait_is_marked(rel, "ove_queue_send", spec["send"])

    def test_receive_marks_the_blocking_wait(self):
        """The consumer-side blocking wait is marked on every backend."""
        for rel, spec in sorted(BACKENDS.items()):
            with self.subTest(backend=rel):
                self._check_wait_is_marked(rel, "ove_queue_receive", spec["receive"])

    def test_post_comes_after_the_wait_exit(self):
        """A send's POST must not land inside its own wait window.

        A consumer that pairs a WAIT_EXIT with the oldest POST of the same
        object inside the wait window would attribute a POST emitted
        before the send's own WAIT_EXIT to the send's own wait - and
        consume it, so the waiter it really woke would end up unpaired.
        """
        for rel, spec in sorted(BACKENDS.items()):
            with self.subTest(backend=rel):
                src = backend_source(ROOT, rel)
                clean = strip_comments_and_strings(src)
                body = find_function(clean, "ove_queue_send")
                self.assertIsNotNone(body, "%s: no ove_queue_send() found" % rel)
                start, end = body
                marks = markers_in(clean, start, end)
                posts = [o for o, act in marks if act == "POST"]
                exits = [o for o, act in marks if act == "WAIT_EXIT"]
                self.assertTrue(posts, "%s: ove_queue_send() emits no QUEUE/POST" % rel)
                self.assertTrue(exits, "%s: ove_queue_send() emits no QUEUE/WAIT_EXIT" % rel)
                for post in posts:
                    self.assertTrue(
                        any(exit_off < post for exit_off in exits),
                        "%s: ove_queue_send() emits its QUEUE/POST before the "
                        "QUEUE/WAIT_EXIT - a pairing consumer would match the producer's "
                        "own POST with its own wait" % rel)

    def test_isr_variants_stay_unmarked(self):
        """The ISR variants never park the caller and have no thread lane."""
        for rel, spec in sorted(BACKENDS.items()):
            if not spec["rtos"]:
                continue
            with self.subTest(backend=rel):
                src = backend_source(ROOT, rel)
                clean = strip_comments_and_strings(src)
                for operation in ("ove_queue_send_from_isr", "ove_queue_receive_from_isr"):
                    body = find_function(clean, operation)
                    self.assertIsNotNone(body, "%s: no %s() found" % (rel, operation))
                    start, end = body
                    acts = [act for _off, act in markers_in(clean, start, end)]
                    self.assertNotIn(
                        "WAIT_ENTER", acts,
                        "%s: %s() emits a QUEUE/WAIT_ENTER marker - it cannot "
                        "block, so it must not report a wait" % (rel, operation))
                    self.assertNotIn(
                        "WAIT_EXIT", acts,
                        "%s: %s() emits a QUEUE/WAIT_EXIT marker - it cannot "
                        "block, so it must not report a wait" % (rel, operation))
                    self.assertNotIn(
                        "POST", acts,
                        "%s: %s() emits a QUEUE/POST marker - an interrupt has "
                        "no thread lane to post from" % (rel, operation))

    def test_isr_variants_do_not_delegate_to_the_blocking_api(self):
        """An RTOS ISR variant uses the non-blocking kernel call.

        The host backend delegates to the blocking API with a zero
        timeout, which makes it report a zero-length wait from ISR
        context; the RTOS backends use the non-blocking kernel calls and
        must keep doing so.
        """
        for rel, spec in sorted(BACKENDS.items()):
            if not spec["rtos"]:
                continue
            with self.subTest(backend=rel):
                src = backend_source(ROOT, rel)
                clean = strip_comments_and_strings(src)
                for operation, key in (("ove_queue_send_from_isr", "isr_send"),
                                       ("ove_queue_receive_from_isr", "isr_receive")):
                    body = find_function(clean, operation)
                    self.assertIsNotNone(body, "%s: no %s() found" % (rel, operation))
                    start, end = body
                    self.assertTrue(
                        calls_in(clean, start, end, spec[key]),
                        "%s: %s() calls none of %s - is the backend table out "
                        "of date?" % (rel, operation, list(spec[key])))
                    blocking = calls_in(
                        clean, start, end,
                        ("ove_queue_send", "ove_queue_receive"))
                    self.assertEqual(
                        blocking, [],
                        "%s: %s() calls the blocking queue API - an ISR variant "
                        "must use the non-blocking one" % (rel, operation))


if __name__ == "__main__":
    unittest.main()
