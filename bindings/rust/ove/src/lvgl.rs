// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Safe LVGL v9 wrappers for the oveRTOS Rust SDK.
//!
//! Provides idiomatic Rust bindings matching the C++ `ove::lvgl` wrapper:
//!
//! - **Zero-cost abstractions** — every widget is `Copy` + pointer-sized
//! - **Fluent API** — method chaining via `self -> Self` on `Copy` types
//! - **Trait composition** — `Layout`, `Styleable`, `EventTarget` blanket-impl
//!   on anything implementing `Widget`, replacing C++ CRTP mixins
//! - **RAII** — `LvglGuard` for lock/unlock, `Style` with `Drop`
//! - **`no_std` compatible** — no allocator needed

use crate::bindings;
use crate::error::{Error, Result};

// =========================================================================
//  Constants
// =========================================================================

/// LVGL alignment constant: center the widget relative to its parent.
pub const ALIGN_CENTER: u8 = 9;
/// LVGL alignment constant: align to the top-center of the parent.
pub const ALIGN_TOP_MID: u8 = 2;
/// LVGL alignment constant: align to the top-left of the parent.
pub const ALIGN_TOP_LEFT: u8 = 1;

/// LVGL style selector for the main (background) part of a widget.
pub const PART_MAIN: u32 = 0x000000;
/// LVGL style selector for the indicator part (e.g. bar fill, checkbox mark).
pub const PART_INDICATOR: u32 = 0x010000;

/// LVGL v9 `LV_SIZE_CONTENT` — sets widget to size-to-content mode.
/// Computed from: `LV_COORD_SET_SPEC(LV_COORD_MAX)` where
/// `LV_COORD_TYPE_SHIFT = 29`, giving `((1<<29)-1) | (1<<29)`.
pub const SIZE_CONTENT: i32 = 0x3FFF_FFFF;

// =========================================================================
//  Color
// =========================================================================

/// RGB888 color matching `lv_color_t { blue, green, red }` layout.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Color {
    pub blue: u8,
    pub green: u8,
    pub red: u8,
}

impl Color {
    /// Construct a color from red, green, blue components (0–255 each).
    pub const fn make(r: u8, g: u8, b: u8) -> Self {
        Self {
            blue: b,
            green: g,
            red: r,
        }
    }

    /// Return pure white (R=255, G=255, B=255).
    pub const fn white() -> Self {
        Self::make(255, 255, 255)
    }

    /// Return pure black (R=0, G=0, B=0).
    pub const fn black() -> Self {
        Self::make(0, 0, 0)
    }

    /// Construct a color from a packed 24-bit RGB hex value (e.g. `0xFF8800`).
    pub fn hex(hex: u32) -> Self {
        Self::make(
            ((hex >> 16) & 0xFF) as u8,
            ((hex >> 8) & 0xFF) as u8,
            (hex & 0xFF) as u8,
        )
    }

    /// Return the main (500-shade) color of an LVGL palette family.
    ///
    /// `p` must be one of the `PALETTE_*` constants (e.g. [`PALETTE_BLUE`]).
    pub fn palette_main(p: u32) -> Self {
        unsafe {
            let c = bindings::lv_palette_main(p as _);
            core::mem::transmute(c)
        }
    }

    fn to_raw(self) -> bindings::lv_color_t {
        unsafe { core::mem::transmute(self) }
    }
}

/// LVGL palette index for the blue palette family (`LV_PALETTE_BLUE`).
pub const PALETTE_BLUE: u32 = 6;

// =========================================================================
//  Font accessors
// =========================================================================

/// Safe pointer to `lv_font_montserrat_32`.
pub fn font_montserrat_32() -> *const bindings::lv_font_t {
    unsafe { &bindings::lv_font_montserrat_32 }
}

/// Safe pointer to `lv_font_montserrat_14`.
pub fn font_montserrat_14() -> *const bindings::lv_font_t {
    unsafe { &bindings::lv_font_montserrat_14 }
}

// =========================================================================
//  Widget trait — core abstraction replacing C++ ObjectView
// =========================================================================

/// Core trait for all LVGL widget wrappers. Provides access to the raw
/// `lv_obj_t` pointer. All higher-level traits (`Layout`, `Styleable`,
/// `EventTarget`) are blanket-implemented for any type implementing `Widget`.
///
/// # Safety contract
/// Implementors must ensure `raw()` returns a pointer obtained from LVGL.
/// All access must happen under the LVGL lock.
pub trait Widget: Copy {
    /// Return the raw `lv_obj_t` pointer for this widget.
    ///
    /// # Safety
    /// Must only be used while holding the LVGL lock (see [`lock`]).
    fn raw(self) -> *mut bindings::lv_obj_t;
}

