/*
 * oveRTOS Simulation Dashboard
 *
 * Shared between POSIX (WebSocket) and WASM (Emscripten) modes.
 * Provides: event log, WinBox window management, audio waveform (POSIX).
 */

"use strict";

/* ── Constants matching ove_sim_ws.h ───────────────────────────────── */
var FRAME_FB    = 0x01;
var FRAME_AUDIO = 0x02;
var FRAME_EVENT = 0x03;
var FRAME_CMD   = 0x04;
var FRAME_STATE = 0x05;
var FRAME_LOG   = 0x06;

/* ── DOM elements (may be null in WASM mode for missing panels) ───── */
var statusEl     = document.getElementById("status");
var canvas       = document.getElementById("display-canvas");
var ctx          = canvas ? canvas.getContext("2d") : null;
var resolutionEl = document.getElementById("display-resolution");
var fpsEl        = document.getElementById("display-fps");
var audioToggle  = document.getElementById("audio-toggle");
var audioInfoEl  = document.getElementById("audio-info");
var waveCanvas   = document.getElementById("audio-waveform");
var waveCtx      = waveCanvas ? waveCanvas.getContext("2d") : null;
var eventLog     = document.getElementById("event-log");

/* ── State ─────────────────────────────────────────────────────────── */
var ws = null;
var displayWidth = 0;
var displayHeight = 0;
var frameCount = 0;
var lastFpsTime = performance.now();
var lastFpsCount = 0;

/* Audio (POSIX mode only) */
var audioCtx = null;
var audioEnabled = false;
var audioSampleRate = 44100;
var audioChannels = 1;
var audioBitDepth = 16;

/* ── Floating window layout (WinBox) ──────────────────────────────── */
var LAYOUT_KEY = "ove-sim-dashboard-layout";
var MOBILE_BP  = 640;  /* viewport width below which we stack full-width */

/* Proportional defaults: fractions of usable area. */
var WIN_DEFS = {
    display: { title: "Display", col: 0, row: 0, cw: 1,   rh: 0.6 },
    audio:   { title: "Audio",   col: 0, row: 1, cw: 0.5, rh: 0.4 },
    console: { title: "Console", col: 0, row: 1, cw: 0.5, rh: 0.4 },
    events:  { title: "Events",  col: 1, row: 1, cw: 0.5, rh: 0.4 },
};

function isMobile() { return window.innerWidth < MOBILE_BP; }

/**
 * Compute tiled layout rects for all currently open windows.
 * Returns { id: {x, y, width, height}, ... }.
 *
 * 1 window  → full area
 * 2 windows → split left/right (PC) or top/bottom (mobile)
 * 3 windows → first takes left half, other two stacked on right half
 * 4+ windows → 2-column grid
 */
function computeLayout() {
    var ids = [];
    for (var id in wins) ids.push(id);
    var n = ids.length;
    if (n === 0) return {};

    var vw = window.innerWidth, vh = window.innerHeight;
    var usableH = vh - HEADER_H - TASKBAR_H;
    var mobile = isMobile();
    var layout = {};

    if (n === 1) {
        layout[ids[0]] = { x: GAP, y: HEADER_H + GAP,
            width: vw - GAP * 2, height: usableH - GAP * 2 };
    } else if (n === 2) {
        if (mobile) {
            var h = (usableH - GAP * 3) / 2;
            layout[ids[0]] = { x: GAP, y: HEADER_H + GAP, width: vw - GAP * 2, height: h };
            layout[ids[1]] = { x: GAP, y: HEADER_H + GAP * 2 + h, width: vw - GAP * 2, height: h };
        } else {
            var w = (vw - GAP * 3) / 2;
            layout[ids[0]] = { x: GAP, y: HEADER_H + GAP, width: w, height: usableH - GAP * 2 };
            layout[ids[1]] = { x: GAP * 2 + w, y: HEADER_H + GAP, width: w, height: usableH - GAP * 2 };
        }
    } else if (n === 3 && !mobile) {
        var lw = (vw - GAP * 3) / 2;
        var rh = (usableH - GAP * 3) / 2;
        layout[ids[0]] = { x: GAP, y: HEADER_H + GAP, width: lw, height: usableH - GAP * 2 };
        layout[ids[1]] = { x: GAP * 2 + lw, y: HEADER_H + GAP, width: lw, height: rh };
        layout[ids[2]] = { x: GAP * 2 + lw, y: HEADER_H + GAP * 2 + rh, width: lw, height: rh };
    } else {
        /* Grid: 2 columns on PC, 1 column on mobile. */
        var cols = mobile ? 1 : 2;
        var rows = Math.ceil(n / cols);
        var cw = (vw - GAP * (cols + 1)) / cols;
        var rh = (usableH - GAP * (rows + 1)) / rows;
        for (var i = 0; i < n; i++) {
            var c = i % cols, r = Math.floor(i / cols);
            layout[ids[i]] = {
                x: GAP + c * (cw + GAP),
                y: HEADER_H + GAP + r * (rh + GAP),
                width: cw, height: rh
            };
        }
    }
    return layout;
}

