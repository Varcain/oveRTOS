// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! LVGL Gallery (Rust) — one widget per page with top nav bar.
//!
//! 22 pages — one for each widget type in the C++/Rust/Zig binding.
//! Navigation: < title (N/22) > arrow buttons at the top. Clicking
//! an arrow cleans the content area and rebuilds with the next page.

#![cfg_attr(not(feature = "std"), no_std)]

use core::fmt::Write;
use core::sync::atomic::{AtomicI32, Ordering};

use ove::lvgl::{
    self, Arc, Bar, Box, Button, Calendar, Canvas, Chart, Checkbox, Color, Dropdown,
    Image, Keyboard, Label, Layout, Led, List, Msgbox, Obj, Roller, Series, Slider,
    Spinbox, Spinner, State, Styleable, Switch, Tabview, Table, Textarea,
};
use ove::{FmtBuf, Priority, Thread, Timer};

mod images {
    include!(concat!(env!("OVE_GEN_DIR"), "/generated_images/lvgl_images.rs"));
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

static TICK: AtomicI32 = AtomicI32::new(0);

ove::shared!(UI_TIMER: Timer);

ove::shared!(BAR_W: Bar);
ove::shared!(ARC_W: Arc);
ove::shared!(LED_W: Led);
ove::shared!(CHART_W: Chart);
ove::shared!(SERIES_W: Series);
ove::shared!(COUNTER_STATE: State<i32>);

static mut CANVAS_BUF: [u8; 64 * 64 * 4] = [0u8; 64 * 64 * 4];

// Nav state — single-threaded access from LVGL task.
static mut G_PAGE: i32 = 0;
static mut G_CONTENT: Option<Obj> = None;
static mut G_TITLE: Option<Label> = None;

// ---------------------------------------------------------------------------
// UI timer
// ---------------------------------------------------------------------------

fn ui_timer_cb() {
    let tick = TICK.fetch_add(1, Ordering::Relaxed) + 1;
    let _g = lvgl::lock();

    if let Some(bar) = BAR_W.try_get() { bar.set_value(tick % 101, false); }
    if let Some(arc) = ARC_W.try_get() { arc.value(tick % 101); }
    if let Some(led) = LED_W.try_get() { if tick % 10 == 0 { led.toggle(); } }
    if let Some(series) = SERIES_W.try_get() {
        let v = ((tick * 3) % 100) as i32;
        series.next_value(v);
    }
    if let Some(state) = COUNTER_STATE.try_get() { state.set(tick); }
}

// ---------------------------------------------------------------------------
// Event callbacks
// ---------------------------------------------------------------------------

unsafe extern "C" fn on_prev(_e: *mut ove::ffi::lv_event_t) {
    unsafe {
        G_PAGE = (G_PAGE + N_PAGES - 1) % N_PAGES;
        rebuild_page();
    }
}

unsafe extern "C" fn on_next(_e: *mut ove::ffi::lv_event_t) {
    unsafe {
        G_PAGE = (G_PAGE + 1) % N_PAGES;
        rebuild_page();
    }
}

unsafe extern "C" fn on_alert_click(_e: *mut ove::ffi::lv_event_t) {
    let _ = Msgbox::create_modal()
        .add_title(b"Hello\0")
        .add_text(b"Message box from the gallery.\0")
        .add_close_button();
}

// ---------------------------------------------------------------------------
// Page builders
// ---------------------------------------------------------------------------

type PageFn = fn(Obj);

fn p_label(c: Obj) {
    let lbl = Label::create(c)
        .text(b"Hello, oveRTOS!\0")
        .font(lvgl::font_montserrat_32())
        .color(Color::white())
        .center();
    let _ = lbl;
    if let Some(state) = COUNTER_STATE.try_get() {
        Label::create(c)
            .color(Color::hex(0x888888))
            .font(lvgl::font_montserrat_14())
            .bind_text(state, b"Tick: %d\0")
            .align(lvgl::ALIGN_BOTTOM_MID, 0, -16);
    }
}

fn p_button(c: Obj)   { Button::create(c).size(160, 48).toggle_mode(true).center(); let btn = Button::create(c).size(160, 48).toggle_mode(true); Label::create(btn).text(b"Toggle me\0").center(); btn.center(); unsafe { ove::ffi::lv_obj_delete(c.as_raw()); } }

// That was getting messy. Let me use a cleaner approach for all pages.

fn p_button_real(c: Obj) {
    let btn = Button::create(c).size(160, 48).toggle_mode(true);
    Label::create(btn).text(b"Toggle me\0").center();
    btn.center();
}

fn p_switch(c: Obj)   { Switch::create(c).checked(true).center(); }
fn p_checkbox(c: Obj)  { Checkbox::create(c).text(b"Enable option\0").checked(true).text_color(Color::white()).center(); }

fn p_bar(c: Obj) {
    let bar = Bar::create(c).size(300, 20).range(0, 100)
        .indicator_color(Color::palette_main(lvgl::PALETTE_BLUE)).radius(10).center();
    BAR_W.init(bar);
}

fn p_slider(c: Obj)   { Slider::create(c).size(300, 20).range(0, 100).value(50)
    .indicator_color(Color::hex(0x4CAF50)).center(); }

fn p_arc(c: Obj) {
    let arc = Arc::create(c).size(120, 120).range(0, 100).value(40)
        .indicator_color(Color::hex(0xFF9800)).center();
    ARC_W.init(arc);
}

fn p_spinner(c: Obj)   { Spinner::create(c).size(80, 80).anim_params(1000, 60).center(); }
fn p_led(c: Obj)       { let led = Led::create(c).size(60, 60).color(Color::hex(0xF44336)).center(); LED_W.init(led); }
fn p_dropdown(c: Obj)  { Dropdown::create(c).options_static(b"Red\nGreen\nBlue\nYellow\0").selected(2).width(200).center(); }
fn p_roller(c: Obj)    { Roller::create(c).options(b"Mon\nTue\nWed\nThu\nFri\nSat\nSun\0", lvgl::ROLLER_MODE_NORMAL).visible_row_count(4).width(140).center(); }
fn p_spinbox(c: Obj)   { Spinbox::create(c).width(200).digit_format(4, 2).range(-9999, 9999).step(1).value(42).center(); }

fn p_textarea(c: Obj) {
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(c.as_raw(), ove::ffi::LV_FLEX_FLOW_COLUMN);
        ove::ffi::lv_obj_set_style_pad_row(c.as_raw(), 8, 0);
    }
    let ta = Textarea::create(c).one_line(true).placeholder(b"Type here...\0").max_length(40).width(400);
    Keyboard::create(c).size(400, 140).attach(ta);
}

