/*
 * oveRTOS Simulation Dashboard
 *
 * Shared between POSIX (WebSocket) and WASM (Emscripten) modes.
 * Provides: event log, WinBox window management, audio waveform (POSIX).
 */

"use strict";

/* ── Frame type constants (shared with ove-dashboard-bridge.py) ────── */
var FRAME_FB        = 0x01;
var FRAME_AUDIO     = 0x02;
var FRAME_EVENT     = 0x03;
var FRAME_CMD       = 0x04;
var FRAME_STATE     = 0x05;
var FRAME_LOG       = 0x06;
var FRAME_INPUT     = 0x07;
var FRAME_THREAD    = 0x0A;
var FRAME_FILE_LIST = 0x0B;
var FRAME_FILE_REQ  = 0x0C;
var FRAME_FILE_RESP = 0x0D;
var FRAME_TRACE     = 0x0E;
var FRAME_PROFILE   = 0x0F;

/* ── Shared constants ────────────────────────────────────────────────── */
var RING_HDR            = 32;     /* OVE_RING_OFF_BUF — ring header size in bytes */
var MAX_LOG_ENTRIES     = 200;    /* max entries in event log before trimming */
var RECONNECT_BASE_MS  = 500;    /* initial WebSocket reconnect delay */
var RECONNECT_MAX_MS   = 5000;   /* max reconnect delay after backoff */
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
    display: { title: "Display",  col: 0, row: 0, cw: 0.6, rh: 0.55 },
    threads: { title: "Threads",  col: 1, row: 0, cw: 0.4, rh: 0.55 },
    debug:   { title: "Debug",    col: 0, row: 1, cw: 0.6, rh: 0.45 },
    audio:   { title: "Audio",    col: 0, row: 1, cw: 0.5, rh: 0.4 },
    events:  { title: "Events",   col: 0, row: 1, cw: 0.5, rh: 0.4 },
    trace:   { title: "Trace",    col: 0, row: 1, cw: 1.0, rh: 0.35 },
    profiler:{ title: "Profiler", col: 0, row: 1, cw: 1.0, rh: 0.4  },
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
        /* Drop prior-session state — timebase and symbol maps are not
         * comparable across a sim restart. */
        traceResetOnReconnect();
        profilerResetOnReconnect();
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
            case FRAME_FB:         handleFramebuffer(buf, 4); break;
            case FRAME_AUDIO:      handleAudio(buf, 4); break;
            case FRAME_EVENT:      handleEvent(buf, 4); break;
            case FRAME_STATE:      handleState(buf, 4); break;
            case FRAME_LOG:        handleLog(buf, 4); break;
            case FRAME_THREAD:     handleThreadSnapshot(buf, 4); break;
            case FRAME_DEBUG_RESP: handleDebugResp(buf, 4); break;
            case FRAME_FILE_LIST:  handleFileList(buf, 4); break;
            case FRAME_FILE_RESP:  handleFileResp(buf, 4); break;
            case FRAME_TRACE:      handleTrace(buf, 4); break;
            case FRAME_PROFILE:    handleProfile(buf, 4); break;
        }
    };
}

/* ── Lazy window creation (shared by POSIX + WASM modes) ─────────────
 * WASM mode used to early-return here because initWasmMode explicitly
 * created a fixed set of windows up front.  For threads/debug to appear
 * lazily on the first FRAME_THREAD / FRAME_FILE_LIST, the handler-driven
 * path needs to work here too.  The !wins[id] guard below already
 * prevents double-creation. */