/** Get default rect for a single window (used when first created). */
function defaultRect(def) {
    /* If this is the only window, give it full area. Otherwise use
       the proportional hints from WIN_DEFS as a fallback. */
    var openCount = Object.keys(wins).length;
    var vw = window.innerWidth, vh = window.innerHeight;
    var usableH = vh - HEADER_H - TASKBAR_H;

    if (openCount === 0) {
        /* First window — maximize. */
        return { x: GAP, y: HEADER_H + GAP,
            width: vw - GAP * 2, height: usableH - GAP * 2 };
    }
    /* Fallback: proportional placement from WIN_DEFS. */
    var hw = vw * def.cw;
    var hh = usableH * def.rh;
    return {
        x: def.col * (vw / 2) + GAP,
        y: HEADER_H + def.row * (usableH * 0.6) + GAP,
        width: Math.max(hw - GAP * 2, 200),
        height: Math.max(hh - GAP * 2, 80),
    };
}

/** Clamp a saved rect so it stays visible in the current viewport. */
function clampRect(opts) {
    var vw = window.innerWidth, vh = window.innerHeight;
    var maxW = vw - GAP * 2, maxH = vh - HEADER_H - TASKBAR_H - GAP * 2;
    var w = Math.min(opts.width, maxW);
    var h = Math.min(opts.height, maxH);
    var x = Math.max(0, Math.min(opts.x, vw - w));
    var y = Math.max(HEADER_H, Math.min(opts.y, vh - TASKBAR_H - h));
    return { x: x, y: y, width: w, height: h };
}

var wins = {};
var badges = {};

/* ── Bottom taskbar ───────────────────────────────────────────────── */
var taskbar = document.createElement("div");
taskbar.id = "taskbar";
document.body.appendChild(taskbar);

function addBadge(id, title) {
    var b = document.createElement("div");
    b.className = "taskbar-badge";
    b.textContent = title;
    b.addEventListener("click", function () {
        var w = wins[id];
        if (!w) return;
        if (w.min) w.minimize(false);
        w.focus();
    });
    taskbar.appendChild(b);
    badges[id] = b;
}

function updateBadges() {
    for (var id in badges) {
        var w = wins[id];
        if (!w) continue;
        badges[id].className = "taskbar-badge" +
            (w.focused ? " active" : "") +
            (w.min ? " minimized" : "");
    }
}

/* ── Edge tiling & magnetic snap ──────────────────────────────────── */
var EDGE_PX     = 6;   /* cursor pixels from viewport edge to trigger tile */
var SNAP_PX     = 10;  /* magnetic snap distance between windows */
var HEADER_H    = 50;  /* height of page header (top boundary) */
var TASKBAR_H   = 34;  /* height of bottom taskbar */
var GAP         = 8;   /* margin around tiled windows */
var draggingId  = null;
var snapPreview = null;
var _snapping   = false;

function ensurePreview() {
    if (snapPreview) return snapPreview;
    var el = document.createElement("div");
    el.style.cssText =
        "position:fixed;z-index:99999;background:rgba(78,204,163,0.12);" +
        "border:2px dashed rgba(78,204,163,0.5);border-radius:8px;" +
        "pointer-events:none;display:none;transition:all .15s ease;";
    document.body.appendChild(el);
    snapPreview = el;
    return el;
}

function tileZone(cx, cy) {
    var vw = window.innerWidth, vh = window.innerHeight;
    var atLeft  = cx <= EDGE_PX;
    var atRight = cx >= vw - EDGE_PX;
    var atTop   = cy <= EDGE_PX + HEADER_H;
    var atBot   = cy >= vh - EDGE_PX;

    /* Corners → quarter tiles */
    if (atLeft  && atTop) return "tl";
    if (atRight && atTop) return "tr";
    if (atLeft  && atBot) return "bl";
    if (atRight && atBot) return "br";
    /* Edges → half / full tiles */
    if (atLeft)  return "left";
    if (atRight) return "right";
    if (atTop)   return "top";
    return null;
}

