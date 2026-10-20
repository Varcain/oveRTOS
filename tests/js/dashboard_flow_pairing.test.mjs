/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * POST -> WAIT_EXIT pairing tests for sim/dashboard/app.js.
 *
 * The dashboard pairs a blocked thread with a POST on the same object
 * inside its wait window. A single object can accumulate several POSTs
 * inside one wait window, so the pairing is FIFO: the oldest POST inside
 * the window is given to the first waiter that leaves, and every POST is
 * consumed at most once. The pairing is a heuristic, not the real waker:
 * the backends emit POST after the waking call, so a waiter that preempts
 * the poster is paired one post late, and a broadcast pairs with one
 * waiter only.
 *
 * Run: node --test tests/js/
 */

import test from "node:test";
import assert from "node:assert/strict";

import {
    loadDashboard,
    traceStream,
    markRec,
    stateRec,
} from "./dashboard_harness.mjs";

const MUTEX = 1;   /* OVE_TRACE_PRIM_MUTEX */
const WE = 1;      /* OVE_TRACE_ACT_WAIT_ENTER */
const WX = 2;      /* OVE_TRACE_ACT_WAIT_EXIT */
const POST = 3;    /* OVE_TRACE_ACT_POST */

const OBJ = 0x20001234;
const OBJ2 = 0x20005678;

const enter = (ts, tid, obj = OBJ) => markRec(ts, tid, MUTEX, WE, obj);
const exit  = (ts, tid, obj = OBJ) => markRec(ts, tid, MUTEX, WX, obj);
const post  = (ts, tid, obj = OBJ) => markRec(ts, tid, MUTEX, POST, obj);

const flowOf = (g, tid) => g.traceFlows.find((f) => f.tid === tid);

test("trace: two POSTs in one wait window pair FIFO with two waiters", () => {
    const g = loadDashboard();
    g.handleTrace(traceStream([
        enter(100, 5),
        enter(110, 6),
        post(140, 9),
        post(150, 9),
        exit(200, 5),
        exit(210, 6),
    ]), 0);

    assert.equal(g.traceFlows.length, 2);
    /* Each waiter gets its own POST: the oldest one for the waiter that
     * leaves first, the next one for the second waiter. */
    assert.equal(flowOf(g, 5).postTid, 9);
    assert.equal(flowOf(g, 5).postTs, 140);
    assert.equal(flowOf(g, 6).postTid, 9);
    assert.equal(flowOf(g, 6).postTs, 150);
    /* No POST is used twice. */
    assert.deepEqual(g.traceFlows.map((f) => f.postTs).sort(), [140, 150]);
});

test("trace: one POST inside the window wakes exactly one waiter", () => {
    const g = loadDashboard();
    g.handleTrace(traceStream([
        enter(100, 5),
        enter(110, 6),
        post(140, 9),
        exit(200, 5),
        exit(210, 6),
    ]), 0);

    assert.equal(g.traceFlows.length, 2);
    const matched = g.traceFlows.filter((f) => f.postTid !== null);
    assert.equal(matched.length, 1, "a single POST must not be attributed twice");
    /* The FIFO gives the POST to the waiter that leaves first. */
    assert.equal(matched[0].tid, 5);
    assert.equal(matched[0].postTs, 140);
    /* The other waiter stays unmatched: null post, still a valid flow. */
    assert.deepEqual(flowOf(g, 6), {
        tid: 6, start: 110, end: 210, obj: OBJ, prim: MUTEX,
        postTid: null, postTs: null,
    });
});

test("trace: a waiter takes the oldest POST inside its window", () => {
    const g = loadDashboard();
    g.handleTrace(traceStream([
        enter(100, 5),
        post(140, 9),
        post(150, 9),
        exit(200, 5),
    ]), 0);

    assert.equal(g.traceFlows.length, 1);
    assert.equal(g.traceFlows[0].postTid, 9);
    assert.equal(g.traceFlows[0].postTs, 140);
});

test("trace: a POST outside the wait window is not consumed", () => {
    const g = loadDashboard();
    /* The POST lands before the first waiter's window, so it cannot wake
     * that waiter — and it must stay pending for a waiter whose window
     * does contain it (the second waiter is recorded out of timestamp
     * order, which the wire format allows). */
    g.handleTrace(traceStream([
        post(50, 9),
        enter(120, 6),
        exit(200, 6),
        enter(40, 7),
        exit(60, 7),
    ]), 0);

    assert.equal(g.traceFlows.length, 2);
    assert.equal(flowOf(g, 6).postTid, null);
    assert.equal(flowOf(g, 6).postTs, null);
    assert.equal(flowOf(g, 7).postTid, 9);
    assert.equal(flowOf(g, 7).postTs, 50);
});

test("trace: a POST on another object never pairs with this waiter", () => {
    const g = loadDashboard();
    g.handleTrace(traceStream([
        post(140, 9, OBJ2),
        enter(100, 5, OBJ),
        exit(200, 5, OBJ),
    ]), 0);

    assert.equal(g.traceFlows.length, 1);
    assert.equal(g.traceFlows[0].postTid, null);
    assert.equal(g.traceFlows[0].postTs, null);
});

test("trace: unmatched POSTs are reclaimed when the window moves on", () => {
    const g = loadDashboard();
    g.handleTrace(traceStream([post(1000, 9)]), 0);

    /* Move the trace window past the POST (TRACE_WINDOW_US = 30 s): the
     * pending POST is dropped, so a later waiter whose window still
     * contains it must not be paired with the stale POST. */
    g.handleTrace(traceStream([
        stateRec(1000 + 30 * 1000 * 1000 + 1, 1, 0, 0),
    ]), 0);

    g.handleTrace(traceStream([
        enter(900, 5),
        exit(1000 + 30 * 1000 * 1000 + 2, 5),
    ]), 0);

    assert.equal(g.traceFlows.length, 1);
    assert.equal(g.traceFlows[0].postTid, null);
    assert.equal(g.traceFlows[0].postTs, null);
});

test("trace: the per-object POST queue is capped and drops the oldest", () => {
    const g = loadDashboard();
    const posts = [];
    for (let i = 0; i < 4100; i++) posts.push(post(1000 + i, 9));
    g.handleTrace(traceStream(posts), 0);

    /* The oldest POSTs were dropped to stay under the cap, so the waiter
     * that could only have been woken by them stays unmatched ... */
    g.handleTrace(traceStream([
        enter(999, 5),
        exit(1001, 5),
    ]), 0);
    assert.equal(flowOf(g, 5).postTid, null);

    /* ... while the most recent POST is still there for a later waiter. */
    g.handleTrace(traceStream([
        enter(5099, 6),
        exit(5100, 6),
    ]), 0);
    assert.equal(flowOf(g, 6).postTid, 9);
    assert.equal(flowOf(g, 6).postTs, 5099);
});