fn p_chart(c: Obj) {
    let chart = Chart::create(c).size(400, 190).chart_type(lvgl::CHART_TYPE_LINE)
        .point_count(60).range(lvgl::CHART_AXIS_PRIMARY_Y, 0, 100)
        .update_mode(lvgl::CHART_UPDATE_MODE_SHIFT).div_line_count(5, 6).center();
    let series = chart.add_series(Color::hex(0x00BCD4), lvgl::CHART_AXIS_PRIMARY_Y);
    CHART_W.init(chart);
    SERIES_W.init(series);
}

fn p_table(c: Obj) {
    let t = Table::create(c).column_count(2).row_count(4);
    t.column_width(0, 120).column_width(1, 120)
     .cell_value(0, 0, b"Key\0").cell_value(0, 1, b"Value\0")
     .cell_value(1, 0, b"Language\0").cell_value(1, 1, b"Rust\0")
     .cell_value(2, 0, b"LVGL\0").cell_value(2, 1, b"9.2\0")
     .cell_value(3, 0, b"RTOS\0").cell_value(3, 1, b"oveRTOS\0");
    t.center();
}

fn p_list(c: Obj) {
    let l = List::create(c).size(240, 160);
    l.add_text(b"Navigation\0");
    l.add_button(core::ptr::null(), b"Settings\0");
    l.add_button(core::ptr::null(), b"About\0");
    l.add_button(core::ptr::null(), b"Help\0");
    l.add_button(core::ptr::null(), b"Quit\0");
    l.center();
}

