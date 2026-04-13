// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

// LVGL Gallery (Zig) — one widget per page with top nav bar.
// 22 pages — one for each widget type in the bindings.

const std = @import("std");
const ove = @import("ove");
const Thread = ove.Thread;
const Timer = ove.Timer;
const prio = ove.thread.prio;

const has_lvgl = @hasDecl(ove.ffi, "ove_lvgl_init");
const lvgl = if (has_lvgl) ove.lvgl else undefined;

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

var tick_counter: std.atomic.Value(i32) = std.atomic.Value(i32).init(0);

var bar_w: if (has_lvgl) lvgl.Bar else void = if (has_lvgl) undefined else {};
var arc_w: if (has_lvgl) lvgl.Arc else void = if (has_lvgl) undefined else {};
var led_w: if (has_lvgl) lvgl.Led else void = if (has_lvgl) undefined else {};
var chart_w: if (has_lvgl) lvgl.Chart else void = if (has_lvgl) undefined else {};
var series_w: if (has_lvgl) lvgl.Series else void = if (has_lvgl) undefined else {};
var counter_state: if (has_lvgl) lvgl.State(i32) else void = if (has_lvgl) undefined else {};

var canvas_buf: [64 * 64 * 4]u8 = undefined;
var ui_timer: Timer = undefined;

var g_page: i32 = 0;
var g_content: ?*ove.ffi.lv_obj_t = null;
var g_title: ?*ove.ffi.lv_obj_t = null;

var bar_live: bool = false;
var arc_live: bool = false;
var led_live: bool = false;
var chart_live: bool = false;

extern const badge: anyopaque;

// ---------------------------------------------------------------------------
// UI timer
// ---------------------------------------------------------------------------

fn uiTimerCallback() void {
    if (!has_lvgl) return;
    const tick = tick_counter.fetchAdd(1, .release) + 1;
    const guard = lvgl.lock();
    defer guard.deinit();

    if (bar_live) _ = bar_w.value(@intCast(@mod(tick, 101)));
    if (arc_live) _ = arc_w.value(@intCast(@mod(tick, 101)));
    if (led_live and @mod(tick, 10) == 0) _ = led_w.toggle();
    if (chart_live) _ = series_w.nextValue(@intCast(@mod(tick * 3, 100)));
    counter_state.set(tick);
}

// ---------------------------------------------------------------------------
// Event callbacks
// ---------------------------------------------------------------------------

fn onPrev(_: ?*ove.ffi.lv_event_t) callconv(.c) void {
    g_page = @mod(g_page + N_PAGES - 1, N_PAGES);
    rebuildPage();
}

fn onNext(_: ?*ove.ffi.lv_event_t) callconv(.c) void {
    g_page = @mod(g_page + 1, N_PAGES);
    rebuildPage();
}

fn onAlertClick(_: ?*ove.ffi.lv_event_t) callconv(.c) void {
    const mbox = lvgl.Msgbox.createModal();
    _ = mbox.addTitle("Hello").addText("Zig msgbox from gallery.").addCloseButton();
}

// ---------------------------------------------------------------------------
// Page builders
// ---------------------------------------------------------------------------

fn pLabel(c: lvgl.Obj) void {
    _ = lvgl.Label.create(c).text("Hello, oveRTOS!").font(lvgl.fontMontserrat32()).color(lvgl.colorWhite()).center();
    const lbl2 = lvgl.Label.create(c).color(lvgl.colorHex(0x888888)).font(lvgl.fontMontserrat14());
    _ = lvgl.labelBindText(lbl2, &counter_state, "Tick: %d");
    ove.ffi.lv_obj_align(lbl2.obj, ove.ffi.LV_ALIGN_BOTTOM_MID, 0, -16);
}

fn pButton(c: lvgl.Obj) void {
    const btn = lvgl.Button.create(c).size(160, 48).toggleMode(true);
    _ = lvgl.Label.create(btn).text("Toggle me").center();
    _ = btn.center();
}

fn pSwitch(c: lvgl.Obj) void { _ = lvgl.Switch.create(c).checked(true).center(); }
fn pCheckbox(c: lvgl.Obj) void { _ = lvgl.Checkbox.create(c).text("Enable option").checked(true).textColor(lvgl.colorWhite()).center(); }

