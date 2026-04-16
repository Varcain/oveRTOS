// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! LVGL Rendering Benchmark (Rust)
//!
//! Reimplements LVGL's benchmark demo scenes using the oveRTOS Rust
//! binding API. 15 rendering scenes stress-test widgets, animations,
//! layout, images, and compositing. A summary table shows per-scene
//! FPS / CPU / render / flush metrics.

#![cfg_attr(not(feature = "std"), no_std)]

use core::fmt::Write;

use ove::lvgl::{
    self, Animation, Arc, Button, Image, Label, Layout, Obj,
    Styleable, Table, Widget, ANIM_REPEAT_INFINITE,
};
use ove::{FmtBuf, Priority, Thread};

// =========================================================================
//  FFI — C benchmark perf helper + LVGL functions not in the Rust binding
// =========================================================================

#[repr(C)]
struct BenchPerfMetrics {
    fps: u32,
    cpu: u32,
    render_avg_time: u32,
    flush_avg_time: u32,
}

unsafe extern "C" {
    // Shared C performance helper
    static img_benchmark_lvgl_logo_rgb: core::ffi::c_void;
    static img_benchmark_lvgl_logo_argb: core::ffi::c_void;
    static img_benchmark_avatar: core::ffi::c_void;
    fn benchmark_get_perf_subject() -> *mut ove::ffi::lv_subject_t;
    fn benchmark_extract_perf_metrics(info: *const core::ffi::c_void, out: *mut BenchPerfMetrics);

    // LVGL functions not in the Rust binding allowlist
    fn lv_display_get_horizontal_resolution(disp: *mut core::ffi::c_void) -> i32;
    fn lv_display_get_vertical_resolution(disp: *mut core::ffi::c_void) -> i32;
    fn lv_display_get_dpi(disp: *mut core::ffi::c_void) -> i32;
    fn lv_layer_top() -> *mut ove::ffi::lv_obj_t;
    fn lv_obj_scroll_to_y(obj: *mut ove::ffi::lv_obj_t, y: i32, anim: u32);
    fn lv_obj_get_scroll_bottom(obj: *mut ove::ffi::lv_obj_t) -> i32;
    fn lv_obj_update_layout(obj: *mut ove::ffi::lv_obj_t);
    fn lv_obj_set_style_translate_y(obj: *mut ove::ffi::lv_obj_t, v: i32, sel: u32);
    fn lv_obj_set_style_margin_top(obj: *mut ove::ffi::lv_obj_t, v: i32, sel: u32);
    fn lv_obj_set_style_margin_bottom(obj: *mut ove::ffi::lv_obj_t, v: i32, sel: u32);
    fn lv_obj_set_style_margin_left(obj: *mut ove::ffi::lv_obj_t, v: i32, sel: u32);
    fn lv_obj_set_style_margin_right(obj: *mut ove::ffi::lv_obj_t, v: i32, sel: u32);
    fn lv_obj_set_style_arc_opa(obj: *mut ove::ffi::lv_obj_t, opa: u8, sel: u32);
    fn lv_obj_set_style_arc_rounded(obj: *mut ove::ffi::lv_obj_t, en: bool, sel: u32);
    fn lv_obj_set_style_opa_layered(obj: *mut ove::ffi::lv_obj_t, opa: u8, sel: u32);
    fn lv_obj_set_layout(obj: *mut ove::ffi::lv_obj_t, layout: u32);
    fn lv_obj_get_content_width(obj: *mut ove::ffi::lv_obj_t) -> i32;
    fn lv_obj_set_style_max_height(obj: *mut ove::ffi::lv_obj_t, v: i32, sel: u32);
    fn lv_image_set_inner_align(obj: *mut ove::ffi::lv_obj_t, align: u32);
    fn lv_palette_lighten(palette: u32, level: u8) -> ove::ffi::lv_color_t;
    fn lv_palette_darken(palette: u32, level: u8) -> ove::ffi::lv_color_t;
    fn lv_color_hex3(hex: u32) -> ove::ffi::lv_color_t;
    fn lv_text_get_size(
        res: *mut LvPoint,
        text: *const u8,
        font: *const ove::ffi::lv_font_t,
        letter_space: i32,
        line_space: i32,
        max_width: i32,
        flag: u32,
    );
    fn lv_subject_get_pointer(subj: *mut ove::ffi::lv_subject_t) -> *const core::ffi::c_void;
    fn lv_observer_get_target_obj(observer: *mut LvObserver) -> *mut ove::ffi::lv_obj_t;
    fn lv_anim_speed(speed: u32) -> u32;

    // Widgets demo FFI
    fn lv_tabview_create(parent: *mut ove::ffi::lv_obj_t) -> *mut ove::ffi::lv_obj_t;
    fn lv_tabview_add_tab(tv: *mut ove::ffi::lv_obj_t, name: *const u8) -> *mut ove::ffi::lv_obj_t;
    fn lv_tabview_set_active(tv: *mut ove::ffi::lv_obj_t, idx: u32, anim: u32);
    fn lv_tabview_set_tab_bar_size(tv: *mut ove::ffi::lv_obj_t, size: i32);
    fn lv_tabview_get_content(tv: *mut ove::ffi::lv_obj_t) -> *mut ove::ffi::lv_obj_t;
    fn lv_obj_get_child(obj: *mut ove::ffi::lv_obj_t, idx: i32) -> *mut ove::ffi::lv_obj_t;
    fn lv_textarea_create(parent: *mut ove::ffi::lv_obj_t) -> *mut ove::ffi::lv_obj_t;
    fn lv_textarea_set_one_line(ta: *mut ove::ffi::lv_obj_t, en: bool);
    fn lv_textarea_set_placeholder_text(ta: *mut ove::ffi::lv_obj_t, txt: *const u8);
    fn lv_dropdown_create(parent: *mut ove::ffi::lv_obj_t) -> *mut ove::ffi::lv_obj_t;
    fn lv_dropdown_set_options(dd: *mut ove::ffi::lv_obj_t, opts: *const u8);
    fn lv_slider_create(parent: *mut ove::ffi::lv_obj_t) -> *mut ove::ffi::lv_obj_t;
    fn lv_slider_set_value(s: *mut ove::ffi::lv_obj_t, v: i32, anim: u32);
    fn lv_switch_create(parent: *mut ove::ffi::lv_obj_t) -> *mut ove::ffi::lv_obj_t;
    fn lv_checkbox_create(parent: *mut ove::ffi::lv_obj_t) -> *mut ove::ffi::lv_obj_t;
    fn lv_checkbox_set_text(cb: *mut ove::ffi::lv_obj_t, txt: *const u8);
    fn lv_chart_create(parent: *mut ove::ffi::lv_obj_t) -> *mut ove::ffi::lv_obj_t;
    fn lv_chart_set_type(chart: *mut ove::ffi::lv_obj_t, t: u32);
    fn lv_chart_set_point_count(chart: *mut ove::ffi::lv_obj_t, cnt: u32);
    fn lv_chart_add_series(chart: *mut ove::ffi::lv_obj_t, color: ove::ffi::lv_color_t, axis: u32) -> *mut core::ffi::c_void;
    fn lv_chart_set_series_value_by_id(chart: *mut ove::ffi::lv_obj_t, ser: *mut core::ffi::c_void, id: u32, v: i32);
    fn lv_calendar_create(parent: *mut ove::ffi::lv_obj_t) -> *mut ove::ffi::lv_obj_t;
    fn lv_calendar_set_today_date(cal: *mut ove::ffi::lv_obj_t, y: u32, m: u32, d: u32);
    fn lv_calendar_set_month_shown(cal: *mut ove::ffi::lv_obj_t, y: u32, m: u32);
    fn lv_roller_create(parent: *mut ove::ffi::lv_obj_t) -> *mut ove::ffi::lv_obj_t;
    fn lv_roller_set_options(r: *mut ove::ffi::lv_obj_t, opts: *const u8, mode: u32);
    fn lv_roller_set_visible_row_count(r: *mut ove::ffi::lv_obj_t, cnt: u32);
    fn lv_spinbox_create(parent: *mut ove::ffi::lv_obj_t) -> *mut ove::ffi::lv_obj_t;
    fn lv_spinbox_set_range(sb: *mut ove::ffi::lv_obj_t, min: i32, max: i32);
    fn lv_spinbox_set_value(sb: *mut ove::ffi::lv_obj_t, v: i32);
    fn lv_spinbox_set_step(sb: *mut ove::ffi::lv_obj_t, step: u32);
    fn lv_scale_create(parent: *mut ove::ffi::lv_obj_t) -> *mut ove::ffi::lv_obj_t;
    fn lv_scale_set_mode(scale: *mut ove::ffi::lv_obj_t, mode: u32);
    fn lv_scale_set_range(scale: *mut ove::ffi::lv_obj_t, min: i32, max: i32);
    fn lv_scale_set_total_tick_count(scale: *mut ove::ffi::lv_obj_t, cnt: u32);
    fn lv_scale_set_major_tick_every(scale: *mut ove::ffi::lv_obj_t, nth: u32);
    fn lv_scale_set_angle_range(scale: *mut ove::ffi::lv_obj_t, angle: u32);
    fn lv_scale_set_rotation(scale: *mut ove::ffi::lv_obj_t, rot: i32);
    fn lv_scale_add_section(scale: *mut ove::ffi::lv_obj_t) -> *mut core::ffi::c_void;
    fn lv_scale_section_set_range(sec: *mut core::ffi::c_void, min: i32, max: i32);
    fn lv_scale_section_set_style(sec: *mut core::ffi::c_void, part: u32, style: *mut core::ffi::c_void);
    fn lv_palette_main(palette: u32) -> ove::ffi::lv_color_t;
    fn lv_style_init(style: *mut core::ffi::c_void);
    fn lv_style_set_arc_color(style: *mut core::ffi::c_void, color: ove::ffi::lv_color_t);
    fn benchmark_anim_generic(obj: *mut ove::ffi::lv_obj_t, cb: Option<unsafe extern "C" fn(*mut core::ffi::c_void, i32)>, start: i32, end: i32, t1: u32, t2: u32);
    fn benchmark_anim_slideshow(obj: *mut ove::ffi::lv_obj_t, scroll_cb: Option<unsafe extern "C" fn(*mut core::ffi::c_void, i32)>, y_max: i32, speed: u32, ready_cb: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>);
}

