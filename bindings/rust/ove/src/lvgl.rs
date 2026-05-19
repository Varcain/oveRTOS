// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Safe LVGL v9 wrappers for the oveRTOS Rust SDK.
//!
//! Provides idiomatic Rust bindings matching the C++ `ove::lvgl` wrapper:
//!
//! - **Minimal overhead** — every widget is `Copy` + pointer-sized;
//!   per-op cost is benchmarked at <https://varcain.github.io/oveRTOS/benchmarks/>
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

/// LVGL alignment constant: default (inherit from parent).
pub const ALIGN_DEFAULT: u8 = 0;
/// LVGL alignment constant: align to the top-left of the parent.
pub const ALIGN_TOP_LEFT: u8 = 1;
/// LVGL alignment constant: align to the top-center of the parent.
pub const ALIGN_TOP_MID: u8 = 2;
/// LVGL alignment constant: align to the top-right of the parent.
pub const ALIGN_TOP_RIGHT: u8 = 3;
/// LVGL alignment constant: align to the bottom-left of the parent.
pub const ALIGN_BOTTOM_LEFT: u8 = 4;
/// LVGL alignment constant: align to the bottom-center of the parent.
pub const ALIGN_BOTTOM_MID: u8 = 5;
/// LVGL alignment constant: align to the bottom-right of the parent.
pub const ALIGN_BOTTOM_RIGHT: u8 = 6;
/// LVGL alignment constant: align to the middle of the left edge.
pub const ALIGN_LEFT_MID: u8 = 7;
/// LVGL alignment constant: align to the middle of the right edge.
pub const ALIGN_RIGHT_MID: u8 = 8;
/// LVGL alignment constant: center the widget relative to its parent.
pub const ALIGN_CENTER: u8 = 9;

/// LVGL style selector for the main (background) part of a widget.
pub const PART_MAIN: u32 = 0x00_0000;
/// LVGL style selector for the indicator part (e.g. bar fill, checkbox mark).
pub const PART_INDICATOR: u32 = 0x02_0000;
/// LVGL style selector for the knob of interactive widgets (slider, arc).
pub const PART_KNOB: u32 = 0x03_0000;
/// LVGL style selector for the items part (e.g. table cells, list items).
pub const PART_ITEMS: u32 = 0x05_0000;

/// LVGL v9 `LV_SIZE_CONTENT` — sets widget to size-to-content mode.
/// Computed from: `LV_COORD_SET_SPEC(LV_COORD_MAX)` where
/// `LV_COORD_TYPE_SHIFT = 29`, giving `((1<<29)-1) | (1<<29)`.
pub const SIZE_CONTENT: i32 = 0x3FFF_FFFF;

/// LVGL state bit for checked/toggled widgets (`LV_STATE_CHECKED`).
pub const STATE_CHECKED: u32 = 0x0001;

/// Grid descriptor sentinel that terminates column/row arrays
/// (`LV_GRID_TEMPLATE_LAST`). Equal to `LV_COORD_MAX = (1 << 29) - 1`.
pub const GRID_TEMPLATE_LAST: i32 = 0x1FFF_FFFF;
/// Grid track size: size to content (`LV_GRID_CONTENT`).
/// Equal to `LV_COORD_MAX - 101`.
pub const GRID_CONTENT: i32 = 0x1FFF_FF9A;

/// Grid track size: fractional unit. `grid_fr(1)` means "one FR unit".
/// Equal to `LV_COORD_MAX - 100 + x`.
pub const fn grid_fr(x: i32) -> i32 {
    0x1FFF_FF9B + x
}

/// Grid cell alignment: flush with the cell's start edge (`LV_GRID_ALIGN_START`).
pub const GRID_ALIGN_START: u32 = 0;
/// Grid cell alignment: centred within the cell (`LV_GRID_ALIGN_CENTER`).
pub const GRID_ALIGN_CENTER: u32 = 1;
/// Grid cell alignment: flush with the cell's end edge (`LV_GRID_ALIGN_END`).
pub const GRID_ALIGN_END: u32 = 2;
/// Grid cell alignment: stretch to fill the cell (`LV_GRID_ALIGN_STRETCH`).
pub const GRID_ALIGN_STRETCH: u32 = 3;

/// Flex flow direction.
#[repr(u32)]
#[derive(Clone, Copy)]
pub enum FlexFlow {
    Row = 0x00,
    Column = 0x01,
    RowWrap = 0x04,
    RowReverse = 0x02,
    RowWrapReverse = 0x06,
    ColumnWrap = 0x05,
    ColumnReverse = 0x03,
    ColumnWrapReverse = 0x07,
}

/// Flex track / item alignment (matches `lv_flex_align_t`).
#[repr(u32)]
#[derive(Clone, Copy)]
pub enum FlexAlign {
    Start = 0,
    End = 1,
    Center = 2,
    SpaceEvenly = 3,
    SpaceAround = 4,
    SpaceBetween = 5,
}

/// Container layout kind (matches `lv_obj_set_layout` argument).
#[repr(u32)]
#[derive(Clone, Copy)]
pub enum LayoutKind {
    None = 0,
    Flex = 1,
    Grid = 2,
}

/// LVGL palette family (matches `LV_PALETTE_*`).
#[repr(u32)]
#[derive(Clone, Copy)]
pub enum Palette {
    Red = 0,
    Pink = 1,
    Purple = 2,
    DeepPurple = 3,
    Indigo = 4,
    Blue = 5,
    LightBlue = 6,
    Cyan = 7,
    Teal = 8,
    Green = 9,
    LightGreen = 10,
    Lime = 11,
    Yellow = 12,
    Amber = 13,
    Orange = 14,
    DeepOrange = 15,
    Brown = 16,
    BlueGrey = 17,
    Grey = 18,
    None = 0xFF,
}

/// Text alignment selector (`LV_TEXT_ALIGN_*`).
pub const TEXT_ALIGN_AUTO: u32 = 0;
pub const TEXT_ALIGN_LEFT: u32 = 1;
pub const TEXT_ALIGN_CENTER: u32 = 2;
pub const TEXT_ALIGN_RIGHT: u32 = 3;

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
        // SAFETY: `Color` is `#[repr(C)]` with the same layout as `lv_color_t`.
        unsafe {
            let c = bindings::lv_palette_main(p as _);
            core::mem::transmute(c)
        }
    }

    /// Lighten a palette color by `level` (0..=5; 0 = original main).
    pub fn palette_lighten(p: Palette, level: u8) -> Self {
        // SAFETY: `Color` is `#[repr(C)]` with the same layout as `lv_color_t`.
        unsafe {
            let c = bindings::lv_palette_lighten(p as _, level);
            core::mem::transmute(c)
        }
    }

    /// Darken a palette color by `level` (0..=4; 0 = original main).
    pub fn palette_darken(p: Palette, level: u8) -> Self {
        // SAFETY: `Color` is `#[repr(C)]` with the same layout as `lv_color_t`.
        unsafe {
            let c = bindings::lv_palette_darken(p as _, level);
            core::mem::transmute(c)
        }
    }

    /// Construct a color from a packed 12-bit RGB hex value (e.g. `0xF80` → `0xFF8800`).
    pub fn hex3(hex: u32) -> Self {
        // SAFETY: `Color` is `#[repr(C)]` with the same layout as `lv_color_t`.
        unsafe {
            let c = bindings::lv_color_hex3(hex);
            core::mem::transmute(c)
        }
    }

    pub(crate) fn to_raw(self) -> bindings::lv_color_t {
        // SAFETY: `Color` is `#[repr(C)]` with the same layout as `lv_color_t`.
        unsafe { core::mem::transmute(self) }
    }
}

/// LVGL palette index for the blue palette family (`LV_PALETTE_BLUE`).
pub const PALETTE_BLUE: u32 = 6;

// =========================================================================
//  Font accessors
// =========================================================================

/// Safe pointer to `lv_font_montserrat_14`.
pub fn font_montserrat_14() -> *const bindings::lv_font_t {
    unsafe { &bindings::lv_font_montserrat_14 }
}

/// Safe pointer to `lv_font_montserrat_24`.
pub fn font_montserrat_24() -> *const bindings::lv_font_t {
    unsafe { &bindings::lv_font_montserrat_24 }
}

/// Safe pointer to `lv_font_montserrat_32`.
pub fn font_montserrat_32() -> *const bindings::lv_font_t {
    unsafe { &bindings::lv_font_montserrat_32 }
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

    /// Configure this widget as a grid container.
    ///
    /// Both arrays must terminate with [`GRID_TEMPLATE_LAST`]. Track sizes
    /// can be fixed pixel values, [`GRID_CONTENT`], or [`grid_fr`] for
    /// fractional units. Switches the widget's layout to grid mode.
    ///
    /// # Safety
    /// The caller must ensure `cols` and `rows` outlive the widget, since
    /// LVGL retains the pointer rather than copying. `'static` slices are
    /// the typical choice.
    fn grid_dsc(self, cols: &'static [i32], rows: &'static [i32]) -> Self {
        unsafe { bindings::lv_obj_set_grid_dsc_array(self.raw(), cols.as_ptr(), rows.as_ptr()) };
        self
    }

    /// Place this widget into a cell of its parent grid.
    #[allow(clippy::too_many_arguments)]
    fn grid_cell(
        self,
        col_align: u32,
        col_pos: i32,
        col_span: i32,
        row_align: u32,
        row_pos: i32,
        row_span: i32,
    ) -> Self {
        unsafe {
            bindings::lv_obj_set_grid_cell(
                self.raw(),
                col_align as _,
                col_pos,
                col_span,
                row_align as _,
                row_pos,
                row_span,
            );
        }
        self
    }

    /// Configure this widget as a flex container with the given main-axis flow.
    fn flex_flow(self, flow: FlexFlow) -> Self {
        unsafe { bindings::lv_obj_set_flex_flow(self.raw(), flow as _) };
        self
    }

    /// Set flex container alignment along the main, cross (per item), and
    /// cross (per track) axes. Equivalent to `lv_obj_set_flex_align`.
    fn flex_align(self, main: FlexAlign, cross: FlexAlign, track: FlexAlign) -> Self {
        unsafe { bindings::lv_obj_set_flex_align(self.raw(), main as _, cross as _, track as _) };
        self
    }

    /// Set this child's flex grow factor (0 disables growing).
    fn flex_grow(self, grow: u8) -> Self {
        unsafe { bindings::lv_obj_set_flex_grow(self.raw(), grow) };
        self
    }

    /// Switch the widget's layout engine (None / Flex / Grid).
    fn layout(self, kind: LayoutKind) -> Self {
        unsafe { bindings::lv_obj_set_layout(self.raw(), kind as _) };
        self
    }

    /// Scroll the widget so the given Y coordinate is visible. `anim` enables animation.
    fn scroll_to_y(self, y: i32, anim: bool) -> Self {
        unsafe {
            bindings::lv_obj_scroll_to_y(self.raw(), y, anim);
        }
        self
    }

    /// Force layout recomputation immediately (for queries that need live values).
    fn update_layout(self) -> Self {
        unsafe { bindings::lv_obj_update_layout(self.raw()) };
        self
    }

    /// Width of the inner content area (excludes paddings/scrollbars).
    fn content_width(self) -> i32 {
        unsafe { bindings::lv_obj_get_content_width(self.raw()) }
    }

    /// Distance the widget can still scroll towards its bottom edge.
    fn scroll_bottom(self) -> i32 {
        unsafe { bindings::lv_obj_get_scroll_bottom(self.raw()) }
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

    /// Set top padding.
    fn pad_top(self, p: i32) -> Self {
        unsafe { bindings::lv_obj_set_style_pad_top(self.raw(), p, PART_MAIN) };
        self
    }
    /// Set bottom padding.
    fn pad_bottom(self, p: i32) -> Self {
        unsafe { bindings::lv_obj_set_style_pad_bottom(self.raw(), p, PART_MAIN) };
        self
    }
    /// Set left padding.
    fn pad_left(self, p: i32) -> Self {
        unsafe { bindings::lv_obj_set_style_pad_left(self.raw(), p, PART_MAIN) };
        self
    }
    /// Set right padding.
    fn pad_right(self, p: i32) -> Self {
        unsafe { bindings::lv_obj_set_style_pad_right(self.raw(), p, PART_MAIN) };
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
    ///
    /// Takes `&Style` — LVGL reads the pointer but does not mutate the
    /// style once added. `style` must outlive the widget (typical use:
    /// store in a `InitCell<Style>` or `&'static`).
    fn add_style(self, style: &Style, selector: u32) -> Self {
        unsafe { bindings::lv_obj_add_style(self.raw(), style.ptr(), selector) };
        self
    }

    /// Apply a vertical translation (post-layout offset, in pixels).
    fn translate_y(self, v: i32, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_translate_y(self.raw(), v, selector) };
        self
    }

    /// Top margin in pixels.
    fn margin_top(self, v: i32, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_margin_top(self.raw(), v, selector) };
        self
    }
    /// Bottom margin in pixels.
    fn margin_bottom(self, v: i32, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_margin_bottom(self.raw(), v, selector) };
        self
    }
    /// Left margin in pixels.
    fn margin_left(self, v: i32, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_margin_left(self.raw(), v, selector) };
        self
    }
    /// Right margin in pixels.
    fn margin_right(self, v: i32, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_margin_right(self.raw(), v, selector) };
        self
    }

    /// Maximum height in pixels (caps `set_height`/content sizing).
    fn max_height(self, v: i32, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_max_height(self.raw(), v, selector) };
        self
    }

    /// Arc opacity for arc-drawing widgets (0..=255).
    fn arc_opa(self, opa: u8, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_arc_opa(self.raw(), opa, selector) };
        self
    }

    /// Whether the arc has rounded ends.
    fn arc_rounded(self, rounded: bool, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_arc_rounded(self.raw(), rounded, selector) };
        self
    }

    /// Layered opacity (composites the whole subtree at the given alpha).
    fn opa_layered(self, opa: u8, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_opa_layered(self.raw(), opa, selector) };
        self
    }

    /// Set the text alignment (`TEXT_ALIGN_*`) for the given selector.
    fn text_align(self, align: u32, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_text_align(self.raw(), align as _, selector) };
        self
    }

    // ---- Selector variants of the common style setters ----

    /// Background color with explicit selector (part + state bits).
    fn bg_color_sel(self, c: Color, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_bg_color(self.raw(), c.to_raw(), selector) };
        self
    }
    /// Background opacity with explicit selector.
    fn bg_opa_sel(self, opa: u8, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_bg_opa(self.raw(), opa, selector) };
        self
    }
    /// Border color with explicit selector.
    fn border_color_sel(self, c: Color, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_border_color(self.raw(), c.to_raw(), selector) };
        self
    }
    /// Text color with explicit selector.
    fn text_color_sel(self, c: Color, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_text_color(self.raw(), c.to_raw(), selector) };
        self
    }
    /// Arc track color with explicit selector.
    fn arc_color_sel(self, c: Color, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_arc_color(self.raw(), c.to_raw(), selector) };
        self
    }
    /// Arc track width with explicit selector.
    fn arc_width(self, w: i32, selector: u32) -> Self {
        unsafe { bindings::lv_obj_set_style_arc_width(self.raw(), w, selector) };
        self
    }
    /// Row gap (flex layout).
    fn pad_row(self, p: i32) -> Self {
        unsafe { bindings::lv_obj_set_style_pad_row(self.raw(), p, PART_MAIN) };
        self
    }
    /// Column gap (flex layout).
    fn pad_column(self, p: i32) -> Self {
        unsafe { bindings::lv_obj_set_style_pad_column(self.raw(), p, PART_MAIN) };
        self
    }
    /// Overall object opacity (0..=255).
    fn set_opa(self, opa: u8) -> Self {
        unsafe { bindings::lv_obj_set_style_opa(self.raw(), opa, PART_MAIN) };
        self
    }
}