function tileRect(zone) {
    var vw = window.innerWidth, vh = window.innerHeight;
    var h = vh - HEADER_H - TASKBAR_H;
    var hw = vw / 2, hh = h / 2;
    switch (zone) {
        case "left":  return { x: GAP, y: HEADER_H + GAP, w: hw - GAP * 2, h: h - GAP * 2 };
        case "right": return { x: hw + GAP, y: HEADER_H + GAP, w: hw - GAP * 2, h: h - GAP * 2 };
        case "top":   return { x: GAP, y: HEADER_H + GAP, w: vw - GAP * 2, h: h - GAP * 2 };
        case "tl":    return { x: GAP, y: HEADER_H + GAP, w: hw - GAP * 2, h: hh - GAP * 2 };
        case "tr":    return { x: hw + GAP, y: HEADER_H + GAP, w: hw - GAP * 2, h: hh - GAP * 2 };
        case "bl":    return { x: GAP, y: HEADER_H + hh + GAP, w: hw - GAP * 2, h: hh - GAP * 2 };
        case "br":    return { x: hw + GAP, y: HEADER_H + hh + GAP, w: hw - GAP * 2, h: hh - GAP * 2 };
    }
    return null;
}

function showPreview(zone) {
    var el = ensurePreview();
    var r = tileRect(zone);
    if (!r) return;
    el.style.display = "block";
    el.style.left   = r.x + "px";
    el.style.top    = r.y + "px";
    el.style.width  = r.w + "px";
    el.style.height = r.h + "px";
}

function hidePreview() { if (snapPreview) snapPreview.style.display = "none"; }

/* Detect drag start on any WinBox header. */
document.addEventListener("pointerdown", function (e) {
    var hdr = e.target.closest && e.target.closest(".wb-drag");
    if (!hdr) return;
    var el = hdr.closest(".winbox");
    if (!el) return;
    for (var id in wins) {
        if (wins[id].window === el) { draggingId = id; break; }
    }
});

document.addEventListener("pointermove", function (e) {
    if (!draggingId) return;
    var zone = tileZone(e.clientX, e.clientY);
    if (zone) showPreview(zone); else hidePreview();
});

document.addEventListener("pointerup", function (e) {
    if (!draggingId) { hidePreview(); return; }
    var win = wins[draggingId];
    draggingId = null;

    var zone = tileZone(e.clientX, e.clientY);
    var r = zone ? tileRect(zone) : null;
    if (r && win) {
        win.move(r.x, r.y);
        win.resize(r.w, r.h);
        saveLayout();
        syncWaveformSize();
    }
    hidePreview();
});

/* Magnetic snap: align edges to nearby windows during drag. */
function snapToNeighbors(id) {
    if (_snapping) return;
    var w = wins[id];
    if (!w) return;
    var x = w.x, y = w.y, ww = w.width, wh = w.height;
    var sx = x, sy = y;

    for (var oid in wins) {
        if (oid === id || wins[oid].min) continue;
        var o = wins[oid];
        /* vertical edges */
        if (Math.abs(x - (o.x + o.width)) < SNAP_PX) sx = o.x + o.width;
        else if (Math.abs((x + ww) - o.x) < SNAP_PX)  sx = o.x - ww;
        else if (Math.abs(x - o.x) < SNAP_PX)          sx = o.x;
        else if (Math.abs((x + ww) - (o.x + o.width)) < SNAP_PX) sx = o.x + o.width - ww;
        /* horizontal edges */
        if (Math.abs(y - (o.y + o.height)) < SNAP_PX) sy = o.y + o.height;
        else if (Math.abs((y + wh) - o.y) < SNAP_PX)   sy = o.y - wh;
        else if (Math.abs(y - o.y) < SNAP_PX)           sy = o.y;
        else if (Math.abs((y + wh) - (o.y + o.height)) < SNAP_PX) sy = o.y + o.height - wh;
    }
    if (sx !== x || sy !== y) {
        _snapping = true;
        w.move(sx, sy);
        _snapping = false;
    }
}

function saveLayout() {
    var layout = {};
    for (var id in wins) {
        layout[id] = {
            x: wins[id].x, y: wins[id].y,
            width: wins[id].width, height: wins[id].height,
            min: wins[id].min,
        };
    }
    localStorage.setItem(LAYOUT_KEY, JSON.stringify(layout));
}

