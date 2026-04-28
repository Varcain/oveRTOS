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
#![deny(unsafe_code)]

use core::fmt::Write;

use ove::lvgl::{
    self, ANIM_REPEAT_INFINITE, Animation, Arc, Button, Calendar, Chart, Checkbox, Color, Dropdown,
    FlexAlign, FlexFlow, Image, ImageSrc, Label, Layout, LayoutKind, LvCell, Obj, PART_INDICATOR,
    PART_ITEMS, PART_KNOB, PART_MAIN, Palette, Roller, SCALE_MODE_ROUND_OUTER, Scale, Slider,
    Spinbox, Style, Styleable, Switch, Table, Tabview, Textarea,
};
use ove::{FmtBuf, Priority, Thread};

// =========================================================================
//  Perf FFI — thin `#[allow(unsafe_code)]` glue for app-private C helpers
// =========================================================================

#[allow(unsafe_code)]
mod perf_ffi {
    //! Narrow FFI boundary for the app-private benchmark perf subject,
    //! image assets, and the slideshow state machine. Safe callers use
    //! the `pub fn` helpers below; the `unsafe extern "C"` blocks and
    //! trampolines stay confined to this module.

    use super::{PerfMetrics, SCENE, SCENES};
    use core::ffi::c_void;
    use core::fmt::Write;
    use core::ptr::addr_of;
    use core::sync::atomic::{AtomicPtr, AtomicU32, Ordering};
    use ove::FmtBuf;
    use ove::lvgl::{self, Animation, ImageDsc, ImageSrc, Label, Layout, Obj, Tabview};

    // Generated C image descriptors (shipped by the LVGL C demo).
    unsafe extern "C" {
        pub(super) static img_benchmark_lvgl_logo_rgb: ImageDsc;
        pub(super) static img_benchmark_lvgl_logo_argb: ImageDsc;
        pub(super) static img_benchmark_avatar: ImageDsc;

        fn benchmark_get_perf_subject() -> *mut ove::ffi::lv_subject_t;
        fn benchmark_extract_perf_metrics(info: *const c_void, out: *mut PerfMetrics);
    }

    pub fn logo_rgb() -> ImageSrc {
        ImageSrc::from_dsc(addr_of!(img_benchmark_lvgl_logo_rgb))
    }
    pub fn logo_argb() -> ImageSrc {
        ImageSrc::from_dsc(addr_of!(img_benchmark_lvgl_logo_argb))
    }
    pub fn avatar() -> ImageSrc {
        ImageSrc::from_dsc(addr_of!(img_benchmark_avatar))
    }

    // ---- Performance overlay ---------------------------------------------

    static OVERLAY_LABEL: AtomicPtr<ove::ffi::lv_obj_t> = AtomicPtr::new(core::ptr::null_mut());
    static OVERLAY_FN: AtomicPtr<u8> = AtomicPtr::new(core::ptr::null_mut());

    /// Register `label` to display perf metrics published by LVGL's sysmon.
    /// Returns `true` if the C subject is available. `on_tick` is invoked
    /// with each sample so callers can accumulate per-scene statistics.
    pub fn register_perf_overlay(label: Label, on_tick: fn(PerfMetrics)) -> bool {
        let subj = unsafe { benchmark_get_perf_subject() };
        if subj.is_null() {
            return false;
        }
        OVERLAY_LABEL.store(label.as_raw(), Ordering::Release);
        OVERLAY_FN.store(on_tick as *mut _, Ordering::Release);
        unsafe {
            ove::ffi::lv_subject_add_observer_obj(
                subj,
                Some(observer_trampoline),
                label.as_raw(),
                core::ptr::null_mut(),
            );
        }
        true
    }

