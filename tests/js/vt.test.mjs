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
        contains: (node) => node === screen || screen.nodes?.includes(node),
    };
    const window = new EventTarget();
    window.getSelection = () => screen.selection || { toString: () => "" };
    vm.runInNewContext(source, { window, TextDecoder, TextEncoder,
        requestAnimationFrame: (fn) => frames.push(fn),
        document: { createElement: () => ({}), createTextNode: (textContent) => ({ textContent }) },
    }, { filename: "vt.js" });
    const vt = window.oveVt(screen, (bytes) => sent.push(...bytes));
    return { vt, screen, sent, frames, on, out: (text) => vt.write(encoder.encode(text)),
        flush: () => frames.splice(0).forEach((fn) => fn()),
        resize: () => window.dispatchEvent(new Event("resize")) };
}

test("vt: line discipline and erase/cursor sequences", () => {
    const t = terminal();
    t.out("one\r\ntwo\rT\x1b[K\r\n\tx\x08y\x1b[1;3Hz");
    assert.equal(t.vt.text(), "onz\nT\n        y");
});

const clear = "\x1b[H\x1b[J";
const frame = (k) => `\x1b[HMem: ${k}\x1b[K\r\n  1 init\x1b[K\x1b[J\r`;
const old = (n) => Array.from({ length: n }, (_, i) => `old ${i}\r\n`).join("");

test("vt: BusyBox clear drops scrollback before top redraws", () => {
    const t = terminal();
    t.out(old(40) + "# clear\r\n" + clear + "# top\r\n");
    assert.equal(t.vt.text(), "# top\n");
    t.out(frame(1) + frame(2));
    assert.equal(t.vt.text(), "Mem: 2\n  1 init");
});

test("vt: short top frames redraw in place and preserve scrollback", () => {
    const t = terminal();
    t.out(old(40) + "# top\r\n" + frame(1) + frame(2) + frame(3));
    const kept = Array.from({ length: 18 }, (_, i) => `old ${i}`);
    assert.equal(t.vt.text(), [...kept, "Mem: 3", "  1 init"].join("\n"));
    t.out("\x1b[1;3H\x1b[J");
    assert.equal(t.vt.text(), [...kept, "Me"].join("\n"));
});

test("vt: screen origin follows scrollback trimming and reset", () => {
    const t = terminal();
    t.out(old(2100) + "\x1b[Hx\x1b[K");
    assert.equal(t.vt.text().split("\n").at(-24), "x");

    t.out(old(40));
    t.vt.reset();
    t.out("\x1b[Hx");
    assert.equal(t.vt.text(), "x");
});

test("vt: vi restores the shell screen and scrollback on exit", () => {
    const t = terminal();
    t.out(old(40) + "# vi f\r\n");
    const before = t.vt.text();
    t.out("\x1b[?1049h" + clear + "\x1b[1;1Hhello\x1b[K\x1b[24;1H\"f\" 1L\x1b[K");
    assert.equal(t.vt.text().split("\n")[0], "hello");
    t.out("\x1b[24;1H\x1b[K\x1b[?1049l# ");
    assert.equal(t.vt.text(), before + "# ");
    // Home first so the newline cannot recompute the origin from the restored cursor row.
    t.out("\x1b[H\r\n" + frame(1));
    assert.equal(t.vt.text(), old(18).replaceAll("\r", "") + "Mem: 1\n  1 init");
});

test("vt: UTF-8 split across writes, and bytes >= 0x80", () => {
    const t = terminal();
    t.vt.write(Uint8Array.of(0x41, 0xc3));
    t.vt.write(Uint8Array.of(0xa9, 0x42));
    assert.equal(t.vt.text(), "AéB");
});

test("vt: reset clears the screen, escape state and incomplete UTF-8", () => {
    const t = terminal();
    t.out("one\r\ntwo\x1b[");
    t.flush();
    t.screen.scrollHeight = 30;
    t.on.scroll();
    t.vt.reset();
    t.flush();
    assert.equal(t.screen.nodes.map((node) => node.textContent).join(""), " ");
    assert.equal(t.screen.scrollTop, 30);
    t.out("K");
    assert.equal(t.vt.text(), "K");
    t.vt.write(Uint8Array.of(0x0d, 0x0a, 0xc3));
    t.vt.reset();
    t.out("x");
    assert.equal(t.vt.text(), "x");
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
        ["Enter", true], ["Tab", true], ["ArrowUp", true], ["c", true, ctrl], ["é", true],
        ["F5", false], ["F6", false], ["F10", false], ["F11", false],
        ["F11", false, { shiftKey: true }], ["k", false, { metaKey: true }], ["v", false, ctrl],
    ]) {
        const event = Object.assign(new Event("keydown", { cancelable: true }), { key, ...mods });
        t.on.keydown(event);
        assert.equal(event.defaultPrevented, handled, key);
        assert.equal(event.cancelBubble, handled, key);
    }
    assert.deepEqual(t.sent, [0x0d, 0x09, 0x1b, 0x5b, 0x41, 0x03, 0xc3, 0xa9]);
    t.vt.type("é");
    t.on.paste({ clipboardData: { getData: () => "a\nb\r\nc" }, preventDefault() {} });
    assert.deepEqual(t.sent.slice(8), [0xc3, 0xa9, 0x61, 0x0d, 0x62, 0x0d, 0x63]);
});

test("vt: Ctrl-C copies only a selection inside the terminal", () => {
    const t = terminal();
    t.out("terminal text");
    t.flush();
    const outside = {};
    const inside = t.screen.nodes[0];
    for (const [anchorNode, focusNode, copied] of [
        [outside, outside, false], [inside, outside, false], [inside, inside, true],
    ]) {
        t.screen.selection = { anchorNode, focusNode, toString: () => "selected text" };
        const event = Object.assign(new Event("keydown", { cancelable: true }), { key: "c", ctrlKey: true });
        t.on.keydown(event);
        assert.equal(event.defaultPrevented, !copied);
    }
    assert.deepEqual(t.sent, [0x03, 0x03]);
});

test("vt: follow survives a layout scroll reset and pauses on user scroll", () => {
    const t = terminal();
    t.screen.clientHeight = 20;
    t.screen.scrollHeight = 100;
    t.screen.scrollTop = 80;
    t.out("one");
    t.flush();
    t.screen.scrollTop = 80;  /* Browser clamps the programmatic scroll to the bottom. */
    t.on.scroll?.();
    t.screen.scrollTop = 0;   /* A layout move resets the scroll position. */
    t.out("two");
    t.flush();
    assert.equal(t.screen.scrollTop, 100);
    t.screen.scrollTop = 10;
    t.on.scroll();
    t.out("three");
    t.flush();
    assert.equal(t.screen.scrollTop, 10);
});

test("vt: resize restores following after a layout move without new output", () => {
    const t = terminal();
    t.screen.clientHeight = 20;
    t.screen.scrollHeight = 100;
    t.screen.scrollTop = 80;
    t.on.scroll();
    t.screen.scrollTop = 0;
    t.resize();
    assert.equal(t.screen.scrollTop, 100);

    t.screen.scrollTop = 10;
    t.on.scroll();
    t.resize();
    assert.equal(t.screen.scrollTop, 10);
});
