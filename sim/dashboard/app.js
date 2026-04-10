/*
 * oveRTOS Simulation Dashboard
 *
 * Shared between POSIX (WebSocket) and WASM (Emscripten) modes.
 * Provides: event log, WinBox window management, audio waveform (POSIX).
 */

"use strict";

/* ── Frame type constants (shared with ove-dashboard-bridge.py) ────── */
var FRAME_FB    = 0x01;
var FRAME_AUDIO = 0x02;
var FRAME_EVENT = 0x03;
var FRAME_CMD   = 0x04;
var FRAME_STATE = 0x05;
var FRAME_LOG   = 0x06;
var FRAME_INPUT = 0x07;

/* ── Shared constants ────────────────────────────────────────────────── */
var RING_HDR            = 32;     /* OVE_RING_OFF_BUF — ring header size in bytes */
var MAX_LOG_ENTRIES     = 200;    /* max entries in event log before trimming */
var RECONNECT_BASE_MS  = 2000;   /* initial WebSocket reconnect delay */
var RECONNECT_MAX_MS   = 30000;  /* max reconnect delay after backoff */
var DEFAULT_SAMPLE_RATE = 16000;  /* fallback audio sample rate */

/* ── Shared helpers ──────────────────────────────────────────────────── */

/** Convert XRGB8888 pixels to RGBA8888 in-place into an ImageData buffer.
 *  Uses Uint32Array for word-at-a-time conversion. */
function convertXRGBtoRGBA(src, dst, pixelCount) {
    var src32 = new Uint32Array(src.buffer, src.byteOffset, pixelCount);
    var dst32 = new Uint32Array(dst.buffer, dst.byteOffset, pixelCount);
    for (var i = 0; i < pixelCount; i++) {
        var px = src32[i]; /* little-endian: BB GG RR XX */
        dst32[i] = (px & 0x0000FF00)            /* G stays */
                 | ((px & 0x00FF0000) >>> 16)    /* R → byte 0 */
                 | ((px & 0x000000FF) << 16)     /* B → byte 2 */
                 | 0xFF000000;                   /* A = 255 */
    }
}

/** Compute canvas-relative coordinates from a mouse or touch event. */
function canvasCoords(cvs, e) {
    var r = cvs.getBoundingClientRect();
    var sx = cvs.width / r.width;
    var sy = cvs.height / r.height;
    return {
        x: Math.round((e.clientX - r.left) * sx),
        y: Math.round((e.clientY - r.top) * sy)
    };
}

/** Attach mouse + touch input handlers to a canvas.
 *  onInput(x, y, pressed) is called for each pointer event. */
function attachInputHandlers(cvs, onInput) {
    if (!cvs) return;
    cvs.style.touchAction = "none";

    cvs.addEventListener("mousedown", function (e) {
        var c = canvasCoords(cvs, e);
        onInput(c.x, c.y, 1);
        e.preventDefault();
    });
    cvs.addEventListener("mouseup", function (e) {
        var c = canvasCoords(cvs, e);
        onInput(c.x, c.y, 0);
    });
    cvs.addEventListener("mousemove", function (e) {
        if (!(e.buttons & 1)) return;
        var c = canvasCoords(cvs, e);
        onInput(c.x, c.y, 1);
    });
    cvs.addEventListener("touchstart", function (e) {
        var c = canvasCoords(cvs, e.touches[0]);
        onInput(c.x, c.y, 1);
        e.preventDefault();
    }, {passive: false});
    cvs.addEventListener("touchend", function (e) {
        onInput(0, 0, 0);
        e.preventDefault();
    }, {passive: false});
    cvs.addEventListener("touchmove", function (e) {
        var c = canvasCoords(cvs, e.touches[0]);
        onInput(c.x, c.y, 1);
        e.preventDefault();
    }, {passive: false});
}

/* ── DOM elements (may be null in WASM mode for missing panels) ───── */
var statusEl     = document.getElementById("status");
var canvas       = document.getElementById("display-canvas");
var ctx          = canvas ? canvas.getContext("2d") : null;
var resolutionEl = document.getElementById("display-resolution");
var fpsEl        = document.getElementById("display-fps");
var audioInfoEl  = document.getElementById("audio-info");
var eventLog     = document.getElementById("event-log");

/* ── State ─────────────────────────────────────────────────────────── */
var ws = null;
var displayWidth = 0;
var displayHeight = 0;
var frameCount = 0;
var lastFpsTime = performance.now();
var lastFpsCount = 0;

/* Audio state (POSIX WebSocket mode) */
var audioCtx = null;
var playbackNode = null;
var captureStream = null;
var captureNode = null;
var captureSource = null;  /* MediaStreamSourceNode — must be held to prevent GC */
var audioSampleRate = 44100;
var audioChannels = 1;
var audioBitDepth = 16;

/* AudioWorklet playback via SharedArrayBuffer ring.
 * Ring must be power-of-2 size. */
var WS_AUDIO_RING_SIZE = 65536; /* 64KB = ~2s at 16kHz/16-bit */
var wsAudioSab = null;   /* SharedArrayBuffer backing the ring */
var wsAudioPos = null;   /* Uint32Array: [writePos, readPos] */
var wsAudioRing = null;  /* Uint8Array: PCM ring data */

/* ── Floating window layout (WinBox) ──────────────────────────────── */
var LAYOUT_KEY = "ove-sim-dashboard-layout";
var MOBILE_BP  = 640;  /* viewport width below which we stack full-width */