impl<T: Widget> Styleable for T {}

// =========================================================================
//  EventCtx — borrowed view of a live lv_event_t
// =========================================================================

/// Borrowed view of an active `lv_event_t`. Valid only inside an event callback.
///
/// The lifetime parameter ties the context to the callback invocation —
/// it cannot escape the handler.
#[derive(Clone, Copy)]
pub struct EventCtx<'a> {
    raw: *mut bindings::lv_event_t,
    _ph: core::marker::PhantomData<&'a ()>,
}

impl<'a> EventCtx<'a> {
    /// Widget that originally fired the event (innermost).
    pub fn target(self) -> Obj {
        // lv_event_get_target() returns `*mut c_void` in real LVGL
        // (lv_event_t typedef'd through a macro) but `*mut lv_obj_t` in
        // the test stub; the explicit cast keeps both bindgen variants
        // compiling.
        let raw = unsafe { bindings::lv_event_get_target(self.raw) as *mut bindings::lv_obj_t };
        Obj { raw }
    }

    /// Widget the user is interacting with — may differ from `target` for
    /// bubbled events (this is the widget the callback was registered on).
    pub fn current_target(self) -> Obj {
        let raw =
            unsafe { bindings::lv_event_get_current_target(self.raw) as *mut bindings::lv_obj_t };
        Obj { raw }
    }

    /// Numeric event code (`LV_EVENT_*`).
    pub fn code(self) -> bindings::lv_event_code_t {
        unsafe { bindings::lv_event_get_code(self.raw) }
    }

    /// Event-specific parameter (opaque to safe callers — left raw because
    /// the type depends on the event code; rarely needed in practice).
    pub fn param_raw(self) -> *mut core::ffi::c_void {
        unsafe { bindings::lv_event_get_param(self.raw) }
    }
}

/// Static descriptor for a stateful event handler.
///
/// Bundles a `&'static InitCell<T>` of shared state with a `fn(&T, EventCtx)`
/// user callback. Declare with the [`crate::event_handler!`] macro and pass to
/// [`EventTarget::on_with`] / [`EventTarget::on_clicked_with`].
pub struct EventHandler<T: 'static> {
    cell: &'static crate::InitCell<T>,
    user: fn(&T, EventCtx<'_>),
}

impl<T: 'static> EventHandler<T> {
    /// Construct a handler descriptor — usable in `static` declarations.
    pub const fn new(cell: &'static crate::InitCell<T>, user: fn(&T, EventCtx<'_>)) -> Self {
        Self { cell, user }
    }
}

unsafe impl<T: Send + Sync + 'static> Sync for EventHandler<T> {}

unsafe extern "C" fn event_trampoline_fn(e: *mut bindings::lv_event_t) {
    let ud = unsafe { bindings::lv_event_get_user_data(e) };
    if ud.is_null() {
        return;
    }
    // SAFETY: `ud` was set by `EventTarget::on_fn` from a `fn(EventCtx)` pointer.
    let cb: fn(EventCtx<'_>) = unsafe { core::mem::transmute(ud) };
    cb(EventCtx {
        raw: e,
        _ph: core::marker::PhantomData,
    });
}

unsafe extern "C" fn event_trampoline_with<T: Send + Sync + 'static>(e: *mut bindings::lv_event_t) {
    let ud = unsafe { bindings::lv_event_get_user_data(e) } as *const EventHandler<T>;
    if ud.is_null() {
        return;
    }
    // SAFETY: `ud` points to a `'static EventHandler<T>` set by `EventTarget::on_with`.
    let h = unsafe { &*ud };
    if let Some(state) = h.cell.try_get() {
        (h.user)(
            state,
            EventCtx {
                raw: e,
                _ph: core::marker::PhantomData,
            },
        );
    }
    // If the cell is uninitialized we silently drop the event — defensive
    // posture matching the existing `if let Some(...) = CELL.try_get()` callsites.
}

// =========================================================================
//  EventTarget trait — type-safe event callbacks (blanket impl on Widget)
// =========================================================================

/// Event callback registration. Uses `fn` pointers for `no_std` compatibility.
/// Blanket-implemented for all `Widget` types.
pub trait EventTarget: Widget + Sized {
    /// Register a stateless safe handler for any LVGL event code.
    fn on_fn(self, code: bindings::lv_event_code_t, handler: fn(EventCtx<'_>)) -> Self {
        let ud = handler as *mut core::ffi::c_void;
        unsafe { bindings::lv_obj_add_event_cb(self.raw(), Some(event_trampoline_fn), code, ud) };
        self
    }

    /// Register a stateless safe handler for click events.
    fn on_clicked(self, handler: fn(EventCtx<'_>)) -> Self {
        self.on_fn(bindings::LV_EVENT_CLICKED, handler)
    }

    /// Register a stateless safe handler for value-changed events.
    fn on_value_change(self, handler: fn(EventCtx<'_>)) -> Self {
        self.on_fn(bindings::LV_EVENT_VALUE_CHANGED, handler)
    }

    /// Register a stateful safe handler — the handler receives the shared
    /// state from the bundled `InitCell` and an `EventCtx`.
    fn on_with<T: Send + Sync + 'static>(
        self,
        code: bindings::lv_event_code_t,
        handler: &'static EventHandler<T>,
    ) -> Self {
        let ud = handler as *const EventHandler<T> as *mut core::ffi::c_void;
        unsafe {
            bindings::lv_obj_add_event_cb(self.raw(), Some(event_trampoline_with::<T>), code, ud);
        }
        self
    }

    /// Stateful click handler (see [`EventTarget::on_with`]).
    fn on_clicked_with<T: Send + Sync + 'static>(self, handler: &'static EventHandler<T>) -> Self {
        self.on_with(bindings::LV_EVENT_CLICKED, handler)
    }

    /// Stateful value-change handler (see [`EventTarget::on_with`]).
    fn on_value_change_with<T: Send + Sync + 'static>(
        self,
        handler: &'static EventHandler<T>,
    ) -> Self {
        self.on_with(bindings::LV_EVENT_VALUE_CHANGED, handler)
    }

    /// **Legacy escape hatch** — register a raw `lv_event_cb_t`. Prefer
    /// [`EventTarget::on_fn`] or [`EventTarget::on_with`] for typed safe
    /// callbacks.
    #[doc(hidden)]
    fn on(
        self,
        code: bindings::lv_event_code_t,
        cb: bindings::lv_event_cb_t,
        user_data: *mut core::ffi::c_void,
    ) -> Self {
        unsafe { bindings::lv_obj_add_event_cb(self.raw(), cb, code, user_data) };
        self
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

    /// Remove all inline and reused styles from this object.
    pub fn remove_style_all(self) -> Self {
        unsafe { bindings::lv_obj_remove_style_all(self.raw) };
        self
    }

    /// Get the nth child (0-indexed). Returns an [`Obj`] wrapping the
    /// LVGL pointer — may be null if out of range (check before using).
    pub fn get_child(self, idx: i32) -> Obj {
        let raw = unsafe { bindings::lv_obj_get_child(self.raw, idx) };
        Obj { raw }
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

    /// Wrap a raw `lv_obj_t *` as a `Label`. The caller is responsible
    /// for ensuring the pointer was obtained from a Label widget.
    ///
    /// # Safety
    /// `raw` must point to a valid `lv_label_t` object.
    pub unsafe fn from_raw(raw: *mut bindings::lv_obj_t) -> Self {
        Self { raw }
    }

    /// Set text from a null-terminated byte slice.
    pub fn set_text(self, text: &[u8]) {
        unsafe { bindings::lv_label_set_text(self.raw, text.as_ptr() as *const _) };
    }

    /// Set text by pointer — caller-owned buffer must outlive the label.
    /// LVGL stores the pointer instead of duplicating, so re-rendering
    /// with the same buffer triggers a redraw without reallocation.
    pub fn set_text_static(self, text: &'static [u8]) {
        unsafe { bindings::lv_label_set_text_static(self.raw, text.as_ptr() as *const _) };
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
//  Button
// =========================================================================

/// LVGL button widget — a clickable container.
///
/// Add a [`Label`] child to give the button text. Use [`Button::toggle_mode`]
/// to turn it into a checkable toggle whose state is tracked via
/// [`STATE_CHECKED`].
#[derive(Clone, Copy)]
pub struct Button {
    raw: *mut bindings::lv_obj_t,
}

impl Button {
    /// Create a new button as a child of `parent`.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_button_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: enable or disable toggle (checkable) behaviour.
    pub fn toggle_mode(self, on: bool) -> Self {
        if on {
            unsafe { bindings::lv_obj_add_flag(self.raw, bindings::LV_OBJ_FLAG_CHECKABLE) };
        } else {
            unsafe { bindings::lv_obj_remove_flag(self.raw, bindings::LV_OBJ_FLAG_CHECKABLE) };
        }
        self
    }

    /// Fluent: set the checked state explicitly.
    pub fn checked(self, v: bool) -> Self {
        if v {
            unsafe { bindings::lv_obj_add_state(self.raw, STATE_CHECKED as _) };
        } else {
            unsafe { bindings::lv_obj_remove_state(self.raw, STATE_CHECKED as _) };
        }
        self
    }

    /// Returns `true` if the button is currently in the checked state.
    pub fn is_checked(self) -> bool {
        unsafe { bindings::lv_obj_has_state(self.raw, STATE_CHECKED as _) }
    }
}

impl Widget for Button {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Button {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Button as *const Obj) }
    }
}

// =========================================================================
//  Slider — interactive bar
// =========================================================================

/// LVGL slider widget. Shares the bar value/range shape and adds knob
/// styling and drag interaction. Listen for changes with
/// [`EventTarget::on_value_change`].
#[derive(Clone, Copy)]
pub struct Slider {
    raw: *mut bindings::lv_obj_t,
}

impl Slider {
    /// Create a new slider as a child of `parent`.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_slider_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set the current value (animated).
    pub fn value(self, val: i32) -> Self {
        unsafe {
            #[allow(clippy::unnecessary_cast)]
            bindings::lv_slider_set_value(self.raw, val, true as _);
        }
        self
    }

    /// Fluent: set the current value with explicit animation control.
    pub fn value_anim(self, val: i32, anim: bool) -> Self {
        unsafe {
            #[allow(clippy::unnecessary_cast)]
            bindings::lv_slider_set_value(self.raw, val, anim as _);
        }
        self
    }

    /// Fluent: set the min/max range.
    pub fn range(self, min: i32, max: i32) -> Self {
        unsafe { bindings::lv_slider_set_range(self.raw, min, max) };
        self
    }

    /// Returns the slider's current value.
    pub fn get_value(self) -> i32 {
        unsafe { bindings::lv_slider_get_value(self.raw) }
    }

    /// Fluent: set the filled indicator colour (`LV_PART_INDICATOR`).
    pub fn indicator_color(self, c: Color) -> Self {
        unsafe { bindings::lv_obj_set_style_bg_color(self.raw, c.to_raw(), PART_INDICATOR) };
        self
    }

    /// Fluent: set the knob colour (`LV_PART_KNOB`).
    pub fn knob_color(self, c: Color) -> Self {
        unsafe { bindings::lv_obj_set_style_bg_color(self.raw, c.to_raw(), PART_KNOB) };
        self
    }
}

impl Widget for Slider {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Slider {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Slider as *const Obj) }
    }
}