// Opaque types for draw task FFI
#[repr(C)]
struct LvPoint {
    x: i32,
    y: i32,
}

#[repr(C)]
struct LvObserver {
    _opaque: [u8; 64],
}

// =========================================================================
//  Constants
// =========================================================================

const LV_OPA_COVER: u8 = 255;
const LV_OPA_TRANSP: u8 = 0;
const LV_OPA_50: u8 = 127;
const LV_ANIM_OFF: u32 = 0;
const LV_LAYOUT_NONE: u32 = 0;
const LV_COORD_MAX: i32 = (1 << 29) - 1;
const LV_TEXT_FLAG_NONE: u32 = 0;
const LV_IMAGE_ALIGN_TILE: u32 = 12;
const LV_PALETTE_GREY: u32 = 18;
const LV_PALETTE_BLUE_GREY: u32 = 17;
const LV_PART_ITEMS: u32 = 0x050000;

// =========================================================================
//  PRNG — deterministic random number sequence (matches C reference)
// =========================================================================

const RND_MAP: [u32; 64] = [
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
];

static mut RND_ACT: usize = 0;

fn rnd_reset() {
    unsafe { RND_ACT = 0; }
}

fn rnd_next(min: i32, max: i32) -> i32 {
    if min == max { return min; }
    let (lo, hi) = if min < max { (min, max) } else { (max, min) };
    let d = hi - lo;
    let r = unsafe {
        let v = (RND_MAP[RND_ACT] % d as u32) as i32 + lo;
        RND_ACT += 1;
        if RND_ACT >= RND_MAP.len() { RND_ACT = 0; }
        v
    };
    r
}

// =========================================================================
//  Helper: lv_pct (percentage coordinate)
// =========================================================================

/// Compute an LVGL percentage coordinate, matching the C macro `lv_pct(x)`.
/// `LV_PCT(x) = (x < 0) ? (1000 - x) | PCT_flag : x | PCT_flag`
/// where `PCT_flag = 1 << 29 | 1 << 30`.
const fn lv_pct(x: i32) -> i32 {
    const PCT_FLAG: i32 = (1 << 29) | (1 << 30);
    if x < 0 {
        (1000 - x) | PCT_FLAG
    } else {
        x | PCT_FLAG
    }
}

/// Compute `lv_dpx(n)` equivalent at runtime (uses display DPI).
fn lv_dpx(n: i32) -> i32 {
    let dpi = unsafe { lv_display_get_dpi(core::ptr::null_mut()) };
    if dpi <= 0 { return n; }
    (n * dpi + 80) / 160
}


// =========================================================================
//  Scene descriptor
// =========================================================================

struct SceneDsc {
    name: &'static [u8],
    create_cb: fn(),
    scene_time: u32,
    cpu_avg_usage: u32,
    fps_avg: u32,
    render_avg_time: u32,
    flush_avg_time: u32,
    measurement_cnt: u32,
}

impl SceneDsc {
    const fn new(name: &'static [u8], create_cb: fn(), scene_time: u32) -> Self {
        Self {
            name,
            create_cb,
            scene_time,
            cpu_avg_usage: 0,
            fps_avg: 0,
            render_avg_time: 0,
            flush_avg_time: 0,
            measurement_cnt: 0,
        }
    }
}

// =========================================================================
//  Mutable global state (single-threaded LVGL task access)
// =========================================================================

static mut SCENE_ACT: u32 = 0;

static mut SCENES: [SceneDsc; 17] = [
    SceneDsc::new(b"Empty screen\0",              empty_screen_cb,              3000),
    SceneDsc::new(b"Moving wallpaper\0",           moving_wallpaper_cb,          3000),
    SceneDsc::new(b"Single rectangle\0",           single_rectangle_cb,          3000),
    SceneDsc::new(b"Multiple rectangles\0",        multiple_rectangles_cb,       3000),
    SceneDsc::new(b"Multiple RGB images\0",        multiple_rgb_images_cb,       3000),
    SceneDsc::new(b"Multiple ARGB images\0",       multiple_argb_images_cb,      3000),
    SceneDsc::new(b"Rotated ARGB images\0",        rotated_argb_images_cb,       3000),
    SceneDsc::new(b"Multiple labels\0",            multiple_labels_cb,           3000),
    SceneDsc::new(b"Screen sized text\0",          screen_sized_text_cb,         5000),
    SceneDsc::new(b"Multiple arcs\0",              multiple_arcs_cb,             3000),
    SceneDsc::new(b"Containers\0",                 containers_cb,                3000),
    SceneDsc::new(b"Containers with overlay\0",    containers_with_overlay_cb,   3000),
    SceneDsc::new(b"Containers with opa\0",        containers_with_opa_cb,       3000),
    SceneDsc::new(b"Containers with opa_layer\0",  containers_with_opa_layer_cb, 3000),
    SceneDsc::new(b"Containers with scrolling\0",  containers_with_scrolling_cb, 5000),
    SceneDsc::new(b"Widgets demo\0",              widgets_demo_cb,             20000),
    // Sentinel
    SceneDsc::new(b"\0",                           empty_screen_cb,              0),
];

// =========================================================================
//  Animation helpers
// =========================================================================

unsafe extern "C" fn color_anim_cb(var: *mut core::ffi::c_void, _v: i32) {
    unsafe {
        let c1 = lv_color_hex3(rnd_next(0x00f, 0xff0) as u32);
        let c2 = lv_color_hex3(rnd_next(0x00f, 0xff0) as u32);
        ove::ffi::lv_obj_set_style_bg_color(var as *mut ove::ffi::lv_obj_t, c1, 0);
        ove::ffi::lv_obj_set_style_text_color(var as *mut ove::ffi::lv_obj_t, c2, 0);
    }
}

