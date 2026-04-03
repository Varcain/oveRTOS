/*
 * oveRTOS Simulation Dashboard
 *
 * Connects to the sim WebSocket server and renders:
 * - Display framebuffer on an HTML5 canvas
 * - Audio waveform visualization + WebAudio playback
 * - Plugin events log
 */

"use strict";

/* ── Constants matching ove_sim_ws.h ───────────────────────────────── */
const FRAME_FB    = 0x01;
const FRAME_AUDIO = 0x02;
const FRAME_EVENT = 0x03;
const FRAME_CMD   = 0x04;
const FRAME_STATE = 0x05;

/* ── DOM elements ──────────────────────────────────────────────────── */
const statusEl     = document.getElementById("status");
const canvas       = document.getElementById("display-canvas");
const ctx          = canvas.getContext("2d");
const resolutionEl = document.getElementById("display-resolution");
const fpsEl        = document.getElementById("display-fps");
const audioToggle  = document.getElementById("audio-toggle");
const audioInfoEl  = document.getElementById("audio-info");
const waveCanvas   = document.getElementById("audio-waveform");
const waveCtx      = waveCanvas.getContext("2d");
const eventLog     = document.getElementById("event-log");

/* ── State ─────────────────────────────────────────────────────────── */
let ws = null;
let displayWidth = 0;
let displayHeight = 0;
let frameCount = 0;
let lastFpsTime = performance.now();
let lastFpsCount = 0;

/* Audio */
let audioCtx = null;
let audioEnabled = false;
let audioSampleRate = 44100;
let audioChannels = 1;
let audioBitDepth = 16;

/* ── WebSocket connection ──────────────────────────────────────────── */
function connect() {
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    ws = new WebSocket(`${proto}//${location.host}/ws`);
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
        statusEl.textContent = "Connected";
        statusEl.className = "status connected";
        addEvent("system", "Connected to sim server");
    };

    ws.onclose = () => {
        statusEl.textContent = "Disconnected";
        statusEl.className = "status disconnected";
        addEvent("system", "Disconnected");
        setTimeout(connect, 2000);
    };

    ws.onerror = () => {
        ws.close();
    };

    ws.onmessage = (e) => {
        if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4)
            return;

        const view = new DataView(e.data);
        const type = view.getUint32(0, true);
        const payload = e.data.slice(4);

        switch (type) {
            case FRAME_FB:    handleFramebuffer(payload); break;
            case FRAME_AUDIO: handleAudio(payload); break;
            case FRAME_EVENT: handleEvent(payload); break;
            case FRAME_STATE: handleState(payload); break;
        }
    };
}

/* ── Display rendering ─────────────────────────────────────────────── */
function handleFramebuffer(payload) {
    if (payload.byteLength < 8) return;

    const view = new DataView(payload);
    const x1 = view.getUint16(0, true);
    const y1 = view.getUint16(2, true);
    const x2 = view.getUint16(4, true);
    const y2 = view.getUint16(6, true);

    const w = x2 - x1 + 1;
    const h = y2 - y1 + 1;

    /* Auto-detect display size from first full-frame flush. */
    if (displayWidth === 0 || (x1 === 0 && y1 === 0 &&
        (x2 + 1) > displayWidth)) {
        displayWidth = x2 + 1;
        displayHeight = y2 + 1;
        canvas.width = displayWidth;
        canvas.height = displayHeight;
        resolutionEl.textContent = `${displayWidth}x${displayHeight}`;
    }

    /* Decode XRGB8888 pixels (matches LV_COLOR_DEPTH=32).
     * Memory layout per pixel: [B, G, R, X] (little-endian). */
    const pixels = new Uint8Array(payload, 8);
    const imgData = ctx.createImageData(w, h);
    const data = imgData.data;

    for (let i = 0; i < w * h; i++) {
        const off = i * 4;
        data[off]     = pixels[off + 2]; /* R */
        data[off + 1] = pixels[off + 1]; /* G */
        data[off + 2] = pixels[off];     /* B */
        data[off + 3] = 255;             /* A */
    }

    ctx.putImageData(imgData, x1, y1);

    /* Log first few frames for debugging. */
    frameCount++;
    if (frameCount <= 3) {
        /* Count non-black pixels (XRGB8888: 4 bytes each). */
        let nonBlack = 0;
        for (let i = 0; i < w * h; i++) {
            const off = i * 4;
            if (pixels[off] !== 0 || pixels[off+1] !== 0 || pixels[off+2] !== 0)
                nonBlack++;
        }
        addEvent("display",
            `frame #${frameCount}: ${w}x${h} at (${x1},${y1}) ` +
            `${payload.byteLength}B, ${nonBlack} non-black px`);
    }
    const now = performance.now();
    if (now - lastFpsTime >= 1000) {
        fpsEl.textContent = `${frameCount - lastFpsCount} FPS`;
        lastFpsCount = frameCount;
        lastFpsTime = now;
    }
}

/* ── Audio handling ────────────────────────────────────────────────── */
function handleAudio(payload) {
    if (payload.byteLength < 8) return;

    const view = new DataView(payload);
    audioSampleRate = view.getUint32(0, true);
    audioChannels   = view.getUint16(4, true);
    audioBitDepth   = view.getUint16(6, true);

    audioInfoEl.textContent =
        `${audioSampleRate} Hz / ${audioChannels}ch / ${audioBitDepth}bit`;

    const pcmData = new Uint8Array(payload, 8);

    /* Draw waveform. */
    drawWaveform(pcmData);

    /* Play audio if enabled. */
    if (audioEnabled && audioCtx) {
        playPcm(pcmData);
    }
}