    unsafe extern "C" fn observer_trampoline(
        _obs: *mut ove::ffi::lv_observer_t,
        subj: *mut ove::ffi::lv_subject_t,
    ) {
        let mut m = PerfMetrics::default();
        unsafe {
            let info = ove::ffi::lv_subject_get_pointer(subj);
            benchmark_extract_perf_metrics(info, &mut m);
        }

        let label_ptr = OVERLAY_LABEL.load(Ordering::Acquire);
        if !label_ptr.is_null() {
            let label = unsafe { Label::from_raw(label_ptr) };
            let mut buf = [0u8; 192];
            let mut w = FmtBuf::new(&mut buf);
            let scene_idx = SCENE.get().get().current as usize;
            if scene_idx < 15 {
                let name_bytes = SCENES[scene_idx].name;
                let name_len = name_bytes
                    .iter()
                    .position(|&b| b == 0)
                    .unwrap_or(name_bytes.len());
                let name = core::str::from_utf8(&name_bytes[..name_len]).unwrap_or("?");
                let _ = write!(
                    w,
                    "{}: {} FPS, {}% CPU\nrefr. {} ms = {} ms render + {} ms flush",
                    name,
                    m.fps,
                    m.cpu,
                    m.render_avg_time + m.flush_avg_time,
                    m.render_avg_time,
                    m.flush_avg_time
                );
            } else {
                let _ = write!(
                    w,
                    "{} FPS, {}% CPU\nrefr. {} ms = {} ms render + {} ms flush",
                    m.fps,
                    m.cpu,
                    m.render_avg_time + m.flush_avg_time,
                    m.render_avg_time,
                    m.flush_avg_time
                );
            }
            label.set_text(w.as_cstr());
        }

        let fn_ptr = OVERLAY_FN.load(Ordering::Acquire);
        if !fn_ptr.is_null() {
            let cb: fn(PerfMetrics) = unsafe { core::mem::transmute(fn_ptr) };
            cb(m);
        }
    }

    // ---- Slideshow state machine -----------------------------------------

    static SLIDESHOW_TV: AtomicPtr<ove::ffi::lv_obj_t> = AtomicPtr::new(core::ptr::null_mut());
    static SLIDESHOW_IDX: AtomicU32 = AtomicU32::new(0);

    /// Start the infinite slideshow on `tv`, beginning on `tab`.
    pub fn start_slideshow(tv: Tabview, tab: Obj) {
        SLIDESHOW_TV.store(tv.as_raw(), Ordering::Release);
        SLIDESHOW_IDX.store(0, Ordering::Release);
        start_tab_anim(tab);
    }

    fn start_tab_anim(tab: Obj) {
        tab.update_layout();
        let bot = core::cmp::max(tab.scroll_bottom(), 1);
        let spd = Animation::duration_for_speed(lvgl::display::dpi() as u32);

        let mut a: ove::ffi::lv_anim_t = unsafe { core::mem::zeroed() };
        unsafe {
            ove::ffi::lv_anim_init(&mut a);
            ove::ffi::lv_anim_set_var(&mut a, tab.as_raw() as *mut _);
            ove::ffi::lv_anim_set_exec_cb(&mut a, Some(slideshow_scroll_cb));
            ove::ffi::lv_anim_set_values(&mut a, 0, bot);
            ove::ffi::lv_anim_set_duration(&mut a, spd);
            ove::ffi::lv_anim_set_reverse_duration(&mut a, spd);
            ove::ffi::lv_anim_set_completed_cb(&mut a, Some(slideshow_ready_cb));
            ove::ffi::lv_anim_start(&a);
        }
    }

    unsafe extern "C" fn slideshow_scroll_cb(var: *mut c_void, v: i32) {
        unsafe {
            ove::ffi::lv_obj_scroll_to_y(var as *mut _, v, false);
        }
    }

    unsafe extern "C" fn slideshow_ready_cb(_a: *mut ove::ffi::lv_anim_t) {
        let tv_ptr = SLIDESHOW_TV.load(Ordering::Acquire);
        if tv_ptr.is_null() {
            return;
        }
        let idx = (SLIDESHOW_IDX.load(Ordering::Relaxed) + 1) % 3;
        SLIDESHOW_IDX.store(idx, Ordering::Release);

        unsafe {
            ove::ffi::lv_tabview_set_active(tv_ptr, idx, true);
            let content = ove::ffi::lv_tabview_get_content(tv_ptr);
            if content.is_null() {
                return;
            }
            let tab_raw = ove::ffi::lv_obj_get_child(content, idx as i32);
            if tab_raw.is_null() {
                return;
            }
            let tab = Obj::from_raw(tab_raw);
            start_tab_anim(tab);
        }
    }
}

/// Perf metrics struct matching `benchmark_perf.h`.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PerfMetrics {
    pub fps: u32,
    pub cpu: u32,
    pub render_avg_time: u32,
    pub flush_avg_time: u32,
}

// =========================================================================
//  Constants
// =========================================================================

const LV_OPA_COVER: u8 = 255;
const LV_OPA_TRANSP: u8 = 0;
const LV_OPA_50: u8 = 127;
const LV_IMAGE_ALIGN_TILE: u32 = 12;

const LV_OBJ_FLAG_FLEX_IN_NEW_TRACK: u32 = 1 << 12;
const LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS: u32 = 1 << 22;

// =========================================================================
//  PRNG — deterministic random sequence (matches C reference)
// =========================================================================