/**
 * Create a single WinBox window for a dashboard panel.
 * Called from shell.html (WASM) or auto-created (POSIX).
 */
function createDashboardWindow(id, title) {
    if (typeof WinBox === "undefined") return;

    var def = WIN_DEFS[id] || { title: "Window", col: 0, row: 0, cw: 0.5, rh: 0.5 };
    var saved = null;
    try { saved = JSON.parse(localStorage.getItem(LAYOUT_KEY)); } catch (e) {}
    var opts = saved && saved[id] ? clampRect(saved[id]) : defaultRect(def);

    var panel = document.getElementById(id + "-panel");
    if (!panel) return;

    var winTitle = title || def.title;
    var mobile = isMobile();

    wins[id] = new WinBox({
        id: id,
        title: winTitle,
        mount: panel,
        class: mobile ? "no-full no-close no-resize no-move" : "no-full no-close",
        x: opts.x,
        y: opts.y,
        width: opts.width,
        height: opts.height,
        minwidth: 200,
        minheight: 80,
        top: 50,
        bottom: TASKBAR_H,
        onmove: function () {
            snapToNeighbors(id);
            saveLayout();
        },
        onresize: function () {
            saveLayout();
            syncWaveformSize();
        },
        onfocus: updateBadges,
        onblur: updateBadges,
        onminimize: function () { saveLayout(); updateBadges(); },
        onrestore: function () { saveLayout(); updateBadges(); },
        onmaximize: function () {
            var w = wins[id];
            if (w._preMax) {
                /* Restore previous size/position. */
                var p = w._preMax;
                w.move(p.x, p.y);
                w.resize(p.w, p.h);
                w._preMax = null;
            } else {
                /* Save current state, then tile to usable area. */
                w._preMax = { x: w.x, y: w.y, w: w.width, h: w.height };
                var r = tileRect("top");
                if (r) { w.move(r.x, r.y); w.resize(r.w, r.h); }
            }
            saveLayout();
            syncWaveformSize();
            return true; /* cancel default maximize */
        },
    });

    addBadge(id, winTitle);
    if (!mobile && opts.min) wins[id].minimize(true);
    updateBadges();
}

window.createDashboardWindow = createDashboardWindow;

/* Keep waveform canvas pixel-buffer in sync with its CSS size. */
function syncWaveformSize() {
    if (waveCanvas && waveCanvas.clientWidth > 0)
        waveCanvas.width = waveCanvas.clientWidth;
}

/** Re-layout all windows to fill the viewport optimally. */
function autoLayout() {
    var layout = computeLayout();
    for (var id in layout) {
        if (!wins[id]) continue;
        var r = layout[id];
        if (wins[id].min) wins[id].minimize(false);
        wins[id]._preMax = null;
        wins[id].move(r.x, r.y);
        wins[id].resize(r.width, r.height);
        if (isMobile()) {
            wins[id].addClass("no-resize").addClass("no-move");
        } else {
            wins[id].removeClass("no-resize").removeClass("no-move");
        }
    }
    saveLayout();
    syncWaveformSize();
}

/* Reset button (may not exist in all modes). */
var resetBtn = document.getElementById("reset-layout");
if (resetBtn) {
    resetBtn.addEventListener("click", function () {
        localStorage.removeItem(LAYOUT_KEY);
        autoLayout();
    });
}

/* Re-layout on viewport resize (debounced). */
var resizeTimer = null;
window.addEventListener("resize", function () {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function () {
        /* On mobile, always auto-layout. On desktop, just clamp. */
        if (isMobile()) {
            autoLayout();
        } else {
            for (var id in wins) {
                if (wins[id].min) continue;
                var c = clampRect({ x: wins[id].x, y: wins[id].y,
                                     width: wins[id].width, height: wins[id].height });
                wins[id].move(c.x, c.y);
                wins[id].resize(c.width, c.height);
                wins[id].removeClass("no-resize").removeClass("no-move");
            }
        }
        syncWaveformSize();
        saveLayout();
    }, 150);
});

/* ── POSIX mode: create all windows + connect WebSocket ───────────── */
var isWasmMode = (typeof Module !== "undefined" && Module.onRuntimeInitialized);

if (!isWasmMode && typeof WinBox !== "undefined") {
    /* POSIX mode — events window is always useful; display/audio/console
       windows are created lazily when the first data frame arrives so that
       only features the firmware actually uses get a panel. */
    if (document.getElementById("events-panel"))
        createDashboardWindow("events", WIN_DEFS.events.title);
}

