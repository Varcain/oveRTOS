/*
 * oveRTOS Sim Dashboard — page layout
 *
 * The layout host for the panels app.js renders (see "Layout host" at the
 * top of app.js). The simulated board is the centre of the page: the LCD
 * plus the parts of the board that carry live state, drawn from the
 * board's own description (manifest.json = boards/<board>/board.yaml +
 * the build's .config, see config/scripts/ove_manifest.py). Clicking a
 * part opens the matching tool in the inspector beside it.
 *
 * Layout model
 *   status bar   run state, run control, workspace, selection, alerts
 *   board        LCD + parts, scaled to fit; glance cards underneath
 *   inspector    two stacked panes, each showing one panel via tabs
 *   workspaces   Run / Debug / Perf: saved pane + size presets, kept per
 *                build ID; halting switches to Debug and back on resume
 *
 * Panels are declared once in PANELS; anything the build lacks (per the
 * manifest) shows how to enable it instead of disappearing.
 */
(function () {
"use strict";

/* ── Registry ─────────────────────────────────────────────────────── */

/* Inspector panels. el = section id from index.html's panel store,
 * feature = manifest.features key that must be true for the panel to have
 * data, kconfig = what to tell the user when it is not. */
var PANELS = {
    console:  { title: "Console",  el: "events-panel" },
    threads:  { title: "Threads",  el: "threads-panel" },
    trace:    { title: "Trace",    el: "trace-panel",    feature: "trace",    kconfig: "CONFIG_OVE_TRACE_STREAM" },
    profiler: { title: "Profiler", el: "profiler-panel", feature: "profiler", kconfig: "CONFIG_OVE_PROFILER" },
    debug:    { title: "Debug",    el: "debug-panel",    feature: "debugger" },
    audio:    { title: "Audio",    el: "audio-panel",    feature: "audio",    kconfig: "CONFIG_OVE_AUDIO" },
};
var PANEL_ORDER = ["console", "threads", "trace", "profiler", "debug", "audio"];

/* app.js window ids → board-view ids. */
var APP_IDS = { events: "console", threads: "threads", trace: "trace",
                profiler: "profiler", debug: "debug", audio: "audio", display: "display" };

/* board = board column width, split = pane A share of the inspector. */
var WORKSPACES = {
    run:   { title: "Run",   key: "1", board: 0.56, a: "console", b: null,       split: 0.62 },
    debug: { title: "Debug", key: "2", board: 0.32, a: "debug",   b: "console",  split: 0.66 },
    perf:  { title: "Perf",  key: "3", board: 0.28, a: "trace",   b: "profiler", split: 0.48 },
};
var WS_ORDER = ["run", "debug", "perf"];

var RTOS_NAMES = { freertos: "FreeRTOS", zephyr: "Zephyr", nuttx: "NuttX" };
var IDLE_RE = /^(IDLE|idle|Idle)(\d*)$|^Idle_Task$/;

/* ── State ────────────────────────────────────────────────────────── */

var S = {
    manifest: { features: {}, board: {} },
    haveManifest: false,
    seen: {},                       /* panel id → data has arrived */
    link: false, attempt: 0,
    dbg: "disconnected", dbgFile: null, dbgLine: 0, dbgReason: null,
    threads: [], heap: null, cpu: null, cpuHist: [],
    counts: { w: 0, e: 0 }, unseen: { w: 0, e: 0 }, lastAlert: null,
    selected: null,
    lcd: { w: 0, h: 0, fps: 0, scale: 1, live: false },
    simUs: 0, audioOut: 0,
    conLevel: "all", conQuery: "",
    autoFrom: null, autoTimer: null,
    extStatus: null,
    prefs: null,
};

function $(id) { return document.getElementById(id); }
function el(tag, cls, html) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (html != null) e.innerHTML = html;
    return e;
}
function esc(s) {
    return String(s == null ? "" : s).replace(/&/g, "&amp;").replace(/</g, "&lt;")
        .replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#39;");
}
function fmtBytes(n) {
    if (n >= 1048576) return (n / 1048576).toFixed(1) + " MB";
    if (n >= 1024) return (n / 1024).toFixed(1) + " KB";
    return n + " B";
}
function shortPath(p) {
    return typeof window._shortPath === "function" ? window._shortPath(p)
        : String(p || "").split("/").slice(-2).join("/");
}
function narrow() { return window.innerWidth <= 1000; }

/* ── Preferences (per build ID) ───────────────────────────────────── */

function storeKey() { return "ove-board:" + (S.manifest.build_id || "default"); }

function defaultWs(k) {
    var w = WORKSPACES[k];
    return { a: w.a, b: w.b, split: w.split, board: w.board };
}

function loadPrefs() {
    var p = null;
    try { p = JSON.parse(localStorage.getItem(storeKey())); } catch (e) {}
    p = p || {};
    var out = {
        ws: WORKSPACES[p.ws] ? p.ws : "run",
        wsState: {},
        scale: p.scale || "auto",
        autoDebug: p.autoDebug !== false,
        focus: !!p.focus,
    };
    WS_ORDER.forEach(function (k) {
        var saved = p.wsState && p.wsState[k];
        var d = defaultWs(k);
        if (saved && PANELS[saved.a]) {
            d.a = saved.a;
            d.b = PANELS[saved.b] ? saved.b : null;
            if (saved.split > 0.1 && saved.split < 0.9) d.split = saved.split;
            if (saved.board > 0.1 && saved.board < 0.9) d.board = saved.board;
        }
        out.wsState[k] = d;
    });
    return out;
}

function savePrefs() {
    try { localStorage.setItem(storeKey(), JSON.stringify(S.prefs)); } catch (e) {}
}

function wsState() { return S.prefs.wsState[S.prefs.ws]; }

/* ── Layout host API (consumed by app.js) ─────────────────────────── */

window.oveLayoutHost = {
    panelAvailable: function (id) {
        var p = APP_IDS[id];
        if (!p || p === "display") return;
        if (!S.seen[p]) {
            S.seen[p] = true;
            if (S.prefs) renderPanes();
        }
    },
    isPanelVisible: function (id) {
        return !document.body.classList.contains("bv-focus") && paneOf(APP_IDS[id] || id) !== null;
    },
    layoutDisplay: function (w, h) {
        var first = !S.lcd.live;
        S.lcd.w = w; S.lcd.h = h; S.lcd.live = true;
        if (first) updateLcdOverlay();
        layoutBoard();
    },
    on: function (evt, d) {
        var f = HANDLERS[evt];
        if (f) f(d);
    },
};

var HANDLERS = {
    link: function (d) {
        S.link = d.connected;
        S.attempt = d.attempt || 0;
        if (d.connected) S.extStatus = null;
        if (!d.connected) { S.dbg = "disconnected"; }
        renderRun();
        renderBoardLive();
        updateLcdOverlay();
    },
    log: function (d) {
        if (d.level === "w" || d.level === "e") {
            S.counts[d.level]++;
            S.lastAlert = { level: d.level, msg: d.msg };
            if (paneOf("console") === null) S.unseen[d.level]++;
            renderCounts();
            renderTabs();
        }
        if (d.el) applyConsoleFilter(d.el);
        if (d.type === "log") blink("uart-tx");
        scheduleGlance();
    },
    threads: function (d) {
        S.threads = d.threads || [];
        S.heap = d.heap;
        var idle = 0, have = false;
        S.threads.forEach(function (t) {
            if (t.cpu == null) return;
            have = true;
            if (IDLE_RE.test(t.name)) idle += t.cpu;
        });
        S.cpu = have ? Math.max(0, Math.min(100, 100 - idle)) : null;
        if (S.cpu != null) {
            S.cpuHist.push(S.cpu);
            if (S.cpuHist.length > 60) S.cpuHist.shift();
        }
        applyRowSelection();
        applyProfilerThread();
        renderBoardLive();
        scheduleGlance();
    },
    debug: function (d) {
        var prev = S.dbg;
        S.dbg = d.state;
        S.dbgFile = d.file; S.dbgLine = d.line; S.dbgReason = d.reason;
        if (d.state !== "disconnected") S.seen.debug = true;
        autoSwitch(prev, d.state);
        renderRun();
        renderBoardLive();
    },
    display: function (d) {
        S.lcd.fps = d.fps;
        renderLcdInfo();
    },
    audio: function (d) {
        S.seen.audio = true;
        S.audioOut = Math.max(d.peak, S.audioOut * 0.8);
        renderBoardLive();
    },
    simtime: function (d) {
        if (d.us > S.simUs) S.simUs = d.us;
        renderTime();
    },
    /* Progress / failure text from the in-page emulator (qemu-wasm.js). */
    status: function (d) {
        S.extStatus = d.ok ? null : d.text;
        renderRun();
    },
    "uart-rx": function () { blink("uart-rx"); },
    "select-thread": function (d) {
        selectThread(d.tid != null ? d.tid : tidByName(d.name), { fromTrace: true });
    },
};

/* ── Boot ─────────────────────────────────────────────────────────── */

function fetchWithTimeout(url, ms) {
    return new Promise(function (resolve) {
        var done = false;
        var t = setTimeout(function () { done = true; resolve(null); }, ms);
        fetch(url, { cache: "no-store" }).then(function (r) {
            if (done) return;
            clearTimeout(t);
            resolve(r.ok ? r : null);
        }).catch(function () { clearTimeout(t); resolve(null); });
    });
}

function loadScript(src) {
    return new Promise(function (resolve, reject) {
        var s = document.createElement("script");
        s.src = src;
        s.onload = resolve;
        s.onerror = reject;
        document.body.appendChild(s);
    });
}

/* app.js is loaded only after the layout exists, so its first frames
 * land in a finished page (and window.oveLayoutHost is already set). */
async function boot() {
    var res = await fetchWithTimeout("manifest.json", 2000);
    if (res) {
        try {
            S.manifest = await res.json();
            S.manifest.features = S.manifest.features || {};
            S.manifest.board = S.manifest.board || {};
            S.haveManifest = true;
        } catch (e) {}
    }

    S.prefs = loadPrefs();
    renderIdentity();
    renderBoard();
    renderGlance();
    drawGlance();
    renderWsNav();
    bindBar();
    bindSplitters();
    bindScale();
    bindPalette();
    bindKeys();
    document.body.classList.toggle("bv-focus", S.prefs.focus);
    $("bv-focus").classList.toggle("on", S.prefs.focus);
    applyWorkspace(S.prefs.ws, { initial: true });

    try {
        await loadScript("https://cdn.jsdelivr.net/npm/monaco-editor@0.52.2/min/vs/loader.js");
    } catch (e) { console.warn("[board] Monaco unavailable; source view disabled"); }
    await loadScript("app.js");
    afterApp();
}

/* Things that need app.js's DOM wiring in place. */
function afterApp() {
    bindConsoleBar();
    var tbody = $("thread-tbody");
    if (tbody) {
        tbody.addEventListener("click", function (e) {
            var tr = e.target.closest("tr");
            if (!tr || !tr.cells.length) return;
            var tid = tr.getAttribute("data-tid");
            tid = tid === null ? null : parseInt(tid, 10);
            selectThread(tid !== null && tid === S.selected ? null : tid);
        });
    }
    renderPanes();
    renderRun();
    layoutBoard();
}

/* ── Identity / status bar ────────────────────────────────────────── */

function renderIdentity() {
    var m = S.manifest, b = m.board || {};
    var app = m.app || "oveRTOS simulation";
    $("bv-app").textContent = app;
    var sub = [];
    if (m.rtos) sub.push(RTOS_NAMES[m.rtos] || m.rtos);
    if (b.title || b.name) sub.push(b.title || b.name);
    if (m.zero_heap) sub.push("zero-heap");
    $("bv-sub").textContent = sub.join("  ·  ");
    $("bv-id").title = m.build_id || "";
    $("bv-board-title").textContent = b.title || b.name || "Simulated board";
    document.title = app + " · oveRTOS";
}

function canDebug() { return S.link && S.dbg !== "disconnected"; }

function renderRun() {
    var pill = $("bv-state"), txt = $("bv-state-text");
    var halted = S.link && S.dbg === "stopped";
    if (S.extStatus) {
        pill.className = "bv-pill offline";
        txt.textContent = S.extStatus;
        pill.title = "";
    } else if (!S.link) {
        pill.className = "bv-pill offline";
        txt.textContent = S.attempt ? "Reconnecting (" + S.attempt + ")" : "Connecting";
        pill.title = "";
    } else if (halted) {
        pill.className = "bv-pill halted";
        var loc = S.dbgFile ? shortPath(S.dbgFile) + ":" + S.dbgLine : "";
        txt.textContent = "Halted" + (loc ? "  " + loc : "");
        pill.title = (S.dbgReason ? S.dbgReason + "  " : "") + (S.dbgFile ? S.dbgFile + ":" + S.dbgLine : "");
    } else {
        pill.className = "bv-pill running";
        txt.textContent = "Running";
        pill.title = canDebug() ? "" : "No debugger attached: run control unavailable";
    }

    var t = $("bv-toggle");
    t.disabled = !canDebug();
    t.innerHTML = halted ? "&#9654;" : "&#10074;&#10074;";
    t.title = halted ? "Continue (F5)" : "Pause (F6)";
    ["bv-over", "bv-into", "bv-out"].forEach(function (id) { $(id).disabled = !halted; });
    $("bv-reset").disabled = !canDebug();
    renderTime();
}

function renderTime() {
    var el = $("bv-time");
    if (!S.simUs) { el.textContent = ""; return; }
    var s = S.simUs / 1e6;
    var m = Math.floor(s / 60);
    var rest = (s - m * 60).toFixed(1);
    el.textContent = "t " + m + ":" + (rest < 10 ? "0" : "") + rest;
}

function renderCounts() {
    $("bv-wcount").textContent = S.counts.w;
    $("bv-ecount").textContent = S.counts.e;
    document.querySelector(".bv-counts .w").classList.toggle("hot", S.counts.w > 0);
    document.querySelector(".bv-counts .e").classList.toggle("hot", S.counts.e > 0);
}

function dbg(cmd) {
    if (typeof window.sendDebugCmd === "function") window.sendDebugCmd(cmd);
}

function resetBoard() {
    if (!canDebug()) return;
    dbg(window.DBG_CMD_RESET);
    S.simUs = 0;
    renderTime();
}

function bindBar() {
    $("bv-toggle").addEventListener("click", function () {
        dbg(S.dbg === "stopped" ? window.DBG_CMD_CONTINUE : window.DBG_CMD_PAUSE);
    });
    $("bv-over").addEventListener("click", function () { dbg(window.DBG_CMD_STEP_OVER); });
    $("bv-into").addEventListener("click", function () { dbg(window.DBG_CMD_STEP_INTO); });
    $("bv-out").addEventListener("click", function () { dbg(window.DBG_CMD_STEP_OUT); });
    $("bv-reset").addEventListener("click", resetBoard);
    $("bv-counts").addEventListener("click", function () {
        setConsoleLevel(S.counts.e && !S.counts.w ? "e" : "w");
        openPanel("console");
    });
    $("bv-palette-btn").addEventListener("click", openPalette);
    $("bv-focus").addEventListener("click", toggleFocus);
}

/* Halting switches to the Debug workspace; resuming switches back. The
 * return is delayed so single-steps (stop → run → stop) don't flicker. */
function autoSwitch(prev, now) {
    if (!S.prefs || !S.prefs.autoDebug) return;
    if (now === "stopped") {
        clearTimeout(S.autoTimer);
        if (prev !== "stopped" && S.prefs.ws !== "debug" && S.autoFrom === null) {
            S.autoFrom = S.prefs.ws;
            applyWorkspace("debug", { auto: true });
        }
    } else if (now === "running" && S.autoFrom !== null) {
        clearTimeout(S.autoTimer);
        S.autoTimer = setTimeout(function () {
            if (S.dbg === "running" && S.autoFrom !== null && S.prefs.ws === "debug") {
                var back = S.autoFrom;
                S.autoFrom = null;
                applyWorkspace(back, { auto: true });
            }
        }, 600);
    }
}

/* ── Workspaces ───────────────────────────────────────────────────── */

function renderWsNav() {
    var nav = $("bv-ws");
    nav.innerHTML = "";
    WS_ORDER.forEach(function (k) {
        var b = el("button", "", esc(WORKSPACES[k].title) + "<kbd>" + WORKSPACES[k].key + "</kbd>");
        b.dataset.ws = k;
        b.title = WORKSPACES[k].title + " workspace (" + WORKSPACES[k].key + ")";
        b.addEventListener("click", function () { applyWorkspace(k, { manual: true }); });
        nav.appendChild(b);
    });
}

function applyWorkspace(k, opts) {
    opts = opts || {};
    if (!WORKSPACES[k]) return;
    if (opts.manual) S.autoFrom = null;    /* user took over */
    S.prefs.ws = k;
    var nav = $("bv-ws").children;
    for (var i = 0; i < nav.length; i++) nav[i].classList.toggle("active", nav[i].dataset.ws === k);
    applySizes();
    renderPanes();
    savePrefs();
}

function applySizes() {
    var st = wsState();
    document.body.style.setProperty("--board-w", (st.board * 100).toFixed(1) + "%");
    document.body.style.setProperty("--split", (st.split * 100).toFixed(1) + "%");
}

function resetWorkspace() {
    S.prefs.wsState[S.prefs.ws] = defaultWs(S.prefs.ws);
    applyWorkspace(S.prefs.ws);
}

/* ── Inspector panes ──────────────────────────────────────────────── */

function paneEl(k) { return document.querySelector('.bv-pane[data-pane="' + k + '"]'); }

function paneOf(p) {
    if (!S.prefs || !p) return null;
    var st = wsState();
    if (st.a === p) return "a";
    if (st.b === p && !narrow()) return "b";
    return null;
}

function panelState(p) {
    var d = PANELS[p];
    if (S.seen[p] || !d.feature || !S.haveManifest) return "ok";
    return S.manifest.features[d.feature] ? "ok" : "na";
}

function emptyState(p) {
    var d = PANELS[p];
    var box = el("div", "bv-empty");
    var html;
    if (p === "debug") {
        html = "<b>No debugger attached</b><span>" + (S.manifest.web
            ? "The browser bundle runs the optimised firmware inside the page, with no GDB to attach. "
              + "Debug the same app on the native <code>qemu</code> board (<code>make run</code>)."
            : "QEMU was started without its GDB server.") + "</span>";
        box.innerHTML = html;
        return box;
    }
    html = "<b>" + esc(d.title) + " is not available in this build</b>";
    if (d.kconfig)
        html += "<span>Enable <code>" + esc(d.kconfig) + "</code> (make menuconfig) and rebuild.</span>";
    box.innerHTML = html;
    return box;
}

function renderTabs() {
    if (!S.prefs) return;
    var st = wsState();
    ["a", "b"].forEach(function (k) {
        var strip = paneEl(k).querySelector(".bv-tabs");
        strip.innerHTML = "";
        var other = k === "a" ? st.b : st.a;
        PANEL_ORDER.forEach(function (p) {
            var t = el("button", "bv-tab");
            t.textContent = PANELS[p].title;
            if (st[k] === p) t.classList.add("active");
            else if (other === p) { t.classList.add("elsewhere"); t.title = "Shown in the other pane: click to swap"; }
            if (panelState(p) === "na") { t.classList.add("na"); t.title = "Not enabled in this build"; }
            if (p === "console" && st[k] !== p && other !== p && (S.unseen.e || S.unseen.w)) {
                var bd = el("span", "badge" + (S.unseen.e ? "" : " w"));
                bd.textContent = S.unseen.e || S.unseen.w;
                t.appendChild(bd);
            }
            t.addEventListener("click", function () { showInPane(k, p); });
            strip.appendChild(t);
        });
        var act;
        if (k === "a") {
            act = el("button", "bv-icon bv-pane-act", st.b ? "&#9707;" : "&#9707;");
            act.title = st.b ? "Close the second pane" : "Split: show a second panel below";
            act.classList.toggle("on", !!st.b);
            act.addEventListener("click", toggleSplit);
        } else {
            act = el("button", "bv-icon bv-pane-act", "&#10005;");
            act.title = "Close this pane";
            act.addEventListener("click", function () { wsState().b = null; renderPanes(); savePrefs(); });
        }
        strip.appendChild(act);
    });
}

function renderPanes() {
    if (!S.prefs) return;
    var st = wsState();
    var store = $("panel-store");
    $("bv-inspector").classList.toggle("single", !st.b);
    /* Unmount everything first so a panel moving A→B isn't yanked back. */
    ["a", "b"].forEach(function (k) {
        var body = paneEl(k).querySelector(".bv-pane-body");
        Array.prototype.slice.call(body.children).forEach(function (c) {
            if (c.classList.contains("panel")) store.appendChild(c); else c.remove();
        });
    });
    ["a", "b"].forEach(function (k) {
        var p = st[k];
        if (!p || (k === "b" && narrow())) return;
        var body = paneEl(k).querySelector(".bv-pane-body");
        if (panelState(p) === "na") { body.appendChild(emptyState(p)); return; }
        var sec = $(PANELS[p].el);
        if (sec) body.appendChild(sec);
    });
    if (paneOf("console") !== null) { S.unseen.w = S.unseen.e = 0; }
    renderTabs();
    relayoutSoon();
}

function showInPane(k, p) {
    var st = wsState();
    var o = k === "a" ? "b" : "a";
    if (st[o] === p) st[o] = st[k];          /* swap */
    st[k] = p;
    if (!st.a) { st.a = st.b; st.b = null; }
    renderPanes();
    savePrefs();
}

function toggleSplit() {
    var st = wsState();
    if (st.b) { st.b = null; }
    else {
        st.b = PANEL_ORDER.filter(function (p) {
            return p !== st.a && panelState(p) === "ok";
        })[0] || null;
    }
    renderPanes();
    savePrefs();
}

/* Bring a panel on screen: leave it where it is if visible, otherwise
 * put it in the top pane. */
function openPanel(p) {
    if (!PANELS[p]) return;
    if (document.body.classList.contains("bv-focus")) toggleFocus();
    if (paneOf(p) === null) showInPane("a", p);
    var k = paneOf(p);
    var tab = k && paneEl(k).querySelector(".bv-tab.active");
    if (tab) { tab.animate([{ background: "rgba(91,141,239,0.35)" }, { background: "transparent" }], 500); }
}

var _relayout = 0;
function relayoutSoon() {
    if (_relayout) return;
    _relayout = requestAnimationFrame(function () {
        _relayout = 0;
        layoutBoard();
        /* app.js re-measures its canvases (trace, waveforms) on resize. */
        window.dispatchEvent(new Event("resize"));
        if (typeof window.scheduleTraceRender === "function") window.scheduleTraceRender();
    });
}

function toggleFocus() {
    S.prefs.focus = !S.prefs.focus;
    document.body.classList.toggle("bv-focus", S.prefs.focus);
    $("bv-focus").classList.toggle("on", S.prefs.focus);
    savePrefs();
    relayoutSoon();
}

/* ── Splitters ────────────────────────────────────────────────────── */

function bindSplitters() {
    function drag(handle, cls, onMove) {
        handle.addEventListener("pointerdown", function (e) {
            e.preventDefault();
            handle.setPointerCapture(e.pointerId);
            document.body.classList.add(cls);
            function move(ev) { onMove(ev); applySizes(); relayoutSoon(); }
            function up() {
                document.body.classList.remove(cls);
                handle.removeEventListener("pointermove", move);
                handle.removeEventListener("pointerup", up);
                savePrefs();
            }
            handle.addEventListener("pointermove", move);
            handle.addEventListener("pointerup", up);
        });
    }
    drag($("bv-split-v"), "bv-dragging", function (ev) {
        var r = $("bv-main").getBoundingClientRect();
        wsState().board = Math.max(0.2, Math.min(0.75, (ev.clientX - r.left) / r.width));
    });
    drag($("bv-split-h"), "bv-dragging-h", function (ev) {
        var r = $("bv-inspector").getBoundingClientRect();
        wsState().split = Math.max(0.15, Math.min(0.85, (ev.clientY - r.top) / r.height));
    });
    /* Double-click restores the workspace default. */
    $("bv-split-v").addEventListener("dblclick", function () {
        wsState().board = WORKSPACES[S.prefs.ws].board; applySizes(); relayoutSoon(); savePrefs();
    });
    $("bv-split-h").addEventListener("dblclick", function () {
        wsState().split = WORKSPACES[S.prefs.ws].split; applySizes(); relayoutSoon(); savePrefs();
    });
    window.addEventListener("resize", function (e) {
        if (e.isTrusted) renderPanes();     /* crossing the narrow breakpoint */
    });
}

/* ── The board ────────────────────────────────────────────────────── */

function boardDisplay() {
    var b = S.manifest.board, f = S.manifest.features;
    if (b.display && b.display.width) return b.display;
    if (f.display || !S.haveManifest) return { width: 480, height: 272 };
    return null;
}

function comp(cls, open, label, inner, title) {
    var c = el("button", "comp " + cls, inner);
    if (open) { c.dataset.open = open; c.dataset.label = label; }
    else c.disabled = true;
    if (title) c.title = title;
    return c;
}

function renderBoard() {
    var pcb = $("pcb");
    var m = S.manifest, b = m.board, f = m.features;
    var canvas = $("display-canvas");
    pcb.innerHTML = "";
    ["tl", "tr", "bl", "br"].forEach(function (c) { pcb.appendChild(el("i", "pcb-hole " + c)); });
    pcb.appendChild(el("div", "pcb-title",
        "<span>" + esc((b.name || "sim board").toUpperCase()) + "</span>" +
        "<span>" + esc(m.rtos ? (RTOS_NAMES[m.rtos] || m.rtos) : "oveRTOS") + "</span>"));

    /* LCD module: the real canvas lives here (pointer input goes to the
     * firmware as touch). Sized before the first frame from board.yaml so
     * the board doesn't jump when video starts. */
    var disp = boardDisplay();
    if (disp && canvas) {
        var lcd = el("div", "pcb-lcd");
        var bezel = el("div", "lcd-bezel");
        var glass = el("div", "lcd-glass");
        glass.appendChild(canvas);
        glass.appendChild(el("div", "lcd-overlay"));
        bezel.appendChild(glass);
        lcd.appendChild(bezel);
        var silk = el("div", "silk lcd-silk");
        silk.dataset.base = "LCD " + disp.width + "\u00d7" + disp.height +
            (disp.color_format ? " " + disp.color_format : "");
        silk.id = "bv-lcd-info";
        silk.textContent = silk.dataset.base;
        lcd.appendChild(silk);
        pcb.appendChild(lcd);
        if (!S.lcd.w) { S.lcd.w = disp.width; S.lcd.h = disp.height; }
    }

    var row = el("div", "pcb-row");

    /* MCU */
    var core = (/\(([^)]+)\)/.exec(b.title || "") || [])[1] || (b.mcu_family || "").toUpperCase();
    var mhz = b.clock && b.clock.sysclk_mhz ? b.clock.sysclk_mhz + " MHz" : "";
    row.appendChild(comp("comp-mcu", f.profiler !== false ? "profiler" : "threads",
        f.profiler !== false ? "Profiler" : "Threads",
        "<div class='chip-qfp' id='bv-mcu-chip'><i class='dot'></i>" +
        "<div class='chip-label'><b>" + esc(b.mcu || "MCU") + "</b><br>" + esc(core) +
        (mhz ? "<br>" + esc(mhz) : "") + "</div></div>" +
        "<span class='live' id='bv-mcu-live'>CPU --</span>",
        "Microcontroller: CPU load from the RTOS thread statistics"));

    /* SRAM / heap */
    var ram = b.memory && b.memory.ram_kb ? (b.memory.ram_kb >= 1024 ? b.memory.ram_kb / 1024 + " MB" : b.memory.ram_kb + " KB") : "";
    row.appendChild(comp("comp-ram", "threads", "Threads",
        "<div class='chip-tsop'><div class='chip-label'><b>SRAM</b> " + esc(ram) + "</div></div>" +
        "<div class='meter'><span id='bv-ram-fill'></span></div>" +
        "<span class='live' id='bv-ram-live'>heap --</span>",
        "RAM: RTOS heap usage (peak marked in the Threads panel)"));

    /* Console UART */
    var con = b.serial && b.serial.console;
    if (con || f.console || !S.haveManifest) {
        var inst = con ? String(con.instance || "UART").replace(/^CMSDK_/, "") : "UART";
        row.appendChild(comp("comp-uart", "console", "Console",
            "<div class='leds'><span class='led green'><i id='bv-uart-tx'></i><span>TX</span></span>" +
            "<span class='led amber'><i id='bv-uart-rx'></i><span>RX</span></span></div>" +
            "<div class='header header-uart'><i></i><i></i><i></i><i></i></div>" +
            "<div class='pin-names'><span>G</span><span>R</span><span>T</span><span>V</span></div>" +
            "<span class='silk'>" + esc(inst) + (con && con.baud ? " &middot; " + con.baud : "") + "</span>",
            "Console UART: TX blinks on firmware output, RX on shell input"));
    }

    /* SWD / debugger */
    var swd = comp("comp-swd" + (f.debugger === false ? " unused" : ""), "debug", "Debug",
        "<div class='leds'><span class='led' id='bv-swd-led-wrap'><i id='bv-swd-led'></i><span>DBG</span></span></div>" +
        "<div class='header header-swd'>" + "<i></i>".repeat(10) + "</div>" +
        "<span class='silk'>SWD &middot; GDB</span>",
        f.debugger === false ? "No debugger attached to this run" : "Debug port: GDB attached (green running, amber halted)");
    row.appendChild(swd);

    /* Audio codec */
    if (b.audio || f.audio) {
        var i2s = b.audio && b.audio.i2s;
        var codec = (b.audio && b.audio.codec && b.audio.codec.type) || "I2S codec";
        var used = f.audio || S.seen.audio;
        row.appendChild(comp("comp-codec" + (used ? "" : " unused"), "audio", "Audio",
            "<div class='codec'><div class='jack'><i></i><div class='lvl'><span id='bv-jack-in'></span></div><b>IN</b></div>" +
            "<div class='chip-small'></div>" +
            "<div class='jack'><i></i><div class='lvl'><span id='bv-jack-out'></span></div><b>OUT</b></div></div>" +
            "<span class='silk'>" + esc(codec) + (i2s && i2s.sample_rate ? " &middot; " + (i2s.sample_rate / 1000) + " kHz" : "") + "</span>",
            used ? "Audio codec: output level" : "Audio hardware present, not used by this app"));
    }

    /* User LEDs (board.yaml leds:). The bridge does not stream GPIO yet,
     * so they are drawn but stay dark — the slot for a GPIO panel. */
    var leds = b.leds || [];
    if (leds.length) {
        var html = "<div class='leds'>";
        leds.forEach(function (l, i) {
            html += "<span class='led green' title='P" + l.port + "." + l.pin + "'><i></i><span>LD" + (i + 1) + "</span></span>";
        });
        row.appendChild(comp("comp-leds unused", null, null,
            html + "</div><span class='silk'>User LEDs</span>",
            "User LEDs: live GPIO state is not streamed to the dashboard yet"));
    }

    /* Status LEDs + reset */
    row.appendChild(comp("comp-status", null, null,
        "<div class='leds'><span class='led red'><i id='bv-pwr'></i><span>PWR</span></span>" +
        "<span class='led green' id='bv-run-wrap'><i id='bv-run'></i><span>RUN</span></span></div>" +
        "<span class='silk'>Status</span>",
        "PWR: connected to the simulator. RUN: green running, amber halted"));
    row.appendChild(comp("comp-reset", "reset", "Reset board",
        "<div class='tact'><i></i></div><span class='silk'>Reset</span>",
        "Reset the board (needs the debugger)"));

    pcb.appendChild(row);

    pcb.onclick = function (e) {
        var c = e.target.closest(".comp");
        if (!c || c.disabled) return;
        if (c.dataset.open === "reset") resetBoard();
        else openPanel(c.dataset.open);
    };

    updateLcdOverlay();
    renderBoardLive();
    if (typeof ResizeObserver !== "undefined" && !renderBoard._ro) {
        renderBoard._ro = new ResizeObserver(function () { layoutBoard(); });
        renderBoard._ro.observe($("bv-stage"));
    }
}