// =========================================================================
//  Layout trait — fluent positioning/sizing (blanket impl on Widget)
// =========================================================================

/// Fluent positioning, sizing, and flag manipulation.
/// Blanket-implemented for all `Widget` types.
pub trait Layout: Widget + Sized {
    /// Set both width and height of the widget in pixels.
    fn size(self, w: i32, h: i32) -> Self {
        unsafe { bindings::lv_obj_set_size(self.raw(), w, h) };
        self
    }

    /// Set the width of the widget in pixels.
    fn width(self, w: i32) -> Self {
        unsafe { bindings::lv_obj_set_width(self.raw(), w) };
        self
    }

    /// Set the height of the widget in pixels.
    fn height(self, h: i32) -> Self {
        unsafe { bindings::lv_obj_set_height(self.raw(), h) };
        self
    }

    /// Set the position of the widget relative to its parent's top-left corner.
    fn pos(self, x: i32, y: i32) -> Self {
        unsafe { bindings::lv_obj_set_pos(self.raw(), x, y) };
        self
    }

    /// Center the widget within its parent.
    fn center(self) -> Self {
        unsafe { bindings::lv_obj_center(self.raw()) };
        self
    }

    /// Align the widget using an LVGL alignment constant and pixel offsets.
    ///
    /// `a` must be one of the `ALIGN_*` constants (e.g. [`ALIGN_CENTER`]).
    fn align(self, a: u8, x_ofs: i32, y_ofs: i32) -> Self {
        unsafe { bindings::lv_obj_align(self.raw(), a as _, x_ofs, y_ofs) };
        self
    }

    /// Hide the widget by setting `LV_OBJ_FLAG_HIDDEN`.
    fn hide(self) -> Self {
        unsafe { bindings::lv_obj_add_flag(self.raw(), bindings::LV_OBJ_FLAG_HIDDEN) };
        self
    }

    /// Make the widget visible by removing `LV_OBJ_FLAG_HIDDEN`.
    fn show(self) -> Self {
        unsafe { bindings::lv_obj_remove_flag(self.raw(), bindings::LV_OBJ_FLAG_HIDDEN) };
        self
    }

    /// Show or hide the widget. Equivalent to calling [`show`](Layout::show) or [`hide`](Layout::hide).
    fn visible(self, v: bool) -> Self {
        if v { self.show() } else { self.hide() }
    }

    /// Add one or more LVGL object flags (bitwise OR of `LV_OBJ_FLAG_*` values).
    fn add_flag(self, f: u32) -> Self {
        unsafe { bindings::lv_obj_add_flag(self.raw(), f) };
        self
    }

    /// Remove one or more LVGL object flags.
    fn remove_flag(self, f: u32) -> Self {
        unsafe { bindings::lv_obj_remove_flag(self.raw(), f) };
        self
    }

    /// Add one or more LVGL object state bits (e.g. `LV_STATE_CHECKED`).
    fn add_state(self, s: u32) -> Self {
        unsafe { bindings::lv_obj_add_state(self.raw(), s as _) };
        self
    }

    /// Remove one or more LVGL object state bits.
    fn remove_state(self, s: u32) -> Self {
        unsafe { bindings::lv_obj_remove_state(self.raw(), s as _) };
        self
    }

    /// Enable or disable click events on the widget.
    fn clickable(self, on: bool) -> Self {
        if on {
            self.add_flag(bindings::LV_OBJ_FLAG_CLICKABLE)
        } else {
            self.remove_flag(bindings::LV_OBJ_FLAG_CLICKABLE)
        }
    }
}

impl<T: Widget> Layout for T {}

// =========================================================================
//  Styleable trait — fluent inline style setters (blanket impl on Widget)
// =========================================================================

/// Fluent inline style setters. Applied to `LV_PART_MAIN` by default.
/// Blanket-implemented for all `Widget` types.
pub trait Styleable: Widget + Sized {
    /// Set the background color of the widget's main part.
    fn bg_color(self, c: Color) -> Self {
        unsafe { bindings::lv_obj_set_style_bg_color(self.raw(), c.to_raw(), PART_MAIN) };
        self
    }

