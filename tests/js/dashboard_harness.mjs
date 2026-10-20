/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Minimal DOM/WebSocket shim so sim/dashboard/app.js can be loaded by the
 * Node test runner. app.js is a classic script (no modules): running it
 * with vm.runInThisContext() puts its `var`/`function` declarations on
 * globalThis, which lets the tests call the trace/profiler parsers and
 * inspect the state they populate.
 *
 * Call loadDashboard() at the start of every test for a clean slate.
 */

import fs from "node:fs";
import path from "node:path";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const APP_JS = path.resolve(HERE, "../../sim/dashboard/app.js");

const noop = () => {};

/* A DOM element stand-in: known object-shaped properties are real, any
 * other property is a no-op method so app.js's event wiring can't throw. */
function fakeElement() {
    const target = {
        style: {},
        classList: { add: noop, remove: noop, toggle: noop, contains: () => false },
        dataset: {},
        children: [],
        options: [],
        files: [],
        value: "",
        checked: false,
        hidden: false,
        width: 0,
        height: 0,
        textContent: "",
        innerHTML: "",
    };
    return new Proxy(target, {
        get(t, p) {
            if (p in t) return t[p];
            if (typeof p === "symbol") return undefined;
            return noop;
        },
        set(t, p, v) {
            t[p] = v;
            return true;
        },
    });
}

function installGlobals() {
    globalThis.window = globalThis;
    globalThis.addEventListener = noop;
    globalThis.removeEventListener = noop;
    globalThis.window.oveLayoutHost = {};
    globalThis.document = {
        getElementById: () => fakeElement(),
        querySelector: () => fakeElement(),
        querySelectorAll: () => [],
        createElement: () => fakeElement(),
        createTextNode: (text) => ({ textContent: String(text) }),
        addEventListener: noop,
        removeEventListener: noop,
        body: fakeElement(),
        documentElement: fakeElement(),
        head: fakeElement(),
    };
    globalThis.location = {
        protocol: "http:",
        host: "localhost:8081",
        hostname: "localhost",
        port: "8081",
        href: "http://localhost:8081/",
    };
    /* Never connect: a real WebSocket would retry against localhost and
     * eventually recurse through ws.onerror. */
    globalThis.WebSocket = class {
        close() {}
        send() {}
        addEventListener() {}
        removeEventListener() {}
    };
    globalThis.requestAnimationFrame = noop;
    globalThis.cancelAnimationFrame = noop;
    globalThis.setInterval = () => 0;
    globalThis.clearInterval = noop;
}

/**
 * Load a fresh copy of sim/dashboard/app.js into the current global.
 * @returns {object} globalThis, whose properties include the app's
 *          functions and state (handleTrace, traceSpans, ...).
 */
export function loadDashboard() {
    installGlobals();
    const src = fs.readFileSync(APP_JS, "utf8");
    vm.runInThisContext(src, { filename: "app.js" });
    /* Pretend both panels were already shown so ensureWindow() doesn't run
     * the DOM-heavy traceBindPanel()/profilerBindPanel() during parsing. */
    globalThis._panelsSeen.trace = true;
    globalThis._panelsSeen.profiler = true;
    return globalThis;
}

/* ── Wire-format builders (mirror the plugin envelopes) ──────────────── */

/** Build an OVE_SIM_TRACE_EVT_DESCRIPTORS payload. */
export function traceDescriptors(entries, dropped = 0) {
    /* Entry is [tid, name] (generation 0) or [tid, generation, name]. */
    const norm = (e) => e.length === 3
        ? { tid: e[0], gen: e[1], name: e[2] }
        : { tid: e[0], gen: 0, name: e[1] };
    const descs = entries.map(norm);
    let size = 8;
    for (const d of descs) size += 9 + d.name.length;
    const dv = new DataView(new ArrayBuffer(size));
    dv.setUint8(0, 0); /* sub_type */
    dv.setUint8(1, 2); /* version */
    dv.setUint16(2, entries.length, true);
    dv.setUint32(4, dropped, true);
    let p = 8;
    for (const d of descs) {
        dv.setUint32(p, d.tid, true);
        p += 4;
        dv.setUint32(p, d.gen, true);
        p += 4;
        dv.setUint8(p, d.name.length);
        p += 1;
        for (let i = 0; i < d.name.length; i++) dv.setUint8(p + i, d.name.charCodeAt(i));
        p += d.name.length;
    }
    return dv.buffer;
}

/** Build an OVE_SIM_TRACE_EVT_NAMES payload (user id -> name). */
export function traceNames(entries, dropped = 0) {
    let size = 8;
    for (const [id, name] of entries) size += 3 + name.length;
    const dv = new DataView(new ArrayBuffer(size));
    dv.setUint8(0, 2); /* sub_type = NAMES */
    dv.setUint8(1, 3); /* version */
    dv.setUint16(2, entries.length, true);
    dv.setUint32(4, dropped, true);
    let p = 8;
    for (const [id, name] of entries) {
        dv.setUint16(p, id, true);
        p += 2;
        dv.setUint8(p, name.length);
        p += 1;
        for (let i = 0; i < name.length; i++) dv.setUint8(p + i, name.charCodeAt(i));
        p += name.length;
    }
    return dv.buffer;
}

