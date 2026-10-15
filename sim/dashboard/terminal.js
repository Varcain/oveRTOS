/*
 * oveRTOS browser bundle — terminal for a Linux personality build
 *
 * A personality build has no sim framework, so the dashboard has nothing to
 * show; this page is its console instead.  It boots the firmware in QEMU
 * compiled to WebAssembly with the arguments the board's qemu-run.sh uses,
 * and connects the guest console (QEMU's stdio) to a small terminal: enough
 * of VT100 for BusyBox's line editing, less, top and vi to be usable.
 *
 * web.json, next to the page, says what to run:
 *   { "firmware": "firmware.elf", "emulator": "qemu/",
 *     "files": { "rootfs.cpio": "/rootfs.cpio" }, "args": [ ... ] }
 *
 * "files" maps more downloads to paths in the emulator's filesystem, and
 * "args" is QEMU's whole command line.  window.oveTerm.type() and .text()
 * let a test drive the console.
 */

(function () {
"use strict";

var MAX_LINES = 2000;

var screen = document.getElementById("screen");
var statusEl = document.getElementById("status");

function setStatus(text, running) {
    statusEl.textContent = text;
    statusEl.className = "status" + (running ? " running" : "");
}

/* ── Screen model: lines of text and a cursor ──────────────────────── */

var lines = [""];
var row = 0, col = 0;
var esc = null;                /* null, "" after ESC, or "[..." in a CSI */
var dirty = false;

function setLine(text) {
    lines[row] = text;
}

function put(ch) {
    var line = lines[row];
    while (line.length < col) line += " ";
    setLine(line.slice(0, col) + ch + line.slice(col + 1));
    col++;
}

function newline() {
    row++;
    if (row >= lines.length) lines.push("");
    if (lines.length > MAX_LINES) {
        var drop = lines.length - MAX_LINES;
        lines.splice(0, drop);
        row -= drop;
    }
    col = 0;
}

/* Cursor addressing works on the last ROWS lines, the screen a full-screen
 * program such as top or vi draws on; what is above stays as scrollback. */
var ROWS = 24;

function moveTo(r, c) {
    var top = Math.max(0, lines.length - ROWS);
    row = top + Math.min(Math.max(r, 0), ROWS - 1);
    while (lines.length <= row) lines.push("");
    col = Math.max(c, 0);
}

function csi(seq) {
    var final = seq[seq.length - 1];
    var params = seq.slice(1, -1).replace(/^\?/, "").split(";");
    var n = parseInt(params[0], 10) || 0;
    var top = Math.max(0, lines.length - ROWS);
    switch (final) {
    case "K":                  /* erase in line */
        if (n === 0) setLine(lines[row].slice(0, col));
        else if (n === 2) setLine("");
        break;
    case "J":                  /* erase in display */
        if (n === 0) {
            lines.length = row + 1;
            setLine(lines[row].slice(0, col));
        } else if (n === 2 || n === 3) {
            for (var i = top; i < lines.length; i++) lines[i] = "";
        }
        break;
    case "H": case "f":        /* cursor position, 1-based */
        moveTo((n || 1) - 1, (parseInt(params[1], 10) || 1) - 1);
        break;
    case "A": moveTo(row - top - (n || 1), col); break;
    case "B": moveTo(row - top + (n || 1), col); break;
    case "C": col += n || 1; break;
    case "D": col = Math.max(0, col - (n || 1)); break;
    case "G": col = Math.max(0, (n || 1) - 1); break;
    default: break;            /* colours, modes, scrolling: ignored */
    }
}

function write(code) {
    var ch = String.fromCharCode(code);
    if (esc !== null) {
        if (esc === "") {
            if (ch === "[") { esc = "["; return; }
            esc = null;        /* a two-byte escape: ignored */
            return;
        }
        esc += ch;
        if (code >= 0x40 && code <= 0x7e) { csi(esc); esc = null; }
        else if (esc.length > 32) esc = null;
        return;
    }
    switch (code) {
    case 0x1b: esc = ""; break;
    case 0x0d: col = 0; break;
    case 0x0a: newline(); break;
    case 0x08: col = Math.max(0, col - 1); break;
    case 0x09: do put(" "); while (col % 8); break;
    case 0x07: break;
    default:
        if (code >= 0x20) put(ch);
    }
    dirty = true;
}

function render() {
    if (dirty) {
        dirty = false;
        var stick = screen.scrollTop + screen.clientHeight >= screen.scrollHeight - 4;
        var line = lines[row];
        var before = lines.slice(0, row).join("\n") + (row ? "\n" : "") + line.slice(0, col);
        var cursor = document.createElement("span");
        cursor.className = "cursor";
        cursor.textContent = line.charAt(col) || " ";
        var after = line.slice(col + 1) + (row + 1 < lines.length ? "\n" + lines.slice(row + 1).join("\n") : "");
        screen.replaceChildren(document.createTextNode(before), cursor,
                               document.createTextNode(after));
        if (stick) screen.scrollTop = screen.scrollHeight;
    }
    requestAnimationFrame(render);
}

/* ── Keyboard: bytes for the guest's UART ──────────────────────────── */

var input = [];

function type(text) {
    for (var i = 0; i < text.length; i++) input.push(text.charCodeAt(i) & 0xff);
}

var KEYS = {
    Enter: "\r", Backspace: "\x7f", Tab: "\t", Escape: "\x1b",
    ArrowUp: "\x1b[A", ArrowDown: "\x1b[B", ArrowRight: "\x1b[C", ArrowLeft: "\x1b[D",
    Home: "\x1b[H", End: "\x1b[F", Delete: "\x1b[3~",
    PageUp: "\x1b[5~", PageDown: "\x1b[6~"
};

screen.addEventListener("keydown", function (e) {
    if (e.metaKey) return;
    var seq = null;
    if (e.ctrlKey && !e.altKey && e.key.length === 1) {
        var c = e.key.toUpperCase().charCodeAt(0);
        if (c >= 0x40 && c <= 0x5f) seq = String.fromCharCode(c - 0x40);
        else if (e.key === " ") seq = "\0";
        if (e.key === "c" && window.getSelection().toString()) return;  /* copy */
        if (e.key === "v") return;                                      /* paste */
    } else if (KEYS[e.key]) {
        seq = KEYS[e.key];
    } else if (e.key.length === 1 && !e.ctrlKey) {
        seq = (e.altKey ? "\x1b" : "") + e.key;
    }
    if (seq === null) return;
    type(seq);
    e.preventDefault();
});

screen.addEventListener("paste", function (e) {
    type(e.clipboardData.getData("text").replace(/\r?\n/g, "\r"));
    e.preventDefault();
});

window.oveTerm = {
    type: type,
    text: function () { return lines.join("\n"); }
};

/* ── Emulator ──────────────────────────────────────────────────────── */

async function fetchBytes(url) {
    var resp = await fetch(url, { cache: "no-cache" });
    if (!resp.ok) throw new Error("cannot load " + url);
    return new Uint8Array(await resp.arrayBuffer());
}

async function boot() {
    if (!window.crossOriginIsolated) {
        setStatus("Activating service worker... (page will reload)", false);
        return;
    }
    setStatus("Loading firmware...", false);
    var cfg = await (await fetch("web.json", { cache: "no-cache" })).json();
    var names = Object.keys(cfg.files || {});
    var data = await Promise.all([cfg.firmware].concat(names).map(fetchBytes));

    setStatus("Loading emulator...", false);
    var emulatorUrl = new URL(cfg.emulator + "qemu-system-arm.js", location.href);
    var createQemu = (await import(emulatorUrl.href)).default;

    /* A read returns undefined (EAGAIN) while nothing is queued. */
    function stdinByte() {
        return input.length ? input.shift() : undefined;
    }
    function stderrByte(code) {
        if (code !== null) console.log("qemu: " + String.fromCharCode(code));
    }

    setStatus("Starting emulator...", false);
    await createQemu({
        arguments: cfg.args,
        locateFile: function (path, prefix) {
            if (cfg.wasm && /\.wasm$/.test(path))
                return new URL(cfg.wasm, location.href).href;
            return prefix + path;
        },
        preRun: [function (m) {
            /* Byte-wise stdio, and a readiness op for stdin: without one
             * Emscripten reports it always readable and QEMU busy-reads it. */
            m.FS.init(stdinByte, function (code) { if (code !== null) write(code); },
                      stderrByte);
            m.FS.getStream(0).stream_ops.poll = function () {
                return input.length ? 1 : 0;   /* POLLIN */
            };
            m.FS.writeFile("/firmware.elf", data[0]);
            names.forEach(function (name, i) {
                m.FS.writeFile(cfg.files[name], data[i + 1]);
            });
            setStatus("Running", true);
        }],
        onExit: function (code) {
            setStatus("Firmware exited (" + code + ")", false);
        },
        onAbort: function (what) {
            setStatus("Emulator aborted: " + what, false);
        }
    });
}

requestAnimationFrame(render);
screen.focus();
boot().catch(function (e) {
    setStatus("Failed to start: " + (e && e.message ? e.message : e), false);
});

})();
