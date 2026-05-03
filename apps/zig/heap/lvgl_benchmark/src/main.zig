// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

// LVGL Rendering Benchmark (Zig)
//
// Reimplements LVGL's benchmark demo scenes using the Zig binding API
// through oveRTOS. 15 rendering scenes stress-test widgets, animations,
// layout, images, and compositing. A summary table shows per-scene
// FPS / CPU / render / flush metrics.
//
// Style note: scene callbacks intentionally mix the safe `lvgl.*` wrapper
// (for screen/parent lookup + occasional chained setup) with raw `c.lv_*`
// calls for child widgets. This matches the C reference benchmark's call
// sequence exactly, preserving apples-to-apples comparison. Prefer raw
// `c.lv_*` in hot paths; reserve wrappers for top-level screen access.

const std = @import("std");
const ove = @import("ove");
const prio = ove.thread.prio;

const lvgl = ove.lvgl;
const c = ove.ffi;

// lv_anim_enable_t is bool on Zephyr but c_uint on other platforms.
// Detect at comptime by inspecting the 3rd parameter of lv_obj_scroll_to_y.
const LvAnimEnable = @typeInfo(@TypeOf(c.lv_obj_scroll_to_y)).@"fn".params[2].type.?;
fn animOff() LvAnimEnable {
    return if (LvAnimEnable == bool) false else 0;
}
fn animOn() LvAnimEnable {
    return if (LvAnimEnable == bool) true else 1;
}

// ---------------------------------------------------------------------------
// External C assets (linked from the C benchmark app)
// ---------------------------------------------------------------------------

extern const img_benchmark_lvgl_logo_rgb: anyopaque;
extern const img_benchmark_lvgl_logo_argb: anyopaque;
extern const img_benchmark_avatar: anyopaque;

// ---------------------------------------------------------------------------
// Shared C performance helper
// ---------------------------------------------------------------------------

const BenchPerfMetrics = extern struct {
    fps: u32,
    cpu: u32,
    render_avg_time: u32,
    flush_avg_time: u32,
};

extern fn benchmark_get_perf_subject() ?*c.lv_subject_t;
extern fn benchmark_extract_perf_metrics(info: ?*const anyopaque, out: *BenchPerfMetrics) void;
extern fn benchmark_anim_color(obj: *c.lv_obj_t, cb: c.lv_anim_exec_xcb_t) void;
extern fn benchmark_anim_shake(obj: *c.lv_obj_t, cb: c.lv_anim_exec_xcb_t, y_max: i32, t1: u32, t2: u32) void;
extern fn benchmark_anim_scroll(obj: *c.lv_obj_t, cb: c.lv_anim_exec_xcb_t, y_max: i32, t: u32) void;
extern fn benchmark_anim_arc(obj: *c.lv_obj_t, cb: c.lv_anim_exec_xcb_t, t1: u32, t2: u32) void;
extern fn benchmark_table_draw_task_cb(e: ?*c.lv_event_t) void;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Equivalent to C's lv_layer_top() macro.
/// Zig's cImport cannot translate macros, so call the underlying functions.
fn layerTop() ?*c.lv_obj_t {
    return c.lv_display_get_layer_top(c.lv_display_get_default());
}

/// Equivalent to C's lv_dpx(n) macro: DPI-scaled pixel value.
fn lvDpx(n: i32) i32 {
    const dpi: i32 = @intCast(lvgl.display.dpi());
    if (dpi <= 0) return n;
    return @divTrunc(n * dpi + 80, 160);
}

/// Equivalent to C's lv_anim_speed(v) macro: 500_000_000 / v.
fn lvAnimSpeed(speed: u32) u32 {
    if (speed == 0) return 0;
    return @intCast(@as(u64, 500_000_000) / @as(u64, speed));
}

// ---------------------------------------------------------------------------
// PRNG — deterministic sequence matching the C reference
// ---------------------------------------------------------------------------

const rnd_map = [64]u32{
    0xbd13204f, 0x67d8167f, 0x20211c99, 0xb0a7cc05,
    0x06d5c703, 0xeafb01a7, 0xd0473b5c, 0xc999aaa2,
    0x86f9d5d9, 0x294bdb29, 0x12a3c207, 0x78914d14,
    0x10a30006, 0x6134c7db, 0x194443af, 0x142d1099,
    0x376292d5, 0x20f433c5, 0x074d2a59, 0x4e74c293,
    0x072a0810, 0xdd0f136d, 0x5cca6dbc, 0x623bfdd8,
    0xb645eb2f, 0xbe50894a, 0xc9b56717, 0xe0f912c8,
    0x4f6b5e24, 0xfe44b128, 0xe12d57a8, 0x9b15c9cc,
    0xab2ae1d3, 0xb4dc5074, 0x67d457c8, 0x8e46b00c,
    0xa29a1871, 0xcee40332, 0x80f93aa1, 0x85286096,
    0x09bd6b49, 0x95072088, 0x2093924b, 0x6a27328f,
    0xa796079b, 0xc3b488bc, 0xe29bcce0, 0x07048a4c,
    0x7d81bd99, 0x27aacb30, 0x44fc7a0e, 0xa2382241,
    0x8357a17d, 0x97e9c9cc, 0xad10ff52, 0x9923fc5c,
    0x8f2c840a, 0x20356ba2, 0x7997a677, 0x9a7f1800,
    0x35c7562b, 0xd901fe51, 0x8f4e053d, 0xa5b94923,
};

var rnd_act: u32 = 0;

fn rndReset() void {
    rnd_act = 0;
}

fn rndNext(min: i32, max: i32) i32 {
    var mn = min;
    var mx = max;
    if (mn == mx) return mn;
    if (mn > mx) {
        const tmp = mn;
        mn = mx;
        mx = tmp;
    }
    const d: u32 = @intCast(mx - mn);
    const r: i32 = @intCast(rnd_map[rnd_act] % d);
    rnd_act += 1;
    if (rnd_act >= rnd_map.len) rnd_act = 0;
    return r + mn;
}

// ---------------------------------------------------------------------------
// Scene descriptor
// ---------------------------------------------------------------------------

const SceneDsc = struct {
    name: [*:0]const u8,
    create_cb: ?*const fn () void,
    scene_time: u32,
    cpu_avg_usage: u32 = 0,
    fps_avg: u32 = 0,
    render_avg_time: u32 = 0,
    flush_avg_time: u32 = 0,
    measurement_cnt: u32 = 0,
};

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