    /// Set the background opacity of the widget's main part (0 = transparent, 255 = opaque).
    fn bg_opa(self, opa: u8) -> Self {
        unsafe { bindings::lv_obj_set_style_bg_opa(self.raw(), opa, PART_MAIN) };
        self
    }

    /// Set the border color of the widget's main part.
    fn border_color(self, c: Color) -> Self {
        unsafe { bindings::lv_obj_set_style_border_color(self.raw(), c.to_raw(), PART_MAIN) };
        self
    }

    /// Set the border width in pixels.
    fn border_width(self, w: i32) -> Self {
        unsafe { bindings::lv_obj_set_style_border_width(self.raw(), w, PART_MAIN) };
        self
    }

    /// Set the corner radius in pixels (use `LV_RADIUS_CIRCLE` for a pill shape).
    fn radius(self, r: i32) -> Self {
        unsafe { bindings::lv_obj_set_style_radius(self.raw(), r, PART_MAIN) };
        self
    }

    /// Set equal padding on all four sides.
    fn pad_all(self, p: i32) -> Self {
        unsafe {
            let r = self.raw();
            bindings::lv_obj_set_style_pad_left(r, p, PART_MAIN);
            bindings::lv_obj_set_style_pad_right(r, p, PART_MAIN);
            bindings::lv_obj_set_style_pad_top(r, p, PART_MAIN);
            bindings::lv_obj_set_style_pad_bottom(r, p, PART_MAIN);
        }
        self
    }

    /// Set equal left and right (horizontal) padding.
    fn pad_hor(self, p: i32) -> Self {
        unsafe {
            let r = self.raw();
            bindings::lv_obj_set_style_pad_left(r, p, PART_MAIN);
            bindings::lv_obj_set_style_pad_right(r, p, PART_MAIN);
        }
        self
    }

    /// Set equal top and bottom (vertical) padding.
    fn pad_ver(self, p: i32) -> Self {
        unsafe {
            let r = self.raw();
            bindings::lv_obj_set_style_pad_top(r, p, PART_MAIN);
            bindings::lv_obj_set_style_pad_bottom(r, p, PART_MAIN);
        }
        self
    }

    /// Set the row and column gap between flex children.
    fn pad_gap(self, g: i32) -> Self {
        unsafe {
            let r = self.raw();
            bindings::lv_obj_set_style_pad_row(r, g, PART_MAIN);
            bindings::lv_obj_set_style_pad_column(r, g, PART_MAIN);
        }
        self
    }

    /// Set the text color for label-like widgets.
    fn text_color(self, c: Color) -> Self {
        unsafe { bindings::lv_obj_set_style_text_color(self.raw(), c.to_raw(), PART_MAIN) };
        self
    }

    /// Set the text font. Use [`font_montserrat_32`] or [`font_montserrat_14`] for built-ins.
    fn text_font(self, f: *const bindings::lv_font_t) -> Self {
        unsafe { bindings::lv_obj_set_style_text_font(self.raw(), f, PART_MAIN) };
        self
    }

    /// Apply a reusable [`Style`] to the given style selector (part + state bitmask).
    fn add_style(self, style: &mut Style, selector: u32) -> Self {
        unsafe { bindings::lv_obj_add_style(self.raw(), style.as_mut_ptr(), selector) };
        self
    }
}

impl<T: Widget> Styleable for T {}

// =========================================================================
//  EventTarget trait — type-safe event callbacks (blanket impl on Widget)
// =========================================================================

/// Event callback registration. Uses `fn` pointers for `no_std` compatibility.
/// Blanket-implemented for all `Widget` types.
pub trait EventTarget: Widget + Sized {
    /// Register a callback for an arbitrary LVGL event code.
    ///
    /// `code` is any `LV_EVENT_*` constant. `user_data` is passed through to `cb` unchanged.
    fn on(self, code: u32, cb: bindings::lv_event_cb_t, user_data: *mut core::ffi::c_void) -> Self {
        unsafe { bindings::lv_obj_add_event_cb(self.raw(), cb, code, user_data) };
        self
    }

    /// Register a callback for click (`LV_EVENT_CLICKED`) events.
    fn on_click(self, cb: bindings::lv_event_cb_t, user_data: *mut core::ffi::c_void) -> Self {
        self.on(bindings::LV_EVENT_CLICKED, cb, user_data)
    }

    /// Register a callback for value-changed (`LV_EVENT_VALUE_CHANGED`) events.
    fn on_value_changed(self, cb: bindings::lv_event_cb_t, user_data: *mut core::ffi::c_void) -> Self {
        self.on(bindings::LV_EVENT_VALUE_CHANGED, cb, user_data)
    }
}