/* Proportional defaults: fractions of usable area. */
var WIN_DEFS = {
    display: { title: "Display", col: 0, row: 0, cw: 1,   rh: 0.6 },
    audio:   { title: "Audio",   col: 0, row: 1, cw: 0.5, rh: 0.4 },
    events:  { title: "Events",  col: 0, row: 1, cw: 0.5, rh: 0.4 },
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

/* Keep waveform canvases pixel-buffer in sync with their CSS size. */
function syncWaveformSize() {
    var wOut = document.getElementById("waveform-output");
    var wIn  = document.getElementById("waveform-input");
    if (wOut && wOut.clientWidth > 0) wOut.width = wOut.clientWidth;
    if (wIn  && wIn.clientWidth  > 0) wIn.width  = wIn.clientWidth;
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
    /* POSIX mode — events + console windows are always useful;
       display/audio windows are created lazily when the first data
       frame arrives so that only features the firmware actually uses
       get a panel. */
    if (document.getElementById("events-panel"))
        createDashboardWindow("events", WIN_DEFS.events.title);
}

/* ── WebSocket connection (POSIX mode only) ───────────────────────── */
var reconnectDelay = RECONNECT_BASE_MS;
var reconnectAttempt = 0;

function connect() {
    var proto = location.protocol === "https:" ? "wss:" : "ws:";
    ws = new WebSocket(proto + "//" + location.host + "/ws");
    ws.binaryType = "arraybuffer";

    ws.onopen = function () {
        reconnectDelay = RECONNECT_BASE_MS;
        reconnectAttempt = 0;
        statusEl.textContent = "Connected";
        statusEl.className = "status connected";
        addEvent("system", "Connected to sim server");
    };

    ws.onclose = function () {
        reconnectAttempt++;
        statusEl.textContent = "Reconnecting (" + reconnectAttempt + ")...";
        statusEl.className = "status disconnected";
        setTimeout(connect, reconnectDelay);
        reconnectDelay = Math.min(reconnectDelay * 2, RECONNECT_MAX_MS);
    };

    ws.onerror = function () { ws.close(); };

    ws.onmessage = function (e) {
        if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4) return;
        var buf = e.data;
        var type = new DataView(buf).getUint32(0, true);

        switch (type) {
            case FRAME_FB:    handleFramebuffer(buf, 4); break;
            case FRAME_AUDIO: handleAudio(buf, 4); break;
            case FRAME_EVENT: handleEvent(buf, 4); break;
            case FRAME_STATE: handleState(buf, 4); break;
            case FRAME_LOG:   handleLog(buf, 4); break;
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
        /* Attach input handlers when display panel is created. */
        if (id === "display") attachCanvasInput();
    }
}

/* ── Display rendering (POSIX mode) ───────────────────────────────── */
function handleFramebuffer(buf, off) {
    ensureWindow("display");
    if (!ctx || buf.byteLength - off < 8) return;

    var view = new DataView(buf, off);
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

    var pixels = new Uint8Array(buf, off + 8);
    var imgData = ctx.createImageData(w, h);
    convertXRGBtoRGBA(pixels, imgData.data, w * h);
    ctx.putImageData(imgData, x1, y1);

    frameCount++;
    var now = performance.now();
    if (now - lastFpsTime >= 1000) {
        fpsEl.textContent = (frameCount - lastFpsCount) + " FPS";
        lastFpsCount = frameCount;
        lastFpsTime = now;
    }
}

/* ── Audio handling (POSIX WebSocket mode) ────────────────────────── */

/** Draw a float32 or int16 waveform on a canvas.
 *  Canvas elements and contexts are cached to avoid per-frame DOM lookups. */
var _waveCache = {};
function drawWave(canvasId, samples, color) {
    var cached = _waveCache[canvasId];
    if (!cached) {
        var cvs = document.getElementById(canvasId);
        if (!cvs) return;
        cached = { cvs: cvs, ctx: cvs.getContext("2d") };
        _waveCache[canvasId] = cached;
    }
    var cvs = cached.cvs, wCtx = cached.ctx;
    var cw = cvs.width || cvs.clientWidth;
    var ch = cvs.height;
    if (cw <= 0) { cw = cvs.clientWidth; cvs.width = cw; }
    wCtx.fillStyle = "#0d1117";
    wCtx.fillRect(0, 0, cw, ch);
    var n = samples.length;
    /* Detect format: Int16Array values exceed [-1,1], Float32 don't. */
    var isInt16 = (samples instanceof Int16Array);
    var scale = isInt16 ? 1.0 / 32768.0 : 1.0;
    wCtx.strokeStyle = color;
    wCtx.lineWidth = 1;
    wCtx.beginPath();
    for (var x = 0; x < cw; x++) {
        var idx = Math.min(Math.floor(x * n / cw), n - 1);
        var v = (samples[idx] || 0) * scale;
        var y = (1 - v) * ch / 2;
        if (x === 0) wCtx.moveTo(x, y); else wCtx.lineTo(x, y);
    }
    wCtx.stroke();
}

/* Pending waveform data — decouples audio callbacks from DOM rendering. */
var _wavePending = {};
var _waveRafScheduled = false;
function scheduleWaveDraw(canvasId, samples, color) {
    _wavePending[canvasId] = { samples: samples, color: color };
    if (!_waveRafScheduled) {
        _waveRafScheduled = true;
        requestAnimationFrame(function () {
            _waveRafScheduled = false;
            for (var id in _wavePending) {
                var p = _wavePending[id];
                drawWave(id, p.samples, p.color);
            }
            _wavePending = {};
        });
    }
}

function handleAudio(buf, off) {
    ensureWindow("audio");
    if (buf.byteLength - off < 8) return;
    var view = new DataView(buf, off);
    audioSampleRate = view.getUint32(0, true);
    audioChannels   = view.getUint16(4, true);
    audioBitDepth   = view.getUint16(6, true);
    if (audioInfoEl)
        audioInfoEl.textContent = audioSampleRate + " Hz / " + audioChannels + "ch / " + audioBitDepth + "bit";

    var pcmData = new Uint8Array(buf, off + 8);
    var samples = new Int16Array(pcmData.buffer, pcmData.byteOffset,
                                 Math.floor(pcmData.length / 2));
    drawWave("waveform-output", samples, "#e94560");

    /* Record output if active. */
    if (isRecording) {
        if (recordBytes + pcmData.length > MAX_RECORD_BYTES) {
            /* Auto-stop to prevent memory exhaustion. */
            isRecording = false;
            var recBtn = document.getElementById("audio-record-toggle");
            if (recBtn) recBtn.textContent = "Record Output";
            console.warn("[audio] Recording stopped: max size reached (" +
                (MAX_RECORD_BYTES >> 20) + "MB)");
        } else {
            var chunk = new Uint8Array(pcmData.buffer.slice(
                pcmData.byteOffset, pcmData.byteOffset + pcmData.length));
            recordBuffer.push(chunk);
            recordBytes += chunk.length;
        }
    }

    /* Write full data to SharedArrayBuffer ring (no dropping on write side).
     * Clock drift compensation happens in the AudioWorklet (read side). */
    if (playbackState && wsAudioRing && wsAudioPos) {
        var wp = Atomics.load(wsAudioPos, 0);
        var rp = Atomics.load(wsAudioPos, 1);
        var free = WS_AUDIO_RING_SIZE - (wp - rp);
        var n = Math.min(pcmData.length, free);
        var mask = WS_AUDIO_RING_SIZE - 1;
        for (var i = 0; i < n; i++) {
            wsAudioRing[(wp + i) & mask] = pcmData[i];
        }
        Atomics.store(wsAudioPos, 0, wp + n);

        /* Phase 1: measure rate for 1 second, then start worklet. */
        if (playbackState === "measuring") {
            if (measureStart === 0) measureStart = performance.now();
            var totalWritten = Atomics.load(wsAudioPos, 0);
            var fwRate = audioSampleRate || DEFAULT_SAMPLE_RATE;
            if ((totalWritten / 2) >= fwRate) {
                startMeasuredPlayback();
            }
        }
    }
}

/* ── Audio device enumeration ─────────────────────────────────────── */
function refreshAudioDevices() {
    if (!navigator.mediaDevices || !navigator.mediaDevices.enumerateDevices) return;
    var outSel = document.getElementById("audio-output-select");
    var inSel  = document.getElementById("audio-input-select");
    if (!outSel || !inSel) return;

    navigator.mediaDevices.enumerateDevices().then(function (devs) {
        outSel.innerHTML = "";
        inSel.innerHTML = "";
        var oi = 0, ii = 0;
        devs.forEach(function (d) {
            var opt = document.createElement("option");
            opt.value = d.deviceId;
            if (d.kind === "audiooutput") {
                opt.textContent = d.label || ("Speaker " + (++oi));
                outSel.appendChild(opt);
            } else if (d.kind === "audioinput") {
                opt.textContent = d.label || ("Microphone " + (++ii));
                inSel.appendChild(opt);
            }
        });
    });
}

var playToggle = document.getElementById("audio-play-toggle");
var measureStart = 0;
var playbackState = null;  /* null | "measuring" | "playing" */
if (playToggle && !isWasmMode) {
    playToggle.addEventListener("click", function () {
        if (playbackState) {
            if (playbackNode && typeof playbackNode.disconnect === "function")
                playbackNode.disconnect();
            playbackNode = null;
            playbackState = null;
            if (audioCtx) { audioCtx.close(); audioCtx = null; }
            wsAudioSab = null; wsAudioPos = null; wsAudioRing = null;
            this.textContent = "Enable Playback";
            return;
        }

        if (typeof SharedArrayBuffer === "undefined") {
            alert("SharedArrayBuffer not available. Try reloading — the service worker needs one reload to activate.");
            return;
        }

        /* Allocate SharedArrayBuffer ring: 32-byte header + ring data.
         * Matches ove_sim_audio_ring layout (OVE_RING_OFF_BUF = 32). */
        wsAudioSab = new SharedArrayBuffer(RING_HDR + WS_AUDIO_RING_SIZE);
        wsAudioPos = new Uint32Array(wsAudioSab, 0, 2);  /* [writePos, readPos] */
        wsAudioRing = new Uint8Array(wsAudioSab, RING_HDR, WS_AUDIO_RING_SIZE);
        Atomics.store(wsAudioPos, 0, 0);
        Atomics.store(wsAudioPos, 1, 0);

        /* Phase 1: measure actual QEMU sample rate over 1 second. */
        measureStart = 0;
        playbackState = "measuring";
        this.textContent = "Measuring...";
    });
}

function startMeasuredPlayback() {
    var elapsed = (performance.now() - measureStart) / 1000;
    var totalWritten = Atomics.load(wsAudioPos, 0);
    var actualRate = Math.round((totalWritten / 2) / elapsed);
    if (actualRate < 8000) actualRate = 8000;
    if (actualRate > 48000) actualRate = 48000;
    audioCtx = new AudioContext({ sampleRate: actualRate });
    audioCtx.resume();
    if (audioCtx.setSinkId && outSelect && outSelect.value)
        audioCtx.setSinkId(outSelect.value);

    /* AudioWorklet runs on audio thread — immune to main-thread jank.
     * Inline Blob URL avoids file-serving/CORS issues. */
    var workletSrc = [
        "const HDR = 32;",
        "class P extends AudioWorkletProcessor {",
        "  constructor(o) {",
        "    super();",
        "    this.pos = new Uint32Array(o.processorOptions.sab, 0, 2);",
        "    this.ring = new Uint8Array(o.processorOptions.sab, HDR, o.processorOptions.rs);",
        "    this.rs = o.processorOptions.rs;",
        "    this.mask = o.processorOptions.rs - 1;",
        "    this.target = o.processorOptions.rs >> 3;",
        "  }",
        "  process(ins, outs) {",
        "    const out = outs[0][0]; if (!out) return true;",
        "    let wp = Atomics.load(this.pos, 0);",
        "    let rp = Atomics.load(this.pos, 1);",
        "    let av = wp - rp;",
        "    if (av > this.rs) { rp = wp - this.target; av = this.target; }",
        "    const sa = av >> 1;",
        "    for (let i = 0; i < out.length; i++) {",
        "      if (i < sa) {",
        "        const bi = (rp + i*2) & this.mask;",
        "        let s = (this.ring[(bi+1)&this.mask] << 8) | this.ring[bi];",
        "        if (s > 32767) s -= 65536;",
        "        out[i] = s / 32768.0;",
        "      } else { out[i] = i > 0 ? out[i-1] : 0; }",
        "    }",
        "    let consumed = Math.min(sa, out.length);",
        "    /* Drift compensation: if ring > target, skip extra samples",
        "     * to drain faster.  Each skip is 1 sample — inaudible. */",
        "    let excess = av - this.target;",
        "    if (excess > 0) {",
        "      let skip = Math.min(out.length >> 1, Math.floor(excess / 16));",
        "      consumed += skip;",
        "      if (consumed > sa) consumed = sa;",
        "    }",
        "    Atomics.store(this.pos, 1, rp + consumed * 2);",
        "    return true;",
        "  }",
        "}",
        "registerProcessor('dashboard-audio', P);"
    ].join("\n");
    var workletBlob = new Blob([workletSrc], { type: "application/javascript" });
    var workletUrl = URL.createObjectURL(workletBlob);

    audioCtx.audioWorklet.addModule(workletUrl).then(function () {
        URL.revokeObjectURL(workletUrl);
        var node = new AudioWorkletNode(audioCtx, "dashboard-audio", {
            processorOptions: {
                sab: wsAudioSab,
                rs: WS_AUDIO_RING_SIZE,
                maxLat: actualRate  /* 500ms in bytes = rate * 1ch * 2bytes * 0.5s = rate */
            },
            outputChannelCount: [1]
        });
        node.connect(audioCtx.destination);
        playbackNode = node;
        playbackState = "playing";
        if (playToggle) playToggle.textContent = "Disable Playback";
    }).catch(function (err) {
        console.error("[audio] AudioWorklet failed:", err);
        /* Fallback: ScriptProcessorNode (main thread, may have jitter). */
        playbackNode = audioCtx.createScriptProcessor(4096, 0, 1);
        playbackNode.onaudioprocess = function (e) {
            var output = e.outputBuffer.getChannelData(0);
            var wp = Atomics.load(wsAudioPos, 0);
            var rp = Atomics.load(wsAudioPos, 1);
            var avail = wp - rp;
            if (avail > WS_AUDIO_RING_SIZE) {
                rp = wp - (WS_AUDIO_RING_SIZE >> 1);
                avail = WS_AUDIO_RING_SIZE >> 1;
            }
            var mask = WS_AUDIO_RING_SIZE - 1;
            for (var i = 0; i < output.length; i++) {
                if (i * 2 < avail) {
                    var idx = (rp + i * 2) & mask;
                    var lo = wsAudioRing[idx];
                    var hi = wsAudioRing[(idx + 1) & mask];
                    var sample = (hi << 8) | lo;
                    if (sample > 32767) sample -= 65536;
                    output[i] = sample / 32768.0;
                } else {
                    output[i] = (i > 0) ? output[i - 1] : 0;
                }
            }
            Atomics.store(wsAudioPos, 1, rp + Math.min(avail, output.length * 2));
        };
        playbackNode.connect(audioCtx.destination);
        playbackState = "playing";
        if (playToggle) playToggle.textContent = "Disable Playback (fallback)";
    });
}

/* Switch output device. */
var outSelect = document.getElementById("audio-output-select");
if (outSelect) {
    outSelect.addEventListener("change", function () {
        if (audioCtx && audioCtx.setSinkId)
            audioCtx.setSinkId(outSelect.value);
    });
}

/* ── Input source selector (mic or file) ──────────────────────────── */
var audioSourceSel = document.getElementById("audio-source-select");
var audioFileControls = document.getElementById("audio-file-controls");
var audioInputSel = document.getElementById("audio-input-select");
var fileInputEl = document.getElementById("audio-file-input");
var fileBrowseBtn = document.getElementById("audio-file-browse");
var filePlayBtn = document.getElementById("audio-file-play");
var fileNameEl = document.getElementById("audio-file-name");

/* File input state. */
var fileInputPcm = null;     /* Int16Array of decoded audio */
var fileInputTimer = null;    /* setInterval for chunk feeding */
var fileInputPos = 0;         /* current read position in fileInputPcm */
var fileInputLoop = true;     /* loop when reaching end */

function stopMicCapture() {
    if (captureNode) { captureNode.disconnect(); captureNode = null; }
    if (captureSource) { captureSource.disconnect(); captureSource = null; }
    if (captureStream) {
        captureStream.getTracks().forEach(function (t) { t.stop(); });
        captureStream = null;
    }
}

function stopFileInput() {
    if (fileInputTimer) { clearInterval(fileInputTimer); fileInputTimer = null; }
    if (filePlayBtn) filePlayBtn.textContent = "Play File";
}

function stopAllInput() {
    stopMicCapture();
    stopFileInput();
}

/* Source dropdown: toggle between mic device selector and file controls. */
if (audioSourceSel) {
    audioSourceSel.addEventListener("change", function () {
        stopAllInput();
        var isMic = (this.value === "mic");
        if (audioInputSel) audioInputSel.style.display = isMic ? "" : "none";
        if (audioFileControls) audioFileControls.style.display = isMic ? "none" : "";
    });
    /* Trigger initial state. */
    if (audioInputSel) audioInputSel.style.display = "";
    if (audioFileControls) audioFileControls.style.display = "none";
}

/* ── Mic capture ──────────────────────────────────────────────────── */
function startMicCapture() {
    var inSel = document.getElementById("audio-input-select");
    var constraints = { audio: (inSel && inSel.value)
                        ? { deviceId: { exact: inSel.value } } : true };
    navigator.mediaDevices.getUserMedia(constraints).then(function (stream) {
        captureStream = stream;
        if (!audioCtx) audioCtx = new AudioContext({ sampleRate: audioSampleRate });
        captureSource = audioCtx.createMediaStreamSource(stream);
        captureNode = audioCtx.createScriptProcessor(1024, 1, 1);
        captureNode.onaudioprocess = function (e) {
            var input = e.inputBuffer.getChannelData(0);
            scheduleWaveDraw("waveform-input", input, "#4ecca3");
            var pcm = new ArrayBuffer(input.length * 2);
            var view16 = new Int16Array(pcm);
            for (var i = 0; i < input.length; i++) {
                var s = Math.max(-1, Math.min(1, input[i]));
                view16[i] = (s < 0 ? s * 32768 : s * 32767) | 0;
            }
            sendCmd(audioPluginId, 0, new Uint8Array(pcm));
        };
        captureSource.connect(captureNode);
        captureNode.connect(audioCtx.destination);
    }).catch(function (err) {
        console.error("Mic access denied:", err);
    });
}

/* Auto-start mic when source is "mic" and audio window opens. */
if (audioSourceSel && audioSourceSel.value === "mic") {
    /* Mic starts when user interacts; we just set up device enum. */
}

/* ── File input: browse, decode, feed ─────────────────────────────── */
if (fileBrowseBtn && fileInputEl) {
    fileBrowseBtn.addEventListener("click", function () { fileInputEl.click(); });
    fileInputEl.addEventListener("change", function () {
        var file = this.files[0];
        if (!file) return;
        if (fileNameEl) fileNameEl.textContent = file.name;
        if (filePlayBtn) filePlayBtn.style.display = "";

        var reader = new FileReader();
        reader.onload = function (ev) {
            var fwRate = audioSampleRate;
            /* In WASM mode, audioSampleRate is never updated from FRAME_AUDIO.
             * Read the actual rate from the playback ring header. */
            if (typeof Module !== "undefined" && Module && Module.ccall) {
                var _p = Module.ccall('ove_wasm_audio_get_playback_ptr', 'number');
                var _h = Module.HEAPU32 || new Uint32Array(Module.HEAPU8.buffer);
                var _sr = _h[(_p + 8) >> 2];
                if (_sr > 0 && _sr <= 48000) fwRate = _sr;
            }
            if (!fwRate || fwRate <= 0) fwRate = DEFAULT_SAMPLE_RATE;
            /* Temporary AudioContext for decoding. */
            var decodeCtx = new AudioContext();
            decodeCtx.decodeAudioData(ev.target.result).then(function (audioBuffer) {
                /* Resample to firmware rate. */
                var duration = audioBuffer.duration;
                var outLen = Math.ceil(duration * fwRate);
                var offCtx = new OfflineAudioContext(1, outLen, fwRate);
                var src = offCtx.createBufferSource();
                src.buffer = audioBuffer;
                src.connect(offCtx.destination);
                src.start();
                return offCtx.startRendering();
            }).then(function (resampled) {
                decodeCtx.close();
                /* Convert float32 to int16. */
                var f32 = resampled.getChannelData(0);
                fileInputPcm = new Int16Array(f32.length);
                for (var i = 0; i < f32.length; i++) {
                    var s = Math.max(-1, Math.min(1, f32[i]));
                    fileInputPcm[i] = (s < 0 ? s * 32768 : s * 32767) | 0;
                }
                fileInputPos = 0;
            }).catch(function (err) {
                decodeCtx.close();
                console.error("[audio] File decode failed:", err);
            });
        };
        reader.readAsArrayBuffer(file);
    });
}

/* Play/stop file input — feeds chunks to firmware via sendCmd. */
if (filePlayBtn) {
    filePlayBtn.addEventListener("click", function () {
        if (fileInputTimer) {
            stopFileInput();
            return;
        }
        if (!fileInputPcm) return;

        stopMicCapture(); /* stop mic if running */
        fileInputPos = 0;
        var chunkSamples = 1024;
        /* Use firmware's actual rate for injection pacing. */
        var fwRate = audioSampleRate;
        if (typeof Module !== "undefined" && Module && Module.ccall) {
            var _p = Module.ccall('ove_wasm_audio_get_playback_ptr', 'number');
            var _sr = (Module.HEAPU32 || new Uint32Array(Module.HEAPU8.buffer))[(_p + 8) >> 2];
            if (_sr > 0 && _sr <= 48000) fwRate = _sr;
        }
        if (!fwRate || fwRate <= 0) fwRate = DEFAULT_SAMPLE_RATE;
        var intervalMs = Math.round(chunkSamples / fwRate * 1000);

        fileInputTimer = setInterval(function () {
            if (!fileInputPcm) { stopFileInput(); return; }
            var end = fileInputPos + chunkSamples;
            var chunk;
            if (end <= fileInputPcm.length) {
                chunk = fileInputPcm.subarray(fileInputPos, end);
            } else if (fileInputLoop) {
                /* Wrap around. */
                chunk = new Int16Array(chunkSamples);
                var first = fileInputPcm.length - fileInputPos;
                chunk.set(fileInputPcm.subarray(fileInputPos, fileInputPcm.length));
                chunk.set(fileInputPcm.subarray(0, chunkSamples - first), first);
            } else {
                stopFileInput();
                return;
            }
            fileInputPos = end % fileInputPcm.length;

            drawWave("waveform-input", chunk, "#4ecca3");
            sendCmd(audioPluginId, 0, new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength));
        }, intervalMs);

        this.textContent = "Stop File";
    });
}