var scene_act: u32 = 0;

// ---------------------------------------------------------------------------
// perf_ffi — C-ABI animation callbacks + helpers boundary
//
// All callbacks with `callconv(.c)` signatures and their wrapper helpers
// live inside this namespace struct so the audit boundary between "safe
// Zig scene code" and "C-linkage glue" is visible at a glance. Scene
// code calls `perf_ffi.colorAnim(obj)` etc. instead of the raw
// `callconv(.c)` callbacks.
// ---------------------------------------------------------------------------

const perf_ffi = struct {
    pub fn colorAnimCb(var_ptr: ?*anyopaque, v: i32) callconv(.c) void {
        _ = v;
        const obj: *c.lv_obj_t = @ptrCast(@alignCast(var_ptr));
        c.lv_obj_set_style_bg_color(obj, c.lv_color_hex3(@intCast(rndNext(0x00f, 0xff0))), 0);
        c.lv_obj_set_style_text_color(obj, c.lv_color_hex3(@intCast(rndNext(0x00f, 0xff0))), 0);
    }

    pub fn shakeAnimYCb(var_ptr: ?*anyopaque, v: i32) callconv(.c) void {
        const obj: *c.lv_obj_t = @ptrCast(@alignCast(var_ptr));
        c.lv_obj_set_style_translate_y(obj, v, 0);
    }

    pub fn scrollAnimYCb(var_ptr: ?*anyopaque, v: i32) callconv(.c) void {
        const obj: *c.lv_obj_t = @ptrCast(@alignCast(var_ptr));
        c.lv_obj_scroll_to_y(obj, v, animOff());
    }

    pub fn arcAnimCb(var_ptr: ?*anyopaque, v: i32) callconv(.c) void {
        const obj: *c.lv_obj_t = @ptrCast(@alignCast(var_ptr));
        c.lv_arc_set_value(obj, v);
    }

    pub fn colorAnim(obj: *c.lv_obj_t) void {
        benchmark_anim_color(obj, perf_ffi.colorAnimCb);
    }

    pub fn shakeAnim(obj: *c.lv_obj_t, y_max: i32) void {
        const t1: u32 = @intCast(rndNext(300, 3000));
        const t2: u32 = @intCast(rndNext(300, 3000));
        benchmark_anim_shake(obj, perf_ffi.shakeAnimYCb, y_max, t1, t2);
    }

    pub fn scrollAnim(obj: *c.lv_obj_t, y_max: i32) void {
        const t: u32 = lvAnimSpeed(@intCast(lvgl.display.dpi()));
        benchmark_anim_scroll(obj, perf_ffi.scrollAnimYCb, y_max, t);
    }

    pub fn arcAnim(obj: *c.lv_obj_t) void {
        const t1: u32 = @intCast(rndNext(1000, 3000));
        const t2: u32 = @intCast(rndNext(1000, 3000));
        benchmark_anim_arc(obj, perf_ffi.arcAnimCb, t1, t2);
    }
};

// ---------------------------------------------------------------------------
// Card composite widget
// ---------------------------------------------------------------------------

fn cardCreate() *c.lv_obj_t {
    const scr = c.lv_screen_active();
    const panel = c.lv_obj_create(scr);
    c.lv_obj_set_size(panel, 270, 120);
    c.lv_obj_set_style_pad_all(panel, 8, 0);

    var child = c.lv_image_create(panel);
    c.lv_obj_align(child, c.LV_ALIGN_LEFT_MID, 0, 0);
    c.lv_image_set_src(child, &img_benchmark_avatar);

    child = c.lv_label_create(panel);
    c.lv_label_set_text(child, "John Smith");
    c.lv_obj_set_pos(child, 100, 0);

    child = c.lv_label_create(panel);
    c.lv_label_set_text(child, "A DIY enthusiast");
    c.lv_obj_set_pos(child, 100, 30);

    child = c.lv_button_create(panel);
    c.lv_obj_set_pos(child, 100, 50);

    child = c.lv_label_create(child);
    c.lv_label_set_text(child, "Connect");

    return panel.?;
}

// ---------------------------------------------------------------------------
// Scene callbacks
// ---------------------------------------------------------------------------

fn emptyScreenCb() void {
    perf_ffi.colorAnim(c.lv_screen_active().?);
}

fn movingWallpaperCb() void {
    const scr = lvgl.screenActive();
    _ = scr.padAll(0);

    const img = c.lv_image_create(scr.obj).?;
    c.lv_obj_set_size(img, c.lv_pct(150), c.lv_pct(150));
    c.lv_image_set_src(img, &img_benchmark_lvgl_logo_rgb);
    c.lv_image_set_inner_align(img, c.LV_IMAGE_ALIGN_TILE);
    perf_ffi.shakeAnim(img, -@divTrunc(@as(i32, @intCast(lvgl.display.height())), 3));
}

fn singleRectangleCb() void {
    const scr = lvgl.screenActive();
    const obj = c.lv_obj_create(scr.obj).?;
    c.lv_obj_remove_style_all(obj);
    c.lv_obj_set_style_bg_opa(obj, c.LV_OPA_COVER, 0);
    c.lv_obj_center(obj);
    c.lv_obj_set_size(obj, c.lv_pct(30), c.lv_pct(30));
    perf_ffi.colorAnim(obj);
}

fn multipleRectanglesCb() void {
    const scr = lvgl.screenActive();
    _ = scr.flexFlow(c.LV_FLEX_FLOW_ROW_WRAP);
    _ = scr.flexAlign(c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_CENTER, c.LV_FLEX_ALIGN_SPACE_EVENLY);

    for (0..9) |_| {
        const obj = c.lv_obj_create(scr.obj).?;
        c.lv_obj_remove_style_all(obj);
        c.lv_obj_set_style_bg_opa(obj, c.LV_OPA_COVER, 0);
        c.lv_obj_set_size(obj, c.lv_pct(25), c.lv_pct(25));
        perf_ffi.colorAnim(obj);
    }
}