// =========================================================================
//  Switch — binary toggle
// =========================================================================

/// LVGL switch widget (binary toggle). State is tracked via
/// [`STATE_CHECKED`]; listen for changes with
/// [`EventTarget::on_value_change`].
#[derive(Clone, Copy)]
pub struct Switch {
    raw: *mut bindings::lv_obj_t,
}

impl Switch {
    /// Create a new switch as a child of `parent`.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_switch_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set the on/off state.
    pub fn checked(self, v: bool) -> Self {
        if v {
            unsafe { bindings::lv_obj_add_state(self.raw, STATE_CHECKED as _) };
        } else {
            unsafe { bindings::lv_obj_remove_state(self.raw, STATE_CHECKED as _) };
        }
        self
    }

    /// Returns `true` when the switch is on.
    pub fn is_checked(self) -> bool {
        unsafe { bindings::lv_obj_has_state(self.raw, STATE_CHECKED as _) }
    }
}

impl Widget for Switch {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Switch {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Switch as *const Obj) }
    }
}

// =========================================================================
//  Checkbox — labelled toggle
// =========================================================================

/// LVGL checkbox widget — a labelled binary toggle.
#[derive(Clone, Copy)]
pub struct Checkbox {
    raw: *mut bindings::lv_obj_t,
}

impl Checkbox {
    /// Create a new checkbox as a child of `parent`.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_checkbox_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set the label text (LVGL copies into its own buffer).
    pub fn text(self, txt: &[u8]) -> Self {
        unsafe { bindings::lv_checkbox_set_text(self.raw, txt.as_ptr() as *const _) };
        self
    }

    /// Fluent: set the label text from a persistent static string (no copy).
    pub fn text_static(self, txt: &'static [u8]) -> Self {
        unsafe { bindings::lv_checkbox_set_text_static(self.raw, txt.as_ptr() as *const _) };
        self
    }

    /// Fluent: set the checked state.
    pub fn checked(self, v: bool) -> Self {
        if v {
            unsafe { bindings::lv_obj_add_state(self.raw, STATE_CHECKED as _) };
        } else {
            unsafe { bindings::lv_obj_remove_state(self.raw, STATE_CHECKED as _) };
        }
        self
    }

    /// Returns `true` if the checkbox is ticked.
    pub fn is_checked(self) -> bool {
        unsafe { bindings::lv_obj_has_state(self.raw, STATE_CHECKED as _) }
    }
}

impl Widget for Checkbox {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Checkbox {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Checkbox as *const Obj) }
    }
}

// =========================================================================
//  Arc — circular slider / gauge
// =========================================================================

/// LVGL arc widget — a circular gauge or slider. Track uses `arc_color`
/// on `LV_PART_MAIN`, the filled indicator uses `arc_color` on
/// `LV_PART_INDICATOR`, and the draggable knob uses `bg_color` on
/// `LV_PART_KNOB`.
#[derive(Clone, Copy)]
pub struct Arc {
    raw: *mut bindings::lv_obj_t,
}

impl Arc {
    /// Create a new arc as a child of `parent`.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_arc_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set the current value.
    pub fn value(self, val: i32) -> Self {
        unsafe { bindings::lv_arc_set_value(self.raw, val) };
        self
    }

    /// Fluent: set the min/max range.
    pub fn range(self, min: i32, max: i32) -> Self {
        unsafe { bindings::lv_arc_set_range(self.raw, min, max) };
        self
    }

    /// Fluent: set the background arc angles in degrees.
    pub fn bg_angles(self, start: u32, end: u32) -> Self {
        unsafe { bindings::lv_arc_set_bg_angles(self.raw, start as _, end as _) };
        self
    }

    /// Fluent: set the indicator arc angles in degrees.
    pub fn angles(self, start: u32, end: u32) -> Self {
        unsafe { bindings::lv_arc_set_angles(self.raw, start as _, end as _) };
        self
    }

    /// Fluent: set the rotation offset of the arc in degrees.
    pub fn rotation(self, rot: u32) -> Self {
        unsafe { bindings::lv_arc_set_rotation(self.raw, rot as _) };
        self
    }

    /// Returns the current value.
    pub fn get_value(self) -> i32 {
        unsafe { bindings::lv_arc_get_value(self.raw) }
    }

    /// Fluent: set the track arc colour (`LV_PART_MAIN`).
    pub fn track_color(self, c: Color) -> Self {
        unsafe { bindings::lv_obj_set_style_arc_color(self.raw, c.to_raw(), PART_MAIN) };
        self
    }

    /// Fluent: set the filled indicator arc colour (`LV_PART_INDICATOR`).
    pub fn indicator_color(self, c: Color) -> Self {
        unsafe { bindings::lv_obj_set_style_arc_color(self.raw, c.to_raw(), PART_INDICATOR) };
        self
    }

    /// Fluent: set the indicator arc width in pixels.
    pub fn indicator_width(self, w: i32) -> Self {
        unsafe { bindings::lv_obj_set_style_arc_width(self.raw, w, PART_INDICATOR) };
        self
    }

    /// Fluent: set the knob colour (`LV_PART_KNOB`).
    pub fn knob_color(self, c: Color) -> Self {
        unsafe { bindings::lv_obj_set_style_bg_color(self.raw, c.to_raw(), PART_KNOB) };
        self
    }
}

impl Widget for Arc {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Arc {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Arc as *const Obj) }
    }
}

// =========================================================================
//  Msgbox — modal dialog
// =========================================================================

/// LVGL message box widget — a modal dialog with optional title, body
/// text, close button, and footer buttons.
///
/// Passing a null parent to [`Msgbox::create`] centers the msgbox on the
/// active screen.
#[derive(Clone, Copy)]
pub struct Msgbox {
    raw: *mut bindings::lv_obj_t,
}

impl Msgbox {
    /// Create a new msgbox. If `parent.raw()` is null, the msgbox is
    /// centered on the active screen.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_msgbox_create(parent.raw()) };
        Self { raw }
    }

    /// Create a new top-level msgbox (centered on the active screen).
    pub fn create_modal() -> Self {
        let raw = unsafe { bindings::lv_msgbox_create(core::ptr::null_mut()) };
        Self { raw }
    }

    /// Fluent: add a title in the header.
    pub fn add_title(self, txt: &[u8]) -> Self {
        unsafe { bindings::lv_msgbox_add_title(self.raw, txt.as_ptr() as *const _) };
        self
    }

    /// Fluent: add body text.
    pub fn add_text(self, txt: &[u8]) -> Self {
        unsafe { bindings::lv_msgbox_add_text(self.raw, txt.as_ptr() as *const _) };
        self
    }

    /// Fluent: add a close (X) button in the header.
    pub fn add_close_button(self) -> Self {
        unsafe { bindings::lv_msgbox_add_close_button(self.raw) };
        self
    }

    /// Add a footer button with the given text. Returns the raw button
    /// handle for event handler attachment.
    pub fn add_footer_button(self, txt: &[u8]) -> Obj {
        let btn =
            unsafe { bindings::lv_msgbox_add_footer_button(self.raw, txt.as_ptr() as *const _) };
        unsafe { Obj::from_raw(btn) }
    }

    /// Returns the content container.
    pub fn get_content(self) -> Obj {
        let c = unsafe { bindings::lv_msgbox_get_content(self.raw) };
        unsafe { Obj::from_raw(c) }
    }

    /// Returns the header container.
    pub fn get_header(self) -> Obj {
        let h = unsafe { bindings::lv_msgbox_get_header(self.raw) };
        unsafe { Obj::from_raw(h) }
    }

    /// Returns the footer container.
    pub fn get_footer(self) -> Obj {
        let f = unsafe { bindings::lv_msgbox_get_footer(self.raw) };
        unsafe { Obj::from_raw(f) }
    }

    /// Close and delete the msgbox.
    pub fn close(self) {
        unsafe { bindings::lv_msgbox_close(self.raw) };
    }
}

impl Widget for Msgbox {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Msgbox {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Msgbox as *const Obj) }
    }
}

// =========================================================================
//  Spinner — circular loading indicator
// =========================================================================

/// LVGL spinner widget — a continuously rotating arc used as a loading
/// indicator.
#[derive(Clone, Copy)]
pub struct Spinner {
    raw: *mut bindings::lv_obj_t,
}

impl Spinner {
    /// Create a new spinner as a child of `parent`.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_spinner_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set the animation period (ms) and indicator arc length (deg).
    pub fn anim_params(self, time_ms: u32, angle_deg: u32) -> Self {
        unsafe { bindings::lv_spinner_set_anim_params(self.raw, time_ms, angle_deg) };
        self
    }
}

impl Widget for Spinner {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Spinner {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Spinner as *const Obj) }
    }
}

// =========================================================================
//  Led — colored indicator circle
// =========================================================================

/// LVGL LED widget — a small colored indicator circle with adjustable
/// brightness.
#[derive(Clone, Copy)]
pub struct Led {
    raw: *mut bindings::lv_obj_t,
}

impl Led {
    /// Create a new LED as a child of `parent`.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_led_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set the LED base colour.
    pub fn color(self, c: Color) -> Self {
        unsafe { bindings::lv_led_set_color(self.raw, c.to_raw()) };
        self
    }

    /// Fluent: set the LED brightness (0–255).
    pub fn brightness(self, bright: u8) -> Self {
        unsafe { bindings::lv_led_set_brightness(self.raw, bright) };
        self
    }

    /// Fluent: turn the LED on.
    pub fn on(self) -> Self {
        unsafe { bindings::lv_led_on(self.raw) };
        self
    }

    /// Fluent: turn the LED off.
    pub fn off(self) -> Self {
        unsafe { bindings::lv_led_off(self.raw) };
        self
    }

    /// Fluent: toggle on/off.
    pub fn toggle(self) -> Self {
        unsafe { bindings::lv_led_toggle(self.raw) };
        self
    }

    /// Returns the current brightness.
    pub fn get_brightness(self) -> u8 {
        unsafe { bindings::lv_led_get_brightness(self.raw) }
    }
}

impl Widget for Led {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Led {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Led as *const Obj) }
    }
}

// =========================================================================
//  Chart — line / bar / scatter chart
// =========================================================================

/// LVGL chart type: no chart (empty).
pub const CHART_TYPE_NONE: u32 = 0;
/// LVGL chart type: line chart.
pub const CHART_TYPE_LINE: u32 = 1;
/// LVGL chart type: bar chart.
pub const CHART_TYPE_BAR: u32 = 2;
/// LVGL chart type: scatter chart.
pub const CHART_TYPE_SCATTER: u32 = 3;

/// Chart axis: primary Y.
pub const CHART_AXIS_PRIMARY_Y: u32 = 0x00;
/// Chart axis: secondary Y.
pub const CHART_AXIS_SECONDARY_Y: u32 = 0x01;
/// Chart axis: primary X.
pub const CHART_AXIS_PRIMARY_X: u32 = 0x02;
/// Chart axis: secondary X.
pub const CHART_AXIS_SECONDARY_X: u32 = 0x04;

/// Chart update mode: shift existing points left when adding.
pub const CHART_UPDATE_MODE_SHIFT: u32 = 0;
/// Chart update mode: wrap around (circular buffer).
pub const CHART_UPDATE_MODE_CIRCULAR: u32 = 1;

/// Handle to an `lv_chart_series_t *`. Owned by the Chart; non-owning copy.
#[derive(Clone, Copy)]
pub struct Series {
    chart: *mut bindings::lv_obj_t,
    raw: *mut bindings::lv_chart_series_t,
}

impl Series {
    /// Push a new value using the chart's current update mode.
    pub fn next_value(self, v: i32) -> Self {
        unsafe { bindings::lv_chart_set_next_value(self.chart, self.raw, v) };
        self
    }

    /// Set a specific point index.
    pub fn set_value_by_idx(self, idx: u32, v: i32) -> Self {
        unsafe { bindings::lv_chart_set_series_value_by_id(self.chart, self.raw, idx, v) };
        self
    }

    /// Returns the raw `lv_chart_series_t *` pointer.
    pub fn as_raw(self) -> *mut bindings::lv_chart_series_t {
        self.raw
    }
}

unsafe impl Send for Series {}
unsafe impl Sync for Series {}

/// LVGL chart widget — line / bar / scatter with multiple series and axes.
#[derive(Clone, Copy)]
pub struct Chart {
    raw: *mut bindings::lv_obj_t,
}

impl Chart {
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_chart_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set the chart type (use `CHART_TYPE_*` constants).
    pub fn chart_type(self, t: u32) -> Self {
        unsafe { bindings::lv_chart_set_type(self.raw, t as _) };
        self
    }

    /// Fluent: set the number of data points per series.
    pub fn point_count(self, count: u32) -> Self {
        unsafe { bindings::lv_chart_set_point_count(self.raw, count) };
        self
    }

    /// Fluent: set the min/max range for an axis.
    pub fn range(self, axis: u32, min: i32, max: i32) -> Self {
        unsafe { bindings::lv_chart_set_axis_range(self.raw, axis as _, min, max) };
        self
    }

    /// Fluent: set the update mode (shift or circular).
    pub fn update_mode(self, mode: u32) -> Self {
        unsafe { bindings::lv_chart_set_update_mode(self.raw, mode as _) };
        self
    }

    /// Fluent: set horizontal/vertical division line count.
    pub fn div_line_count(self, hdiv: u8, vdiv: u8) -> Self {
        unsafe { bindings::lv_chart_set_div_line_count(self.raw, hdiv as u32, vdiv as u32) };
        self
    }