function ensureWindow(id) {
    if (!WIN_DEFS[id]) return;
    if (document.getElementById(id + "-panel") && !wins[id]) {
        createDashboardWindow(id, WIN_DEFS[id].title);
        /* Re-tile all windows to accommodate the new one. */
        autoLayout();
        /* Attach input handlers when display panel is created. */
        if (id === "display") attachCanvasInput();
        if (id === "profiler") profilerBindPanel();
        if (id === "trace") traceBindPanel();
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

/* ── Thread snapshot (FRAME_THREAD) ───────────────────────────────── */

var THREAD_STATE_NAMES = ["running", "ready", "blocked", "suspended", "terminated", "unknown"];
var THREAD_STATE_CSS   = ["st-running", "st-ready", "st-blocked", "st-suspended", "st-terminated", "st-unknown"];

/* Mirrors the ove_prio_t enum in include/ove/thread.h. */
var THREAD_PRIO_NAMES = [
    "idle", "low", "below-normal", "normal",
    "above-normal", "high", "realtime", "critical"
];

function handleThreadSnapshot(buf, off) {
    ensureWindow("threads");
    var view = new DataView(buf, off);
    var len = buf.byteLength - off;
    if (len < 17) return; /* 1 + 4*4 minimum */

    var pos = 0;
    var threadCount = view.getUint8(pos); pos += 1;
    var heapTotal = view.getUint32(pos, true); pos += 4;
    var heapFree  = view.getUint32(pos, true); pos += 4;
    var heapUsed  = view.getUint32(pos, true); pos += 4;
    var heapPeak  = view.getUint32(pos, true); pos += 4;

    /* Update heap bar */
    var memFill = document.getElementById("thread-mem-fill");
    var memText = document.getElementById("thread-mem-text");
    if (memFill && heapTotal > 0) {
        var pct = Math.round(heapUsed * 100 / heapTotal);
        memFill.style.width = pct + "%";
        memFill.className = "thread-mem-fill" + (pct > 90 ? " mem-critical" : pct > 70 ? " mem-warn" : "");
    }
    if (memText) {
        if (heapTotal > 0)
            memText.textContent = _fmtBytes(heapUsed) + " / " + _fmtBytes(heapTotal) + " (peak " + _fmtBytes(heapPeak) + ")";
        else
            memText.textContent = "--";
    }

    /* Parse thread entries */
    var threads = [];
    for (var i = 0; i < threadCount; i++) {
        if (pos >= len) break;
        var nameLen = view.getUint8(pos); pos += 1;
        if (pos + nameLen + 10 > len) break;
        var nameBytes = new Uint8Array(buf, off + pos, nameLen);
        var name = new TextDecoder().decode(nameBytes);
        pos += nameLen;
        var state = view.getUint8(pos); pos += 1;
        var priority = view.getUint8(pos); pos += 1;
        var stackUsed = view.getUint32(pos, true); pos += 4;
        var stackSize = view.getUint32(pos, true); pos += 4;
        var cpuX100 = view.getUint32(pos, true); pos += 4;
        var stRunning = view.getUint32(pos, true); pos += 4;
        var stReady = view.getUint32(pos, true); pos += 4;
        var stBlocked = view.getUint32(pos, true); pos += 4;
        var stSuspended = view.getUint32(pos, true); pos += 4;
        threads.push({ name: name, state: state, priority: priority,
                        stackUsed: stackUsed, stackSize: stackSize,
                        cpuX100: cpuX100,
                        stRunning: stRunning, stReady: stReady,
                        stBlocked: stBlocked, stSuspended: stSuspended });
    }

    /* Render table */
    var tbody = document.getElementById("thread-tbody");
    var empty = document.getElementById("thread-empty");
    if (!tbody) return;

    if (threads.length === 0) {
        tbody.innerHTML = "";
        if (empty) empty.style.display = "block";
        return;
    }
    if (empty) empty.style.display = "none";

    var html = "";
    for (var j = 0; j < threads.length; j++) {
        var t = threads[j];
        var stIdx = t.state < THREAD_STATE_NAMES.length ? t.state : 5;
        var stName = THREAD_STATE_NAMES[stIdx];
        var stCss = THREAD_STATE_CSS[stIdx];
        var cpuStr = (t.cpuX100 / 100).toFixed(1);
        html += "<tr>"
              + "<td class=\"td-name\">" + _esc(t.name) + "</td>"
              + "<td><span class=\"state-badge " + stCss + "\">" + stName + "</span></td>"
              + "<td>" + (THREAD_PRIO_NAMES[t.priority] || t.priority) + "</td>"
              + "<td class=\"td-num\">" + _fmtStack(t.stackUsed, t.stackSize) + "</td>"
              + "<td class=\"td-num\">" + cpuStr + "%</td>"
              + "<td>" + _fmtStateBar(t) + "</td>"
              + "</tr>";
    }
    tbody.innerHTML = html;
}

function _fmtStateBar(t) {
    var r = (t.stRunning || 0) / 100;
    var rd = (t.stReady || 0) / 100;
    var b = (t.stBlocked || 0) / 100;
    var s = (t.stSuspended || 0) / 100;
    var total = r + rd + b + s;
    if (total < 0.1) return "<span class=\"td-num\" style=\"color:#555\">--</span>";
    /* Normalize to 100% */
    var rp = (r / total * 100).toFixed(0);
    var rdp = (rd / total * 100).toFixed(0);
    var bp = (b / total * 100).toFixed(0);
    var sp = (s / total * 100).toFixed(0);
    return "<div class=\"state-bar\" title=\"run:" + rp + "% rdy:" + rdp + "% blk:" + bp + "% sus:" + sp + "%\">"
         + "<span class=\"sb-run\" style=\"width:" + rp + "%\"></span>"
         + "<span class=\"sb-rdy\" style=\"width:" + rdp + "%\"></span>"
         + "<span class=\"sb-blk\" style=\"width:" + bp + "%\"></span>"
         + "<span class=\"sb-sus\" style=\"width:" + sp + "%\"></span>"
         + "</div>";
}

function _fmtStack(used, total) {
    if (total > 0)
        return _fmtBytes(used) + " / " + _fmtBytes(total);
    if (used > 0)
        return _fmtBytes(used);
    return "--";
}

function _fmtBytes(n) {
    if (n >= 1048576) return (n / 1048576).toFixed(1) + " MB";
    if (n >= 1024) return (n / 1024).toFixed(1) + " KB";
    return n + " B";
}

function _esc(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

/* ── Debug controller (FRAME_DEBUG_CMD / FRAME_DEBUG_RESP) ───────── */

var FRAME_DEBUG_CMD  = 0x08;
var FRAME_DEBUG_RESP = 0x09;

var DBG_CMD_CONTINUE    = 0x01;
var DBG_CMD_PAUSE       = 0x02;
var DBG_CMD_STEP_OVER   = 0x03;
var DBG_CMD_STEP_INTO   = 0x04;
var DBG_CMD_STEP_OUT    = 0x05;
var DBG_CMD_RESET       = 0x06;
var DBG_CMD_BACKTRACE   = 0x07;
var DBG_CMD_REGISTERS   = 0x08;
var DBG_CMD_DISASSEMBLE = 0x09;
var DBG_CMD_BP_SET      = 0x0A;
var DBG_CMD_BP_CLEAR    = 0x0B;
var DBG_CMD_SOURCE      = 0x0C;

var DBG_RESP_STATE       = 0x00;
var DBG_RESP_BACKTRACE   = 0x07;
var DBG_RESP_REGISTERS   = 0x08;
var DBG_RESP_DISASSEMBLY = 0x09;
var DBG_RESP_BREAKPOINT  = 0x0A;
var DBG_RESP_SOURCE      = 0x0C;

var dbgState = "disconnected";  /* disconnected | running | stopped */
var dbgBreakpoints = [];
var dbgSourceCache = {};  /* file → {lines, startLine} */
var dbgCurrentFile = null;
var dbgCurrentLine = 0;
var dbgCurrentAddr = null;
var dbgActiveTab = "source";
var dbgLastDisasmData = null;  /* cached disassembly response */
var dbgLastSourceData = null;  /* cached source response */

/* ── Monaco Editor instance for source view ──────────────────────── */
var monacoEditor = null;
var monacoReady = false;
var monacoCurrentDecorations = [];  /* decoration IDs for current line + breakpoints */

(function initMonaco() {
    if (typeof require === "undefined") return;
    require.config({ paths: { vs: "https://cdn.jsdelivr.net/npm/monaco-editor@0.52.2/min/vs" }});
    require(["vs/editor/editor.main"], function () {
        monacoReady = true;
        /* Editor will be created lazily when the debug panel exists */
    });
})();

function _ensureMonacoEditor() {
    if (monacoEditor) return monacoEditor;
    if (!monacoReady) return null;
    var container = document.getElementById("dbg-source-view");
    if (!container) return null;
    monacoEditor = monaco.editor.create(container, {
        value: "// Waiting for debug session...\n",
        language: "c",
        theme: "vs-dark",
        readOnly: true,
        minimap: { enabled: false },
        scrollBeyondLastLine: false,
        fontSize: 12,
        lineNumbersMinChars: 4,
        glyphMargin: true,
        folding: false,
        renderLineHighlight: "none",
        overviewRulerLanes: 0,
        hideCursorInOverviewRuler: true,
        contextmenu: false,
        automaticLayout: true,
    });
    /* Click glyph margin to toggle breakpoint */
    monacoEditor.onMouseDown(function (e) {
        if (e.target.type === monaco.editor.MouseTargetType.GUTTER_GLYPH_MARGIN
            || e.target.type === monaco.editor.MouseTargetType.GUTTER_LINE_NUMBERS) {
            var lineNum = e.target.position.lineNumber;
            if (dbgCurrentFile && monacoEditor._oveStartLine) {
                _toggleBreakpoint(dbgCurrentFile, monacoEditor._oveStartLine + lineNum - 1);
            }
        }
    });
    return monacoEditor;
}

function _monacoSetDecorations(currentLine, startLine, file) {
    var editor = monacoEditor;
    if (!editor) return;
    var decorations = [];
    /* Current line highlight */
    if (currentLine && startLine) {
        var editorLine = currentLine - startLine + 1;
        if (editorLine > 0) {
            decorations.push({
                range: new monaco.Range(editorLine, 1, editorLine, 1),
                options: {
                    isWholeLine: true,
                    className: "monaco-current-line",
                    glyphMarginClassName: "monaco-current-glyph",
                }
            });
        }
    }
    /* Breakpoint markers */
    for (var i = 0; i < dbgBreakpoints.length; i++) {
        var bp = dbgBreakpoints[i];
        var bpFile = bp.fullname || bp.file || "";
        if (bpFile === file) {
            var bpEditorLine = parseInt(bp.line, 10) - startLine + 1;
            if (bpEditorLine > 0) {
                decorations.push({
                    range: new monaco.Range(bpEditorLine, 1, bpEditorLine, 1),
                    options: {
                        isWholeLine: true,
                        glyphMarginClassName: "monaco-bp-glyph",
                    }
                });
            }
        }
    }
    monacoCurrentDecorations = editor.deltaDecorations(monacoCurrentDecorations, decorations);
}

/* ── Trace swimlane (FRAME_TRACE) ───────────────────────────────── */

var TRACE_KIND_STATE = 1;
var TRACE_KIND_MARK  = 2;

/* Rolling window in simulation time (microseconds). */
var TRACE_WINDOW_US  = 30 * 1000 * 1000;
/* Hard memory cap on each of traceSpans / traceMarks — evict oldest first.
 * Memory is bounded, not workload-sized: one entry ~80 B in V8 so the pair
 * of arrays peaks at ~10 MB. Measured rate on example_c was ~700 rec/s
 * (state 617 + marks 90), so a typical 30 s window fits in ~21 k records;
 * the cap only clamps pathological bursts where the in-kernel ring would
 * otherwise flood the browser before time-based eviction ran. */
var TRACE_MAX_RECORDS = 60000;
/* Plot geometry: left label column + right padding. Shared by renderTrace
 * and the pan/zoom pointer handlers so pixel↔time math matches exactly. */
var TRACE_LABEL_W = 90;
var TRACE_PLOT_PAD_R = 4;

var TRACE_STATE_COLORS = {
    0: "#4ecca3", /* RUNNING */
    1: "#f5d76e", /* READY */
    2: "#e94560", /* BLOCKED */
    3: "#666666", /* SUSPENDED */
    4: "#222222", /* TERMINATED */
};

/* Names for OVE_TRACE_PRIM_* / OVE_TRACE_ACT_* nibble codes in mark.code. */
function tracePrimName(p) {
    return p === 1 ? "mutex"
         : p === 2 ? "sem"
         : p === 3 ? "event"
         : p === 4 ? "cv"
         : p === 5 ? "queue"
         : p === 15 ? "user"
         : "prim" + p;
}
function traceActName(a) {
    return a === 1 ? "wait_enter"
         : a === 2 ? "wait_exit"
         : a === 3 ? "post"
         : a === 4 ? "user"
         : "act" + a;
}

/* Per-thread lane state: last state + last timestamp (for closing span). */
var traceThreads = {};            /* tid -> { name, lastState, lastTs } */
var traceSpans   = [];            /* [{tid, start, end, state}] */
var traceMarks   = [];            /* [{tid, ts, prim, act}] */
var traceDropped = 0;
var traceMinTs   = 0;             /* earliest observed ts */
var traceMaxTs   = 0;             /* latest observed ts */
/* spanUs = visible time window in µs. follow = auto-track latest ts (live).
 * When follow is turned off, tEndFrozen captures the anchor so new samples
 * keep filling the ring but the display stays fixed. */
var TRACE_MIN_SPAN_US = 100;
var traceView    = { spanUs: TRACE_WINDOW_US, follow: true, tEndFrozen: 0 };
var traceDirty   = false;
var traceRafScheduled = false;

/* Clear timebase-tied trace state so a sim restart can't leave stale
 * spans/marks plotted against a reused timestamp range. User settings
 * (zoom span, tab) are preserved; follow is forced back on since the
 * paused tEndFrozen anchor belongs to the prior session. */
function traceResetOnReconnect() {
    traceThreads = {};
    traceSpans = [];
    traceMarks = [];
    traceDropped = 0;
    traceMinTs = 0;
    traceMaxTs = 0;
    traceView.follow = true;
    traceView.tEndFrozen = 0;
    var follow = document.getElementById("trace-follow");
    if (follow) follow.checked = true;
    scheduleTraceRender();
}

function traceEvict() {
    if (traceSpans.length > TRACE_MAX_RECORDS) {
        traceSpans.splice(0, traceSpans.length - TRACE_MAX_RECORDS);
    }
    if (traceMarks.length > TRACE_MAX_RECORDS) {
        traceMarks.splice(0, traceMarks.length - TRACE_MAX_RECORDS);
    }
    /* Drop spans older than the window from our retention anchor.
     * When paused we anchor at the frozen view edge (not traceMaxTs), so
     * new samples keep flowing in but the left side of the paused display
     * isn't evicted as simulation time advances. The TRACE_MAX_RECORDS cap
     * above still bounds total memory. */
    var anchor = traceView.follow ? traceMaxTs : traceView.tEndFrozen;
    var cutoff = anchor - TRACE_WINDOW_US;
    while (traceSpans.length && traceSpans[0].end < cutoff) traceSpans.shift();
    while (traceMarks.length && traceMarks[0].ts  < cutoff) traceMarks.shift();
    if (traceSpans.length) traceMinTs = traceSpans[0].start;
}

function handleTrace(buf, off) {
    if (buf.byteLength - off < 8) return;
    var dv = new DataView(buf, off);
    var subType = dv.getUint8(0);
    /* version at offset 1 — unused for now */
    var count   = dv.getUint16(2, true);
    var dropped = dv.getUint32(4, true);
    traceDropped += dropped;

    var base = 8;
    if (subType === 0) {
        /* DESCRIPTORS — tid/name pairs. */
        var p = base;
        for (var i = 0; i < count; i++) {
            if (p + 5 > dv.byteLength) break;
            var tid = dv.getUint32(p, true); p += 4;
            var nameLen = dv.getUint8(p); p += 1;
            if (p + nameLen > dv.byteLength) break;
            var nameBytes = new Uint8Array(buf, off + p, nameLen);
            var name = new TextDecoder().decode(nameBytes);
            p += nameLen;
            if (!traceThreads[tid]) traceThreads[tid] = { name: name, lastState: -1, lastTs: 0 };
            else traceThreads[tid].name = name;
        }
        traceDirty = true;
        ensureWindow("trace");
        scheduleTraceRender();
        return;
    }
    if (subType !== 1) return;

    /* STREAM — 16-byte records. */
    for (var j = 0; j < count; j++) {
        var p0 = base + j * 16;
        if (p0 + 16 > dv.byteLength) break;
        /* ts_us: read as two uint32 LE and combine. Safe up to 2^53 us. */
        var tsLo = dv.getUint32(p0, true);
        var tsHi = dv.getUint32(p0 + 4, true);
        var ts = tsHi * 0x100000000 + tsLo;
        var tid = dv.getUint32(p0 + 8, true);
        var kind = dv.getUint8(p0 + 12);
        var code = dv.getUint8(p0 + 13);
        var arg  = dv.getUint16(p0 + 14, true);

        if (ts > traceMaxTs) traceMaxTs = ts;
        if (traceMinTs === 0 || ts < traceMinTs) traceMinTs = ts;

        var th = traceThreads[tid];
        if (!th) {
            th = traceThreads[tid] = { name: "0x" + tid.toString(16), lastState: -1, lastTs: ts };
        }

        if (kind === TRACE_KIND_STATE) {
            /* Close out the previous span for this thread. */
            if (th.lastState >= 0 && ts > th.lastTs) {
                traceSpans.push({ tid: tid, start: th.lastTs, end: ts, state: th.lastState });
            }
            th.lastState = code;
            th.lastTs = ts;
        } else if (kind === TRACE_KIND_MARK) {
            traceMarks.push({ tid: tid, ts: ts,
                              prim: (code >> 4) & 0x0F, act: code & 0x0F,
                              arg: arg });
        }
    }

    traceEvict();
    traceDirty = true;
    ensureWindow("trace");
    scheduleTraceRender();
}

function scheduleTraceRender() {
    if (traceRafScheduled) return;
    traceRafScheduled = true;
    requestAnimationFrame(function () {
        traceRafScheduled = false;
        renderTrace();
    });
}

function renderTrace() {
    var cvs = document.getElementById("trace-canvas");
    if (!cvs) return;
    var wrap = cvs.parentElement;
    if (!wrap) return;

    /* Match the canvas backing store to its CSS size (device pixels). */
    var dpr = window.devicePixelRatio || 1;
    var cssW = wrap.clientWidth;
    var cssH = wrap.clientHeight;
    if (cssW <= 0 || cssH <= 0) return;
    if (cvs.width !== cssW * dpr || cvs.height !== cssH * dpr) {
        cvs.width = cssW * dpr;
        cvs.height = cssH * dpr;
    }
    var ctx = cvs.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cssW, cssH);

    /* Collect live tids in insertion order. */
    var tids = [];
    for (var k in traceThreads) tids.push(+k);
    if (tids.length === 0) {
        ctx.fillStyle = "#888";
        ctx.font = "11px 'Consolas', monospace";
        ctx.fillText("Waiting for trace data…", 10, 20);
        return;
    }
    tids.sort(function (a, b) {
        var na = traceThreads[a].name, nb = traceThreads[b].name;
        return na < nb ? -1 : (na > nb ? 1 : 0);
    });

    var laneH = Math.max(14, Math.min(28, Math.floor((cssH - 20) / tids.length)));
    var plotX = TRACE_LABEL_W;
    var plotW = cssW - TRACE_LABEL_W - TRACE_PLOT_PAD_R;
    var axisY = 0;
    var axisH = 14;

    /* Time range: live follows traceMaxTs; paused anchors to tEndFrozen.
     * Visible span is traceView.spanUs (zoom controls). */
    var tEnd = traceView.follow ? traceMaxTs : traceView.tEndFrozen;
    var tStart = tEnd - traceView.spanUs;
    var span = tEnd - tStart;
    if (span <= 0) span = 1;

    /* Axis ticks every 1 s. */
    ctx.fillStyle = "#2a2f3a";
    ctx.fillRect(plotX, axisY, plotW, axisH);
    ctx.strokeStyle = "#40475a";
    ctx.fillStyle = "#8a94ad";
    ctx.font = "10px 'Consolas', monospace";
    var tickStep = 1000000;
    if (span < 5e6) tickStep = 500000;
    if (span > 20e6) tickStep = 2000000;
    var firstTick = Math.ceil(tStart / tickStep) * tickStep;
    for (var t = firstTick; t <= tEnd; t += tickStep) {
        var x = plotX + ((t - tStart) / span) * plotW;
        ctx.beginPath();
        ctx.moveTo(x, axisY);
        ctx.lineTo(x, axisY + axisH);
        ctx.stroke();
        ctx.fillText((t / 1e6).toFixed(1) + "s", x + 2, axisY + axisH - 2);
    }

    /* Lanes. */
    for (var i = 0; i < tids.length; i++) {
        var tid = tids[i];
        var th = traceThreads[tid];
        var ly = axisY + axisH + i * laneH;

        /* Lane label. */
        ctx.fillStyle = (i % 2 === 0) ? "#181c28" : "#141826";
        ctx.fillRect(0, ly, cssW, laneH);
        ctx.fillStyle = "#cfd3dc";
        ctx.fillText(th.name, 4, ly + laneH / 2 + 3);
    }

    /* Draw spans. */
    for (var s = 0; s < traceSpans.length; s++) {
        var sp = traceSpans[s];
        if (sp.end < tStart || sp.start > tEnd) continue;
        var row = tids.indexOf(sp.tid);
        if (row < 0) continue;
        var x0 = plotX + ((Math.max(sp.start, tStart) - tStart) / span) * plotW;
        var x1 = plotX + ((Math.min(sp.end, tEnd) - tStart) / span) * plotW;
        var w  = Math.max(1, x1 - x0);
        var y  = axisY + axisH + row * laneH + 2;
        ctx.fillStyle = TRACE_STATE_COLORS[sp.state] || "#888";
        ctx.fillRect(x0, y, w, laneH - 4);
    }

    /* Draw in-flight spans (open: lastTs -> tEnd). */
    for (var ti = 0; ti < tids.length; ti++) {
        var tid2 = tids[ti];
        var th2 = traceThreads[tid2];
        if (th2.lastState < 0 || th2.lastTs >= tEnd) continue;
        var xs = plotX + ((Math.max(th2.lastTs, tStart) - tStart) / span) * plotW;
        var xe = plotX + plotW;
        var ys = axisY + axisH + ti * laneH + 2;
        ctx.fillStyle = TRACE_STATE_COLORS[th2.lastState] || "#888";
        ctx.fillRect(xs, ys, xe - xs, laneH - 4);
    }

    /* Draw markers as small triangles at the top of the lane. */
    ctx.fillStyle = "#ffffff";
    for (var m = 0; m < traceMarks.length; m++) {
        var mk = traceMarks[m];
        if (mk.ts < tStart || mk.ts > tEnd) continue;
        var mrow = tids.indexOf(mk.tid);
        if (mrow < 0) continue;
        var mx = plotX + ((mk.ts - tStart) / span) * plotW;
        var my = axisY + axisH + mrow * laneH + 1;
        ctx.beginPath();
        ctx.moveTo(mx, my);
        ctx.lineTo(mx - 3, my + 4);
        ctx.lineTo(mx + 3, my + 4);
        ctx.closePath();
        ctx.fill();
    }

    /* Info line. */
    var info = document.getElementById("trace-info");
    if (info) {
        info.textContent = tids.length + " threads, "
            + traceSpans.length + " spans, "
            + traceMarks.length + " marks"
            + (traceDropped ? ", " + traceDropped + " dropped" : "");
    }
}

/* Re-render on panel resize / visibility changes. */
window.addEventListener("resize", scheduleTraceRender);

/* Compute the plot area (CSS px) for the current canvas size. Returns
 * null if the canvas isn't laid out yet. */
function traceGetPlot() {
    var cvs = document.getElementById("trace-canvas");
    if (!cvs) return null;
    var rect = cvs.getBoundingClientRect();
    var plotW = rect.width - TRACE_LABEL_W - TRACE_PLOT_PAD_R;
    if (plotW <= 0) return null;
    return { cssW: rect.width, plotX: TRACE_LABEL_W, plotW: plotW };
}

/* Current right-edge timestamp, honouring live vs paused. */
function traceCurrentTEnd() {
    return traceView.follow ? traceMaxTs : traceView.tEndFrozen;
}

/* Clamp tEnd so the view can't scroll past live (latest) and stays
 * anchored to where we actually have data on the left. */
function traceClampTEnd(tEnd) {
    if (tEnd > traceMaxTs) tEnd = traceMaxTs;
    /* Allow scrolling back to traceMinTs as the right edge — left of the
     * view will then be empty, which is fine. */
    if (traceMinTs > 0 && tEnd < traceMinTs) tEnd = traceMinTs;
    return tEnd;
}

/* Enter paused mode anchored at the given tEnd. Idempotent. */
function traceEnterPaused(tEnd, followCheckbox) {
    traceView.follow = false;
    traceView.tEndFrozen = traceClampTEnd(tEnd);
    if (followCheckbox) followCheckbox.checked = false;
}

/* Button + checkbox + canvas wiring — installed on first trace panel visibility. */
function traceBindPanel() {
    if (traceBindPanel._bound) return;
    traceBindPanel._bound = true;

    var zoomIn  = document.getElementById("trace-zoom-in");
    var zoomOut = document.getElementById("trace-zoom-out");
    var zoomFit = document.getElementById("trace-zoom-fit");
    var follow  = document.getElementById("trace-follow");
    var cvs     = document.getElementById("trace-canvas");

    function clampSpan(s) {
        if (s < TRACE_MIN_SPAN_US) s = TRACE_MIN_SPAN_US;
        if (s > TRACE_WINDOW_US)   s = TRACE_WINDOW_US;
        return s;
    }

    /* Zoom keeping the anchor point at the same time. In live mode the
     * anchor is the right edge (traceMaxTs), so spanUs alone changes.
     * In paused mode anchor is the supplied CSS x — or the midpoint if
     * none is given. */
    function applyZoom(factor, xCssOpt) {
        var nextSpan = clampSpan(traceView.spanUs * factor);
        if (traceView.follow) {
            traceView.spanUs = nextSpan;
            scheduleTraceRender();
            return;
        }
        var plot = traceGetPlot();
        if (!plot) { traceView.spanUs = nextSpan; scheduleTraceRender(); return; }
        var tEnd = traceView.tEndFrozen;
        var tStart = tEnd - traceView.spanUs;
        var xCss = (typeof xCssOpt === "number")
            ? xCssOpt : plot.plotX + plot.plotW / 2;
        var frac = (xCss - plot.plotX) / plot.plotW;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        var tAtCursor = tStart + frac * traceView.spanUs;
        traceView.spanUs = nextSpan;
        traceView.tEndFrozen = traceClampTEnd(tAtCursor + (1 - frac) * nextSpan);
        scheduleTraceRender();
    }

    if (zoomIn)  zoomIn.addEventListener("click",  function () { applyZoom(1 / 1.5); });
    if (zoomOut) zoomOut.addEventListener("click", function () { applyZoom(1.5); });
    if (zoomFit) zoomFit.addEventListener("click", function () {
        traceView.spanUs = TRACE_WINDOW_US;
        traceView.follow = true;
        if (follow) follow.checked = true;
        scheduleTraceRender();
    });

    if (follow) {
        follow.addEventListener("change", function () {
            traceView.follow = follow.checked;
            if (!traceView.follow) traceView.tEndFrozen = traceMaxTs;
            scheduleTraceRender();
        });
    }

    if (!cvs) return;

    /* ── Pan (mouse drag) ──────────────────────────────────────────
     * Dragging auto-pauses; drag right moves the view into older data
     * (grab-the-timeline metaphor). Releasing at the live edge flips
     * follow back on so the display resumes tracking. */
    cvs.style.cursor = "grab";
    var drag = null;

    cvs.addEventListener("mousedown", function (e) {
        var plot = traceGetPlot();
        if (!plot) return;
        var rect = cvs.getBoundingClientRect();
        var xCss = e.clientX - rect.left;
        if (xCss < plot.plotX) return; /* clicks in the label column */
        var startTEnd = traceCurrentTEnd();
        traceEnterPaused(startTEnd, follow);
        drag = { startX: e.clientX, startTEnd: startTEnd,
                 pxPerUs: plot.plotW / traceView.spanUs };
        cvs.style.cursor = "grabbing";
        e.preventDefault();
    });

    window.addEventListener("mousemove", function (e) {
        if (!drag) return;
        var deltaUs = (e.clientX - drag.startX) / drag.pxPerUs;
        traceView.tEndFrozen = traceClampTEnd(drag.startTEnd - deltaUs);
        scheduleTraceRender();
    });

    window.addEventListener("mouseup", function () {
        if (!drag) return;
        drag = null;
        cvs.style.cursor = "grab";
        /* If the user scrolled all the way back to live, re-enable follow. */
        if (traceView.tEndFrozen >= traceMaxTs) {
            traceView.follow = true;
            if (follow) follow.checked = true;
            scheduleTraceRender();
        }
    });

    /* ── Zoom (mouse wheel) ────────────────────────────────────────
     * Wheel auto-pauses and zooms centered on the cursor's time. */
    cvs.addEventListener("wheel", function (e) {
        var plot = traceGetPlot();
        if (!plot) return;
        var rect = cvs.getBoundingClientRect();
        var xCss = e.clientX - rect.left;
        if (xCss < plot.plotX) return;
        /* Freeze the current view before zooming so the cursor's
         * time is well-defined. */
        if (traceView.follow) traceEnterPaused(traceMaxTs, follow);
        var factor = (e.deltaY < 0) ? (1 / 1.2) : 1.2;
        applyZoom(factor, xCss);
        e.preventDefault();
    }, { passive: false });

    /* ── Marker tooltip ────────────────────────────────────────────
     * Triangles are 6 px wide; widen hit tolerance to ±6 px so the
     * glyph is easy to land on. Pick the nearest mark in time on the
     * cursor's lane. */
    var tip = document.getElementById("trace-mark-tip");
    if (tip) {
        cvs.addEventListener("mousemove", function (e) {
            if (drag) { tip.style.display = "none"; return; }
            if (traceMarks.length === 0) { tip.style.display = "none"; return; }
            var plot = traceGetPlot();
            if (!plot) { tip.style.display = "none"; return; }
            var rect = cvs.getBoundingClientRect();
            var xCss = e.clientX - rect.left;
            var yCss = e.clientY - rect.top;
            if (xCss < plot.plotX) { tip.style.display = "none"; return; }

            /* Rebuild tids list the same way renderTrace does so lane
             * row math matches. Cheap — only runs on hover. */
            var tids = [];
            for (var k in traceThreads) tids.push(+k);
            if (tids.length === 0) { tip.style.display = "none"; return; }
            tids.sort(function (a, b) {
                var na = traceThreads[a].name, nb = traceThreads[b].name;
                return na < nb ? -1 : (na > nb ? 1 : 0);
            });

            var cssH = rect.height;
            var axisH = 14;
            var laneH = Math.max(14, Math.min(28,
                Math.floor((cssH - 20) / tids.length)));
            var row = Math.floor((yCss - axisH) / laneH);
            if (row < 0 || row >= tids.length) {
                tip.style.display = "none"; return;
            }
            var rowTid = tids[row];

            var tEnd = traceCurrentTEnd();
            var tStart = tEnd - traceView.spanUs;
            var span = tEnd - tStart; if (span <= 0) span = 1;

            var best = null, bestDx = Infinity;
            for (var i = traceMarks.length - 1; i >= 0; i--) {
                var mk = traceMarks[i];
                if (mk.ts < tStart || mk.ts > tEnd) continue;
                if (mk.tid !== rowTid) continue;
                var mx = plot.plotX + ((mk.ts - tStart) / span) * plot.plotW;
                var dx = Math.abs(mx - xCss);
                if (dx < bestDx) { bestDx = dx; best = mk; }
            }
            if (!best || bestDx > 6) {
                tip.style.display = "none"; return;
            }

            var name = traceThreads[best.tid]
                ? traceThreads[best.tid].name : ("0x" + best.tid.toString(16));
            tip.textContent = name + "  "
                + tracePrimName(best.prim) + " "
                + traceActName(best.act)
                + "  obj=0x" + best.arg.toString(16)
                + "  @ " + (best.ts / 1e6).toFixed(3) + "s";
            tip.style.display = "block";
            tip.style.left = (xCss + 12) + "px";
            tip.style.top  = (yCss + 12) + "px";
        });
        cvs.addEventListener("mouseleave", function () {
            tip.style.display = "none";
        });
    }
}

/* ── Sampling profiler (FRAME_PROFILE) ──────────────────────────────── */

var PROFILE_SUB_SAMPLES = 1;
var PROFILE_SUB_SYMBOLS = 2;
var PROFILE_SUB_CAPS    = 3;

/* Profiler plugin's cmd_type for "set sampling rate" (payload = u32 Hz). */
var PROFILER_CMD_SET_RATE = 100;

/* Capabilities received from the firmware — max_hz (compile-time ceiling)
 * and current_hz (what the backend is using right now). Arrive on the
 * first pump tick after transport is up. */
var profilerMaxHz     = 0;
var profilerCurrentHz = 0;

/* Memory safeguard, scaled with the active window at evict time. The
 * raw constant previously capped at 8000 regardless of window, which at
 * ~1.3 kHz multi-thread sample rates silently clipped the 30 s default
 * to ~6 s. Budget 3000 samples/s headroom (250 Hz × ~12 threads) so the
 * window setting actually governs retention. */
var PROFILER_SAMPLE_BUDGET_HZ = 3000;
var PROFILER_MIN_CAP = 8000;
var PROFILER_REFRESH_MS = 500;              /* ~2 Hz UI refresh */
var PROFILER_FLAT_TOP   = 40;               /* rows in flat table */
var PROFILER_WINDOW_MIN_S = 1;
var PROFILER_WINDOW_MAX_S = 360;

/* Skip list for flat-top / flame-graph leaf detection. Two purposes:
 *   (1) Signal-delivery bookkeeping frames — the target already drops
 *       them at source but build/glibc skew can leave leftovers.
 *   (2) Blocking-primitive leaves — when a sample lands on a thread
 *       parked in pthread_cond_wait / nanosleep / read, the leaf is
 *       libc syscall internals and the *calling* user-code frame is
 *       what actually matters for profiling. Skipping these reveals
 *       the user function doing the blocking (e.g. lv_thread_sync_wait).
 * Names are compared AFTER the GLIBC version suffix has been stripped. */
var PROFILER_SKIP_FRAMES = {
    /* handler / trampoline */
    "profile_sig_handler": 1,
    "__restore_rt": 1,
    "__sigaction": 1,
    "sigaction": 1,
    "__libc_sigaction": 1,
    /* futex / condvar internals — exported and non-exported misattribution targets */
    "__nptl_death_event": 1,
    "__futex_abstimed_wait_common": 1,
    "__futex_abstimed_wait_common64": 1,
    "__pthread_cond_wait_common": 1,
    "__pthread_cond_wait": 1,
    "pthread_cond_wait": 1,
    "pthread_cond_timedwait": 1,
    "pthread_cond_signal": 1,
    "__lll_lock_wait": 1,
    "__lll_lock_wake_private": 1,
    "__lll_unlock_wake_private": 1,
    /* sleep / time */
    "__nanosleep": 1,
    "nanosleep": 1,
    "clock_nanosleep": 1,
    "__clock_nanosleep": 1,
    "usleep": 1,
    /* basic I/O that often blocks */
    "__read": 1,
    "__write": 1,
    "__open": 1,
    "__close": 1,
    /* libc internal misattribution targets */
    "__nss_database_lookup": 1,
};

var profilerWindowUs    = 30 * 1000 * 1000; /* rolling window, mutable */
var profilerPaused      = false;
var profilerOnCpuOnly   = false; /* filter: keep only RUNNING/READY samples */
var profilerHidePump    = true;  /* hide the sim-debug pump thread (default on) */
var profilerHideIdle    = true;  /* hide RTOS IDLE / Tmr Svc (default on) */
var profilerApiOnly     = true;  /* collapse non-ove/non-app frames (default on) */
var profilerThreadFilter = 0;    /* 0 = all threads, else only this tid */
var profilerFlameZoom   = 1;     /* horizontal zoom multiplier (mouse-wheel) */
var PROFILER_FLAME_MAX_ZOOM = 20;

/* TIDs classified as the sim-debug pump thread. Populated lazily as
 * samples arrive and at least one frame resolves to debug_thread_fn —
 * avoids re-walking the stack on every render. */
var profilerPumpTids    = Object.create(null);

/* Case-insensitive match of a thread name against the RTOS system tasks
 * whose wall-clock dominance makes the aggregate view uninformative on
 * mostly-idle runs. Used both to build the thread dropdown (so the user
 * can still explicitly select e.g. IDLE) and to short-circuit samples
 * when the Hide-idle toggle is on. */
function profilerThreadIsIdle(name) {
    if (!name) return false;
    var lc = name.toLowerCase();
    return lc === "idle" || lc === "tmr svc" || lc === "timer svc";
}

/*
 * Symbol prefixes that the API-only toggle collapses. The goal is to
 * leave oveRTOS API (ove_*) and app code visible while hiding LVGL,
 * RTOS kernel, and libc internals. App code has no mandatory prefix,
 * so "app" is defined by elimination — anything that doesn't start
 * with one of these and isn't already filtered by PROFILER_SKIP_FRAMES
 * is treated as user code.
 *
 * Prefixes are matched with startsWith(). Be deliberately conservative
 * about adding an item — a prefix like "port" intentionally catches
 * every FreeRTOS portXXX symbol and will also drop any app function
 * starting with "port". Users can still turn the toggle off to see
 * everything.
 */
var PROFILER_EXCLUDE_PREFIXES = [
    /* LVGL */
    "lv_", "_lv_",
    /* Zephyr kernel + arch + sys helpers */
    "k_", "z_", "_k_", "_z_", "arch_", "sys_",
    "_isr_wrapper", "_interrupt_stack",
    /* FreeRTOS kernel */
    "xTask", "vTask", "uxTask",
    "xQueue", "vQueue",
    "xList", "vList",
    "xEvent", "vEvent",
    "xSemaphore",
    "xTimer", "vTimer",
    "prv", "pxCurrent",
    "xPort", "vPort", "port",
    /* NuttX kernel */
    "nxsched_", "nxtask_", "nxmutex_", "nxsem_",
    "nxcondvar_", "nxrmutex_", "nxsig_", "nxclock_", "nxnotify_",
    "nx_", "up_", "sched_", "group_",
    /* Cortex-M handlers + runtime */
    "HardFault_", "Reset_Handler", "Default_Handler",
    "SysTick_Handler", "PendSV_Handler", "SVC_Handler", "NMI_Handler",
    "__aeabi_", "__gnu_", "__cxa_",
    /* pthreads / libc (most already caught by PROFILER_SKIP_FRAMES) */
    "__pthread_", "__nptl_", "__futex_", "__lll_",
    "__libc_", "__restore_", "__errno", "__nss_",
];

/**
 * Return true when @name is an LVGL / RTOS kernel / libc internal
 * frame that the user almost certainly didn't write, and not the
 * oveRTOS API. Called by the API-only toggle in every render path so
 * flat-top, flame graph, and info-line denominators stay consistent.
 */
function profilerFrameIsExcluded(name) {
    if (!name) return false;
    if (name.indexOf("ove_") === 0) return false;
    for (var i = 0; i < PROFILER_EXCLUDE_PREFIXES.length; i++) {
        if (name.indexOf(PROFILER_EXCLUDE_PREFIXES[i]) === 0) return true;
    }
    return false;
}

/*
 * Return true if @sample came from the sim-debug pump thread. The pump
 * samples itself every tick (since it's the one flagging RUNNING
 * threads), and its stack is always pinned at debug_thread_fn →
 * sleep_ms — pure noise in the flame graph. Classification is cached
 * per tid so the check only walks the stack once per new thread.
 */
function profilerSampleIsPump(sample) {
    if (profilerPumpTids[sample.tid]) return true;
    var pcs = sample.pcs;
    for (var i = 0; i < pcs.length; i++) {
        var n = resolvePc(pcs[i]);
        if (n && n.indexOf("debug_thread_fn") !== -1) {
            profilerPumpTids[sample.tid] = 1;
            return true;
        }
    }
    return false;
}

/*
 * Apply all four profiler toggles — On-CPU, Hide pump, Hide idle, and
 * the per-thread dropdown — in one place so the three render paths
 * (info line, flat top, flame graph) can't drift out of sync. Returns
 * true when @s should contribute to aggregation.
 *
 * When the user has picked a specific thread, the pump/idle toggles are
 * intentionally bypassed: asking to look at e.g. sim_debug shouldn't
 * then drop every one of its samples because it's the pump.
 */
function profilerSampleIncluded(s) {
    if (profilerThreadFilter) {
        if (s.tid !== profilerThreadFilter) return false;
        if (profilerOnCpuOnly && s.state !== 0 && s.state !== 1) return false;
        return true;
    }
    if (profilerOnCpuOnly && s.state !== 0 && s.state !== 1) return false;
    if (profilerHidePump && profilerSampleIsPump(s)) return false;
    if (profilerHideIdle) {
        var th = traceThreads[s.tid];
        if (th && profilerThreadIsIdle(th.name)) return false;
    }
    return true;
}
var profilerSymbols     = []; /* sorted [pc_start, pc_end, name] */
var profilerSamples     = []; /* [{ts, tid, state, pcs:[...]}] */
var profilerDropped     = 0;
var profilerLastTs      = 0;
var profilerSymbolCount = 0;  /* for info line */
var profilerActiveTab   = "flame";
var profilerRefreshTimer = null;
var profilerDirty       = false;
var profilerFlameLayout = null; /* cached for hover lookup */
var profilerLastFlameData = null;

/* Clear timebase- and ELF-tied profiler state so a sim restart (new
 * timebase, potentially rebuilt binary with shifted symbols) can't mix
 * its caps/samples with the previous session's. User settings (window,
 * pause, on-cpu filter, active tab, refresh timer) are preserved;
 * capabilities + symbols are re-sent by the backend on reconnect. */
function profilerResetOnReconnect() {
    profilerMaxHz = 0;
    profilerCurrentHz = 0;
    profilerSymbols = [];
    profilerSamples = [];
    profilerDropped = 0;
    profilerLastTs = 0;
    profilerSymbolCount = 0;
    profilerFlameLayout = null;
    profilerLastFlameData = null;
    profilerPumpTids = Object.create(null);
    profilerThreadFilter = 0;
    var threadSel = document.getElementById("profiler-thread");
    if (threadSel) {
        threadSel.dataset.signature = "";
        threadSel.value = "0";
    }
    profilerDirty = true;
    if (typeof renderProfiler === "function") renderProfiler();
}

/**
 * Given a sample's pcs[] (leaf-first), return the index of the first
 * frame that is not a known signal-handler trampoline. All profiler
 * samples are captured inside a SIGRTMIN handler, so every stack starts
 * with the handler; treating pcs[0] as the leaf pegs flat-top at
 * profile_sig_handler 100%.
 */
function profilerLeafOffset(pcs) {
    for (var i = 0; i < pcs.length; i++) {
        var n = resolvePc(pcs[i]);
        if (PROFILER_SKIP_FRAMES[n]) continue;
        if (profilerApiOnly && profilerFrameIsExcluded(n)) continue;
        return i;
    }
    return pcs.length; /* all frames filtered — skip this sample */
}

/**
 * Resolve a PC to a symbol name via binary search. Falls back to a
 * hex string if no symbol contains the PC. Glibc's `@@GLIBC_X.Y` /
 * `@GLIBC_X.Y` version-tag suffixes are stripped so aggregation lumps
 * the same function across glibc builds and the skip-frames set can
 * match without encoding every version.
 */
function resolvePc(pc) {
    var lo = 0, hi = profilerSymbols.length - 1;
    while (lo <= hi) {
        var mid = (lo + hi) >>> 1;
        var e = profilerSymbols[mid];
        if (pc < e[0]) hi = mid - 1;
        else if (pc >= e[1]) lo = mid + 1;
        else {
            var nm = e[2];
            var at = nm.indexOf("@");
            return at > 0 ? nm.slice(0, at) : nm;
        }
    }
    return "0x" + pc.toString(16);
}

function handleProfile(buf, off) {
    if (buf.byteLength - off < 1) return;
    var dv = new DataView(buf, off);
    var subType = dv.getUint8(0);

    if (subType === PROFILE_SUB_SYMBOLS) {
        /* JSON array of [pc_start, pc_end, name].
         *
         * Merge into the existing table rather than replacing it: POSIX
         * bridge delivers one large upfront blob (merge is a no-op on an
         * empty table), while WASM emits incremental batches as its
         * on-target interner observes new frame names. Either way the
         * net effect is an append-only sorted set keyed by pc_start. */
        var bytes = new Uint8Array(buf, off + 1, dv.byteLength - 1);
        try {
            var json = new TextDecoder().decode(bytes);
            var arr = JSON.parse(json);
            var seen = Object.create(null);
            for (var k = 0; k < profilerSymbols.length; k++)
                seen[profilerSymbols[k][0]] = 1;
            var added = 0;
            for (var i = 0; i < arr.length; i++) {
                var e = arr[i];
                if (seen[e[0]]) continue;
                seen[e[0]] = 1;
                profilerSymbols.push(e);
                added++;
            }
            if (added > 0)
                profilerSymbols.sort(function (a, b) { return a[0] - b[0]; });
            profilerSymbolCount = profilerSymbols.length;
        } catch (err) {
            console.error("profiler: symbol parse failed", err);
        }
        profilerDirty = true;
        ensureWindow("profiler");
        return;
    }

    if (subType === PROFILE_SUB_CAPS) {
        /* Payload: uint32 max_hz, uint32 current_hz. */
        if (dv.byteLength < 1 + 8) return;
        profilerMaxHz     = dv.getUint32(1, true);
        profilerCurrentHz = dv.getUint32(5, true);
        profilerUpdateRateOptions();
        return;
    }

    if (subType !== PROFILE_SUB_SAMPLES) return;
    if (profilerPaused) return;
    /* envelope layout after subType byte:
     *   version(1), word_size(1), count(2), dropped(4) */
    if (dv.byteLength < 1 + 8) return;
    var wordSize = dv.getUint8(2);
    var count    = dv.getUint16(3, true);
    var dropped  = dv.getUint32(5, true);
    profilerDropped += dropped;

    if (wordSize !== 8) {
        /* 32-bit support would add a branch below; POSIX x86_64 is 8. */
        console.warn("profiler: unsupported word size", wordSize);
        return;
    }

    var p = 9;
    for (var j = 0; j < count; j++) {
        if (p + 16 > dv.byteLength) break;
        var tsLo = dv.getUint32(p, true);
        var tsHi = dv.getUint32(p + 4, true);
        var ts   = tsHi * 0x100000000 + tsLo;
        var tid  = dv.getUint32(p + 8, true);
        var depth = dv.getUint8(p + 12);
        var state = dv.getUint8(p + 13);
        p += 16;
        if (p + depth * 8 > dv.byteLength) break;
        var pcs = new Array(depth);
        for (var k = 0; k < depth; k++) {
            var lo = dv.getUint32(p, true);
            var hi = dv.getUint32(p + 4, true);
            pcs[k] = hi * 0x100000000 + lo;
            p += 8;
        }
        profilerSamples.push({ ts: ts, tid: tid, state: state, pcs: pcs });
        if (ts > profilerLastTs) profilerLastTs = ts;
    }

    profilerEvict();
    profilerDirty = true;
    ensureWindow("profiler");
    ensureProfilerRefresh();
}

/* Populate the rate dropdown using a standard set of rates filtered to
 * values the firmware can actually deliver (<= compile-time max). Called
 * on every CAPS event so the ceiling can adapt if it ever moves. */
function profilerUpdateRateOptions() {
    var sel = document.getElementById("profiler-rate");
    if (!sel) return;
    if (!profilerMaxHz) return;

    var candidates = [50, 100, 250, 500, 1000, 2000];
    var options = [];
    for (var i = 0; i < candidates.length; i++) {
        if (candidates[i] <= profilerMaxHz) options.push(candidates[i]);
    }
    /* Always include the max itself so the user has an "uncapped" choice
     * even if it's not a round number from the list. */
    if (options[options.length - 1] !== profilerMaxHz)
        options.push(profilerMaxHz);

    /* Rebuild only if the set changed — avoids wiping the user's
     * selection every 200 ms once the CAPS event is stable. */
    var signature = options.join(",");
    if (sel.dataset.signature === signature) {
        sel.value = String(profilerCurrentHz || profilerMaxHz);
        return;
    }
    sel.dataset.signature = signature;
    sel.innerHTML = "";
    for (var j = 0; j < options.length; j++) {
        var opt = document.createElement("option");
        opt.value = String(options[j]);
        opt.textContent = options[j] + " Hz";
        sel.appendChild(opt);
    }
    sel.value = String(profilerCurrentHz || profilerMaxHz);
}

function profilerEvict() {
    var cutoff = profilerLastTs - profilerWindowUs;
    while (profilerSamples.length && profilerSamples[0].ts < cutoff) {
        profilerSamples.shift();
    }
    var cap = Math.max(PROFILER_MIN_CAP,
        Math.round((profilerWindowUs / 1e6) * PROFILER_SAMPLE_BUDGET_HZ));
    if (profilerSamples.length > cap) {
        profilerSamples.splice(0, profilerSamples.length - cap);
    }
}

function ensureProfilerRefresh() {
    if (profilerRefreshTimer) return;
    profilerRefreshTimer = setInterval(function () {
        if (!profilerDirty) return;
        profilerDirty = false;
        renderProfiler();
    }, PROFILER_REFRESH_MS);
}

function renderProfiler() {
    profilerUpdateThreadOptions();
    var info = document.getElementById("profiler-info");
    if (info) {
        var winS = Math.round(profilerWindowUs / 1e6);
        var kept = 0;
        for (var si = 0; si < profilerSamples.length; si++) {
            var _smp = profilerSamples[si];
            if (!profilerSampleIncluded(_smp)) continue;
            /* Mirror the render-path drop: samples whose every frame is
             * excluded by API-only shouldn't be counted either. */
            if (profilerLeafOffset(_smp.pcs) >= _smp.pcs.length) continue;
            kept++;
        }
        var mode = profilerOnCpuOnly ? "on-CPU" : "wall-clock";
        var threadTag = "";
        if (profilerThreadFilter) {
            var th = traceThreads[profilerThreadFilter];
            threadTag = " — thread: "
                + (th ? th.name
                      : ("0x" + profilerThreadFilter.toString(16)));
        }
        var label = kept + " samples (" + mode + ") / " + winS + " s, "
            + profilerSymbolCount + " symbols"
            + (profilerDropped ? ", " + profilerDropped + " dropped" : "")
            + threadTag;
        if (profilerPaused) label = "PAUSED — " + label;
        info.textContent = label;
        info.classList.toggle("paused", profilerPaused);
    }
    if (profilerActiveTab === "flat") renderProfilerFlat();
    else renderProfilerFlame();
}

/*
 * Rebuild the Thread dropdown from traceThreads, sorted for stable
 * ordering. Called on each render so tids picked up from DESCRIPTORS
 * after profiler samples started arriving still land in the list. Uses
 * a signature gate so the dropdown isn't wiped (and the user's current
 * selection isn't lost) on the majority of refreshes where nothing
 * about the set changed.
 */
function profilerUpdateThreadOptions() {
    var sel = document.getElementById("profiler-thread");
    if (!sel) return;
    var tids = Object.keys(traceThreads).map(function (s) { return +s; });
    tids.sort(function (a, b) {
        var na = traceThreads[a].name || "";
        var nb = traceThreads[b].name || "";
        return na.localeCompare(nb);
    });
    var signature = tids.map(function (t) {
        return t + ":" + (traceThreads[t].name || "");
    }).join("|");
    if (sel.dataset.signature === signature) return;
    sel.dataset.signature = signature;
    sel.innerHTML = "";
    var optAll = document.createElement("option");
    optAll.value = "0";
    optAll.textContent = "All threads";
    sel.appendChild(optAll);
    for (var i = 0; i < tids.length; i++) {
        var t = tids[i];
        var opt = document.createElement("option");
        opt.value = String(t);
        opt.textContent = traceThreads[t].name || ("0x" + t.toString(16));
        sel.appendChild(opt);
    }
    sel.value = String(profilerThreadFilter || 0);
}

function renderProfilerFlat() {
    var tbody = document.getElementById("profiler-flat-tbody");
    if (!tbody) return;

    /* self = leaf PC (after skipping signal-handler frames); total = any
     * frame. */
    var selfCount = Object.create(null);
    var totalCount = Object.create(null);
    var countedSamples = 0;
    for (var i = 0; i < profilerSamples.length; i++) {
        var smp = profilerSamples[i];
        if (!profilerSampleIncluded(smp)) continue;
        var pcs = smp.pcs;
        if (!pcs.length) continue;
        var leafOff = profilerLeafOffset(pcs);
        if (leafOff >= pcs.length) continue; /* all frames filtered */
        countedSamples++;
        var leafName = resolvePc(pcs[leafOff]);
        selfCount[leafName] = (selfCount[leafName] || 0) + 1;
        var seenInSample = Object.create(null);
        for (var k = leafOff; k < pcs.length; k++) {
            var n = resolvePc(pcs[k]);
            if (profilerApiOnly && profilerFrameIsExcluded(n)) continue;
            if (seenInSample[n]) continue;
            seenInSample[n] = 1;
            totalCount[n] = (totalCount[n] || 0) + 1;
        }
    }
    var totalSamples = countedSamples;

    var rows = [];
    for (var name in selfCount) {
        rows.push({
            name: name,
            self: selfCount[name],
            total: totalCount[name] || 0,
        });
    }
    rows.sort(function (a, b) { return b.self - a.self; });
    if (rows.length > PROFILER_FLAT_TOP) rows.length = PROFILER_FLAT_TOP;

    var html = "";
    var denom = totalSamples || 1;
    for (var r = 0; r < rows.length; r++) {
        var row = rows[r];
        var selfPct = (row.self * 100 / denom);
        var totalPct = (row.total * 100 / denom);
        html += "<tr>"
            + '<td class="pf-num pf-bar" style="--bar:' + selfPct.toFixed(1) + '%">'
            + row.self + "</td>"
            + '<td class="pf-num">' + selfPct.toFixed(1) + "%</td>"
            + '<td class="pf-num">' + totalPct.toFixed(1) + "%</td>"
            + '<td class="pf-func">' + escapeHtml(row.name) + "</td>"
            + "</tr>";
    }
    if (!rows.length) html = '<tr><td colspan="4" style="padding:12px;'
        + 'color:#666;text-align:center">Waiting for samples…</td></tr>';
    tbody.innerHTML = html;
}

function escapeHtml(s) {
    return s.replace(/[&<>"']/g, function (c) {
        return { "&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;" }[c];
    });
}

/**
 * Build a flame-graph trie and render as inverted stacks. Each sample
 * contributes one path (outermost → leaf) weighted by 1. Root frame is
 * at the bottom — hotter leaves bubble up. Standard Brendan-Gregg
 * icicle style.
 */
function renderProfilerFlame() {
    var cvs = document.getElementById("profiler-flame-canvas");
    if (!cvs) return;
    var view = cvs.parentElement;
    if (!view) return;

    /* Build trie. Walk pcs outermost→leaf. A sample's pcs[] is recorded
     * in call-stack order, which with glibc backtrace() is leaf-first.
     * Reverse so the root is index 0. Drop the signal-handler frames at
     * the leaf — they dominate the graph and obscure real call paths.
     *
     * Between [root] and the PC frames we insert a synthetic thread-name
     * row. Without it the FreeRTOS/Cortex-M backend (depth-1 samples —
     * the ISR can't unwind r4-r11) collapses to a single row of leaf
     * PCs with no per-thread breakdown. With it, [root] fans out into
     * one box per thread, and each thread's leaves stack below — making
     * attribution visually obvious even when the stack is depth-1. */
    var root = { name: "[root]", count: 0, kids: Object.create(null) };
    for (var i = 0; i < profilerSamples.length; i++) {
        var s = profilerSamples[i];
        if (!profilerSampleIncluded(s)) continue;
        if (!s.pcs.length) continue;
        var leafOff = profilerLeafOffset(s.pcs);
        if (leafOff >= s.pcs.length) continue;
        root.count++;
        var node = root;
        var th = traceThreads[s.tid];
        var threadLabel = (th && th.name)
                        ? "[" + th.name + "]"
                        : "[tid:0x" + s.tid.toString(16) + "]";
        var tnode = node.kids[threadLabel];
        if (!tnode) {
            tnode = { name: threadLabel, count: 0,
                      kids: Object.create(null), isThread: true };
            node.kids[threadLabel] = tnode;
        }
        tnode.count++;
        node = tnode;
        for (var k = s.pcs.length - 1; k >= leafOff; k--) {
            var n = resolvePc(s.pcs[k]);
            if (profilerApiOnly && profilerFrameIsExcluded(n)) continue;
            var next = node.kids[n];
            if (!next) {
                next = { name: n, count: 0, kids: Object.create(null) };
                node.kids[n] = next;
            }
            next.count++;
            node = next;
        }
    }
    profilerLastFlameData = root;

    /* Layout: rows top→bottom, each row one frame depth. Row 0 = root. */
    var dpr = window.devicePixelRatio || 1;
    var cssW = view.clientWidth, cssH = view.clientHeight;
    if (cssW <= 0 || cssH <= 0) return;

    var ctx = cvs.getContext("2d");
    ctx.font = "11px 'Consolas', monospace";

    if (root.count === 0) {
        cvs.style.width = "100%";
        cvs.width  = cssW * dpr;
        cvs.height = cssH * dpr;
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, cssW, cssH);
        ctx.fillStyle = "#666";
        ctx.font = "12px 'Consolas', monospace";
        ctx.fillText("Waiting for samples…", 10, 20);
        profilerFlameLayout = [];
        return;
    }

    /* Horizontal zoom is user-controlled via wheel events on the canvas
     * (see profilerBindPanel). At zoom = 1 the root fills cssW; zoom > 1
     * grows the canvas past the viewport so .profiler-view's
     * overflow:auto yields a horizontal scrollbar. Anything that can't
     * fit its label at the current zoom falls back to ellipsis. */
    var totalW = Math.ceil(cssW * profilerFlameZoom);

    /* Size bitmap to total width; explicit style.width overrides the
     * stylesheet's width:100% so the parent's overflow:auto can scroll. */
    cvs.width  = totalW * dpr;
    cvs.height = cssH  * dpr;
    cvs.style.width = totalW + "px";
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, totalW, cssH);
    ctx.font = "11px 'Consolas', monospace";
    ctx.textBaseline = "middle";

    var rowH = 16;
    var layout = []; /* [{x, y, w, h, name, count, total}] */

    function drawNode(node, x, w, depth, parentTotal) {
        if (w < 0.5) return;
        var y = depth * rowH;
        if (y > cssH) return;
        layout.push({
            x: x, y: y, w: w, h: rowH - 1,
            name: node.name, count: node.count, total: parentTotal
        });
        var children = [];
        for (var n in node.kids) children.push(node.kids[n]);
        children.sort(function (a, b) { return b.count - a.count; });
        var childX = x;
        for (var c = 0; c < children.length; c++) {
            var cn = children[c];
            var cw = (cn.count / node.count) * w;
            drawNode(cn, childX, cw, depth + 1, node.count);
            childX += cw;
        }
    }
    drawNode(root, 0, totalW, 0, root.count);

    /* Paint boxes. Hash each name to a stable hue so the same function
     * keeps the same colour across refreshes. Labels use ellipsis only
     * as a safety net when the computed scale hit MAX_ZOOM and a block
     * still can't show its full name. */
    var ellW = ctx.measureText("…").width;
    for (var i2 = 0; i2 < layout.length; i2++) {
        var b = layout[i2];
        var h = strHash(b.name) % 60;
        ctx.fillStyle = "hsl(" + (10 + h * 4) + ", 60%, 48%)";
        ctx.fillRect(b.x, b.y, b.w, b.h);
        var maxTextW = b.w - 8;
        if (maxTextW <= ellW) continue;
        var label = fitTextToWidth(ctx, b.name, maxTextW, ellW);
        if (!label) continue;
        ctx.fillStyle = "#0d1117";
        ctx.fillText(label, b.x + 4, b.y + b.h / 2);
    }
    profilerFlameLayout = layout;
}