fn multipleRgbImagesCb() void {
    const scr = lvgl.screenActive();
    _ = scr.flexFlow(c.LV_FLEX_FLOW_ROW_WRAP);
    _ = scr.flexAlign(c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_START, c.LV_FLEX_ALIGN_START);
    _ = scr.padRow(20);

    var hor: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.width())) - 16, 116);
    var ver: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.height())) - 116, 116);
    if (hor < 1) hor = 1;
    if (ver < 1) ver = 1;

    var y: i32 = 0;
    while (y < ver) : (y += 1) {
        var x: i32 = 0;
        while (x < hor) : (x += 1) {
            const obj = c.lv_image_create(scr.obj).?;
            c.lv_image_set_src(obj, &img_benchmark_lvgl_logo_rgb);
            if (x == 0)
                c.lv_obj_add_flag(obj, c.LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            perf_ffi.shakeAnim(obj, 80);
        }
    }
}

fn multipleArgbImagesCb() void {
    const scr = lvgl.screenActive();
    _ = scr.flexFlow(c.LV_FLEX_FLOW_ROW_WRAP);
    _ = scr.flexAlign(c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_START, c.LV_FLEX_ALIGN_START);
    _ = scr.padRow(20);

    var hor: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.width())) - 16, 116);
    var ver: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.height())) - 116, 116);
    if (hor < 1) hor = 1;
    if (ver < 1) ver = 1;

    var y: i32 = 0;
    while (y < ver) : (y += 1) {
        var x: i32 = 0;
        while (x < hor) : (x += 1) {
            const obj = c.lv_image_create(scr.obj).?;
            c.lv_image_set_src(obj, &img_benchmark_lvgl_logo_argb);
            if (x == 0)
                c.lv_obj_add_flag(obj, c.LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            perf_ffi.shakeAnim(obj, 80);
        }
    }
}

fn rotatedArgbImagesCb() void {
    const scr = lvgl.screenActive();
    _ = scr.flexFlow(c.LV_FLEX_FLOW_ROW_WRAP);
    _ = scr.flexAlign(c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_START, c.LV_FLEX_ALIGN_START);
    _ = scr.padRow(20);

    var hor: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.width())) - 16, 116);
    var ver: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.height())) - 116, 116);
    if (hor < 1) hor = 1;
    if (ver < 1) ver = 1;

    var y: i32 = 0;
    while (y < ver) : (y += 1) {
        var x: i32 = 0;
        while (x < hor) : (x += 1) {
            const obj = c.lv_image_create(scr.obj).?;
            c.lv_image_set_src(obj, &img_benchmark_lvgl_logo_argb);
            if (x == 0)
                c.lv_obj_add_flag(obj, c.LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            c.lv_image_set_rotation(obj, @intCast(rndNext(100, 3500)));
            perf_ffi.shakeAnim(obj, 80);
        }
    }
}

fn multipleLabelsCb() void {
    const scr = lvgl.screenActive();
    _ = scr.flexFlow(c.LV_FLEX_FLOW_ROW_WRAP);
    _ = scr.flexAlign(c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_START, c.LV_FLEX_ALIGN_START);
    _ = scr.padRow(80);

    var s: c.lv_point_t = undefined;
    c.lv_text_get_size(&s, "Hello LVGL!", c.lv_obj_get_style_text_font(scr.obj, 0), 0, 0, c.LV_COORD_MAX, c.LV_TEXT_FLAG_NONE);

    var cnt: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.width())) - 16, s.x + 30);
    cnt *= @divTrunc(@as(i32, @intCast(lvgl.display.height())) - 200, s.y + 50);
    if (cnt < 1) cnt = 1;

    var i: i32 = 0;
    while (i < cnt) : (i += 1) {
        const obj = c.lv_label_create(scr.obj).?;
        c.lv_label_set_text(obj, "Hello LVGL!");
        perf_ffi.colorAnim(obj);
    }
}

fn screenSizedTextCb() void {
    const txt: [*:0]const u8 =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit. " ++
        "Nulla nec rhoncus arcu, in consectetur orci. Sed vitae dolor " ++
        "sed nisi ultrices vehicula quis ac dolor. Vivamus hendrerit " ++
        "hendrerit lectus, sed tempus velit suscipit in. Fusce eu " ++
        "tristique arcu. Sed et molestie leo, in lacinia nunc. Quisque " ++
        "semper lorem sed ante feugiat, at molestie risus blandit. " ++
        "Maecenas lobortis urna in diam feugiat porta. Ut facilisis " ++
        "mauris eget nibh posuere aliquet. Proin facilisis egestas " ++
        "magna, id vulputate massa bibendum a.\n\n" ++
        "Phasellus iaculis malesuada molestie. Cras ullamcorper justo " ++
        "a dolor dignissim tincidunt. Mauris euismod risus quis " ++
        "lobortis mollis. Ut vitae placerat massa, aliquet various " ++
        "lectus. Nulla ac ornare purus, quis auctor velit. Donec " ++
        "posuere dolor rhoncus efficitur dictum. Integer venenatis " ++
        "aliquet nunc eu convallis. Nunc quis various velit. " ++
        "Suspendisse enim metus, molestie eget mauris sit amet, " ++
        "euismod volutpat turpis.\n\n" ++
        "Aliquam id tellus in enim hendrerit mattis. Sed ipsum arcu, " ++
        "feugiat sed eros quis, vulputate facilisis turpis. Quisque " ++
        "venenatis risus massa. Proin lacinia, nunc non ultrices " ++
        "commodo, ligula dolor lobortis lectus, iaculis pulvinar metus " ++
        "orci eu elit. Donec tincidunt lacinia semper. Class aptent " ++
        "taciti sociosqu ad litora torquent per conubia nostra, per " ++
        "inceptos himenaeos.\n\n" ++
        "Integer vehicula vestibulum eros. Donec facilisis magna a est " ++
        "cursus, sed posuere velit faucibus. In et ultrices lorem. Sed " ++
        "et lacus finibus, vulputate odio et, finibus tellus. Aenean " ++
        "finibus nibh vehicula elementum maximus.\n\n" ++
        "Fusce dignissim turpis massa, eget semper purus semper at. " ++
        "Ut et augue vitae metus laoreet auctor. Morbi tincidunt, " ++
        "neque vel tincidunt interdum, sapien nibh finibus lorem, eu " ++
        "eleifend diam ipsum et eros.";

    const scr = lvgl.screenActive();
    const obj = c.lv_label_create(scr.obj).?;
    c.lv_obj_set_width(obj, c.lv_pct(100));
    c.lv_label_set_text(obj, txt);
    c.lv_obj_update_layout(obj);
    perf_ffi.scrollAnim(scr.obj, scr.getScrollBottom());
}