impl<T: Widget> EventTarget for T {}

// =========================================================================
//  Obj — base widget wrapper
// =========================================================================

/// Non-owning handle to an LVGL object (`lv_obj_t *`).
///
/// LVGL manages object lifetimes (parent owns children). No `Drop`.
/// `Copy` enables fluent method chaining (`self -> Self`).
#[derive(Clone, Copy)]
pub struct Obj {
    raw: *mut bindings::lv_obj_t,
}

impl Obj {
    /// Wrap a raw LVGL object pointer.
    ///
    /// # Safety
    /// The pointer must be valid and obtained from an LVGL function.
    pub unsafe fn from_raw(raw: *mut bindings::lv_obj_t) -> Self {
        Self { raw }
    }

    /// Get the raw pointer (for passing to FFI).
    pub fn as_raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }

    /// Create a plain container object.
    pub fn create(parent: Obj) -> Self {
        let raw = unsafe { bindings::lv_obj_create(parent.raw) };
        Self { raw }
    }

    /// Delete this object and all its children.
    pub fn del(self) {
        unsafe { bindings::lv_obj_delete(self.raw) };
    }

    /// Delete all children of this object.
    pub fn clean(self) {
        unsafe { bindings::lv_obj_clean(self.raw) };
    }

    /// Get parent object.
    pub fn parent(self) -> Self {
        Self {
            raw: unsafe { bindings::lv_obj_get_parent(self.raw) },
        }
    }

    /// Get number of children.
    pub fn child_count(self) -> u32 {
        unsafe { bindings::lv_obj_get_child_count(self.raw) }
    }

    /// Get current width.
    pub fn get_width(self) -> i32 {
        unsafe { bindings::lv_obj_get_width(self.raw) }
    }

    /// Get current height.
    pub fn get_height(self) -> i32 {
        unsafe { bindings::lv_obj_get_height(self.raw) }
    }

    /// Set user data pointer.
    pub fn set_user_data(self, data: *mut core::ffi::c_void) -> Self {
        unsafe { bindings::lv_obj_set_user_data(self.raw, data) };
        self
    }

    /// Get user data pointer.
    pub fn get_user_data(self) -> *mut core::ffi::c_void {
        unsafe { bindings::lv_obj_get_user_data(self.raw) }
    }

    // Legacy API preserved for backward compatibility
    /// Set the size of the widget (legacy, non-fluent variant).
    pub fn set_size(self, w: i32, h: i32) {
        unsafe { bindings::lv_obj_set_size(self.raw, w, h) };
    }

    /// Set the position of the widget (legacy, non-fluent variant).
    pub fn set_pos(self, x: i32, y: i32) {
        unsafe { bindings::lv_obj_set_pos(self.raw, x, y) };
    }

    /// Set the background color for the given selector (legacy, non-fluent variant).
    pub fn set_style_bg_color(self, color: Color, selector: u32) {
        unsafe { bindings::lv_obj_set_style_bg_color(self.raw, color.to_raw(), selector) };
    }

    /// Set the text color for the given selector (legacy, non-fluent variant).
    pub fn set_style_text_color(self, color: Color, selector: u32) {
        unsafe { bindings::lv_obj_set_style_text_color(self.raw, color.to_raw(), selector) };
    }

    /// Set the corner radius for the given selector (legacy, non-fluent variant).
    pub fn set_style_radius(self, radius: i32, selector: u32) {
        unsafe { bindings::lv_obj_set_style_radius(self.raw, radius, selector) };
    }

    /// Set the text font for the given selector (legacy, non-fluent variant).
    pub fn set_style_text_font(self, font: *const bindings::lv_font_t, selector: u32) {
        unsafe { bindings::lv_obj_set_style_text_font(self.raw, font, selector) };
    }
}

impl Widget for Obj {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

// =========================================================================
//  Label
// =========================================================================

/// LVGL label widget.
#[derive(Clone, Copy)]
pub struct Label {
    raw: *mut bindings::lv_obj_t,
}

impl Label {
    /// Create a new label as a child of `parent`.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_label_create(parent.raw()) };
        Self { raw }
    }

    /// Set text from a null-terminated byte slice.
    pub fn set_text(self, text: &[u8]) {
        unsafe { bindings::lv_label_set_text(self.raw, text.as_ptr() as *const _) };
    }

