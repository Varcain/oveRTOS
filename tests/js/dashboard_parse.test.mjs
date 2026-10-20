/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* Trace and profiler decoder tests for sim/dashboard/app.js. */

import test from "node:test";
import assert from "node:assert/strict";
import {
    loadDashboard, traceDescriptors, traceStream, stateRec, markRec,
    profilerSamples, profilerCaps, profilerSymbols,
} from "./dashboard_harness.mjs";

const T = 2 ** 32; /* a timestamp that needs the high word */

test("threads: snapshot rows distinguish threads with the same name", () => {
    const g = loadDashboard();
    const name = new TextEncoder().encode("worker");
    const rows = [[0x20000010, 7], [0x20000020, 8]];
    const frame = new DataView(new ArrayBuffer(17 + rows.length * (43 + name.length)));
    frame.setUint8(0, rows.length);
    let p = 17;
    for (const [tid, generation] of rows) {
        frame.setUint8(p++, name.length);
        new Uint8Array(frame.buffer, p, name.length).set(name);
        p += name.length + 34; // Skip state and metrics.
        frame.setUint32(p, tid, true); p += 4;
        frame.setUint32(p, generation, true); p += 4;
    }
    let threads;
    g.HOST.on = (event, data) => { if (event === "threads") threads = data.threads; };
    g.handleThreadSnapshot(frame.buffer, 0);
    assert.deepEqual(threads.map((t) => [t.name, t.tid, t.generation]),
        rows.map((row) => ["worker", ...row]));
});

test("trace: records decode into spans and marks, a lost record is a gap", () => {
    const g = loadDashboard();
    g.handleTrace(traceDescriptors([[5, "récepteur"]]), 0);
    /* The final transition reports BLOCKED, but we last saw READY. */
    g.handleTrace(traceStream([
        stateRec(T + 1000, 5, 1, 0),
        stateRec(T + 2000, 5, 0, 2),
        markRec(T + 3000, 5, 5 /* queue */, 3 /* post */, 0xbeef),
        stateRec(T + 4000, 5, 2, 1),
        stateRec(T + 5000, 5, 2, 0),
    ], 7), 0);
    assert.equal(g.traceThreads[5].name, "récepteur");
    assert.equal(g.traceDropped, 7);
    assert.deepEqual(g.traceSpans, [
        { tid: 5, start: T + 1000, end: T + 2000, state: 0, gap: false },
        { tid: 5, start: T + 2000, end: T + 4000, state: 2, gap: false },
        { tid: 5, start: T + 4000, end: T + 5000, state: -1, gap: true },
    ]);
    assert.deepEqual(g.traceMarks, [{ tid: 5, ts: T + 3000, prim: 5, act: 3, arg: 0xbeef }]);
});

test("trace: a lane reused after TERMINATED starts READY without a gap", () => {
    const g = loadDashboard();
    g.handleTrace(traceStream([
        stateRec(100, 5, 1, 0), stateRec(200, 5, 0, 4), stateRec(5000, 5, 1, 0),
        /* Any other old state after TERMINATED is still a lost record. */
        stateRec(100, 6, 1, 0), stateRec(200, 6, 0, 4), stateRec(5000, 6, 2, 0),
    ]), 0);
    assert.deepEqual(g.traceSpans.map((s) => [s.tid, s.state, s.gap]),
        [[5, 0, false], [5, 4, false], [6, 0, false], [6, -1, true]]);
});