fn multipleArcsCb() void {
    const scr = lvgl.screenActive();
    _ = scr.flexFlow(c.LV_FLEX_FLOW_ROW_WRAP);
    _ = scr.flexAlign(c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_START, c.LV_FLEX_ALIGN_START);

    var hor: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.width())) - 16, lvDpx(160));
    var ver: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.height())) - 16, lvDpx(160));
    if (hor < 1) hor = 1;
    if (ver < 1) ver = 1;

    var y: i32 = 0;
    while (y < ver) : (y += 1) {
        var x: i32 = 0;
        while (x < hor) : (x += 1) {
            const obj = c.lv_arc_create(scr.obj).?;
            if (x == 0)
                c.lv_obj_add_flag(obj, c.LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            c.lv_obj_set_size(obj, lvDpx(100), lvDpx(100));
            c.lv_obj_center(obj);
            c.lv_arc_set_bg_angles(obj, 0, 360);
            c.lv_obj_set_style_margin_all(obj, lvDpx(20), 0);
            c.lv_obj_set_style_arc_opa(obj, 0, c.LV_PART_MAIN);
            c.lv_obj_set_style_bg_opa(obj, 0, c.LV_PART_KNOB);
            c.lv_obj_set_style_arc_width(obj, 10, c.LV_PART_INDICATOR);
            c.lv_obj_set_style_arc_rounded(obj, false, c.LV_PART_INDICATOR);
            c.lv_obj_set_style_arc_color(obj, c.lv_color_hex3(@intCast(rndNext(0x00f, 0xff0))), c.LV_PART_INDICATOR);
            perf_ffi.arcAnim(obj);
        }
    }
}

fn containersCb() void {
    const scr = lvgl.screenActive();
    _ = scr.flexFlow(c.LV_FLEX_FLOW_ROW_WRAP);
    _ = scr.flexAlign(c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_START, c.LV_FLEX_ALIGN_START);

    var hor: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.width())) - 16, 300);
    var ver: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.height())) - 16, 150);
    if (hor < 1) hor = 1;
    if (ver < 1) ver = 1;

    var y: i32 = 0;
    while (y < ver) : (y += 1) {
        var x: i32 = 0;
        while (x < hor) : (x += 1) {
            const card = cardCreate();
            if (x == 0)
                c.lv_obj_add_flag(card, c.LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            perf_ffi.shakeAnim(card, 30);
        }
    }
}

fn containersWithOverlayCb() void {
    const scr = lvgl.screenActive();
    _ = scr.flexFlow(c.LV_FLEX_FLOW_ROW_WRAP);
    _ = scr.flexAlign(c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_START, c.LV_FLEX_ALIGN_START);

    var hor: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.width())) - 16, 300);
    var ver: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.height())) - 16, 150);
    if (hor < 1) hor = 1;
    if (ver < 1) ver = 1;

    var y: i32 = 0;
    while (y < ver) : (y += 1) {
        var x: i32 = 0;
        while (x < hor) : (x += 1) {
            const card = cardCreate();
            if (x == 0)
                c.lv_obj_add_flag(card, c.LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            perf_ffi.shakeAnim(card, 30);
        }
    }

    const layer = layerTop();
    c.lv_obj_set_style_bg_opa(layer, c.LV_OPA_50, 0);
    perf_ffi.colorAnim(layer.?);
}

fn containersWithOpaCb() void {
    const scr = lvgl.screenActive();
    _ = scr.flexFlow(c.LV_FLEX_FLOW_ROW_WRAP);
    _ = scr.flexAlign(c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_START, c.LV_FLEX_ALIGN_START);

    var hor: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.width())) - 16, 300);
    var ver: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.height())) - 16, 150);
    if (hor < 1) hor = 1;
    if (ver < 1) ver = 1;

    var y: i32 = 0;
    while (y < ver) : (y += 1) {
        var x: i32 = 0;
        while (x < hor) : (x += 1) {
            const card = cardCreate();
            if (x == 0)
                c.lv_obj_add_flag(card, c.LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            c.lv_obj_set_style_opa(card, c.LV_OPA_50, 0);
            perf_ffi.shakeAnim(card, 30);
        }
    }
}

fn containersWithOpaLayerCb() void {
    const scr = lvgl.screenActive();
    _ = scr.flexFlow(c.LV_FLEX_FLOW_ROW_WRAP);
    _ = scr.flexAlign(c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_START, c.LV_FLEX_ALIGN_START);

    var hor: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.width())) - 16, 300);
    var ver: i32 = @divTrunc(@as(i32, @intCast(lvgl.display.height())) - 16, 150);
    if (hor < 1) hor = 1;
    if (ver < 1) ver = 1;

    var y: i32 = 0;
    while (y < ver) : (y += 1) {
        var x: i32 = 0;
        while (x < hor) : (x += 1) {
            const card = cardCreate();
            c.lv_obj_set_style_opa_layered(card, c.LV_OPA_50, 0);
            if (x == 0)
                c.lv_obj_add_flag(card, c.LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            perf_ffi.shakeAnim(card, 30);
        }
    }
}

fn containersWithScrollingCb() void {
    const scr = lvgl.screenActive();
    _ = scr.flexFlow(c.LV_FLEX_FLOW_ROW_WRAP);
    _ = scr.flexAlign(c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_CENTER, c.LV_FLEX_ALIGN_START);

    for (0..50) |_| {
        _ = cardCreate();
    }

    _ = scr.updateLayout();
    perf_ffi.scrollAnim(scr.obj, scr.getScrollBottom());
}

// ---------------------------------------------------------------------------
// Scene 15: Widgets demo
// ---------------------------------------------------------------------------

var g_tabview: ?*c.lv_obj_t = null;
var g_slideshow_tab: u32 = 0;

extern fn benchmark_anim_generic(obj: *c.lv_obj_t, cb: c.lv_anim_exec_xcb_t, start: i32, end: i32, t1: u32, t2: u32) void;
extern fn benchmark_anim_slideshow(obj: *c.lv_obj_t, scroll_cb: c.lv_anim_exec_xcb_t, y_max: i32, speed: u32, ready_cb: ?*const fn (?*anyopaque) callconv(.c) void) void;