    /// Add a new series with the given color and axis binding.
    pub fn add_series(self, color: Color, axis: u32) -> Series {
        let raw = unsafe { bindings::lv_chart_add_series(self.raw, color.to_raw(), axis as _) };
        Series {
            chart: self.raw,
            raw,
        }
    }

    /// Remove a series from the chart.
    pub fn remove_series(self, s: Series) -> Self {
        unsafe { bindings::lv_chart_remove_series(self.raw, s.raw) };
        self
    }
}

impl Widget for Chart {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Chart {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Chart as *const Obj) }
    }
}

// =========================================================================
//  Calendar — month-grid date picker
// =========================================================================

/// LVGL calendar widget — month-grid date picker with optional
/// arrow/dropdown navigation header.
#[derive(Clone, Copy)]
pub struct Calendar {
    raw: *mut bindings::lv_obj_t,
}

impl Calendar {
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_calendar_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set today's date (highlighted in the grid).
    pub fn today(self, year: u32, month: u32, day: u32) -> Self {
        unsafe { bindings::lv_calendar_set_today_date(self.raw, year, month, day) };
        self
    }

    /// Fluent: set the currently visible month.
    pub fn showed(self, year: u32, month: u32) -> Self {
        unsafe { bindings::lv_calendar_set_month_shown(self.raw, year, month) };
        self
    }

    /// Returns the last date the user pressed, as `(y, m, d)`, or `None`.
    pub fn get_pressed_date(self) -> Option<(u32, u32, u32)> {
        let mut date: bindings::lv_calendar_date_t = unsafe { core::mem::zeroed() };
        let ok = unsafe { bindings::lv_calendar_get_pressed_date(self.raw, &mut date) };
        // LVGL's lv_result_t: 1 = OK, 0 = INVALID. Non-zero means we got a date.
        if ok != 0 {
            // lv_calendar_date_t fields are uint16_t/uint8_t in real
            // LVGL but bindgen lifts them to u32 in the test stub; the
            // `as u32` keeps both variants compiling (the no-op cast in
            // the stub is silenced by clippy::unnecessary_cast at the
            // crate root).
            Some((date.year as u32, date.month as u32, date.day as u32))
        } else {
            None
        }
    }

    /// Add an arrow-header navigation bar as a child. Returns the header.
    pub fn add_header_arrow(self) -> Obj {
        let raw = unsafe { bindings::lv_calendar_add_header_arrow(self.raw) };
        unsafe { Obj::from_raw(raw) }
    }

    /// Add a dropdown-header navigation bar as a child. Returns the header.
    pub fn add_header_dropdown(self) -> Obj {
        let raw = unsafe { bindings::lv_calendar_add_header_dropdown(self.raw) };
        unsafe { Obj::from_raw(raw) }
    }
}

impl Widget for Calendar {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Calendar {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Calendar as *const Obj) }
    }
}

// =========================================================================
//  Canvas — user-buffer drawing surface
// =========================================================================

/// LVGL canvas — a widget backed by a user-supplied pixel buffer.
///
/// The caller must keep the buffer alive for the lifetime of the widget.
/// Buffer size is `w * h * bytes_per_pixel(cf)`.
///
/// Initial cut exposes buffer setup, background fill, per-pixel writes,
/// and the layer init/finish dance that LVGL 9 requires for draw
/// operations. `draw_rect/arc/label` wrappers can be added later.
#[derive(Clone, Copy)]
pub struct Canvas {
    raw: *mut bindings::lv_obj_t,
}

impl Canvas {
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_canvas_create(parent.raw()) };
        Self { raw }
    }

    /// **Legacy escape hatch** — prefer [`Canvas::set_buffer`] which takes
    /// a typed, lifetime-tracked [`CanvasBuffer`]. Retained for code
    /// that must supply a raw pointer (e.g. hardware-allocated framebuffers).
    ///
    /// # Safety
    /// `buf` must remain valid for the lifetime of the canvas widget.
    /// The buffer size must be exactly `w * h * bytes_per_pixel(cf)`.
    #[doc(hidden)]
    pub unsafe fn buffer(
        self,
        buf: *mut core::ffi::c_void,
        w: i32,
        h: i32,
        cf: bindings::lv_color_format_t,
    ) -> Self {
        unsafe { bindings::lv_canvas_set_buffer(self.raw, buf, w, h, cf) };
        self
    }

    /// Fill the entire canvas with the given colour and opacity.
    pub fn fill_bg(self, color: Color, opa: u8) -> Self {
        unsafe { bindings::lv_canvas_fill_bg(self.raw, color.to_raw(), opa) };
        self
    }

    /// Set a single pixel to the given colour.
    pub fn set_pixel(self, x: i32, y: i32, color: Color) -> Self {
        unsafe { bindings::lv_canvas_set_px(self.raw, x, y, color.to_raw(), 255) };
        self
    }

    /// Returns the raw `lv_obj_t *` for low-level canvas operations
    /// (e.g. passing to `lv_canvas_init_layer` / draw descriptor calls
    /// from other crates).
    pub fn raw_obj(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl Widget for Canvas {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Canvas {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Canvas as *const Obj) }
    }
}

// =========================================================================
//  Table — 2D data grid
// =========================================================================

/// LVGL table — 2D grid of cells with per-cell text.
#[derive(Clone, Copy)]
pub struct Table {
    raw: *mut bindings::lv_obj_t,
}

impl Table {
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_table_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set a cell's text (null-terminated byte slice).
    pub fn cell_value(self, row: u32, col: u32, txt: &[u8]) -> Self {
        unsafe { bindings::lv_table_set_cell_value(self.raw, row, col, txt.as_ptr() as *const _) };
        self
    }

    /// Fluent: set the row count.
    pub fn row_count(self, n: u32) -> Self {
        unsafe { bindings::lv_table_set_row_count(self.raw, n) };
        self
    }

    /// Fluent: set the column count.
    pub fn column_count(self, n: u32) -> Self {
        unsafe { bindings::lv_table_set_column_count(self.raw, n) };
        self
    }

    /// Fluent: set the pixel width of a column.
    pub fn column_width(self, col: u32, w: i32) -> Self {
        unsafe { bindings::lv_table_set_column_width(self.raw, col, w) };
        self
    }

    /// Returns the total row count.
    pub fn get_row_count(self) -> u32 {
        unsafe { bindings::lv_table_get_row_count(self.raw) }
    }

    /// Returns the total column count.
    pub fn get_column_count(self) -> u32 {
        unsafe { bindings::lv_table_get_column_count(self.raw) }
    }

    /// Returns the pixel width of a specific column.
    pub fn get_column_width(self, col: u32) -> i32 {
        unsafe { bindings::lv_table_get_column_width(self.raw, col) }
    }
}

impl Widget for Table {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Table {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Table as *const Obj) }
    }
}

// =========================================================================
//  Tabview — tabbed container with swipeable pages
// =========================================================================

/// LVGL tabview — tabbed container with swipeable pages.
#[derive(Clone, Copy)]
pub struct Tabview {
    raw: *mut bindings::lv_obj_t,
}

impl Tabview {
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_tabview_create(parent.raw()) };
        Self { raw }
    }

    /// Add a new tab and return its content container as an [`Obj`].
    pub fn add_tab(self, name: &[u8]) -> Obj {
        let raw = unsafe { bindings::lv_tabview_add_tab(self.raw, name.as_ptr() as *const _) };
        unsafe { Obj::from_raw(raw) }
    }

    /// Fluent: rename an existing tab by index.
    pub fn rename_tab(self, idx: u32, name: &[u8]) -> Self {
        unsafe { bindings::lv_tabview_rename_tab(self.raw, idx, name.as_ptr() as *const _) };
        self
    }

    /// Fluent: set the active tab index. `anim` = true to animate the switch.
    pub fn set_active(self, idx: u32, anim: bool) -> Self {
        unsafe {
            #[allow(clippy::unnecessary_cast)]
            bindings::lv_tabview_set_active(self.raw, idx, anim as _);
        }
        self
    }

    /// Fluent: set the tab bar position. Use an `LV_DIR_*` value.
    pub fn tab_bar_position(self, dir: bindings::lv_dir_t) -> Self {
        unsafe { bindings::lv_tabview_set_tab_bar_position(self.raw, dir) };
        self
    }

    /// Fluent: set the tab bar size in pixels.
    pub fn tab_bar_size(self, size: i32) -> Self {
        unsafe { bindings::lv_tabview_set_tab_bar_size(self.raw, size) };
        self
    }

    /// Returns the total number of tabs.
    pub fn get_tab_count(self) -> u32 {
        unsafe { bindings::lv_tabview_get_tab_count(self.raw) }
    }

    /// Returns the currently active tab index.
    pub fn get_tab_active(self) -> u32 {
        unsafe { bindings::lv_tabview_get_tab_active(self.raw) }
    }

    /// Returns the content container that houses all tabs' content areas.
    pub fn get_content(self) -> Obj {
        let raw = unsafe { bindings::lv_tabview_get_content(self.raw) };
        unsafe { Obj::from_raw(raw) }
    }
}

impl Widget for Tabview {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Tabview {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Tabview as *const Obj) }
    }
}

// =========================================================================
//  List — scrollable list of buttons and text headings
// =========================================================================

/// LVGL list — scrollable list of text headings and icon+text buttons.
#[derive(Clone, Copy)]
pub struct List {
    raw: *mut bindings::lv_obj_t,
}

impl List {
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_list_create(parent.raw()) };
        Self { raw }
    }

    /// Add a text heading row. Returns the label as an [`Obj`].
    pub fn add_text(self, text: &[u8]) -> Obj {
        let raw = unsafe { bindings::lv_list_add_text(self.raw, text.as_ptr() as *const _) };
        unsafe { Obj::from_raw(raw) }
    }

    /// Add a button row with an optional icon. Returns the button as an [`Obj`].
    pub fn add_button(self, icon: Option<ImageSrc>, text: &[u8]) -> Obj {
        let icon_ptr = icon.map_or(core::ptr::null(), ImageSrc::raw_ptr);
        let raw =
            unsafe { bindings::lv_list_add_button(self.raw, icon_ptr, text.as_ptr() as *const _) };
        unsafe { Obj::from_raw(raw) }
    }
}

impl Widget for List {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for List {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const List as *const Obj) }
    }
}

// =========================================================================
//  Textarea — text input
// =========================================================================

/// LVGL textarea — single- or multi-line text input with placeholder,
/// password mode, max length, and accepted-character filter.
#[derive(Clone, Copy)]
pub struct Textarea {
    raw: *mut bindings::lv_obj_t,
}

impl Textarea {
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_textarea_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: replace the text (null-terminated byte slice).
    pub fn text(self, txt: &[u8]) -> Self {
        unsafe { bindings::lv_textarea_set_text(self.raw, txt.as_ptr() as *const _) };
        self
    }

    /// Fluent: append text.
    pub fn add_text(self, txt: &[u8]) -> Self {
        unsafe { bindings::lv_textarea_add_text(self.raw, txt.as_ptr() as *const _) };
        self
    }

    /// Fluent: set placeholder shown while empty.
    pub fn placeholder(self, txt: &[u8]) -> Self {
        unsafe { bindings::lv_textarea_set_placeholder_text(self.raw, txt.as_ptr() as *const _) };
        self
    }

    /// Fluent: toggle single-line mode.
    pub fn one_line(self, en: bool) -> Self {
        unsafe { bindings::lv_textarea_set_one_line(self.raw, en) };
        self
    }

    /// Fluent: enable password masking.
    pub fn password_mode(self, en: bool) -> Self {
        unsafe { bindings::lv_textarea_set_password_mode(self.raw, en) };
        self
    }

    /// Fluent: set max character count (0 = unlimited).
    pub fn max_length(self, n: u32) -> Self {
        unsafe { bindings::lv_textarea_set_max_length(self.raw, n) };
        self
    }

    /// Fluent: restrict input to characters in `list` (null pointer = any).
    pub fn accepted_chars(self, list: &[u8]) -> Self {
        unsafe { bindings::lv_textarea_set_accepted_chars(self.raw, list.as_ptr() as *const _) };
        self
    }

    /// Fluent: move cursor to character position.
    pub fn cursor_pos(self, pos: i32) -> Self {
        unsafe { bindings::lv_textarea_set_cursor_pos(self.raw, pos) };
        self
    }

    /// Fluent: enable moving cursor by clicking.
    pub fn cursor_click_pos(self, en: bool) -> Self {
        unsafe { bindings::lv_textarea_set_cursor_click_pos(self.raw, en) };
        self
    }

    /// Returns the current text as a raw C pointer owned by LVGL. The
    /// pointer is invalidated by any subsequent mutation of the textarea.
    pub fn get_text_ptr(&self) -> *const core::ffi::c_char {
        unsafe { bindings::lv_textarea_get_text(self.raw) }
    }

    /// Returns the current cursor position.
    pub fn get_cursor_pos(self) -> u32 {
        unsafe { bindings::lv_textarea_get_cursor_pos(self.raw) }
    }

    /// Returns `true` if password masking is enabled.
    pub fn is_password_mode(self) -> bool {
        unsafe { bindings::lv_textarea_get_password_mode(self.raw) }
    }

    /// Returns `true` if the textarea is in single-line mode.
    pub fn is_one_line(self) -> bool {
        unsafe { bindings::lv_textarea_get_one_line(self.raw) }
    }

    /// Fluent: append a single Unicode codepoint.
    pub fn add_char(self, c: u32) -> Self {
        unsafe { bindings::lv_textarea_add_char(self.raw, c) };
        self
    }

    /// Fluent: delete the character before the cursor.
    pub fn delete_char(self) -> Self {
        unsafe { bindings::lv_textarea_delete_char(self.raw) };
        self
    }
}