    /// Fluent: set text (copies string).
    pub fn text(self, txt: &[u8]) -> Self {
        unsafe { bindings::lv_label_set_text(self.raw, txt.as_ptr() as *const _) };
        self
    }

    /// Fluent: set static text (pointer must remain valid).
    pub fn text_static(self, txt: &'static [u8]) -> Self {
        unsafe { bindings::lv_label_set_text_static(self.raw, txt.as_ptr() as *const _) };
        self
    }

    /// Fluent: set font shorthand.
    pub fn font(self, f: *const bindings::lv_font_t) -> Self {
        unsafe { bindings::lv_obj_set_style_text_font(self.raw, f, PART_MAIN) };
        self
    }

    /// Fluent: set text color shorthand.
    pub fn color(self, c: Color) -> Self {
        unsafe { bindings::lv_obj_set_style_text_color(self.raw, c.to_raw(), PART_MAIN) };
        self
    }
}

impl Widget for Label {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Label {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        // SAFETY: Label and Obj have identical layout (single *mut lv_obj_t)
        unsafe { &*(self as *const Label as *const Obj) }
    }
}

// =========================================================================
//  Bar
// =========================================================================

/// LVGL bar widget.
#[derive(Clone, Copy)]
pub struct Bar {
    raw: *mut bindings::lv_obj_t,
}

impl Bar {
    /// Create a new bar widget as a child of `parent`.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_bar_create(parent.raw()) };
        Self { raw }
    }

    /// Set bar value. `anim`: false = instant, true = animate.
    pub fn set_value(self, value: i32, anim: bool) {
        unsafe {
            #[allow(clippy::unnecessary_cast)]
            bindings::lv_bar_set_value(self.raw, value, anim as _);
        }
    }

    /// Set bar range.
    pub fn set_range(self, min: i32, max: i32) {
        unsafe { bindings::lv_bar_set_range(self.raw, min, max) };
    }

    /// Fluent: set value with animation.
    pub fn value(self, val: i32) -> Self {
        unsafe {
            #[allow(clippy::unnecessary_cast)]
            bindings::lv_bar_set_value(self.raw, val, true as _);
        }
        self
    }

    /// Fluent: set value with animation control.
    pub fn value_anim(self, val: i32, anim: bool) -> Self {
        unsafe {
            #[allow(clippy::unnecessary_cast)]
            bindings::lv_bar_set_value(self.raw, val, anim as _);
        }
        self
    }

    /// Fluent: set range.
    pub fn range(self, min: i32, max: i32) -> Self {
        unsafe { bindings::lv_bar_set_range(self.raw, min, max) };
        self
    }

    /// Fluent: set indicator color.
    pub fn indicator_color(self, c: Color) -> Self {
        unsafe { bindings::lv_obj_set_style_bg_color(self.raw, c.to_raw(), PART_INDICATOR) };
        self
    }

    /// Fluent: set bar background color.
    pub fn bar_color(self, c: Color) -> Self {
        unsafe { bindings::lv_obj_set_style_bg_color(self.raw, c.to_raw(), PART_MAIN) };
        self
    }
}

impl Widget for Bar {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Bar {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Bar as *const Obj) }
    }
}

// =========================================================================
//  Box — container without scrollbar
// =========================================================================

/// Plain container object with scrolling disabled.
#[derive(Clone, Copy)]
pub struct Box {
    raw: *mut bindings::lv_obj_t,
}

impl Box {
    /// Create a plain container widget (scrolling disabled) as a child of `parent`.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe {
            let obj = bindings::lv_obj_create(parent.raw());
            bindings::lv_obj_remove_flag(obj, bindings::LV_OBJ_FLAG_SCROLLABLE);
            obj
        };
        Self { raw }
    }
}

impl Widget for Box {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Box {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Box as *const Obj) }
    }
}

// =========================================================================
//  Layout helpers — vbox / hbox
// =========================================================================

/// Create a vertical flex container.
pub fn vbox(parent: impl Widget) -> Box {
    let b = Box::create(parent);
    unsafe {
        bindings::lv_obj_set_flex_flow(b.raw, bindings::LV_FLEX_FLOW_COLUMN);
        bindings::lv_obj_set_size(b.raw, SIZE_CONTENT, SIZE_CONTENT);
    }
    b
}

/// Create a horizontal flex container.
pub fn hbox(parent: impl Widget) -> Box {
    let b = Box::create(parent);
    unsafe {
        bindings::lv_obj_set_flex_flow(b.raw, bindings::LV_FLEX_FLOW_ROW);
        bindings::lv_obj_set_size(b.raw, SIZE_CONTENT, SIZE_CONTENT);
    }
    b
}

