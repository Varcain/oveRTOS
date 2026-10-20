/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* Loads sim/dashboard/app.js, a classic script, under a small DOM shim:
 * vm.runInThisContext() puts its declarations on globalThis. */

import fs from "node:fs";
import vm from "node:vm";

const APP_JS = new URL("../../sim/dashboard/app.js", import.meta.url);
const noop = () => {};

/* An element: a few real properties, any other one is a no-op method. */
function fakeElement() {
    const el = { style: {}, dataset: {}, value: "" };
    el.classList = { add: noop, remove: noop, toggle: noop };
    const get = (t, p) => (p in t ? t[p] : typeof p === "symbol" ? undefined : noop);
    return new Proxy(el, { get });
}

export function loadDashboard() {
    Object.assign(globalThis, {
        window: globalThis,
        addEventListener: noop,
        document: {
            getElementById: fakeElement, createElement: fakeElement, querySelectorAll: () => [],
            createTextNode: (text) => ({ textContent: text }), addEventListener: noop,
        },
        location: { protocol: "http:", host: "localhost:8081" },
        WebSocket: class { close() {} send() {} }, /* never connects */
        requestAnimationFrame: noop,
        setInterval: () => 0,
        clearInterval: noop,
    });
    vm.runInThisContext(fs.readFileSync(APP_JS, "utf8"), { filename: "app.js" });
    /* Skip the DOM-heavy first-show panel wiring. */
    globalThis._panelsSeen.trace = true;
    globalThis._panelsSeen.profiler = true;
    return globalThis;
}

/* Wire formats: sim/include/ove/sim/ove_sim_{trace,profiler}.h. Trace
 * payloads start with sub_type, version, count and dropped. */
function traceEnvelope(size, subType, count, dropped, version) {
    const dv = new DataView(new ArrayBuffer(size));
    dv.setUint8(0, subType);
    dv.setUint8(1, version);
    dv.setUint16(2, count, true);
    dv.setUint32(4, dropped, true);
    return dv;
}

/** DESCRIPTORS from [tid, name] or [tid, generation, name] entries. */
export function traceDescriptors(entries, dropped = 0, version = 2) {
    const encoder = new TextEncoder();
    const encoded = entries.map(([tid, gen, name]) =>
        name === undefined ? [tid, 0, encoder.encode(gen)] : [tid, gen, encoder.encode(name)]);
    const hasGen = version >= 2;
    const size = encoded.reduce((n, [, , name]) => n + (hasGen ? 9 : 5) + name.length, 8);
    const dv = traceEnvelope(size, 0, encoded.length, dropped, version);
    let p = 8;
    for (const [tid, gen, name] of encoded) {
        dv.setUint32(p, tid, true); p += 4;
        if (hasGen) { dv.setUint32(p, gen, true); p += 4; }
        dv.setUint8(p++, name.length);
        new Uint8Array(dv.buffer, p, name.length).set(name);
        p += name.length;
    }
    return dv.buffer;
}

/** NAMES from [id, name] entries. */
export function traceNames(entries, dropped = 0) {
    const encoder = new TextEncoder();
    const encoded = entries.map(([id, name]) => [id, encoder.encode(name)]);
    const size = encoded.reduce((n, [, name]) => n + 3 + name.length, 8);
    const dv = traceEnvelope(size, 2, entries.length, dropped, 3);
    let p = 8;
    for (const [id, name] of encoded) {
        dv.setUint16(p, id, true); p += 2;
        dv.setUint8(p++, name.length);
        new Uint8Array(dv.buffer, p, name.length).set(name);
        p += name.length;
    }
    return dv.buffer;
}

/** STREAM of 24-byte v3 or 16-byte legacy records. */
export function traceStream(records, dropped = 0, version = 3) {
    const recSize = version >= 3 ? 24 : 16;
    const dv = traceEnvelope(8 + records.length * recSize, 1, records.length, dropped, version);
    records.forEach((r, i) => {
        const p = 8 + i * recSize;
        dv.setBigUint64(p, BigInt(r.ts), true);
        dv.setUint32(p + 8, r.tid, true);
        dv.setUint8(p + 12, r.kind);
        dv.setUint8(p + 13, r.code);
        dv.setUint16(p + 14, version >= 3 ? (r.reserved ?? 0) : r.obj, true);
        if (version >= 3) dv.setUint32(p + 16, r.obj, true);
    });
    return dv.buffer;
}

export const stateRec = (ts, tid, oldState, newState) =>
    ({ ts, tid, kind: 1, code: newState, reserved: oldState, obj: oldState });
export const markRec = (ts, tid, prim, act, obj) =>
    ({ ts, tid, kind: 2, code: ((prim & 0xf) << 4) | (act & 0xf), obj });

/* FRAME_PROFILE bodies start with the bridge's sub-type byte. */

/** SAMPLES: version, word_size, count, dropped, then the samples. */
export function profilerSamples(samples, wordSize, dropped = 0) {
    const size = samples.reduce((n, s) => n + 16 + s.pcs.length * wordSize, 9);
    const dv = new DataView(new ArrayBuffer(size));
    dv.setUint8(0, 1);
    dv.setUint8(1, 1);
    dv.setUint8(2, wordSize);
    dv.setUint16(3, samples.length, true);
    dv.setUint32(5, dropped, true);
    let p = 9;
    for (const s of samples) {
        dv.setBigUint64(p, BigInt(s.ts), true);
        dv.setUint32(p + 8, s.tid, true);
        dv.setUint8(p + 12, s.pcs.length);
        dv.setUint8(p + 13, s.state || 0);
        dv.setUint16(p + 14, s.isr || 0, true);
        p += 16;
        for (const pc of s.pcs) {
            const value = BigInt(pc);
            if (wordSize === 8) dv.setBigUint64(p, value, true);
            else if (wordSize === 2) dv.setUint16(p, Number(value & 0xffffn), true);
            else dv.setUint32(p, Number(value & 0xffffffffn), true);
            p += wordSize;
        }
    }
    return dv.buffer;
}

/** CAPS: max_hz, current_hz, flags. */
export function profilerCaps(maxHz, currentHz, flags = 0) {
    const dv = new DataView(new ArrayBuffer(13));
    dv.setUint8(0, 3);
    [maxHz, currentHz, flags].forEach((v, i) => dv.setUint32(1 + 4 * i, v, true));
    return dv.buffer;
}

/** SYMBOLS: a JSON [start, end, name] table. */
export function profilerSymbols(entries) {
    const json = new TextEncoder().encode(JSON.stringify(entries));
    const out = new Uint8Array(1 + json.length);
    out[0] = 2;
    out.set(json, 1);
    return out.buffer;
}