function updateLcdOverlay() {
    var ov = document.querySelector(".lcd-overlay");
    if (!ov) return;
    var f = S.manifest.features;
    if (S.lcd.live) { ov.hidden = true; return; }
    ov.hidden = false;
    ov.textContent = !S.link ? "Waiting for the simulator…"
        : (S.haveManifest && !f.display) ? "This app does not drive the display"
        : "Waiting for the first frame…";
}

/* Fit LCD + parts into the stage. "auto" = largest whole-number scale
 * that fits (crisp pixels), dropping below 1x only when it must.
 *
 * The parts row can be laid out three ways and the one that leaves the LCD
 * bigger wins: "wrap" (parts flow onto extra rows, full size), "zoom2"
 * or "zoom1" (shrunk into two rows or one — what small screens get). The board is
 * as wide as the parts row needs, up to the stage width, so parts only
 * wrap or shrink when there really is no room. */
function layoutBoard() {
    var pcb = $("pcb"), stage = $("bv-stage"), cv = $("display-canvas");
    var glass = pcb.querySelector(".lcd-glass");
    var row = pcb.querySelector(".pcb-row");
    if (!stage.clientWidth || !row) return;
    var cs = getComputedStyle(stage);
    var availW = stage.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
    var availH = stage.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom);
    var ps = getComputedStyle(pcb);
    var padX = parseFloat(ps.paddingLeft) + parseFloat(ps.paddingRight) + 2;

    row.style.zoom = 1;
    row.style.width = "";
    row.style.maxWidth = "";
    row.classList.add("nowrap");
    var rowNat = row.scrollWidth;

    if (!glass || !cv) {
        row.classList.remove("nowrap");
        pcb.style.width = Math.min(availW, rowNat + padX) + "px";
        return;
    }
    var w = S.lcd.w || 480, h = S.lcd.h || 272;
    var bezelX = glass.parentElement.offsetWidth - glass.offsetWidth;

    function pick(fit) {
        var mode = S.prefs ? S.prefs.scale : "auto";
        if (mode === "fit") return fit;
        if (mode === "auto") return fit >= 1 ? Math.floor(fit) : fit;
        return parseFloat(mode) || 1;
    }
    function apply(sc, mode) {
        var lw = Math.max(1, Math.floor(w * sc)), lh = Math.max(1, Math.floor(h * sc));
        cv.style.width = lw + "px";
        cv.style.height = lh + "px";
        var pw = Math.max(lw + bezelX + padX, Math.min(availW, Math.max(rowNat + padX, 300)));
        pcb.style.width = pw + "px";
        row.classList.remove("nowrap");
        if (mode !== "wrap") {
            /* shrink so the parts fit in two rows (zoom2) or one (zoom1):
             * lay the row out at width/zoom, then zoom it back down */
            var z = mode === "zoom2"
                ? Math.max(0.4, Math.min(1, 1.85 * (pw - padX) / rowNat))
                : Math.max(0.25, Math.min(1, 0.98 * (pw - padX) / rowNat));
            row.style.zoom = z;
            row.style.width = Math.floor((pw - padX) / z) + "px";
            row.style.maxWidth = "none";
        } else {
            row.style.zoom = 1;
            row.style.width = "";
            row.style.maxWidth = "";
        }
    }
    var fitW = Math.max(0.1, (availW - padX - bezelX) / w);
    function trial(mode) {
        var sc = pick(fitW);
        for (var pass = 0; pass < 4; pass++) {
            apply(sc, mode);
            var chromeH = pcb.offsetHeight - glass.offsetHeight;
            var next = pick(Math.max(0.1, Math.min(fitW, (availH - chromeH) / h)));
            if (next === sc) break;
            sc = next;
        }
        return sc;
    }
    /* Prefer full-size parts; shrink them only for a clearly bigger LCD. */
    var mode = "wrap", s = trial("wrap");
    ["zoom2", "zoom1"].forEach(function (m) {
        var t = trial(m);
        if (t > s * 1.05) { s = t; mode = m; }
    });
    apply(s, mode);
    cv.classList.toggle("smooth", s !== Math.floor(s));
    S.lcd.scale = s;
    renderLcdInfo();
}