/* ── WebSocket connection (POSIX mode only) ───────────────────────── */
function connect() {
    var proto = location.protocol === "https:" ? "wss:" : "ws:";
    ws = new WebSocket(proto + "//" + location.host + "/ws");
    ws.binaryType = "arraybuffer";

    ws.onopen = function () {
        statusEl.textContent = "Connected";
        statusEl.className = "status connected";
        addEvent("system", "Connected to sim server");
    };

    ws.onclose = function () {
        statusEl.textContent = "Disconnected";
        statusEl.className = "status disconnected";
        setTimeout(connect, 2000);
    };

    ws.onerror = function () { ws.close(); };

    ws.onmessage = function (e) {
        if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4) return;
        var view = new DataView(e.data);
        var type = view.getUint32(0, true);
        var payload = e.data.slice(4);

        switch (type) {
            case FRAME_FB:    handleFramebuffer(payload); break;
            case FRAME_AUDIO: handleAudio(payload); break;
            case FRAME_EVENT: handleEvent(payload); break;
            case FRAME_STATE: handleState(payload); break;
            case FRAME_LOG:   handleLog(payload); break;
        }
    };
}

/* ── Lazy window creation for POSIX mode ───────────────────────────── */
function ensureWindow(id) {
    if (isWasmMode || !WIN_DEFS[id]) return;
    if (document.getElementById(id + "-panel") && !wins[id]) {
        createDashboardWindow(id, WIN_DEFS[id].title);
        /* Re-tile all windows to accommodate the new one. */
        autoLayout();
    }
}

/* ── Display rendering (POSIX mode) ───────────────────────────────── */
function handleFramebuffer(payload) {
    ensureWindow("display");
    if (!ctx || payload.byteLength < 8) return;

    var view = new DataView(payload);
    var x1 = view.getUint16(0, true), y1 = view.getUint16(2, true);
    var x2 = view.getUint16(4, true), y2 = view.getUint16(6, true);
    var w = x2 - x1 + 1, h = y2 - y1 + 1;

    if (displayWidth === 0 || (x1 === 0 && y1 === 0 && (x2 + 1) > displayWidth)) {
        displayWidth = x2 + 1;
        displayHeight = y2 + 1;
        canvas.width = displayWidth;
        canvas.height = displayHeight;
        resolutionEl.textContent = displayWidth + "x" + displayHeight;
    }

    var pixels = new Uint8Array(payload, 8);
    var imgData = ctx.createImageData(w, h);
    var data = imgData.data;
    for (var i = 0; i < w * h; i++) {
        var off = i * 4;
        data[off]     = pixels[off + 2];
        data[off + 1] = pixels[off + 1];
        data[off + 2] = pixels[off];
        data[off + 3] = 255;
    }
    ctx.putImageData(imgData, x1, y1);

    frameCount++;
    var now = performance.now();
    if (now - lastFpsTime >= 1000) {
        fpsEl.textContent = (frameCount - lastFpsCount) + " FPS";
        lastFpsCount = frameCount;
        lastFpsTime = now;
    }
}

/* ── Audio handling (POSIX mode) ──────────────────────────────────── */
function handleAudio(payload) {
    ensureWindow("audio");
    if (payload.byteLength < 8) return;
    var view = new DataView(payload);
    audioSampleRate = view.getUint32(0, true);
    audioChannels   = view.getUint16(4, true);
    audioBitDepth   = view.getUint16(6, true);
    if (audioInfoEl)
        audioInfoEl.textContent = audioSampleRate + " Hz / " + audioChannels + "ch / " + audioBitDepth + "bit";
    var pcmData = new Uint8Array(payload, 8);
    drawWaveform(pcmData);
    if (audioEnabled && audioCtx) playPcm(pcmData);
}

function drawWaveform(pcmBytes) {
    if (!waveCtx) return;
    var w = waveCanvas.width, h = waveCanvas.height;
    waveCtx.fillStyle = "#0d1117";
    waveCtx.fillRect(0, 0, w, h);
    if (pcmBytes.length < 2) return;
    var samples = new Int16Array(pcmBytes.buffer, pcmBytes.byteOffset, Math.floor(pcmBytes.length / 2));
    var step = Math.max(1, Math.floor(samples.length / w));
    waveCtx.strokeStyle = "#4ecca3";
    waveCtx.lineWidth = 1;
    waveCtx.beginPath();
    for (var x = 0; x < w && x * step < samples.length; x++) {
        var v = samples[x * step] / 32768;
        var y = (1 - v) * h / 2;
        if (x === 0) waveCtx.moveTo(x, y); else waveCtx.lineTo(x, y);
    }
    waveCtx.stroke();
}