impl Widget for Textarea {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Textarea {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Textarea as *const Obj) }
    }
}

// =========================================================================
//  Dropdown — click-to-open selection list
// =========================================================================

/// LVGL dropdown — click opens a popup list of options (newline-separated).
#[derive(Clone, Copy)]
pub struct Dropdown {
    raw: *mut bindings::lv_obj_t,
}

impl Dropdown {
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_dropdown_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set options from a newline-separated string (LVGL copies it).
    pub fn options(self, opts: &[u8]) -> Self {
        unsafe { bindings::lv_dropdown_set_options(self.raw, opts.as_ptr() as *const _) };
        self
    }

    /// Fluent: set options from a persistent newline-separated string (no copy).
    pub fn options_static(self, opts: &'static [u8]) -> Self {
        unsafe { bindings::lv_dropdown_set_options_static(self.raw, opts.as_ptr() as *const _) };
        self
    }

    /// Fluent: insert a single option at `pos`.
    pub fn add_option(self, opt: &[u8], pos: u32) -> Self {
        unsafe { bindings::lv_dropdown_add_option(self.raw, opt.as_ptr() as *const _, pos) };
        self
    }

    /// Fluent: clear the entire option list.
    pub fn clear_options(self) -> Self {
        unsafe { bindings::lv_dropdown_clear_options(self.raw) };
        self
    }

    /// Fluent: set the selected index.
    pub fn selected(self, sel: u32) -> Self {
        unsafe { bindings::lv_dropdown_set_selected(self.raw, sel) };
        self
    }

    /// Fluent: set popup direction.
    pub fn dir(self, d: bindings::lv_dir_t) -> Self {
        unsafe { bindings::lv_dropdown_set_dir(self.raw, d) };
        self
    }

    /// Returns the currently selected index.
    pub fn get_selected(self) -> u32 {
        unsafe { bindings::lv_dropdown_get_selected(self.raw) }
    }

    /// Returns the total option count.
    pub fn get_option_count(self) -> u32 {
        unsafe { bindings::lv_dropdown_get_option_count(self.raw) }
    }

    /// Fills `buf` with the currently selected option text.
    pub fn get_selected_str(self, buf: &mut [u8]) {
        unsafe {
            bindings::lv_dropdown_get_selected_str(
                self.raw,
                buf.as_mut_ptr() as *mut _,
                buf.len() as u32,
            );
        }
    }

    /// Returns `true` if the popup list is currently expanded.
    pub fn is_open(self) -> bool {
        unsafe { bindings::lv_dropdown_is_open(self.raw) }
    }

    /// Expands the popup list.
    pub fn open(self) {
        unsafe { bindings::lv_dropdown_open(self.raw) };
    }

    /// Collapses the popup list.
    pub fn close(self) {
        unsafe { bindings::lv_dropdown_close(self.raw) };
    }
}

impl Widget for Dropdown {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Dropdown {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Dropdown as *const Obj) }
    }
}

/// Roller mode: normal — selection clamps at ends of the list.
pub const ROLLER_MODE_NORMAL: u32 = 0;
/// Roller mode: infinite — selection can wrap through the list repeatedly.
pub const ROLLER_MODE_INFINITE: u32 = 1;

// =========================================================================
//  Roller — iOS-picker-style scrollable wheel
// =========================================================================

/// LVGL roller — iOS-picker-style scrollable option wheel.
#[derive(Clone, Copy)]
pub struct Roller {
    raw: *mut bindings::lv_obj_t,
}

impl Roller {
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_roller_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set options and scroll mode. Use [`ROLLER_MODE_NORMAL`] or
    /// [`ROLLER_MODE_INFINITE`].
    pub fn options(self, opts: &[u8], mode: u32) -> Self {
        unsafe { bindings::lv_roller_set_options(self.raw, opts.as_ptr() as *const _, mode as _) };
        self
    }

    /// Fluent: set selected index. `anim` = true to animate the scroll.
    pub fn selected(self, sel: u32, anim: bool) -> Self {
        unsafe {
            #[allow(clippy::unnecessary_cast)]
            bindings::lv_roller_set_selected(self.raw, sel, anim as _);
        }
        self
    }

    /// Fluent: set how many rows are visible in the wheel.
    pub fn visible_row_count(self, rows: u32) -> Self {
        unsafe { bindings::lv_roller_set_visible_row_count(self.raw, rows) };
        self
    }

    /// Returns the currently selected index.
    pub fn get_selected(self) -> u32 {
        unsafe { bindings::lv_roller_get_selected(self.raw) }
    }

    /// Returns the total option count.
    pub fn get_option_count(self) -> u32 {
        unsafe { bindings::lv_roller_get_option_count(self.raw) }
    }

    /// Fills `buf` with the currently selected option text.
    pub fn get_selected_str(self, buf: &mut [u8]) {
        unsafe {
            bindings::lv_roller_get_selected_str(
                self.raw,
                buf.as_mut_ptr() as *mut _,
                buf.len() as u32,
            );
        }
    }
}

impl Widget for Roller {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Roller {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Roller as *const Obj) }
    }
}

// =========================================================================
//  Spinbox — numeric stepper
// =========================================================================

/// LVGL spinbox — numeric stepper with `+` / `−` buttons.
#[derive(Clone, Copy)]
pub struct Spinbox {
    raw: *mut bindings::lv_obj_t,
}

impl Spinbox {
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_spinbox_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set the current integer value.
    pub fn value(self, v: i32) -> Self {
        unsafe { bindings::lv_spinbox_set_value(self.raw, v) };
        self
    }

    /// Fluent: set the min/max range.
    pub fn range(self, min: i32, max: i32) -> Self {
        unsafe { bindings::lv_spinbox_set_range(self.raw, min, max) };
        self
    }

    /// Fluent: set the increment/decrement step size.
    pub fn step(self, s: u32) -> Self {
        unsafe { bindings::lv_spinbox_set_step(self.raw, s) };
        self
    }

    /// Fluent: configure display format.
    ///
    /// `digit_count` is the total number of digits, `sep_pos` is the
    /// (1-based) decimal-point position from the right. `digit_format(5, 2)`
    /// renders `###.##`.
    pub fn digit_format(self, digit_count: u32, sep_pos: u32) -> Self {
        unsafe { bindings::lv_spinbox_set_digit_format(self.raw, digit_count, sep_pos) };
        self
    }

    /// Fluent: enable wrap-around at range bounds.
    pub fn rollover(self, on: bool) -> Self {
        unsafe { bindings::lv_spinbox_set_rollover(self.raw, on) };
        self
    }

    /// Fluent: set the edit cursor to a specific digit.
    pub fn cursor_pos(self, pos: u32) -> Self {
        unsafe { bindings::lv_spinbox_set_cursor_pos(self.raw, pos) };
        self
    }

    /// Returns the current integer value.
    pub fn get_value(self) -> i32 {
        unsafe { bindings::lv_spinbox_get_value(self.raw) }
    }

    /// Returns the current step size.
    pub fn get_step(self) -> i32 {
        unsafe { bindings::lv_spinbox_get_step(self.raw) }
    }

    /// Fluent: increment by one step.
    pub fn increment(self) -> Self {
        unsafe { bindings::lv_spinbox_increment(self.raw) };
        self
    }

    /// Fluent: decrement by one step.
    pub fn decrement(self) -> Self {
        unsafe { bindings::lv_spinbox_decrement(self.raw) };
        self
    }
}

impl Widget for Spinbox {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Spinbox {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Spinbox as *const Obj) }
    }
}

/// Keyboard layout: lowercase QWERTY.
pub const KEYBOARD_MODE_TEXT_LOWER: u32 = 0;
/// Keyboard layout: uppercase QWERTY.
pub const KEYBOARD_MODE_TEXT_UPPER: u32 = 1;
/// Keyboard layout: special characters.
pub const KEYBOARD_MODE_SPECIAL: u32 = 2;
/// Keyboard layout: numeric keypad.
pub const KEYBOARD_MODE_NUMBER: u32 = 3;

// =========================================================================
//  Keyboard — on-screen keyboard attached to a Textarea
// =========================================================================

/// LVGL on-screen keyboard. Attach to a [`Textarea`] so key presses
/// flow into it.
#[derive(Clone, Copy)]
pub struct Keyboard {
    raw: *mut bindings::lv_obj_t,
}

impl Keyboard {
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_keyboard_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: attach to a Textarea so typing inserts into it.
    pub fn attach(self, ta: Textarea) -> Self {
        unsafe { bindings::lv_keyboard_set_textarea(self.raw, ta.raw) };
        self
    }

    /// Fluent: switch keyboard mode (use the `KEYBOARD_MODE_*` constants).
    pub fn mode(self, m: u32) -> Self {
        unsafe { bindings::lv_keyboard_set_mode(self.raw, m as _) };
        self
    }

    /// Fluent: enable/disable on-press letter popovers.
    pub fn popovers(self, en: bool) -> Self {
        unsafe { bindings::lv_keyboard_set_popovers(self.raw, en) };
        self
    }
}

impl Widget for Keyboard {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Keyboard {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Keyboard as *const Obj) }
    }
}

// =========================================================================
//  Screen — top-level display root
// =========================================================================

/// Top-level LVGL screen (display root without a parent).
///
/// Create via [`Screen::create`], swap it in with [`Screen::load`] or
/// [`Screen::load_anim`] for animated transitions.
#[derive(Clone, Copy)]
pub struct Screen {
    raw: *mut bindings::lv_obj_t,
}

impl Screen {
    /// Create a new top-level screen (parent = null).
    pub fn create() -> Self {
        let raw = unsafe { bindings::lv_obj_create(core::ptr::null_mut()) };
        Self { raw }
    }

    /// Returns the currently active screen.
    pub fn active() -> Self {
        let raw = unsafe { bindings::lv_screen_active() };
        Self { raw }
    }

    /// Instantly load this screen as the active one.
    pub fn load(self) {
        unsafe { bindings::lv_screen_load(self.raw) };
    }

    /// Load this screen with an animated transition.
    ///
    /// # Safety
    /// When `auto_del` is `true`, LVGL deletes the previously active
    /// screen after the transition — any surviving Rust handle to the
    /// old screen becomes a dangling pointer. Callers must ensure no
    /// other references to the old screen are used afterwards.
    pub unsafe fn load_anim(
        self,
        anim: bindings::lv_screen_load_anim_t,
        time_ms: u32,
        delay_ms: u32,
        auto_del: bool,
    ) {
        unsafe { bindings::lv_screen_load_anim(self.raw, anim, time_ms, delay_ms, auto_del) };
    }
}

impl Widget for Screen {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Screen {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Screen as *const Obj) }
    }
}

// =========================================================================
//  Image
// =========================================================================

/// LVGL image widget.
///
/// Sources can be compiled-in image descriptors (`*const lv_image_dsc_t`),
/// symbol strings (`LV_SYMBOL_*`), or filesystem paths if a file system
/// driver is registered.
#[derive(Clone, Copy)]
pub struct Image {
    raw: *mut bindings::lv_obj_t,
}

impl Image {
    /// Create a new image widget as a child of `parent`.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_image_create(parent.raw()) };
        Self { raw }
    }

    /// **Legacy escape hatch** — prefer [`Image::source`] which takes a typed
    /// [`ImageSrc`]. This raw variant is kept for narrow cases where a
    /// non-`'static` pointer must be supplied.
    ///
    /// # Caller obligations
    /// `src` must point to an LVGL-compatible image source that outlives
    /// the widget: a static `lv_image_dsc_t`, a symbol string literal, or
    /// a null-terminated path string. The pointer is not validated; pass
    /// the wrong shape and LVGL will misinterpret it.
    #[doc(hidden)]
    pub fn src(self, src: *const core::ffi::c_void) -> Self {
        unsafe { bindings::lv_image_set_src(self.raw, src) };
        self
    }

    /// Fluent: set the rotation angle in 0.1-degree units (0–3600).
    pub fn rotation(self, angle: i32) -> Self {
        unsafe { bindings::lv_image_set_rotation(self.raw, angle) };
        self
    }

    /// Fluent: set the zoom factor (256 = 1.0x, 512 = 2.0x).
    pub fn scale(self, zoom: u32) -> Self {
        unsafe { bindings::lv_image_set_scale(self.raw, zoom) };
        self
    }

    /// Fluent: set the rotation/scale pivot point.
    pub fn pivot(self, x: i32, y: i32) -> Self {
        unsafe { bindings::lv_image_set_pivot(self.raw, x, y) };
        self
    }
}

impl Widget for Image {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Image {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Image as *const Obj) }
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
//  Group — keyboard / encoder focus group
// =========================================================================

/// RAII wrapper for an LVGL input focus group (`lv_group_t`).
///
/// Groups route keyboard/encoder/button events to a focused member
/// widget. Essential for non-touch targets. Create a group, add widgets
/// via [`Group::add`], then wire it to an input device via the
/// backend-specific indev registration (outside this crate).
///
/// Dropping the `Group` deletes the underlying `lv_group_t`.
pub struct Group {
    raw: *mut bindings::lv_group_t,
}

impl Group {
    /// Create a fresh focus group.
    pub fn new() -> Self {
        let raw = unsafe { bindings::lv_group_create() };
        Self { raw }
    }

    /// Raw `lv_group_t *` for FFI interop.
    pub fn as_raw(&self) -> *mut bindings::lv_group_t {
        self.raw
    }

    /// Fluent: mark this group as the default.
    pub fn set_as_default(&self) -> &Self {
        unsafe { bindings::lv_group_set_default(self.raw) };
        self
    }