/* ── Output recording ─────────────────────────────────────────────── */
var MAX_RECORD_BYTES = 200 * 1024 * 1024;  /* 200MB cap (~1.7h at 16kHz/16bit) */
var recordBuffer = [];
var recordBytes = 0;
var isRecording = false;

function createWavBlob(chunks, sampleRate, channels, bitsPerSample) {
    var dataLen = 0;
    for (var i = 0; i < chunks.length; i++) dataLen += chunks[i].length;
    var header = new ArrayBuffer(44);
    var v = new DataView(header);
    var blockAlign = channels * (bitsPerSample >> 3);
    /* RIFF header */
    v.setUint32(0, 0x46464952, false);           /* "RIFF" */
    v.setUint32(4, 36 + dataLen, true);
    v.setUint32(8, 0x45564157, false);            /* "WAVE" */
    /* fmt chunk */
    v.setUint32(12, 0x20746d66, false);           /* "fmt " */
    v.setUint32(16, 16, true);                    /* chunk size */
    v.setUint16(20, 1, true);                     /* PCM */
    v.setUint16(22, channels, true);
    v.setUint32(24, sampleRate, true);
    v.setUint32(28, sampleRate * blockAlign, true);
    v.setUint16(32, blockAlign, true);
    v.setUint16(34, bitsPerSample, true);
    /* data chunk */
    v.setUint32(36, 0x61746164, false);           /* "data" */
    v.setUint32(40, dataLen, true);
    return new Blob([header].concat(chunks), { type: "audio/wav" });
}