function playPcm(pcmBytes) {
    if (!audioCtx || audioCtx.sampleRate !== audioSampleRate)
        audioCtx = new AudioContext({ sampleRate: audioSampleRate });
    var samples = new Int16Array(pcmBytes.buffer, pcmBytes.byteOffset, Math.floor(pcmBytes.length / 2));
    var numFrames = Math.floor(samples.length / audioChannels);
    var buf = audioCtx.createBuffer(audioChannels, numFrames, audioSampleRate);
    for (var ch = 0; ch < audioChannels; ch++) {
        var cd = buf.getChannelData(ch);
        for (var i = 0; i < numFrames; i++) cd[i] = samples[i * audioChannels + ch] / 32768;
    }
    var src = audioCtx.createBufferSource();
    src.buffer = buf;
    src.connect(audioCtx.destination);
    src.start();
}

if (audioToggle) {
    audioToggle.addEventListener("click", function () {
        audioEnabled = !audioEnabled;
        audioToggle.textContent = audioEnabled ? "Disable Audio" : "Enable Audio";
        if (audioEnabled && !audioCtx) audioCtx = new AudioContext({ sampleRate: audioSampleRate });
    });
}

/* ── Plugin events ─────────────────────────────────────────────────── */
function handleEvent(payload) {
    if (payload.byteLength < 16) return;
    var view = new DataView(payload);
    addEvent("plugin:" + view.getUint32(0, true),
             "type=" + view.getUint32(4, true) + " t=" + view.getUint32(8, true) + "ms");
}

function handleLog(payload) {
    ensureWindow("events");
    var text = new TextDecoder().decode(new Uint8Array(payload));
    /* Split into lines — firmware may send multiple lines at once. */
    var lines = text.split("\n");
    for (var i = 0; i < lines.length; i++) {
        var line = lines[i].replace(/\r$/, "");
        if (line.length > 0) addEvent("log", line);
    }
}

function handleState(payload) {
    if (payload.byteLength < 4) return;
    var view = new DataView(payload);
    addEvent("state:" + view.getUint32(0, true),
             new TextDecoder().decode(new Uint8Array(payload, 4)));
}

var autoScroll = true;
var scrollBtn = document.getElementById("autoscroll-toggle");
if (scrollBtn) {
    scrollBtn.addEventListener("click", function () {
        autoScroll = !autoScroll;
        scrollBtn.textContent = "Auto-scroll: " + (autoScroll ? "ON" : "OFF");
        scrollBtn.className = "btn btn-small" + (autoScroll ? " autoscroll-on" : "");
        if (autoScroll && eventLog)
            eventLog.scrollTop = eventLog.scrollHeight;
    });
}

function addEvent(type, msg) {
    if (!eventLog) return;
    var el = document.createElement("div");
    el.className = "entry";
    var time = new Date().toTimeString().split(" ")[0];
    el.innerHTML = '<span class="time">' + time + '</span>' +
                   '<span class="type">[' + type + ']</span>' + msg;
    eventLog.appendChild(el);
    if (autoScroll) eventLog.scrollTop = eventLog.scrollHeight;
    while (eventLog.children.length > 200) eventLog.removeChild(eventLog.firstChild);
}

/* ── Send command to firmware ──────────────────────────────────────── */
function sendCmd(pluginId, cmdType, data) {
    var hdrLen = 4 + 12 + (data ? data.length : 0);
    var buf = new ArrayBuffer(hdrLen);
    var view = new DataView(buf);
    view.setUint32(0, FRAME_CMD, true);
    view.setUint32(4, pluginId, true);
    view.setUint32(8, cmdType, true);
    view.setUint32(12, data ? data.length : 0, true);
    if (data) new Uint8Array(buf, 16).set(data);

    if (typeof wasmWorker !== "undefined" && wasmWorker) {
        if (typeof Module !== "undefined" && Module && Module.ccall) {
            var arr = new Uint8Array(buf, 4);
            var ptr = Module._malloc(arr.length);
            Module.HEAPU8.set(arr, ptr);
            Module.ccall('ove_sim_wasm_push_cmd', null, ['number', 'number'], [ptr, arr.length]);
            Module._free(ptr);
        }
    } else if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(buf);
    }
}
window.sendCmd = sendCmd;

/* ── Init: POSIX mode connects via WebSocket ──────────────────────── */
if (!isWasmMode) {
    connect();
}
