/*
 * oveRTOS Simulation Dashboard — in-page firmware link (browser build)
 *
 * The native dashboard talks to ove-dashboard-bridge.py over a WebSocket;
 * the bridge relays between that socket and the shared-memory files the
 * firmware reads and writes through semihosting.  In the browser build the
 * firmware runs right here, in QEMU compiled to WebAssembly, and those same
 * files live in Emscripten's in-memory filesystem on this thread.  This
 * file is the bridge for that case: it boots the emulator and provides
 * window.oveCreateLink(), an object with the WebSocket surface app.js uses,
 * speaking the same frames.
 *
 * Covered: console log, framebuffer, pointer input, plugin events (threads,
 * trace) and plugin commands (console input included).
 * Not covered: audio, the sampling profiler and the debugger.
 *
 * web.json, next to the page, says what to run:
 *   { "firmware": "firmware.elf", "emulator": "qemu/",
 *     "machine": "mps2-an500", "args": [] }
 */

(function () {
"use strict";

/* ── Frame types (shared with app.js and ove-dashboard-bridge.py) ──── */
var FRAME_FB     = 0x01;
var FRAME_CMD    = 0x04;
var FRAME_LOG    = 0x06;
var FRAME_INPUT  = 0x07;
var FRAME_THREAD = 0x0A;
var FRAME_TRACE  = 0x0E;

/* ── Plugin event/command file (sim/include/ove/sim/ove_sim_shm.h) ─── */
var SIM_PATH           = "/dev/shm/ove-sim";
var SIM_MAGIC          = 0x4F565349;   /* "OVSI" */
var SIM_HDR_SIZE       = 64;
var SIM_RING_SIZE      = 1 << 16;
var SIM_EVENT_RING_OFF = SIM_HDR_SIZE;
var SIM_CMD_RING_OFF   = SIM_HDR_SIZE + SIM_RING_SIZE;
var SIM_TOTAL_SIZE     = SIM_HDR_SIZE + 2 * SIM_RING_SIZE;
var SIM_OFF_EVENT_WPOS = 16;
var SIM_OFF_CMD_WPOS   = 24;
var SIM_OFF_CMD_RPOS   = 28;
var SIM_EVENT_HDR_SIZE = 16;           /* struct ove_sim_event header */
var SIM_EVENT_MAX      = 4096;

var EVT_DEBUG_THREADS     = 0;         /* OVE_SIM_DEBUG_EVT_THREADS */
var EVT_TRACE_STREAM      = 10;        /* OVE_SIM_TRACE_EVT_STREAM */
var EVT_TRACE_DESCRIPTORS = 11;        /* OVE_SIM_TRACE_EVT_DESCRIPTORS */

/* ── Framebuffer file: 16-byte header + XRGB8888 pixels ────────────── */
var FB_PATH     = "/dev/shm/ove-fb";
var FB_MAGIC    = 0x42465854;          /* "TXFB" */
var FB_HDR_SIZE = 16;
var FB_OFF_DIRTY = 12;
var FB_FILE_SIZE = 1 << 20;

/* ── Pointer file: magic, int16 x, int16 y, uint8 pressed ──────────── */
var INPUT_PATH  = "/dev/shm/ove-input";
var INPUT_MAGIC = 0x54504E49;          /* "INPT" */
var INPUT_SIZE  = 16;

var AUDIO_PATH = "/dev/shm/ove-audio";
var AUDIO_FILE_SIZE = 2 * (32 + (1 << 16));

var EVENT_POLL_MS = 30;

var statusEl = document.getElementById("status");
function setStatus(text, ok) {
    if (!statusEl) return;
    statusEl.textContent = text;
    statusEl.className = "status " + (ok ? "connected" : "disconnected");
}

/* ── The link object handed to app.js ──────────────────────────────── */

function Link() {
    this.readyState = 0;               /* WebSocket.CONNECTING */
    this.onopen = this.onclose = this.onerror = this.onmessage = null;
    this._fs = null;
    this._sim = null;                  /* open stream on SIM_PATH */
    this._input = null;                /* open stream on INPUT_PATH */
    this._eventRpos = 0;
    this._fbPending = false;
    this._fbFrame = null;              /* reused FRAME_FB buffer */

    var self = this;
    boot(self).catch(function (e) {
        self._log("[web] " + (e && e.message ? e.message : e));
        setStatus("Failed to start", false);
    });
}

Link.prototype.close = function () {};

Link.prototype._emit = function (buf) {
    if (this.onmessage) this.onmessage({ data: buf });
};

Link.prototype._log = function (text) {
    var bytes = new TextEncoder().encode(text);
    var buf = new ArrayBuffer(4 + bytes.length);
    new DataView(buf).setUint32(0, FRAME_LOG, true);
    new Uint8Array(buf, 4).set(bytes);
    this._emit(buf);
};

/* ── Emulator boot ─────────────────────────────────────────────────── */

async function boot(link) {
    if (!window.crossOriginIsolated) {
        /* Static hosts cannot send the isolation headers the emulator's
         * threads need; coi-serviceworker.js adds them and reloads. */
        setStatus("Activating service worker... (page will reload)", false);
        return;
    }

    setStatus("Loading firmware...", false);
    var cfg = await (await fetch("web.json", { cache: "no-cache" })).json();
    var firmwareResp = await fetch(cfg.firmware, { cache: "no-cache" });
    if (!firmwareResp.ok)
        throw new Error("cannot load " + cfg.firmware);
    var firmware = new Uint8Array(await firmwareResp.arrayBuffer());

    setStatus("Loading emulator...", false);
    var emulatorUrl = new URL(cfg.emulator + "qemu-system-arm.js", location.href);
    var createQemu = (await import(emulatorUrl.href)).default;

    var args = [
        "-M", cfg.machine || "mps2-an500",
        "-nographic", "-monitor", "none",
        "-kernel", "/firmware.elf",
        "-semihosting-config", "enable=on,target=native,arg=" + FB_PATH,
        /* The firmware is small; the default translation cache alone
         * would take hundreds of megabytes of browser memory. */
        "-accel", "tcg,tb-size=32"
    ].concat(cfg.args || []);

    function consoleLine(text) {
        /* Emscripten stubs for syscalls QEMU probes but does not need. */
        if (text.indexOf("warning: unsupported syscall") === 0) return;
        if (text === "") return;
        link._log(text);
    }

    setStatus("Starting emulator...", false);
    var module = await createQemu({
        arguments: args,
        print: consoleLine,
        printErr: consoleLine,
        stdin: function () { return null; },
        preRun: [function (m) {
            m.FS.writeFile("/firmware.elf", firmware);
            /* The files qemu-run.sh creates before a native run. */
            m.FS.writeFile(FB_PATH, new Uint8Array(FB_FILE_SIZE));
            m.FS.writeFile(SIM_PATH, new Uint8Array(SIM_TOTAL_SIZE));
            m.FS.writeFile(AUDIO_PATH, new Uint8Array(AUDIO_FILE_SIZE));
            var input = new Uint8Array(INPUT_SIZE);
            new DataView(input.buffer).setUint32(0, INPUT_MAGIC, true);
            m.FS.writeFile(INPUT_PATH, input);
        }],
        onExit: function (code) {
            link._log("[web] emulator exited (" + code + ")");
            setStatus("Firmware exited", false);
        },
        onAbort: function (what) {
            link._log("[web] emulator aborted: " + what);
            setStatus("Emulator aborted", false);
        }
    });

    link._fs = module.FS;
    link._sim = module.FS.open(SIM_PATH, "r+");
    link._input = module.FS.open(INPUT_PATH, "r+");

    link.readyState = 1;               /* WebSocket.OPEN */
    if (link.onopen) link.onopen();

    setInterval(function () { link._pollEvents(); }, EVENT_POLL_MS);
    (function frameLoop() {
        link._pollFramebuffer();
        requestAnimationFrame(frameLoop);
    })();
}

/* ── File access ───────────────────────────────────────────────────────
 * The firmware opens these files through semihosting with truncation and
 * rewrites them as it goes, so a file's backing array is replaced while
 * it grows: look it up again on every poll and trust usedBytes. */

function fileView(fs, path) {
    var node;
    try {
        node = fs.lookupPath(path).node;
    } catch (e) {
        return null;
    }
    var c = node.contents;
    if (!c || !node.usedBytes) return null;
    return { bytes: c, size: node.usedBytes,
             view: new DataView(c.buffer, c.byteOffset, node.usedBytes) };
}

/* ── Framebuffer: firmware -> dashboard ────────────────────────────── */

Link.prototype._pollFramebuffer = function () {
    var f = fileView(this._fs, FB_PATH);
    if (!f || f.size < FB_HDR_SIZE) return;
    if (f.view.getUint32(0, true) !== FB_MAGIC ||
        !f.view.getUint32(FB_OFF_DIRTY, true)) {
        this._fbPending = false;
        return;
    }
    /* The header (dirty flag included) lands one write before the pixels:
     * take the frame on the poll after the one that first saw it. */
    if (!this._fbPending) {
        this._fbPending = true;
        return;
    }
    this._fbPending = false;

    var w = f.view.getUint16(4, true), h = f.view.getUint16(6, true);
    var len = w * h * 4;
    if (!w || !h || f.size < FB_HDR_SIZE + len) return;
    f.view.setUint32(FB_OFF_DIRTY, 0, true);

    if (!this._fbFrame || this._fbFrame.byteLength !== 12 + len)
        this._fbFrame = new ArrayBuffer(12 + len);
    var out = new DataView(this._fbFrame);
    out.setUint32(0, FRAME_FB, true);
    out.setUint16(4, 0, true);
    out.setUint16(6, 0, true);
    out.setUint16(8, w - 1, true);
    out.setUint16(10, h - 1, true);
    new Uint8Array(this._fbFrame, 12).set(
        f.bytes.subarray(FB_HDR_SIZE, FB_HDR_SIZE + len));
    this._emit(this._fbFrame);
};

/* ── Plugin events: firmware -> dashboard ──────────────────────────── */

Link.prototype._pollEvents = function () {
    var f = fileView(this._fs, SIM_PATH);
    if (!f || f.size < SIM_HDR_SIZE) return;
    if (f.view.getUint32(0, true) !== SIM_MAGIC) return;

    var mask = SIM_RING_SIZE - 1;
    var wpos = f.view.getUint32(SIM_OFF_EVENT_WPOS, true);
    var bytes = f.bytes, size = f.size;

    function ringByte(pos) {
        var off = SIM_EVENT_RING_OFF + (pos & mask);
        return off < size ? bytes[off] : 0;
    }

    for (;;) {
        var avail = (wpos - this._eventRpos) >>> 0;
        if (avail < 2) return;
        if (avail > SIM_RING_SIZE) {    /* lapped: resynchronise */
            this._eventRpos = wpos;
            return;
        }

        var rpos = this._eventRpos;
        var len = ringByte(rpos) | (ringByte(rpos + 1) << 8);
        rpos = (rpos + 2) >>> 0;
        this._eventRpos = (rpos + len) >>> 0;
        if (len < SIM_EVENT_HDR_SIZE || len > SIM_EVENT_MAX) continue;

        var msg = new Uint8Array(len);
        for (var i = 0; i < len; i++) msg[i] = ringByte(rpos + i);
        var hdr = new DataView(msg.buffer);
        var eventType = hdr.getUint32(4, true);
        var dataLen = hdr.getUint32(12, true);
        if (!dataLen || SIM_EVENT_HDR_SIZE + dataLen > len) continue;

        var frameType;
        if (eventType === EVT_DEBUG_THREADS)
            frameType = FRAME_THREAD;
        else if (eventType === EVT_TRACE_STREAM ||
                 eventType === EVT_TRACE_DESCRIPTORS)
            frameType = FRAME_TRACE;
        else
            continue;

        var frame = new ArrayBuffer(4 + dataLen);
        new DataView(frame).setUint32(0, frameType, true);
        new Uint8Array(frame, 4).set(
            msg.subarray(SIM_EVENT_HDR_SIZE, SIM_EVENT_HDR_SIZE + dataLen));
        this._emit(frame);
    }
};

/* ── Dashboard -> firmware ─────────────────────────────────────────── */

Link.prototype.send = function (buf) {
    if (this.readyState !== 1 || !(buf instanceof ArrayBuffer) ||
        buf.byteLength < 4)
        return;
    var view = new DataView(buf);
    var type = view.getUint32(0, true);

    if (type === FRAME_INPUT && buf.byteLength >= 9) {
        /* int16 x, int16 y, uint8 pressed — at offset 4 in both. */
        this._fs.write(this._input, new Uint8Array(buf, 4, 5), 0, 5, 4);
    } else if (type === FRAME_CMD && buf.byteLength >= 16) {
        /* [plugin_id][cmd_type][data_len][data] is already the layout of
         * struct ove_sim_cmd; the ring wants it behind a uint16 length. */
        this._sendCmd(new Uint8Array(buf, 4));
    }
};

Link.prototype._sendCmd = function (cmd) {
    var f = fileView(this._fs, SIM_PATH);
    if (!f || f.size < SIM_HDR_SIZE || cmd.length > 0xFFFF) return;
    if (f.view.getUint32(0, true) !== SIM_MAGIC) return;

    var wpos = f.view.getUint32(SIM_OFF_CMD_WPOS, true);
    var rpos = f.view.getUint32(SIM_OFF_CMD_RPOS, true);
    if (((wpos - rpos) >>> 0) + 2 + cmd.length > SIM_RING_SIZE) return;

    var msg = new Uint8Array(2 + cmd.length);
    msg[0] = cmd.length & 0xFF;
    msg[1] = cmd.length >> 8;
    msg.set(cmd, 2);

    /* The command ring lies past what the firmware has written so far;
     * writing through the filesystem extends the file as needed. */
    var mask = SIM_RING_SIZE - 1;
    var pos = wpos & mask;
    var first = Math.min(SIM_RING_SIZE - pos, msg.length);
    this._fs.write(this._sim, msg, 0, first, SIM_CMD_RING_OFF + pos);
    if (first < msg.length)
        this._fs.write(this._sim, msg, first, msg.length - first, SIM_CMD_RING_OFF);

    var newWpos = new Uint8Array(4);
    new DataView(newWpos.buffer).setUint32(0, (wpos + msg.length) >>> 0, true);
    this._fs.write(this._sim, newWpos, 0, 4, SIM_OFF_CMD_WPOS);
};

window.oveCreateLink = function () { return new Link(); };

})();