fn color_anim(obj: *mut ove::ffi::lv_obj_t) {
    Animation::new()
        .target(obj as *mut core::ffi::c_void)
        .exec_cb(Some(color_anim_cb))
        .values(0, 100)
        .duration(100)
        .repeat_count(ANIM_REPEAT_INFINITE)
        .start();
}

unsafe extern "C" fn shake_anim_y_cb(var: *mut core::ffi::c_void, v: i32) {
    unsafe {
        lv_obj_set_style_translate_y(var as *mut ove::ffi::lv_obj_t, v, 0);
    }
}

fn shake_anim(obj: *mut ove::ffi::lv_obj_t, y_max: i32) {
    let t1 = rnd_next(300, 3000) as u32;
    let t2 = rnd_next(300, 3000) as u32;

    Animation::new()
        .target(obj as *mut core::ffi::c_void)
        .exec_cb(Some(shake_anim_y_cb))
        .values(0, y_max)
        .duration(t1)
        .playback_duration(t2)
        .repeat_count(ANIM_REPEAT_INFINITE)
        .start();
}

unsafe extern "C" fn scroll_anim_y_cb(var: *mut core::ffi::c_void, v: i32) {
    unsafe {
        lv_obj_scroll_to_y(var as *mut ove::ffi::lv_obj_t, v, LV_ANIM_OFF);
    }
}

fn scroll_anim(obj: *mut ove::ffi::lv_obj_t, y_max: i32) {
    let t = unsafe {
        let dpi = lv_display_get_dpi(core::ptr::null_mut());
        lv_anim_speed(dpi as u32)
    };

    Animation::new()
        .target(obj as *mut core::ffi::c_void)
        .exec_cb(Some(scroll_anim_y_cb))
        .values(0, y_max)
        .duration(t)
        .playback_duration(t)
        .repeat_count(ANIM_REPEAT_INFINITE)
        .start();
}

unsafe extern "C" fn arc_anim_cb(var: *mut core::ffi::c_void, v: i32) {
    unsafe {
        ove::ffi::lv_arc_set_value(var as *mut ove::ffi::lv_obj_t, v);
    }
}

fn arc_anim(obj: *mut ove::ffi::lv_obj_t) {
    let t1 = rnd_next(1000, 3000) as u32;
    let t2 = rnd_next(1000, 3000) as u32;

    Animation::new()
        .target(obj as *mut core::ffi::c_void)
        .exec_cb(Some(arc_anim_cb))
        .values(0, 100)
        .duration(t1)
        .playback_duration(t2)
        .repeat_count(ANIM_REPEAT_INFINITE)
        .start();
}

// =========================================================================
//  Card composite widget (matches C reference)
// =========================================================================

fn card_create() -> *mut ove::ffi::lv_obj_t {
    let scr = lvgl::screen_active();
    let panel = Obj::create(scr);
    panel.size(270, 120).pad_all(8);

    unsafe {
        let child = Image::create(panel);
        child.src(&img_benchmark_avatar as *const _ as *const core::ffi::c_void)
             .align(lvgl::ALIGN_LEFT_MID, 0, 0);

        let name = Label::create(panel);
        name.text(b"John Smith\0");
        ove::ffi::lv_obj_set_pos(name.raw(), 100, 0);

        let desc = Label::create(panel);
        desc.text(b"A DIY enthusiast\0");
        ove::ffi::lv_obj_set_pos(desc.raw(), 100, 30);

        let btn = Button::create(panel);
        ove::ffi::lv_obj_set_pos(btn.raw(), 100, 50);

        Label::create(btn).text(b"Connect\0");
    }

    panel.raw()
}

// =========================================================================
//  Scene callbacks — 15 scenes
// =========================================================================

// Scene 0: Empty screen with color animation
fn empty_screen_cb() {
    color_anim(lvgl::screen_active().raw());
}

// Scene 1: Moving wallpaper (tiled RGB image with shake)
fn moving_wallpaper_cb() {
    let scr = lvgl::screen_active();
    scr.pad_all(0);

    unsafe {
        let img = Image::create(scr);
        ove::ffi::lv_obj_set_size(img.raw(), lv_pct(150), lv_pct(150));
        img.src(&img_benchmark_lvgl_logo_rgb as *const _ as *const core::ffi::c_void);
        lv_image_set_inner_align(img.raw(), LV_IMAGE_ALIGN_TILE);
        let vres = lv_display_get_vertical_resolution(core::ptr::null_mut());
        shake_anim(img.raw(), -vres / 3);
    }
}

// Scene 2: Single rectangle with color animation
fn single_rectangle_cb() {
    let scr = lvgl::screen_active();
    let obj = Obj::create(scr);
    unsafe { ove::ffi::lv_obj_remove_style_all(obj.raw()); }
    obj.bg_opa(LV_OPA_COVER).center().size(lv_pct(30), lv_pct(30));
    color_anim(obj.raw());
}

// Scene 3: Multiple rectangles in flex layout
fn multiple_rectangles_cb() {
    let scr = lvgl::screen_active();
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(scr.raw(), ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(
            scr.raw(),
            ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
            ove::ffi::LV_FLEX_ALIGN_CENTER,
            ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
        );
    }

    for _ in 0..9 {
        let obj = Obj::create(scr);
        unsafe { ove::ffi::lv_obj_remove_style_all(obj.raw()); }
        obj.bg_opa(LV_OPA_COVER).size(lv_pct(25), lv_pct(25));
        color_anim(obj.raw());
    }
}

// Scene 4: Multiple RGB images
fn multiple_rgb_images_cb() {
    let scr = lvgl::screen_active();
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(scr.raw(), ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(
            scr.raw(),
            ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
            ove::ffi::LV_FLEX_ALIGN_START,
            ove::ffi::LV_FLEX_ALIGN_START,
        );
        ove::ffi::lv_obj_set_style_pad_row(scr.raw(), 20, 0);

        let hres = lv_display_get_horizontal_resolution(core::ptr::null_mut());
        let vres = lv_display_get_vertical_resolution(core::ptr::null_mut());
        let mut hor = (hres - 16) / 116;
        let mut ver = (vres - 116) / 116;
        if hor < 1 { hor = 1; }
        if ver < 1 { ver = 1; }

        for y in 0..ver {
            for x in 0..hor {
                let img = Image::create(scr);
                img.src(&img_benchmark_lvgl_logo_rgb as *const _ as *const core::ffi::c_void);
                if x == 0 {
                    ove::ffi::lv_obj_add_flag(
                        img.raw(),
                        ove::ffi::LV_OBJ_FLAG_FLEX_IN_NEW_TRACK,
                    );
                }
                shake_anim(img.raw(), 80);
            }
            let _ = y;
        }
    }
}

// Scene 5: Multiple ARGB images
fn multiple_argb_images_cb() {
    let scr = lvgl::screen_active();
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(scr.raw(), ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(
            scr.raw(),
            ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
            ove::ffi::LV_FLEX_ALIGN_START,
            ove::ffi::LV_FLEX_ALIGN_START,
        );
        ove::ffi::lv_obj_set_style_pad_row(scr.raw(), 20, 0);

        let hres = lv_display_get_horizontal_resolution(core::ptr::null_mut());
        let vres = lv_display_get_vertical_resolution(core::ptr::null_mut());
        let mut hor = (hres - 16) / 116;
        let mut ver = (vres - 116) / 116;
        if hor < 1 { hor = 1; }
        if ver < 1 { ver = 1; }

        for y in 0..ver {
            for x in 0..hor {
                let img = Image::create(scr);
                img.src(&img_benchmark_lvgl_logo_argb as *const _ as *const core::ffi::c_void);
                if x == 0 {
                    ove::ffi::lv_obj_add_flag(
                        img.raw(),
                        ove::ffi::LV_OBJ_FLAG_FLEX_IN_NEW_TRACK,
                    );
                }
                shake_anim(img.raw(), 80);
            }
            let _ = y;
        }
    }
}