    /// Fluent: add a widget to the group's focusable list.
    pub fn add(&self, obj: impl Widget) -> &Self {
        unsafe { bindings::lv_group_add_obj(self.raw, obj.raw()) };
        self
    }

    /// Fluent: remove all widgets from this group.
    pub fn remove_all(&self) -> &Self {
        unsafe { bindings::lv_group_remove_all_objs(self.raw) };
        self
    }

    /// Fluent: move focus to the next member.
    pub fn focus_next(&self) -> &Self {
        unsafe { bindings::lv_group_focus_next(self.raw) };
        self
    }

    /// Fluent: move focus to the previous member.
    pub fn focus_prev(&self) -> &Self {
        unsafe { bindings::lv_group_focus_prev(self.raw) };
        self
    }

    /// Fluent: freeze/unfreeze focus movement.
    pub fn focus_freeze(&self, freeze: bool) -> &Self {
        unsafe { bindings::lv_group_focus_freeze(self.raw, freeze) };
        self
    }

    /// Returns the currently focused widget as an [`Obj`] handle, or
    /// `None` if the group is empty.
    pub fn focused(&self) -> Option<Obj> {
        let raw = unsafe { bindings::lv_group_get_focused(self.raw) };
        if raw.is_null() {
            None
        } else {
            Some(unsafe { Obj::from_raw(raw) })
        }
    }

    /// Fluent: enable/disable encoder edit mode.
    pub fn edit_mode(&self, enable: bool) -> &Self {
        unsafe { bindings::lv_group_set_editing(self.raw, enable) };
        self
    }

    /// Returns the current edit-mode flag.
    pub fn is_editing(&self) -> bool {
        unsafe { bindings::lv_group_get_editing(self.raw) }
    }

    /// Returns the number of widgets in the group.
    pub fn obj_count(&self) -> u32 {
        unsafe { bindings::lv_group_get_obj_count(self.raw) }
    }

    /// Static helper: focus a widget regardless of its group.
    pub fn focus_obj(obj: impl Widget) {
        unsafe { bindings::lv_group_focus_obj(obj.raw()) };
    }

    /// Static helper: remove a widget from whatever group it's in.
    pub fn remove_obj(obj: impl Widget) {
        unsafe { bindings::lv_group_remove_obj(obj.raw()) };
    }
}

impl Drop for Group {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe { bindings::lv_group_delete(self.raw) };
        }
    }
}

impl Default for Group {
    fn default() -> Self {
        Self::new()
    }
}

unsafe impl Send for Group {}
unsafe impl Sync for Group {}

// =========================================================================
//  Timer — RAII wrapper around lv_timer_t
// =========================================================================

/// RAII wrapper for LVGL's built-in timer.
///
/// LVGL timers fire from inside `lvgl::handler()` while the LVGL lock is
/// already held, so callbacks must NOT call [`lock`] again — the mutex is
/// not reentrant and you will deadlock. Prefer `Timer` over `ove::Timer`
/// for UI-update ticks that only touch LVGL state.
///
/// Dropping the `Timer` deletes the underlying `lv_timer_t`.
pub struct Timer {
    raw: *mut bindings::lv_timer_t,
}

impl Timer {
    /// Create and start an LVGL timer.
    ///
    /// # Safety
    /// `cb` must be an `extern "C" fn(*mut lv_timer_t)` and `user_data`
    /// must remain valid until the timer is dropped or its repeat count
    /// reaches zero.
    pub fn new(
        cb: bindings::lv_timer_cb_t,
        period_ms: u32,
        user_data: *mut core::ffi::c_void,
    ) -> Self {
        let raw = unsafe { bindings::lv_timer_create(cb, period_ms, user_data) };
        Self { raw }
    }

    /// Returns the raw `lv_timer_t *` for FFI interop.
    pub fn as_raw(&self) -> *mut bindings::lv_timer_t {
        self.raw
    }

    /// Fluent: update the period in milliseconds.
    pub fn period(&self, ms: u32) -> &Self {
        unsafe { bindings::lv_timer_set_period(self.raw, ms) };
        self
    }

    /// Fluent: pause the timer (can be resumed).
    pub fn pause(&self) -> &Self {
        unsafe { bindings::lv_timer_pause(self.raw) };
        self
    }

    /// Fluent: resume a paused timer.
    pub fn resume(&self) -> &Self {
        unsafe { bindings::lv_timer_resume(self.raw) };
        self
    }

    /// Fluent: set the number of times the timer fires; `-1` for infinite.
    pub fn repeat_count(&self, count: i32) -> &Self {
        unsafe { bindings::lv_timer_set_repeat_count(self.raw, count) };
        self
    }

    /// Fluent: reset the internal elapsed-time counter.
    pub fn reset(&self) -> &Self {
        unsafe { bindings::lv_timer_reset(self.raw) };
        self
    }

    /// Fluent: make the timer ready to fire on the next handler pass.
    pub fn ready(&self) -> &Self {
        unsafe { bindings::lv_timer_ready(self.raw) };
        self
    }
}

impl Drop for Timer {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe { bindings::lv_timer_delete(self.raw) };
        }
    }
}

unsafe impl Send for Timer {}
unsafe impl Sync for Timer {}

// =========================================================================
//  State<T> — reactive integer state (lv_subject_t wrapper)
// =========================================================================

/// Reactive integer state backed by LVGL's observer subsystem
/// (`lv_subject_t`). Bind widget properties to a `State` and they
/// update automatically when you call [`State::set`].
///
/// # Stability requirement
///
/// `lv_subject_t` keeps a linked list of observer references, so the
/// `State` address must be stable from the moment any observer attaches.
/// The intended usage is inside a static slot — either a raw `static`
/// declared by the user, or via the `ove::shared!` macro. Dropping a
/// `State` while observers are still attached will corrupt the observer
/// list. `PhantomPinned` prevents move after construction.
///
/// ```ignore
/// use ove::lvgl::{State, Label};
/// ove::shared!(COUNTER: State<i32>);
/// // init once:
/// COUNTER.init(State::new(0));
/// // bind:
/// Label::create(screen).bind_text(COUNTER.get_ref(), b"Count: %d\0");
/// // update from anywhere:
/// COUNTER.get_ref().set(42);  // label updates automatically
/// ```
pub struct State<T: Copy> {
    subject: core::cell::UnsafeCell<bindings::lv_subject_t>,
    _pin: core::marker::PhantomPinned,
    _phantom: core::marker::PhantomData<T>,
}

impl<T: Copy + Into<i32> + TryFrom<i32>> State<T> {
    /// Create a new reactive state with an initial value.
    pub fn new(initial: T) -> Self {
        let mut subject: bindings::lv_subject_t = unsafe { core::mem::zeroed() };
        unsafe { bindings::lv_subject_init_int(&mut subject, initial.into()) };
        Self {
            subject: core::cell::UnsafeCell::new(subject),
            _pin: core::marker::PhantomPinned,
            _phantom: core::marker::PhantomData,
        }
    }

    /// Set a new value. All bound widgets update immediately.
    ///
    /// Takes `&self` rather than `&mut self` because LVGL serializes
    /// observer notification under the global LVGL lock — the lock is
    /// the real mutex, not Rust's borrow checker.
    pub fn set(&self, value: T) {
        unsafe {
            bindings::lv_subject_set_int(self.subject.get(), value.into());
        }
    }

    /// Read the current value. Falls back to a zeroed `T` if the stored
    /// `i32` cannot be converted back to `T` (unreachable for well-typed
    /// integer states).
    pub fn get(&self) -> T {
        let raw = unsafe { bindings::lv_subject_get_int(self.subject.get()) };
        T::try_from(raw).unwrap_or_else(|_| {
            // Unreachable for well-typed integers; zero is the safest fallback.
            unsafe { core::mem::zeroed() }
        })
    }

    /// Returns the raw `*mut lv_subject_t` for widget binding.
    pub fn subject_ptr(&self) -> *mut bindings::lv_subject_t {
        self.subject.get()
    }
}

impl<T: Copy> Drop for State<T> {
    fn drop(&mut self) {
        unsafe { bindings::lv_subject_deinit(self.subject.get()) };
    }
}

unsafe impl<T: Copy + Send> Send for State<T> {}
unsafe impl<T: Copy + Send> Sync for State<T> {}

// Widget bind methods — only the widgets with native LVGL bind_* calls
// get these. Bar/Checkbox/Switch/Spinbox users must wire observers
// manually via `lv_subject_add_observer_obj` (not yet wrapped).

impl Label {
    /// Bind this label's text to a reactive state with a printf-style format.
    ///
    /// `fmt` must be a null-terminated byte slice containing a format
    /// string (e.g. `b"Count: %d\0"`).
    pub fn bind_text<T: Copy + Into<i32> + TryFrom<i32>>(
        self,
        state: &State<T>,
        fmt: &'static [u8],
    ) -> Self {
        unsafe {
            bindings::lv_label_bind_text(self.raw(), state.subject_ptr(), fmt.as_ptr() as *const _);
        }
        self
    }
}

impl Arc {
    /// Bind this arc's value to a reactive state.
    pub fn bind_value<T: Copy + Into<i32> + TryFrom<i32>>(self, state: &State<T>) -> Self {
        unsafe { bindings::lv_arc_bind_value(self.raw(), state.subject_ptr()) };
        self
    }
}

impl Slider {
    /// Bind this slider's value to a reactive state.
    pub fn bind_value<T: Copy + Into<i32> + TryFrom<i32>>(self, state: &State<T>) -> Self {
        unsafe { bindings::lv_slider_bind_value(self.raw(), state.subject_ptr()) };
        self
    }
}

impl Roller {
    /// Bind this roller's selected index to a reactive state.
    pub fn bind_value<T: Copy + Into<i32> + TryFrom<i32>>(self, state: &State<T>) -> Self {
        unsafe { bindings::lv_roller_bind_value(self.raw(), state.subject_ptr()) };
        self
    }
}

impl Dropdown {
    /// Bind this dropdown's selected index to a reactive state.
    pub fn bind_value<T: Copy + Into<i32> + TryFrom<i32>>(self, state: &State<T>) -> Self {
        unsafe { bindings::lv_dropdown_bind_value(self.raw(), state.subject_ptr()) };
        self
    }
}

// =========================================================================
//  Component primitives — user_data + from_event walk
// =========================================================================

/// Walk up from the event's target through the parent chain looking for
/// any non-null `user_data` pointer, returning the first one found.
///
/// Used by Component-style patterns to recover the owning component
/// instance from inside an event callback. The caller must cast the
/// returned pointer to the correct type.
///
/// # Safety
///
/// `e` must be a valid `lv_event_t *` active inside an event callback.
/// The returned pointer is whatever the widget tree's user_data holds —
/// the caller is responsible for its type.
pub unsafe fn component_from_event(e: *mut bindings::lv_event_t) -> *mut core::ffi::c_void {
    unsafe {
        // lv_event_get_target may return *mut c_void or *mut lv_obj_t
        // depending on bindgen config; cast normalises to *mut lv_obj_t
        // for the lv_obj_get_user_data / lv_obj_get_parent calls below.
        let mut target = bindings::lv_event_get_target(e) as *mut bindings::lv_obj_t;
        while !target.is_null() {
            let ud = bindings::lv_obj_get_user_data(target);
            if !ud.is_null() {
                return ud;
            }
            target = bindings::lv_obj_get_parent(target);
        }
        core::ptr::null_mut()
    }
}

// =========================================================================
//  Animation — builder for lv_anim_t
// =========================================================================

/// LVGL repeat-forever sentinel for [`Animation::repeat_count`].
pub const ANIM_REPEAT_INFINITE: u32 = 0xFFFF_FFFF;

/// Fluent builder for an LVGL animation.
///
/// Configure the animation step-by-step, then call [`Animation::start`] —
/// LVGL copies the state into its internal animation list, so the
/// builder can be dropped immediately after.
///
/// # Callback constraints
/// In `no_std` Rust we're stuck with `extern "C" fn` pointers for
/// `exec_cb` / `ready_cb` — no closures. Use the [`animate_x`],
/// [`animate_y`], [`animate_width`], [`animate_opa`] helpers for the
/// common cases where you just want to tween a single property.
pub struct Animation {
    inner: bindings::lv_anim_t,
}

impl Animation {
    /// Create a fresh animation with default values (duration 0, no exec cb).
    pub fn new() -> Self {
        let mut a: bindings::lv_anim_t = unsafe { core::mem::zeroed() };
        unsafe { bindings::lv_anim_init(&mut a) };
        Self { inner: a }
    }

    /// Set the target variable (typically `obj.raw() as *mut c_void`).
    pub fn target(mut self, var: *mut core::ffi::c_void) -> Self {
        unsafe { bindings::lv_anim_set_var(&mut self.inner, var) };
        self
    }

    /// Set start and end values.
    pub fn values(mut self, from: i32, to: i32) -> Self {
        unsafe { bindings::lv_anim_set_values(&mut self.inner, from, to) };
        self
    }

    /// Set duration in milliseconds.
    pub fn duration(mut self, ms: u32) -> Self {
        unsafe { bindings::lv_anim_set_duration(&mut self.inner, ms) };
        self
    }

    /// Set the delay before the animation starts.
    pub fn delay(mut self, ms: u32) -> Self {
        unsafe { bindings::lv_anim_set_delay(&mut self.inner, ms) };
        self
    }

    /// Set the easing path callback. Use e.g. [`path_ease_out`].
    pub fn path(mut self, cb: bindings::lv_anim_path_cb_t) -> Self {
        unsafe { bindings::lv_anim_set_path_cb(&mut self.inner, cb) };
        self
    }

