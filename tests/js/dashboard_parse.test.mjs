/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Parser tests for sim/dashboard/app.js. These exercise the trace and
 * profiler event decoders and the derived state the render paths depend
 * on (spans, gap detection, lane ordering, PC resolution, frame buckets)
 * without a browser.
 *
 * Run: node --test tests/js/
 */

import test from "node:test";
import assert from "node:assert/strict";

import {
    loadDashboard,
    traceDescriptors,
    traceStream,
    stateRec,
    markRec,
    profilerSamples,
    profilerCaps,
    profilerSymbols,
} from "./dashboard_harness.mjs";

test("trace: state transitions produce spans and detect gaps", () => {
    const g = loadDashboard();
    g.handleTrace(traceDescriptors([[5, "producer"]]), 0);
    assert.equal(g.traceThreads[5].name, "producer");
    assert.equal(g.traceThreads[5].described, true);

    /* RUN -> BLOCKED -> READY -> RUN, then a transition whose old_state
     * (9) does not match the last recorded state (READY): records were
     * lost, so that interval is UNKNOWN and flagged as a gap. */
    g.handleTrace(traceStream([
        stateRec(1000, 5, 1, 0),
        stateRec(2000, 5, 0, 2),
        stateRec(4000, 5, 2, 1),
        stateRec(5000, 5, 9, 0),
    ], 7), 0);

    assert.equal(g.traceDropped, 7);
    assert.deepEqual(g.traceSpans, [
        { tid: 5, start: 1000, end: 2000, state: 0, gap: false },
        { tid: 5, start: 2000, end: 4000, state: 2, gap: false },
        { tid: 5, start: 4000, end: 5000, state: -1, gap: true },
    ]);
    assert.equal(g.traceMaxTs, 5000);
});

test("trace: 64-bit timestamps survive the split-word decode", () => {
    const g = loadDashboard();
    const ts = 0x1_0000_0005; /* > 2^32 us */
    g.handleTrace(traceStream([stateRec(ts, 1, 1, 0)]), 0);
    assert.equal(g.traceMaxTs, ts);
    assert.equal(g.traceThreads[1].lastTs, ts);
});

test("trace: marks decode the primitive/action nibbles and arg", () => {
    const g = loadDashboard();
    g.handleTrace(traceStream([
        markRec(100, 3, 5 /* QUEUE */, 3 /* POST */, 0xbeef),
        markRec(200, 3, 1 /* MUTEX */, 1 /* WAIT_ENTER */, 0x1234),
    ]), 0);
    assert.deepEqual(g.traceMarks, [
        { tid: 3, ts: 100, prim: 5, act: 3, arg: 0xbeef },
        { tid: 3, ts: 200, prim: 1, act: 1, arg: 0x1234 },
    ]);
});

test("trace: lanes only include threads with data, sorted by name", () => {
    const g = loadDashboard();
    /* Name order differs from tid order (for..in visits integer keys in
     * ascending order, so tid order alone would pass). */
    g.handleTrace(traceDescriptors([[1, "zeta"], [2, "alpha"], [9, "no-data"]]), 0);
    g.handleTrace(traceStream([
        stateRec(100, 2, 1, 0),
        stateRec(100, 1, 1, 0),
    ]), 0);
    assert.deepEqual(g.traceLanes(), [2, 1]);
});

test("trace: reused thread storage starts a new lane", () => {
    const g = loadDashboard();
    g.handleTrace(traceDescriptors([[7, 1, "worker"]]), 0);
    g.handleTrace(traceStream([
        stateRec(1000, 7, 0, 1),
        stateRec(2000, 7, 1, 0),
    ]), 0);
    assert.equal(g.traceSpans.length, 1);

    /* Same tid, new generation: the old thread's spans and open state are
     * dropped, and new records start a fresh lane. */
    g.handleTrace(traceDescriptors([[7, 2, "worker"]]), 0);
    assert.equal(g.traceSpans.length, 0);
    assert.equal(g.traceThreads[7].lastState, -1);
    assert.equal(g.traceThreads[7].generation, 2);

    g.handleTrace(traceStream([stateRec(3000, 7, 0, 1)]), 0);
    assert.equal(g.traceThreads[7].lastState, 1);
});