var recordToggle = document.getElementById("audio-record-toggle");
if (recordToggle) {
    recordToggle.addEventListener("click", function () {
        if (isRecording) {
            isRecording = false;
            this.textContent = "Record Output";
            /* Create WAV and trigger download. */
            if (recordBuffer.length > 0) {
                var wav = createWavBlob(recordBuffer,
                    audioSampleRate || DEFAULT_SAMPLE_RATE, audioChannels || 1, audioBitDepth || 16);
                var url = URL.createObjectURL(wav);
                var a = document.getElementById("audio-download-link");
                if (a) {
                    a.href = url;
                    a.download = "ove_audio_" +
                        new Date().toISOString().replace(/[:.]/g, "-") + ".wav";
                    a.click();
                    setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
                }
            }
            recordBuffer = [];
            recordBytes = 0;
            return;
        }
        recordBuffer = [];
        recordBytes = 0;
        isRecording = true;
        this.textContent = "Stop & Save";
    });
}

/* Enumerate audio devices on load and device change. */
if (!isWasmMode) {
    refreshAudioDevices();
    if (navigator.mediaDevices)
        navigator.mediaDevices.addEventListener("devicechange", refreshAudioDevices);
}

/* ── Plugin events ─────────────────────────────────────────────────── */
function handleEvent(buf, off) {
    if (buf.byteLength - off < 16) return;
    var view = new DataView(buf, off);
    addEvent("plugin:" + view.getUint32(0, true),
             "type=" + view.getUint32(4, true) + " t=" + view.getUint32(8, true) + "ms");
}