/*
 * Truncate @text to fit @maxW pixels at the current ctx.font; appends a
 * single "…" when the whole string doesn't fit. @ellW is the pre-measured
 * width of "…" — passed in so the caller doesn't re-measure per box.
 * Returns the empty string if even the ellipsis alone won't fit.
 */
function fitTextToWidth(ctx, text, maxW, ellW) {
    if (maxW <= 0) return "";
    if (ctx.measureText(text).width <= maxW) return text;
    if (ellW >= maxW) return "";
    var lo = 0, hi = text.length;
    while (lo < hi) {
        var mid = (lo + hi + 1) >> 1;
        if (ctx.measureText(text.slice(0, mid)).width + ellW <= maxW) lo = mid;
        else hi = mid - 1;
    }
    if (lo === 0) return "";
    return text.slice(0, lo) + "…";
}

function strHash(s) {
    var h = 0;
    for (var i = 0; i < s.length; i++) {
        h = ((h << 5) - h + s.charCodeAt(i)) | 0;
    }
    return h < 0 ? -h : h;
}

/* Tooltip + tab wiring — installed on first profiler panel visibility. */
function profilerBindPanel() {
    if (profilerBindPanel._bound) return;
    profilerBindPanel._bound = true;

    var tabs = document.querySelectorAll(".prof-tab");
    for (var i = 0; i < tabs.length; i++) {
        tabs[i].addEventListener("click", function () {
            var name = this.getAttribute("data-tab");
            var all = document.querySelectorAll(".prof-tab");
            for (var j = 0; j < all.length; j++)
                all[j].classList.toggle("active",
                    all[j].getAttribute("data-tab") === name);
            document.getElementById("profiler-flat-view").style.display =
                (name === "flat") ? "" : "none";
            document.getElementById("profiler-flame-view").style.display =
                (name === "flame") ? "" : "none";
            profilerActiveTab = name;
            renderProfiler();
        });
    }

    var winInput = document.getElementById("profiler-window");
    if (winInput) {
        var applyWindow = function () {
            var v = parseInt(winInput.value, 10);
            if (!isFinite(v)) v = PROFILER_WINDOW_MIN_S;
            if (v < PROFILER_WINDOW_MIN_S) v = PROFILER_WINDOW_MIN_S;
            if (v > PROFILER_WINDOW_MAX_S) v = PROFILER_WINDOW_MAX_S;
            winInput.value = v;
            profilerWindowUs = v * 1000 * 1000;
            profilerEvict();
            renderProfiler();
        };
        winInput.addEventListener("change", applyWindow);
        winInput.addEventListener("blur", applyWindow);
    }

    var pauseBtn = document.getElementById("profiler-pause");
    if (pauseBtn) {
        pauseBtn.addEventListener("click", function () {
            profilerPaused = !profilerPaused;
            pauseBtn.textContent = profilerPaused ? "Resume" : "Pause";
            pauseBtn.classList.toggle("paused", profilerPaused);
            renderProfiler();
        });
    }

    var onCpuBtn = document.getElementById("profiler-oncpu");
    if (onCpuBtn) {
        onCpuBtn.addEventListener("click", function () {
            profilerOnCpuOnly = !profilerOnCpuOnly;
            onCpuBtn.classList.toggle("active", profilerOnCpuOnly);
            renderProfiler();
        });
    }

    var hidePumpBtn = document.getElementById("profiler-hidepump");
    if (hidePumpBtn) {
        hidePumpBtn.classList.toggle("active", profilerHidePump);
        hidePumpBtn.addEventListener("click", function () {
            profilerHidePump = !profilerHidePump;
            hidePumpBtn.classList.toggle("active", profilerHidePump);
            renderProfiler();
        });
    }

    var hideIdleBtn = document.getElementById("profiler-hideidle");
    if (hideIdleBtn) {
        hideIdleBtn.classList.toggle("active", profilerHideIdle);
        hideIdleBtn.addEventListener("click", function () {
            profilerHideIdle = !profilerHideIdle;
            hideIdleBtn.classList.toggle("active", profilerHideIdle);
            renderProfiler();
        });
    }

    var apiOnlyBtn = document.getElementById("profiler-apionly");
    if (apiOnlyBtn) {
        apiOnlyBtn.classList.toggle("active", profilerApiOnly);
        apiOnlyBtn.addEventListener("click", function () {
            profilerApiOnly = !profilerApiOnly;
            apiOnlyBtn.classList.toggle("active", profilerApiOnly);
            renderProfiler();
        });
    }

    var threadSel = document.getElementById("profiler-thread");
    if (threadSel) {
        threadSel.addEventListener("change", function () {
            profilerThreadFilter = parseInt(threadSel.value, 10) || 0;
            renderProfiler();
        });
    }

    var rateSel = document.getElementById("profiler-rate");
    if (rateSel) {
        rateSel.addEventListener("change", function () {
            var hz = parseInt(rateSel.value, 10);
            if (!isFinite(hz) || hz <= 0) return;
            /* plugin_id is resolved by broadcast on the firmware side
             * (profiler.handle_cmd filters on cmd_type). Using a non-
             * audio, non-console sentinel keeps the bridge's audio
             * short-circuit out of the picture. */
            var payload = new Uint8Array(4);
            new DataView(payload.buffer).setUint32(0, hz, true);
            sendCmd(0xFFFFFFFE, PROFILER_CMD_SET_RATE, payload);
            /* Optimistic local update; firmware echoes via CAPS event. */
            profilerCurrentHz = hz;
        });
    }

    var cvs = document.getElementById("profiler-flame-canvas");
    var tip = document.getElementById("profiler-flame-tip");
    if (cvs && tip) {
        cvs.addEventListener("mousemove", function (e) {
            if (!profilerFlameLayout) { tip.style.display = "none"; return; }
            var r = cvs.getBoundingClientRect();
            var x = (e.clientX - r.left) * (cvs.width / r.width)
                  / (window.devicePixelRatio || 1);
            var y = (e.clientY - r.top) * (cvs.height / r.height)
                  / (window.devicePixelRatio || 1);
            var hit = null;
            for (var k = 0; k < profilerFlameLayout.length; k++) {
                var b = profilerFlameLayout[k];
                if (x >= b.x && x < b.x + b.w
                        && y >= b.y && y < b.y + b.h) {
                    hit = b; break;
                }
            }
            if (!hit) { tip.style.display = "none"; return; }
            var pct = profilerLastFlameData && profilerLastFlameData.count
                ? (hit.count * 100 / profilerLastFlameData.count).toFixed(1)
                : "0.0";
            tip.textContent = hit.name + " — " + hit.count
                + " samples (" + pct + "%)";
            tip.style.display = "block";
            tip.style.left = (e.clientX - r.left + 12) + "px";
            tip.style.top  = (e.clientY - r.top + 12) + "px";
        });
        cvs.addEventListener("mouseleave", function () {
            tip.style.display = "none";
        });

        /* Mouse-wheel zoom, anchored at the cursor so the block under
         * the pointer stays under the pointer across zoom steps. */
        var flameView = cvs.parentElement;
        cvs.addEventListener("wheel", function (e) {
            if (!e.deltaY || profilerActiveTab !== "flame") return;
            e.preventDefault();

            var rect = flameView.getBoundingClientRect();
            var mX = e.clientX - rect.left;
            if (mX < 0 || mX > rect.width) return;

            var oldW = cvs.clientWidth || rect.width;
            var logicalX = flameView.scrollLeft + mX;

            var factor = e.deltaY < 0 ? 1.15 : 1 / 1.15;
            var next = profilerFlameZoom * factor;
            if (next < 1) next = 1;
            if (next > PROFILER_FLAME_MAX_ZOOM) next = PROFILER_FLAME_MAX_ZOOM;
            if (next === profilerFlameZoom) return;
            profilerFlameZoom = next;

            renderProfiler();

            var newW = cvs.clientWidth || oldW;
            flameView.scrollLeft = logicalX * (newW / oldW) - mX;
        }, { passive: false });
    }

    window.addEventListener("resize", function () {
        if (profilerActiveTab === "flame") renderProfiler();
    });

    /* WinBox drag-resize doesn't emit a viewport resize event, so the flame
     * canvas bitmap stays pinned at its first-render width and the right
     * edge gets cropped once the panel grows. A ResizeObserver on the
     * flame view re-renders whenever its content box changes size. */
    var flameView = document.getElementById("profiler-flame-view");
    if (flameView && typeof ResizeObserver !== "undefined") {
        var ro = new ResizeObserver(function () {
            if (profilerActiveTab === "flame") renderProfiler();
        });
        ro.observe(flameView);
    }
}