// Scene-level C-ABI callbacks (slideshow state machine + gauge anim) —
// extension of the perf_ffi boundary for callbacks that need scene state.
const scene_ffi = struct {
    pub fn slideshowScrollCb(var_ptr: ?*anyopaque, v: i32) callconv(.c) void {
        c.lv_obj_scroll_to_y(@ptrCast(var_ptr), v, animOff());
    }

    pub fn slideshowReadyCb(_: ?*anyopaque) callconv(.c) void {
        const tv = g_tabview orelse return;
        g_slideshow_tab = (g_slideshow_tab + 1) % 3;
        c.lv_tabview_set_active(tv, g_slideshow_tab, animOn());

        const content = c.lv_tabview_get_content(tv) orelse return;
        const tab = c.lv_obj_get_child(content, @intCast(g_slideshow_tab)) orelse return;
        c.lv_obj_update_layout(tab);
        var bot = c.lv_obj_get_scroll_bottom(tab);
        if (bot <= 0) bot = 1;
        const spd: u32 = lvAnimSpeed(@intCast(lvgl.display.dpi()));
        benchmark_anim_slideshow(tab, scene_ffi.slideshowScrollCb, bot, spd, scene_ffi.slideshowReadyCb);
    }

    pub fn gaugeArcExecCb(var_ptr: ?*anyopaque, v: i32) callconv(.c) void {
        c.lv_arc_set_value(@ptrCast(var_ptr), v);
    }

    pub fn nextSceneTimerCb(timer: ?*c.lv_timer_t) callconv(.c) void {
        scene_act += 1;
        loadScene(scene_act);

        if (scenes[scene_act].scene_time == 0) {
            c.lv_timer_delete(timer);
            summaryCreate();
        } else {
            c.lv_timer_set_period(timer, scenes[scene_act].scene_time);
        }
    }

    pub fn sysmonPerfObserverCb(observer: ?*c.lv_observer_t, subject: ?*c.lv_subject_t) callconv(.c) void {
        if (subject == null) return;

        var m: BenchPerfMetrics = undefined;
        benchmark_extract_perf_metrics(c.lv_subject_get_pointer(subject), &m);

        if (observer) |obs| {
            const label: ?*c.lv_obj_t = @ptrCast(c.lv_observer_get_target(obs));
            if (label) |lbl| {
                var buf: [192]u8 = undefined;
                const act: usize = @intCast(scene_act);
                const name = scenes[act].name;
                const total = m.render_avg_time + m.flush_avg_time;

                const name_prefix: [*:0]const u8 = if (name[0] != 0) name else "";
                const sep: [*:0]const u8 = if (name[0] != 0) ": " else "";
                const txt = std.fmt.bufPrint(&buf, "{s}{s}{d} FPS, {d}% CPU\nrefr. {d} ms = {d} ms render + {d} ms flush\x00", .{
                    name_prefix, sep, m.fps, m.cpu, total, m.render_avg_time, m.flush_avg_time,
                }) catch "benchmark\x00";
                c.lv_label_set_text(lbl, @ptrCast(txt.ptr));
            }
        }

        if (scenes[scene_act].measurement_cnt != 0) {
            scenes[scene_act].cpu_avg_usage += m.cpu;
            scenes[scene_act].fps_avg += m.fps;
            scenes[scene_act].render_avg_time += m.render_avg_time;
            scenes[scene_act].flush_avg_time += m.flush_avg_time;
        }
        scenes[scene_act].measurement_cnt += 1;
    }
};