// =========================================================================
//  Style — RAII style object
// =========================================================================

/// RAII wrapper around `lv_style_t`. Calls `lv_style_reset` on drop.
pub struct Style {
    inner: bindings::lv_style_t,
}

impl Style {
    /// Create and initialize a new LVGL style object.
    pub fn new() -> Self {
        let mut s = Self {
            inner: unsafe { core::mem::zeroed() },
        };
        unsafe { bindings::lv_style_init(&mut s.inner) };
        s
    }

    /// Return a raw mutable pointer for use with [`Styleable::add_style`].
    pub fn as_mut_ptr(&mut self) -> *mut bindings::lv_style_t {
        &mut self.inner
    }

    /// Set the background color in this style.
    pub fn bg_color(mut self, c: Color) -> Self {
        unsafe { bindings::lv_style_set_bg_color(&mut self.inner, c.to_raw()) };
        self
    }

    /// Set the background opacity in this style (0 = transparent, 255 = opaque).
    pub fn bg_opa(mut self, opa: u8) -> Self {
        unsafe { bindings::lv_style_set_bg_opa(&mut self.inner, opa) };
        self
    }

    /// Set the corner radius in this style.
    pub fn radius(mut self, r: i32) -> Self {
        unsafe { bindings::lv_style_set_radius(&mut self.inner, r) };
        self
    }

    /// Set the border color in this style.
    pub fn border_color(mut self, c: Color) -> Self {
        unsafe { bindings::lv_style_set_border_color(&mut self.inner, c.to_raw()) };
        self
    }

    /// Set the border width in this style.
    pub fn border_width(mut self, w: i32) -> Self {
        unsafe { bindings::lv_style_set_border_width(&mut self.inner, w) };
        self
    }

    /// Set equal padding on all four sides in this style.
    pub fn pad_all(mut self, p: i32) -> Self {
        unsafe {
            let s = &mut self.inner;
            bindings::lv_style_set_pad_left(s, p);
            bindings::lv_style_set_pad_right(s, p);
            bindings::lv_style_set_pad_top(s, p);
            bindings::lv_style_set_pad_bottom(s, p);
        }
        self
    }

    /// Set the text color in this style.
    pub fn text_color(mut self, c: Color) -> Self {
        unsafe { bindings::lv_style_set_text_color(&mut self.inner, c.to_raw()) };
        self
    }

    /// Set the text font in this style.
    pub fn text_font(mut self, f: *const bindings::lv_font_t) -> Self {
        unsafe { bindings::lv_style_set_text_font(&mut self.inner, f) };
        self
    }
}

impl Drop for Style {
    fn drop(&mut self) {
        unsafe { bindings::lv_style_reset(&mut self.inner) };
    }
}

// =========================================================================
//  Send + Sync (same contract as C/C++: all access under LVGL lock)
// =========================================================================

unsafe impl Send for Obj {}
unsafe impl Sync for Obj {}
unsafe impl Send for Label {}
unsafe impl Sync for Label {}
unsafe impl Send for Bar {}
unsafe impl Sync for Bar {}
unsafe impl Send for Box {}
unsafe impl Sync for Box {}

// =========================================================================
//  LvglGuard — RAII lock/unlock
// =========================================================================

/// RAII guard for the LVGL mutex. `Drop` calls `ove_lvgl_unlock()`.
pub struct LvglGuard(());

impl Drop for LvglGuard {
    fn drop(&mut self) {
        unsafe { bindings::ove_lvgl_unlock() };
    }
}

// =========================================================================
//  Module-level functions
// =========================================================================

/// Initialize LVGL via `ove_lvgl_init()`.
pub fn init() -> Result<()> {
    let ret = unsafe { bindings::ove_lvgl_init() };
    Error::from_code(ret)
}

/// Feed LVGL tick counter.
pub fn tick(ms: u32) {
    unsafe { bindings::ove_lvgl_tick(ms) };
}

/// Run the LVGL task handler.
pub fn handler() {
    unsafe { bindings::ove_lvgl_handler() };
}

/// Acquire the LVGL mutex and return an RAII guard.
pub fn lock() -> LvglGuard {
    unsafe { bindings::ove_lvgl_lock() };
    LvglGuard(())
}

/// Get the currently active screen.
pub fn screen_active() -> Obj {
    let raw = unsafe { bindings::lv_screen_active() };
    unsafe { Obj::from_raw(raw) }
}