function handleLog(buf, off) {
    ensureWindow("events");
    var text = new TextDecoder().decode(new Uint8Array(buf, off));
    /* Split into lines — firmware may send multiple lines at once. */
    var lines = text.split("\n");
    for (var i = 0; i < lines.length; i++) {
        var line = lines[i].replace(/\r$/, "");
        if (line.length > 0) addEvent("log", line);
    }
}

var audioPluginId = 0;  /* default; updated from FRAME_STATE */

function handleState(buf, off) {
    if (buf.byteLength - off < 4) return;
    var view = new DataView(buf, off);
    var id = view.getUint32(0, true);
    var json = new TextDecoder().decode(new Uint8Array(buf, off + 4));
    addEvent("state:" + id, json);
    try {
        var obj = JSON.parse(json);
        if (obj && obj.type === "audio") audioPluginId = id;
    } catch (e) {}
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

var _logQueue = [];
var _logRafScheduled = false;

function _flushLogQueue() {
    _logRafScheduled = false;
    if (!eventLog || _logQueue.length === 0) return;
    var frag = document.createDocumentFragment();
    for (var i = 0; i < _logQueue.length; i++) frag.appendChild(_logQueue[i]);
    _logQueue = [];
    eventLog.appendChild(frag);
    while (eventLog.children.length > MAX_LOG_ENTRIES) eventLog.removeChild(eventLog.firstChild);
    if (autoScroll) eventLog.scrollTop = eventLog.scrollHeight;
}

function addEvent(type, msg) {
    if (!eventLog) return;
    var el = document.createElement("div");
    el.className = "entry";
    var time = new Date().toTimeString().split(" ")[0];
    var timeSpan = document.createElement("span");
    timeSpan.className = "time";
    timeSpan.textContent = time;
    var typeSpan = document.createElement("span");
    typeSpan.className = "type";
    typeSpan.textContent = "[" + type + "]";
    el.appendChild(timeSpan);
    el.appendChild(typeSpan);
    el.appendChild(document.createTextNode(msg));
    _logQueue.push(el);
    if (!_logRafScheduled) {
        _logRafScheduled = true;
        requestAnimationFrame(_flushLogQueue);
    }
}

/* ── Send pointer input to firmware (POSIX mode) ──────────────────── */
function sendInput(x, y, pressed) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    var buf = new ArrayBuffer(4 + 5);
    var view = new DataView(buf);
    view.setUint32(0, FRAME_INPUT, true);
    view.setInt16(4, x, true);
    view.setInt16(6, y, true);
    view.setUint8(8, pressed ? 1 : 0);
    ws.send(buf);
}