fn widgetsDemoCb() void {
    const scr = lvgl.screenActive();
    _ = scr.padAll(0);
    _ = scr.padTop(0);

    // Tabview
    const tv = c.lv_tabview_create(scr.obj).?;
    g_tabview = tv;
    c.lv_tabview_set_tab_bar_size(tv, 40);
    const tab1 = c.lv_tabview_add_tab(tv, "Form").?;
    const tab2 = c.lv_tabview_add_tab(tv, "Gauges").?;
    const tab3 = c.lv_tabview_add_tab(tv, "Pickers").?;

    // Tab 1: Form
    c.lv_obj_set_flex_flow(tab1, c.LV_FLEX_FLOW_COLUMN);
    c.lv_obj_set_style_pad_gap(tab1, 10, 0);

    const ta = c.lv_textarea_create(tab1);
    c.lv_textarea_set_one_line(ta, true);
    c.lv_textarea_set_placeholder_text(ta, "Username");
    c.lv_obj_set_width(ta, c.lv_pct(90));

    const dd = c.lv_dropdown_create(tab1);
    c.lv_dropdown_set_options(dd, "Option A\nOption B\nOption C");
    c.lv_obj_set_width(dd, c.lv_pct(90));

    const slider = c.lv_slider_create(tab1);
    c.lv_slider_set_value(slider, 40, animOff());
    c.lv_obj_set_width(slider, c.lv_pct(90));

    const sw = c.lv_switch_create(tab1);
    c.lv_obj_add_state(sw, c.LV_STATE_CHECKED);

    const cb = c.lv_checkbox_create(tab1);
    c.lv_checkbox_set_text(cb, "I agree");

    const btn = c.lv_button_create(tab1).?;
    c.lv_obj_set_width(btn, c.lv_pct(90));
    const btn_lbl = c.lv_label_create(btn);
    c.lv_label_set_text(btn_lbl, "Submit");
    c.lv_obj_center(btn_lbl);

    // Tab 2: Gauges
    c.lv_obj_set_flex_flow(tab2, c.LV_FLEX_FLOW_ROW_WRAP);
    c.lv_obj_set_flex_align(tab2, c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_CENTER, c.LV_FLEX_ALIGN_START);
    c.lv_obj_set_style_pad_gap(tab2, 10, 0);

    // Gauge 1: circular 360° with 3 arcs
    {
        const gbox = c.lv_obj_create(tab2).?;
        c.lv_obj_set_size(gbox, 200, 200);
        c.lv_obj_set_style_pad_all(gbox, 0, 0);
        c.lv_obj_set_style_border_width(gbox, 0, 0);
        c.lv_obj_set_style_bg_opa(gbox, c.LV_OPA_TRANSP, 0);

        const scale = c.lv_scale_create(gbox);
        c.lv_obj_set_size(scale, 180, 180);
        c.lv_obj_center(scale);
        c.lv_scale_set_mode(scale, c.LV_SCALE_MODE_ROUND_OUTER);
        c.lv_scale_set_range(scale, 0, 100);
        c.lv_scale_set_total_tick_count(scale, 11);
        c.lv_scale_set_major_tick_every(scale, 5);
        c.lv_scale_set_angle_range(scale, 360);

        const arc_params = [_]struct { t1: u32, t2: u32, pal: c_uint, margin: i32 }{
            .{ .t1 = 4100, .t2 = 2700, .pal = c.LV_PALETTE_BLUE, .margin = 0 },
            .{ .t1 = 2600, .t2 = 3200, .pal = c.LV_PALETTE_RED, .margin = 20 },
            .{ .t1 = 2800, .t2 = 1800, .pal = c.LV_PALETTE_GREEN, .margin = 40 },
        };
        for (arc_params) |p| {
            const arc = c.lv_arc_create(gbox).?;
            c.lv_obj_set_size(arc, 180 - p.margin * 2, 180 - p.margin * 2);
            c.lv_obj_center(arc);
            c.lv_arc_set_range(arc, 0, 100);
            c.lv_arc_set_bg_angles(arc, 0, 360);
            c.lv_obj_set_style_arc_opa(arc, 0, c.LV_PART_MAIN);
            c.lv_obj_set_style_bg_opa(arc, 0, c.LV_PART_KNOB);
            c.lv_obj_set_style_arc_width(arc, 8, c.LV_PART_INDICATOR);
            c.lv_obj_set_style_arc_color(arc, c.lv_palette_main(p.pal), c.LV_PART_INDICATOR);
            benchmark_anim_generic(arc, scene_ffi.gaugeArcExecCb, 20, 100, p.t1, p.t2);
        }
    }

    // Gauge 2: semi-circular 270° with sections
    {
        const gbox = c.lv_obj_create(tab2).?;
        c.lv_obj_set_size(gbox, 200, 200);
        c.lv_obj_set_style_pad_all(gbox, 0, 0);
        c.lv_obj_set_style_border_width(gbox, 0, 0);
        c.lv_obj_set_style_bg_opa(gbox, c.LV_OPA_TRANSP, 0);

        const scale = c.lv_scale_create(gbox);
        c.lv_obj_set_size(scale, 180, 180);
        c.lv_obj_center(scale);
        c.lv_scale_set_mode(scale, c.LV_SCALE_MODE_ROUND_OUTER);
        c.lv_scale_set_range(scale, 10, 60);
        c.lv_scale_set_total_tick_count(scale, 21);
        c.lv_scale_set_major_tick_every(scale, 4);
        c.lv_scale_set_angle_range(scale, 270);
        c.lv_scale_set_rotation(scale, 135);

        // Animated indicator arc
        const arc = c.lv_arc_create(gbox).?;
        c.lv_obj_set_size(arc, 160, 160);
        c.lv_obj_center(arc);
        c.lv_arc_set_range(arc, 10, 60);
        c.lv_arc_set_bg_angles(arc, 0, 270);
        c.lv_arc_set_rotation(arc, 135);
        c.lv_obj_set_style_arc_opa(arc, 0, c.LV_PART_MAIN);
        c.lv_obj_set_style_bg_opa(arc, 0, c.LV_PART_KNOB);
        c.lv_obj_set_style_arc_width(arc, 12, c.LV_PART_INDICATOR);
        benchmark_anim_generic(arc, scene_ffi.gaugeArcExecCb, 10, 60, 4100, 800);
    }

    // Line chart: 12 points
    {
        const chart = c.lv_chart_create(tab2);
        c.lv_obj_set_size(chart, 200, 140);
        c.lv_chart_set_type(chart, c.LV_CHART_TYPE_LINE);
        c.lv_chart_set_point_count(chart, 12);
        const ser = c.lv_chart_add_series(chart, c.lv_palette_main(c.LV_PALETTE_BLUE), c.LV_CHART_AXIS_PRIMARY_Y);
        const data = [_]i32{ 10, 20, 30, 25, 40, 35, 50, 60, 55, 70, 65, 80 };
        for (data, 0..) |v, i| {
            c.lv_chart_set_value_by_id(chart, ser, @intCast(i), v);
        }
    }

    // Tab 3: Pickers
    c.lv_obj_set_flex_flow(tab3, c.LV_FLEX_FLOW_ROW_WRAP);
    c.lv_obj_set_flex_align(tab3, c.LV_FLEX_ALIGN_SPACE_EVENLY, c.LV_FLEX_ALIGN_CENTER, c.LV_FLEX_ALIGN_START);
    c.lv_obj_set_style_pad_gap(tab3, 10, 0);

    const cal = c.lv_calendar_create(tab3);
    c.lv_obj_set_size(cal, 200, 200);
    c.lv_calendar_set_today_date(cal, 2026, 4, 13);
    c.lv_calendar_set_showed_date(cal, 2026, 4);

    const roller = c.lv_roller_create(tab3);
    c.lv_roller_set_options(roller, "Mon\nTue\nWed\nThu\nFri\nSat\nSun", c.LV_ROLLER_MODE_NORMAL);
    c.lv_roller_set_visible_row_count(roller, 3);

    const spinbox = c.lv_spinbox_create(tab3);
    c.lv_spinbox_set_range(spinbox, 0, 100);
    c.lv_spinbox_set_value(spinbox, 42);
    c.lv_spinbox_set_step(spinbox, 1);

    // Bar chart: 7 points
    {
        const chart = c.lv_chart_create(tab3);
        c.lv_obj_set_size(chart, 200, 140);
        c.lv_chart_set_type(chart, c.LV_CHART_TYPE_BAR);
        c.lv_chart_set_point_count(chart, 7);
        const ser = c.lv_chart_add_series(chart, c.lv_palette_main(c.LV_PALETTE_GREEN), c.LV_CHART_AXIS_PRIMARY_Y);
        const data = [_]i32{ 40, 55, 30, 70, 50, 65, 45 };
        for (data, 0..) |v, i| {
            c.lv_chart_set_value_by_id(chart, ser, @intCast(i), v);
        }
    }

    // Start slideshow
    g_slideshow_tab = 0;
    c.lv_obj_update_layout(tab1);
    var bot = c.lv_obj_get_scroll_bottom(tab1);
    if (bot <= 0) bot = 1;
    const spd: u32 = lvAnimSpeed(@intCast(lvgl.display.dpi()));
    benchmark_anim_slideshow(tab1, scene_ffi.slideshowScrollCb, bot, spd, scene_ffi.slideshowReadyCb);
}

// ---------------------------------------------------------------------------
// Scene table
// ---------------------------------------------------------------------------