    /// Set the repeat count; use [`ANIM_REPEAT_INFINITE`] for endless.
    pub fn repeat_count(mut self, count: u32) -> Self {
        unsafe { bindings::lv_anim_set_repeat_count(&mut self.inner, count) };
        self
    }

    /// Set the delay between repeats.
    pub fn repeat_delay(mut self, ms: u32) -> Self {
        unsafe { bindings::lv_anim_set_repeat_delay(&mut self.inner, ms) };
        self
    }

    /// Set the duration of the reverse (playback) phase.
    pub fn playback_duration(mut self, ms: u32) -> Self {
        unsafe { bindings::lv_anim_set_reverse_duration(&mut self.inner, ms) };
        self
    }

    /// Set the delay before the playback phase.
    pub fn playback_delay(mut self, ms: u32) -> Self {
        unsafe { bindings::lv_anim_set_reverse_delay(&mut self.inner, ms) };
        self
    }

    /// Set the exec callback invoked every frame with `(var, value)`.
    pub fn exec_cb(mut self, cb: bindings::lv_anim_exec_xcb_t) -> Self {
        unsafe { bindings::lv_anim_set_exec_cb(&mut self.inner, cb) };
        self
    }

    /// Set the callback invoked when the animation completes (`lv_anim_set_completed_cb`).
    pub fn completed_cb(mut self, cb: bindings::lv_anim_completed_cb_t) -> Self {
        unsafe { bindings::lv_anim_set_completed_cb(&mut self.inner, cb) };
        self
    }

    /// Start the animation. LVGL copies internal state, so the builder
    /// can be dropped immediately after.
    pub fn start(self) {
        unsafe { bindings::lv_anim_start(&self.inner) };
    }

    /// Stop any animations matching `(var, exec_cb)`.
    pub fn stop(var: *mut core::ffi::c_void, exec_cb: bindings::lv_anim_exec_xcb_t) -> bool {
        unsafe { bindings::lv_anim_delete(var, exec_cb) }
    }
}

impl Default for Animation {
    fn default() -> Self {
        Self::new()
    }
}

/// Linear easing path (`lv_anim_path_linear`).
pub fn path_linear() -> bindings::lv_anim_path_cb_t {
    Some(bindings::lv_anim_path_linear)
}

/// Ease-in path (`lv_anim_path_ease_in`).
pub fn path_ease_in() -> bindings::lv_anim_path_cb_t {
    Some(bindings::lv_anim_path_ease_in)
}

/// Ease-out path (`lv_anim_path_ease_out`).
pub fn path_ease_out() -> bindings::lv_anim_path_cb_t {
    Some(bindings::lv_anim_path_ease_out)
}

/// Ease-in-out path (`lv_anim_path_ease_in_out`).
pub fn path_ease_in_out() -> bindings::lv_anim_path_cb_t {
    Some(bindings::lv_anim_path_ease_in_out)
}

/// Overshoot path (`lv_anim_path_overshoot`).
pub fn path_overshoot() -> bindings::lv_anim_path_cb_t {
    Some(bindings::lv_anim_path_overshoot)
}

/// Bounce path (`lv_anim_path_bounce`).
pub fn path_bounce() -> bindings::lv_anim_path_cb_t {
    Some(bindings::lv_anim_path_bounce)
}

/// Step path (`lv_anim_path_step`).
pub fn path_step() -> bindings::lv_anim_path_cb_t {
    Some(bindings::lv_anim_path_step)
}

// Shims that adapt widget setters (`fn(*mut lv_obj_t, i32)`) to the
// animation exec callback signature (`fn(*mut c_void, i32)`).
unsafe extern "C" fn anim_set_x_shim(var: *mut core::ffi::c_void, v: i32) {
    unsafe { bindings::lv_obj_set_x(var as *mut bindings::lv_obj_t, v) };
}

unsafe extern "C" fn anim_set_y_shim(var: *mut core::ffi::c_void, v: i32) {
    unsafe { bindings::lv_obj_set_y(var as *mut bindings::lv_obj_t, v) };
}

unsafe extern "C" fn anim_set_width_shim(var: *mut core::ffi::c_void, v: i32) {
    unsafe { bindings::lv_obj_set_width(var as *mut bindings::lv_obj_t, v) };
}

unsafe extern "C" fn anim_set_opa_shim(var: *mut core::ffi::c_void, v: i32) {
    unsafe { bindings::lv_obj_set_style_opa(var as *mut bindings::lv_obj_t, v as u8, PART_MAIN) };
}

/// Animate an object's X position to `to` over `duration_ms` (ease-out).
pub fn animate_x(obj: impl Widget, to: i32, duration_ms: u32) {
    let from = unsafe { bindings::lv_obj_get_x(obj.raw()) };
    Animation::new()
        .target(obj.raw() as *mut _)
        .values(from, to)
        .duration(duration_ms)
        .path(path_ease_out())
        .exec_cb(Some(anim_set_x_shim))
        .start();
}

/// Animate an object's Y position to `to` over `duration_ms` (ease-out).
pub fn animate_y(obj: impl Widget, to: i32, duration_ms: u32) {
    let from = unsafe { bindings::lv_obj_get_y(obj.raw()) };
    Animation::new()
        .target(obj.raw() as *mut _)
        .values(from, to)
        .duration(duration_ms)
        .path(path_ease_out())
        .exec_cb(Some(anim_set_y_shim))
        .start();
}

/// Animate an object's width to `to` over `duration_ms` (ease-out).
pub fn animate_width(obj: impl Widget, to: i32, duration_ms: u32) {
    let from = unsafe { bindings::lv_obj_get_width(obj.raw()) };
    Animation::new()
        .target(obj.raw() as *mut _)
        .values(from, to)
        .duration(duration_ms)
        .path(path_ease_out())
        .exec_cb(Some(anim_set_width_shim))
        .start();
}

/// Fade an object's main-part opacity from `from` to `to` over `duration_ms`.
///
/// Unlike the C++/Zig counterparts this requires the caller to pass the
/// starting opacity explicitly because `lv_obj_get_style_opa` is a
/// static inline that Rust bindgen doesn't expose.
pub fn animate_opa(obj: impl Widget, from: u8, to: u8, duration_ms: u32) {
    Animation::new()
        .target(obj.raw() as *mut _)
        .values(from as i32, to as i32)
        .duration(duration_ms)
        .path(path_ease_in_out())
        .exec_cb(Some(anim_set_opa_shim))
        .start();
}

// =========================================================================
//  Style — RAII style object
// =========================================================================

/// RAII wrapper around `lv_style_t`. Calls `lv_style_reset` on drop.
pub struct Style {
    inner: bindings::lv_style_t,
}

impl Default for Style {
    fn default() -> Self {
        Self::new()
    }
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

    /// Return the raw pointer from a shared reference, for APIs where LVGL
    /// only reads through the pointer (e.g. `lv_obj_add_style`,
    /// `lv_scale_section_set_style`). LVGL conventionally does not mutate
    /// styles once they've been applied.
    pub fn ptr(&self) -> *mut bindings::lv_style_t {
        &self.inner as *const _ as *mut _
    }

    /// Set the arc color in this style.
    pub fn arc_color(mut self, c: Color) -> Self {
        unsafe { bindings::lv_style_set_arc_color(&mut self.inner, c.to_raw()) };
        self
    }

