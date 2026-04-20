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

const lvgl = ove.lvgl;

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

var tick_counter: std.atomic.Value(i32) = std.atomic.Value(i32).init(0);

var bar_w: lvgl.Bar = undefined;
var arc_w: lvgl.Arc = undefined;
var led_w: lvgl.Led = undefined;
var chart_w: lvgl.Chart = undefined;
var series_w: lvgl.Series = undefined;
var counter_state: lvgl.State(i32) = undefined;

var canvas_buf: [64 * 64 * 4]u8 = undefined;
var ui_timer: Timer = undefined;

var g_page: i32 = 0;
var g_content: ?lvgl.Obj = null;
var g_title: ?lvgl.Label = null;

var bar_live: bool = false;
var arc_live: bool = false;
var led_live: bool = false;
var chart_live: bool = false;

extern const badge: lvgl.ImageDsc;

// ---------------------------------------------------------------------------
// UI timer
// ---------------------------------------------------------------------------

fn uiTimerCallback() void {
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
// Event handlers — safe Zig fns; the binding generates the C trampolines
// ---------------------------------------------------------------------------

fn onPrev(_: lvgl.EventCtx) void {
    g_page = @mod(g_page + N_PAGES - 1, N_PAGES);
    rebuildPage();
}

fn onNext(_: lvgl.EventCtx) void {
    g_page = @mod(g_page + 1, N_PAGES);
    rebuildPage();
}

fn onAlertClick(_: lvgl.EventCtx) void {
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
    _ = lbl2.alignTo(lvgl.ALIGN_BOTTOM_MID, 0, -16);
}

fn pButton(c: lvgl.Obj) void {
    const btn = lvgl.Button.create(c).size(160, 48).toggleMode(true);
    _ = lvgl.Label.create(btn).text("Toggle me").center();
    _ = btn.center();
}

fn pSwitch(c: lvgl.Obj) void {
    _ = lvgl.Switch.create(c).checked(true).center();
}
fn pCheckbox(c: lvgl.Obj) void {
    _ = lvgl.Checkbox.create(c).text("Enable option").checked(true).textColor(lvgl.colorWhite()).center();
}

fn pBar(c: lvgl.Obj) void {
    bar_w = lvgl.Bar.create(c).size(300, 20).range(0, 100).indicatorColor(lvgl.paletteMain(lvgl.PALETTE_BLUE)).radius(10).center();
    bar_live = true;
}

fn pSlider(c: lvgl.Obj) void {
    _ = lvgl.Slider.create(c).size(300, 20).range(0, 100).value(50).indicatorColor(lvgl.paletteMain(lvgl.PALETTE_GREEN)).center();
}

fn pArc(c: lvgl.Obj) void {
    arc_w = lvgl.Arc.create(c).size(120, 120).range(0, 100).value(40).indicatorColor(lvgl.paletteMain(lvgl.PALETTE_ORANGE)).center();
    arc_live = true;
}

fn pSpinner(c: lvgl.Obj) void {
    _ = lvgl.Spinner.create(c).size(80, 80).animParams(1000, 60).center();
}

fn pLed(c: lvgl.Obj) void {
    led_w = lvgl.Led.create(c).size(60, 60).color(lvgl.paletteMain(lvgl.PALETTE_RED)).center();
    led_live = true;
}

fn pDropdown(c: lvgl.Obj) void {
    _ = lvgl.Dropdown.create(c).optionsStatic("Red\nGreen\nBlue\nYellow").selected(2).width(200).center();
}
fn pRoller(c: lvgl.Obj) void {
    _ = lvgl.Roller.create(c).options("Mon\nTue\nWed\nThu\nFri\nSat\nSun", lvgl.ROLLER_MODE_NORMAL).visibleRowCount(4).width(140).center();
}
fn pSpinbox(c: lvgl.Obj) void {
    _ = lvgl.Spinbox.create(c).width(200).digitFormat(4, 2).range(-9999, 9999).step(1).value(42).center();
}

fn pTextarea(c: lvgl.Obj) void {
    _ = c.flexFlow(lvgl.FLEX_FLOW_COLUMN).padRow(8);
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

fn pImage(c: lvgl.Obj) void {
    _ = lvgl.imageSource(lvgl.Image.create(c), lvgl.ImageSrc.fromDsc(&badge)).center();
}

fn pCanvas(c: lvgl.Obj) void {
    const canvas = lvgl.Canvas.create(c).size(64, 64);
    _ = canvas.buffer(&canvas_buf, 64, 64, ove.ffi.LV_COLOR_FORMAT_XRGB8888);
    _ = canvas.fillBg(lvgl.colorHex(0x202020), 255);
    var y: i32 = 0;
    while (y < 64) : (y += 1) {
        var x: i32 = 0;
        while (x < 64) : (x += 1) {
            _ = canvas.setPixel(x, y, lvgl.colorMake(@intCast(@mod(x * 4, 256)), @intCast(@mod(y * 4, 256)), 128));
        }
    }
    _ = canvas.center();
}

fn pCalendar(c: lvgl.Obj) void {
    _ = lvgl.Calendar.create(c).size(240, 240).today(2026, 4, 13).showed(2026, 4).center();
}

fn pMsgbox(c: lvgl.Obj) void {
    const btn = lvgl.Button.create(c).size(200, 48);
    _ = lvgl.Label.create(btn).text("Show Msgbox").center();
    _ = btn.onClicked(onAlertClick);
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
    .{ .name = "Label", .build = pLabel },
    .{ .name = "Button", .build = pButton },
    .{ .name = "Switch", .build = pSwitch },
    .{ .name = "Checkbox", .build = pCheckbox },
    .{ .name = "Bar", .build = pBar },
    .{ .name = "Slider", .build = pSlider },
    .{ .name = "Arc", .build = pArc },
    .{ .name = "Spinner", .build = pSpinner },
    .{ .name = "Led", .build = pLed },
    .{ .name = "Dropdown", .build = pDropdown },
    .{ .name = "Roller", .build = pRoller },
    .{ .name = "Spinbox", .build = pSpinbox },
    .{ .name = "Text+Kbd", .build = pTextarea },
    .{ .name = "Chart", .build = pChart },
    .{ .name = "Table", .build = pTable },
    .{ .name = "List", .build = pList },
    .{ .name = "Image", .build = pImage },
    .{ .name = "Canvas", .build = pCanvas },
    .{ .name = "Calendar", .build = pCalendar },
    .{ .name = "Msgbox", .build = pMsgbox },
    .{ .name = "Box", .build = pBox },
    .{ .name = "Tabview", .build = pTabview },
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
    clearLiveWidgets();

    if (g_content) |c| c.clean();

    const page: usize = @intCast(g_page);
    if (g_content) |c| pages[page].build(c);

    if (g_title) |t| {
        var buf: [48]u8 = undefined;
        const slice = std.fmt.bufPrintZ(&buf, "{s} ({d}/{d})", .{
            pages[page].name, g_page + 1, N_PAGES,
        }) catch return;
        _ = t.text(slice.ptr);
    }
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

fn createUi() void {
    const screen = lvgl.screenActive();
    _ = screen.bgColor(lvgl.colorBlack())
        .bgOpa(lvgl.OPA_COVER)
        .flexFlow(lvgl.FLEX_FLOW_COLUMN)
        .padAll(0)
        .padRow(0);

    // Nav bar
    const nav = lvgl.Box.create(screen).size(480, 40)
        .bgColor(lvgl.colorHex(0x1A237E)).bgOpa(255).radius(0)
        .flexFlow(lvgl.FLEX_FLOW_ROW)
        .flexAlign(lvgl.FLEX_ALIGN_SPACE_BETWEEN, lvgl.FLEX_ALIGN_CENTER, lvgl.FLEX_ALIGN_CENTER)
        .padLeft(4)
        .padRight(4);

    const prev = lvgl.Button.create(nav).size(40, 32);
    _ = lvgl.Label.create(prev).text("<").center();
    _ = prev.onClicked(onPrev);

    const title = lvgl.Label.create(nav).text("").color(lvgl.colorWhite()).font(lvgl.fontMontserrat14())
        .flexGrow(1)
        .textAlign(2, lvgl.PART_MAIN); // CENTER
    g_title = title;

    const next = lvgl.Button.create(nav).size(40, 32);
    _ = lvgl.Label.create(next).text(">").center();
    _ = next.onClicked(onNext);

    // Content area
    const content = lvgl.Box.create(screen).size(480, 232)
        .flexGrow(1)
        .bgOpa(0)
        .borderWidth(0)
        .padAll(8);
    g_content = .{ .obj = content.obj };

    counter_state = lvgl.State(i32).init_(0);

    rebuildPage();
}

// ---------------------------------------------------------------------------
// Graphics thread
// ---------------------------------------------------------------------------

fn graphicsEntry() void {
    var last_us: u64 = 0;
    _ = ove.ffi.ove_time_get_us(&last_us);
    while (true) {
        var now_us: u64 = 0;
        _ = ove.ffi.ove_time_get_us(&now_us);
        const elapsed_ms: u32 = @intCast((now_us - last_us) / 1000);
        last_us = now_us;
        {
            const guard = lvgl.lock();
            defer guard.deinit();
            lvgl.tick(elapsed_ms);
            lvgl.handler();
        }
        Thread.sleepMs(30);
    }
}

// ---------------------------------------------------------------------------
// App entry
// ---------------------------------------------------------------------------

fn appMain() void {
    ove.log.inf("LVGL gallery (Zig): init", .{});

    _ = Thread.spawn("graphics", graphicsEntry, prio.high, 4096) catch {
        ove.log.err("Failed to spawn graphics", .{});
        return;
    };
    ui_timer = Timer.create(uiTimerCallback, 100, false) catch {
        ove.log.err("Timer create fail", .{});
        return;
    };
    lvgl.init() catch {
        ove.log.err("LVGL init fail", .{});
        return;
    };
    {
        const guard = lvgl.lock();
        defer guard.deinit();
        createUi();
    }
    ove.log.inf("LVGL widgets created", .{});
    ui_timer.start() catch {
        ove.log.err("Timer start fail", .{});
        return;
    };

    ove.log.inf("LVGL gallery (Zig): ready", .{});
    ove.run();
}

comptime {
    ove.exportMain(appMain);
}