test("trace: a record a second behind restarts the timebase", () => {
    const g = loadDashboard();
    g.handleTrace(traceDescriptors([[5, "worker"]]), 0);
    g.handleTrace(traceStream([stateRec(5_000_000, 5, 1, 0), stateRec(5_100_000, 5, 0, 2)]), 0);
    /* Less than a second behind is a late record, not a restart. */
    g.handleTrace(traceStream([markRec(4_500_000, 5, 1, 2, 0x10)]), 0);
    assert.equal(g.traceSpans.length, 1);
    assert.equal(g.traceMarks.length, 1);
    /* Board reset: the old records go, the name stays, and the first new
     * record is not compared with the old last state. */
    g.traceView.follow = false;
    g.handleTrace(traceStream([stateRec(4_100_000, 5, 1, 0), stateRec(4_100_100, 5, 0, 2)]), 0);
    assert.deepEqual(g.traceSpans, [
        { tid: 5, start: 4_100_000, end: 4_100_100, state: 0, gap: false },
    ]);
    assert.equal(g.traceMarks.length, 0);
    assert.equal(g.traceThreads[5].name, "worker");
    assert.equal(g.traceMinTs, 4_100_000);
    assert.equal(g.traceView.follow, true);
});

test("trace: records older than the 30 s window are evicted", () => {
    const g = loadDashboard();
    g.handleTrace(traceStream([
        stateRec(100, 1, 1, 0), markRec(150, 1, 1, 1, 0), stateRec(200, 1, 0, 2),
    ]), 0);
    g.handleTrace(traceStream([stateRec(30_000_300, 1, 2, 0)]), 0);
    assert.deepEqual(g.traceSpans.map((s) => s.start), [200]);
    assert.equal(g.traceMarks.length, 0);
    assert.equal(g.traceMinTs, 200);
});

test("trace: reused storage drops history from an unclosed lane", () => {
    const g = loadDashboard();
    g.handleTrace(traceDescriptors([[7, 1, "worker"]]), 0);
    g.handleTrace(traceStream([stateRec(100, 7, 1, 0), markRec(150, 7, 1, 1, 0),
        stateRec(200, 7, 0, 2)]), 0);
    g.handleTrace(traceDescriptors([[7, 2, "worker"]]), 0);
    assert.deepEqual(g.traceSpans, []);
    assert.deepEqual(g.traceMarks, []);
    assert.equal(g.traceThreads[7].lastState, -1);
});

for (const descriptorsFirst of [false, true]) {
    const order = descriptorsFirst ? "descriptors first" : "records first";
    test(`trace: reused storage preserves a closed lane (${order})`, () => {
        const g = loadDashboard();
        g.handleTrace(traceDescriptors([[7, 1, "old"]]), 0);
        g.handleTrace(traceStream([stateRec(100, 7, 1, 0), stateRec(200, 7, 0, 4)]), 0);
        const descriptor = traceDescriptors([[7, 2, "new"]]);
        if (descriptorsFirst) g.handleTrace(descriptor, 0);
        g.handleTrace(traceStream([stateRec(500, 7, 1, 0), stateRec(600, 7, 0, 2)]), 0);
        if (!descriptorsFirst) g.handleTrace(descriptor, 0);
        assert.equal(g.traceThreads[7].name, "new");
        assert.deepEqual(g.traceSpans.map((s) => [s.start, s.end, s.state, s.gap]),
            [[100, 200, 0, false], [200, 500, 4, false], [500, 600, 0, false]]);
    });
}

test("trace: reuse preserves a lane first described after TERMINATED", () => {
    const g = loadDashboard();
    g.handleTrace(traceStream([stateRec(100, 7, 1, 0), stateRec(200, 7, 0, 4)]), 0);
    g.handleTrace(traceDescriptors([[7, 1, "old"]]), 0);
    g.handleTrace(traceStream([stateRec(500, 7, 1, 0), stateRec(600, 7, 0, 2)]), 0);
    g.handleTrace(traceDescriptors([[7, 2, "new"]]), 0);
    assert.deepEqual(g.traceSpans.map((s) => [s.start, s.end, s.state, s.gap]),
        [[100, 200, 0, false], [200, 500, 4, false], [500, 600, 0, false]]);
});

test("trace: replacing a pre-start generation zero preserves the first spans", () => {
    const g = loadDashboard();
    g.handleTrace(traceDescriptors([[7, 0, "worker"]]), 0);
    g.handleTrace(traceStream([stateRec(100, 7, 1, 0), stateRec(200, 7, 0, 2)]), 0);
    g.handleTrace(traceDescriptors([[7, 6, "worker"]]), 0);
    assert.deepEqual(g.traceSpans, [{ tid: 7, start: 100, end: 200, state: 0, gap: false }]);
});