fn pBar(c: lvgl.Obj) void {
    bar_w = lvgl.Bar.create(c).size(300, 20).range(0, 100).indicatorColor(lvgl.paletteMain(ove.ffi.LV_PALETTE_BLUE)).radius(10).center();
    bar_live = true;
}

fn pSlider(c: lvgl.Obj) void { _ = lvgl.Slider.create(c).size(300, 20).range(0, 100).value(50).indicatorColor(lvgl.paletteMain(ove.ffi.LV_PALETTE_GREEN)).center(); }

fn pArc(c: lvgl.Obj) void {
    arc_w = lvgl.Arc.create(c).size(120, 120).range(0, 100).value(40).indicatorColor(lvgl.paletteMain(ove.ffi.LV_PALETTE_ORANGE)).center();
    arc_live = true;
}

fn pSpinner(c: lvgl.Obj) void { _ = lvgl.Spinner.create(c).size(80, 80).animParams(1000, 60).center(); }

fn pLed(c: lvgl.Obj) void {
    led_w = lvgl.Led.create(c).size(60, 60).color(lvgl.paletteMain(ove.ffi.LV_PALETTE_RED)).center();
    led_live = true;
}

fn pDropdown(c: lvgl.Obj) void { _ = lvgl.Dropdown.create(c).optionsStatic("Red\nGreen\nBlue\nYellow").selected(2).width(200).center(); }
fn pRoller(c: lvgl.Obj) void { _ = lvgl.Roller.create(c).options("Mon\nTue\nWed\nThu\nFri\nSat\nSun", lvgl.ROLLER_MODE_NORMAL).visibleRowCount(4).width(140).center(); }
fn pSpinbox(c: lvgl.Obj) void { _ = lvgl.Spinbox.create(c).width(200).digitFormat(4, 2).range(-9999, 9999).step(1).value(42).center(); }

fn pTextarea(c: lvgl.Obj) void {
    ove.ffi.lv_obj_set_flex_flow(c.obj, ove.ffi.LV_FLEX_FLOW_COLUMN);
    ove.ffi.lv_obj_set_style_pad_row(c.obj, 8, 0);
    const ta = lvgl.Textarea.create(c).oneLine(true).placeholder("Type here...").maxLength(40).width(400);
    _ = lvgl.Keyboard.create(c).size(400, 140).attach(ta);
}

fn pChart(c: lvgl.Obj) void {
    chart_w = lvgl.Chart.create(c).size(400, 190).chartType(lvgl.CHART_TYPE_LINE).pointCount(60).range(lvgl.CHART_AXIS_PRIMARY_Y, 0, 100).updateMode(lvgl.CHART_UPDATE_MODE_SHIFT).divLineCount(5, 6).center();
    series_w = chart_w.addSeries(lvgl.colorHex(0x00BCD4), lvgl.CHART_AXIS_PRIMARY_Y);
    chart_live = true;
}

fn pTable(c: lvgl.Obj) void {
    const t = lvgl.Table.create(c).columnCount(2).rowCount(4);
    _ = t.columnWidth(0, 120).columnWidth(1, 120)
        .cellValue(0, 0, "Key").cellValue(0, 1, "Value")
        .cellValue(1, 0, "Language").cellValue(1, 1, "Zig")
        .cellValue(2, 0, "LVGL").cellValue(2, 1, "9.2")
        .cellValue(3, 0, "RTOS").cellValue(3, 1, "oveRTOS");
    _ = t.center();
}

fn pList(c: lvgl.Obj) void {
    const l = lvgl.List.create(c).size(240, 160);
    _ = l.addText("Navigation");
    _ = l.addButton(null, "Settings");
    _ = l.addButton(null, "About");
    _ = l.addButton(null, "Help");
    _ = l.addButton(null, "Quit");
    _ = l.center();
}

fn pImage(c: lvgl.Obj) void { _ = lvgl.Image.create(c).src(&badge).center(); }