fn p_image(c: Obj) {
    unsafe {
        let badge_ptr = &images::badge as *const _ as *const core::ffi::c_void;
        Image::create(c).src(badge_ptr).center();
    }
}

fn p_canvas(c: Obj) {
    let canvas = Canvas::create(c).size(64, 64);
    unsafe {
        let buf_ptr = core::ptr::addr_of_mut!(CANVAS_BUF) as *mut core::ffi::c_void;
        canvas.buffer(buf_ptr, 64, 64, ove::ffi::LV_COLOR_FORMAT_XRGB8888);
    }
    canvas.fill_bg(Color::hex(0x202020), 255);
    for y in 0..64i32 { for x in 0..64i32 {
        canvas.set_pixel(x, y, Color::make((x * 4) as u8, (y * 4) as u8, 128));
    }}
    canvas.center();
}

fn p_calendar(c: Obj) { Calendar::create(c).size(240, 240).today(2026, 4, 13).showed(2026, 4).center(); }

fn p_msgbox(c: Obj) {
    let btn = Button::create(c).size(200, 48);
    Label::create(btn).text(b"Show Msgbox\0").center();
    unsafe {
        ove::ffi::lv_obj_add_event_cb(btn.as_raw() as *mut _, Some(on_alert_click), ove::ffi::LV_EVENT_CLICKED, core::ptr::null_mut());
    }
    btn.center();
}

fn p_box(c: Obj) {
    Box::create(c).size(200, 120).bg_color(Color::hex(0x1A237E)).bg_opa(255)
        .border_color(Color::white()).border_width(2).radius(16).pad_all(16).center();
}

fn p_tabview(c: Obj) {
    let tv = Tabview::create(c).size(380, 180).center();
    tv.tab_bar_size(32);
    let t1 = tv.add_tab(b"Tab A\0");
    Label::create(t1).text(b"Content A\0").center();
    let t2 = tv.add_tab(b"Tab B\0");
    Label::create(t2).text(b"Content B\0").center();
}

// ---------------------------------------------------------------------------
// Page directory
// ---------------------------------------------------------------------------

static PAGES: &[(&[u8], PageFn)] = &[
    (b"Label\0",    p_label),
    (b"Button\0",   p_button_real),
    (b"Switch\0",   p_switch),
    (b"Checkbox\0", p_checkbox),
    (b"Bar\0",      p_bar),
    (b"Slider\0",   p_slider),
    (b"Arc\0",      p_arc),
    (b"Spinner\0",  p_spinner),
    (b"Led\0",      p_led),
    (b"Dropdown\0", p_dropdown),
    (b"Roller\0",   p_roller),
    (b"Spinbox\0",  p_spinbox),
    (b"Text+Kbd\0", p_textarea),
    (b"Chart\0",    p_chart),
    (b"Table\0",    p_table),
    (b"List\0",     p_list),
    (b"Image\0",    p_image),
    (b"Canvas\0",   p_canvas),
    (b"Calendar\0", p_calendar),
    (b"Msgbox\0",   p_msgbox),
    (b"Box\0",      p_box),
    (b"Tabview\0",  p_tabview),
];

const N_PAGES: i32 = 22;

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

fn clear_live_widgets() {
    BAR_W.shutdown();
    ARC_W.shutdown();
    LED_W.shutdown();
    CHART_W.shutdown();
    SERIES_W.shutdown();
}