/** Build an OVE_SIM_TRACE_EVT_STREAM payload from record tuples.
 *  Defaults to the current v3 layout; pass version=2 for the legacy
 *  16-byte layout. */
export function traceStream(records, dropped = 0, version = 3) {
    const recSize = version >= 3 ? 24 : 16;
    const dv = new DataView(new ArrayBuffer(8 + records.length * recSize));
    dv.setUint8(0, 1); /* sub_type */
    dv.setUint8(1, version);
    dv.setUint16(2, records.length, true);
    dv.setUint32(4, dropped, true);
    let p = 8;
    for (const r of records) {
        const ts = BigInt(r.ts);
        const obj = r.obj !== undefined ? r.obj : (r.arg || 0);
        dv.setUint32(p, Number(ts & 0xffffffffn), true);
        dv.setUint32(p + 4, Number((ts >> 32n) & 0xffffffffn), true);
        dv.setUint32(p + 8, r.tid, true);
        dv.setUint8(p + 12, r.kind);
        dv.setUint8(p + 13, r.code);
        if (version >= 3) {
            dv.setUint16(p + 14, r.reserved !== undefined ? r.reserved : 0, true);
            dv.setUint32(p + 16, obj >>> 0, true);
        } else {
            dv.setUint16(p + 14, obj & 0xffff, true);
        }
        p += recSize;
    }
    return dv.buffer;
}

/** A state transition record. */
export function stateRec(ts, tid, oldState, newState) {
    return { ts, tid, kind: 1, code: newState, reserved: oldState, obj: oldState };
}

/** A sync-primitive marker record. */
export function markRec(ts, tid, prim, act, obj) {
    return { ts, tid, kind: 2, code: ((prim & 0xf) << 4) | (act & 0xf), obj };
}

/** Build a FRAME_PROFILE samples body: the bridge's PROFILE_SUB_SAMPLES
 *  byte followed by an OVE_SIM_PROFILER_EVT_SAMPLES payload. */
export function profilerSamples(samples, wordSize, dropped = 0) {
    let size = 9;
    for (const s of samples) size += 16 + s.pcs.length * wordSize;
    const dv = new DataView(new ArrayBuffer(size));
    dv.setUint8(0, 1); /* sub_type = SAMPLES */
    dv.setUint8(1, 1); /* version */
    dv.setUint8(2, wordSize);
    dv.setUint16(3, samples.length, true);
    dv.setUint32(5, dropped, true);
    let p = 9;
    for (const s of samples) {
        const ts = BigInt(s.ts);
        dv.setUint32(p, Number(ts & 0xffffffffn), true);
        dv.setUint32(p + 4, Number((ts >> 32n) & 0xffffffffn), true);
        dv.setUint32(p + 8, s.tid, true);
        dv.setUint8(p + 12, s.pcs.length);
        dv.setUint8(p + 13, s.state || 0);
        dv.setUint16(p + 14, s.isr || 0, true);
        p += 16;
        for (const pc of s.pcs) {
            const v = BigInt(pc);
            if (wordSize === 2) {
                dv.setUint16(p, Number(v & 0xffffn), true);
            } else {
                dv.setUint32(p, Number(v & 0xffffffffn), true);
                if (wordSize === 8) dv.setUint32(p + 4, Number((v >> 32n) & 0xffffffffn), true);
            }
            p += wordSize;
        }
    }
    return dv.buffer;
}

/** Build a FRAME_PROFILE caps body: the bridge's PROFILE_SUB_CAPS byte
 *  followed by an OVE_SIM_PROFILER_EVT_CAPS payload. */
export function profilerCaps(maxHz, currentHz, flags = 0) {
    const dv = new DataView(new ArrayBuffer(13));
    dv.setUint8(0, 3); /* sub_type = CAPS */
    dv.setUint32(1, maxHz, true);
    dv.setUint32(5, currentHz, true);
    dv.setUint32(9, flags, true);
    return dv.buffer;
}

/** Build a FRAME_PROFILE symbols body: PROFILE_SUB_SYMBOLS and the JSON
 *  [start, end, name] table the bridge builds from the ELF. */
export function profilerSymbols(entries) {
    const json = new TextEncoder().encode(JSON.stringify(entries));
    const dv = new DataView(new ArrayBuffer(1 + json.length));
    dv.setUint8(0, 2); /* sub_type = SYMBOLS */
    new Uint8Array(dv.buffer, 1).set(json);
    return dv.buffer;
}