fn pCanvas(c: lvgl.Obj) void {
    const canvas = lvgl.Canvas.create(c).size(64, 64);
    _ = canvas.buffer(&canvas_buf, 64, 64, ove.ffi.LV_COLOR_FORMAT_XRGB8888);
    _ = canvas.fillBg(lvgl.colorHex(0x202020), 255);
    var y: i32 = 0;
    while (y < 64) : (y += 1) { var x: i32 = 0; while (x < 64) : (x += 1) {
        _ = canvas.setPixel(x, y, lvgl.colorMake(@intCast(@mod(x * 4, 256)), @intCast(@mod(y * 4, 256)), 128));
    }}
    _ = canvas.center();
}

fn pCalendar(c: lvgl.Obj) void { _ = lvgl.Calendar.create(c).size(240, 240).today(2026, 4, 13).showed(2026, 4).center(); }

fn pMsgbox(c: lvgl.Obj) void {
    const btn = lvgl.Button.create(c).size(200, 48);
    _ = lvgl.Label.create(btn).text("Show Msgbox").center();
    _ = ove.ffi.lv_obj_add_event_cb(btn.obj, onAlertClick, ove.ffi.LV_EVENT_CLICKED, null);
    _ = btn.center();
}

fn pBox(c: lvgl.Obj) void {
    _ = lvgl.Box.create(c).size(200, 120).bgColor(lvgl.colorHex(0x1A237E)).bgOpa(255)
        .borderColor(lvgl.colorWhite()).borderWidth(2).radius(16).padAll(16).center();
}

fn pTabview(c: lvgl.Obj) void {
    const tv = lvgl.Tabview.create(c).size(380, 180).center();
    _ = tv.setTabBarSize(32);
    const t1 = tv.addTab("Tab A");
    _ = lvgl.Label.create(t1).text("Content A").center();
    const t2 = tv.addTab("Tab B");
    _ = lvgl.Label.create(t2).text("Content B").center();
}

// ---------------------------------------------------------------------------
// Page directory
// ---------------------------------------------------------------------------

const PageEntry = struct { name: [*:0]const u8, build: *const fn (lvgl.Obj) void };

const pages = [_]PageEntry{
    .{ .name = "Label",    .build = pLabel    },
    .{ .name = "Button",   .build = pButton   },
    .{ .name = "Switch",   .build = pSwitch   },
    .{ .name = "Checkbox", .build = pCheckbox },
    .{ .name = "Bar",      .build = pBar      },
    .{ .name = "Slider",   .build = pSlider   },
    .{ .name = "Arc",      .build = pArc      },
    .{ .name = "Spinner",  .build = pSpinner  },
    .{ .name = "Led",      .build = pLed      },
    .{ .name = "Dropdown", .build = pDropdown },
    .{ .name = "Roller",   .build = pRoller   },
    .{ .name = "Spinbox",  .build = pSpinbox  },
    .{ .name = "Text+Kbd", .build = pTextarea },
    .{ .name = "Chart",    .build = pChart    },
    .{ .name = "Table",    .build = pTable    },
    .{ .name = "List",     .build = pList     },
    .{ .name = "Image",    .build = pImage    },
    .{ .name = "Canvas",   .build = pCanvas   },
    .{ .name = "Calendar", .build = pCalendar },
    .{ .name = "Msgbox",   .build = pMsgbox   },
    .{ .name = "Box",      .build = pBox      },
    .{ .name = "Tabview",  .build = pTabview  },
};

const N_PAGES: i32 = @intCast(pages.len);

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

fn clearLiveWidgets() void {
    bar_live = false;
    arc_live = false;
    led_live = false;
    chart_live = false;
}