function sendDebugCmd(cmdType, payload) {
    if (!ws || ws.readyState !== 1) return;
    var hdr = new ArrayBuffer(5 + (payload ? payload.byteLength : 0));
    var view = new DataView(hdr);
    view.setUint32(0, FRAME_DEBUG_CMD, true);
    view.setUint8(4, cmdType);
    if (payload) {
        new Uint8Array(hdr, 5).set(new Uint8Array(payload));
    }
    ws.send(hdr);
}

function handleDebugResp(buf, off) {
    if (buf.byteLength - off < 1) return;
    var respType = new DataView(buf, off).getUint8(0);
    var jsonBytes = new Uint8Array(buf, off + 1);
    var json;
    try {
        json = JSON.parse(new TextDecoder().decode(jsonBytes));
    } catch (e) { return; }

    var data = json.data || {};

    switch (respType) {
        case DBG_RESP_STATE:
            dbgHandleState(data);
            break;
        case DBG_RESP_BACKTRACE:
            dbgHandleBacktrace(data);
            break;
        case DBG_RESP_REGISTERS:
            dbgHandleRegisters(data);
            break;
        case DBG_RESP_DISASSEMBLY:
            dbgHandleDisassembly(data);
            break;
        case DBG_RESP_BREAKPOINT:
            dbgHandleBreakpoint(data);
            break;
        case DBG_RESP_SOURCE:
            dbgHandleSource(data);
            break;
    }
}