function attachCanvasInput() {
    if (!canvas || isWasmMode) return;
    attachInputHandlers(canvas, sendInput);
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

    if (typeof Module !== "undefined" && Module && Module.HEAPU8) {
        /* WASM: write audio inject directly to the capture ring,
         * bypassing the command queue (avoids main-thread mutex). */
        if (cmdType === 0 && data && data.length > 0) {
            if (!sendCmd._capPtr)
                sendCmd._capPtr = Module.ccall('ove_wasm_audio_get_capture_ptr', 'number');
            var cp = sendCmd._capPtr;
            var h32 = Module.HEAPU32;
            var h8 = Module.HEAPU8;
            var rs = h32[(cp + 16) >> 2] || 65536;
            var mask = rs - 1;
            var wp = Atomics.load(h32, cp >> 2);
            var rp = Atomics.load(h32, (cp + 4) >> 2);
            var free = rs - (wp - rp);
            var n = Math.min(data.length, free);
            var off = cp + RING_HDR;
            for (var i = 0; i < n; i++)
                h8[off + ((wp + i) & mask)] = data[i];
            Atomics.store(h32, cp >> 2, wp + n);
        }
    } else if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(buf);
    }
}
window.sendCmd = sendCmd;

/* ── Console input (works in both POSIX and WASM mode) ────────────── */
function initConsoleInput() {
    var conInput = document.getElementById("console-input");
    var conSend  = document.getElementById("console-send");
    if (!conInput || !conSend) return;

    function sendConsoleText() {
        var text = conInput.value + "\n";
        if (isWasmMode && typeof Module !== "undefined" && Module.ccall) {
            for (var i = 0; i < text.length; i++)
                Module.ccall('ove_wasm_console_push', null,
                    ['number'], [text.charCodeAt(i)]);
        } else {
            /* Batch entire string into a single WebSocket message. */
            var bytes = new Uint8Array(text.length);
            for (var i = 0; i < text.length; i++)
                bytes[i] = text.charCodeAt(i) & 0xFF;
            sendCmd(0xFFFFFFFF, 0, bytes);  /* console sentinel */
        }
        conInput.value = "";
    }
    conSend.addEventListener("click", sendConsoleText);
    conInput.addEventListener("keydown", function (e) {
        if (e.key === "Enter") sendConsoleText();
    });
}

