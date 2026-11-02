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
#![deny(unsafe_code)]

use ove_allocator as _;

use core::ptr::addr_of;
use core::sync::atomic::{AtomicI32, Ordering};

use ove::lvgl::{
    self, Arc, Bar, Box, Button, Calendar, Canvas, Chart, Checkbox, Color, ColorFormat, Dropdown,
    EventCtx, EventTarget, FlexAlign, FlexFlow, Image, ImageSrc, Keyboard, Label, Layout, Led,
    List, LvCell, Msgbox, Obj, PART_MAIN, Roller, Series, Slider, Spinbox, Spinner, State,
    Styleable, Switch, TEXT_ALIGN_CENTER, Table, Tabview, Textarea,
};
use ove::{Priority, Thread, Timer};

// Auto-generated glue module — carries the lone `unsafe extern "C"` block
// for linking against LVGL-formatted C image assets. Narrowly scoped.
#[allow(unsafe_code)]
mod images {
    include!(concat!(
        env!("OVE_GEN_DIR"),
        "/generated_images/lvgl_images.rs"
    ));
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


/// Navigation state — single-threaded mutation under the LVGL lock.
#[derive(Clone, Copy)]
struct NavState {
    page: i32,
    content: Option<Obj>,
    title: Option<Label>,
}

ove::shared!(NAV: LvCell<NavState>);

// ---------------------------------------------------------------------------
// UI timer
// ---------------------------------------------------------------------------

fn ui_timer_cb() {
    let tick = TICK.fetch_add(1, Ordering::Relaxed) + 1;
    let _g = lvgl::lock();

    if let Some(bar) = BAR_W.try_get() {
        bar.set_value(tick % 101, false);
    }
    if let Some(arc) = ARC_W.try_get() {
        arc.value(tick % 101);
    }
    if let Some(led) = LED_W.try_get() {
        if tick % 10 == 0 {
            led.toggle();
        }
    }
    if let Some(series) = SERIES_W.try_get() {
        let v = ((tick * 3) % 100) as i32;
        series.next_value(v);
    }
    if let Some(state) = COUNTER_STATE.try_get() {
        state.set(tick);
    }
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

fn on_prev(nav: &LvCell<NavState>, _e: EventCtx<'_>) {
    nav.update(|s| NavState {
        page: (s.page + N_PAGES - 1) % N_PAGES,
        ..s
    });
    rebuild_page();
}

fn on_next(nav: &LvCell<NavState>, _e: EventCtx<'_>) {
    nav.update(|s| NavState {
        page: (s.page + 1) % N_PAGES,
        ..s
    });
    rebuild_page();
}

fn on_alert_click(_e: EventCtx<'_>) {
    let _ = Msgbox::create_modal()
        .add_title("Hello")
        .expect("LVGL text")
        .add_text("Message box from the gallery.")
        .expect("LVGL text")
        .add_close_button();
}

ove::event_handler!(NAV_PREV: LvCell<NavState> = &NAV, on_prev);
ove::event_handler!(NAV_NEXT: LvCell<NavState> = &NAV, on_next);

// ---------------------------------------------------------------------------
// Page builders
// ---------------------------------------------------------------------------

type PageFn = fn(Obj);

fn p_label(c: Obj) {
    let lbl = Label::create(c)
        .text("Hello, oveRTOS!")
        .expect("LVGL text")
        .font(lvgl::font_montserrat_32())
        .color(Color::white())
        .center();
    let _ = lbl;
    if let Some(state) = COUNTER_STATE.try_get() {
        Label::create(c)
            .color(Color::hex(0x888888))
            .font(lvgl::font_montserrat_14())
            .bind_text(state, |value, out| write!(out, "Tick: {value}"))
            .expect("LVGL text")
            .align(lvgl::ALIGN_BOTTOM_MID, 0, -16);
    }
}

fn p_button(c: Obj) {
    let btn = Button::create(c).size(160, 48).toggle_mode(true);
    Label::create(btn)
        .text("Toggle me")
        .expect("LVGL text")
        .center();
    btn.center();
}

fn p_switch(c: Obj) {
    Switch::create(c).checked(true).center();
}
fn p_checkbox(c: Obj) {
    Checkbox::create(c)
        .text("Enable option")
        .expect("LVGL text")
        .checked(true)
        .text_color(Color::white())
        .center();
}

fn p_bar(c: Obj) {
    let bar = Bar::create(c)
        .size(300, 20)
        .range(0, 100)
        .indicator_color(Color::palette_main(lvgl::PALETTE_BLUE))
        .radius(10)
        .center();
    BAR_W.init(bar);
}

fn p_slider(c: Obj) {
    Slider::create(c)
        .size(300, 20)
        .range(0, 100)
        .value(50)
        .indicator_color(Color::hex(0x4CAF50))
        .center();
}

fn p_arc(c: Obj) {
    let arc = Arc::create(c)
        .size(120, 120)
        .range(0, 100)
        .value(40)
        .indicator_color(Color::hex(0xFF9800))
        .center();
    ARC_W.init(arc);
}

fn p_spinner(c: Obj) {
    Spinner::create(c)
        .size(80, 80)
        .anim_params(1000, 60)
        .center();
}
fn p_led(c: Obj) {
    let led = Led::create(c)
        .size(60, 60)
        .color(Color::hex(0xF44336))
        .center();
    LED_W.init(led);
}
fn p_dropdown(c: Obj) {
    Dropdown::create(c)
        .options("Red\nGreen\nBlue\nYellow")
        .expect("LVGL text")
        .selected(2)
        .width(200)
        .center();
}
fn p_roller(c: Obj) {
    Roller::create(c)
        .options(
            "Mon\nTue\nWed\nThu\nFri\nSat\nSun",
            lvgl::ROLLER_MODE_NORMAL,
        )
        .expect("LVGL text")
        .visible_row_count(4)
        .width(140)
        .center();
}
fn p_spinbox(c: Obj) {
    Spinbox::create(c)
        .width(200)
        .digit_format(4, 2)
        .range(-9999, 9999)
        .step(1)
        .value(42)
        .center();
}

fn p_textarea(c: Obj) {
    c.flex_flow(FlexFlow::Column).pad_gap(8);
    let ta = Textarea::create(c)
        .one_line(true)
        .placeholder("Type here...")
        .expect("LVGL text")
        .max_length(40)
        .width(400);
    Keyboard::create(c).size(400, 140).attach(ta);
}

fn p_chart(c: Obj) {
    let chart = Chart::create(c)
        .size(400, 190)
        .chart_type(lvgl::CHART_TYPE_LINE)
        .point_count(60)
        .range(lvgl::CHART_AXIS_PRIMARY_Y, 0, 100)
        .update_mode(lvgl::CHART_UPDATE_MODE_SHIFT)
        .div_line_count(5, 6)
        .center();
    let series = chart.add_series(Color::hex(0x00BCD4), lvgl::CHART_AXIS_PRIMARY_Y);
    CHART_W.init(chart);
    SERIES_W.init(series);
}

fn p_table(c: Obj) {
    let t = Table::create(c).column_count(2).row_count(4);
    t.column_width(0, 120)
        .column_width(1, 120)
        .cell_value(0, 0, "Key")
        .expect("LVGL text")
        .cell_value(0, 1, "Value")
        .expect("LVGL text")
        .cell_value(1, 0, "Language")
        .expect("LVGL text")
        .cell_value(1, 1, "Rust")
        .expect("LVGL text")
        .cell_value(2, 0, "LVGL")
        .expect("LVGL text")
        .cell_value(2, 1, "9.2")
        .expect("LVGL text")
        .cell_value(3, 0, "RTOS")
        .expect("LVGL text")
        .cell_value(3, 1, "oveRTOS")
        .expect("LVGL text");
    t.center();
}

fn p_list(c: Obj) {
    let l = List::create(c).size(240, 160);
    l.add_text("Navigation").expect("LVGL text");
    l.add_button(None, "Settings").expect("LVGL text");
    l.add_button(None, "About").expect("LVGL text");
    l.add_button(None, "Help").expect("LVGL text");
    l.add_button(None, "Quit").expect("LVGL text");
    l.center();
}

fn p_image(c: Obj) {
    Image::create(c)
        .source(ImageSrc::from_dsc(addr_of!(images::badge)))
        .expect("LVGL text")
        .center();
}

fn p_canvas(c: Obj) {
    let canvas = Canvas::create(c)
        .size(32, 32)
        .allocate_buffer(32, 32, ColorFormat::RGB565)
        .expect("LVGL canvas pixels")
        .fill_bg(Color::hex(0x202020), 255);
    for y in 0..32i32 {
        for x in 0..32i32 {
            canvas.set_pixel(x, y, Color::make((x * 8) as u8, (y * 8) as u8, 128));
        }
    }
    canvas.center();
}

fn p_calendar(c: Obj) {
    Calendar::create(c)
        .size(240, 240)
        .today(2026, 4, 13)
        .showed(2026, 4)
        .center();
}

fn p_msgbox(c: Obj) {
    let btn = Button::create(c).size(200, 48).on_clicked(on_alert_click);
    Label::create(btn)
        .text("Show Msgbox")
        .expect("LVGL text")
        .center();
    btn.center();
}

fn p_box(c: Obj) {
    Box::create(c)
        .size(200, 120)
        .bg_color(Color::hex(0x1A237E))
        .bg_opa(255)
        .border_color(Color::white())
        .border_width(2)
        .radius(16)
        .pad_all(16)
        .center();
}

fn p_tabview(c: Obj) {
    let tv = Tabview::create(c).size(380, 180).center();
    tv.tab_bar_size(32);
    let t1 = tv.add_tab("Tab A").expect("LVGL text");
    Label::create(t1)
        .text("Content A")
        .expect("LVGL text")
        .center();
    let t2 = tv.add_tab("Tab B").expect("LVGL text");
    Label::create(t2)
        .text("Content B")
        .expect("LVGL text")
        .center();
}

// ---------------------------------------------------------------------------
// Page directory
// ---------------------------------------------------------------------------

static PAGES: &[(&str, PageFn)] = &[
    ("Label", p_label),
    ("Button", p_button),
    ("Switch", p_switch),
    ("Checkbox", p_checkbox),
    ("Bar", p_bar),
    ("Slider", p_slider),
    ("Arc", p_arc),
    ("Spinner", p_spinner),
    ("Led", p_led),
    ("Dropdown", p_dropdown),
    ("Roller", p_roller),
    ("Spinbox", p_spinbox),
    ("Text+Kbd", p_textarea),
    ("Chart", p_chart),
    ("Table", p_table),
    ("List", p_list),
    ("Image", p_image),
    ("Canvas", p_canvas),
    ("Calendar", p_calendar),
    ("Msgbox", p_msgbox),
    ("Box", p_box),
    ("Tabview", p_tabview),
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
    let nav = NAV.get().get();
    clear_live_widgets();
    if let Some(c) = nav.content {
        c.clean();
    }

    let page = nav.page as usize;
    let (_, build_fn) = PAGES[page];
    if let Some(c) = nav.content {
        build_fn(c);
    }

    if let Some(lbl) = nav.title {
        let name = PAGES[page].0;
        lbl.set_text_fmt(format_args!("{} ({}/{})", name, nav.page + 1, N_PAGES))
            .expect("LVGL text");
    }
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

fn create_ui() {
    let screen = lvgl::screen_active();
    screen.bg_color(Color::black());
    screen.flex_flow(FlexFlow::Column).pad_all(0).pad_gap(0);

    // ── Nav bar ──
    let nav = Box::create(screen)
        .size(480, 40)
        .bg_color(Color::hex(0x1A237E))
        .bg_opa(255)
        .radius(0)
        .flex_flow(FlexFlow::Row)
        .flex_align(
            FlexAlign::SpaceBetween,
            FlexAlign::Center,
            FlexAlign::Center,
        )
        .pad_left(4)
        .pad_right(4);

    let prev = Button::create(nav).size(40, 32).on_clicked_with(&NAV_PREV);
    Label::create(prev).text("<").expect("LVGL text").center();

    let title = Label::create(nav)
        .text("")
        .expect("LVGL text")
        .color(Color::white())
        .font(lvgl::font_montserrat_14())
        .flex_grow(1)
        .text_align(TEXT_ALIGN_CENTER, PART_MAIN);

    let next = Button::create(nav).size(40, 32).on_clicked_with(&NAV_NEXT);
    Label::create(next).text(">").expect("LVGL text").center();

    // ── Content ──
    let content = Box::create(screen)
        .size(480, 232)
        .bg_opa(0)
        .border_width(0)
        .pad_all(8)
        .flex_grow(1);

    NAV.init(LvCell::new(NavState {
        page: 0,
        content: Some(*content),
        title: Some(title),
    }));

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
        {
            let _g = lvgl::lock();
            lvgl::tick(elapsed_ms);
            lvgl::handler();
        }
        Thread::sleep_ms(33);
    }
}

// ---------------------------------------------------------------------------
// App entry
// ---------------------------------------------------------------------------

#[ove::main]
fn app_main() {
    ove::log::try_init();
    log::info!("LVGL gallery (Rust): init");

    // Detach the graphics worker: it runs for the program lifetime, so we
    // don't want its `Drop` to request-stop + join.  `detach()` is the
    // binding's recommended fire-and-forget over `core::mem::forget`.
    // 16 KB: the gallery's pages (charts, canvas, image transforms) recurse
    // deep in LVGL's draw pipeline — 4 KB overflows on the 32-bit sim display.
    ove::Thread::builder()
        .name(c"graphics")
        .priority(Priority::High)
        .stack_size(16384)
        .spawn(|_tok| {
            graphics_entry();
        })
        .expect("graphics thread spawn")
        .detach();

    {
        UI_TIMER.init(ove::timer!(ui_timer_cb, 100, false));
        if lvgl::init().is_err() {
            log::error!("Failed to init LVGL");
            return;
        }
        {
            let _g = lvgl::lock();
            create_ui();
        }
        log::info!("LVGL widgets created");
        if UI_TIMER.start().is_err() {
            log::error!("Failed to start UI timer");
            return;
        }
    }

    log::info!("LVGL gallery (Rust): ready");
    ove::run();
}