function drawWaveform(pcmBytes) {
    const w = waveCanvas.width;
    const h = waveCanvas.height;
    waveCtx.fillStyle = "#0d1117";
    waveCtx.fillRect(0, 0, w, h);

    if (pcmBytes.length < 2) return;

    /* Interpret as signed 16-bit samples for waveform. */
    const samples = new Int16Array(pcmBytes.buffer,
                                   pcmBytes.byteOffset,
                                   Math.floor(pcmBytes.length / 2));
    const step = Math.max(1, Math.floor(samples.length / w));

    waveCtx.strokeStyle = "#4ecca3";
    waveCtx.lineWidth = 1;
    waveCtx.beginPath();

    for (let x = 0; x < w && x * step < samples.length; x++) {
        const v = samples[x * step] / 32768;
        const y = (1 - v) * h / 2;
        if (x === 0) waveCtx.moveTo(x, y);
        else waveCtx.lineTo(x, y);
    }
    waveCtx.stroke();
}

function playPcm(pcmBytes) {
    if (!audioCtx || audioCtx.sampleRate !== audioSampleRate) {
        audioCtx = new AudioContext({ sampleRate: audioSampleRate });
    }

    const samples = new Int16Array(pcmBytes.buffer,
                                   pcmBytes.byteOffset,
                                   Math.floor(pcmBytes.length / 2));
    const numFrames = Math.floor(samples.length / audioChannels);
    const buf = audioCtx.createBuffer(audioChannels, numFrames,
                                      audioSampleRate);

    for (let ch = 0; ch < audioChannels; ch++) {
        const channelData = buf.getChannelData(ch);
        for (let i = 0; i < numFrames; i++) {
            channelData[i] = samples[i * audioChannels + ch] / 32768;
        }
    }

    const src = audioCtx.createBufferSource();
    src.buffer = buf;
    src.connect(audioCtx.destination);
    src.start();
}

audioToggle.addEventListener("click", () => {
    audioEnabled = !audioEnabled;
    audioToggle.textContent = audioEnabled ? "Disable Audio" : "Enable Audio";
    if (audioEnabled && !audioCtx) {
        audioCtx = new AudioContext({ sampleRate: audioSampleRate });
    }
});

/* ── Plugin events ─────────────────────────────────────────────────── */
function handleEvent(payload) {
    if (payload.byteLength < 16) return;

    const view = new DataView(payload);
    const pluginId  = view.getUint32(0, true);
    const eventType = view.getUint32(4, true);
    const timestamp = view.getUint32(8, true);

    addEvent(`plugin:${pluginId}`, `type=${eventType} t=${timestamp}ms`);
}

function handleState(payload) {
    if (payload.byteLength < 4) return;

    const view = new DataView(payload);
    const pluginId = view.getUint32(0, true);
    const json = new TextDecoder().decode(new Uint8Array(payload, 4));

    addEvent(`state:${pluginId}`, json);
}

function addEvent(type, msg) {
    const el = document.createElement("div");
    el.className = "entry";

    const now = new Date();
    const time = now.toTimeString().split(" ")[0];

    el.innerHTML = `<span class="time">${time}</span>` +
                   `<span class="type">[${type}]</span>` +
                   `${msg}`;

    eventLog.prepend(el);

    /* Limit log size. */
    while (eventLog.children.length > 200) {
        eventLog.removeChild(eventLog.lastChild);
    }
}

/* ── Send command to firmware ──────────────────────────────────────── */
function sendCmd(pluginId, cmdType, data) {
    const hdrLen = 4 + 12 + (data ? data.length : 0);
    const buf = new ArrayBuffer(hdrLen);
    const view = new DataView(buf);

    view.setUint32(0, FRAME_CMD, true);
    view.setUint32(4, pluginId, true);
    view.setUint32(8, cmdType, true);
    view.setUint32(12, data ? data.length : 0, true);
    if (data) {
        new Uint8Array(buf, 16).set(data);
    }

    if (wasmWorker) {
        /* WASM mode: push command via exported C function. */
        if (Module && Module.ccall) {
            const arr = new Uint8Array(buf, 4); /* skip WS type header */
            const ptr = Module._malloc(arr.length);
            Module.HEAPU8.set(arr, ptr);
            Module.ccall('ove_sim_wasm_push_cmd', null,
                         ['number', 'number'], [ptr, arr.length]);
            Module._free(ptr);
        }
    } else if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(buf);
    }
}

window.sendCmd = sendCmd;

/* ── Init: auto-detect WASM vs WebSocket mode ─────────────────────── */
if (typeof Module !== 'undefined' && Module.onRuntimeInitialized) {
    /* WASM mode — Module was pre-defined in the shell HTML.
     * Display rendering is handled by the shell's pollFrame() loop
     * which reads directly from WASM heap via requestAnimationFrame.
     * No WebSocket needed. */
    addEvent("system", "WASM mode — waiting for runtime...");
} else {
    /* POSIX mode — connect via WebSocket. */
    connect();
}
