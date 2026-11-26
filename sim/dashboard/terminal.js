/*
 * oveRTOS browser bundle — terminal for a Linux personality build
 *
 * Linux browser bundles use this terminal for the guest console. The
 * simulation framework is off by default in their firmware.
 * It boots the firmware in WebAssembly QEMU with the arguments from the
 * board's qemu-run.sh and connects QEMU's stdio to the terminal.
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

var screen = document.getElementById("screen");
var statusEl = document.getElementById("status");

function setStatus(text, running) {
    statusEl.textContent = text;
    statusEl.className = "status" + (running ? " running" : "");
}

var input = [];
var vt = window.oveVt(screen, function (bytes) {
    for (var i = 0; i < bytes.length; i++) input.push(bytes[i]);
});

window.oveTerm = { type: vt.type, text: vt.text };

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
            m.FS.init(stdinByte, function (code) {
                if (code !== null) vt.write(Uint8Array.of(code & 0xff));
            }, stderrByte);
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

screen.focus();
boot().catch(function (e) {
    setStatus("Failed to start: " + (e && e.message ? e.message : e), false);
});

})();