test("trace: a timebase restart forgets the old boot's generations", () => {
    const g = loadDashboard();
    g.handleTrace(traceDescriptors([[7, 3, "worker"]]), 0);
    g.handleTrace(traceStream([stateRec(5_000_000, 7, 1, 0), stateRec(5_001_000, 7, 0, 2)]), 0);
    g.handleTrace(traceStream([stateRec(100, 7, 1, 0), stateRec(200, 7, 0, 2)]), 0);
    g.handleTrace(traceDescriptors([[7, 1, "rebooted"]]), 0);
    assert.equal(g.traceThreads[7].name, "rebooted");
    assert.deepEqual(g.traceSpans, [{ tid: 7, start: 100, end: 200, state: 0, gap: false }]);
});

test("trace: v1 descriptors still decode alongside v2", () => {
    const g = loadDashboard();
    g.handleTrace(traceDescriptors([[7, 1, "current"]]), 0);
    g.handleTrace(traceDescriptors([[8, "legacy"]], 0, 1), 0);
    assert.equal(g.traceThreads[8].name, "legacy");
    assert.equal(g.traceThreads[8].generation, undefined);
    assert.equal(g.traceThreads[7].name, "current");
});

test("trace: lanes sort by name and the hover finds the span on its lane", () => {
    const g = loadDashboard();
    /* Name order is not tid order; tid 9 has no records and no lane. */
    g.handleTrace(traceDescriptors([[1, "zeta"], [2, "alpha"], [9, "idle"]]), 0);
    g.handleTrace(traceStream([
        stateRec(1_000_000, 2, 1, 0),
        stateRec(1_000_100, 1, 1, 0),
        stateRec(1_000_400, 1, 0, 2),
        stateRec(1_500_000, 2, 0, 2),
    ]), 0);
    const lanes = g.traceLanes();
    assert.deepEqual(lanes, [2, 1]);
    g.traceBuildLaneIndex(lanes); /* as renderTrace does */

    const els = {};
    g.document.getElementById = (id) => (els[id] ||= {
        style: {}, checked: true,
        addEventListener(type, fn) { this["on" + type] = fn; },
        getBoundingClientRect: () => ({ left: 0, top: 0, width: 1000, height: 80 }),
    });
    g.traceBindPanel();
    /* The middle of zeta's 300 us RUNNING span, on the second lane. */
    const r = g.traceViewRange();
    const plotW = 1000 - g.TRACE_LABEL_W - g.TRACE_PLOT_PAD_R;
    els["trace-canvas"].onmousemove({
        clientX: g.TRACE_LABEL_W + ((1_000_250 - r.tStart) / r.span) * plotW,
        clientY: 16 + 1.5 * g.traceLaneH(80, 2),
    });
    assert.equal(els["trace-mark-tip"].textContent,
        "zeta  running  300 µs  [1.000100s → 1.000400s]");
});

test("profiler: samples decode at word size 4 and 8, other sizes are skipped", () => {
    const g = loadDashboard();
    g.handleProfile(profilerSamples([
        { ts: T + 10, tid: 2, pcs: [0x1000, 0x2000] },
        { ts: T + 11, tid: 2, state: 1, pcs: [0x3000] },
    ], 4), 0);
    g.handleProfile(profilerSamples(
        [{ ts: T + 20, tid: 3, state: 2, pcs: [0x1_0000_1234, 0x40] }], 8, 5), 0);
    /* An unknown word size: no samples, but the drop count still counts. */
    g.handleProfile(profilerSamples([{ ts: T + 30, tid: 4, pcs: [] }], 2, 4), 0);
    assert.equal(g.profilerDropped, 9);
    assert.deepEqual(g.profilerSamples, [
        { ts: T + 10, tid: 2, state: 0, pcs: [0x1000, 0x2000] },
        { ts: T + 11, tid: 2, state: 1, pcs: [0x3000] },
        { ts: T + 20, tid: 3, state: 2, pcs: [0x1_0000_1234, 0x40] },
    ]);
});