test("trace: eviction splices the expired prefix in one step", () => {
    const g = loadDashboard();
    g.traceView.follow = true;
    const cutoff = 30 * 1000 * 1000; /* TRACE_WINDOW_US */
    g.traceSpans = [
        { tid: 1, start: 0, end: 10, state: 0, gap: false },
        { tid: 1, start: cutoff + 100, end: cutoff + 200, state: 0, gap: false },
    ];
    g.traceMarks = [
        { tid: 1, ts: 5, prim: 1, act: 1, arg: 0 },
        { tid: 1, ts: cutoff + 150, prim: 1, act: 1, arg: 0 },
    ];
    g.traceMaxTs = cutoff + 200;
    g.traceEvict();
    assert.equal(g.traceSpans.length, 1);
    assert.equal(g.traceSpans[0].start, cutoff + 100);
    assert.equal(g.traceMarks.length, 1);
    assert.equal(g.traceMinTs, cutoff + 100);
});

test("profiler: samples decode at word size 4 and 8", () => {
    const g = loadDashboard();
    g.handleProfile(profilerSamples(
        [{ ts: 10, tid: 2, state: 0, pcs: [0x1000, 0x2000] }], 4), 0);
    g.handleProfile(profilerSamples(
        [{ ts: 20, tid: 3, state: 2, pcs: [0x1_0000_1234] }], 8, 5), 0);

    assert.equal(g.profilerDropped, 5);
    assert.deepEqual(g.profilerSamples, [
        { ts: 10, tid: 2, state: 0, pcs: [0x1000, 0x2000] },
        { ts: 20, tid: 3, state: 2, pcs: [0x1_0000_1234] },
    ]);
});

test("profiler: an unsupported word size is ignored", () => {
    const g = loadDashboard();
    g.handleProfile(profilerSamples([{ ts: 1, tid: 1, pcs: [0x10] }], 2, 9), 0);
    assert.equal(g.profilerSamples.length, 0);
    /* The drop count is format-independent and still honoured. */
    assert.equal(g.profilerDropped, 9);
});

test("profiler: caps parse the rate, and flags expose wall-clock mode", () => {
    const g = loadDashboard();
    assert.equal(g.profilerIsWallClock(), false);
    g.handleProfile(profilerCaps(1000, 250, 1), 0);
    assert.equal(g.profilerMaxHz, 1000);
    assert.equal(g.profilerCurrentHz, 250);
    assert.equal(g.profilerIsWallClock(), true);
});

test("profiler: symbols merge, sort and clear the PC name cache", () => {
    const g = loadDashboard();
    g.handleProfile(profilerSymbols([[0x2000, 0x2100, "bar"]]), 0);
    assert.equal(g.resolvePc(0x2050), "bar");
    g.profilerPcNames.set(0x1050, "stale");

    g.handleProfile(profilerSymbols([[0x1000, 0x1100, "foo@@LIB"]]), 0);
    assert.deepEqual(g.profilerSymbols, [
        [0x1000, 0x1100, "foo@@LIB"],
        [0x2000, 0x2100, "bar"],
    ]);
    /* Cache was cleared and the glibc version tag stripped. */
    assert.equal(g.resolvePc(0x1050), "foo");
    assert.equal(g.resolvePc(0x9999), "0x9999");
    assert.equal(g.profilerPcNames.get(0x1050), "foo");
});