const RND_MAP: [u32; 64] = [
    0xbd13204f, 0x67d8167f, 0x20211c99, 0xb0a7cc05, 0x06d5c703, 0xeafb01a7, 0xd0473b5c, 0xc999aaa2,
    0x86f9d5d9, 0x294bdb29, 0x12a3c207, 0x78914d14, 0x10a30006, 0x6134c7db, 0x194443af, 0x142d1099,
    0x376292d5, 0x20f433c5, 0x074d2a59, 0x4e74c293, 0x072a0810, 0xdd0f136d, 0x5cca6dbc, 0x623bfdd8,
    0xb645eb2f, 0xbe50894a, 0xc9b56717, 0xe0f912c8, 0x4f6b5e24, 0xfe44b128, 0xe12d57a8, 0x9b15c9cc,
    0xab2ae1d3, 0xb4dc5074, 0x67d457c8, 0x8e46b00c, 0xa29a1871, 0xcee40332, 0x80f93aa1, 0x85286096,
    0x09bd6b49, 0x95072088, 0x2093924b, 0x6a27328f, 0xa796079b, 0xc3b488bc, 0xe29bcce0, 0x07048a4c,
    0x7d81bd99, 0x27aacb30, 0x44fc7a0e, 0xa2382241, 0x8357a17d, 0x97e9c9cc, 0xad10ff52, 0x9923fc5c,
    0x8f2c840a, 0x20356ba2, 0x7997a677, 0x9a7f1800, 0x35c7562b, 0xd901fe51, 0x8f4e053d, 0xa5b94923,
];

ove::shared!(RNG: LvCell<usize>);

fn rnd_reset() {
    RNG.get().set(0);
}

fn rnd_next(min: i32, max: i32) -> i32 {
    if min == max {
        return min;
    }
    let (lo, hi) = if min < max { (min, max) } else { (max, min) };
    let d = hi - lo;
    let cell = RNG.get();
    let i = cell.get();
    let v = (RND_MAP[i] % d as u32) as i32 + lo;
    let next = if i + 1 >= RND_MAP.len() { 0 } else { i + 1 };
    cell.set(next);
    v
}

// =========================================================================
//  Coordinate helpers
// =========================================================================

const fn lv_pct(x: i32) -> i32 {
    const PCT_FLAG: i32 = (1 << 29) | (1 << 30);
    if x < 0 {
        (1000 - x) | PCT_FLAG
    } else {
        x | PCT_FLAG
    }
}

fn lv_dpx(n: i32) -> i32 {
    let dpi = lvgl::display::dpi();
    if dpi <= 0 {
        return n;
    }
    (n * dpi + 80) / 160
}

// =========================================================================
//  Scene descriptor
// =========================================================================

#[derive(Clone, Copy)]
struct SceneDsc {
    name: &'static [u8],
    create_cb: fn(),
    scene_time: u32,
}

impl SceneDsc {
    const fn new(name: &'static [u8], create_cb: fn(), scene_time: u32) -> Self {
        Self {
            name,
            create_cb,
            scene_time,
        }
    }
}

#[derive(Clone, Copy, Default)]
struct SceneStats {
    cpu_avg_usage: u32,
    fps_avg: u32,
    render_avg_time: u32,
    flush_avg_time: u32,
    measurement_cnt: u32,
}

static SCENES: [SceneDsc; 17] = [
    SceneDsc::new(b"Empty screen\0", empty_screen_cb, 3000),
    SceneDsc::new(b"Moving wallpaper\0", moving_wallpaper_cb, 3000),
    SceneDsc::new(b"Single rectangle\0", single_rectangle_cb, 3000),
    SceneDsc::new(b"Multiple rectangles\0", multiple_rectangles_cb, 3000),
    SceneDsc::new(b"Multiple RGB images\0", multiple_rgb_images_cb, 3000),
    SceneDsc::new(b"Multiple ARGB images\0", multiple_argb_images_cb, 3000),
    SceneDsc::new(b"Rotated ARGB images\0", rotated_argb_images_cb, 3000),
    SceneDsc::new(b"Multiple labels\0", multiple_labels_cb, 3000),
    SceneDsc::new(b"Screen sized text\0", screen_sized_text_cb, 5000),
    SceneDsc::new(b"Multiple arcs\0", multiple_arcs_cb, 3000),
    SceneDsc::new(b"Containers\0", containers_cb, 3000),
    SceneDsc::new(
        b"Containers with overlay\0",
        containers_with_overlay_cb,
        3000,
    ),
    SceneDsc::new(b"Containers with opa\0", containers_with_opa_cb, 3000),
    SceneDsc::new(
        b"Containers with opa_layer\0",
        containers_with_opa_layer_cb,
        3000,
    ),
    SceneDsc::new(
        b"Containers with scrolling\0",
        containers_with_scrolling_cb,
        5000,
    ),
    SceneDsc::new(b"Widgets demo\0", widgets_demo_cb, 20000),
    SceneDsc::new(b"\0", empty_screen_cb, 0),
];