function renderLcdInfo() {
    var info = $("bv-lcd-info");
    if (!info) return;
    var s = S.lcd.scale;
    var parts = [info.dataset.base];
    if (s) parts.push(s === Math.floor(s) ? s + "\u00d7" : Math.round(s * 100) + "%");
    if (S.lcd.live && S.lcd.fps != null) parts.push(S.lcd.fps + " fps");
    info.textContent = parts.join("  \u00b7  ");
}

function bindScale() {
    var segs = document.querySelectorAll("#bv-scale .seg");
    function mark() {
        for (var i = 0; i < segs.length; i++)
            segs[i].classList.toggle("active", segs[i].dataset.scale === S.prefs.scale);
    }
    for (var i = 0; i < segs.length; i++) {
        segs[i].addEventListener("click", function () { setScale(this.dataset.scale); });
    }
    bindScale.mark = mark;
    mark();
}

function setScale(v) {
    S.prefs.scale = v;
    savePrefs();
    if (bindScale.mark) bindScale.mark();
    layoutBoard();
}

/* Live values drawn on the board. */
function renderBoardLive() {
    var mcuLive = $("bv-mcu-live");
    if (!mcuLive) return;
    mcuLive.textContent = S.cpu == null ? "CPU --" : "CPU " + S.cpu.toFixed(1) + "%";
    var chip = $("bv-mcu-chip");
    if (chip) {
        var g = S.cpu == null ? 0 : Math.min(1, S.cpu / 100);
        chip.style.boxShadow = "0 3px 8px rgba(0,0,0,0.5), 0 0 " + (4 + g * 22) + "px " +
            (g * 6) + "px rgba(78,204,163," + (0.15 + g * 0.6).toFixed(2) + ")";
    }
    var fill = $("bv-ram-fill"), ramLive = $("bv-ram-live");
    if (S.heap && S.heap.total) {
        var pct = S.heap.used * 100 / S.heap.total;
        fill.style.width = pct.toFixed(1) + "%";
        fill.className = pct > 90 ? "crit" : pct > 70 ? "warn" : "";
        ramLive.textContent = "heap " + fmtBytes(S.heap.used) + " / " + fmtBytes(S.heap.total);
    }
    var pwr = $("bv-pwr");
    if (pwr) pwr.classList.toggle("on", S.link);
    var run = $("bv-run"), runWrap = $("bv-run-wrap");
    if (run) {
        var halted = S.link && S.dbg === "stopped";
        runWrap.className = "led " + (halted ? "amber" : "green");
        run.classList.toggle("on", S.link);
        run.classList.toggle("beat", S.link && !halted);
    }
    var swd = $("bv-swd-led"), swdWrap = $("bv-swd-led-wrap");
    if (swd) {
        swdWrap.className = "led " + (S.dbg === "stopped" ? "amber" : "green");
        swd.classList.toggle("on", canDebug());
    }
    var out = $("bv-jack-out");
    if (out) out.style.width = Math.min(100, S.audioOut * 140).toFixed(0) + "%";
}