function dbgHandleState(data) {
    ensureWindow("debug");
    dbgState = data.state || "disconnected";
    dbgCurrentFile = data.file || null;
    dbgCurrentLine = data.line || 0;
    dbgCurrentAddr = data.addr || null;

    var statusEl = document.getElementById("dbg-status");
    if (statusEl) {
        if (dbgState === "running") {
            statusEl.textContent = "Running";
            statusEl.className = "dbg-status dbg-running";
        } else if (dbgState === "stopped") {
            var reason = data.reason || "";
            var loc = dbgCurrentFile
                ? _shortPath(dbgCurrentFile) + ":" + dbgCurrentLine
                : "";
            statusEl.textContent = "Stopped" + (reason ? " (" + reason + ")" : "") + (loc ? " at " + loc : "");
            statusEl.className = "dbg-status dbg-stopped";
        } else {
            statusEl.textContent = "Disconnected";
            statusEl.className = "dbg-status";
        }
    }

    /* Dim source view when running */
    var srcView = document.getElementById("dbg-source-view");
    if (srcView) srcView.classList.toggle("dbg-dimmed", dbgState === "running");
}

function dbgHandleBacktrace(data) {
    var stack = data.stack || [];
    if (!Array.isArray(stack)) stack = [];
    var el = document.getElementById("dbg-callstack");
    if (!el) return;
    if (stack.length === 0) {
        el.innerHTML = "<div class=\"dbg-empty\">No frames</div>";
        return;
    }
    var html = "";
    for (var i = 0; i < stack.length; i++) {
        var f = stack[i];
        var func = f.func || f["function"] || "??";
        var file = f.file || f.fullname || "";
        var line = f.line || "";
        var addr = f.addr || "";
        var loc = file ? _shortPath(file) + ":" + line : addr;
        var cls = i === 0 ? "dbg-frame active" : "dbg-frame";
        html += "<div class=\"" + cls + "\" data-file=\"" + _esc(f.fullname || file) + "\" data-line=\"" + line + "\">"
              + "<span class=\"dbg-frame-idx\">#" + (f.level || i) + "</span> "
              + "<span class=\"dbg-frame-func\">" + _esc(func) + "</span> "
              + "<span class=\"dbg-frame-loc\">" + _esc(loc) + "</span>"
              + "</div>";
    }
    el.innerHTML = html;

    /* Click on frame to navigate source */
    el.querySelectorAll(".dbg-frame").forEach(function(frame) {
        frame.addEventListener("click", function() {
            var file = this.getAttribute("data-file");
            var line = parseInt(this.getAttribute("data-line"), 10);
            if (file && line) requestSource(file, line, 20);
        });
    });
}