// =========================================================================
//  Mutable app state
// =========================================================================

#[derive(Clone, Copy)]
struct SceneState {
    current: u32,
}

ove::shared!(SCENE: LvCell<SceneState>);
ove::shared!(STATS: LvCell<[SceneStats; 17]>);

ove::shared!(STYLE_RED: Style);
ove::shared!(STYLE_BLUE: Style);
ove::shared!(STYLE_GREEN: Style);

ove::shared!(SCENE_TIMER: lvgl::Timer);

// =========================================================================
//  Animations — all expressed through the safe ove::lvgl API
// =========================================================================

fn color_tick(obj: Obj, _v: i32) {
    let c1 = Color::hex3(rnd_next(0x00f, 0xff0) as u32);
    let c2 = Color::hex3(rnd_next(0x00f, 0xff0) as u32);
    obj.bg_color_sel(c1, PART_MAIN)
        .text_color_sel(c2, PART_MAIN);
}

fn color_anim(obj: Obj) {
    Animation::new()
        .tick_fn(obj, color_tick)
        .values(0, 100)
        .duration(100)
        .repeat_count(ANIM_REPEAT_INFINITE)
        .start();
}

fn shake_anim(obj: Obj, y_max: i32) {
    let t1 = rnd_next(300, 3000) as u32;
    let t2 = rnd_next(300, 3000) as u32;
    lvgl::animate_translate_y_playback(obj, 0, y_max, t1, t2);
}

fn scroll_anim(obj: Obj, y_max: i32) {
    let t = Animation::duration_for_speed(lvgl::display::dpi() as u32);
    lvgl::animate_scroll_y_playback(obj, 0, y_max, t, t);
}

fn arc_anim(arc: Arc) {
    let t1 = rnd_next(1000, 3000) as u32;
    let t2 = rnd_next(1000, 3000) as u32;
    lvgl::animate_arc_value_playback(arc, 0, 100, t1, t2);
}

fn gauge_anim(arc: Arc, start: i32, end: i32, t1: u32, t2: u32) {
    lvgl::animate_arc_value_playback(arc, start, end, t1, t2);
}

// =========================================================================
//  Card composite widget
// =========================================================================

fn card_create() -> Obj {
    let scr = lvgl::screen_active();
    let panel = Obj::create(scr);
    panel.size(270, 120).pad_all(8);

    Image::create(panel)
        .source(perf_ffi::avatar())
        .align(lvgl::ALIGN_LEFT_MID, 0, 0);

    let name = Label::create(panel).text(b"John Smith\0");
    name.pos(100, 0);

    let desc = Label::create(panel).text(b"A DIY enthusiast\0");
    desc.pos(100, 30);

    let btn = Button::create(panel);
    btn.pos(100, 50);
    Label::create(btn).text(b"Connect\0");

    panel
}

// =========================================================================
//  Scene helpers
// =========================================================================

fn set_row_wrap(scr: Obj, main: FlexAlign, cross: FlexAlign, track: FlexAlign) {
    scr.flex_flow(FlexFlow::RowWrap)
        .flex_align(main, cross, track);
}

// =========================================================================
//  Scene callbacks — 15 scenes
// =========================================================================

fn empty_screen_cb() {
    color_anim(lvgl::screen_active());
}

fn moving_wallpaper_cb() {
    let scr = lvgl::screen_active();
    scr.pad_all(0);
    let img = Image::create(scr)
        .size(lv_pct(150), lv_pct(150))
        .source(perf_ffi::logo_rgb())
        .inner_align(LV_IMAGE_ALIGN_TILE);
    let vres = lvgl::display::height();
    shake_anim(*img, -vres / 3);
}

fn single_rectangle_cb() {
    let scr = lvgl::screen_active();
    let obj = Obj::create(scr);
    obj.remove_style_all();
    obj.bg_opa(LV_OPA_COVER)
        .center()
        .size(lv_pct(30), lv_pct(30));
    color_anim(obj);
}

fn multiple_rectangles_cb() {
    let scr = lvgl::screen_active();
    set_row_wrap(
        scr,
        FlexAlign::SpaceEvenly,
        FlexAlign::Center,
        FlexAlign::SpaceEvenly,
    );
    for _ in 0..9 {
        let obj = Obj::create(scr);
        obj.remove_style_all();
        obj.bg_opa(LV_OPA_COVER).size(lv_pct(25), lv_pct(25));
        color_anim(obj);
    }
}