var _blinkT = {};
function blink(which) {
    var id = which === "uart-tx" ? "bv-uart-tx" : "bv-uart-rx";
    var led = $(id);
    if (!led) return;
    led.classList.add("on");
    clearTimeout(_blinkT[id]);
    _blinkT[id] = setTimeout(function () { led.classList.remove("on"); }, 90);
}

/* ── Glance cards ─────────────────────────────────────────────────── */

function renderGlance() {
    var g = $("bv-glance");
    g.innerHTML = "";
    function card(id, title, open, viz) {
        var c = el("button", "card",
            "<span class='lbl'>" + title + "</span><b id='" + id + "-v'>--</b><span class='viz'>" + viz + "</span>");
        c.id = id;
        c.title = "Open " + PANELS[open].title;
        c.addEventListener("click", function () { openPanel(open); });
        g.appendChild(c);
    }
    card("gl-cpu", "CPU", S.manifest.features.profiler === false ? "threads" : "profiler", "<canvas id='gl-cpu-spark'></canvas>");
    card("gl-heap", "Heap", "threads", "<span class='meter'><span id='gl-heap-fill'></span></span>");
    card("gl-thr", "Threads", "threads", "<span class='dots' id='gl-thr-dots'></span>");
    card("gl-log", "Log", "console", "<span class='sub' id='gl-log-sub'>no warnings</span>");
}