fn rebuild_page() {
    unsafe {
        clear_live_widgets();
        if let Some(c) = G_CONTENT { c.clean(); }

        let page = G_PAGE as usize;
        let (_, build_fn) = PAGES[page];
        if let Some(c) = G_CONTENT { build_fn(c); }

        if let Some(lbl) = G_TITLE {
            let mut buf = [0u8; 48];
            let mut w = FmtBuf::new(&mut buf);
            let name = core::str::from_utf8(&PAGES[page].0[..PAGES[page].0.len()-1]).unwrap_or("?");
            let _ = write!(w, "{} ({}/{})", name, G_PAGE + 1, N_PAGES);
            lbl.set_text(w.as_cstr());
        }
    }
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

fn create_ui() {
    let screen = lvgl::screen_active();
    screen.bg_color(Color::black());
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(screen.as_raw(), ove::ffi::LV_FLEX_FLOW_COLUMN);
        ove::ffi::lv_obj_set_style_pad_top(screen.as_raw(), 0, 0);
        ove::ffi::lv_obj_set_style_pad_row(screen.as_raw(), 0, 0);
    }

    // ── Nav bar ──
    let nav = Box::create(screen).size(480, 40).bg_color(Color::hex(0x1A237E)).bg_opa(255).radius(0);
    unsafe {
        ove::ffi::lv_obj_set_flex_flow(nav.as_raw(), ove::ffi::LV_FLEX_FLOW_ROW);
        ove::ffi::lv_obj_set_flex_align(nav.as_raw(), 3, 2, 2); // SPACE_BETWEEN, CENTER, CENTER
        ove::ffi::lv_obj_set_style_pad_left(nav.as_raw(), 4, 0);
        ove::ffi::lv_obj_set_style_pad_right(nav.as_raw(), 4, 0);
    }

    let prev = Button::create(nav).size(40, 32);
    Label::create(prev).text(b"<\0").center();
    unsafe { ove::ffi::lv_obj_add_event_cb(prev.as_raw() as *mut _, Some(on_prev), ove::ffi::LV_EVENT_CLICKED, core::ptr::null_mut()); }

    let title = Label::create(nav).text(b"\0").color(Color::white()).font(lvgl::font_montserrat_14());
    unsafe {
        ove::ffi::lv_obj_set_flex_grow(title.as_raw() as *mut _, 1);
        ove::ffi::lv_obj_set_style_text_align(title.as_raw() as *mut _, 2, 0); // CENTER
        G_TITLE = Some(title);
    }

    let next = Button::create(nav).size(40, 32);
    Label::create(next).text(b">\0").center();
    unsafe { ove::ffi::lv_obj_add_event_cb(next.as_raw() as *mut _, Some(on_next), ove::ffi::LV_EVENT_CLICKED, core::ptr::null_mut()); }

    // ── Content ──
    let content = Box::create(screen)
        .size(480, 232)
        .bg_opa(0)
        .border_width(0)
        .pad_all(8);
    unsafe {
        ove::ffi::lv_obj_set_flex_grow(content.as_raw(), 1);
        G_CONTENT = Some(Obj::from_raw(content.as_raw()));
    }

    // State for reactive label
    COUNTER_STATE.init(State::<i32>::new(0));

    rebuild_page();
}

// ---------------------------------------------------------------------------
// Graphics thread
// ---------------------------------------------------------------------------

fn graphics_entry() {
    let mut last_us = ove::time::get_us().unwrap_or(0);
    loop {
        let now_us = ove::time::get_us().unwrap_or(last_us);
        let elapsed_ms = ((now_us - last_us) / 1000) as u32;
        last_us = now_us;
        { let _g = lvgl::lock(); lvgl::tick(elapsed_ms); lvgl::handler(); }
        Thread::sleep_ms(33);
    }
}

// ---------------------------------------------------------------------------
// App entry
// ---------------------------------------------------------------------------

fn app_main() {
    ove::log_inf!("LVGL gallery (Rust): init");

    let _graphics = ove::thread!("graphics", graphics_entry, Priority::High, 4096);

    {
        UI_TIMER.init(ove::timer!(ui_timer_cb, 100, false));
        if lvgl::init().is_err() { ove::log_err!("Failed to init LVGL"); return; }
        { let _g = lvgl::lock(); create_ui(); }
        ove::log_inf!("LVGL widgets created");
        if UI_TIMER.start().is_err() { ove::log_err!("Failed to start UI timer"); return; }
    }

    ove::log_inf!("LVGL gallery (Rust): ready");
    ove::run();
}

ove::main!(app_main);