fn images_grid(scr: Obj, src: ImageSrc, rotate: bool) {
    set_row_wrap(
        scr,
        FlexAlign::SpaceEvenly,
        FlexAlign::Start,
        FlexAlign::Start,
    );
    scr.pad_row(20);
    let hres = lvgl::display::width();
    let vres = lvgl::display::height();
    let hor = core::cmp::max((hres - 16) / 116, 1);
    let ver = core::cmp::max((vres - 116) / 116, 1);
    for _y in 0..ver {
        for x in 0..hor {
            let img = Image::create(scr).source(src);
            if rotate {
                img.rotation(rnd_next(100, 3500));
            }
            if x == 0 {
                img.add_flag(LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            }
            shake_anim(*img, 80);
        }
    }
}

fn multiple_rgb_images_cb() {
    images_grid(lvgl::screen_active(), perf_ffi::logo_rgb(), false);
}

fn multiple_argb_images_cb() {
    images_grid(lvgl::screen_active(), perf_ffi::logo_argb(), false);
}

fn rotated_argb_images_cb() {
    images_grid(lvgl::screen_active(), perf_ffi::logo_argb(), true);
}

fn multiple_labels_cb() {
    let scr = lvgl::screen_active();
    set_row_wrap(
        scr,
        FlexAlign::SpaceEvenly,
        FlexAlign::Start,
        FlexAlign::Start,
    );
    scr.pad_row(80);

    let font = lvgl::font_montserrat_14();
    let (tw, th) = lvgl::text_size(b"Hello LVGL!\0", font);
    let hres = lvgl::display::width();
    let vres = lvgl::display::height();
    let cnt = core::cmp::max(((hres - 16) / (tw + 30)) * ((vres - 200) / (th + 50)), 1);

    for _ in 0..cnt {
        let lbl = Label::create(scr).text(b"Hello LVGL!\0");
        color_anim(*lbl);
    }
}

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
    scr.update_layout();
    let scroll_bottom = scr.scroll_bottom();
    scroll_anim(scr, scroll_bottom);
}