// Scene 6: Rotated ARGB images
fn rotated_argb_images_cb() {
    let scr = lvgl::screen_active();
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(scr.raw(), ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(
            scr.raw(),
            ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
            ove::ffi::LV_FLEX_ALIGN_START,
            ove::ffi::LV_FLEX_ALIGN_START,
        );
        ove::ffi::lv_obj_set_style_pad_row(scr.raw(), 20, 0);

        let hres = lv_display_get_horizontal_resolution(core::ptr::null_mut());
        let vres = lv_display_get_vertical_resolution(core::ptr::null_mut());
        let mut hor = (hres - 16) / 116;
        let mut ver = (vres - 116) / 116;
        if hor < 1 { hor = 1; }
        if ver < 1 { ver = 1; }

        for y in 0..ver {
            for x in 0..hor {
                let img = Image::create(scr);
                img.src(&img_benchmark_lvgl_logo_argb as *const _ as *const core::ffi::c_void)
                   .rotation(rnd_next(100, 3500));
                if x == 0 {
                    ove::ffi::lv_obj_add_flag(
                        img.raw(),
                        ove::ffi::LV_OBJ_FLAG_FLEX_IN_NEW_TRACK,
                    );
                }
                shake_anim(img.raw(), 80);
            }
            let _ = y;
        }
    }
}

// Scene 7: Multiple labels with color animation
fn multiple_labels_cb() {
    let scr = lvgl::screen_active();
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(scr.raw(), ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(
            scr.raw(),
            ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
            ove::ffi::LV_FLEX_ALIGN_START,
            ove::ffi::LV_FLEX_ALIGN_START,
        );
        ove::ffi::lv_obj_set_style_pad_row(scr.raw(), 80, 0);

        // Use the default screen font (montserrat_14) — lv_obj_get_style_text_font
        // is a static inline in LVGL and cannot be called from Rust FFI.
        let font = lvgl::font_montserrat_14();
        let mut s = LvPoint { x: 0, y: 0 };
        lv_text_get_size(
            &mut s,
            b"Hello LVGL!\0".as_ptr(),
            font,
            0, 0,
            LV_COORD_MAX,
            LV_TEXT_FLAG_NONE,
        );

        let hres = lv_display_get_horizontal_resolution(core::ptr::null_mut());
        let vres = lv_display_get_vertical_resolution(core::ptr::null_mut());
        let mut cnt = ((hres - 16) / (s.x + 30)) * ((vres - 200) / (s.y + 50));
        if cnt < 1 { cnt = 1; }

        for _ in 0..cnt {
            let lbl = Label::create(scr);
            lbl.text(b"Hello LVGL!\0");
            color_anim(lbl.raw());
        }
    }
}

// Scene 8: Screen-sized scrolling text
fn screen_sized_text_cb() {
    let scr = lvgl::screen_active();
    let lbl = Label::create(scr);
    lbl.width(lv_pct(100));
    lbl.text(
        b"Lorem ipsum dolor sit amet, consectetur adipiscing elit. \
Nulla nec rhoncus arcu, in consectetur orci. Sed vitae dolor \
sed nisi ultrices vehicula quis ac dolor. Vivamus hendrerit \
hendrerit lectus, sed tempus velit suscipit in. Fusce eu \
tristique arcu. Sed et molestie leo, in lacinia nunc. Quisque \
semper lorem sed ante feugiat, at molestie risus blandit. \
Maecenas lobortis urna in diam feugiat porta. Ut facilisis \
mauris eget nibh posuere aliquet. Proin facilisis egestas \
magna, id vulputate massa bibendum a.\n\n\
Phasellus iaculis malesuada molestie. Cras ullamcorper justo \
a dolor dignissim tincidunt. Mauris euismod risus quis \
lobortis mollis. Ut vitae placerat massa, aliquet various \
lectus. Nulla ac ornare purus, quis auctor velit. Donec \
posuere dolor rhoncus efficitur dictum. Integer venenatis \
aliquet nunc eu convallis. Nunc quis various velit. \
Suspendisse enim metus, molestie eget mauris sit amet, \
euismod volutpat turpis.\n\n\
Aliquam id tellus in enim hendrerit mattis. Sed ipsum arcu, \
feugiat sed eros quis, vulputate facilisis turpis. Quisque \
venenatis risus massa. Proin lacinia, nunc non ultrices \
commodo, ligula dolor lobortis lectus, iaculis pulvinar metus \
orci eu elit. Donec tincidunt lacinia semper. Class aptent \
taciti sociosqu ad litora torquent per conubia nostra, per \
inceptos himenaeos.\n\n\
Integer vehicula vestibulum eros. Donec facilisis magna a est \
cursus, sed posuere velit faucibus. In et ultrices lorem. Sed \
et lacus finibus, vulputate odio et, finibus tellus. Aenean \
finibus nibh vehicula elementum maximus.\n\n\
Fusce dignissim turpis massa, eget semper purus semper at. \
Ut et augue vitae metus laoreet auctor. Morbi tincidunt, \
neque vel tincidunt interdum, sapien nibh finibus lorem, eu \
eleifend diam ipsum et eros.\0",
    );

    unsafe {
        lv_obj_update_layout(lbl.raw());
        let scroll_bottom = lv_obj_get_scroll_bottom(scr.raw());
        scroll_anim(scr.raw(), scroll_bottom);
    }
}

// Scene 9: Multiple arcs with animation
fn multiple_arcs_cb() {
    let scr = lvgl::screen_active();
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(scr.raw(), ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(
            scr.raw(),
            ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
            ove::ffi::LV_FLEX_ALIGN_START,
            ove::ffi::LV_FLEX_ALIGN_START,
        );

        let hres = lv_display_get_horizontal_resolution(core::ptr::null_mut());
        let vres = lv_display_get_vertical_resolution(core::ptr::null_mut());
        let dpx160 = lv_dpx(160);
        let mut hor = (hres - 16) / dpx160;
        let mut ver = (vres - 16) / dpx160;
        if hor < 1 { hor = 1; }
        if ver < 1 { ver = 1; }

        let arc_size = lv_dpx(100);
        let margin = lv_dpx(20);

        for y in 0..ver {
            for x in 0..hor {
                let arc = Arc::create(scr);
                if x == 0 {
                    ove::ffi::lv_obj_add_flag(
                        arc.raw(),
                        ove::ffi::LV_OBJ_FLAG_FLEX_IN_NEW_TRACK,
                    );
                }
                arc.size(arc_size, arc_size).center().bg_angles(0, 360);
                lv_obj_set_style_margin_top(arc.raw(), margin, 0);
                lv_obj_set_style_margin_bottom(arc.raw(), margin, 0);
                lv_obj_set_style_margin_left(arc.raw(), margin, 0);
                lv_obj_set_style_margin_right(arc.raw(), margin, 0);
                lv_obj_set_style_arc_opa(arc.raw(), 0, lvgl::PART_MAIN);
                ove::ffi::lv_obj_set_style_bg_opa(arc.raw(), 0, lvgl::PART_KNOB);
                ove::ffi::lv_obj_set_style_arc_width(arc.raw(), 10, lvgl::PART_INDICATOR);
                lv_obj_set_style_arc_rounded(arc.raw(), false, lvgl::PART_INDICATOR);
                let c = lv_color_hex3(rnd_next(0x00f, 0xff0) as u32);
                ove::ffi::lv_obj_set_style_arc_color(arc.raw(), c, lvgl::PART_INDICATOR);
                arc_anim(arc.raw());
            }
            let _ = y;
        }
    }
}