test("profiler: the Runnable filter needs wall-clock caps, reset on reconnect", () => {
    const g = loadDashboard();
    const blocked = { ts: 1, tid: 5, state: 2, pcs: [0x10] };
    g.profilerOnCpuOnly = true; /* the user chose "Runnable" */
    g.handleProfile(profilerCaps(1000, 250, 0), 0);
    assert.deepEqual([g.profilerMaxHz, g.profilerCurrentHz], [1000, 250]);
    assert.equal(g.profilerSampleIncluded(blocked), true);
    g.handleProfile(profilerCaps(1000, 250, 1), 0);
    assert.equal(g.profilerSampleIncluded(blocked), false);
    assert.equal(g.profilerSampleIncluded({ ...blocked, state: 1 }), true);
    /* Until the backend re-sends caps the filter is off; the choice stays. */
    g.renderProfiler = () => {};
    g.ws.onopen();
    assert.equal(g.profilerSampleIncluded(blocked), true);
    assert.equal(g.profilerOnCpuOnly, true);
});

test("profiler: symbols merge sorted, drop version tags, reset the PC cache", () => {
    const g = loadDashboard();
    g.handleProfile(profilerSymbols([[0x2000, 0x2100, "bar"]]), 0);
    assert.equal(g.resolvePc(0x1050), "0x1050");
    g.handleProfile(profilerSymbols([[0x1000, 0x1100, "foo@@LIB"]]), 0);
    assert.deepEqual(g.profilerSymbols.map((s) => s[0]), [0x1000, 0x2000]);
    assert.equal(g.resolvePc(0x1050), "foo");
    assert.equal(g.resolvePc(0x2050), "bar");
});

test("profiler: API-only selects the first user frame or the innermost library bucket", () => {
    const g = loadDashboard();
    g.profilerSymbols = [
        [0x1000, 0x1100, "lv_obj_redraw"],
        [0x2000, 0x2100, "ove_queue_send"],
        [0x3000, 0x3100, "vTaskDelay"],
    ];
    g.profilerApiOnly = false;
    assert.deepEqual(g.profilerLeafInfo([0x1050, 0x2050]), { off: 0, bucket: null });
    g.profilerApiOnly = true;
    assert.deepEqual(g.profilerLeafInfo([0x1050, 0x2050]), { off: 1, bucket: null });
    /* No user frame: the innermost frame's bucket. */
    assert.deepEqual(g.profilerLeafInfo([0x1050, 0x3050]), { off: -1, bucket: "[lvgl]" });
    /* An unresolved PC matches no bucket, so it is a user frame. */
    assert.deepEqual(g.profilerLeafInfo([0x9999, 0x1050]), { off: 0, bucket: null });
});

test("link: trace and profile frames decode behind the frame type word", () => {
    const g = loadDashboard();
    const send = (type, body) => {
        const out = new Uint8Array(4 + body.byteLength);
        new DataView(out.buffer).setUint32(0, type, true);
        out.set(new Uint8Array(body), 4);
        g.ws.onmessage({ data: out.buffer });
    };
    send(g.FRAME_TRACE, traceDescriptors([[5, "producer"]]));
    send(g.FRAME_TRACE, traceStream([stateRec(1000, 5, 1, 0), stateRec(2000, 5, 0, 2)]));
    send(g.FRAME_PROFILE, profilerSymbols([[0x1000, 0x1100, "foo"]]));
    send(g.FRAME_PROFILE, profilerSamples([{ ts: 10, tid: 5, pcs: [0x1050] }], 4));
    assert.equal(g.traceThreads[5].name, "producer");
    assert.equal(g.traceSpans.length, 1);
    assert.equal(g.resolvePc(g.profilerSamples[0].pcs[0]), "foo");
});