fn rebuildPage() void {
    if (!has_lvgl) return;
    clearLiveWidgets();

    if (g_content) |c| ove.ffi.lv_obj_clean(c);

    const page: usize = @intCast(g_page);
    if (g_content) |c| pages[page].build(.{ .obj = c });

    if (g_title) |t| {
        var buf: [48]u8 = undefined;
        _ = std.fmt.bufPrint(&buf, "{s} ({d}/{d})\x00", .{
            pages[page].name, g_page + 1, N_PAGES,
        }) catch {};
        ove.ffi.lv_label_set_text(t, @ptrCast(&buf));
    }
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

fn createUi() void {
    if (!has_lvgl) return;

    const screen = lvgl.screenActive();
    ove.ffi.lv_obj_set_style_bg_color(screen.obj, ove.ffi.lv_color_black(), 0);
    ove.ffi.lv_obj_set_style_bg_opa(screen.obj, 255, 0);
    ove.ffi.lv_obj_set_flex_flow(screen.obj, ove.ffi.LV_FLEX_FLOW_COLUMN);
    ove.ffi.lv_obj_set_style_pad_all(screen.obj, 0, 0);
    ove.ffi.lv_obj_set_style_pad_row(screen.obj, 0, 0);

    // Nav bar
    const nav = lvgl.Box.create(screen).size(480, 40).bgColor(lvgl.colorHex(0x1A237E)).bgOpa(255).radius(0);
    ove.ffi.lv_obj_set_flex_flow(nav.obj, ove.ffi.LV_FLEX_FLOW_ROW);
    ove.ffi.lv_obj_set_flex_align(nav.obj, 3, 2, 2); // SPACE_BETWEEN, CENTER, CENTER
    ove.ffi.lv_obj_set_style_pad_left(nav.obj, 4, 0);
    ove.ffi.lv_obj_set_style_pad_right(nav.obj, 4, 0);

    const prev = lvgl.Button.create(nav).size(40, 32);
    _ = lvgl.Label.create(prev).text("<").center();
    _ = ove.ffi.lv_obj_add_event_cb(prev.obj, onPrev, ove.ffi.LV_EVENT_CLICKED, null);

    const title = lvgl.Label.create(nav).text("").color(lvgl.colorWhite()).font(lvgl.fontMontserrat14());
    ove.ffi.lv_obj_set_flex_grow(title.obj, 1);
    ove.ffi.lv_obj_set_style_text_align(title.obj, 2, 0); // CENTER
    g_title = title.obj;

    const next = lvgl.Button.create(nav).size(40, 32);
    _ = lvgl.Label.create(next).text(">").center();
    _ = ove.ffi.lv_obj_add_event_cb(next.obj, onNext, ove.ffi.LV_EVENT_CLICKED, null);

    // Content area
    const content = lvgl.Box.create(screen);
    ove.ffi.lv_obj_set_size(content.obj, 480, 232);
    ove.ffi.lv_obj_set_flex_grow(content.obj, 1);
    ove.ffi.lv_obj_set_style_bg_opa(content.obj, 0, 0);
    ove.ffi.lv_obj_set_style_border_width(content.obj, 0, 0);
    ove.ffi.lv_obj_set_style_pad_all(content.obj, 8, 0);
    g_content = content.obj;

    counter_state = lvgl.State(i32).init_(0);

    rebuildPage();
}

// ---------------------------------------------------------------------------
// Graphics thread
// ---------------------------------------------------------------------------

fn graphicsEntry() void {
    if (!has_lvgl) return;
    var last_us: u64 = 0;
    _ = ove.ffi.ove_time_get_us(&last_us);
    while (true) {
        var now_us: u64 = 0;
        _ = ove.ffi.ove_time_get_us(&now_us);
        const elapsed_ms: u32 = @intCast((now_us - last_us) / 1000);
        last_us = now_us;
        { const guard = lvgl.lock(); defer guard.deinit(); lvgl.tick(elapsed_ms); lvgl.handler(); }
        Thread.sleepMs(30);
    }
}

// ---------------------------------------------------------------------------
// App entry
// ---------------------------------------------------------------------------

fn appMain() void {
    ove.log.inf("LVGL gallery (Zig): init", .{});

    if (has_lvgl) {
        _ = Thread.spawn("graphics", graphicsEntry, prio.high, 4096) catch { ove.log.err("Failed to spawn graphics", .{}); return; };
        ui_timer = Timer.create(uiTimerCallback, 100, false) catch { ove.log.err("Timer create fail", .{}); return; };
        lvgl.init() catch { ove.log.err("LVGL init fail", .{}); return; };
        { const guard = lvgl.lock(); defer guard.deinit(); createUi(); }
        ove.log.inf("LVGL widgets created", .{});
        ui_timer.start() catch { ove.log.err("Timer start fail", .{}); return; };
    }

    ove.log.inf("LVGL gallery (Zig): ready", .{});
    ove.run();
}

comptime { ove.exportMain(appMain); }
