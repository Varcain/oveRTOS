/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { loadDashboard } from "./dashboard_harness.mjs";

function pollFramebuffer(format, w, h, pixels, fileSize = 16 + pixels.length) {
    const contents = new Uint8Array(fileSize);
    const header = new DataView(contents.buffer);
    header.setUint32(0, 0x42465854, true);
    header.setUint16(4, w, true);
    header.setUint16(6, h, true);
    header.setUint32(8, format, true);
    header.setUint32(12, 1, true);
    contents.set(pixels, 16);

    const g = loadDashboard({ wasm: true });
    const frames = [];
    g.ws._fs = { lookupPath: () => ({ node: { contents, usedBytes: fileSize } }) };
    g.ws._emit = (buf) => frames.push(buf.slice(0));
    g.ws._pollFramebuffer();
    assert.equal(frames.length, 0);
    g.ws._pollFramebuffer();
    return frames;
}

function framePixels(frames, w, h) {
    assert.equal(frames.length, 1);
    const frame = new DataView(frames[0]);
    assert.equal(frame.getUint32(0, true), 0x01);
    assert.deepEqual([4, 6, 8, 10].map((off) => frame.getUint16(off, true)),
        [0, 0, w - 1, h - 1]);
    assert.equal(frame.byteLength, 12 + w * h * 4);
    return new DataView(frame.buffer, 12);
}

test("framebuffer: RGB565 converts to the native bridge's XRGB8888 order", async (t) => {
    const input = [0x001F, 0xF800, 0x07E0, 0xFFFF, 0x8410, 0];
    const pixels = new DataView(new ArrayBuffer(input.length * 2));
    input.forEach((pixel, i) => pixels.setUint16(i * 2, pixel, true));
    const bytes = new Uint8Array(pixels.buffer);
    const expected = [0xFF0000FF, 0xFFFF0000, 0xFF00FF00, 0xFFFFFFFF, 0xFF838183, 0xFF000000];

    for (const [name, format, fileSize] of [
        ["exact-size file", 0, 16 + bytes.length],
        ["1 MiB file", 0, 1 << 20],
        ["other format falls back to RGB565", 2, 16 + bytes.length],
    ]) {
        await t.test(name, () => {
            const frame = framePixels(pollFramebuffer(format, 3, 2, bytes, fileSize), 3, 2);
            assert.deepEqual(input.map((_, i) => frame.getUint32(i * 4, true)), expected);
        });
    }
});

test("framebuffer: XRGB8888 bytes pass through unchanged", () => {
    const pixels = Uint8Array.of(1, 2, 3, 4, 5, 6, 7, 8);
    const frame = framePixels(pollFramebuffer(1, 2, 1, pixels), 2, 1);
    assert.deepEqual(new Uint8Array(frame.buffer, frame.byteOffset, frame.byteLength), pixels);
});

test("framebuffer: rejects a truncated XRGB8888 frame", function () {
    assert.equal(pollFramebuffer(1, 2, 1, new Uint8Array(4), 16 + 4).length, 0);
});