test("profiler: frame buckets classify libs/kernel/runtime, keep app+ove", () => {
    const g = loadDashboard();
    assert.equal(g.profilerFrameBucket("lv_obj_redraw"), "[lvgl]");
    assert.equal(g.profilerFrameBucket("vTaskDelay"), "[kernel]");
    assert.equal(g.profilerFrameBucket("nxsched_add"), "[kernel]");
    assert.equal(g.profilerFrameBucket("__aeabi_memcpy"), "[runtime]");
    assert.equal(g.profilerFrameBucket("ove_queue_send"), null);
    assert.equal(g.profilerFrameBucket("app_main"), null);
});

test("profiler: leaf info finds the first user frame, else the bucket", () => {
    const g = loadDashboard();
    g.profilerSymbols = [
        [0x1000, 0x1100, "lv_obj_redraw"],
        [0x2000, 0x2100, "app_draw"],
    ];
    g.profilerApiOnly = false;
    assert.deepEqual(g.profilerLeafInfo([0x1050, 0x2050]), { off: 0, bucket: null });

    g.profilerApiOnly = true;
    assert.deepEqual(g.profilerLeafInfo([0x1050, 0x2050]), { off: 1, bucket: null });

    /* Every frame excluded: report the innermost bucket, not a fake offset. */
    assert.deepEqual(g.profilerLeafInfo([0x1050]), { off: -1, bucket: "[lvgl]" });

    /* An unresolved PC is not a bucket match, so it is a user frame. */
    assert.deepEqual(g.profilerLeafInfo([0x9999, 0x1050]), { off: 0, bucket: null });
});

test("trace: a terminated lane reused from READY is not a gap", () => {
    const g = loadDashboard();
    /* Thread storage reused: the old thread ends TERMINATED and the new
     * one starts READY. That is not a lost record. */
    g.handleTrace(traceStream([
        stateRec(100, 5, 1, 0),
        stateRec(200, 5, 0, 4),
        stateRec(5000, 5, 1, 0),
    ]), 0);
    assert.deepEqual(g.traceSpans, [
        { tid: 5, start: 100, end: 200, state: 0, gap: false },
        { tid: 5, start: 200, end: 5000, state: 4, gap: false },
    ]);

    /* Any other old state after TERMINATED is still a lost record. */
    g.handleTrace(traceStream([
        stateRec(6000, 6, 1, 0),
        stateRec(6100, 6, 0, 4),
        stateRec(9000, 6, 2, 0),
    ]), 0);
    assert.deepEqual(g.traceSpans.filter((s) => s.tid === 6), [
        { tid: 6, start: 6000, end: 6100, state: 0, gap: false },
        { tid: 6, start: 6100, end: 9000, state: -1, gap: true },
    ]);
});

test("trace: a record a second behind restarts the timebase", () => {
    const g = loadDashboard();
    g.handleTrace(traceDescriptors([[5, "worker"]]), 0);
    g.handleTrace(traceStream([
        stateRec(5_000_000, 5, 1, 0),
        stateRec(5_100_000, 5, 0, 2),
        markRec(5_050_000, 5, 1, 1, 0x10),
    ]), 0);
    /* A late record less than a second behind is not a restart. */
    g.handleTrace(traceStream([markRec(4_500_000, 5, 1, 2, 0x10)]), 0);
    assert.equal(g.traceSpans.length, 1);
    assert.equal(g.traceMarks.length, 2);
    g.traceView.follow = false;
    g.traceView.tEndFrozen = 5_100_000;

    /* Board reset without a reconnect: time starts over near zero. The
     * old spans go, the descriptor name stays, and the first record of
     * the new timebase is not compared with the old last state. */
    g.handleTrace(traceStream([
        stateRec(200, 5, 9, 0),
        stateRec(300, 5, 0, 2),
    ]), 0);
    assert.deepEqual(g.traceSpans, [
        { tid: 5, start: 200, end: 300, state: 0, gap: false },
    ]);
    assert.equal(g.traceMarks.length, 0);
    assert.equal(g.traceThreads[5].name, "worker");
    assert.equal(g.traceMinTs, 200);
    assert.equal(g.traceMaxTs, 300);
    assert.equal(g.traceView.follow, true);
});