function dbgHandleRegisters(data) {
    var regs = data["register-values"] || [];
    if (!Array.isArray(regs)) regs = [];
    var el = document.getElementById("dbg-registers");
    if (!el) return;

    /* Filter out registers with empty names (unused GDB slots) */
    regs = regs.filter(function(r) { return r.name; });
    if (regs.length === 0) {
        el.innerHTML = "<div class=\"dbg-empty\">No registers</div>";
        return;
    }

    /* Group: core (r0-r12, sp, lr, pc), status (xPSR etc), FPU (s/d regs) */
    var core = [], status = [], fpu = [], other = [];
    var coreNames = {"r0":1,"r1":1,"r2":1,"r3":1,"r4":1,"r5":1,"r6":1,
                     "r7":1,"r8":1,"r9":1,"r10":1,"r11":1,"r12":1,
                     "sp":1,"lr":1,"pc":1};
    var statusNames = {"xPSR":1,"xpsr":1,"PRIMASK":1,"primask":1,
                       "BASEPRI":1,"basepri":1,"FAULTMASK":1,"faultmask":1,
                       "CONTROL":1,"control":1,"msp":1,"psp":1};

    for (var i = 0; i < regs.length; i++) {
        var r = regs[i];
        var name = r.name;
        if (coreNames[name]) core.push(r);
        else if (statusNames[name]) status.push(r);
        else if (name.match(/^[sd]\d+$/) || name === "fpscr" || name === "FPSCR")
            fpu.push(r);
        else other.push(r);
    }

    var html = "";
    if (core.length) html += _renderRegGroup("Core", core);
    if (status.length) html += _renderRegGroup("Status", status);
    if (fpu.length) html += _renderRegGroup("FPU", fpu);
    if (other.length) html += _renderRegGroup("Other", other);
    el.innerHTML = html;
}