    /// Set the arc width in this style.
    pub fn arc_width(mut self, w: i32) -> Self {
        unsafe { bindings::lv_style_set_arc_width(&mut self.inner, w) };
        self
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

// SAFETY: Style access is serialized by the global LVGL lock.
unsafe impl Send for Style {}
unsafe impl Sync for Style {}

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
unsafe impl Send for Button {}
unsafe impl Sync for Button {}
unsafe impl Send for Slider {}
unsafe impl Sync for Slider {}
unsafe impl Send for Switch {}
unsafe impl Sync for Switch {}
unsafe impl Send for Checkbox {}
unsafe impl Sync for Checkbox {}
unsafe impl Send for Arc {}
unsafe impl Sync for Arc {}
unsafe impl Send for Image {}
unsafe impl Sync for Image {}
unsafe impl Send for Screen {}
unsafe impl Sync for Screen {}
unsafe impl Send for Msgbox {}
unsafe impl Sync for Msgbox {}
unsafe impl Send for Spinner {}
unsafe impl Sync for Spinner {}
unsafe impl Send for Led {}
unsafe impl Sync for Led {}
unsafe impl Send for Textarea {}
unsafe impl Sync for Textarea {}
unsafe impl Send for Dropdown {}
unsafe impl Sync for Dropdown {}
unsafe impl Send for Roller {}
unsafe impl Sync for Roller {}
unsafe impl Send for Spinbox {}
unsafe impl Sync for Spinbox {}
unsafe impl Send for Keyboard {}
unsafe impl Sync for Keyboard {}
unsafe impl Send for Chart {}
unsafe impl Sync for Chart {}
unsafe impl Send for Table {}
unsafe impl Sync for Table {}
unsafe impl Send for Tabview {}
unsafe impl Sync for Tabview {}
unsafe impl Send for List {}
unsafe impl Sync for List {}
unsafe impl Send for Canvas {}
unsafe impl Sync for Canvas {}
unsafe impl Send for Calendar {}
unsafe impl Sync for Calendar {}

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

// =========================================================================
//  Display info (default display getters)
// =========================================================================

/// Helpers querying the default LVGL display.
pub mod display {
    use super::bindings;

    /// Horizontal resolution of the default display in pixels.
    pub fn width() -> i32 {
        unsafe { bindings::lv_display_get_horizontal_resolution(core::ptr::null_mut()) }
    }

    /// Vertical resolution of the default display in pixels.
    pub fn height() -> i32 {
        unsafe { bindings::lv_display_get_vertical_resolution(core::ptr::null_mut()) }
    }

    /// DPI of the default display.
    pub fn dpi() -> i32 {
        unsafe { bindings::lv_display_get_dpi(core::ptr::null_mut()) }
    }
}

// =========================================================================
//  Top-layer access
// =========================================================================

/// Top-most overlay layer of the default display (sits above all screens).
pub fn layer_top() -> Obj {
    let raw = unsafe { bindings::lv_layer_top() };
    unsafe { Obj::from_raw(raw) }
}

// =========================================================================
//  Text metrics
// =========================================================================

/// Measure the rendered size of `text` using `font`. Returns `(width, height)`
/// in pixels.
///
/// `text` must be NUL-terminated (LVGL reads C strings).
pub fn text_size(text: &[u8], font: *const bindings::lv_font_t) -> (i32, i32) {
    debug_assert!(
        text.last() == Some(&0),
        "text_size: text must be NUL-terminated"
    );
    let mut p = bindings::lv_point_t { x: 0, y: 0 };
    unsafe {
        bindings::lv_text_get_size(&mut p, text.as_ptr() as *const _, font, 0, 0, i32::MAX, 0);
    }
    (p.x, p.y)
}

// =========================================================================
//  Animation extras
// =========================================================================

impl Animation {
    /// Configure the animation duration based on a movement speed
    /// (LVGL's `lv_anim_speed`). Returns the precomputed duration value.
    pub fn duration_for_speed(speed: u32) -> u32 {
        unsafe { bindings::lv_anim_speed(speed) }
    }

    /// Set both the animation target and a safe tick callback.
    ///
    /// `on_tick(obj, v)` is invoked every frame with the target widget
    /// (as [`Obj`]) and the interpolated value. Uses LVGL's custom exec
    /// callback so the `fn` pointer is smuggled through `user_data`.
    pub fn tick_fn<W: Widget>(mut self, obj: W, on_tick: fn(Obj, i32)) -> Self {
        unsafe {
            bindings::lv_anim_set_var(&mut self.inner, obj.raw() as *mut _);
            bindings::lv_anim_set_user_data(&mut self.inner, on_tick as *mut _);
            bindings::lv_anim_set_custom_exec_cb(
                &mut self.inner,
                Some(anim_custom_tick_trampoline),
            );
        }
        self
    }
}

unsafe extern "C" fn anim_custom_tick_trampoline(a: *mut bindings::lv_anim_t, v: i32) {
    unsafe {
        let ud = bindings::lv_anim_get_user_data(a);
        if ud.is_null() {
            return;
        }
        // SAFETY: `ud` was stored from a `fn(Obj, i32)` pointer by the
        // custom-tick animation setup. Function pointers round-trip through
        // `*mut c_void` on supported targets.
        let cb: fn(Obj, i32) = core::mem::transmute(ud);
        let var = (*a).var;
        cb(
            Obj {
                raw: var as *mut bindings::lv_obj_t,
            },
            v,
        );
    }
}

// Internal trampolines for additional convenience animators.
unsafe extern "C" fn anim_translate_y_shim(var: *mut core::ffi::c_void, v: i32) {
    unsafe {
        bindings::lv_obj_set_style_translate_y(var as *mut bindings::lv_obj_t, v, PART_MAIN);
    }
}

unsafe extern "C" fn anim_scroll_y_shim(var: *mut core::ffi::c_void, v: i32) {
    unsafe {
        bindings::lv_obj_scroll_to_y(var as *mut bindings::lv_obj_t, v, false);
    }
}

unsafe extern "C" fn anim_arc_value_shim(var: *mut core::ffi::c_void, v: i32) {
    unsafe {
        bindings::lv_arc_set_value(var as *mut bindings::lv_obj_t, v);
    }
}

/// Animate the `translate_y` style property from `from` to `to` over
/// `duration_ms` (ease-in-out).
pub fn animate_translate_y(obj: impl Widget, from: i32, to: i32, duration_ms: u32) {
    Animation::new()
        .target(obj.raw() as *mut _)
        .values(from, to)
        .duration(duration_ms)
        .path(path_ease_in_out())
        .exec_cb(Some(anim_translate_y_shim))
        .start();
}

/// Animate the vertical scroll position to `to` (no easing — direct scroll).
pub fn animate_scroll_y(obj: impl Widget, from: i32, to: i32, duration_ms: u32) {
    Animation::new()
        .target(obj.raw() as *mut _)
        .values(from, to)
        .duration(duration_ms)
        .path(path_linear())
        .exec_cb(Some(anim_scroll_y_shim))
        .start();
}

/// Animate an [`Arc`] widget's value from `from` to `to` over `duration_ms`.
pub fn animate_arc_value(arc: Arc, from: i32, to: i32, duration_ms: u32) {
    Animation::new()
        .target(arc.raw() as *mut _)
        .values(from, to)
        .duration(duration_ms)
        .path(path_ease_in_out())
        .exec_cb(Some(anim_arc_value_shim))
        .start();
}

/// Animate `translate_y` forward for `forward_ms` then back for `playback_ms`,
/// looping forever. Useful for subtle "shake" / bob animations.
pub fn animate_translate_y_playback(
    obj: impl Widget,
    from: i32,
    to: i32,
    forward_ms: u32,
    playback_ms: u32,
) {
    Animation::new()
        .target(obj.raw() as *mut _)
        .values(from, to)
        .duration(forward_ms)
        .playback_duration(playback_ms)
        .path(path_ease_in_out())
        .exec_cb(Some(anim_translate_y_shim))
        .repeat_count(ANIM_REPEAT_INFINITE)
        .start();
}

/// Animate the vertical scroll position forward then back, looping forever.
pub fn animate_scroll_y_playback(
    obj: impl Widget,
    from: i32,
    to: i32,
    forward_ms: u32,
    playback_ms: u32,
) {
    Animation::new()
        .target(obj.raw() as *mut _)
        .values(from, to)
        .duration(forward_ms)
        .playback_duration(playback_ms)
        .path(path_linear())
        .exec_cb(Some(anim_scroll_y_shim))
        .repeat_count(ANIM_REPEAT_INFINITE)
        .start();
}

/// Animate an [`Arc`] value forward then back, looping forever.
pub fn animate_arc_value_playback(arc: Arc, from: i32, to: i32, forward_ms: u32, playback_ms: u32) {
    Animation::new()
        .target(arc.raw() as *mut _)
        .values(from, to)
        .duration(forward_ms)
        .playback_duration(playback_ms)
        .path(path_ease_in_out())
        .exec_cb(Some(anim_arc_value_shim))
        .repeat_count(ANIM_REPEAT_INFINITE)
        .start();
}

/// Stop all animations with any exec callback running on `obj`.
///
/// LVGL's `lv_anim_delete(var, NULL)` matches all callbacks for the given variable.
pub fn stop_animations(obj: impl Widget) {
    unsafe {
        bindings::lv_anim_delete(obj.raw() as *mut _, None);
    }
}

// =========================================================================
//  LvCell / LvRefCell — re-exported from the top-level `cell` module
// =========================================================================

pub use crate::cell::{LvCell, LvRefCell};

// =========================================================================
//  ImageSrc — typed safe image source for Image::source
// =========================================================================

/// A safe LVGL image source, constructible only from `'static` data.
///
/// LVGL recognizes three flavours of source pointer: an image descriptor
/// (`lv_image_dsc_t`), a built-in symbol byte string (`LV_SYMBOL_*`), and
/// a NUL-terminated filesystem path (FS driver required). All three are
/// covered with type-safe constructors.
#[derive(Clone, Copy)]
pub struct ImageSrc {
    raw: *const core::ffi::c_void,
}

// SAFETY: the inner pointer is always `'static` and read-only.
unsafe impl Send for ImageSrc {}
unsafe impl Sync for ImageSrc {}

impl ImageSrc {
    /// Wrap an LVGL image descriptor by pointer. The pointed-to data must
    /// outlive any widget using this source — `'static` in practice.
    ///
    /// Typical call: `ImageSrc::from_dsc(core::ptr::addr_of!(images::badge))`
    /// where `images::badge` is a generated `extern "C" static` image.
    pub fn from_dsc(dsc: *const ImageDsc) -> Self {
        Self {
            raw: dsc as *const _,
        }
    }

    /// Wrap a NUL-terminated symbol byte string (e.g. `LV_SYMBOL_OK`).
    pub fn from_symbol(sym: &'static [u8]) -> Self {
        debug_assert!(sym.last() == Some(&0), "ImageSrc::from_symbol: missing NUL");
        Self {
            raw: sym.as_ptr() as *const _,
        }
    }

    /// Wrap a NUL-terminated filesystem path (LVGL FS driver must be registered).
    pub fn from_path(path: &'static [u8]) -> Self {
        debug_assert!(path.last() == Some(&0), "ImageSrc::from_path: missing NUL");
        Self {
            raw: path.as_ptr() as *const _,
        }
    }

    pub(crate) fn raw_ptr(self) -> *const core::ffi::c_void {
        self.raw
    }
}

/// Safe re-export of the LVGL image descriptor type, so generated asset
/// modules (e.g. `images::badge`) can declare their externals without
/// needing to reach into the `ove::ffi` escape hatch.
pub type ImageDsc = bindings::lv_image_dsc_t;

impl Image {
    /// Safe replacement for the legacy [`Image::src`] — accepts a typed
    /// [`ImageSrc`] guaranteeing the underlying memory is `'static`.
    pub fn source(self, src: ImageSrc) -> Self {
        unsafe { bindings::lv_image_set_src(self.raw, src.raw) };
        self
    }

    /// Set the inner alignment of the image content (`LV_IMAGE_ALIGN_*`).
    pub fn inner_align(self, align: u32) -> Self {
        unsafe { bindings::lv_image_set_inner_align(self.raw, align as _) };
        self
    }
}

// =========================================================================
//  ColorFormat / CanvasBuffer — typed safe canvas pixel storage
// =========================================================================

/// LVGL color format selector (matches `LV_COLOR_FORMAT_*`).
#[derive(Clone, Copy)]
pub struct ColorFormat(pub bindings::lv_color_format_t);

impl ColorFormat {
    pub const I1: Self = Self(0x07);
    pub const A8: Self = Self(0x0E);
    pub const RGB565: Self = Self(0x12);
    pub const RGB888: Self = Self(0x0F);
    pub const ARGB8888: Self = Self(0x10);
    pub const XRGB8888: Self = Self(0x11);

    /// Bytes per pixel for this format. Panics for unknown formats.
    pub const fn bpp(self) -> usize {
        match self.0 {
            0x10 | 0x11 => 4, // ARGB8888 / XRGB8888
            0x0F => 3,        // RGB888
            0x12 => 2,        // RGB565
            0x0E => 1,        // A8
            0x07 => 0,        // I1: < 1 byte/px (caller must size externally)
            _ => panic!("ColorFormat::bpp: unknown format"),
        }
    }
}

/// Pixel storage for a [`Canvas`]. Borrows the buffer for the lifetime `'a`.
pub struct CanvasBuffer<'a> {
    ptr: *mut u8,
    w: i32,
    h: i32,
    cf: ColorFormat,
    _ph: core::marker::PhantomData<&'a mut [u8]>,
}

impl<'a> CanvasBuffer<'a> {
    /// Wrap a mutable byte slice as canvas pixel storage.
    ///
    /// Asserts `buf.len() == w * h * cf.bpp()`. Panics on mismatch — sizing
    /// errors are programming bugs, not runtime conditions.
    pub fn new(buf: &'a mut [u8], w: i32, h: i32, cf: ColorFormat) -> Self {
        let need = (w as usize) * (h as usize) * cf.bpp();
        assert!(
            buf.len() == need,
            "CanvasBuffer: expected {} bytes, got {}",
            need,
            buf.len()
        );
        Self {
            ptr: buf.as_mut_ptr(),
            w,
            h,
            cf,
            _ph: core::marker::PhantomData,
        }
    }
}

impl Canvas {
    /// Safe replacement for the legacy [`Canvas::buffer`] — accepts a
    /// typed [`CanvasBuffer`] borrowed for at least as long as the canvas
    /// is used.
    pub fn set_buffer(self, buf: CanvasBuffer<'_>) -> Self {
        unsafe {
            bindings::lv_canvas_set_buffer(self.raw, buf.ptr as *mut _, buf.w, buf.h, buf.cf.0);
        }
        self
    }
}

// =========================================================================
//  Scale — radial / linear scale widget
// =========================================================================

/// LVGL scale modes (`LV_SCALE_MODE_*`).
pub const SCALE_MODE_HORIZONTAL_TOP: u32 = 0x00;
pub const SCALE_MODE_HORIZONTAL_BOTTOM: u32 = 0x01;
pub const SCALE_MODE_VERTICAL_LEFT: u32 = 0x02;
pub const SCALE_MODE_VERTICAL_RIGHT: u32 = 0x04;
pub const SCALE_MODE_ROUND_INNER: u32 = 0x08;
pub const SCALE_MODE_ROUND_OUTER: u32 = 0x10;

/// LVGL scale widget — linear or radial scale with ticks and labels.
#[derive(Clone, Copy)]
pub struct Scale {
    raw: *mut bindings::lv_obj_t,
}

/// Handle to an `lv_scale_section_t *`.
#[derive(Clone, Copy)]
pub struct ScaleSection {
    raw: *mut bindings::lv_scale_section_t,
}

impl Scale {
    /// Create a new scale as a child of `parent`.
    pub fn create(parent: impl Widget) -> Self {
        let raw = unsafe { bindings::lv_scale_create(parent.raw()) };
        Self { raw }
    }

    /// Fluent: set the layout mode (use `SCALE_MODE_*`).
    pub fn mode(self, mode: u32) -> Self {
        unsafe { bindings::lv_scale_set_mode(self.raw, mode as _) };
        self
    }

    /// Fluent: set the value range.
    pub fn range(self, min: i32, max: i32) -> Self {
        unsafe { bindings::lv_scale_set_range(self.raw, min, max) };
        self
    }

    /// Fluent: total number of tick marks.
    pub fn total_tick_count(self, count: u32) -> Self {
        unsafe { bindings::lv_scale_set_total_tick_count(self.raw, count) };
        self
    }

    /// Fluent: every Nth tick is a major tick.
    pub fn major_tick_every(self, nth: u32) -> Self {
        unsafe { bindings::lv_scale_set_major_tick_every(self.raw, nth) };
        self
    }

    /// Fluent: angle range for round scales.
    pub fn angle_range(self, angle: u32) -> Self {
        unsafe { bindings::lv_scale_set_angle_range(self.raw, angle) };
        self
    }

    /// Fluent: rotation offset for round scales.
    pub fn rotation(self, rot: i32) -> Self {
        unsafe { bindings::lv_scale_set_rotation(self.raw, rot as _) };
        self
    }

    /// Add a coloured/styled section to the scale.
    pub fn add_section(self) -> ScaleSection {
        let raw = unsafe { bindings::lv_scale_add_section(self.raw) };
        ScaleSection { raw }
    }
}

impl ScaleSection {
    /// Set the section value range.
    pub fn range(self, min: i32, max: i32) -> Self {
        unsafe { bindings::lv_scale_section_set_range(self.raw, min, max) };
        self
    }

    /// Apply a [`Style`] to the given part of the section.
    ///
    /// Takes `&Style` — the style must outlive the scale (typical use:
    /// store in a `InitCell<Style>` or `&'static`).
    pub fn style(self, part: u32, style: &Style) -> Self {
        unsafe { bindings::lv_scale_section_set_style(self.raw, part, style.ptr()) };
        self
    }
}

impl Widget for Scale {
    fn raw(self) -> *mut bindings::lv_obj_t {
        self.raw
    }
}

impl core::ops::Deref for Scale {
    type Target = Obj;
    fn deref(&self) -> &Obj {
        unsafe { &*(self as *const Scale as *const Obj) }
    }
}

unsafe impl Send for Scale {}
unsafe impl Sync for Scale {}

// =========================================================================
//  LvTimer extension — safe `fn`-callback constructor
// =========================================================================

unsafe extern "C" fn lvgl_timer_trampoline_fn(t: *mut bindings::lv_timer_t) {
    let ud = unsafe { bindings::lv_timer_get_user_data(t) };
    if ud.is_null() {
        return;
    }
    // SAFETY: stored as a `fn()` pointer by `Timer::new_fn`.
    let cb: fn() = unsafe { core::mem::transmute(ud) };
    cb();
}

impl Timer {
    /// Create an LVGL timer with a safe `fn()` callback.
    ///
    /// The function pointer is smuggled through `lv_timer_t::user_data`;
    /// an internal trampoline calls it with no arguments.
    pub fn new_fn(callback: fn(), period_ms: u32) -> Self {
        let ud = callback as *mut core::ffi::c_void;
        let raw =
            unsafe { bindings::lv_timer_create(Some(lvgl_timer_trampoline_fn), period_ms, ud) };
        Self { raw }
    }
}

// =========================================================================
//  Subject extension — safe Obj <-> Subject linking
// =========================================================================

impl<T: Copy + Into<i32> + TryFrom<i32>> State<T> {
    /// Attach an observer that automatically refreshes `obj` when this
    /// state changes. The actual rebinding logic is on the widget side
    /// (e.g. [`Label::bind_text`]) — this is a low-level escape hatch.
    pub fn observe(&self, obj: impl Widget) {
        unsafe {
            bindings::lv_subject_add_observer_obj(
                self.subject_ptr(),
                None,
                obj.raw(),
                core::ptr::null_mut(),
            );
        }
    }
}