// Scene 10: Containers (card widgets with shake)
fn containers_cb() {
    let scr = lvgl::screen_active();
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(scr.raw(), ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(
            scr.raw(),
            ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
            ove::ffi::LV_FLEX_ALIGN_START,
            ove::ffi::LV_FLEX_ALIGN_START,
        );

        let hres = lv_display_get_horizontal_resolution(core::ptr::null_mut());
        let vres = lv_display_get_vertical_resolution(core::ptr::null_mut());
        let mut hor = (hres - 16) / 300;
        let mut ver = (vres - 16) / 150;
        if hor < 1 { hor = 1; }
        if ver < 1 { ver = 1; }

        for y in 0..ver {
            for x in 0..hor {
                let card = card_create();
                if x == 0 {
                    ove::ffi::lv_obj_add_flag(card, ove::ffi::LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
                }
                shake_anim(card, 30);
            }
            let _ = y;
        }
    }
}

// Scene 11: Containers with overlay
fn containers_with_overlay_cb() {
    let scr = lvgl::screen_active();
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(scr.raw(), ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(
            scr.raw(),
            ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
            ove::ffi::LV_FLEX_ALIGN_START,
            ove::ffi::LV_FLEX_ALIGN_START,
        );

        let hres = lv_display_get_horizontal_resolution(core::ptr::null_mut());
        let vres = lv_display_get_vertical_resolution(core::ptr::null_mut());
        let mut hor = (hres - 16) / 300;
        let mut ver = (vres - 16) / 150;
        if hor < 1 { hor = 1; }
        if ver < 1 { ver = 1; }

        for y in 0..ver {
            for x in 0..hor {
                let card = card_create();
                if x == 0 {
                    ove::ffi::lv_obj_add_flag(card, ove::ffi::LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
                }
                shake_anim(card, 30);
            }
            let _ = y;
        }

        let top = lv_layer_top();
        ove::ffi::lv_obj_set_style_bg_opa(top, LV_OPA_50, 0);
        color_anim(top);
    }
}

// Scene 12: Containers with per-object opacity
fn containers_with_opa_cb() {
    let scr = lvgl::screen_active();
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(scr.raw(), ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(
            scr.raw(),
            ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
            ove::ffi::LV_FLEX_ALIGN_START,
            ove::ffi::LV_FLEX_ALIGN_START,
        );

        let hres = lv_display_get_horizontal_resolution(core::ptr::null_mut());
        let vres = lv_display_get_vertical_resolution(core::ptr::null_mut());
        let mut hor = (hres - 16) / 300;
        let mut ver = (vres - 16) / 150;
        if hor < 1 { hor = 1; }
        if ver < 1 { ver = 1; }

        for y in 0..ver {
            for x in 0..hor {
                let card = card_create();
                if x == 0 {
                    ove::ffi::lv_obj_add_flag(card, ove::ffi::LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
                }
                ove::ffi::lv_obj_set_style_opa(card, LV_OPA_50, 0);
                shake_anim(card, 30);
            }
            let _ = y;
        }
    }
}

// Scene 13: Containers with layered opacity
fn containers_with_opa_layer_cb() {
    let scr = lvgl::screen_active();
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(scr.raw(), ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(
            scr.raw(),
            ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
            ove::ffi::LV_FLEX_ALIGN_START,
            ove::ffi::LV_FLEX_ALIGN_START,
        );

        let hres = lv_display_get_horizontal_resolution(core::ptr::null_mut());
        let vres = lv_display_get_vertical_resolution(core::ptr::null_mut());
        let mut hor = (hres - 16) / 300;
        let mut ver = (vres - 16) / 150;
        if hor < 1 { hor = 1; }
        if ver < 1 { ver = 1; }

        for y in 0..ver {
            for x in 0..hor {
                let card = card_create();
                lv_obj_set_style_opa_layered(card, LV_OPA_50, 0);
                if x == 0 {
                    ove::ffi::lv_obj_add_flag(card, ove::ffi::LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
                }
                shake_anim(card, 30);
            }
            let _ = y;
        }
    }
}

// Scene 14: Containers with scrolling
fn containers_with_scrolling_cb() {
    let scr = lvgl::screen_active();
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(scr.raw(), ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(
            scr.raw(),
            ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
            ove::ffi::LV_FLEX_ALIGN_CENTER,
            ove::ffi::LV_FLEX_ALIGN_START,
        );

        for _ in 0..50 {
            card_create();
        }

        lv_obj_update_layout(scr.raw());
        let scroll_bottom = lv_obj_get_scroll_bottom(scr.raw());
        scroll_anim(scr.raw(), scroll_bottom);
    }
}

// Scene 15: Widgets demo
static mut G_TABVIEW: *mut ove::ffi::lv_obj_t = core::ptr::null_mut();
static mut G_SLIDESHOW_TAB: u32 = 0;

unsafe extern "C" fn slideshow_scroll_cb(var: *mut core::ffi::c_void, v: i32) {
    unsafe { lv_obj_scroll_to_y(var as *mut ove::ffi::lv_obj_t, v, 0); }
}

unsafe extern "C" fn slideshow_ready_cb(_a: *mut core::ffi::c_void) {
    unsafe {
        if G_TABVIEW.is_null() { return; }
        G_SLIDESHOW_TAB = (G_SLIDESHOW_TAB + 1) % 3;
        lv_tabview_set_active(G_TABVIEW, G_SLIDESHOW_TAB, 1); // LV_ANIM_ON
        let content = lv_tabview_get_content(G_TABVIEW);
        if content.is_null() { return; }
        let tab = lv_obj_get_child(content, G_SLIDESHOW_TAB as i32);
        if tab.is_null() { return; }
        lv_obj_update_layout(tab);
        let mut bot = lv_obj_get_scroll_bottom(tab);
        if bot <= 0 { bot = 1; }
        let spd = lv_anim_speed(lv_display_get_dpi(core::ptr::null_mut()) as u32);
        benchmark_anim_slideshow(tab, Some(slideshow_scroll_cb), bot, spd, Some(slideshow_ready_cb));
    }
}

unsafe extern "C" fn gauge_arc_exec_cb(var: *mut core::ffi::c_void, v: i32) {
    ove::ffi::lv_arc_set_value(var as *mut ove::ffi::lv_obj_t, v);
}

fn widgets_demo_cb() {
    unsafe {
        let scr = lvgl::screen_active();
        scr.pad_all(0);

        // Tabview
        let tv = lv_tabview_create(scr.raw());
        G_TABVIEW = tv;
        lv_tabview_set_tab_bar_size(tv, 40);
        let tab1 = lv_tabview_add_tab(tv, b"Form\0".as_ptr());
        let tab2 = lv_tabview_add_tab(tv, b"Gauges\0".as_ptr());
        let tab3 = lv_tabview_add_tab(tv, b"Pickers\0".as_ptr());

        // Tab 1: Form
        ove::ffi::lv_obj_set_flex_flow(tab1, ove::ffi::LV_FLEX_FLOW_COLUMN);
        ove::ffi::lv_obj_set_style_pad_row(tab1, 10, 0);
        ove::ffi::lv_obj_set_style_pad_column(tab1, 10, 0);

        let ta = lv_textarea_create(tab1);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_placeholder_text(ta, b"Username\0".as_ptr());
        ove::ffi::lv_obj_set_width(ta, lv_pct(90));

        let dd = lv_dropdown_create(tab1);
        lv_dropdown_set_options(dd, b"Option A\nOption B\nOption C\0".as_ptr());
        ove::ffi::lv_obj_set_width(dd, lv_pct(90));

        let slider = lv_slider_create(tab1);
        lv_slider_set_value(slider, 40, 0);
        ove::ffi::lv_obj_set_width(slider, lv_pct(90));

        let sw = lv_switch_create(tab1);
        ove::ffi::lv_obj_add_state(sw, 0x0001); // LV_STATE_CHECKED

        let cb = lv_checkbox_create(tab1);
        lv_checkbox_set_text(cb, b"I agree\0".as_ptr());

        let btn = Button::create(Obj::from_raw(tab1));
        btn.width(lv_pct(90));
        let btn_lbl = Label::create(btn);
        btn_lbl.text(b"Submit\0").center();

        // Tab 2: Gauges
        ove::ffi::lv_obj_set_flex_flow(tab2, ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(tab2, ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
                                        ove::ffi::LV_FLEX_ALIGN_CENTER,
                                        ove::ffi::LV_FLEX_ALIGN_START);
        ove::ffi::lv_obj_set_style_pad_row(tab2, 10, 0);
        ove::ffi::lv_obj_set_style_pad_column(tab2, 10, 0);

        // Gauge 1: circular 360° with 3 arcs
        {
            let gbox = ove::ffi::lv_obj_create(tab2);
            ove::ffi::lv_obj_set_size(gbox, 200, 200);
            ove::ffi::lv_obj_set_style_pad_top(gbox, 0, 0);
            ove::ffi::lv_obj_set_style_pad_bottom(gbox, 0, 0);
            ove::ffi::lv_obj_set_style_pad_left(gbox, 0, 0);
            ove::ffi::lv_obj_set_style_pad_right(gbox, 0, 0);
            ove::ffi::lv_obj_set_style_border_width(gbox, 0, 0);
            ove::ffi::lv_obj_set_style_bg_opa(gbox, 0, 0);

            let scale = lv_scale_create(gbox);
            ove::ffi::lv_obj_set_size(scale, 180, 180);
            ove::ffi::lv_obj_center(scale);
            lv_scale_set_mode(scale, 3); // LV_SCALE_MODE_ROUND_OUTER
            lv_scale_set_range(scale, 0, 100);
            lv_scale_set_total_tick_count(scale, 11);
            lv_scale_set_major_tick_every(scale, 5);
            lv_scale_set_angle_range(scale, 360);

            let arc_params: [(u32, u32, u32, i32); 3] = [
                (4100, 2700, 4, 0),   // LV_PALETTE_BLUE=4
                (2600, 3200, 0, 20),  // LV_PALETTE_RED=0
                (2800, 1800, 13, 40), // LV_PALETTE_GREEN=13
            ];
            for &(t1, t2, pal, margin) in &arc_params {
                let arc = ove::ffi::lv_arc_create(gbox);
                ove::ffi::lv_obj_set_size(arc, 180 - margin * 2, 180 - margin * 2);
                ove::ffi::lv_obj_center(arc);
                ove::ffi::lv_arc_set_range(arc, 0, 100);
                ove::ffi::lv_arc_set_bg_angles(arc, 0, 360);
                lv_obj_set_style_arc_opa(arc, 0, 0);
                ove::ffi::lv_obj_set_style_bg_opa(arc, 0, 0x40000); // LV_PART_KNOB
                ove::ffi::lv_obj_set_style_arc_width(arc, 8, 0x30000); // LV_PART_INDICATOR
                ove::ffi::lv_obj_set_style_arc_color(arc, lv_palette_main(pal), 0x30000);
                benchmark_anim_generic(arc, Some(gauge_arc_exec_cb), 20, 100, t1, t2);
            }
        }

        // Gauge 2: semi-circular 270° with sections
        {
            let gbox = ove::ffi::lv_obj_create(tab2);
            ove::ffi::lv_obj_set_size(gbox, 200, 200);
            ove::ffi::lv_obj_set_style_pad_top(gbox, 0, 0);
            ove::ffi::lv_obj_set_style_pad_bottom(gbox, 0, 0);
            ove::ffi::lv_obj_set_style_pad_left(gbox, 0, 0);
            ove::ffi::lv_obj_set_style_pad_right(gbox, 0, 0);
            ove::ffi::lv_obj_set_style_border_width(gbox, 0, 0);
            ove::ffi::lv_obj_set_style_bg_opa(gbox, 0, 0);

            let scale = lv_scale_create(gbox);
            ove::ffi::lv_obj_set_size(scale, 180, 180);
            ove::ffi::lv_obj_center(scale);
            lv_scale_set_mode(scale, 3);
            lv_scale_set_range(scale, 10, 60);
            lv_scale_set_total_tick_count(scale, 21);
            lv_scale_set_major_tick_every(scale, 4);
            lv_scale_set_angle_range(scale, 270);
            lv_scale_set_rotation(scale, 135);

            // Colored sections via static styles — use raw pointers to avoid
            // mutable static reference errors in Rust edition 2024.
            static mut STYLE_RED: [u8; 64] = [0; 64];
            static mut STYLE_BLUE: [u8; 64] = [0; 64];
            static mut STYLE_GREEN: [u8; 64] = [0; 64];
            let sr = core::ptr::addr_of_mut!(STYLE_RED) as *mut core::ffi::c_void;
            let sb = core::ptr::addr_of_mut!(STYLE_BLUE) as *mut core::ffi::c_void;
            let sg = core::ptr::addr_of_mut!(STYLE_GREEN) as *mut core::ffi::c_void;
            lv_style_init(sr);
            lv_style_set_arc_color(sr, lv_palette_main(0));
            lv_style_init(sb);
            lv_style_set_arc_color(sb, lv_palette_main(4));
            lv_style_init(sg);
            lv_style_set_arc_color(sg, lv_palette_main(13));

            let sec = lv_scale_add_section(scale);
            lv_scale_section_set_range(sec, 10, 25);
            lv_scale_section_set_style(sec, 0x30000, sr);
            let sec = lv_scale_add_section(scale);
            lv_scale_section_set_range(sec, 25, 45);
            lv_scale_section_set_style(sec, 0x30000, sb);
            let sec = lv_scale_add_section(scale);
            lv_scale_section_set_range(sec, 45, 60);
            lv_scale_section_set_style(sec, 0x30000, sg);

            let arc = ove::ffi::lv_arc_create(gbox);
            ove::ffi::lv_obj_set_size(arc, 160, 160);
            ove::ffi::lv_obj_center(arc);
            ove::ffi::lv_arc_set_range(arc, 10, 60);
            ove::ffi::lv_arc_set_bg_angles(arc, 0, 270);
            ove::ffi::lv_arc_set_rotation(arc, 135);
            lv_obj_set_style_arc_opa(arc, 0, 0);
            ove::ffi::lv_obj_set_style_bg_opa(arc, 0, 0x40000);
            ove::ffi::lv_obj_set_style_arc_width(arc, 12, 0x30000);
            benchmark_anim_generic(arc, Some(gauge_arc_exec_cb), 10, 60, 4100, 800);
        }

        // Line chart
        {
            let chart = lv_chart_create(tab2);
            ove::ffi::lv_obj_set_size(chart, 200, 140);
            lv_chart_set_type(chart, 1); // LV_CHART_TYPE_LINE
            lv_chart_set_point_count(chart, 12);
            let ser = lv_chart_add_series(chart, lv_palette_main(4), 0);
            let data: [i32; 12] = [10, 20, 30, 25, 40, 35, 50, 60, 55, 70, 65, 80];
            for (i, &v) in data.iter().enumerate() {
                lv_chart_set_series_value_by_id(chart, ser, i as u32, v);
            }
        }

        // Tab 3: Pickers
        ove::ffi::lv_obj_set_flex_flow(tab3, ove::ffi::LV_FLEX_FLOW_ROW_WRAP);
        ove::ffi::lv_obj_set_flex_align(tab3, ove::ffi::LV_FLEX_ALIGN_SPACE_EVENLY,
                                        ove::ffi::LV_FLEX_ALIGN_CENTER,
                                        ove::ffi::LV_FLEX_ALIGN_START);
        ove::ffi::lv_obj_set_style_pad_row(tab3, 10, 0);
        ove::ffi::lv_obj_set_style_pad_column(tab3, 10, 0);

        let cal = lv_calendar_create(tab3);
        ove::ffi::lv_obj_set_size(cal, 200, 200);
        lv_calendar_set_today_date(cal, 2026, 4, 13);
        lv_calendar_set_month_shown(cal, 2026, 4);

        let roller = lv_roller_create(tab3);
        lv_roller_set_options(roller, b"Mon\nTue\nWed\nThu\nFri\nSat\nSun\0".as_ptr(), 0);
        lv_roller_set_visible_row_count(roller, 3);

        let spinbox = lv_spinbox_create(tab3);
        lv_spinbox_set_range(spinbox, 0, 100);
        lv_spinbox_set_value(spinbox, 42);
        lv_spinbox_set_step(spinbox, 1);

        // Bar chart
        {
            let chart = lv_chart_create(tab3);
            ove::ffi::lv_obj_set_size(chart, 200, 140);
            lv_chart_set_type(chart, 2); // LV_CHART_TYPE_BAR
            lv_chart_set_point_count(chart, 7);
            let ser = lv_chart_add_series(chart, lv_palette_main(13), 0);
            let data: [i32; 7] = [40, 55, 30, 70, 50, 65, 45];
            for (i, &v) in data.iter().enumerate() {
                lv_chart_set_series_value_by_id(chart, ser, i as u32, v);
            }
        }

        // Start slideshow
        G_SLIDESHOW_TAB = 0;
        lv_obj_update_layout(tab1);
        let mut bot = lv_obj_get_scroll_bottom(tab1);
        if bot <= 0 { bot = 1; }
        let spd = lv_anim_speed(lv_display_get_dpi(core::ptr::null_mut()) as u32);
        benchmark_anim_slideshow(tab1, Some(slideshow_scroll_cb), bot, spd, Some(slideshow_ready_cb));
    }
}

// =========================================================================
//  Scene management
// =========================================================================

fn load_scene(scene: u32) {
    let scr = lvgl::screen_active();
    scr.clean();

    unsafe {
        let grey_light = lv_palette_lighten(LV_PALETTE_GREY, 4);
        ove::ffi::lv_obj_set_style_bg_color(scr.raw(), grey_light, 0);
        ove::ffi::lv_obj_set_style_text_color(
            scr.raw(), ove::ffi::lv_color_black(), 0,
        );
        ove::ffi::lv_obj_set_style_pad_top(scr.raw(), 40, 0);
        ove::ffi::lv_obj_set_style_pad_bottom(scr.raw(), 8, 0);
        ove::ffi::lv_obj_set_style_pad_left(scr.raw(), 8, 0);
        ove::ffi::lv_obj_set_style_pad_right(scr.raw(), 8, 0);
        ove::ffi::lv_obj_set_style_pad_row(scr.raw(), 8, 0);
        ove::ffi::lv_obj_set_style_pad_column(scr.raw(), 8, 0);
        lv_obj_set_layout(scr.raw(), LV_LAYOUT_NONE);
        ove::ffi::lv_obj_set_flex_align(
            scr.raw(),
            ove::ffi::LV_FLEX_ALIGN_START,
            ove::ffi::LV_FLEX_ALIGN_START,
            ove::ffi::LV_FLEX_ALIGN_START,
        );

        // Delete any leftover animations on screen and layer_top
        Animation::stop(scr.raw() as *mut _, Some(scroll_anim_y_cb));
        Animation::stop(scr.raw() as *mut _, Some(shake_anim_y_cb));
        Animation::stop(scr.raw() as *mut _, Some(color_anim_cb));

        let top = lv_layer_top();
        Animation::stop(top as *mut _, Some(color_anim_cb));
        ove::ffi::lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);

        rnd_reset();

        let idx = scene as usize;
        if idx < 15 {
            (SCENES[idx].create_cb)();
        }
    }
}

unsafe extern "C" fn next_scene_timer_cb(timer: *mut ove::ffi::lv_timer_t) {
    unsafe {
        SCENE_ACT += 1;
        let act = SCENE_ACT as usize;
        load_scene(SCENE_ACT);

        if act >= 15 || SCENES[act].scene_time == 0 {
            ove::ffi::lv_timer_delete(timer);
            summary_create();
        } else {
            ove::ffi::lv_timer_set_period(timer, SCENES[act].scene_time);
        }
    }
}

// =========================================================================
//  Performance observer
// =========================================================================

unsafe extern "C" fn sysmon_perf_observer_cb(
    observer: *mut ove::ffi::lv_observer_t,
    subject: *mut ove::ffi::lv_subject_t,
) {
    unsafe {
        let mut m = BenchPerfMetrics {
            fps: 0, cpu: 0, render_avg_time: 0, flush_avg_time: 0,
        };
        benchmark_extract_perf_metrics(lv_subject_get_pointer(subject), &mut m);

        let label = lv_observer_get_target_obj(observer as *mut LvObserver);
        let act = SCENE_ACT as usize;

        // Format the overlay label text
        let mut buf = [0u8; 192];
        let mut w = FmtBuf::new(&mut buf);

        if act < 15 {
            let name_bytes = SCENES[act].name;
            let name_len = name_bytes.iter().position(|&b| b == 0).unwrap_or(name_bytes.len());
            let name = core::str::from_utf8(&name_bytes[..name_len]).unwrap_or("?");
            let _ = write!(
                w, "{}: {} FPS, {}% CPU\nrefr. {} ms = {} ms render + {} ms flush",
                name,
                m.fps, m.cpu,
                m.render_avg_time + m.flush_avg_time,
                m.render_avg_time,
                m.flush_avg_time,
            );
        } else {
            let _ = write!(
                w, "{} FPS, {}% CPU\nrefr. {} ms = {} ms render + {} ms flush",
                m.fps, m.cpu,
                m.render_avg_time + m.flush_avg_time,
                m.render_avg_time,
                m.flush_avg_time,
            );
        }

        ove::ffi::lv_label_set_text(label, w.as_cstr().as_ptr() as *const _);

        // Ignore first call (stale data from previous scene)
        if act < 15 {
            if SCENES[act].measurement_cnt != 0 {
                SCENES[act].cpu_avg_usage += m.cpu;
                SCENES[act].fps_avg += m.fps;
                SCENES[act].render_avg_time += m.render_avg_time;
                SCENES[act].flush_avg_time += m.flush_avg_time;
            }
            SCENES[act].measurement_cnt += 1;
        }
    }
}

// =========================================================================
//  Summary table
// =========================================================================

unsafe extern "C" fn table_draw_task_event_cb(_e: *mut ove::ffi::lv_event_t) {
    // The draw-task event callback requires internal struct access that is
    // highly dependent on LVGL's internal layout. For the Rust benchmark
    // we omit the cell-styling logic to avoid fragile opaque-struct casts.
    // The summary table still displays correctly with default styling.
}

fn summary_create() {
    let scr = lvgl::screen_active();
    scr.clean();

    unsafe {
        ove::ffi::lv_obj_set_style_pad_left(scr.raw(), 0, 0);
        ove::ffi::lv_obj_set_style_pad_right(scr.raw(), 0, 0);

        let table = Table::create(scr);
        table.width(lv_pct(100));
        lv_obj_set_style_max_height(table.raw(), lv_pct(100), 0);
        ove::ffi::lv_obj_add_flag(table.raw(), ove::ffi::LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        ove::ffi::lv_obj_set_style_text_font(table.raw(), &ove::ffi::lv_font_montserrat_14, LV_PART_ITEMS);
        ove::ffi::lv_obj_set_style_text_font(table.raw(), &ove::ffi::lv_font_montserrat_14, ove::ffi::LV_PART_MAIN);
        ove::ffi::lv_obj_set_style_pad_top(table.raw(), 2, LV_PART_ITEMS);
        ove::ffi::lv_obj_set_style_pad_bottom(table.raw(), 2, LV_PART_ITEMS);
        ove::ffi::lv_obj_set_style_pad_left(table.raw(), 4, LV_PART_ITEMS);
        ove::ffi::lv_obj_set_style_pad_right(table.raw(), 4, LV_PART_ITEMS);

        let text_color = lv_palette_darken(LV_PALETTE_BLUE_GREY, 2);
        ove::ffi::lv_obj_set_style_text_color(table.raw(), text_color, LV_PART_ITEMS);
        let border_color = lv_palette_darken(LV_PALETTE_BLUE_GREY, 2);
        ove::ffi::lv_obj_set_style_border_color(table.raw(), border_color, LV_PART_ITEMS);
        ove::ffi::lv_obj_add_event_cb(
            table.raw(),
            Some(table_draw_task_event_cb),
            ove::ffi::LV_EVENT_DRAW_TASK_ADDED,
            core::ptr::null_mut(),
        );

        // Header row
        table.column_count(4);
        table.cell_value(0, 0, b"Name\0")
             .cell_value(0, 1, b"Avg. CPU\0")
             .cell_value(0, 2, b"Avg. FPS\0")
             .cell_value(0, 3, b"Avg. time (render + flush)\0");

        ove::log_inf!("Benchmark Summary");
        ove::log_inf!("Name, Avg. CPU, Avg. FPS, Avg. time, render, flush");

        lv_obj_update_layout(table.raw());
        let col_w = lv_obj_get_content_width(table.raw()) / 4;
        for c in 0..4u32 {
            table.column_width(c, col_w);
        }

        let mut total_fps: i32 = 0;
        let mut total_cpu: i32 = 0;
        let mut total_render: i32 = 0;
        let mut total_flush: i32 = 0;
        let mut valid: i32 = 0;

        for i in 0..15u32 {
            let idx = i as usize;
            let name = SCENES[idx].name;
            table.cell_value(i + 2, 0, name);

            if SCENES[idx].measurement_cnt <= 1 {
                table.cell_value(i + 2, 1, b"N/A\0")
                     .cell_value(i + 2, 2, b"N/A\0")
                     .cell_value(i + 2, 3, b"N/A\0");
            } else {
                let cnt = SCENES[idx].measurement_cnt - 1;
                let cpu = SCENES[idx].cpu_avg_usage / cnt;
                let fps = SCENES[idx].fps_avg / cnt;
                let render = SCENES[idx].render_avg_time / cnt;
                let flush = SCENES[idx].flush_avg_time / cnt;

                let mut buf1 = [0u8; 32];
                let mut w1 = FmtBuf::new(&mut buf1);
                let _ = write!(w1, "{} %", cpu);
                table.cell_value(i + 2, 1, w1.as_cstr());

                let mut buf2 = [0u8; 32];
                let mut w2 = FmtBuf::new(&mut buf2);
                let _ = write!(w2, "{} FPS", fps);
                table.cell_value(i + 2, 2, w2.as_cstr());

                let mut buf3 = [0u8; 48];
                let mut w3 = FmtBuf::new(&mut buf3);
                let _ = write!(w3, "{} ms ({} + {})", render + flush, render, flush);
                table.cell_value(i + 2, 3, w3.as_cstr());

                {
                    let name_len = name.iter().position(|&b| b == 0).unwrap_or(name.len());
                    let name_str = core::str::from_utf8(&name[..name_len]).unwrap_or("?");
                    ove::log_inf!("{}, {}%, {}, {}, {}, {}",
                        name_str, cpu, fps, render + flush, render, flush);
                }

                valid += 1;
                total_cpu += cpu as i32;
                total_fps += fps as i32;
                total_render += render as i32;
                total_flush += flush as i32;
            }
        }

        // "All scenes avg." row
        table.cell_value(1, 0, b"All scenes avg.\0");
        if valid < 1 {
            table.cell_value(1, 1, b"N/A\0")
                 .cell_value(1, 2, b"N/A\0")
                 .cell_value(1, 3, b"N/A\0");
        } else {
            let avg_cpu = total_cpu / valid;
            let avg_fps = total_fps / valid;
            let avg_render = total_render / valid;
            let avg_flush = total_flush / valid;

            let mut buf1 = [0u8; 32];
            let mut w1 = FmtBuf::new(&mut buf1);
            let _ = write!(w1, "{} %", avg_cpu);
            table.cell_value(1, 1, w1.as_cstr());

            let mut buf2 = [0u8; 32];
            let mut w2 = FmtBuf::new(&mut buf2);
            let _ = write!(w2, "{} FPS", avg_fps);
            table.cell_value(1, 2, w2.as_cstr());

            let mut buf3 = [0u8; 48];
            let mut w3 = FmtBuf::new(&mut buf3);
            let _ = write!(w3, "{} ms ({} + {})", avg_render + avg_flush, avg_render, avg_flush);
            table.cell_value(1, 3, w3.as_cstr());

            ove::log_inf!("All avg, {}%, {}, {}, {}, {}",
                avg_cpu, avg_fps, avg_render + avg_flush, avg_render, avg_flush);
        }
    }
}

// =========================================================================
//  Graphics thread
// =========================================================================

fn graphics_entry() {
    let mut last_us = ove::time::get_us().unwrap_or(0);
    loop {
        let now_us = ove::time::get_us().unwrap_or(last_us);
        let elapsed_ms = ((now_us - last_us) / 1000) as u32;
        last_us = now_us;
        {
            let _g = lvgl::lock();
            lvgl::tick(elapsed_ms);
            lvgl::handler();
        }
        Thread::sleep_ms(33);
    }
}

// =========================================================================
//  Entry point
// =========================================================================

fn app_main() {
    ove::log_inf!("LVGL benchmark (Rust): init");

    let _graphics = ove::thread!("graphics", graphics_entry, Priority::High, 4096);

    {
        if lvgl::init().is_err() {
            ove::log_err!("Failed to init LVGL");
            return;
        }

        let _g = lvgl::lock();

        unsafe {
            SCENE_ACT = 0;

            let scr = lvgl::screen_active();
            ove::ffi::lv_obj_remove_style_all(scr.raw());
            ove::ffi::lv_obj_set_style_bg_opa(scr.raw(), LV_OPA_COVER, 0);
            ove::ffi::lv_obj_set_style_text_color(
                scr.raw(), ove::ffi::lv_color_black(), 0,
            );
            let grey_light = lv_palette_lighten(LV_PALETTE_GREY, 4);
            ove::ffi::lv_obj_set_style_bg_color(scr.raw(), grey_light, 0);
            ove::ffi::lv_obj_set_style_pad_top(scr.raw(), 40, 0);
            ove::ffi::lv_obj_set_style_pad_bottom(scr.raw(), 8, 0);
            ove::ffi::lv_obj_set_style_pad_left(scr.raw(), 8, 0);
            ove::ffi::lv_obj_set_style_pad_right(scr.raw(), 8, 0);
            ove::ffi::lv_obj_set_style_pad_row(scr.raw(), 8, 0);
            ove::ffi::lv_obj_set_style_pad_column(scr.raw(), 8, 0);

            // Title overlay on layer_top
            let top = lv_layer_top();
            let title = ove::ffi::lv_label_create(top);
            ove::ffi::lv_obj_set_style_bg_opa(title, LV_OPA_COVER, 0);
            ove::ffi::lv_obj_set_style_bg_color(
                title, ove::ffi::lv_color_white(), 0,
            );
            ove::ffi::lv_obj_set_style_text_color(
                title, ove::ffi::lv_color_black(), 0,
            );
            ove::ffi::lv_obj_set_style_text_font(
                title, &ove::ffi::lv_font_montserrat_14, 0,
            );
            ove::ffi::lv_obj_set_width(title, lv_pct(100));

            load_scene(0);

            // Scene timer
            let scene_time = SCENES[0].scene_time;
            ove::ffi::lv_timer_create(
                Some(next_scene_timer_cb),
                scene_time,
                core::ptr::null_mut(),
            );

            // Performance observer
            let perf_subj = benchmark_get_perf_subject();
            if !perf_subj.is_null() {
                ove::ffi::lv_subject_add_observer_obj(
                    perf_subj,
                    Some(sysmon_perf_observer_cb),
                    title,
                    core::ptr::null_mut(),
                );
            } else {
                ove::ffi::lv_label_set_text(
                    title,
                    b"Perf monitor unavailable\0".as_ptr() as *const _,
                );
            }
        }
    }

    ove::log_inf!("LVGL benchmark (Rust): running");
    ove::run();
}

ove::main!(app_main);