function _renderRegGroup(title, regs) {
    var html = "<div class=\"dbg-reg-group-title\">" + title + "</div>"
             + "<div class=\"dbg-reg-grid\">";
    for (var i = 0; i < regs.length; i++) {
        var r = regs[i];
        html += "<span class=\"dbg-reg-name\">" + _esc(r.name) + "</span>"
              + "<span class=\"dbg-reg-val\">" + _esc(r.value || "0x0") + "</span>";
    }
    return html + "</div>";
}

/* ── File explorer and full-file loading ──────────────────────────── */

var projectFiles = [];      /* [{path, short, cat}] from bridge */
var fileCache = {};          /* path → full file content string */
var monacoOpenFile = null;   /* path of currently open file in Monaco */

function handleFileList(buf, off) {
    var json;
    try {
        json = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, off)));
    } catch (e) { return; }
    if (!Array.isArray(json)) return;
    projectFiles = json;
    /* Show the debug window for the file explorer even without GDB.
     * WASM has no in-dashboard debugger — users go to DevTools — so
     * skip the pane there. */
    if (projectFiles.length > 0 && !isWasmMode) ensureWindow("debug");
    _renderFileExplorer();
}

function handleFileResp(buf, off) {
    if (buf.byteLength - off < 6) return;
    var view = new DataView(buf, off);
    var pathLen = view.getUint16(0, true);
    if (buf.byteLength - off < 2 + pathLen) return;
    var pathBytes = new Uint8Array(buf, off + 2, pathLen);
    var path = new TextDecoder().decode(pathBytes);
    var content = new TextDecoder().decode(new Uint8Array(buf, off + 2 + pathLen));
    fileCache[path] = content;
    _openFileInMonaco(path, content);
}

function requestFullFile(path) {
    if (fileCache[path]) {
        _openFileInMonaco(path, fileCache[path]);
        return;
    }
    if (!ws || ws.readyState !== 1) return;
    var pathBytes = new TextEncoder().encode(path);
    var buf = new ArrayBuffer(4 + pathBytes.length);
    new DataView(buf).setUint32(0, FRAME_FILE_REQ, true);
    new Uint8Array(buf, 4).set(pathBytes);
    ws.send(buf);
}

function _openFileInMonaco(path, content) {
    var editor = _ensureMonacoEditor();
    if (!editor) return;
    /* Switch to source tab */
    dbgActiveTab = "source";
    document.querySelectorAll(".dbg-tab").forEach(function(t) {
        t.classList.toggle("active", t.getAttribute("data-tab") === "source");
    });
    var container = document.getElementById("dbg-source-view");
    if (container) container.innerHTML = "";
    var dom = editor.getDomNode();
    if (dom) {
        dom.style.display = "";
        if (container) container.appendChild(dom);
    }

    monacoOpenFile = path;
    dbgCurrentFile = path;
    editor.setValue(content);
    editor._oveStartLine = 1;
    editor.updateOptions({ lineNumbers: "on" });

    /* Update file label */
    var fileLabel = document.getElementById("dbg-source-file");
    if (fileLabel) fileLabel.textContent = _shortPath(path);

    /* Re-apply decorations (breakpoints + current line if in this file) */
    _monacoSetDecorations(
        dbgCurrentFile === path ? dbgCurrentLine : 0, 1, path);
    editor.layout();

    /* Highlight active file in explorer */
    _highlightActiveFile(path);
}

function _renderFileExplorer() {
    var el = document.getElementById("dbg-files");
    if (!el) return;
    if (projectFiles.length === 0) {
        el.innerHTML = "<div class=\"dbg-empty\">No files</div>";
        return;
    }
    var html = "";
    var lastCat = "";
    for (var i = 0; i < projectFiles.length; i++) {
        var f = projectFiles[i];
        if (f.cat !== lastCat) {
            lastCat = f.cat;
            html += "<div class=\"dbg-file-cat\">" + _esc(f.cat) + "</div>";
        }
        var basename = f.short.split("/").pop();
        var dir = f.short.substring(0, f.short.length - basename.length);
        html += "<div class=\"dbg-file-entry\" data-path=\"" + _esc(f.path) + "\" title=\"" + _esc(f.short) + "\">"
              + "<span class=\"dbg-file-dir\">" + _esc(dir) + "</span>"
              + "<span class=\"dbg-file-name\">" + _esc(basename) + "</span>"
              + "</div>";
    }
    el.innerHTML = html;
    el.querySelectorAll(".dbg-file-entry").forEach(function(entry) {
        entry.addEventListener("click", function() {
            requestFullFile(this.getAttribute("data-path"));
        });
    });
}

function _highlightActiveFile(path) {
    var el = document.getElementById("dbg-files");
    if (!el) return;
    el.querySelectorAll(".dbg-file-entry").forEach(function(entry) {
        entry.classList.toggle("dbg-file-active",
            entry.getAttribute("data-path") === path);
    });
}

function dbgHandleDisassembly(data) {
    var asmInsns = data["asm_insns"] || [];
    if (!Array.isArray(asmInsns)) asmInsns = [];
    /* Cache for tab switching */
    dbgLastDisasmData = asmInsns;
    /* Only render if disasm/mixed tab is active */
    if (dbgActiveTab === "disasm" || dbgActiveTab === "mixed")
        _renderDisassembly(asmInsns);
}

function _renderDisassembly(asmInsns) {
    var el = document.getElementById("dbg-source-view");
    if (!el || !asmInsns || asmInsns.length === 0) {
        if (el) el.innerHTML = "<div class=\"dbg-empty\">No disassembly</div>";
        return;
    }
    var mixed = (dbgActiveTab === "mixed");
    var html = "";
    for (var i = 0; i < asmInsns.length; i++) {
        var item = asmInsns[i];
        if (item.line_asm_insn) {
            var insns = item.line_asm_insn;
            if (!Array.isArray(insns)) insns = [];
            /* Skip entries with no instructions (empty source lines) */
            if (insns.length === 0) continue;
            /* Mixed mode: show source line header */
            if (mixed) {
                var srcLine = item.line || "";
                var srcFile = item.file || "";
                if (srcFile || srcLine)
                    html += "<div class=\"dbg-asm-src\">"
                          + _esc(_shortPath(srcFile)) + ":" + srcLine
                          + "</div>";
            }
            for (var j = 0; j < insns.length; j++) {
                html += _fmtAsmLine(insns[j]);
            }
        } else {
            html += _fmtAsmLine(item);
        }
    }
    el.innerHTML = html || "<div class=\"dbg-empty\">No disassembly</div>";
}

function _fmtAsmLine(ins) {
    var addr = ins.address || ins.addr || "";
    var func = ins["func-name"] || "";
    var inst = ins.inst || "";
    var offset = ins.offset || "";
    var prefix = func ? func + "+" + offset : "";
    return "<div class=\"dbg-asm-line\">"
         + "<span class=\"dbg-asm-addr\">" + _esc(addr) + "</span>"
         + (prefix ? "<span class=\"dbg-asm-func\">" + _esc(prefix) + "</span>" : "")
         + "<span class=\"dbg-asm-inst\">" + _esc(inst) + "</span>"
         + "</div>";
}

function dbgHandleBreakpoint(data) {
    var bkpt = data.bkpt || data;
    if (bkpt.number) {
        /* Add/update breakpoint in our list */
        var found = false;
        for (var i = 0; i < dbgBreakpoints.length; i++) {
            if (dbgBreakpoints[i].number === bkpt.number) {
                dbgBreakpoints[i] = bkpt;
                found = true;
                break;
            }
        }
        if (!found) dbgBreakpoints.push(bkpt);
    }
    _renderBreakpoints();
}