/* ══════════════════════════════════════════════════════════════════════
   WASM mode initialization
   Called from shell.html Module.onRuntimeInitialized.
   All WASM-specific dashboard logic lives here, not in shell.html.
   ══════════════════════════════════════════════════════════════════════ */
window.initWasmMode = function () {
    statusEl.textContent = "Connected (WASM)";
    statusEl.className = "status connected";

    /* Create WinBox windows for panels enabled by compile-time config. */
    if (window.createDashboardWindow) {
        if (Module.ccall('ove_wasm_has_lvgl', 'number'))
            window.createDashboardWindow('display', 'Display');
        if (Module.ccall('ove_wasm_has_audio', 'number'))
            window.createDashboardWindow('audio', 'Audio');
        window.createDashboardWindow('events', 'Events');
    }

    initConsoleInput();

    /* ── Frame polling via shared framebuffer in WASM heap ──────── */
    var lastSeq = 0;
    var fCanvas = document.getElementById('display-canvas');
    var fCtx = fCanvas ? fCanvas.getContext('2d') : null;
    var fResEl = document.getElementById('display-resolution');
    var fFpsEl = document.getElementById('display-fps');
    var fFrames = 0;
    var fLastFpsTime = performance.now();

    function pollFrame() {
        requestAnimationFrame(pollFrame);
        var seq = Module.ccall('ove_wasm_fb_get_seq', 'number');
        if (seq === lastSeq) return;
        lastSeq = seq;

        var w = Module.ccall('ove_wasm_fb_get_width', 'number');
        var h = Module.ccall('ove_wasm_fb_get_height', 'number');
        var size = Module.ccall('ove_wasm_fb_get_size', 'number');
        if (w === 0 || h === 0 || size === 0) return;

        if (fCanvas.width !== w || fCanvas.height !== h) {
            fCanvas.width = w;
            fCanvas.height = h;
            if (fResEl) fResEl.textContent = w + 'x' + h;
        }

        var pxPtr = Module.ccall('ove_wasm_fb_get_pixels', 'number');
        var src = new Uint8Array(Module.HEAPU8.buffer, pxPtr, size);
        var imgData = fCtx.createImageData(w, h);
        convertXRGBtoRGBA(src, imgData.data, w * h);
        fCtx.putImageData(imgData, 0, 0);

        fFrames++;
        var now = performance.now();
        if (now - fLastFpsTime >= 1000) {
            if (fFpsEl) fFpsEl.textContent = fFrames + ' FPS';
            fFrames = 0;
            fLastFpsTime = now;
        }
    }
    pollFrame();

    /* ── Mouse/touch input forwarding to LVGL ──────────────────── */
    if (fCanvas) {
        attachInputHandlers(fCanvas, function (x, y, pressed) {
            Module.ccall('ove_wasm_input_set', null,
                ['number','number','number'], [x, y, pressed]);
        });
    }

    /* ── WASM audio: playback via ScriptProcessorNode ──────────── */
    var RING_SIZE = WS_AUDIO_RING_SIZE;

    if (playToggle) {
        playToggle.addEventListener('click', function() {
            if (playbackNode && typeof playbackNode.disconnect === 'function') {
                playbackNode.disconnect();
                playbackNode = null;
                if (audioCtx) { audioCtx.close(); audioCtx = null; }
                this.textContent = 'Enable Playback';
                return;
            }

            var pbPtr = Module.ccall('ove_wasm_audio_get_playback_ptr', 'number');
            var heap32 = Module.HEAPU32 || new Uint32Array(Module.HEAPU8.buffer);
            var sampleRate = heap32[(pbPtr + 8) >> 2] || DEFAULT_SAMPLE_RATE;

            audioCtx = new AudioContext({ sampleRate: sampleRate });
            if (audioCtx.setSinkId && outSelect && outSelect.value)
                audioCtx.setSinkId(outSelect.value);

            var wpOff = pbPtr;
            var rpOff = pbPtr + 4;
            var bufOff = pbPtr + RING_HDR;

            var self = this;

            /* Read samples from WASM heap ring buffer into a Float32Array.
             * Used by both the ScriptProcessor callback and the waveform poller. */
            function drainRing(output) {
                var h32 = Module.HEAPU32 || new Uint32Array(Module.HEAPU8.buffer);
                var h8 = Module.HEAPU8;
                var wp = Atomics.load(h32, wpOff >> 2);
                var rp = Atomics.load(h32, rpOff >> 2);
                var avail = wp - rp;
                for (var i = 0; i < output.length; i++) {
                    if (i * 2 < avail) {
                        var bi = (rp + i * 2) & (RING_SIZE - 1);
                        var lo = h8[bufOff + bi];
                        var hi = h8[bufOff + ((bi + 1) & (RING_SIZE - 1))];
                        var s = (hi << 8) | lo;
                        if (s > 32767) s -= 65536;
                        output[i] = s / 32768.0;
                    } else {
                        output[i] = 0;
                    }
                }
                var consumed = Math.min(avail, output.length * 2);
                Atomics.store(h32, rpOff >> 2, rp + consumed);
            }

            /* ScriptProcessorNode: simple, works everywhere. */
            playbackNode = audioCtx.createScriptProcessor(1024, 0, 1);
            playbackNode.onaudioprocess = function(e) {
                var output = e.outputBuffer.getChannelData(0);
                drainRing(output);
                scheduleWaveDraw('waveform-output', output, '#e94560');
            };
            playbackNode.connect(audioCtx.destination);
            self.textContent = 'Disable Playback';
            if (audioInfoEl)
                audioInfoEl.textContent = sampleRate + ' Hz / mono / 16bit';
        });
    }

    /* ── WASM audio: capture via source selector ─────────────── */
    /* Mic capture for WASM — writes directly to SharedArrayBuffer.
     * Triggered by the shared audio-source-select dropdown. */
    var wSourceSel = document.getElementById('audio-source-select');
    if (wSourceSel) {
        wSourceSel.addEventListener('change', function() {
            stopAllInput();
        });
    }

    /* Start WASM mic when source is "mic" and user clicks input area. */
    var wInputSel = document.getElementById('audio-input-select');
    if (wInputSel) {
        wInputSel.addEventListener('change', function() {
            if (!wSourceSel || wSourceSel.value !== 'mic') return;
            stopMicCapture();
            /* Restart mic with new device. */
            var constraints = { audio: this.value
                ? { deviceId: { exact: this.value } } : true };
            navigator.mediaDevices.getUserMedia(constraints).then(function(stream) {
                captureStream = stream;
                var capPtr = Module.ccall('ove_wasm_audio_get_capture_ptr', 'number');
                var tmpH32 = Module.HEAPU32 || new Uint32Array(Module.HEAPU8.buffer);
                var appRate = tmpH32[(capPtr + 8) >> 2] || DEFAULT_SAMPLE_RATE;
                if (!audioCtx) audioCtx = new AudioContext({ sampleRate: appRate });

                var wpOff = capPtr;
                var rpOff = capPtr + 4;
                var bufOff = capPtr + RING_HDR;

                captureSource = audioCtx.createMediaStreamSource(stream);
                captureNode = audioCtx.createScriptProcessor(1024, 1, 1);
                captureNode.onaudioprocess = function(e) {
                    var input = e.inputBuffer.getChannelData(0);
                    var h32 = Module.HEAPU32 || new Uint32Array(Module.HEAPU8.buffer);
                    var h8 = Module.HEAPU8;
                    var wp = Atomics.load(h32, wpOff >> 2);
                    var rp = Atomics.load(h32, rpOff >> 2);
                    var free = RING_SIZE - (wp - rp);
                    var bytesToWrite = Math.min(input.length * 2, free);
                    var samplesToWrite = bytesToWrite >> 1;
                    for (var i = 0; i < samplesToWrite; i++) {
                        var s = Math.max(-1, Math.min(1, input[i]));
                        var val = (s < 0 ? s * 32768 : s * 32767) | 0;
                        var bi = (wp + i * 2) & (RING_SIZE - 1);
                        h8[bufOff + bi] = val & 0xFF;
                        h8[bufOff + ((bi + 1) & (RING_SIZE - 1))] = (val >> 8) & 0xFF;
                    }
                    Atomics.store(h32, wpOff >> 2, wp + samplesToWrite * 2);
                    scheduleWaveDraw('waveform-input', input, '#4ecca3');
                };
                captureSource.connect(captureNode);
                captureNode.connect(audioCtx.destination);
            }).catch(function(err) {
                console.error('Mic access denied:', err);
            });
        });
    }

    /* Device enumeration (shared with POSIX path). */
    refreshAudioDevices();
    if (navigator.mediaDevices)
        navigator.mediaDevices.addEventListener('devicechange', refreshAudioDevices);
};

/* ══════════════════════════════════════════════════════════════════════
   Init: detect mode and start
   ══════════════════════════════════════════════════════════════════════ */
if (!isWasmMode) {
    /* POSIX mode: connect WebSocket, set up console input. */
    initConsoleInput();
    connect();
}