var scenes = [_]SceneDsc{
    .{ .name = "Empty screen", .create_cb = emptyScreenCb, .scene_time = 3000 },
    .{ .name = "Moving wallpaper", .create_cb = movingWallpaperCb, .scene_time = 3000 },
    .{ .name = "Single rectangle", .create_cb = singleRectangleCb, .scene_time = 3000 },
    .{ .name = "Multiple rectangles", .create_cb = multipleRectanglesCb, .scene_time = 3000 },
    .{ .name = "Multiple RGB images", .create_cb = multipleRgbImagesCb, .scene_time = 3000 },
    .{ .name = "Multiple ARGB images", .create_cb = multipleArgbImagesCb, .scene_time = 3000 },
    .{ .name = "Rotated ARGB images", .create_cb = rotatedArgbImagesCb, .scene_time = 3000 },
    .{ .name = "Multiple labels", .create_cb = multipleLabelsCb, .scene_time = 3000 },
    .{ .name = "Screen sized text", .create_cb = screenSizedTextCb, .scene_time = 5000 },
    .{ .name = "Multiple arcs", .create_cb = multipleArcsCb, .scene_time = 3000 },
    .{ .name = "Containers", .create_cb = containersCb, .scene_time = 3000 },
    .{ .name = "Containers with overlay", .create_cb = containersWithOverlayCb, .scene_time = 3000 },
    .{ .name = "Containers with opa", .create_cb = containersWithOpaCb, .scene_time = 3000 },
    .{ .name = "Containers with opa_layer", .create_cb = containersWithOpaLayerCb, .scene_time = 3000 },
    .{ .name = "Containers with scrolling", .create_cb = containersWithScrollingCb, .scene_time = 5000 },
    .{ .name = "Widgets demo", .create_cb = widgetsDemoCb, .scene_time = 20000 },
    .{ .name = "", .create_cb = null, .scene_time = 0 },
};

const N_SCENES: u32 = scenes.len - 1;

// ---------------------------------------------------------------------------
// Scene management
// ---------------------------------------------------------------------------

fn loadScene(scene: u32) void {
    const scr = lvgl.screenActive();
    scr.clean();
    _ = scr.bgColorSel(c.lv_palette_lighten(c.LV_PALETTE_GREY, 4), 0);
    _ = scr.textColorSel(c.lv_color_black(), 0);
    _ = scr.padAll(8);
    _ = scr.padTop(40);
    _ = scr.padGap(8);
    _ = scr.layout(c.LV_LAYOUT_NONE);
    _ = scr.flexAlign(c.LV_FLEX_ALIGN_START, c.LV_FLEX_ALIGN_START, c.LV_FLEX_ALIGN_START);

    _ = c.lv_anim_delete(scr.obj, perf_ffi.scrollAnimYCb);
    _ = c.lv_anim_delete(scr.obj, perf_ffi.shakeAnimYCb);
    _ = c.lv_anim_delete(scr.obj, perf_ffi.colorAnimCb);

    const layer = layerTop();
    _ = c.lv_anim_delete(layer, perf_ffi.colorAnimCb);
    c.lv_obj_set_style_bg_opa(layer, c.LV_OPA_TRANSP, 0);

    rndReset();
    if (scenes[scene].create_cb) |cb| cb();
}

// `scene_ffi.nextSceneTimerCb` and `scene_ffi.sysmonPerfObserverCb` live in `scene_ffi` — see
// the perf_ffi section near the top of the file.

// ---------------------------------------------------------------------------
// Draw task event callback for summary table styling
// ---------------------------------------------------------------------------

// Table draw task styling uses the shared C helper (benchmark_table_draw_task_cb)
// because lv_draw_buf_t embeds lv_image_header_t with C bitfields that are
// opaque in Zig's @cImport.

// ---------------------------------------------------------------------------
// Summary table
// ---------------------------------------------------------------------------