test("trace: span tooltip prints the interval to the microsecond", () => {
    const g = loadDashboard();
    g.handleTrace(traceStream([
        stateRec(1_000_100, 5, 1, 0),
        stateRec(1_000_400, 5, 0, 2),
        stateRec(1_500_000, 5, 2, 1),
    ]), 0);

    /* Minimal canvas + tooltip elements for traceBindPanel's hover. */
    const els = {};
    const on = {};
    const el = (id) => ({
        id, style: {}, textContent: "", checked: true,
        addEventListener(type, fn) { on[id + ":" + type] = fn; },
        getBoundingClientRect: () => ({ left: 0, top: 0, width: 1000, height: 80 }),
    });
    g.document.getElementById = (id) => (els[id] ||= el(id));
    g.traceBindPanel();

    /* Hover the middle of the 300 us RUNNING span (single lane: row 0). */
    const r = g.traceViewRange();
    const plotW = 1000 - g.TRACE_LABEL_W - g.TRACE_PLOT_PAD_R;
    const x = g.TRACE_LABEL_W + ((1_000_250 - r.tStart) / r.span) * plotW;
    on["trace-canvas:mousemove"]({ clientX: x, clientY: 30 });

    const tip = els["trace-mark-tip"];
    assert.equal(tip.style.display, "block");
    assert.match(tip.textContent, /running {2}300 µs {2}\[1\.000100s → 1\.000400s\]$/);
});

test("profiler: the Runnable filter applies only to a wall-clock backend", () => {
    const g = loadDashboard();
    const blocked = { ts: 1, tid: 5, state: 2, pcs: [0x10] };
    const ready = { ts: 1, tid: 5, state: 1, pcs: [0x10] };
    g.profilerOnCpuOnly = true; /* the user chose "Runnable" */

    g.handleProfile(profilerCaps(1000, 1000, 0), 0);
    assert.equal(g.profilerSampleIncluded(blocked), true);

    g.handleProfile(profilerCaps(250, 250, 1), 0);
    assert.equal(g.profilerSampleIncluded(blocked), false);
    assert.equal(g.profilerSampleIncluded(ready), true);

    /* Reconnect: caps are reset until the backend re-sends them, so the
     * filter is off meanwhile, but the user's choice is kept. Go through
     * the real ws.onopen; only the canvas-drawing render is stubbed. */
    g.renderProfiler = () => {};
    g.ws.onopen();
    assert.equal(g.profilerIsWallClock(), false);
    assert.equal(g.profilerSampleIncluded(blocked), true);
    assert.equal(g.profilerOnCpuOnly, true);

    g.handleProfile(profilerCaps(250, 250, 1), 0);
    assert.equal(g.profilerSampleIncluded(blocked), false);
});

test("link: trace and profile frames decode behind the frame type word", () => {
    const g = loadDashboard();
    /* The bridge prefixes every WebSocket frame with a u32 type, so the
     * decoders run at offset 4; everything above uses offset 0. */
    const frame = (type, body) => {
        const out = new Uint8Array(4 + body.byteLength);
        new DataView(out.buffer).setUint32(0, type, true);
        out.set(new Uint8Array(body), 4);
        return { data: out.buffer };
    };
    g.ws.onmessage(frame(g.FRAME_TRACE, traceDescriptors([[5, "producer"]])));
    g.ws.onmessage(frame(g.FRAME_TRACE, traceStream([
        stateRec(1000, 5, 1, 0),
        stateRec(2000, 5, 0, 2),
    ])));
    g.ws.onmessage(frame(g.FRAME_PROFILE, profilerSymbols([[0x1000, 0x1100, "foo"]])));
    g.ws.onmessage(frame(g.FRAME_PROFILE, profilerSamples(
        [{ ts: 10, tid: 5, pcs: [0x1050] }], 4)));

    assert.equal(g.traceThreads[5].name, "producer");
    assert.deepEqual(g.traceSpans, [
        { tid: 5, start: 1000, end: 2000, state: 0, gap: false },
    ]);
    assert.equal(g.resolvePc(0x1050), "foo");
    assert.deepEqual(g.profilerSamples.map((s) => s.pcs), [[0x1050]]);
});