var _glanceRaf = 0;
function scheduleGlance() {
    if (_glanceRaf) return;
    _glanceRaf = requestAnimationFrame(function () { _glanceRaf = 0; drawGlance(); });
}

function drawGlance() {
    var v = $("gl-cpu-v");
    if (!v) return;
    v.textContent = S.cpu == null ? "--" : S.cpu.toFixed(1) + "%";
    var cv = $("gl-cpu-spark");
    if (cv && cv.clientWidth) {
        var dpr = window.devicePixelRatio || 1;
        var W = cv.clientWidth, H = cv.clientHeight;
        if (cv.width !== W * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
        var c = cv.getContext("2d");
        c.setTransform(dpr, 0, 0, dpr, 0, 0);
        c.clearRect(0, 0, W, H);
        var hist = S.cpuHist, n = 60;
        if (hist.length > 1) {
            var max = Math.max(10, Math.max.apply(null, hist) * 1.2);
            c.beginPath();
            for (var i = 0; i < hist.length; i++) {
                var x = W - (hist.length - 1 - i) * (W / (n - 1));
                var y = H - 2 - (hist[i] / max) * (H - 4);
                if (i === 0) c.moveTo(x, y); else c.lineTo(x, y);
            }
            c.strokeStyle = "#4ecca3";
            c.lineWidth = 1.5;
            c.stroke();
            c.lineTo(W, H); c.lineTo(W - (hist.length - 1) * (W / (n - 1)), H); c.closePath();
            c.fillStyle = "rgba(78,204,163,0.12)";
            c.fill();
        }
    }
    if (S.heap && S.heap.total) {
        var pct = S.heap.used * 100 / S.heap.total;
        $("gl-heap-v").textContent = pct.toFixed(0) + "%";
        var f = $("gl-heap-fill");
        f.style.width = pct.toFixed(1) + "%";
        f.className = pct > 90 ? "crit" : pct > 70 ? "warn" : "";
        $("gl-heap").title = "Heap " + fmtBytes(S.heap.used) + " / " + fmtBytes(S.heap.total) +
            ", peak " + fmtBytes(S.heap.peak) + ". Open Threads";
    }
    var dots = $("gl-thr-dots");
    if (S.threads.length) {
        $("gl-thr-v").textContent = S.threads.length;
        dots.innerHTML = S.threads.map(function (t) {
            return "<i class='s" + t.state + (t.tid === S.selected ? " sel" : "") + "' title='" +
                esc(t.name) + "'></i>";
        }).join("");
    }
    $("gl-log-v").innerHTML = "<span style='color:" + (S.counts.w ? "var(--amber)" : "var(--faint)") + "'>\u26a0" + S.counts.w +
        "</span> <span style='color:" + (S.counts.e ? "#ff6b81" : "var(--faint)") + "'>\u2716" + S.counts.e + "</span>";
    var sub = $("gl-log-sub");
    if (S.lastAlert) {
        sub.textContent = S.lastAlert.msg.replace(/^\s*\[[WE]\]\s*/, "");
        sub.className = "sub " + S.lastAlert.level;
        $("gl-log").title = S.lastAlert.msg;
    } else {
        sub.textContent = "no warnings";
        sub.className = "sub";
    }
}

/* ── Console filtering ────────────────────────────────────────────── */

function bindConsoleBar() {
    var bar = document.querySelector(".bv-con-bar");
    if (!bar) return;
    $("bv-con-search").addEventListener("input", function () {
        S.conQuery = this.value.toLowerCase();
        refilterConsole();
    });
    bar.querySelectorAll(".seg").forEach(function (b) {
        b.addEventListener("click", function () { setConsoleLevel(b.dataset.lvl); });
    });
    $("bv-con-clear").addEventListener("click", clearConsole);
}

function setConsoleLevel(lvl) {
    S.conLevel = lvl;
    document.querySelectorAll(".bv-levels .seg").forEach(function (b) {
        b.classList.toggle("active", b.dataset.lvl === lvl);
    });
    refilterConsole();
}

function applyConsoleFilter(entry) {
    var ok = true;
    if (S.conLevel === "w") ok = entry.classList.contains("lvl-w") || entry.classList.contains("lvl-e");
    else if (S.conLevel === "e") ok = entry.classList.contains("lvl-e");
    if (ok && S.conQuery) ok = entry.textContent.toLowerCase().indexOf(S.conQuery) >= 0;
    entry.hidden = !ok;
}

function refilterConsole() {
    var log = $("event-log");
    if (!log) return;
    for (var i = 0; i < log.children.length; i++) applyConsoleFilter(log.children[i]);
}

function clearConsole() {
    var log = $("event-log");
    if (log) log.innerHTML = "";
    S.counts.w = S.counts.e = 0;
    S.unseen.w = S.unseen.e = 0;
    S.lastAlert = null;
    renderCounts();
    renderTabs();
    scheduleGlance();
}

function focusShell() {
    openPanel("console");
    var i = $("console-input");
    if (i) i.focus();
}

/* ── Linked thread selection ──────────────────────────────────────── */

function tidByName(name) {
    if (name == null) return null;
    for (var i = 0; i < S.threads.length; i++)
        if (S.threads[i].name === name) return S.threads[i].tid;
    return null;
}

function selectedThreadName() {
    if (S.selected == null) return null;
    for (var i = 0; i < S.threads.length; i++)
        if (S.threads[i].tid === S.selected) return S.threads[i].name;
    return "0x" + S.selected.toString(16);
}

function selectThread(tid, opts) {
    S.selected = (tid == null) ? null : tid;
    if (!(opts && opts.fromTrace) && typeof window.oveTraceSelect === "function")
        window.oveTraceSelect(S.selected);
    applyRowSelection();
    applyProfilerThread();
    var chip = $("bv-sel");
    if (S.selected != null) {
        chip.hidden = false;
        chip.innerHTML = "&#9673; " + esc(selectedThreadName()) + " <button title='Clear selection (Esc)'>&#10005;</button>";
        chip.title = "Selected thread: highlighted in Trace, Profiler filtered to it";
        chip.querySelector("button").onclick = function () { selectThread(null); };
    } else {
        chip.hidden = true;
    }
    scheduleGlance();
}

function applyRowSelection() {
    var tbody = $("thread-tbody");
    if (!tbody) return;
    for (var i = 0; i < tbody.rows.length; i++) {
        var r = tbody.rows[i];
        var rowTid = r.getAttribute("data-tid");
        r.classList.toggle("bv-selected", S.selected != null && rowTid !== null &&
                            parseInt(rowTid, 10) === S.selected);
    }
}

function applyProfilerThread() {
    var sel = $("profiler-thread");
    if (!sel) return;
    var target = null;
    for (var i = 0; i < sel.options.length; i++) {
        var o = sel.options[i];
        if (S.selected != null ? o.value === String(S.selected) : o.value === "0") { target = o; break; }
    }
    if (target && sel.value !== target.value) {
        sel.value = target.value;
        sel.dispatchEvent(new Event("change"));
    }
}

/* ── Command palette ──────────────────────────────────────────────── */

function commands() {
    var halted = S.dbg === "stopped";
    var c = [];
    WS_ORDER.forEach(function (k) {
        c.push({ grp: "Workspace", title: WORKSPACES[k].title, key: WORKSPACES[k].key,
                 run: function () { applyWorkspace(k, { manual: true }); } });
    });
    PANEL_ORDER.forEach(function (p) {
        c.push({ grp: "Open", title: PANELS[p].title, run: function () { openPanel(p); } });
    });
    c.push({ grp: "Run", title: "Pause", key: "F6", off: !canDebug() || halted, run: function () { dbg(window.DBG_CMD_PAUSE); } });
    c.push({ grp: "Run", title: "Continue", key: "F5", off: !halted, run: function () { dbg(window.DBG_CMD_CONTINUE); } });
    c.push({ grp: "Run", title: "Step over", key: "F10", off: !halted, run: function () { dbg(window.DBG_CMD_STEP_OVER); } });
    c.push({ grp: "Run", title: "Step into", key: "F11", off: !halted, run: function () { dbg(window.DBG_CMD_STEP_INTO); } });
    c.push({ grp: "Run", title: "Step out", key: "Shift+F11", off: !halted, run: function () { dbg(window.DBG_CMD_STEP_OUT); } });
    c.push({ grp: "Run", title: "Reset board", off: !canDebug(), run: resetBoard });
    S.threads.forEach(function (t) {
        c.push({ grp: "Thread", title: "Select " + t.name, run: function () { selectThread(t.tid); } });
    });
    if (S.selected) c.push({ grp: "Thread", title: "Clear selection", key: "Esc", run: function () { selectThread(null); } });
    [["auto", "Auto (whole-number scale)"], ["fit", "Fit"], ["1", "1\u00d7"], ["2", "2\u00d7"]].forEach(function (s) {
        c.push({ grp: "Display", title: "Scale: " + s[1], run: function () { setScale(s[0]); } });
    });
    c.push({ grp: "View", title: S.prefs.focus ? "Show inspector" : "Board only", key: "F", run: toggleFocus });
    c.push({ grp: "View", title: wsState().b ? "Close second pane" : "Split inspector", run: toggleSplit });
    c.push({ grp: "View", title: "Reset this workspace's layout", run: resetWorkspace });
    c.push({ grp: "Console", title: "Focus shell input", key: "`", run: focusShell });
    c.push({ grp: "Console", title: "Show only warnings and errors", run: function () { setConsoleLevel("w"); openPanel("console"); } });
    c.push({ grp: "Console", title: "Show all lines", run: function () { setConsoleLevel("all"); } });
    c.push({ grp: "Console", title: "Clear", run: clearConsole });
    c.push({ grp: "Settings", title: (S.prefs.autoDebug ? "Don't switch" : "Switch") + " to Debug when the target halts",
             run: function () { S.prefs.autoDebug = !S.prefs.autoDebug; savePrefs(); } });
    return c;
}

var PAL = { open: false, items: [], idx: 0 };

function bindPalette() {
    var box = $("bv-palette"), input = $("bv-palette-input");
    box.addEventListener("mousedown", function (e) { if (e.target === box) closePalette(); });
    input.addEventListener("input", function () { PAL.idx = 0; renderPalette(); });
    input.addEventListener("keydown", function (e) {
        if (e.key === "ArrowDown") { PAL.idx = Math.min(PAL.items.length - 1, PAL.idx + 1); renderPalette(); e.preventDefault(); }
        else if (e.key === "ArrowUp") { PAL.idx = Math.max(0, PAL.idx - 1); renderPalette(); e.preventDefault(); }
        else if (e.key === "Enter") { runPalette(PAL.idx); e.preventDefault(); }
        else if (e.key === "Escape") { closePalette(); e.preventDefault(); }
    });
}

function openPalette() {
    PAL.open = true;
    PAL.idx = 0;
    $("bv-palette").hidden = false;
    var input = $("bv-palette-input");
    input.value = "";
    renderPalette();
    input.focus();
}

function closePalette() {
    PAL.open = false;
    $("bv-palette").hidden = true;
}

/* Every query word must appear; earlier matches rank higher. */
function renderPalette() {
    var q = $("bv-palette-input").value.toLowerCase().trim().split(/\s+/).filter(Boolean);
    var all = commands();
    var scored = [];
    all.forEach(function (c) {
        var text = (c.grp + " " + c.title).toLowerCase();
        var score = 0;
        for (var i = 0; i < q.length; i++) {
            var at = text.indexOf(q[i]);
            if (at < 0) return;
            score += at;
        }
        scored.push({ c: c, s: score + (c.off ? 1000 : 0) });
    });
    if (q.length) scored.sort(function (a, b) { return a.s - b.s; });
    PAL.items = scored.map(function (x) { return x.c; });
    var ul = $("bv-palette-list");
    ul.innerHTML = "";
    PAL.items.forEach(function (c, i) {
        var li = el("li", (i === PAL.idx ? "active" : "") + (c.off ? " disabled" : ""),
            "<span class='grp'>" + esc(c.grp) + "</span><span>" + esc(c.title) + "</span>" +
            (c.off ? "<span class='key'>unavailable now</span>"
                   : c.key ? "<span class='key'>" + esc(c.key) + "</span>" : ""));
        li.addEventListener("mousemove", function () {
            if (PAL.idx !== i) { PAL.idx = i; renderPalette(); }
        });
        li.addEventListener("click", function () { runPalette(i); });
        ul.appendChild(li);
    });
    var act = ul.children[PAL.idx];
    if (act) act.scrollIntoView({ block: "nearest" });
}

function runPalette(i) {
    var c = PAL.items[i];
    if (!c || c.off) return;
    closePalette();
    c.run();
}

/* ── Keyboard ─────────────────────────────────────────────────────── */

function bindKeys() {
    document.addEventListener("keydown", function (e) {
        var k = e.key;
        if ((e.ctrlKey || e.metaKey) && (k === "k" || k === "K" || (e.shiftKey && (k === "p" || k === "P")))) {
            e.preventDefault();
            if (PAL.open) closePalette(); else openPalette();
            return;
        }
        if (PAL.open) return;
        var t = e.target;
        var typing = t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.tagName === "SELECT" || t.isContentEditable);
        if (typing) {
            if (k === "Escape" && t.blur) t.blur();
            return;
        }
        if (e.ctrlKey || e.metaKey || e.altKey) return;
        for (var i = 0; i < WS_ORDER.length; i++) {
            if (k === WORKSPACES[WS_ORDER[i]].key) { applyWorkspace(WS_ORDER[i], { manual: true }); e.preventDefault(); return; }
        }
        if (k === "f" || k === "F") { toggleFocus(); e.preventDefault(); }
        else if (k === "`") { focusShell(); e.preventDefault(); }
        else if (k === "/") {
            openPanel("console");
            var s = $("bv-con-search");
            if (s) { s.focus(); e.preventDefault(); }
        }
        else if (k === "Escape" && S.selected != null) selectThread(null);
    });
}

/* ── Go ───────────────────────────────────────────────────────────── */

if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot);
else boot();

})();