fn multiple_arcs_cb() {
    let scr = lvgl::screen_active();
    set_row_wrap(
        scr,
        FlexAlign::SpaceEvenly,
        FlexAlign::Start,
        FlexAlign::Start,
    );

    let hres = lvgl::display::width();
    let vres = lvgl::display::height();
    let dpx160 = lv_dpx(160);
    let hor = core::cmp::max((hres - 16) / dpx160, 1);
    let ver = core::cmp::max((vres - 16) / dpx160, 1);

    let arc_size = lv_dpx(100);
    let margin = lv_dpx(20);

    for _y in 0..ver {
        for x in 0..hor {
            let arc = Arc::create(scr);
            if x == 0 {
                arc.add_flag(LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            }
            arc.size(arc_size, arc_size).center().bg_angles(0, 360);
            arc.margin_top(margin, PART_MAIN)
                .margin_bottom(margin, PART_MAIN)
                .margin_left(margin, PART_MAIN)
                .margin_right(margin, PART_MAIN)
                .arc_opa(0, PART_MAIN)
                .bg_opa_sel(0, PART_KNOB)
                .arc_width(10, PART_INDICATOR)
                .arc_rounded(false, PART_INDICATOR);
            let c = Color::hex3(rnd_next(0x00f, 0xff0) as u32);
            arc.arc_color_sel(c, PART_INDICATOR);
            arc_anim(arc);
        }
    }
}

fn containers_cb() {
    let scr = lvgl::screen_active();
    set_row_wrap(
        scr,
        FlexAlign::SpaceEvenly,
        FlexAlign::Start,
        FlexAlign::Start,
    );
    let hres = lvgl::display::width();
    let vres = lvgl::display::height();
    let hor = core::cmp::max((hres - 16) / 300, 1);
    let ver = core::cmp::max((vres - 16) / 150, 1);
    for _y in 0..ver {
        for x in 0..hor {
            let card = card_create();
            if x == 0 {
                card.add_flag(LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            }
            shake_anim(card, 30);
        }
    }
}

fn containers_with_overlay_cb() {
    containers_cb();
    let top = lvgl::layer_top();
    top.bg_opa_sel(LV_OPA_50, PART_MAIN);
    color_anim(top);
}

fn containers_with_opa_cb() {
    let scr = lvgl::screen_active();
    set_row_wrap(
        scr,
        FlexAlign::SpaceEvenly,
        FlexAlign::Start,
        FlexAlign::Start,
    );
    let hres = lvgl::display::width();
    let vres = lvgl::display::height();
    let hor = core::cmp::max((hres - 16) / 300, 1);
    let ver = core::cmp::max((vres - 16) / 150, 1);
    for _y in 0..ver {
        for x in 0..hor {
            let card = card_create();
            if x == 0 {
                card.add_flag(LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            }
            card.set_opa(LV_OPA_50);
            shake_anim(card, 30);
        }
    }
}

fn containers_with_opa_layer_cb() {
    let scr = lvgl::screen_active();
    set_row_wrap(
        scr,
        FlexAlign::SpaceEvenly,
        FlexAlign::Start,
        FlexAlign::Start,
    );
    let hres = lvgl::display::width();
    let vres = lvgl::display::height();
    let hor = core::cmp::max((hres - 16) / 300, 1);
    let ver = core::cmp::max((vres - 16) / 150, 1);
    for _y in 0..ver {
        for x in 0..hor {
            let card = card_create();
            card.opa_layered(LV_OPA_50, PART_MAIN);
            if x == 0 {
                card.add_flag(LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            }
            shake_anim(card, 30);
        }
    }
}

fn containers_with_scrolling_cb() {
    let scr = lvgl::screen_active();
    set_row_wrap(
        scr,
        FlexAlign::SpaceEvenly,
        FlexAlign::Center,
        FlexAlign::Start,
    );
    for _ in 0..50 {
        card_create();
    }
    scr.update_layout();
    let scroll_bottom = scr.scroll_bottom();
    scroll_anim(scr, scroll_bottom);
}

fn widgets_demo_cb() {
    let scr = lvgl::screen_active();
    scr.pad_all(0);

    let tv = Tabview::create(scr);
    tv.tab_bar_size(40);
    let tab1 = tv.add_tab(b"Form\0");
    let tab2 = tv.add_tab(b"Gauges\0");
    let tab3 = tv.add_tab(b"Pickers\0");

    // Tab 1: Form
    tab1.flex_flow(FlexFlow::Column).pad_row(10).pad_column(10);
    Textarea::create(tab1)
        .one_line(true)
        .placeholder(b"Username\0")
        .width(lv_pct(90));
    Dropdown::create(tab1)
        .options_static(b"Option A\nOption B\nOption C\0")
        .width(lv_pct(90));
    Slider::create(tab1).value(40).width(lv_pct(90));
    Switch::create(tab1).checked(true);
    Checkbox::create(tab1).text(b"I agree\0");
    let btn = Button::create(tab1).width(lv_pct(90));
    Label::create(btn).text(b"Submit\0").center();

    // Tab 2: Gauges
    tab2.flex_flow(FlexFlow::RowWrap)
        .flex_align(FlexAlign::SpaceEvenly, FlexAlign::Center, FlexAlign::Start)
        .pad_row(10)
        .pad_column(10);

    // Gauge 1: circular 360° with 3 arcs
    {
        let gbox = Obj::create(tab2);
        gbox.size(200, 200).pad_all(0).border_width(0).bg_opa(0);
        Scale::create(gbox)
            .size(180, 180)
            .center()
            .mode(SCALE_MODE_ROUND_OUTER)
            .range(0, 100)
            .total_tick_count(11)
            .major_tick_every(5)
            .angle_range(360);

        let arc_params: [(u32, u32, Palette, i32); 3] = [
            (4100, 2700, Palette::Indigo, 0),
            (2600, 3200, Palette::Red, 20),
            (2800, 1800, Palette::Amber, 40),
        ];
        for &(t1, t2, pal, margin) in &arc_params {
            let arc = Arc::create(gbox);
            arc.size(180 - margin * 2, 180 - margin * 2)
                .center()
                .range(0, 100)
                .bg_angles(0, 360);
            arc.arc_opa(0, PART_MAIN)
                .bg_opa_sel(0, PART_KNOB)
                .arc_width(8, PART_INDICATOR)
                .arc_color_sel(Color::palette_main(pal as u32), PART_INDICATOR);
            gauge_anim(arc, 20, 100, t1, t2);
        }
    }

    // Gauge 2: semi-circular 270° with sections
    {
        let gbox = Obj::create(tab2);
        gbox.size(200, 200).pad_all(0).border_width(0).bg_opa(0);
        let scale = Scale::create(gbox);
        scale.size(180, 180).center();
        scale
            .mode(SCALE_MODE_ROUND_OUTER)
            .range(10, 60)
            .total_tick_count(21)
            .major_tick_every(4)
            .angle_range(270)
            .rotation(135);

        scale
            .add_section()
            .range(10, 25)
            .style(PART_INDICATOR, STYLE_RED.get());
        scale
            .add_section()
            .range(25, 45)
            .style(PART_INDICATOR, STYLE_BLUE.get());
        scale
            .add_section()
            .range(45, 60)
            .style(PART_INDICATOR, STYLE_GREEN.get());

        let arc = Arc::create(gbox);
        arc.size(160, 160)
            .center()
            .range(10, 60)
            .bg_angles(0, 270)
            .rotation(135);
        arc.arc_opa(0, PART_MAIN)
            .bg_opa_sel(0, PART_KNOB)
            .arc_width(12, PART_INDICATOR);
        gauge_anim(arc, 10, 60, 4100, 800);
    }

    // Line chart
    {
        let chart = Chart::create(tab2)
            .size(200, 140)
            .chart_type(lvgl::CHART_TYPE_LINE)
            .point_count(12);
        let ser = chart.add_series(
            Color::palette_main(Palette::Indigo as u32),
            lvgl::CHART_AXIS_PRIMARY_Y,
        );
        let data: [i32; 12] = [10, 20, 30, 25, 40, 35, 50, 60, 55, 70, 65, 80];
        for (i, &v) in data.iter().enumerate() {
            ser.set_value_by_idx(i as u32, v);
        }
    }

    // Tab 3: Pickers
    tab3.flex_flow(FlexFlow::RowWrap)
        .flex_align(FlexAlign::SpaceEvenly, FlexAlign::Center, FlexAlign::Start)
        .pad_row(10)
        .pad_column(10);

    Calendar::create(tab3)
        .size(200, 200)
        .today(2026, 4, 13)
        .showed(2026, 4);
    Roller::create(tab3)
        .options(
            b"Mon\nTue\nWed\nThu\nFri\nSat\nSun\0",
            lvgl::ROLLER_MODE_NORMAL,
        )
        .visible_row_count(3);
    Spinbox::create(tab3).range(0, 100).value(42).step(1);

    {
        let chart = Chart::create(tab3)
            .size(200, 140)
            .chart_type(lvgl::CHART_TYPE_BAR)
            .point_count(7);
        let ser = chart.add_series(
            Color::palette_main(Palette::Amber as u32),
            lvgl::CHART_AXIS_PRIMARY_Y,
        );
        let data: [i32; 7] = [40, 55, 30, 70, 50, 65, 45];
        for (i, &v) in data.iter().enumerate() {
            ser.set_value_by_idx(i as u32, v);
        }
    }

    perf_ffi::start_slideshow(tv, tab1);
}

// =========================================================================
//  Scene management
// =========================================================================

fn load_scene(scene: u32) {
    let scr = lvgl::screen_active();
    scr.clean();

    scr.bg_color(Color::palette_lighten(Palette::Grey, 4))
        .text_color(Color::black())
        .pad_top(40)
        .pad_bottom(8)
        .pad_left(8)
        .pad_right(8)
        .pad_row(8)
        .pad_column(8)
        .layout(LayoutKind::None);

    lvgl::stop_animations(scr);
    let top = lvgl::layer_top();
    lvgl::stop_animations(top);
    top.bg_opa_sel(LV_OPA_TRANSP, PART_MAIN);

    rnd_reset();

    let idx = scene as usize;
    if idx < 15 {
        (SCENES[idx].create_cb)();
    }
}

fn next_scene_tick() {
    let st = SCENE.get();
    let cur = st.get();
    let act = cur.current + 1;
    st.set(SceneState { current: act });
    load_scene(act);

    let acti = act as usize;
    if acti >= 15 || SCENES[acti].scene_time == 0 {
        SCENE_TIMER.shutdown();
        summary_create();
    } else if let Some(t) = SCENE_TIMER.try_get() {
        t.period(SCENES[acti].scene_time);
    }
}

fn perf_tick(m: PerfMetrics) {
    let cur = SCENE.get().get().current as usize;
    if cur >= 15 {
        return;
    }
    let stats_cell = STATS.get();
    let mut arr = stats_cell.get();
    if arr[cur].measurement_cnt != 0 {
        arr[cur].cpu_avg_usage += m.cpu;
        arr[cur].fps_avg += m.fps;
        arr[cur].render_avg_time += m.render_avg_time;
        arr[cur].flush_avg_time += m.flush_avg_time;
    }
    arr[cur].measurement_cnt += 1;
    stats_cell.set(arr);
}

// =========================================================================
//  Summary table
// =========================================================================

fn summary_create() {
    let scr = lvgl::screen_active();
    scr.clean();
    scr.pad_left(0).pad_right(0);

    let table = Table::create(scr);
    table.width(lv_pct(100));
    table.max_height(lv_pct(100), PART_MAIN);
    table.add_flag(LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);

    table
        .text_color_sel(Color::palette_darken(Palette::BlueGrey, 2), PART_ITEMS)
        .border_color_sel(Color::palette_darken(Palette::BlueGrey, 2), PART_ITEMS);

    table
        .column_count(4)
        .cell_value(0, 0, b"Name\0")
        .cell_value(0, 1, b"Avg. CPU\0")
        .cell_value(0, 2, b"Avg. FPS\0")
        .cell_value(0, 3, b"Avg. time (render + flush)\0");

    ove::log_inf!("Benchmark Summary");
    ove::log_inf!("Name, Avg. CPU, Avg. FPS, Avg. time, render, flush");

    table.update_layout();
    let col_w = table.content_width() / 4;
    for c in 0..4u32 {
        table.column_width(c, col_w);
    }

    let stats = STATS.get().get();
    let mut total_fps: i32 = 0;
    let mut total_cpu: i32 = 0;
    let mut total_render: i32 = 0;
    let mut total_flush: i32 = 0;
    let mut valid: i32 = 0;

    for i in 0..15u32 {
        let idx = i as usize;
        let name = SCENES[idx].name;
        table.cell_value(i + 2, 0, name);

        if stats[idx].measurement_cnt <= 1 {
            table
                .cell_value(i + 2, 1, b"N/A\0")
                .cell_value(i + 2, 2, b"N/A\0")
                .cell_value(i + 2, 3, b"N/A\0");
        } else {
            let cnt = stats[idx].measurement_cnt - 1;
            let cpu = stats[idx].cpu_avg_usage / cnt;
            let fps = stats[idx].fps_avg / cnt;
            let render = stats[idx].render_avg_time / cnt;
            let flush = stats[idx].flush_avg_time / cnt;

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

            let name_len = name.iter().position(|&b| b == 0).unwrap_or(name.len());
            let name_str = core::str::from_utf8(&name[..name_len]).unwrap_or("?");
            ove::log_inf!(
                "{}, {}%, {}, {}, {}, {}",
                name_str,
                cpu,
                fps,
                render + flush,
                render,
                flush
            );

            valid += 1;
            total_cpu += cpu as i32;
            total_fps += fps as i32;
            total_render += render as i32;
            total_flush += flush as i32;
        }
    }

    table.cell_value(1, 0, b"All scenes avg.\0");
    if valid < 1 {
        table
            .cell_value(1, 1, b"N/A\0")
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
        let _ = write!(
            w3,
            "{} ms ({} + {})",
            avg_render + avg_flush,
            avg_render,
            avg_flush
        );
        table.cell_value(1, 3, w3.as_cstr());

        ove::log_inf!(
            "All avg, {}%, {}, {}, {}, {}",
            avg_cpu,
            avg_fps,
            avg_render + avg_flush,
            avg_render,
            avg_flush
        );
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

    RNG.init(LvCell::new(0));
    SCENE.init(LvCell::new(SceneState { current: 0 }));
    STATS.init(LvCell::new([SceneStats::default(); 17]));

    let _graphics = ove::thread!("graphics", graphics_entry, Priority::High, 4096);

    if lvgl::init().is_err() {
        ove::log_err!("Failed to init LVGL");
        return;
    }

    {
        let _g = lvgl::lock();

        // Section styles for the semi-circular gauge.
        STYLE_RED.init(Style::new().arc_color(Color::palette_main(Palette::Red as u32)));
        STYLE_BLUE.init(Style::new().arc_color(Color::palette_main(Palette::Indigo as u32)));
        STYLE_GREEN.init(Style::new().arc_color(Color::palette_main(Palette::Amber as u32)));

        let scr = lvgl::screen_active();
        scr.remove_style_all()
            .bg_opa(LV_OPA_COVER)
            .text_color(Color::black())
            .bg_color(Color::palette_lighten(Palette::Grey, 4))
            .pad_top(40)
            .pad_bottom(8)
            .pad_left(8)
            .pad_right(8)
            .pad_row(8)
            .pad_column(8);

        let top = lvgl::layer_top();
        let title = Label::create(top);
        title
            .bg_opa(LV_OPA_COVER)
            .bg_color(Color::white())
            .text_color(Color::black())
            .font(lvgl::font_montserrat_14())
            .width(lv_pct(100));

        load_scene(0);

        let scene_time = SCENES[0].scene_time;
        SCENE_TIMER.init(lvgl::Timer::new_fn(next_scene_tick, scene_time));

        if !perf_ffi::register_perf_overlay(title, perf_tick) {
            title.set_text(b"Perf monitor unavailable\0");
        }
    }

    ove::log_inf!("LVGL benchmark (Rust): running");
    ove::run();
}

ove::main!(app_main);
