/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const source = fs.readFileSync(new URL("../../sim/dashboard/vt.js", import.meta.url), "utf8");
const encoder = new TextEncoder();

function terminal() {
    const on = {}, sent = [], frames = [];
    const screen = {
        scrollTop: 0, clientHeight: 0, scrollHeight: 0,
        addEventListener: (type, fn) => (on[type] = fn),
        replaceChildren: (...nodes) => (screen.nodes = nodes),
    };
    const window = { getSelection: () => screen.selection || "" };
    vm.runInNewContext(source, { window, TextDecoder, TextEncoder,
        requestAnimationFrame: (fn) => frames.push(fn),
        document: { createElement: () => ({}), createTextNode: (textContent) => ({ textContent }) },
    }, { filename: "vt.js" });
    const vt = window.oveVt(screen, (bytes) => sent.push(...bytes));
    return { vt, screen, sent, frames, on, out: (text) => vt.write(encoder.encode(text)),
        flush: () => frames.splice(0).forEach((fn) => fn()) };
}

test("vt: line discipline and erase/cursor sequences", () => {
    const t = terminal();
    t.out("one\r\ntwo\rT\x1b[K\r\n\tx\x08y\x1b[1;3Hz");
    assert.equal(t.vt.text(), "onz\nT\n        y");
});

test("vt: UTF-8 split across writes, and bytes >= 0x80", () => {
    const t = terminal();
    t.vt.write(Uint8Array.of(0x41, 0xc3));
    t.vt.write(Uint8Array.of(0xa9, 0x42));
    assert.equal(t.vt.text(), "AéB");
});

test("vt: renders once per frame, also after a cursor-only update", () => {
    const t = terminal();
    t.out("abc");
    t.out("\x1b[2");
    assert.equal(t.frames.length, 1);
    t.flush();
    assert.equal(t.frames.length, 0);
    assert.equal(t.screen.nodes[0].textContent, "abc");
    t.out("D");
    t.flush();
    assert.deepEqual(t.screen.nodes.map((node) => node.textContent), ["a", "b", "c"]);
    assert.equal(t.screen.nodes[1].className, "cursor");
});

test("vt: keys send serial bytes and stay in the terminal", () => {
    const t = terminal();
    const ctrl = { ctrlKey: true };
    for (const [key, handled, mods] of [
        ["Enter", true], ["ArrowUp", true], ["c", true, ctrl], ["é", true],
        ["F5", false], ["k", false, { metaKey: true }], ["c", false, ctrl], ["v", false, ctrl],
    ]) {
        t.screen.selection = handled ? "" : "copied";
        const event = Object.assign(new Event("keydown", { cancelable: true }), { key, ...mods });
        t.on.keydown(event);
        assert.equal(event.defaultPrevented, handled, key);
        assert.equal(event.cancelBubble, handled, key);
    }
    assert.deepEqual(t.sent, [0x0d, 0x1b, 0x5b, 0x41, 0x03, 0xc3, 0xa9]);
    t.vt.type("é");
    t.on.paste({ clipboardData: { getData: () => "a\nb\r\nc" }, preventDefault() {} });
    assert.deepEqual(t.sent.slice(7), [0xc3, 0xa9, 0x61, 0x0d, 0x62, 0x0d, 0x63]);
});