fn summaryCreate() void {
    const scr = lvgl.screenActive();
    scr.clean();
    c.lv_obj_set_style_pad_hor(scr.obj, 0, 0);

    const table = c.lv_table_create(scr.obj).?;
    c.lv_obj_set_width(table, c.lv_pct(100));
    c.lv_obj_set_style_max_height(table, c.lv_pct(100), 0);
    c.lv_obj_add_flag(table, c.LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    c.lv_obj_set_style_text_font(table, &c.lv_font_montserrat_14, c.LV_PART_ITEMS);
    c.lv_obj_set_style_text_font(table, &c.lv_font_montserrat_14, 0);
    c.lv_obj_set_style_pad_top(table, 2, c.LV_PART_ITEMS);
    c.lv_obj_set_style_pad_bottom(table, 2, c.LV_PART_ITEMS);
    c.lv_obj_set_style_pad_left(table, 4, c.LV_PART_ITEMS);
    c.lv_obj_set_style_pad_right(table, 4, c.LV_PART_ITEMS);
    c.lv_obj_set_style_text_color(table, c.lv_palette_darken(c.LV_PALETTE_BLUE_GREY, 2), c.LV_PART_ITEMS);
    c.lv_obj_set_style_border_color(table, c.lv_palette_darken(c.LV_PALETTE_BLUE_GREY, 2), c.LV_PART_ITEMS);
    _ = c.lv_obj_add_event_cb(table, benchmark_table_draw_task_cb, c.LV_EVENT_DRAW_TASK_ADDED, null);

    c.lv_table_set_cell_value(table, 0, 0, "Name");
    c.lv_table_set_cell_value(table, 0, 1, "Avg. CPU");
    c.lv_table_set_cell_value(table, 0, 2, "Avg. FPS");
    c.lv_table_set_cell_value(table, 0, 3, "Avg. time (render + flush)");

    ove.log.inf("Benchmark Summary", .{});
    ove.log.inf("Name, Avg. CPU, Avg. FPS, Avg. time, render, flush", .{});

    c.lv_obj_update_layout(table);
    const col_w: i32 = @divTrunc(c.lv_obj_get_content_width(table), 4);
    for (0..4) |ci| {
        c.lv_table_set_column_width(table, @intCast(ci), col_w);
    }

    var total_fps: i32 = 0;
    var total_cpu: i32 = 0;
    var total_render: i32 = 0;
    var total_flush: i32 = 0;
    var valid: i32 = 0;

    var i: u32 = 0;
    while (i < N_SCENES) : (i += 1) {
        c.lv_table_set_cell_value(table, i + 2, 0, scenes[i].name);

        if (scenes[i].measurement_cnt <= 1) {
            c.lv_table_set_cell_value(table, i + 2, 1, "N/A");
            c.lv_table_set_cell_value(table, i + 2, 2, "N/A");
            c.lv_table_set_cell_value(table, i + 2, 3, "N/A");
        } else {
            const cnt = scenes[i].measurement_cnt - 1;
            const cpu_val = scenes[i].cpu_avg_usage / cnt;
            const fps_val = scenes[i].fps_avg / cnt;
            const render_val = scenes[i].render_avg_time / cnt;
            const flush_val = scenes[i].flush_avg_time / cnt;

            var cpu_buf: [32]u8 = undefined;
            var fps_buf: [32]u8 = undefined;
            var time_buf: [64]u8 = undefined;

            const cpu_str = std.fmt.bufPrint(&cpu_buf, "{d} %\x00", .{cpu_val}) catch "N/A\x00";
            const fps_str = std.fmt.bufPrint(&fps_buf, "{d} FPS\x00", .{fps_val}) catch "N/A\x00";
            const time_str = std.fmt.bufPrint(&time_buf, "{d} ms ({d} + {d})\x00", .{ render_val + flush_val, render_val, flush_val }) catch "N/A\x00";

            c.lv_table_set_cell_value(table, i + 2, 1, @ptrCast(cpu_str.ptr));
            c.lv_table_set_cell_value(table, i + 2, 2, @ptrCast(fps_str.ptr));
            c.lv_table_set_cell_value(table, i + 2, 3, @ptrCast(time_str.ptr));

            ove.log.inf("{s}, {d}%, {d}, {d}, {d}, {d}", .{ scenes[i].name, cpu_val, fps_val, render_val + flush_val, render_val, flush_val });

            valid += 1;
            total_cpu += @intCast(cpu_val);
            total_fps += @intCast(fps_val);
            total_render += @intCast(render_val);
            total_flush += @intCast(flush_val);
        }
    }

    c.lv_table_set_cell_value(table, 1, 0, "All scenes avg.");
    if (valid < 1) {
        c.lv_table_set_cell_value(table, 1, 1, "N/A");
        c.lv_table_set_cell_value(table, 1, 2, "N/A");
        c.lv_table_set_cell_value(table, 1, 3, "N/A");
    } else {
        const avg_cpu: u32 = @intCast(@divTrunc(total_cpu, valid));
        const avg_fps: u32 = @intCast(@divTrunc(total_fps, valid));
        const avg_render: u32 = @intCast(@divTrunc(total_render, valid));
        const avg_flush: u32 = @intCast(@divTrunc(total_flush, valid));

        var avg_cpu_buf: [32]u8 = undefined;
        var avg_fps_buf: [32]u8 = undefined;
        var avg_time_buf: [64]u8 = undefined;

        const avg_cpu_str = std.fmt.bufPrint(&avg_cpu_buf, "{d} %\x00", .{avg_cpu}) catch "N/A\x00";
        const avg_fps_str = std.fmt.bufPrint(&avg_fps_buf, "{d} FPS\x00", .{avg_fps}) catch "N/A\x00";
        const avg_time_str = std.fmt.bufPrint(&avg_time_buf, "{d} ms ({d} + {d})\x00", .{ avg_render + avg_flush, avg_render, avg_flush }) catch "N/A\x00";

        c.lv_table_set_cell_value(table, 1, 1, @ptrCast(avg_cpu_str.ptr));
        c.lv_table_set_cell_value(table, 1, 2, @ptrCast(avg_fps_str.ptr));
        c.lv_table_set_cell_value(table, 1, 3, @ptrCast(avg_time_str.ptr));

        ove.log.inf("All avg, {d}%, {d}, {d}, {d}, {d}", .{ avg_cpu, avg_fps, avg_render + avg_flush, avg_render, avg_flush });
    }
}

// ---------------------------------------------------------------------------
// Graphics thread
// ---------------------------------------------------------------------------

fn graphicsEntry() void {
    var last_us: u64 = 0;
    _ = c.ove_time_get_us(&last_us);
    while (true) {
        var now_us: u64 = 0;
        _ = c.ove_time_get_us(&now_us);
        const elapsed_ms: u32 = @intCast((now_us - last_us) / 1000);
        last_us = now_us;
        {
            const guard = lvgl.lock();
            defer guard.deinit();
            lvgl.tick(elapsed_ms);
            lvgl.handler();
        }
        ove.thread.sleepMs(33);
    }
}

// ---------------------------------------------------------------------------
// App entry
// ---------------------------------------------------------------------------

var graphics_thread: ?ove.Thread(4096) = null;

fn appMain() void {
    ove.log.inf("LVGL benchmark (Zig): init", .{});

    graphics_thread = ove.Thread(4096).create("graphics", graphicsEntry, prio.high) catch {
        ove.log.err("Failed to spawn graphics", .{});
        return;
    };

    lvgl.init() catch {
        ove.log.err("LVGL init fail", .{});
        return;
    };

    // Setup benchmark
    {
        const guard = lvgl.lock();
        defer guard.deinit();

        scene_act = 0;

        const scr = lvgl.screenActive();
        _ = scr.removeStyleAll();
        _ = scr.bgOpaSel(c.LV_OPA_COVER, 0);
        _ = scr.textColorSel(c.lv_color_black(), 0);
        _ = scr.bgColorSel(c.lv_palette_lighten(c.LV_PALETTE_GREY, 4), 0);
        _ = scr.padAll(8);
        _ = scr.padTop(40);
        _ = scr.padGap(8);

        const title = c.lv_label_create(layerTop()).?;
        c.lv_obj_set_style_bg_opa(title, c.LV_OPA_COVER, 0);
        c.lv_obj_set_style_bg_color(title, c.lv_color_white(), 0);
        c.lv_obj_set_style_text_color(title, c.lv_color_black(), 0);
        c.lv_obj_set_style_text_font(title, &c.lv_font_montserrat_14, 0);
        c.lv_obj_set_width(title, c.lv_pct(100));

        loadScene(scene_act);

        _ = c.lv_timer_create(scene_ffi.nextSceneTimerCb, scenes[0].scene_time, null);

        // Setup performance observer
        const perf_subj = benchmark_get_perf_subject();
        if (perf_subj) |subj| {
            _ = c.lv_subject_add_observer_obj(subj, scene_ffi.sysmonPerfObserverCb, title, null);
        } else {
            c.lv_label_set_text(title, "Perf monitor unavailable");
        }
    }

    ove.log.inf("LVGL benchmark (Zig): running", .{});

    ove.run();
}

comptime {
    ove.exportMain(appMain);
}