test("trace: a new thread on a closed lane keeps its first records", () => {
    const g = loadDashboard();
    g.handleTrace(traceDescriptors([[7, 1, "worker"]]), 0);
    g.handleTrace(traceStream([stateRec(1000, 7, 1, 0), stateRec(2000, 7, 0, 4)]), 0);
    /* The pump drains the stream before it sends descriptors, so the new
     * thread's first records arrive before its new generation. */
    g.handleTrace(traceStream([
        stateRec(5000, 7, 1, 0),
        stateRec(6000, 7, 0, 2),
        stateRec(7000, 7, 2, 0),
    ]), 0);
    g.handleTrace(traceDescriptors([[7, 2, "worker"]]), 0);
    const spans = g.traceSpans.filter((s) => s.tid === 7);
    assert.ok(spans.some((s) => s.start === 5000 && s.end === 6000 && s.state === 0));
    assert.ok(spans.some((s) => s.start === 1000), "the old thread's history stays");
    assert.ok(!spans.some((s) => s.gap));
});

test("trace: a timebase restart forgets the old boot's generations", () => {
    const g = loadDashboard();
    g.handleTrace(traceDescriptors([[7, 3, "worker"]]), 0);
    g.handleTrace(traceStream([stateRec(5_000_000, 7, 1, 0), stateRec(5_001_000, 7, 0, 2)]), 0);
    /* Board reset without a reconnect: the new boot's records, then its
     * descriptor with a generation of its own. */
    g.handleTrace(traceStream([
        stateRec(1000, 7, 1, 0),
        stateRec(2000, 7, 0, 2),
        stateRec(3000, 7, 2, 0),
    ]), 0);
    const before = g.traceSpans.filter((s) => s.tid === 7).length;
    g.handleTrace(traceDescriptors([[7, 1, "worker"]]), 0);
    assert.equal(g.traceSpans.filter((s) => s.tid === 7).length, before);
    assert.ok(before > 0);
});

test("trace: version 1 descriptors carry no generation", () => {
    const g = loadDashboard();
    const name = "worker";
    const dv = new DataView(new ArrayBuffer(8 + 5 + name.length));
    dv.setUint8(0, 0); /* sub_type: descriptors */
    dv.setUint8(1, 1); /* version 1 */
    dv.setUint16(2, 1, true);
    dv.setUint32(8, 7, true);
    dv.setUint8(12, name.length);
    new Uint8Array(dv.buffer).set(new TextEncoder().encode(name), 13);
    g.handleTrace(dv.buffer, 0);
    assert.equal(g.traceThreads[7].name, "worker");
});

test("trace: a closed lane survives a new thread described before it runs", () => {
    const g = loadDashboard();
    g.handleTrace(traceDescriptors([[7, 1, "worker"]]), 0);
    g.handleTrace(traceStream([stateRec(1000, 7, 1, 0), stateRec(2000, 7, 0, 4)]), 0);
    /* The new thread on the same storage exists but has not run when the
     * pump lists the threads: its descriptor arrives before its records. */
    g.handleTrace(traceDescriptors([[7, 2, "worker"]]), 0);
    g.handleTrace(traceStream([stateRec(5000, 7, 1, 0), stateRec(6000, 7, 0, 2)]), 0);
    const spans = g.traceSpans.filter((s) => s.tid === 7);
    assert.ok(spans.some((s) => s.start === 1000), "the closed lane's history stays");
    assert.ok(spans.some((s) => s.start === 5000 && s.state === 0));
    assert.ok(!spans.some((s) => s.gap));
});