function _renderBreakpoints() {
    var el = document.getElementById("dbg-breakpoints");
    if (!el) return;
    if (dbgBreakpoints.length === 0) {
        el.innerHTML = "No breakpoints";
        return;
    }
    var html = "";
    for (var i = 0; i < dbgBreakpoints.length; i++) {
        var bp = dbgBreakpoints[i];
        var file = bp.fullname || bp.file || "";
        var line = bp.line || "";
        html += "<div class=\"dbg-bp\">"
              + "<span class=\"dbg-bp-num\">#" + bp.number + "</span> "
              + "<span class=\"dbg-bp-loc\">" + _esc(_shortPath(file)) + ":" + line + "</span>"
              + "<button class=\"dbg-bp-del\" data-id=\"" + bp.number + "\">x</button>"
              + "</div>";
    }
    el.innerHTML = html;
    el.querySelectorAll(".dbg-bp-del").forEach(function(btn) {
        btn.addEventListener("click", function() {
            var id = parseInt(this.getAttribute("data-id"), 10);
            var buf = new ArrayBuffer(4);
            new DataView(buf).setUint32(0, id, true);
            sendDebugCmd(DBG_CMD_BP_CLEAR, buf);
            dbgBreakpoints = dbgBreakpoints.filter(function(bp) {
                return parseInt(bp.number, 10) !== id;
            });
            _renderBreakpoints();
        });
    });
}

function dbgHandleSource(data) {
    /* Bridge sends a snippet (±20 lines). If we have the full file
     * cached, just update the current line and re-render decorations.
     * Otherwise, request the full file and fall back to the snippet. */
    var file = data.file || "";
    var currentLine = data.current_line || 0;
    dbgCurrentFile = file;
    dbgCurrentLine = currentLine;
    dbgLastSourceData = data;

    if (dbgActiveTab !== "source") return;

    if (file && fileCache[file]) {
        /* Full file already cached — just scroll to the line */
        _openFileInMonaco(file, fileCache[file]);
        _scrollMonacoToLine(currentLine);
        return;
    }

    if (file) {
        /* Request full file — meanwhile show snippet */
        requestFullFile(file);
    }

    /* Show snippet as fallback */
    _renderSourceSnippet(data);
}

function _renderSourceSnippet(data) {
    var file = data.file || "";
    var startLine = data.start_line || 1;
    var currentLine = data.current_line || 0;
    var lines = data.lines || [];

    var fileLabel = document.getElementById("dbg-source-file");
    if (fileLabel) fileLabel.textContent = file ? _shortPath(file) : "No source loaded";

    var editor = _ensureMonacoEditor();
    if (editor) {
        var container = document.getElementById("dbg-source-view");
        if (container) container.innerHTML = "";
        var dom = editor.getDomNode();
        if (dom && container) {
            dom.style.display = "";
            container.appendChild(dom);
        }
        editor.setValue(lines.join("\n"));
        editor._oveStartLine = startLine;
        editor.updateOptions({
            lineNumbers: function (n) { return String(startLine + n - 1); }
        });
        _monacoSetDecorations(currentLine, startLine, file);
        if (currentLine) {
            editor.revealLineInCenter(currentLine - startLine + 1);
        }
        editor.layout();
    }
}

function _scrollMonacoToLine(line) {
    if (!monacoEditor || !line) return;
    _monacoSetDecorations(line, 1, dbgCurrentFile);
    monacoEditor.revealLineInCenter(line);
}

function requestSource(file, line, ctx) {
    var pathBytes = new TextEncoder().encode(file);
    var buf = new ArrayBuffer(2 + pathBytes.length + 8);
    var view = new DataView(buf);
    view.setUint16(0, pathBytes.length, true);
    new Uint8Array(buf, 2, pathBytes.length).set(pathBytes);
    view.setUint32(2 + pathBytes.length, line, true);
    view.setUint32(6 + pathBytes.length, ctx || 20, true);
    sendDebugCmd(DBG_CMD_SOURCE, buf);
}

function requestDisassembly(addr, count) {
    var buf = new ArrayBuffer(8);
    var view = new DataView(buf);
    view.setUint32(0, addr, true);
    view.setUint32(4, count || 32, true);
    sendDebugCmd(DBG_CMD_DISASSEMBLE, buf);
}

function _toggleBreakpoint(file, line) {
    var existing = null;
    for (var i = 0; i < dbgBreakpoints.length; i++) {
        var bp = dbgBreakpoints[i];
        var bpFile = bp.fullname || bp.file || "";
        if (bpFile === file && parseInt(bp.line, 10) === line) {
            existing = bp;
            break;
        }
    }
    if (existing) {
        var buf = new ArrayBuffer(4);
        new DataView(buf).setUint32(0, parseInt(existing.number, 10), true);
        sendDebugCmd(DBG_CMD_BP_CLEAR, buf);
        dbgBreakpoints = dbgBreakpoints.filter(function(bp) { return bp !== existing; });
        _renderBreakpoints();
    } else {
        var pathBytes = new TextEncoder().encode(file);
        var buf2 = new ArrayBuffer(2 + pathBytes.length + 4);
        var view = new DataView(buf2);
        view.setUint16(0, pathBytes.length, true);
        new Uint8Array(buf2, 2, pathBytes.length).set(pathBytes);
        view.setUint32(2 + pathBytes.length, line, true);
        sendDebugCmd(DBG_CMD_BP_SET, buf2);
    }
    /* Re-render source to update gutter marks */
    if (dbgCurrentFile) requestSource(dbgCurrentFile, dbgCurrentLine, 20);
}

function _hasBreakpointAt(file, line) {
    for (var i = 0; i < dbgBreakpoints.length; i++) {
        var bp = dbgBreakpoints[i];
        var bpFile = bp.fullname || bp.file || "";
        if (bpFile === file && parseInt(bp.line, 10) === line) return true;
    }
    return false;
}

function _shortPath(p) {
    if (!p) return "";
    var parts = p.replace(/\\/g, "/").split("/");
    return parts.length > 2 ? parts.slice(-2).join("/") : p;
}

/* ── Debug toolbar button wiring ─────────────────────────────────── */

(function() {
    var btnMap = {
        "dbg-continue":  DBG_CMD_CONTINUE,
        "dbg-pause":     DBG_CMD_PAUSE,
        "dbg-step-over": DBG_CMD_STEP_OVER,
        "dbg-step-into": DBG_CMD_STEP_INTO,
        "dbg-step-out":  DBG_CMD_STEP_OUT,
        "dbg-reset":     DBG_CMD_RESET,
    };
    Object.keys(btnMap).forEach(function(id) {
        var el = document.getElementById(id);
        if (el) el.addEventListener("click", function() {
            sendDebugCmd(btnMap[id]);
        });
    });

    /* Tab switching — renders from cached data */
    document.querySelectorAll(".dbg-tab").forEach(function(tab) {
        tab.addEventListener("click", function() {
            document.querySelectorAll(".dbg-tab").forEach(function(t) { t.classList.remove("active"); });
            this.classList.add("active");
            dbgActiveTab = this.getAttribute("data-tab");
            var container = document.getElementById("dbg-source-view");
            if (dbgActiveTab === "disasm" || dbgActiveTab === "mixed") {
                /* Hide Monaco, show HTML disassembly */
                if (monacoEditor) {
                    var dom = monacoEditor.getDomNode();
                    if (dom) dom.style.display = "none";
                }
                if (dbgLastDisasmData) {
                    _renderDisassembly(dbgLastDisasmData);
                } else if (dbgCurrentAddr) {
                    requestDisassembly(parseInt(dbgCurrentAddr, 16), 32);
                }
            } else {
                /* Show Monaco, clear HTML content */
                if (container) container.innerHTML = "";
                if (monacoEditor) {
                    var dom2 = monacoEditor.getDomNode();
                    if (dom2) {
                        dom2.style.display = "";
                        container.appendChild(dom2);
                        monacoEditor.layout();
                    }
                }
                if (dbgLastSourceData) {
                    _renderSource(dbgLastSourceData);
                } else if (dbgCurrentFile) {
                    requestSource(dbgCurrentFile, dbgCurrentLine, 20);
                }
            }
        });
    });

    /* Keyboard shortcuts */
    document.addEventListener("keydown", function(e) {
        if (e.target.tagName === "INPUT" || e.target.tagName === "TEXTAREA") return;
        if (e.key === "F5") { e.preventDefault(); sendDebugCmd(DBG_CMD_CONTINUE); }
        if (e.key === "F6") { e.preventDefault(); sendDebugCmd(DBG_CMD_PAUSE); }
        if (e.key === "F10") { e.preventDefault(); sendDebugCmd(DBG_CMD_STEP_OVER); }
        if (e.key === "F11" && !e.shiftKey) { e.preventDefault(); sendDebugCmd(DBG_CMD_STEP_INTO); }
        if (e.key === "F11" && e.shiftKey) { e.preventDefault(); sendDebugCmd(DBG_CMD_STEP_OUT); }
    });
})();

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
        /* No debug pane on WASM — code-level debugging is delegated to
         * Chrome/Edge DevTools (F12), which is strictly more capable
         * (named C locals, struct-aware memory).  Keeping the pane
         * would only be a dead-button surface.
         */
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

    /* ── Plugin event dispatch (threads panel, etc.) ────────────────
     * Called from the WASM transport via MAIN_THREAD_ASYNC_EM_ASM.
     * Buffer layout matches `struct ove_sim_event`:
     *   [plugin_id:4][event_type:4][timestamp_ms:4][data_len:4][payload]
     * Map event_type → FRAME_* handlers, mirroring what the Python
     * bridge does on the QEMU/POSIX-host path. */
    var SIM_DEBUG_EVT_THREADS      = 0;   /* OVE_SIM_DEBUG_EVT_THREADS */
    var SIM_TRACE_EVT_STREAM       = 10;  /* OVE_SIM_TRACE_EVT_STREAM */
    var SIM_TRACE_EVT_DESCRIPTORS  = 11;  /* OVE_SIM_TRACE_EVT_DESCRIPTORS */
    var SIM_PROFILER_EVT_SAMPLES   = 20;  /* OVE_SIM_PROFILER_EVT_SAMPLES */
    var SIM_PROFILER_EVT_CAPS      = 21;  /* OVE_SIM_PROFILER_EVT_CAPS */
    var SIM_PROFILER_EVT_SYMBOLS   = 22;  /* OVE_SIM_PROFILER_EVT_SYMBOLS */

    /*
     * Synthesise the sub-type byte the POSIX bridge prepends before
     * FRAME_PROFILE, so handleProfile can be called with the same
     * layout it sees on the WebSocket path. The WASM transport delivers
     * the raw plugin payload without a frame envelope.
     */
    function dispatchProfileEvent(arrayBuf, subType) {
        var payloadLen = arrayBuf.byteLength - 16;
        if (payloadLen < 0) return;
        var combined = new Uint8Array(1 + payloadLen);
        combined[0] = subType;
        if (payloadLen > 0)
            combined.set(new Uint8Array(arrayBuf, 16, payloadLen), 1);
        handleProfile(combined.buffer, 0);
    }

    window.__ove_sim_event = function (arrayBuf) {
        if (!arrayBuf || arrayBuf.byteLength < 16) return;
        var hdr = new DataView(arrayBuf);
        var eventType = hdr.getUint32(4, true);
        var dataLen = hdr.getUint32(12, true);
        if (16 + dataLen > arrayBuf.byteLength) return;

        if (eventType === SIM_DEBUG_EVT_THREADS) {
            handleThreadSnapshot(arrayBuf, 16);
        } else if (eventType === SIM_TRACE_EVT_STREAM
                   || eventType === SIM_TRACE_EVT_DESCRIPTORS) {
            /* Trace plugin payload layout matches what the POSIX bridge
             * delivers as FRAME_TRACE: [sub, ver, count16, dropped, ...].
             * handleTrace starts reading from the offset we hand it. */
            handleTrace(arrayBuf, 16);
        } else if (eventType === SIM_PROFILER_EVT_SAMPLES) {
            dispatchProfileEvent(arrayBuf, PROFILE_SUB_SAMPLES);
        } else if (eventType === SIM_PROFILER_EVT_CAPS) {
            dispatchProfileEvent(arrayBuf, PROFILE_SUB_CAPS);
        } else if (eventType === SIM_PROFILER_EVT_SYMBOLS) {
            dispatchProfileEvent(arrayBuf, PROFILE_SUB_SYMBOLS);
        }
    };
};

/* ══════════════════════════════════════════════════════════════════════
   Init: detect mode and start
   ══════════════════════════════════════════════════════════════════════ */
if (!isWasmMode) {
    /* POSIX mode: connect WebSocket, set up console input. */
    initConsoleInput();
    connect();
}
