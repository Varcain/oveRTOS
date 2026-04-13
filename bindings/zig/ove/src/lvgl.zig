// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

// LVGL bindings — widget types, mixins, and fluent builder API.
// Conditional: only useful when CONFIG_OVE_LVGL is enabled.
//
// Uses its own @cImport for LVGL headers (separate from the core oveRTOS
// cimport) because lvgl/lvgl.h is only available when LVGL is enabled.
// The oveRTOS LVGL C API (ove_lvgl_*) is imported from the core c.zig.

const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

// =========================================================================
//  Module-level functions
// =========================================================================

/// Initialize the oveRTOS LVGL integration.
///
/// Must be called before any widget or drawing API is used.
/// Registers the display driver and input devices with the LVGL core.
pub fn init() Error!void {
    try err.fromCode(c.ove_lvgl_init());
}

/// Advance the LVGL animation and timer engine by `ms` milliseconds.
///
/// Call this from a periodic task (e.g. every 5 ms) to keep animations
/// and internal timers progressing correctly.
pub fn tick(ms: u32) void {
    c.ove_lvgl_tick(ms);
}

/// Process all pending LVGL timer events and redraw dirty areas.
///
/// Must be called periodically (typically the same period as `tick`).
pub fn handler() void {
    c.ove_lvgl_handler();
}

// =========================================================================
//  LvglGuard — RAII lock for thread-safe LVGL access
// =========================================================================

/// RAII guard that holds the LVGL global lock.
///
/// Obtain via `lock()`. The lock is released automatically when `deinit()`
/// is called, typically via `defer`:
///
///     const g = lvgl.lock();
///     defer g.deinit();
pub const LvglGuard = struct {
    /// Release the LVGL lock. Call via `defer` after `lock()`.
    pub fn deinit(_: LvglGuard) void {
        c.ove_lvgl_unlock();
    }
};

/// Acquire the LVGL global mutex and return an RAII guard.
///
/// All LVGL API calls from non-LVGL threads must be wrapped between
/// `lock()` / `guard.deinit()` to prevent concurrent access:
///
///     const g = lvgl.lock();
///     defer g.deinit();
///     _ = lvgl.Label.create(screen);
pub fn lock() LvglGuard {
    c.ove_lvgl_lock();
    return .{};
}

/// Release the LVGL global mutex directly.
///
/// Prefer `lock()` + `LvglGuard.deinit()` for RAII safety.
/// Only use this if the unlock must happen at a non-lexical scope.
pub fn unlock() void {
    c.ove_lvgl_unlock();
}

// =========================================================================
//  Color
// =========================================================================

/// Alias for `lv_color_t`. Represents an RGB color value.
pub const Color = c.lv_color_t;

/// Create a `Color` from individual red, green, and blue components (0–255).
pub fn colorMake(r: u8, g: u8, b: u8) Color {
    return c.lv_color_make(r, g, b);
}

/// Create a `Color` from a packed 24-bit hex value (e.g. `0xFF8800`).
pub fn colorHex(h: u32) Color {
    return c.lv_color_hex(h);
}

/// Return the predefined white color.
pub fn colorWhite() Color {
    return c.lv_color_white();
}

/// Return the predefined black color.
pub fn colorBlack() Color {
    return c.lv_color_black();
}

// lv_anim_enable_t varies across LVGL configurations (bool, enum, int).
fn coerceAnim(comptime val: comptime_int, comptime to_bool: bool) c.lv_anim_enable_t {
    const T = c.lv_anim_enable_t;
    return if (T == bool)
        to_bool
    else switch (@typeInfo(T)) {
        .@"enum" => @enumFromInt(val),
        else => @intCast(val),
    };
}

/// Return the animation-enabled sentinel, compatible with all LVGL configs.
pub fn animOn() c.lv_anim_enable_t {
    return coerceAnim(1, true);
}

/// Return the animation-disabled sentinel, compatible with all LVGL configs.
pub fn animOff() c.lv_anim_enable_t {
    return coerceAnim(0, false);
}

/// Return the main (500-level) color from an LVGL material design palette.
pub fn paletteMain(p: c.lv_palette_t) Color {
    return c.lv_palette_main(p);
}

/// Return a lightened variant of a palette color (level 1–5, lightest=5).
pub fn paletteLighten(p: c.lv_palette_t, level: u8) Color {
    return c.lv_palette_lighten(p, level);
}

/// Return a darkened variant of a palette color (level 1–4, darkest=4).
pub fn paletteDarken(p: c.lv_palette_t, level: u8) Color {
    return c.lv_palette_darken(p, level);
}

// =========================================================================
//  Constants — re-exported from @cImport
// =========================================================================

// Alignment
/// Default alignment (LV_ALIGN_DEFAULT).
pub const ALIGN_DEFAULT = c.LV_ALIGN_DEFAULT;
/// Align to the top-left corner of the parent.
pub const ALIGN_TOP_LEFT = c.LV_ALIGN_TOP_LEFT;
/// Align to the top-center of the parent.
pub const ALIGN_TOP_MID = c.LV_ALIGN_TOP_MID;
/// Align to the top-right corner of the parent.
pub const ALIGN_TOP_RIGHT = c.LV_ALIGN_TOP_RIGHT;
/// Align to the bottom-left corner of the parent.
pub const ALIGN_BOTTOM_LEFT = c.LV_ALIGN_BOTTOM_LEFT;
/// Align to the bottom-center of the parent.
pub const ALIGN_BOTTOM_MID = c.LV_ALIGN_BOTTOM_MID;
/// Align to the bottom-right corner of the parent.
pub const ALIGN_BOTTOM_RIGHT = c.LV_ALIGN_BOTTOM_RIGHT;
/// Align to the middle of the left edge of the parent.
pub const ALIGN_LEFT_MID = c.LV_ALIGN_LEFT_MID;
/// Align to the middle of the right edge of the parent.
pub const ALIGN_RIGHT_MID = c.LV_ALIGN_RIGHT_MID;
/// Center within the parent object.
pub const ALIGN_CENTER = c.LV_ALIGN_CENTER;

// Animation
/// Animation enabled (use with bar/slider value setters).
pub const ANIM_ON = c.LV_ANIM_ON;
/// Animation disabled (use with bar/slider value setters).
pub const ANIM_OFF = c.LV_ANIM_OFF;

// Palette
/// Material Design Red palette.
pub const PALETTE_RED = c.LV_PALETTE_RED;
/// Material Design Pink palette.
pub const PALETTE_PINK = c.LV_PALETTE_PINK;
/// Material Design Purple palette.
pub const PALETTE_PURPLE = c.LV_PALETTE_PURPLE;
/// Material Design Deep Purple palette.
pub const PALETTE_DEEP_PURPLE = c.LV_PALETTE_DEEP_PURPLE;
/// Material Design Indigo palette.
pub const PALETTE_INDIGO = c.LV_PALETTE_INDIGO;
/// Material Design Blue palette.
pub const PALETTE_BLUE = c.LV_PALETTE_BLUE;
/// Material Design Light Blue palette.
pub const PALETTE_LIGHT_BLUE = c.LV_PALETTE_LIGHT_BLUE;
/// Material Design Cyan palette.
pub const PALETTE_CYAN = c.LV_PALETTE_CYAN;
/// Material Design Teal palette.
pub const PALETTE_TEAL = c.LV_PALETTE_TEAL;
/// Material Design Green palette.
pub const PALETTE_GREEN = c.LV_PALETTE_GREEN;
/// Material Design Light Green palette.
pub const PALETTE_LIGHT_GREEN = c.LV_PALETTE_LIGHT_GREEN;
/// Material Design Lime palette.
pub const PALETTE_LIME = c.LV_PALETTE_LIME;
/// Material Design Yellow palette.
pub const PALETTE_YELLOW = c.LV_PALETTE_YELLOW;
/// Material Design Amber palette.
pub const PALETTE_AMBER = c.LV_PALETTE_AMBER;
/// Material Design Orange palette.
pub const PALETTE_ORANGE = c.LV_PALETTE_ORANGE;
/// Material Design Deep Orange palette.
pub const PALETTE_DEEP_ORANGE = c.LV_PALETTE_DEEP_ORANGE;
/// Material Design Brown palette.
pub const PALETTE_BROWN = c.LV_PALETTE_BROWN;
/// Material Design Blue Grey palette.
pub const PALETTE_BLUE_GREY = c.LV_PALETTE_BLUE_GREY;
/// Material Design Grey palette.
pub const PALETTE_GREY = c.LV_PALETTE_GREY;

// Size
/// Size constant meaning "fit content" (LV_SIZE_CONTENT).
pub const SIZE_CONTENT = c.LV_SIZE_CONTENT;
/// Convert a percentage (0–100) to an LVGL percent coordinate.
pub const PCT = c.lv_pct;

// Object flags
/// Flag that hides an object without destroying it.
pub const OBJ_FLAG_HIDDEN = c.LV_OBJ_FLAG_HIDDEN;
/// Flag that makes an object respond to pointer/touch input.
pub const OBJ_FLAG_CLICKABLE = c.LV_OBJ_FLAG_CLICKABLE;
/// Flag that enables scrolling within an object.
pub const OBJ_FLAG_SCROLLABLE = c.LV_OBJ_FLAG_SCROLLABLE;

// Opacity
/// Fully opaque (opacity = 255).
pub const OPA_COVER = c.LV_OPA_COVER;
/// Fully transparent (opacity = 0).
pub const OPA_TRANSP = c.LV_OPA_TRANSP;

// Flex
/// Flex layout: children arranged in a single row.
pub const FLEX_FLOW_ROW = c.LV_FLEX_FLOW_ROW;
/// Flex layout: children arranged in a single column.
pub const FLEX_FLOW_COLUMN = c.LV_FLEX_FLOW_COLUMN;
/// Flex layout: row with wrapping to new rows.
pub const FLEX_FLOW_ROW_WRAP = c.LV_FLEX_FLOW_ROW_WRAP;
/// Flex layout: column with wrapping to new columns.
pub const FLEX_FLOW_COLUMN_WRAP = c.LV_FLEX_FLOW_COLUMN_WRAP;
/// Flex alignment: children packed to the start.
pub const FLEX_ALIGN_START = c.LV_FLEX_ALIGN_START;
/// Flex alignment: children packed to the end.
pub const FLEX_ALIGN_END = c.LV_FLEX_ALIGN_END;
/// Flex alignment: children centered.
pub const FLEX_ALIGN_CENTER = c.LV_FLEX_ALIGN_CENTER;
/// Flex alignment: equal spacing between and around children.
pub const FLEX_ALIGN_SPACE_EVENLY = c.LV_FLEX_ALIGN_SPACE_EVENLY;
/// Flex alignment: equal spacing between children, none at edges.
pub const FLEX_ALIGN_SPACE_BETWEEN = c.LV_FLEX_ALIGN_SPACE_BETWEEN;
/// Flex alignment: half-sized spacing at edges, full spacing between.
pub const FLEX_ALIGN_SPACE_AROUND = c.LV_FLEX_ALIGN_SPACE_AROUND;

// Event codes
/// Event fired when an object is clicked (press + release).
pub const EVENT_CLICKED = c.LV_EVENT_CLICKED;
/// Event fired when a widget's value changes (e.g. slider, checkbox).
pub const EVENT_VALUE_CHANGED = c.LV_EVENT_VALUE_CHANGED;
/// Event fired when a pointer press begins over an object.
pub const EVENT_PRESSED = c.LV_EVENT_PRESSED;
/// Event fired when a pointer is released over an object.
pub const EVENT_RELEASED = c.LV_EVENT_RELEASED;

// Label long modes
/// Label long mode: wrap text to multiple lines.
pub const LABEL_LONG_WRAP = c.LV_LABEL_LONG_WRAP;
/// Label long mode: scroll text horizontally (back and forth).
pub const LABEL_LONG_SCROLL = c.LV_LABEL_LONG_SCROLL;
/// Label long mode: scroll text in a continuous circular loop.
pub const LABEL_LONG_SCROLL_CIRCULAR = c.LV_LABEL_LONG_SCROLL_CIRCULAR;
/// Label long mode: replace the end with dots (`…`) when text overflows.
pub const LABEL_LONG_DOT = c.LV_LABEL_LONG_DOT;
/// Label long mode: clip text at the widget boundary.
pub const LABEL_LONG_CLIP = c.LV_LABEL_LONG_CLIP;

// Parts
/// Style selector for the main (background) part of a widget.
pub const PART_MAIN = c.LV_PART_MAIN;
/// Style selector for the indicator part of a widget (e.g. bar fill).
pub const PART_INDICATOR = c.LV_PART_INDICATOR;
/// Style selector for the knob of interactive widgets (slider, arc).
pub const PART_KNOB = c.LV_PART_KNOB;

// Screen load animations
/// No animation — instant swap.
pub const SCR_LOAD_ANIM_NONE = c.LV_SCR_LOAD_ANIM_NONE;
/// New screen slides in from the right over the old one.
pub const SCR_LOAD_ANIM_OVER_LEFT = c.LV_SCR_LOAD_ANIM_OVER_LEFT;
/// New screen slides in from the left over the old one.
pub const SCR_LOAD_ANIM_OVER_RIGHT = c.LV_SCR_LOAD_ANIM_OVER_RIGHT;
/// New screen slides in from the bottom over the old one.
pub const SCR_LOAD_ANIM_OVER_TOP = c.LV_SCR_LOAD_ANIM_OVER_TOP;
/// New screen slides in from the top over the old one.
pub const SCR_LOAD_ANIM_OVER_BOTTOM = c.LV_SCR_LOAD_ANIM_OVER_BOTTOM;
/// Both screens slide leftward together.
pub const SCR_LOAD_ANIM_MOVE_LEFT = c.LV_SCR_LOAD_ANIM_MOVE_LEFT;
/// Both screens slide rightward together.
pub const SCR_LOAD_ANIM_MOVE_RIGHT = c.LV_SCR_LOAD_ANIM_MOVE_RIGHT;
/// Both screens slide upward together.
pub const SCR_LOAD_ANIM_MOVE_TOP = c.LV_SCR_LOAD_ANIM_MOVE_TOP;
/// Both screens slide downward together.
pub const SCR_LOAD_ANIM_MOVE_BOTTOM = c.LV_SCR_LOAD_ANIM_MOVE_BOTTOM;
/// Cross-fade transition.
pub const SCR_LOAD_ANIM_FADE_IN = c.LV_SCR_LOAD_ANIM_FADE_IN;
/// Fade the old screen out.
pub const SCR_LOAD_ANIM_FADE_OUT = c.LV_SCR_LOAD_ANIM_FADE_OUT;

// States
/// Widget state bit set when a toggle (button, switch, checkbox) is checked.
pub const STATE_CHECKED = c.LV_STATE_CHECKED;

// Grid
/// Grid descriptor sentinel terminating column/row arrays.
pub const GRID_TEMPLATE_LAST = c.LV_GRID_TEMPLATE_LAST;
/// Grid track size: fit content.
pub const GRID_CONTENT = c.LV_GRID_CONTENT;
/// Build a grid "fractional unit" track descriptor.
pub fn gridFr(n: i32) i32 {
    return c.LV_GRID_CONTENT + 1 + n;
}
/// Grid cell alignment: flush with the start edge.
pub const GRID_ALIGN_START = c.LV_GRID_ALIGN_START;
/// Grid cell alignment: centered within the cell.
pub const GRID_ALIGN_CENTER = c.LV_GRID_ALIGN_CENTER;
/// Grid cell alignment: flush with the end edge.
pub const GRID_ALIGN_END = c.LV_GRID_ALIGN_END;
/// Grid cell alignment: stretch to fill the cell.
pub const GRID_ALIGN_STRETCH = c.LV_GRID_ALIGN_STRETCH;

// =========================================================================
//  Fonts
// =========================================================================

/// Return a pointer to the Montserrat 14-pt font.
pub fn fontMontserrat14() *const c.lv_font_t {
    return &c.lv_font_montserrat_14;
}

/// Return a pointer to the Montserrat 16-pt font.
pub fn fontMontserrat16() *const c.lv_font_t {
    return &c.lv_font_montserrat_16;
}

/// Return a pointer to the Montserrat 20-pt font.
pub fn fontMontserrat20() *const c.lv_font_t {
    return &c.lv_font_montserrat_20;
}

/// Return a pointer to the Montserrat 24-pt font.
pub fn fontMontserrat24() *const c.lv_font_t {
    return &c.lv_font_montserrat_24;
}

/// Return a pointer to the Montserrat 32-pt font.
pub fn fontMontserrat32() *const c.lv_font_t {
    return &c.lv_font_montserrat_32;
}

/// Return a pointer to the Montserrat 48-pt font.
pub fn fontMontserrat48() *const c.lv_font_t {
    return &c.lv_font_montserrat_48;
}

// =========================================================================
//  Screen
// =========================================================================

/// Return the currently active (displayed) screen as an `Obj`.
pub fn screenActive() Obj {
    return .{ .obj = c.lv_screen_active().? };
}

// =========================================================================
//  Helper: extract lv_obj_t* from any widget type
// =========================================================================

fn parentObj(parent: anytype) *c.lv_obj_t {
    return parent.obj;
}

// =========================================================================
//  ObjectMixin — size, position, flags, user_data
// =========================================================================

/// Mixin providing size, position, visibility, flex, and user-data setters.
///
/// All methods use a fluent (builder) pattern: they return `Self` so calls
/// can be chained. `Self` must have an `obj: *lv_obj_t` field.
pub fn ObjectMixin(comptime Self: type) type {
    return struct {
        /// Set the object's width and height in pixels. Returns `self`.
        pub fn size(self: Self, w: i32, h: i32) Self {
            c.lv_obj_set_size(self.obj, w, h);
            return self;
        }

        /// Set the object's width in pixels. Returns `self`.
        pub fn width(self: Self, w: i32) Self {
            c.lv_obj_set_width(self.obj, w);
            return self;
        }

        /// Set the object's height in pixels. Returns `self`.
        pub fn height(self: Self, h: i32) Self {
            c.lv_obj_set_height(self.obj, h);
            return self;
        }

        /// Set the object's position relative to its parent. Returns `self`.
        pub fn pos(self: Self, x: i32, y: i32) Self {
            c.lv_obj_set_pos(self.obj, x, y);
            return self;
        }

        /// Center the object within its parent. Returns `self`.
        pub fn center(self: Self) Self {
            c.lv_obj_center(self.obj);
            return self;
        }

        /// Align the object to a corner/edge of its parent with pixel offsets.
        ///
        /// `a` is one of the `ALIGN_*` constants. Returns `self`.
        pub fn alignTo(self: Self, a: c.lv_align_t, x_ofs: i32, y_ofs: i32) Self {
            c.lv_obj_align(self.obj, a, x_ofs, y_ofs);
            return self;
        }

        /// Add the `LV_OBJ_FLAG_HIDDEN` flag to make the object invisible. Returns `self`.
        pub fn hide(self: Self) Self {
            c.lv_obj_add_flag(self.obj, c.LV_OBJ_FLAG_HIDDEN);
            return self;
        }

        /// Remove the `LV_OBJ_FLAG_HIDDEN` flag to make the object visible. Returns `self`.
        pub fn show(self: Self) Self {
            c.lv_obj_remove_flag(self.obj, c.LV_OBJ_FLAG_HIDDEN);
            return self;
        }

        /// Show or hide the object based on a boolean. Returns `self`.
        pub fn visible(self: Self, v: bool) Self {
            return if (v) self.show() else self.hide();
        }

        /// Add an LVGL object flag. Returns `self`.
        pub fn addFlag(self: Self, f: c.lv_obj_flag_t) Self {
            c.lv_obj_add_flag(self.obj, f);
            return self;
        }

        /// Remove an LVGL object flag. Returns `self`.
        pub fn removeFlag(self: Self, f: c.lv_obj_flag_t) Self {
            c.lv_obj_remove_flag(self.obj, f);
            return self;
        }

        /// Add an LVGL state bit (e.g. `LV_STATE_CHECKED`). Returns `self`.
        pub fn addState(self: Self, s: c.lv_state_t) Self {
            c.lv_obj_add_state(self.obj, s);
            return self;
        }

        /// Remove an LVGL state bit. Returns `self`.
        pub fn removeState(self: Self, s: c.lv_state_t) Self {
            c.lv_obj_remove_state(self.obj, s);
            return self;
        }

        /// Enable or disable pointer/touch clickability. Returns `self`.
        pub fn clickable(self: Self, on: bool) Self {
            if (on)
                c.lv_obj_add_flag(self.obj, c.LV_OBJ_FLAG_CLICKABLE)
            else
                c.lv_obj_remove_flag(self.obj, c.LV_OBJ_FLAG_CLICKABLE);
            return self;
        }

        /// Attach an arbitrary pointer as user data on the object. Returns `self`.
        pub fn userData(self: Self, data: ?*anyopaque) Self {
            c.lv_obj_set_user_data(self.obj, data);
            return self;
        }

        /// Set the flex layout flow direction (row, column, or wrapped). Returns `self`.
        pub fn flexFlow(self: Self, flow: c.lv_flex_flow_t) Self {
            c.lv_obj_set_flex_flow(self.obj, flow);
            return self;
        }

        /// Set flex alignment for the main axis, cross axis, and tracks. Returns `self`.
        pub fn flexAlign(self: Self, main: c.lv_flex_align_t, cross: c.lv_flex_align_t, track: c.lv_flex_align_t) Self {
            c.lv_obj_set_flex_align(self.obj, main, cross, track);
            return self;
        }

        /// Configure this object as a grid container.
        ///
        /// Both `cols` and `rows` must terminate with `GRID_TEMPLATE_LAST`.
        /// Track sizes can be fixed pixel values, `GRID_CONTENT`, or
        /// `gridFr(n)` for fractional units. Caller must keep the slices
        /// alive as long as the widget uses them — LVGL retains the pointer.
        /// Returns `self`.
        pub fn gridDsc(self: Self, cols: []const i32, rows: []const i32) Self {
            c.lv_obj_set_grid_dsc_array(self.obj, cols.ptr, rows.ptr);
            return self;
        }

        /// Place this object into a cell of its parent grid. Returns `self`.
        pub fn gridCell(
            self: Self,
            col_align: c.lv_grid_align_t,
            col_pos: i32,
            col_span: i32,
            row_align: c.lv_grid_align_t,
            row_pos: i32,
            row_span: i32,
        ) Self {
            c.lv_obj_set_grid_cell(self.obj, col_align, col_pos, col_span, row_align, row_pos, row_span);
            return self;
        }
    };
}

// =========================================================================
//  StyleMixin — inline style setters (LV_PART_MAIN)
// =========================================================================

/// Mixin providing inline style setters scoped to `LV_PART_MAIN`.
///
/// All methods use a fluent (builder) pattern: they return `Self` so calls
/// can be chained. `Self` must have an `obj: *lv_obj_t` field.
pub fn StyleMixin(comptime Self: type) type {
    return struct {
        /// Set the background color of the main part. Returns `self`.
        pub fn bgColor(self: Self, col: Color) Self {
            c.lv_obj_set_style_bg_color(self.obj, col, c.LV_PART_MAIN);
            return self;
        }

        /// Set the background opacity of the main part (0–255). Returns `self`.
        pub fn bgOpa(self: Self, opa: c.lv_opa_t) Self {
            c.lv_obj_set_style_bg_opa(self.obj, opa, c.LV_PART_MAIN);
            return self;
        }

        /// Set the border color of the main part. Returns `self`.
        pub fn borderColor(self: Self, col: Color) Self {
            c.lv_obj_set_style_border_color(self.obj, col, c.LV_PART_MAIN);
            return self;
        }

        /// Set the border width in pixels of the main part. Returns `self`.
        pub fn borderWidth(self: Self, w: i32) Self {
            c.lv_obj_set_style_border_width(self.obj, w, c.LV_PART_MAIN);
            return self;
        }

        /// Set the corner radius in pixels of the main part. Returns `self`.
        pub fn radius(self: Self, r: i32) Self {
            c.lv_obj_set_style_radius(self.obj, r, c.LV_PART_MAIN);
            return self;
        }

        /// Set equal padding on all four sides of the main part. Returns `self`.
        pub fn padAll(self: Self, p: i32) Self {
            c.lv_obj_set_style_pad_all(self.obj, p, c.LV_PART_MAIN);
            return self;
        }

        /// Set equal left and right (horizontal) padding. Returns `self`.
        pub fn padHor(self: Self, p: i32) Self {
            c.lv_obj_set_style_pad_hor(self.obj, p, c.LV_PART_MAIN);
            return self;
        }

        /// Set equal top and bottom (vertical) padding. Returns `self`.
        pub fn padVer(self: Self, p: i32) Self {
            c.lv_obj_set_style_pad_ver(self.obj, p, c.LV_PART_MAIN);
            return self;
        }

        /// Set the gap between children in a flex/grid layout. Returns `self`.
        pub fn padGap(self: Self, g: i32) Self {
            c.lv_obj_set_style_pad_gap(self.obj, g, c.LV_PART_MAIN);
            return self;
        }

        /// Set the text color of the main part. Returns `self`.
        pub fn textColor(self: Self, col: Color) Self {
            c.lv_obj_set_style_text_color(self.obj, col, c.LV_PART_MAIN);
            return self;
        }

        /// Set the font used for text rendering in the main part. Returns `self`.
        pub fn textFont(self: Self, f: *const c.lv_font_t) Self {
            c.lv_obj_set_style_text_font(self.obj, f, c.LV_PART_MAIN);
            return self;
        }
    };
}

// =========================================================================
//  EventMixin — event callbacks
// =========================================================================

/// Mixin providing event-callback registration helpers.
///
/// All methods use a fluent (builder) pattern: they return `Self` so calls
/// can be chained. `Self` must have an `obj: *lv_obj_t` field.
pub fn EventMixin(comptime Self: type) type {
    return struct {
        /// Register an event callback for any LVGL event code. Returns `self`.
        ///
        /// `user_data` is forwarded to `cb` via `lv_event_get_user_data()`.
        pub fn on(self: Self, code: c.lv_event_code_t, cb: c.lv_event_cb_t, user_data: ?*anyopaque) Self {
            c.lv_obj_add_event_cb(self.obj, cb, code, user_data);
            return self;
        }

        /// Register a callback fired on `LV_EVENT_CLICKED`. Returns `self`.
        pub fn onClick(self: Self, cb: c.lv_event_cb_t, user_data: ?*anyopaque) Self {
            return self.on(c.LV_EVENT_CLICKED, cb, user_data);
        }

        /// Register a callback fired on `LV_EVENT_VALUE_CHANGED`. Returns `self`.
        pub fn onValueChanged(self: Self, cb: c.lv_event_cb_t, user_data: ?*anyopaque) Self {
            return self.on(c.LV_EVENT_VALUE_CHANGED, cb, user_data);
        }
    };
}

// =========================================================================
//  Obj — generic lv_obj_t wrapper (non-owning, pointer-sized)
// =========================================================================

/// Generic wrapper around an `lv_obj_t` pointer.
///
/// Non-owning: the underlying LVGL object is managed by the LVGL tree.
/// Inherits all `ObjectMixin`, `StyleMixin`, and `EventMixin` methods
/// via `pub const` aliases.
pub const Obj = struct {
    obj: *c.lv_obj_t,

    // ObjectMixin — size, position, alignment, visibility, flags, flex, user data.
    /// Set width and height in pixels. Returns `self`.
    pub const size = ObjectMixin(Obj).size;
    /// Set width in pixels. Returns `self`.
    pub const width = ObjectMixin(Obj).width;
    /// Set height in pixels. Returns `self`.
    pub const height = ObjectMixin(Obj).height;
    /// Set position relative to parent. Returns `self`.
    pub const pos = ObjectMixin(Obj).pos;
    /// Center within parent. Returns `self`.
    pub const center = ObjectMixin(Obj).center;
    /// Align to a corner/edge with pixel offsets. Returns `self`.
    pub const alignTo = ObjectMixin(Obj).alignTo;
    /// Add `LV_OBJ_FLAG_HIDDEN`. Returns `self`.
    pub const hide = ObjectMixin(Obj).hide;
    /// Remove `LV_OBJ_FLAG_HIDDEN`. Returns `self`.
    pub const show = ObjectMixin(Obj).show;
    /// Show or hide based on a bool. Returns `self`.
    pub const visible = ObjectMixin(Obj).visible;
    /// Add an object flag. Returns `self`.
    pub const addFlag = ObjectMixin(Obj).addFlag;
    /// Remove an object flag. Returns `self`.
    pub const removeFlag = ObjectMixin(Obj).removeFlag;
    /// Add an object state bit. Returns `self`.
    pub const addState = ObjectMixin(Obj).addState;
    /// Remove an object state bit. Returns `self`.
    pub const removeState = ObjectMixin(Obj).removeState;
    /// Enable or disable pointer input. Returns `self`.
    pub const clickable = ObjectMixin(Obj).clickable;
    /// Attach an arbitrary pointer as user data. Returns `self`.
    pub const userData = ObjectMixin(Obj).userData;
    /// Set flex layout flow direction. Returns `self`.
    pub const flexFlow = ObjectMixin(Obj).flexFlow;
    /// Set flex alignment for main axis, cross axis, and tracks. Returns `self`.
    pub const flexAlign = ObjectMixin(Obj).flexAlign;

    // StyleMixin — inline style setters scoped to LV_PART_MAIN.
    /// Set background color. Returns `self`.
    pub const bgColor = StyleMixin(Obj).bgColor;
    /// Set background opacity. Returns `self`.
    pub const bgOpa = StyleMixin(Obj).bgOpa;
    /// Set border color. Returns `self`.
    pub const borderColor = StyleMixin(Obj).borderColor;
    /// Set border width in pixels. Returns `self`.
    pub const borderWidth = StyleMixin(Obj).borderWidth;
    /// Set corner radius in pixels. Returns `self`.
    pub const radius = StyleMixin(Obj).radius;
    /// Set equal padding on all sides. Returns `self`.
    pub const padAll = StyleMixin(Obj).padAll;
    /// Set left and right padding. Returns `self`.
    pub const padHor = StyleMixin(Obj).padHor;
    /// Set top and bottom padding. Returns `self`.
    pub const padVer = StyleMixin(Obj).padVer;
    /// Set gap between children in flex/grid. Returns `self`.
    pub const padGap = StyleMixin(Obj).padGap;
    /// Set text color. Returns `self`.
    pub const textColor = StyleMixin(Obj).textColor;
    /// Set font for text rendering. Returns `self`.
    pub const textFont = StyleMixin(Obj).textFont;

    // EventMixin — event callback registration.
    /// Register a callback for any LVGL event code. Returns `self`.
    pub const on = EventMixin(Obj).on;
    /// Register a callback for `LV_EVENT_CLICKED`. Returns `self`.
    pub const onClick = EventMixin(Obj).onClick;
    /// Register a callback for `LV_EVENT_VALUE_CHANGED`. Returns `self`.
    pub const onValueChanged = EventMixin(Obj).onValueChanged;

    /// Create a generic LVGL object as a child of `parent_obj`.
    ///
    /// `parent_obj` can be any widget type with an `obj` field (e.g. `Obj`, `Box`).
    pub fn create(parent_obj: anytype) Obj {
        return .{ .obj = c.lv_obj_create(parentObj(parent_obj)).? };
    }

    /// Delete this object and all its children from the LVGL tree.
    pub fn del(self: Obj) void {
        c.lv_obj_delete(self.obj);
    }

    /// Delete all children of this object without deleting the object itself.
    pub fn clean(self: Obj) void {
        c.lv_obj_clean(self.obj);
    }

    /// Return the parent of this object as an `Obj`.
    pub fn parent(self: Obj) Obj {
        return .{ .obj = c.lv_obj_get_parent(self.obj) };
    }

    /// Return the number of direct children this object has.
    pub fn childCount(self: Obj) u32 {
        return c.lv_obj_get_child_count(self.obj);
    }

    /// Return the current rendered width of the object in pixels.
    pub fn getWidth(self: Obj) i32 {
        return c.lv_obj_get_width(self.obj);
    }

    /// Return the current rendered height of the object in pixels.
    pub fn getHeight(self: Obj) i32 {
        return c.lv_obj_get_height(self.obj);
    }

    /// Wrap a raw `lv_obj_t` pointer in an `Obj`. Use when interfacing with C code.
    pub fn fromRaw(raw_obj: *c.lv_obj_t) Obj {
        return .{ .obj = raw_obj };
    }

    /// Return the underlying raw `lv_obj_t` pointer for C interop.
    pub fn raw(self: Obj) *c.lv_obj_t {
        return self.obj;
    }
};

// =========================================================================
//  Label
// =========================================================================

/// LVGL label widget wrapper with fluent builder API.
///
/// Inherits all `ObjectMixin`, `StyleMixin`, and `EventMixin` methods.
/// Use `create()` to instantiate and chain setters:
///
///     const lbl = lvgl.Label.create(screen)
///         .text("Hello")
///         .center();
pub const Label = struct {
    obj: *c.lv_obj_t,

    // ObjectMixin — size, position, alignment, visibility, flags, flex, user data.
    /// Set width and height in pixels. Returns `self`.
    pub const size = ObjectMixin(Label).size;
    /// Set width in pixels. Returns `self`.
    pub const width = ObjectMixin(Label).width;
    /// Set height in pixels. Returns `self`.
    pub const height = ObjectMixin(Label).height;
    /// Set position relative to parent. Returns `self`.
    pub const pos = ObjectMixin(Label).pos;
    /// Center within parent. Returns `self`.
    pub const center = ObjectMixin(Label).center;
    /// Align to a corner/edge with pixel offsets. Returns `self`.
    pub const alignTo = ObjectMixin(Label).alignTo;
    /// Add `LV_OBJ_FLAG_HIDDEN`. Returns `self`.
    pub const hide = ObjectMixin(Label).hide;
    /// Remove `LV_OBJ_FLAG_HIDDEN`. Returns `self`.
    pub const show = ObjectMixin(Label).show;
    /// Show or hide based on a bool. Returns `self`.
    pub const visible = ObjectMixin(Label).visible;
    /// Add an object flag. Returns `self`.
    pub const addFlag = ObjectMixin(Label).addFlag;
    /// Remove an object flag. Returns `self`.
    pub const removeFlag = ObjectMixin(Label).removeFlag;
    /// Add an object state bit. Returns `self`.
    pub const addState = ObjectMixin(Label).addState;
    /// Remove an object state bit. Returns `self`.
    pub const removeState = ObjectMixin(Label).removeState;
    /// Enable or disable pointer input. Returns `self`.
    pub const clickable = ObjectMixin(Label).clickable;
    /// Attach an arbitrary pointer as user data. Returns `self`.
    pub const userData = ObjectMixin(Label).userData;
    /// Set flex layout flow direction. Returns `self`.
    pub const flexFlow = ObjectMixin(Label).flexFlow;
    /// Set flex alignment for main axis, cross axis, and tracks. Returns `self`.
    pub const flexAlign = ObjectMixin(Label).flexAlign;

    // StyleMixin — inline style setters scoped to LV_PART_MAIN.
    /// Set background color. Returns `self`.
    pub const bgColor = StyleMixin(Label).bgColor;
    /// Set background opacity. Returns `self`.
    pub const bgOpa = StyleMixin(Label).bgOpa;
    /// Set border color. Returns `self`.
    pub const borderColor = StyleMixin(Label).borderColor;
    /// Set border width in pixels. Returns `self`.
    pub const borderWidth = StyleMixin(Label).borderWidth;
    /// Set corner radius in pixels. Returns `self`.
    pub const radius = StyleMixin(Label).radius;
    /// Set equal padding on all sides. Returns `self`.
    pub const padAll = StyleMixin(Label).padAll;
    /// Set left and right padding. Returns `self`.
    pub const padHor = StyleMixin(Label).padHor;
    /// Set top and bottom padding. Returns `self`.
    pub const padVer = StyleMixin(Label).padVer;
    /// Set gap between children in flex/grid. Returns `self`.
    pub const padGap = StyleMixin(Label).padGap;
    /// Set text color. Returns `self`.
    pub const textColor = StyleMixin(Label).textColor;
    /// Set font for text rendering. Returns `self`.
    pub const textFont = StyleMixin(Label).textFont;

    // EventMixin — event callback registration.
    /// Register a callback for any LVGL event code. Returns `self`.
    pub const on = EventMixin(Label).on;
    /// Register a callback for `LV_EVENT_CLICKED`. Returns `self`.
    pub const onClick = EventMixin(Label).onClick;
    /// Register a callback for `LV_EVENT_VALUE_CHANGED`. Returns `self`.
    pub const onValueChanged = EventMixin(Label).onValueChanged;

    /// Create a label widget as a child of `parent_`. Returns the new `Label`.
    pub fn create(parent_: anytype) Label {
        return .{ .obj = c.lv_label_create(parentObj(parent_)).? };
    }

    /// Set the label text (copies the string). Returns `self`.
    pub fn text(self: Label, txt: [*:0]const u8) Label {
        c.lv_label_set_text(self.obj, txt);
        return self;
    }

    /// Set the label text to a static string (no copy, caller owns lifetime). Returns `self`.
    pub fn textStatic(self: Label, txt: [*:0]const u8) Label {
        c.lv_label_set_text_static(self.obj, txt);
        return self;
    }

    /// Set the font used for this label's text. Returns `self`.
    pub fn font(self: Label, f: *const c.lv_font_t) Label {
        c.lv_obj_set_style_text_font(self.obj, f, c.LV_PART_MAIN);
        return self;
    }

    /// Set the text color of this label. Returns `self`.
    pub fn color(self: Label, col: Color) Label {
        c.lv_obj_set_style_text_color(self.obj, col, c.LV_PART_MAIN);
        return self;
    }

    /// Set the overflow behaviour when text exceeds the label's bounds. Returns `self`.
    ///
    /// Use one of the `LABEL_LONG_*` constants.
    pub fn longMode(self: Label, mode: c.lv_label_long_mode_t) Label {
        c.lv_label_set_long_mode(self.obj, mode);
        return self;
    }
};

// =========================================================================
//  Bar
// =========================================================================

/// LVGL bar (progress bar) widget wrapper with fluent builder API.
///
/// Inherits all `ObjectMixin`, `StyleMixin`, and `EventMixin` methods.
/// Use `create()` then chain `range()` and `value()`:
///
///     const bar = lvgl.Bar.create(screen)
///         .size(200, 20)
///         .range(0, 100)
///         .value(42);
pub const Bar = struct {
    obj: *c.lv_obj_t,

    // ObjectMixin — size, position, alignment, visibility, flags, flex, user data.
    /// Set width and height in pixels. Returns `self`.
    pub const size = ObjectMixin(Bar).size;
    /// Set width in pixels. Returns `self`.
    pub const width = ObjectMixin(Bar).width;
    /// Set height in pixels. Returns `self`.
    pub const height = ObjectMixin(Bar).height;
    /// Set position relative to parent. Returns `self`.
    pub const pos = ObjectMixin(Bar).pos;
    /// Center within parent. Returns `self`.
    pub const center = ObjectMixin(Bar).center;
    /// Align to a corner/edge with pixel offsets. Returns `self`.
    pub const alignTo = ObjectMixin(Bar).alignTo;
    /// Add `LV_OBJ_FLAG_HIDDEN`. Returns `self`.
    pub const hide = ObjectMixin(Bar).hide;
    /// Remove `LV_OBJ_FLAG_HIDDEN`. Returns `self`.
    pub const show = ObjectMixin(Bar).show;
    /// Show or hide based on a bool. Returns `self`.
    pub const visible = ObjectMixin(Bar).visible;
    /// Add an object flag. Returns `self`.
    pub const addFlag = ObjectMixin(Bar).addFlag;
    /// Remove an object flag. Returns `self`.
    pub const removeFlag = ObjectMixin(Bar).removeFlag;
    /// Add an object state bit. Returns `self`.
    pub const addState = ObjectMixin(Bar).addState;
    /// Remove an object state bit. Returns `self`.
    pub const removeState = ObjectMixin(Bar).removeState;
    /// Enable or disable pointer input. Returns `self`.
    pub const clickable = ObjectMixin(Bar).clickable;
    /// Attach an arbitrary pointer as user data. Returns `self`.
    pub const userData = ObjectMixin(Bar).userData;
    /// Set flex layout flow direction. Returns `self`.
    pub const flexFlow = ObjectMixin(Bar).flexFlow;
    /// Set flex alignment for main axis, cross axis, and tracks. Returns `self`.
    pub const flexAlign = ObjectMixin(Bar).flexAlign;

    // StyleMixin — inline style setters scoped to LV_PART_MAIN.
    /// Set background color. Returns `self`.
    pub const bgColor = StyleMixin(Bar).bgColor;
    /// Set background opacity. Returns `self`.
    pub const bgOpa = StyleMixin(Bar).bgOpa;
    /// Set border color. Returns `self`.
    pub const borderColor = StyleMixin(Bar).borderColor;
    /// Set border width in pixels. Returns `self`.
    pub const borderWidth = StyleMixin(Bar).borderWidth;
    /// Set corner radius in pixels. Returns `self`.
    pub const radius = StyleMixin(Bar).radius;
    /// Set equal padding on all sides. Returns `self`.
    pub const padAll = StyleMixin(Bar).padAll;
    /// Set left and right padding. Returns `self`.
    pub const padHor = StyleMixin(Bar).padHor;
    /// Set top and bottom padding. Returns `self`.
    pub const padVer = StyleMixin(Bar).padVer;
    /// Set gap between children in flex/grid. Returns `self`.
    pub const padGap = StyleMixin(Bar).padGap;
    /// Set text color. Returns `self`.
    pub const textColor = StyleMixin(Bar).textColor;
    /// Set font for text rendering. Returns `self`.
    pub const textFont = StyleMixin(Bar).textFont;

    // EventMixin — event callback registration.
    /// Register a callback for any LVGL event code. Returns `self`.
    pub const on = EventMixin(Bar).on;
    /// Register a callback for `LV_EVENT_CLICKED`. Returns `self`.
    pub const onClick = EventMixin(Bar).onClick;
    /// Register a callback for `LV_EVENT_VALUE_CHANGED`. Returns `self`.
    pub const onValueChanged = EventMixin(Bar).onValueChanged;

    /// Create a bar widget as a child of `parent_`. Returns the new `Bar`.
    pub fn create(parent_: anytype) Bar {
        return .{ .obj = c.lv_bar_create(parentObj(parent_)).? };
    }

    /// Set the bar's current value with animation enabled. Returns `self`.
    pub fn value(self: Bar, val: i32) Bar {
        c.lv_bar_set_value(self.obj, val, animOn());
        return self;
    }

    /// Set the bar's current value with explicit animation control. Returns `self`.
    pub fn valueAnim(self: Bar, val: i32, anim: c.lv_anim_enable_t) Bar {
        c.lv_bar_set_value(self.obj, val, anim);
        return self;
    }

    /// Set the minimum and maximum value of the bar's range. Returns `self`.
    pub fn range(self: Bar, min: i32, max: i32) Bar {
        c.lv_bar_set_range(self.obj, min, max);
        return self;
    }

    /// Set the fill (indicator) color of the bar. Returns `self`.
    pub fn indicatorColor(self: Bar, col: Color) Bar {
        c.lv_obj_set_style_bg_color(self.obj, col, c.LV_PART_INDICATOR);
        return self;
    }

    /// Set the background (track) color of the bar. Returns `self`.
    pub fn barColor(self: Bar, col: Color) Bar {
        c.lv_obj_set_style_bg_color(self.obj, col, c.LV_PART_MAIN);
        return self;
    }
};

// =========================================================================
//  Box — plain object with scrolling removed
// =========================================================================

/// Plain container object with scrolling disabled.
///
/// Useful as a layout container for flex rows/columns. Inherits all
/// `ObjectMixin`, `StyleMixin`, and `EventMixin` methods.
pub const Box = struct {
    obj: *c.lv_obj_t,

    // ObjectMixin — size, position, alignment, visibility, flags, flex, user data.
    /// Set width and height in pixels. Returns `self`.
    pub const size = ObjectMixin(Box).size;
    /// Set width in pixels. Returns `self`.
    pub const width = ObjectMixin(Box).width;
    /// Set height in pixels. Returns `self`.
    pub const height = ObjectMixin(Box).height;
    /// Set position relative to parent. Returns `self`.
    pub const pos = ObjectMixin(Box).pos;
    /// Center within parent. Returns `self`.
    pub const center = ObjectMixin(Box).center;
    /// Align to a corner/edge with pixel offsets. Returns `self`.
    pub const alignTo = ObjectMixin(Box).alignTo;
    /// Add `LV_OBJ_FLAG_HIDDEN`. Returns `self`.
    pub const hide = ObjectMixin(Box).hide;
    /// Remove `LV_OBJ_FLAG_HIDDEN`. Returns `self`.
    pub const show = ObjectMixin(Box).show;
    /// Show or hide based on a bool. Returns `self`.
    pub const visible = ObjectMixin(Box).visible;
    /// Add an object flag. Returns `self`.
    pub const addFlag = ObjectMixin(Box).addFlag;
    /// Remove an object flag. Returns `self`.
    pub const removeFlag = ObjectMixin(Box).removeFlag;
    /// Add an object state bit. Returns `self`.
    pub const addState = ObjectMixin(Box).addState;
    /// Remove an object state bit. Returns `self`.
    pub const removeState = ObjectMixin(Box).removeState;
    /// Enable or disable pointer input. Returns `self`.
    pub const clickable = ObjectMixin(Box).clickable;
    /// Attach an arbitrary pointer as user data. Returns `self`.
    pub const userData = ObjectMixin(Box).userData;
    /// Set flex layout flow direction. Returns `self`.
    pub const flexFlow = ObjectMixin(Box).flexFlow;
    /// Set flex alignment for main axis, cross axis, and tracks. Returns `self`.
    pub const flexAlign = ObjectMixin(Box).flexAlign;

    // StyleMixin — inline style setters scoped to LV_PART_MAIN.
    /// Set background color. Returns `self`.
    pub const bgColor = StyleMixin(Box).bgColor;
    /// Set background opacity. Returns `self`.
    pub const bgOpa = StyleMixin(Box).bgOpa;
    /// Set border color. Returns `self`.
    pub const borderColor = StyleMixin(Box).borderColor;
    /// Set border width in pixels. Returns `self`.
    pub const borderWidth = StyleMixin(Box).borderWidth;
    /// Set corner radius in pixels. Returns `self`.
    pub const radius = StyleMixin(Box).radius;
    /// Set equal padding on all sides. Returns `self`.
    pub const padAll = StyleMixin(Box).padAll;
    /// Set left and right padding. Returns `self`.
    pub const padHor = StyleMixin(Box).padHor;
    /// Set top and bottom padding. Returns `self`.
    pub const padVer = StyleMixin(Box).padVer;
    /// Set gap between children in flex/grid. Returns `self`.
    pub const padGap = StyleMixin(Box).padGap;
    /// Set text color. Returns `self`.
    pub const textColor = StyleMixin(Box).textColor;
    /// Set font for text rendering. Returns `self`.
    pub const textFont = StyleMixin(Box).textFont;

    // EventMixin — event callback registration.
    /// Register a callback for any LVGL event code. Returns `self`.
    pub const on = EventMixin(Box).on;
    /// Register a callback for `LV_EVENT_CLICKED`. Returns `self`.
    pub const onClick = EventMixin(Box).onClick;
    /// Register a callback for `LV_EVENT_VALUE_CHANGED`. Returns `self`.
    pub const onValueChanged = EventMixin(Box).onValueChanged;

    /// Create a non-scrollable container object as a child of `parent_`. Returns the new `Box`.
    pub fn create(parent_: anytype) Box {
        const o = c.lv_obj_create(parentObj(parent_)).?;
        c.lv_obj_remove_flag(o, c.LV_OBJ_FLAG_SCROLLABLE);
        return .{ .obj = o };
    }

    // Grid container / child helpers
    /// Configure this box as a grid container with `cols`/`rows` descriptors.
    pub const gridDsc = ObjectMixin(Box).gridDsc;
    /// Place this box into a cell of its parent grid.
    pub const gridCell = ObjectMixin(Box).gridCell;
};

// =========================================================================
//  Button — clickable container
// =========================================================================

/// LVGL button widget — a clickable container. Add a `Label` child to give
/// the button text. Use `toggleMode(true)` to turn it into a checkable
/// toggle whose state is tracked via `STATE_CHECKED`.
pub const Button = struct {
    obj: *c.lv_obj_t,

    // ObjectMixin aliases
    pub const size = ObjectMixin(Button).size;
    pub const width = ObjectMixin(Button).width;
    pub const height = ObjectMixin(Button).height;
    pub const pos = ObjectMixin(Button).pos;
    pub const center = ObjectMixin(Button).center;
    pub const alignTo = ObjectMixin(Button).alignTo;
    pub const hide = ObjectMixin(Button).hide;
    pub const show = ObjectMixin(Button).show;
    pub const visible = ObjectMixin(Button).visible;
    pub const addFlag = ObjectMixin(Button).addFlag;
    pub const removeFlag = ObjectMixin(Button).removeFlag;
    pub const addState = ObjectMixin(Button).addState;
    pub const removeState = ObjectMixin(Button).removeState;
    pub const clickable = ObjectMixin(Button).clickable;
    pub const userData = ObjectMixin(Button).userData;
    pub const flexFlow = ObjectMixin(Button).flexFlow;
    pub const flexAlign = ObjectMixin(Button).flexAlign;
    pub const gridCell = ObjectMixin(Button).gridCell;

    // StyleMixin aliases
    pub const bgColor = StyleMixin(Button).bgColor;
    pub const bgOpa = StyleMixin(Button).bgOpa;
    pub const borderColor = StyleMixin(Button).borderColor;
    pub const borderWidth = StyleMixin(Button).borderWidth;
    pub const radius = StyleMixin(Button).radius;
    pub const padAll = StyleMixin(Button).padAll;
    pub const padHor = StyleMixin(Button).padHor;
    pub const padVer = StyleMixin(Button).padVer;
    pub const padGap = StyleMixin(Button).padGap;
    pub const textColor = StyleMixin(Button).textColor;
    pub const textFont = StyleMixin(Button).textFont;

    // EventMixin aliases
    pub const on = EventMixin(Button).on;
    pub const onClick = EventMixin(Button).onClick;
    pub const onValueChanged = EventMixin(Button).onValueChanged;

    /// Create a button widget as a child of `parent_`.
    pub fn create(parent_: anytype) Button {
        return .{ .obj = c.lv_button_create(parentObj(parent_)).? };
    }

    /// Enable or disable toggle (checkable) behaviour. Returns `self`.
    pub fn toggleMode(self: Button, on_: bool) Button {
        if (on_)
            c.lv_obj_add_flag(self.obj, c.LV_OBJ_FLAG_CHECKABLE)
        else
            c.lv_obj_remove_flag(self.obj, c.LV_OBJ_FLAG_CHECKABLE);
        return self;
    }

    /// Set the checked state explicitly. Returns `self`.
    pub fn checked(self: Button, v: bool) Button {
        if (v)
            c.lv_obj_add_state(self.obj, c.LV_STATE_CHECKED)
        else
            c.lv_obj_remove_state(self.obj, c.LV_STATE_CHECKED);
        return self;
    }

    /// Returns `true` if the button is currently in the checked state.
    pub fn isChecked(self: Button) bool {
        return c.lv_obj_has_state(self.obj, c.LV_STATE_CHECKED);
    }
};

// =========================================================================
//  Slider — interactive bar
// =========================================================================

/// LVGL slider widget. Shares the bar value/range shape and adds knob
/// styling and drag interaction. Listen for changes with `onValueChanged`.
pub const Slider = struct {
    obj: *c.lv_obj_t,

    // ObjectMixin aliases
    pub const size = ObjectMixin(Slider).size;
    pub const width = ObjectMixin(Slider).width;
    pub const height = ObjectMixin(Slider).height;
    pub const pos = ObjectMixin(Slider).pos;
    pub const center = ObjectMixin(Slider).center;
    pub const alignTo = ObjectMixin(Slider).alignTo;
    pub const hide = ObjectMixin(Slider).hide;
    pub const show = ObjectMixin(Slider).show;
    pub const visible = ObjectMixin(Slider).visible;
    pub const addFlag = ObjectMixin(Slider).addFlag;
    pub const removeFlag = ObjectMixin(Slider).removeFlag;
    pub const addState = ObjectMixin(Slider).addState;
    pub const removeState = ObjectMixin(Slider).removeState;
    pub const clickable = ObjectMixin(Slider).clickable;
    pub const userData = ObjectMixin(Slider).userData;
    pub const gridCell = ObjectMixin(Slider).gridCell;

    // StyleMixin aliases
    pub const bgColor = StyleMixin(Slider).bgColor;
    pub const bgOpa = StyleMixin(Slider).bgOpa;
    pub const borderColor = StyleMixin(Slider).borderColor;
    pub const borderWidth = StyleMixin(Slider).borderWidth;
    pub const radius = StyleMixin(Slider).radius;
    pub const padAll = StyleMixin(Slider).padAll;
    pub const padHor = StyleMixin(Slider).padHor;
    pub const padVer = StyleMixin(Slider).padVer;

    // EventMixin aliases
    pub const on = EventMixin(Slider).on;
    pub const onClick = EventMixin(Slider).onClick;
    pub const onValueChanged = EventMixin(Slider).onValueChanged;

    /// Create a slider widget as a child of `parent_`.
    pub fn create(parent_: anytype) Slider {
        return .{ .obj = c.lv_slider_create(parentObj(parent_)).? };
    }

    /// Set the slider's current value with animation enabled. Returns `self`.
    pub fn value(self: Slider, val: i32) Slider {
        c.lv_slider_set_value(self.obj, val, animOn());
        return self;
    }

    /// Set the slider's current value with explicit animation control. Returns `self`.
    pub fn valueAnim(self: Slider, val: i32, anim: c.lv_anim_enable_t) Slider {
        c.lv_slider_set_value(self.obj, val, anim);
        return self;
    }

    /// Set the min/max range. Returns `self`.
    pub fn range(self: Slider, min: i32, max: i32) Slider {
        c.lv_slider_set_range(self.obj, min, max);
        return self;
    }

    /// Returns the slider's current value.
    pub fn getValue(self: Slider) i32 {
        return c.lv_slider_get_value(self.obj);
    }

    /// Set the filled indicator colour. Returns `self`.
    pub fn indicatorColor(self: Slider, col: Color) Slider {
        c.lv_obj_set_style_bg_color(self.obj, col, c.LV_PART_INDICATOR);
        return self;
    }

    /// Set the knob colour. Returns `self`.
    pub fn knobColor(self: Slider, col: Color) Slider {
        c.lv_obj_set_style_bg_color(self.obj, col, c.LV_PART_KNOB);
        return self;
    }
};

// =========================================================================
//  Switch — binary toggle
// =========================================================================

/// LVGL switch widget. State is tracked via `STATE_CHECKED`; listen for
/// changes with `onValueChanged`.
pub const Switch = struct {
    obj: *c.lv_obj_t,

    // ObjectMixin aliases
    pub const size = ObjectMixin(Switch).size;
    pub const width = ObjectMixin(Switch).width;
    pub const height = ObjectMixin(Switch).height;
    pub const pos = ObjectMixin(Switch).pos;
    pub const center = ObjectMixin(Switch).center;
    pub const alignTo = ObjectMixin(Switch).alignTo;
    pub const hide = ObjectMixin(Switch).hide;
    pub const show = ObjectMixin(Switch).show;
    pub const visible = ObjectMixin(Switch).visible;
    pub const addFlag = ObjectMixin(Switch).addFlag;
    pub const removeFlag = ObjectMixin(Switch).removeFlag;
    pub const addState = ObjectMixin(Switch).addState;
    pub const removeState = ObjectMixin(Switch).removeState;
    pub const clickable = ObjectMixin(Switch).clickable;
    pub const userData = ObjectMixin(Switch).userData;
    pub const gridCell = ObjectMixin(Switch).gridCell;

    // StyleMixin aliases
    pub const bgColor = StyleMixin(Switch).bgColor;
    pub const bgOpa = StyleMixin(Switch).bgOpa;
    pub const borderColor = StyleMixin(Switch).borderColor;
    pub const borderWidth = StyleMixin(Switch).borderWidth;
    pub const radius = StyleMixin(Switch).radius;

    // EventMixin aliases
    pub const on = EventMixin(Switch).on;
    pub const onClick = EventMixin(Switch).onClick;
    pub const onValueChanged = EventMixin(Switch).onValueChanged;

    /// Create a switch widget as a child of `parent_`.
    pub fn create(parent_: anytype) Switch {
        return .{ .obj = c.lv_switch_create(parentObj(parent_)).? };
    }

    /// Set the on/off state. Returns `self`.
    pub fn checked(self: Switch, v: bool) Switch {
        if (v)
            c.lv_obj_add_state(self.obj, c.LV_STATE_CHECKED)
        else
            c.lv_obj_remove_state(self.obj, c.LV_STATE_CHECKED);
        return self;
    }

    /// Returns `true` if the switch is on.
    pub fn isChecked(self: Switch) bool {
        return c.lv_obj_has_state(self.obj, c.LV_STATE_CHECKED);
    }
};

// =========================================================================
//  Checkbox — labelled binary toggle
// =========================================================================

/// LVGL checkbox widget — a labelled binary toggle.
pub const Checkbox = struct {
    obj: *c.lv_obj_t,

    // ObjectMixin aliases
    pub const size = ObjectMixin(Checkbox).size;
    pub const width = ObjectMixin(Checkbox).width;
    pub const height = ObjectMixin(Checkbox).height;
    pub const pos = ObjectMixin(Checkbox).pos;
    pub const center = ObjectMixin(Checkbox).center;
    pub const alignTo = ObjectMixin(Checkbox).alignTo;
    pub const hide = ObjectMixin(Checkbox).hide;
    pub const show = ObjectMixin(Checkbox).show;
    pub const visible = ObjectMixin(Checkbox).visible;
    pub const addFlag = ObjectMixin(Checkbox).addFlag;
    pub const removeFlag = ObjectMixin(Checkbox).removeFlag;
    pub const addState = ObjectMixin(Checkbox).addState;
    pub const removeState = ObjectMixin(Checkbox).removeState;
    pub const clickable = ObjectMixin(Checkbox).clickable;
    pub const userData = ObjectMixin(Checkbox).userData;
    pub const gridCell = ObjectMixin(Checkbox).gridCell;

    // StyleMixin aliases
    pub const bgColor = StyleMixin(Checkbox).bgColor;
    pub const textColor = StyleMixin(Checkbox).textColor;
    pub const textFont = StyleMixin(Checkbox).textFont;

    // EventMixin aliases
    pub const on = EventMixin(Checkbox).on;
    pub const onClick = EventMixin(Checkbox).onClick;
    pub const onValueChanged = EventMixin(Checkbox).onValueChanged;

    /// Create a checkbox widget as a child of `parent_`.
    pub fn create(parent_: anytype) Checkbox {
        return .{ .obj = c.lv_checkbox_create(parentObj(parent_)).? };
    }

    /// Set the label text (LVGL copies into its own buffer). Returns `self`.
    pub fn text(self: Checkbox, txt: [*:0]const u8) Checkbox {
        c.lv_checkbox_set_text(self.obj, txt);
        return self;
    }

    /// Set the label from a persistent static string (no copy). Returns `self`.
    pub fn textStatic(self: Checkbox, txt: [*:0]const u8) Checkbox {
        c.lv_checkbox_set_text_static(self.obj, txt);
        return self;
    }

    /// Set the checked state. Returns `self`.
    pub fn checked(self: Checkbox, v: bool) Checkbox {
        if (v)
            c.lv_obj_add_state(self.obj, c.LV_STATE_CHECKED)
        else
            c.lv_obj_remove_state(self.obj, c.LV_STATE_CHECKED);
        return self;
    }

    /// Returns `true` if the checkbox is ticked.
    pub fn isChecked(self: Checkbox) bool {
        return c.lv_obj_has_state(self.obj, c.LV_STATE_CHECKED);
    }
};

// =========================================================================
//  Arc — circular slider / gauge
// =========================================================================

/// LVGL arc widget — a circular gauge or slider. Track uses `arc_color`
/// on `LV_PART_MAIN`, the filled indicator uses `arc_color` on
/// `LV_PART_INDICATOR`, and the draggable knob uses `bg_color` on
/// `LV_PART_KNOB`.
pub const Arc = struct {
    obj: *c.lv_obj_t,

    // ObjectMixin aliases
    pub const size = ObjectMixin(Arc).size;
    pub const width = ObjectMixin(Arc).width;
    pub const height = ObjectMixin(Arc).height;
    pub const pos = ObjectMixin(Arc).pos;
    pub const center = ObjectMixin(Arc).center;
    pub const alignTo = ObjectMixin(Arc).alignTo;
    pub const hide = ObjectMixin(Arc).hide;
    pub const show = ObjectMixin(Arc).show;
    pub const visible = ObjectMixin(Arc).visible;
    pub const addFlag = ObjectMixin(Arc).addFlag;
    pub const removeFlag = ObjectMixin(Arc).removeFlag;
    pub const addState = ObjectMixin(Arc).addState;
    pub const removeState = ObjectMixin(Arc).removeState;
    pub const clickable = ObjectMixin(Arc).clickable;
    pub const userData = ObjectMixin(Arc).userData;
    pub const gridCell = ObjectMixin(Arc).gridCell;

    // StyleMixin aliases
    pub const bgColor = StyleMixin(Arc).bgColor;
    pub const bgOpa = StyleMixin(Arc).bgOpa;
    pub const padAll = StyleMixin(Arc).padAll;

    // EventMixin aliases
    pub const on = EventMixin(Arc).on;
    pub const onClick = EventMixin(Arc).onClick;
    pub const onValueChanged = EventMixin(Arc).onValueChanged;

    /// Create an arc widget as a child of `parent_`.
    pub fn create(parent_: anytype) Arc {
        return .{ .obj = c.lv_arc_create(parentObj(parent_)).? };
    }

    /// Set the current value. Returns `self`.
    pub fn value(self: Arc, val: i32) Arc {
        c.lv_arc_set_value(self.obj, val);
        return self;
    }

    /// Set the min/max range. Returns `self`.
    pub fn range(self: Arc, min: i32, max: i32) Arc {
        c.lv_arc_set_range(self.obj, min, max);
        return self;
    }

    /// Set the background arc start/end angles in degrees. Returns `self`.
    pub fn bgAngles(self: Arc, start: u32, end: u32) Arc {
        c.lv_arc_set_bg_angles(self.obj, @intCast(start), @intCast(end));
        return self;
    }

    /// Set the indicator arc start/end angles in degrees. Returns `self`.
    pub fn angles(self: Arc, start: u32, end: u32) Arc {
        c.lv_arc_set_angles(self.obj, @intCast(start), @intCast(end));
        return self;
    }

    /// Set the rotation offset of the arc in degrees. Returns `self`.
    pub fn rotation(self: Arc, rot: u32) Arc {
        c.lv_arc_set_rotation(self.obj, @intCast(rot));
        return self;
    }

    /// Returns the current value.
    pub fn getValue(self: Arc) i32 {
        return c.lv_arc_get_value(self.obj);
    }

    /// Set the track arc colour (`LV_PART_MAIN`). Returns `self`.
    pub fn trackColor(self: Arc, col: Color) Arc {
        c.lv_obj_set_style_arc_color(self.obj, col, c.LV_PART_MAIN);
        return self;
    }

    /// Set the filled indicator arc colour (`LV_PART_INDICATOR`). Returns `self`.
    pub fn indicatorColor(self: Arc, col: Color) Arc {
        c.lv_obj_set_style_arc_color(self.obj, col, c.LV_PART_INDICATOR);
        return self;
    }

    /// Set the indicator arc width in pixels. Returns `self`.
    pub fn indicatorWidth(self: Arc, w: i32) Arc {
        c.lv_obj_set_style_arc_width(self.obj, w, c.LV_PART_INDICATOR);
        return self;
    }

    /// Set the knob colour (`LV_PART_KNOB`). Returns `self`.
    pub fn knobColor(self: Arc, col: Color) Arc {
        c.lv_obj_set_style_bg_color(self.obj, col, c.LV_PART_KNOB);
        return self;
    }
};

// =========================================================================
//  Image
// =========================================================================

/// LVGL image widget. Sources can be compiled-in descriptors (`const
/// lv_image_dsc_t *`), symbol strings, or filesystem paths when an FS
/// driver is registered.
pub const Image = struct {
    obj: *c.lv_obj_t,

    // ObjectMixin aliases
    pub const size = ObjectMixin(Image).size;
    pub const width = ObjectMixin(Image).width;
    pub const height = ObjectMixin(Image).height;
    pub const pos = ObjectMixin(Image).pos;
    pub const center = ObjectMixin(Image).center;
    pub const alignTo = ObjectMixin(Image).alignTo;
    pub const hide = ObjectMixin(Image).hide;
    pub const show = ObjectMixin(Image).show;
    pub const visible = ObjectMixin(Image).visible;
    pub const addFlag = ObjectMixin(Image).addFlag;
    pub const removeFlag = ObjectMixin(Image).removeFlag;
    pub const clickable = ObjectMixin(Image).clickable;
    pub const userData = ObjectMixin(Image).userData;
    pub const gridCell = ObjectMixin(Image).gridCell;

    // StyleMixin aliases
    pub const bgColor = StyleMixin(Image).bgColor;
    pub const bgOpa = StyleMixin(Image).bgOpa;
    pub const radius = StyleMixin(Image).radius;

    // EventMixin aliases
    pub const on = EventMixin(Image).on;
    pub const onClick = EventMixin(Image).onClick;

    /// Create an image widget as a child of `parent_`.
    pub fn create(parent_: anytype) Image {
        return .{ .obj = c.lv_image_create(parentObj(parent_)).? };
    }

    /// Set the image source (descriptor, symbol, or path). Returns `self`.
    pub fn src(self: Image, src_: ?*const anyopaque) Image {
        c.lv_image_set_src(self.obj, src_);
        return self;
    }

    /// Set the rotation angle in 0.1-degree units (0–3600). Returns `self`.
    pub fn rotation(self: Image, angle: i32) Image {
        c.lv_image_set_rotation(self.obj, angle);
        return self;
    }

    /// Set the zoom factor (256 = 1.0x, 512 = 2.0x). Returns `self`.
    pub fn scale(self: Image, zoom: u32) Image {
        c.lv_image_set_scale(self.obj, zoom);
        return self;
    }

    /// Set the rotation/scale pivot point. Returns `self`.
    pub fn pivot(self: Image, x: i32, y: i32) Image {
        c.lv_image_set_pivot(self.obj, x, y);
        return self;
    }
};

// =========================================================================
//  Msgbox — modal dialog
// =========================================================================

/// LVGL message box widget — a modal dialog. Passing a null parent
/// centers the msgbox on the active screen.
pub const Msgbox = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Msgbox).size;
    pub const width = ObjectMixin(Msgbox).width;
    pub const height = ObjectMixin(Msgbox).height;
    pub const pos = ObjectMixin(Msgbox).pos;
    pub const center = ObjectMixin(Msgbox).center;
    pub const alignTo = ObjectMixin(Msgbox).alignTo;
    pub const hide = ObjectMixin(Msgbox).hide;
    pub const show = ObjectMixin(Msgbox).show;
    pub const userData = ObjectMixin(Msgbox).userData;

    pub const bgColor = StyleMixin(Msgbox).bgColor;
    pub const radius = StyleMixin(Msgbox).radius;

    pub const on = EventMixin(Msgbox).on;

    /// Create a new msgbox. Pass an `Obj` with a null parent for a
    /// screen-centered modal.
    pub fn create(parent_: anytype) Msgbox {
        return .{ .obj = c.lv_msgbox_create(parentObj(parent_)).? };
    }

    /// Create a top-level msgbox centered on the active screen.
    pub fn createModal() Msgbox {
        return .{ .obj = c.lv_msgbox_create(null).? };
    }

    pub fn addTitle(self: Msgbox, txt: [*:0]const u8) Msgbox {
        _ = c.lv_msgbox_add_title(self.obj, txt);
        return self;
    }

    pub fn addText(self: Msgbox, txt: [*:0]const u8) Msgbox {
        _ = c.lv_msgbox_add_text(self.obj, txt);
        return self;
    }

    pub fn addCloseButton(self: Msgbox) Msgbox {
        _ = c.lv_msgbox_add_close_button(self.obj);
        return self;
    }

    /// Add a footer button and return it as an `Obj` for event handlers.
    pub fn addFooterButton(self: Msgbox, txt: [*:0]const u8) Obj {
        return .{ .obj = c.lv_msgbox_add_footer_button(self.obj, txt).? };
    }

    pub fn getContent(self: Msgbox) Obj {
        return .{ .obj = c.lv_msgbox_get_content(self.obj).? };
    }

    pub fn getHeader(self: Msgbox) Obj {
        return .{ .obj = c.lv_msgbox_get_header(self.obj).? };
    }

    pub fn getFooter(self: Msgbox) Obj {
        return .{ .obj = c.lv_msgbox_get_footer(self.obj).? };
    }

    pub fn close(self: Msgbox) void {
        c.lv_msgbox_close(self.obj);
    }
};

// =========================================================================
//  Spinner — circular loading indicator
// =========================================================================

/// LVGL spinner widget — a continuously rotating arc used as a loading
/// indicator.
pub const Spinner = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Spinner).size;
    pub const width = ObjectMixin(Spinner).width;
    pub const height = ObjectMixin(Spinner).height;
    pub const pos = ObjectMixin(Spinner).pos;
    pub const center = ObjectMixin(Spinner).center;
    pub const alignTo = ObjectMixin(Spinner).alignTo;
    pub const hide = ObjectMixin(Spinner).hide;
    pub const show = ObjectMixin(Spinner).show;
    pub const userData = ObjectMixin(Spinner).userData;

    pub const bgColor = StyleMixin(Spinner).bgColor;

    /// Create a new spinner as a child of `parent_`.
    pub fn create(parent_: anytype) Spinner {
        return .{ .obj = c.lv_spinner_create(parentObj(parent_)).? };
    }

    /// Set the animation period (ms) and indicator arc length (deg).
    pub fn animParams(self: Spinner, time_ms: u32, angle_deg: u32) Spinner {
        c.lv_spinner_set_anim_params(self.obj, time_ms, angle_deg);
        return self;
    }
};

// =========================================================================
//  Led — colored indicator circle
// =========================================================================

/// LVGL LED widget — a small colored indicator circle with adjustable
/// brightness.
pub const Led = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Led).size;
    pub const width = ObjectMixin(Led).width;
    pub const height = ObjectMixin(Led).height;
    pub const pos = ObjectMixin(Led).pos;
    pub const center = ObjectMixin(Led).center;
    pub const alignTo = ObjectMixin(Led).alignTo;
    pub const hide = ObjectMixin(Led).hide;
    pub const show = ObjectMixin(Led).show;
    pub const visible = ObjectMixin(Led).visible;
    pub const userData = ObjectMixin(Led).userData;
    pub const gridCell = ObjectMixin(Led).gridCell;

    /// Create a new LED as a child of `parent_`.
    pub fn create(parent_: anytype) Led {
        return .{ .obj = c.lv_led_create(parentObj(parent_)).? };
    }

    /// Set the LED base colour. Returns `self` for chaining.
    pub fn color(self: Led, col: Color) Led {
        c.lv_led_set_color(self.obj, col);
        return self;
    }

    /// Set the LED brightness (0–255). Returns `self` for chaining.
    pub fn brightness(self: Led, bright: u8) Led {
        c.lv_led_set_brightness(self.obj, bright);
        return self;
    }

    /// Turn the LED on. Returns `self` for chaining.
    pub fn on(self: Led) Led {
        c.lv_led_on(self.obj);
        return self;
    }

    /// Turn the LED off. Returns `self` for chaining.
    pub fn off(self: Led) Led {
        c.lv_led_off(self.obj);
        return self;
    }

    /// Toggle on/off. Returns `self` for chaining.
    pub fn toggle(self: Led) Led {
        c.lv_led_toggle(self.obj);
        return self;
    }

    /// Returns the current brightness.
    pub fn getBrightness(self: Led) u8 {
        return c.lv_led_get_brightness(self.obj);
    }
};

// =========================================================================
//  Chart — line / bar / scatter chart
// =========================================================================

/// Chart type constants.
pub const CHART_TYPE_NONE: u32 = 0;
pub const CHART_TYPE_LINE: u32 = 1;
pub const CHART_TYPE_BAR: u32 = 2;
pub const CHART_TYPE_SCATTER: u32 = 3;

/// Chart axis constants.
pub const CHART_AXIS_PRIMARY_Y: u32 = 0x00;
pub const CHART_AXIS_SECONDARY_Y: u32 = 0x01;
pub const CHART_AXIS_PRIMARY_X: u32 = 0x02;
pub const CHART_AXIS_SECONDARY_X: u32 = 0x04;

/// Chart update mode: shift existing points when adding.
pub const CHART_UPDATE_MODE_SHIFT: u32 = 0;
/// Chart update mode: wrap around (circular buffer).
pub const CHART_UPDATE_MODE_CIRCULAR: u32 = 1;

/// Handle to an `lv_chart_series_t`. Non-owning copy.
pub const Series = struct {
    chart: *c.lv_obj_t,
    raw: *c.lv_chart_series_t,

    pub fn nextValue(self: Series, v: i32) Series {
        c.lv_chart_set_next_value(self.chart, self.raw, v);
        return self;
    }

    pub fn setValueByIdx(self: Series, idx: u32, v: i32) Series {
        c.lv_chart_set_value_by_id(self.chart, self.raw, idx, v);
        return self;
    }
};

/// LVGL chart widget — line / bar / scatter with multiple series.
pub const Chart = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Chart).size;
    pub const width = ObjectMixin(Chart).width;
    pub const height = ObjectMixin(Chart).height;
    pub const pos = ObjectMixin(Chart).pos;
    pub const center = ObjectMixin(Chart).center;
    pub const alignTo = ObjectMixin(Chart).alignTo;
    pub const hide = ObjectMixin(Chart).hide;
    pub const show = ObjectMixin(Chart).show;
    pub const userData = ObjectMixin(Chart).userData;
    pub const gridCell = ObjectMixin(Chart).gridCell;

    pub const bgColor = StyleMixin(Chart).bgColor;
    pub const borderColor = StyleMixin(Chart).borderColor;
    pub const borderWidth = StyleMixin(Chart).borderWidth;
    pub const radius = StyleMixin(Chart).radius;
    pub const padAll = StyleMixin(Chart).padAll;

    pub const on = EventMixin(Chart).on;
    pub const onValueChanged = EventMixin(Chart).onValueChanged;

    pub fn create(parent_: anytype) Chart {
        return .{ .obj = c.lv_chart_create(parentObj(parent_)).? };
    }

    pub fn chartType(self: Chart, t: u32) Chart {
        c.lv_chart_set_type(self.obj, @intCast(t));
        return self;
    }

    pub fn pointCount(self: Chart, count: u32) Chart {
        c.lv_chart_set_point_count(self.obj, count);
        return self;
    }

    pub fn range(self: Chart, axis: u32, min: i32, max: i32) Chart {
        c.lv_chart_set_range(self.obj, @intCast(axis), min, max);
        return self;
    }

    pub fn updateMode(self: Chart, mode: u32) Chart {
        c.lv_chart_set_update_mode(self.obj, @intCast(mode));
        return self;
    }

    pub fn divLineCount(self: Chart, hdiv: u8, vdiv: u8) Chart {
        c.lv_chart_set_div_line_count(self.obj, hdiv, vdiv);
        return self;
    }

    pub fn addSeries(self: Chart, color: Color, axis: u32) Series {
        const ser = c.lv_chart_add_series(self.obj, color, @intCast(axis));
        return .{ .chart = self.obj, .raw = ser.? };
    }

    pub fn removeSeries(self: Chart, s: Series) Chart {
        c.lv_chart_remove_series(self.obj, s.raw);
        return self;
    }
};

// =========================================================================
//  Calendar — month-grid date picker
// =========================================================================

/// LVGL calendar widget — month-grid date picker.
pub const Calendar = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Calendar).size;
    pub const width = ObjectMixin(Calendar).width;
    pub const height = ObjectMixin(Calendar).height;
    pub const pos = ObjectMixin(Calendar).pos;
    pub const center = ObjectMixin(Calendar).center;
    pub const alignTo = ObjectMixin(Calendar).alignTo;
    pub const hide = ObjectMixin(Calendar).hide;
    pub const show = ObjectMixin(Calendar).show;
    pub const userData = ObjectMixin(Calendar).userData;

    pub const bgColor = StyleMixin(Calendar).bgColor;
    pub const textColor = StyleMixin(Calendar).textColor;
    pub const textFont = StyleMixin(Calendar).textFont;

    pub const on = EventMixin(Calendar).on;
    pub const onValueChanged = EventMixin(Calendar).onValueChanged;

    pub fn create(parent_: anytype) Calendar {
        return .{ .obj = c.lv_calendar_create(parentObj(parent_)).? };
    }

    pub fn today(self: Calendar, year: u32, month: u32, day: u32) Calendar {
        c.lv_calendar_set_today_date(self.obj, year, month, day);
        return self;
    }

    pub fn showed(self: Calendar, year: u32, month: u32) Calendar {
        c.lv_calendar_set_showed_date(self.obj, year, month);
        return self;
    }

    pub fn addHeaderArrow(self: Calendar) Obj {
        return .{ .obj = c.lv_calendar_header_arrow_create(self.obj).? };
    }

    pub fn addHeaderDropdown(self: Calendar) Obj {
        return .{ .obj = c.lv_calendar_header_dropdown_create(self.obj).? };
    }
};

// =========================================================================
//  Canvas — user-buffer drawing surface
// =========================================================================

/// LVGL canvas — a widget backed by a user-supplied pixel buffer.
pub const Canvas = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Canvas).size;
    pub const width = ObjectMixin(Canvas).width;
    pub const height = ObjectMixin(Canvas).height;
    pub const pos = ObjectMixin(Canvas).pos;
    pub const center = ObjectMixin(Canvas).center;
    pub const alignTo = ObjectMixin(Canvas).alignTo;
    pub const hide = ObjectMixin(Canvas).hide;
    pub const show = ObjectMixin(Canvas).show;
    pub const userData = ObjectMixin(Canvas).userData;

    pub const bgColor = StyleMixin(Canvas).bgColor;

    pub fn create(parent_: anytype) Canvas {
        return .{ .obj = c.lv_canvas_create(parentObj(parent_)).? };
    }

    /// Attach a pixel buffer of the given geometry and format. The
    /// buffer must outlive the canvas.
    pub fn buffer(self: Canvas, buf: ?*anyopaque, w: i32, h: i32, cf: c.lv_color_format_t) Canvas {
        c.lv_canvas_set_buffer(self.obj, buf, w, h, cf);
        return self;
    }

    /// Fill the entire canvas with the given colour and opacity.
    pub fn fillBg(self: Canvas, color: Color, opa: u8) Canvas {
        c.lv_canvas_fill_bg(self.obj, color, opa);
        return self;
    }

    /// Set a single pixel.
    pub fn setPixel(self: Canvas, x: i32, y: i32, color: Color) Canvas {
        c.lv_canvas_set_px(self.obj, x, y, color, 255);
        return self;
    }
};

// =========================================================================
//  Table — 2D data grid
// =========================================================================

/// LVGL table — 2D grid of cells with per-cell text.
pub const Table = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Table).size;
    pub const width = ObjectMixin(Table).width;
    pub const height = ObjectMixin(Table).height;
    pub const pos = ObjectMixin(Table).pos;
    pub const center = ObjectMixin(Table).center;
    pub const alignTo = ObjectMixin(Table).alignTo;
    pub const hide = ObjectMixin(Table).hide;
    pub const show = ObjectMixin(Table).show;
    pub const userData = ObjectMixin(Table).userData;

    pub const bgColor = StyleMixin(Table).bgColor;
    pub const textColor = StyleMixin(Table).textColor;
    pub const textFont = StyleMixin(Table).textFont;

    pub const on = EventMixin(Table).on;

    pub fn create(parent_: anytype) Table {
        return .{ .obj = c.lv_table_create(parentObj(parent_)).? };
    }

    pub fn cellValue(self: Table, row: u32, col: u32, txt: [*:0]const u8) Table {
        c.lv_table_set_cell_value(self.obj, row, col, txt);
        return self;
    }

    pub fn rowCount(self: Table, n: u32) Table {
        c.lv_table_set_row_count(self.obj, n);
        return self;
    }

    pub fn columnCount(self: Table, n: u32) Table {
        c.lv_table_set_column_count(self.obj, n);
        return self;
    }

    pub fn columnWidth(self: Table, col: u32, w: i32) Table {
        c.lv_table_set_column_width(self.obj, col, w);
        return self;
    }

    pub fn getRowCount(self: Table) u32 {
        return c.lv_table_get_row_count(self.obj);
    }

    pub fn getColumnCount(self: Table) u32 {
        return c.lv_table_get_column_count(self.obj);
    }
};

// =========================================================================
//  Tabview — tabbed container with swipeable pages
// =========================================================================

/// LVGL tabview — tabbed container with swipeable pages.
pub const Tabview = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Tabview).size;
    pub const width = ObjectMixin(Tabview).width;
    pub const height = ObjectMixin(Tabview).height;
    pub const pos = ObjectMixin(Tabview).pos;
    pub const center = ObjectMixin(Tabview).center;
    pub const alignTo = ObjectMixin(Tabview).alignTo;
    pub const hide = ObjectMixin(Tabview).hide;
    pub const show = ObjectMixin(Tabview).show;
    pub const userData = ObjectMixin(Tabview).userData;

    pub const bgColor = StyleMixin(Tabview).bgColor;

    pub const on = EventMixin(Tabview).on;
    pub const onValueChanged = EventMixin(Tabview).onValueChanged;

    pub fn create(parent_: anytype) Tabview {
        return .{ .obj = c.lv_tabview_create(parentObj(parent_)).? };
    }

    pub fn addTab(self: Tabview, name: [*:0]const u8) Obj {
        return .{ .obj = c.lv_tabview_add_tab(self.obj, name).? };
    }

    pub fn renameTab(self: Tabview, idx: u32, name: [*:0]const u8) Tabview {
        c.lv_tabview_rename_tab(self.obj, idx, name);
        return self;
    }

    pub fn setActive(self: Tabview, idx: u32, anim: bool) Tabview {
        const anim_val: c.lv_anim_enable_t = if (anim) animOn() else animOff();
        c.lv_tabview_set_active(self.obj, idx, anim_val);
        return self;
    }

    /// Place the tab bar on one of the four edges (LV_DIR_TOP/BOTTOM/LEFT/RIGHT).
    pub fn setTabBarPosition(self: Tabview, dir: c.lv_dir_t) Tabview {
        c.lv_tabview_set_tab_bar_position(self.obj, dir);
        return self;
    }

    /// Tab bar thickness in pixels (height for top/bottom, width for left/right).
    pub fn setTabBarSize(self: Tabview, bar_size: i32) Tabview {
        c.lv_tabview_set_tab_bar_size(self.obj, bar_size);
        return self;
    }

    pub fn getTabCount(self: Tabview) u32 {
        return c.lv_tabview_get_tab_count(self.obj);
    }

    pub fn getTabActive(self: Tabview) u32 {
        return c.lv_tabview_get_tab_active(self.obj);
    }
};

// =========================================================================
//  List — scrollable list of buttons and text headings
// =========================================================================

/// LVGL list — scrollable list of text headings and icon+text buttons.
pub const List = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(List).size;
    pub const width = ObjectMixin(List).width;
    pub const height = ObjectMixin(List).height;
    pub const pos = ObjectMixin(List).pos;
    pub const center = ObjectMixin(List).center;
    pub const alignTo = ObjectMixin(List).alignTo;
    pub const userData = ObjectMixin(List).userData;

    pub const bgColor = StyleMixin(List).bgColor;

    pub fn create(parent_: anytype) List {
        return .{ .obj = c.lv_list_create(parentObj(parent_)).? };
    }

    /// Add a text heading row. Returns the label.
    pub fn addText(self: List, text_: [*:0]const u8) Obj {
        return .{ .obj = c.lv_list_add_text(self.obj, text_).? };
    }

    /// Add a button row with optional icon (pass `null` for none).
    pub fn addButton(self: List, icon: ?*const anyopaque, text_: [*:0]const u8) Obj {
        return .{ .obj = c.lv_list_add_button(self.obj, icon, text_).? };
    }
};

// =========================================================================
//  Textarea — text input
// =========================================================================

/// LVGL textarea — single- or multi-line text input with placeholder,
/// password mode, max length, and accepted-character filter.
pub const Textarea = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Textarea).size;
    pub const width = ObjectMixin(Textarea).width;
    pub const height = ObjectMixin(Textarea).height;
    pub const pos = ObjectMixin(Textarea).pos;
    pub const center = ObjectMixin(Textarea).center;
    pub const alignTo = ObjectMixin(Textarea).alignTo;
    pub const hide = ObjectMixin(Textarea).hide;
    pub const show = ObjectMixin(Textarea).show;
    pub const addFlag = ObjectMixin(Textarea).addFlag;
    pub const removeFlag = ObjectMixin(Textarea).removeFlag;
    pub const addState = ObjectMixin(Textarea).addState;
    pub const removeState = ObjectMixin(Textarea).removeState;
    pub const userData = ObjectMixin(Textarea).userData;
    pub const gridCell = ObjectMixin(Textarea).gridCell;

    pub const bgColor = StyleMixin(Textarea).bgColor;
    pub const borderColor = StyleMixin(Textarea).borderColor;
    pub const borderWidth = StyleMixin(Textarea).borderWidth;
    pub const radius = StyleMixin(Textarea).radius;
    pub const padAll = StyleMixin(Textarea).padAll;
    pub const textColor = StyleMixin(Textarea).textColor;
    pub const textFont = StyleMixin(Textarea).textFont;

    pub const on = EventMixin(Textarea).on;
    pub const onValueChanged = EventMixin(Textarea).onValueChanged;

    pub fn create(parent_: anytype) Textarea {
        return .{ .obj = c.lv_textarea_create(parentObj(parent_)).? };
    }

    pub fn text(self: Textarea, txt: [*:0]const u8) Textarea {
        c.lv_textarea_set_text(self.obj, txt);
        return self;
    }

    pub fn addText(self: Textarea, txt: [*:0]const u8) Textarea {
        c.lv_textarea_add_text(self.obj, txt);
        return self;
    }

    pub fn placeholder(self: Textarea, txt: [*:0]const u8) Textarea {
        c.lv_textarea_set_placeholder_text(self.obj, txt);
        return self;
    }

    pub fn oneLine(self: Textarea, en: bool) Textarea {
        c.lv_textarea_set_one_line(self.obj, en);
        return self;
    }

    pub fn passwordMode(self: Textarea, en: bool) Textarea {
        c.lv_textarea_set_password_mode(self.obj, en);
        return self;
    }

    pub fn maxLength(self: Textarea, n: u32) Textarea {
        c.lv_textarea_set_max_length(self.obj, n);
        return self;
    }

    pub fn acceptedChars(self: Textarea, list: [*:0]const u8) Textarea {
        c.lv_textarea_set_accepted_chars(self.obj, list);
        return self;
    }

    pub fn cursorPos(self: Textarea, pos_: i32) Textarea {
        c.lv_textarea_set_cursor_pos(self.obj, pos_);
        return self;
    }

    pub fn cursorClickPos(self: Textarea, en: bool) Textarea {
        c.lv_textarea_set_cursor_click_pos(self.obj, en);
        return self;
    }

    pub fn getText(self: Textarea) [*:0]const u8 {
        return c.lv_textarea_get_text(self.obj);
    }

    pub fn getCursorPos(self: Textarea) u32 {
        return c.lv_textarea_get_cursor_pos(self.obj);
    }

    pub fn isPasswordMode(self: Textarea) bool {
        return c.lv_textarea_get_password_mode(self.obj);
    }

    pub fn isOneLine(self: Textarea) bool {
        return c.lv_textarea_get_one_line(self.obj);
    }

    pub fn addChar(self: Textarea, ch: u32) Textarea {
        c.lv_textarea_add_char(self.obj, ch);
        return self;
    }

    pub fn deleteChar(self: Textarea) Textarea {
        c.lv_textarea_delete_char(self.obj);
        return self;
    }
};

// =========================================================================
//  Dropdown — click-to-open selection list
// =========================================================================

/// LVGL dropdown — click opens a popup list of options (newline-separated).
pub const Dropdown = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Dropdown).size;
    pub const width = ObjectMixin(Dropdown).width;
    pub const pos = ObjectMixin(Dropdown).pos;
    pub const center = ObjectMixin(Dropdown).center;
    pub const alignTo = ObjectMixin(Dropdown).alignTo;
    pub const hide = ObjectMixin(Dropdown).hide;
    pub const show = ObjectMixin(Dropdown).show;
    pub const userData = ObjectMixin(Dropdown).userData;
    pub const gridCell = ObjectMixin(Dropdown).gridCell;

    pub const bgColor = StyleMixin(Dropdown).bgColor;
    pub const textColor = StyleMixin(Dropdown).textColor;
    pub const textFont = StyleMixin(Dropdown).textFont;

    pub const on = EventMixin(Dropdown).on;
    pub const onValueChanged = EventMixin(Dropdown).onValueChanged;

    pub fn create(parent_: anytype) Dropdown {
        return .{ .obj = c.lv_dropdown_create(parentObj(parent_)).? };
    }

    pub fn options(self: Dropdown, opts: [*:0]const u8) Dropdown {
        c.lv_dropdown_set_options(self.obj, opts);
        return self;
    }

    pub fn optionsStatic(self: Dropdown, opts: [*:0]const u8) Dropdown {
        c.lv_dropdown_set_options_static(self.obj, opts);
        return self;
    }

    pub fn addOption(self: Dropdown, opt: [*:0]const u8, pos_: u32) Dropdown {
        c.lv_dropdown_add_option(self.obj, opt, pos_);
        return self;
    }

    pub fn clearOptions(self: Dropdown) Dropdown {
        c.lv_dropdown_clear_options(self.obj);
        return self;
    }

    pub fn selected(self: Dropdown, sel: u32) Dropdown {
        c.lv_dropdown_set_selected(self.obj, sel);
        return self;
    }

    pub fn getSelected(self: Dropdown) u32 {
        return c.lv_dropdown_get_selected(self.obj);
    }

    pub fn getOptionCount(self: Dropdown) u32 {
        return c.lv_dropdown_get_option_count(self.obj);
    }

    pub fn getSelectedStr(self: Dropdown, buf: []u8) void {
        c.lv_dropdown_get_selected_str(self.obj, buf.ptr, @intCast(buf.len));
    }

    pub fn isOpen(self: Dropdown) bool {
        return c.lv_dropdown_is_open(self.obj);
    }

    pub fn open(self: Dropdown) void {
        c.lv_dropdown_open(self.obj);
    }

    pub fn close(self: Dropdown) void {
        c.lv_dropdown_close(self.obj);
    }
};

// Roller mode constants
pub const ROLLER_MODE_NORMAL: u32 = 0;
pub const ROLLER_MODE_INFINITE: u32 = 1;

// =========================================================================
//  Roller — iOS-picker-style scrollable wheel
// =========================================================================

/// LVGL roller — iOS-picker-style scrollable option wheel.
pub const Roller = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Roller).size;
    pub const width = ObjectMixin(Roller).width;
    pub const pos = ObjectMixin(Roller).pos;
    pub const center = ObjectMixin(Roller).center;
    pub const alignTo = ObjectMixin(Roller).alignTo;
    pub const hide = ObjectMixin(Roller).hide;
    pub const show = ObjectMixin(Roller).show;
    pub const userData = ObjectMixin(Roller).userData;
    pub const gridCell = ObjectMixin(Roller).gridCell;

    pub const bgColor = StyleMixin(Roller).bgColor;
    pub const textColor = StyleMixin(Roller).textColor;
    pub const textFont = StyleMixin(Roller).textFont;

    pub const on = EventMixin(Roller).on;
    pub const onValueChanged = EventMixin(Roller).onValueChanged;

    pub fn create(parent_: anytype) Roller {
        return .{ .obj = c.lv_roller_create(parentObj(parent_)).? };
    }

    pub fn options(self: Roller, opts: [*:0]const u8, mode: u32) Roller {
        c.lv_roller_set_options(self.obj, opts, @intCast(mode));
        return self;
    }

    pub fn selected(self: Roller, sel: u32, anim: bool) Roller {
        const anim_val: c.lv_anim_enable_t = if (anim) animOn() else animOff();
        c.lv_roller_set_selected(self.obj, sel, anim_val);
        return self;
    }

    pub fn visibleRowCount(self: Roller, rows: u32) Roller {
        c.lv_roller_set_visible_row_count(self.obj, rows);
        return self;
    }

    pub fn getSelected(self: Roller) u32 {
        return c.lv_roller_get_selected(self.obj);
    }

    pub fn getOptionCount(self: Roller) u32 {
        return c.lv_roller_get_option_count(self.obj);
    }

    pub fn getSelectedStr(self: Roller, buf: []u8) void {
        c.lv_roller_get_selected_str(self.obj, buf.ptr, @intCast(buf.len));
    }
};

// =========================================================================
//  Spinbox — numeric stepper
// =========================================================================

/// LVGL spinbox — numeric stepper with `+` / `−` buttons.
pub const Spinbox = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Spinbox).size;
    pub const width = ObjectMixin(Spinbox).width;
    pub const pos = ObjectMixin(Spinbox).pos;
    pub const center = ObjectMixin(Spinbox).center;
    pub const alignTo = ObjectMixin(Spinbox).alignTo;
    pub const hide = ObjectMixin(Spinbox).hide;
    pub const show = ObjectMixin(Spinbox).show;
    pub const userData = ObjectMixin(Spinbox).userData;
    pub const gridCell = ObjectMixin(Spinbox).gridCell;

    pub const bgColor = StyleMixin(Spinbox).bgColor;
    pub const textColor = StyleMixin(Spinbox).textColor;
    pub const textFont = StyleMixin(Spinbox).textFont;

    pub const on = EventMixin(Spinbox).on;
    pub const onValueChanged = EventMixin(Spinbox).onValueChanged;

    pub fn create(parent_: anytype) Spinbox {
        return .{ .obj = c.lv_spinbox_create(parentObj(parent_)).? };
    }

    pub fn value(self: Spinbox, v: i32) Spinbox {
        c.lv_spinbox_set_value(self.obj, v);
        return self;
    }

    pub fn range(self: Spinbox, min: i32, max: i32) Spinbox {
        c.lv_spinbox_set_range(self.obj, min, max);
        return self;
    }

    pub fn step(self: Spinbox, s: u32) Spinbox {
        c.lv_spinbox_set_step(self.obj, s);
        return self;
    }

    /// Configure display: `digit_count` total digits, `sep_pos` decimal
    /// position (1-based, from the right). `digitFormat(5, 2)` → `###.##`.
    pub fn digitFormat(self: Spinbox, digit_count: u32, sep_pos: u32) Spinbox {
        c.lv_spinbox_set_digit_format(self.obj, digit_count, sep_pos);
        return self;
    }

    pub fn rollover(self: Spinbox, on_: bool) Spinbox {
        c.lv_spinbox_set_rollover(self.obj, on_);
        return self;
    }

    pub fn cursorPos(self: Spinbox, pos_: u32) Spinbox {
        c.lv_spinbox_set_cursor_pos(self.obj, pos_);
        return self;
    }

    pub fn getValue(self: Spinbox) i32 {
        return c.lv_spinbox_get_value(self.obj);
    }

    pub fn getStep(self: Spinbox) i32 {
        return c.lv_spinbox_get_step(self.obj);
    }

    pub fn increment(self: Spinbox) Spinbox {
        c.lv_spinbox_increment(self.obj);
        return self;
    }

    pub fn decrement(self: Spinbox) Spinbox {
        c.lv_spinbox_decrement(self.obj);
        return self;
    }
};

// Keyboard mode constants
pub const KEYBOARD_MODE_TEXT_LOWER: u32 = 0;
pub const KEYBOARD_MODE_TEXT_UPPER: u32 = 1;
pub const KEYBOARD_MODE_SPECIAL: u32 = 2;
pub const KEYBOARD_MODE_NUMBER: u32 = 3;

// =========================================================================
//  Keyboard — on-screen keyboard attached to a Textarea
// =========================================================================

/// LVGL on-screen keyboard. Attach to a `Textarea` via `attach()`.
pub const Keyboard = struct {
    obj: *c.lv_obj_t,

    pub const size = ObjectMixin(Keyboard).size;
    pub const width = ObjectMixin(Keyboard).width;
    pub const pos = ObjectMixin(Keyboard).pos;
    pub const alignTo = ObjectMixin(Keyboard).alignTo;
    pub const hide = ObjectMixin(Keyboard).hide;
    pub const show = ObjectMixin(Keyboard).show;
    pub const userData = ObjectMixin(Keyboard).userData;

    pub const bgColor = StyleMixin(Keyboard).bgColor;

    pub const on = EventMixin(Keyboard).on;

    pub fn create(parent_: anytype) Keyboard {
        return .{ .obj = c.lv_keyboard_create(parent_obj: {
            break :parent_obj parentObj(parent_);
        }).? };
    }

    pub fn attach(self: Keyboard, ta: Textarea) Keyboard {
        c.lv_keyboard_set_textarea(self.obj, ta.obj);
        return self;
    }

    pub fn mode(self: Keyboard, m: u32) Keyboard {
        c.lv_keyboard_set_mode(self.obj, @intCast(m));
        return self;
    }

    pub fn popovers(self: Keyboard, en: bool) Keyboard {
        c.lv_keyboard_set_popovers(self.obj, en);
        return self;
    }
};

// =========================================================================
//  Screen — top-level display root
// =========================================================================

/// Top-level LVGL screen (display root without a parent).
///
/// Create via `Screen.create()` or fetch the active one with
/// `Screen.active()`, then swap it in via `load()` or `loadAnim()`.
pub const Screen = struct {
    obj: *c.lv_obj_t,

    // ObjectMixin aliases
    pub const size = ObjectMixin(Screen).size;
    pub const width = ObjectMixin(Screen).width;
    pub const height = ObjectMixin(Screen).height;
    pub const pos = ObjectMixin(Screen).pos;
    pub const center = ObjectMixin(Screen).center;
    pub const alignTo = ObjectMixin(Screen).alignTo;
    pub const hide = ObjectMixin(Screen).hide;
    pub const show = ObjectMixin(Screen).show;
    pub const visible = ObjectMixin(Screen).visible;
    pub const addFlag = ObjectMixin(Screen).addFlag;
    pub const removeFlag = ObjectMixin(Screen).removeFlag;
    pub const addState = ObjectMixin(Screen).addState;
    pub const removeState = ObjectMixin(Screen).removeState;
    pub const clickable = ObjectMixin(Screen).clickable;
    pub const userData = ObjectMixin(Screen).userData;
    pub const flexFlow = ObjectMixin(Screen).flexFlow;
    pub const flexAlign = ObjectMixin(Screen).flexAlign;
    pub const gridDsc = ObjectMixin(Screen).gridDsc;

    // StyleMixin aliases
    pub const bgColor = StyleMixin(Screen).bgColor;
    pub const bgOpa = StyleMixin(Screen).bgOpa;
    pub const borderColor = StyleMixin(Screen).borderColor;
    pub const borderWidth = StyleMixin(Screen).borderWidth;
    pub const radius = StyleMixin(Screen).radius;
    pub const padAll = StyleMixin(Screen).padAll;
    pub const padHor = StyleMixin(Screen).padHor;
    pub const padVer = StyleMixin(Screen).padVer;
    pub const padGap = StyleMixin(Screen).padGap;
    pub const textColor = StyleMixin(Screen).textColor;
    pub const textFont = StyleMixin(Screen).textFont;

    // EventMixin aliases
    pub const on = EventMixin(Screen).on;
    pub const onClick = EventMixin(Screen).onClick;

    /// Create a new top-level screen with no parent.
    pub fn create() Screen {
        return .{ .obj = c.lv_obj_create(null).? };
    }

    /// Returns the currently active screen.
    pub fn active() Screen {
        return .{ .obj = c.lv_screen_active().? };
    }

    /// Instantly load this screen as the active one.
    pub fn load(self: Screen) void {
        c.lv_screen_load(self.obj);
    }

    /// Load this screen with an animated transition.
    ///
    /// When `auto_del` is true the previously active screen is deleted
    /// after the transition — any surviving handle to it becomes invalid.
    pub fn loadAnim(
        self: Screen,
        anim: c.lv_screen_load_anim_t,
        time_ms: u32,
        delay_ms: u32,
        auto_del: bool,
    ) void {
        c.lv_screen_load_anim(self.obj, anim, time_ms, delay_ms, auto_del);
    }
};

// =========================================================================
//  Group — keyboard / encoder focus group
// =========================================================================

/// RAII wrapper for an LVGL input focus group (`lv_group_t`).
///
/// Groups route keyboard/encoder/button events to a focused member
/// widget. Essential for non-touch targets. Create, `add` widgets, then
/// call `deinit()` once when done.
pub const Group = struct {
    raw: *c.lv_group_t,

    /// Create a fresh focus group.
    pub fn create() Group {
        return .{ .raw = c.lv_group_create().? };
    }

    /// Delete the underlying group. Call once per Group.
    pub fn deinit(self: Group) void {
        c.lv_group_delete(self.raw);
    }

    /// Mark this group as the default. Returns `self` for chaining.
    pub fn setAsDefault(self: Group) Group {
        c.lv_group_set_default(self.raw);
        return self;
    }

    /// Add a widget to the group's focusable list. Returns `self`.
    pub fn add(self: Group, obj: anytype) Group {
        c.lv_group_add_obj(self.raw, obj.obj);
        return self;
    }

    /// Remove all widgets from this group. Returns `self`.
    pub fn removeAll(self: Group) Group {
        c.lv_group_remove_all_objs(self.raw);
        return self;
    }

    /// Move focus to the next member. Returns `self`.
    pub fn focusNext(self: Group) Group {
        c.lv_group_focus_next(self.raw);
        return self;
    }

    /// Move focus to the previous member. Returns `self`.
    pub fn focusPrev(self: Group) Group {
        c.lv_group_focus_prev(self.raw);
        return self;
    }

    /// Freeze/unfreeze focus movement. Returns `self`.
    pub fn focusFreeze(self: Group, freeze: bool) Group {
        c.lv_group_focus_freeze(self.raw, freeze);
        return self;
    }

    /// Returns the currently focused widget as an `Obj`.
    pub fn focused(self: Group) ?Obj {
        const raw = c.lv_group_get_focused(self.raw);
        return if (raw) |r| .{ .obj = r } else null;
    }

    /// Enable or disable encoder edit mode. Returns `self`.
    pub fn editMode(self: Group, enable: bool) Group {
        c.lv_group_set_editing(self.raw, enable);
        return self;
    }

    /// Returns the current edit-mode flag.
    pub fn isEditing(self: Group) bool {
        return c.lv_group_get_editing(self.raw);
    }

    /// Returns the number of widgets in the group.
    pub fn objCount(self: Group) u32 {
        return c.lv_group_get_obj_count(self.raw);
    }
};

// =========================================================================
//  Timer — RAII wrapper around lv_timer_t
// =========================================================================

/// RAII wrapper around `lv_timer_t`.
///
/// LVGL timers fire from inside `handler()` while the LVGL lock is
/// already held. Callbacks must NOT call `lock()` — the mutex is not
/// reentrant and a recursive acquire will deadlock. Prefer this over
/// `ove.Timer` for UI updates that only touch LVGL state.
///
/// Call `deinit()` once to free the underlying timer.
pub const Timer = struct {
    raw: *c.lv_timer_t,

    /// Create and start an LVGL timer. Returns the wrapper.
    pub fn create(cb: c.lv_timer_cb_t, period_ms: u32, user_data: ?*anyopaque) Timer {
        return .{ .raw = c.lv_timer_create(cb, period_ms, user_data).? };
    }

    /// Delete the underlying `lv_timer_t`. Call once per Timer.
    pub fn deinit(self: Timer) void {
        c.lv_timer_delete(self.raw);
    }

    /// Update the period in milliseconds. Returns `self` for chaining.
    pub fn period(self: Timer, ms: u32) Timer {
        c.lv_timer_set_period(self.raw, ms);
        return self;
    }

    /// Pause the timer (can be resumed). Returns `self` for chaining.
    pub fn pause(self: Timer) Timer {
        c.lv_timer_pause(self.raw);
        return self;
    }

    /// Resume a paused timer. Returns `self` for chaining.
    pub fn resume_(self: Timer) Timer {
        c.lv_timer_resume(self.raw);
        return self;
    }

    /// Set the number of times the timer fires; -1 for infinite. Returns `self`.
    pub fn repeatCount(self: Timer, count: i32) Timer {
        c.lv_timer_set_repeat_count(self.raw, count);
        return self;
    }

    /// Reset the internal elapsed-time counter. Returns `self`.
    pub fn reset(self: Timer) Timer {
        c.lv_timer_reset(self.raw);
        return self;
    }

    /// Make the timer ready to fire on the next handler pass. Returns `self`.
    pub fn ready(self: Timer) Timer {
        c.lv_timer_ready(self.raw);
        return self;
    }
};

// =========================================================================
//  State(T) — reactive integer state (lv_subject_t wrapper)
// =========================================================================

/// Storage size for an `lv_subject_t` — LVGL 9.2's layout is
/// ~80 bytes (linked list head + type/size bitfields + two
/// subject_value unions + user_data + flags). We use 128 bytes as a
/// safe upper bound, which gives generous headroom for LVGL 9.x
/// layout changes. Zig's cImport treats `lv_subject_t` as opaque
/// because of its internal bitfields, so we can't embed it directly;
/// instead we store a correctly-aligned byte buffer and cast.
const SUBJECT_STORAGE_SIZE: usize = 128;
const SUBJECT_STORAGE_ALIGN: usize = 8;

/// Reactive integer state backed by LVGL's observer subsystem.
///
/// Bind widget properties (Label text, Arc/Slider/Roller/Dropdown value)
/// to a `State` via `lvgl.labelBindText(&state, …)` and they update
/// automatically whenever you call `state.set(…)`.
///
/// # Address stability
///
/// `lv_subject_t` stores a linked list of observer references — the
/// `State` must not move after the first observer attaches. The
/// intended usage is inside a module-level `var` or a struct stored in
/// a stable location. Call `deinit()` exactly once when done.
pub fn State(comptime T: type) type {
    return struct {
        _storage: [SUBJECT_STORAGE_SIZE]u8 align(SUBJECT_STORAGE_ALIGN) = undefined,

        const Self = @This();

        fn rawPtr(self: *Self) *c.lv_subject_t {
            return @ptrCast(@alignCast(&self._storage));
        }

        /// Create a new reactive state with an initial value.
        pub fn init_(initial: T) Self {
            var s: Self = .{};
            c.lv_subject_init_int(s.rawPtr(), @intCast(initial));
            return s;
        }

        /// Tear down the underlying subject. Call once; no more accesses
        /// after this point.
        pub fn deinit(self: *Self) void {
            c.lv_subject_deinit(self.rawPtr());
        }

        /// Set a new value. All bound widgets update immediately.
        pub fn set(self: *Self, value: T) void {
            c.lv_subject_set_int(self.rawPtr(), @intCast(value));
        }

        /// Read the current value.
        pub fn get(self: *Self) T {
            return @intCast(c.lv_subject_get_int(self.rawPtr()));
        }

        /// Returns the raw `*lv_subject_t` pointer for widget binding.
        pub fn subjectPtr(self: *Self) *c.lv_subject_t {
            return self.rawPtr();
        }
    };
}

/// Walk up from the event's target through parents, returning the
/// first non-null `user_data` pointer. Used to recover a Component
/// instance from inside an event callback. Caller casts the returned
/// `*anyopaque` to the appropriate type.
pub fn componentFromEvent(e: ?*c.lv_event_t) ?*anyopaque {
    var target: ?*c.lv_obj_t = @ptrCast(c.lv_event_get_target(e));
    while (target) |t| {
        const ud = c.lv_obj_get_user_data(t);
        if (ud) |p| return p;
        target = c.lv_obj_get_parent(t);
    }
    return null;
}

/// Component framework: wraps a user type implementing
/// `pub fn build(self: *Derived, parent: Obj) Obj` with mount/unmount
/// lifecycle, root tracking, and `fromEvent` instance recovery.
///
/// Usage:
/// ```zig
/// const MyCounter = struct {
///     count: lvgl.State(i32),
///     pub fn build(self: *MyCounter, parent: lvgl.Obj) lvgl.Obj {
///         const root = lvgl.vbox(parent);
///         _ = lvgl.Label.create(root).bindText(&self.count, "Count: %d");
///         return .{ .obj = root.obj };
///     }
/// };
/// var my_counter: lvgl.Component(MyCounter) = undefined;
/// my_counter.init_(.{ .count = lvgl.State(i32).init_(0) });
/// my_counter.mount(lvgl.screenActive());
/// ```
pub fn Component(comptime Derived: type) type {
    return struct {
        derived: Derived,
        root: ?*c.lv_obj_t,

        const Self = @This();

        /// Initialize with a fully-constructed Derived instance.
        pub fn init_(self: *Self, derived: Derived) void {
            self.derived = derived;
            self.root = null;
        }

        /// Build and mount the component under `parent`. No-op if
        /// already mounted.
        pub fn mount(self: *Self, parent: anytype) void {
            if (self.root != null) return;
            const built = self.derived.build(.{ .obj = parentObj(parent) });
            self.root = built.obj;
            c.lv_obj_set_user_data(built.obj, @ptrCast(self));
        }

        /// Unmount and delete the component's widget subtree.
        pub fn unmount(self: *Self) void {
            if (self.root) |root| {
                self.root = null;
                c.lv_obj_delete(root);
            }
        }

        /// Returns `true` if currently mounted.
        pub fn isMounted(self: *const Self) bool {
            return self.root != null;
        }

        /// Returns the root widget wrapped as `Obj`, or null if unmounted.
        pub fn getRoot(self: *const Self) ?Obj {
            if (self.root) |r| return .{ .obj = r };
            return null;
        }

        /// Recover the component instance from an event, walking up
        /// through the parent chain.
        pub fn fromEvent(e: ?*c.lv_event_t) ?*Self {
            const ud = componentFromEvent(e) orelse return null;
            return @ptrCast(@alignCast(ud));
        }
    };
}

// Widget bind methods — attach these to existing widget types to avoid
// editing their struct definitions (Zig supports free-standing methods
// added at the namespace level via separate fns).

/// Bind a Label's text to a reactive State with a printf-style format.
pub fn labelBindText(label: Label, subject: anytype, fmt: [*:0]const u8) Label {
    _ = c.lv_label_bind_text(label.obj, subject.subjectPtr(), fmt);
    return label;
}

/// Bind an Arc's value to a reactive State.
pub fn arcBindValue(arc: Arc, subject: anytype) Arc {
    _ = c.lv_arc_bind_value(arc.obj, subject.subjectPtr());
    return arc;
}

/// Bind a Slider's value to a reactive State.
pub fn sliderBindValue(slider: Slider, subject: anytype) Slider {
    _ = c.lv_slider_bind_value(slider.obj, subject.subjectPtr());
    return slider;
}

/// Bind a Roller's selected index to a reactive State.
pub fn rollerBindValue(roller: Roller, subject: anytype) Roller {
    _ = c.lv_roller_bind_value(roller.obj, subject.subjectPtr());
    return roller;
}

/// Bind a Dropdown's selected index to a reactive State.
pub fn dropdownBindValue(dropdown: Dropdown, subject: anytype) Dropdown {
    _ = c.lv_dropdown_bind_value(dropdown.obj, subject.subjectPtr());
    return dropdown;
}

// =========================================================================
//  Animation — builder for lv_anim_t
// =========================================================================

/// LVGL sentinel for infinite repeats.
pub const ANIM_REPEAT_INFINITE: u32 = 0xFFFF_FFFF;

/// Fluent builder for an LVGL animation.
///
/// Configure step-by-step, then call `start()` — LVGL copies the state
/// into its internal list, so the builder can go out of scope right
/// after. Callbacks run under the LVGL lock; do not call `lock()` from
/// them.
pub const Animation = struct {
    inner: c.lv_anim_t,

    /// Create a fresh animation with defaults.
    pub fn init_() Animation {
        var a: c.lv_anim_t = undefined;
        c.lv_anim_init(&a);
        return .{ .inner = a };
    }

    /// Set the target variable. Returns `*self` for chaining.
    pub fn target(self: *Animation, var_: ?*anyopaque) *Animation {
        c.lv_anim_set_var(&self.inner, var_);
        return self;
    }

    /// Set the target as any widget-like struct with an `obj` field.
    pub fn targetObj(self: *Animation, widget: anytype) *Animation {
        c.lv_anim_set_var(&self.inner, widget.obj);
        return self;
    }

    /// Set start and end values. Returns `*self`.
    pub fn values(self: *Animation, from: i32, to: i32) *Animation {
        c.lv_anim_set_values(&self.inner, from, to);
        return self;
    }

    /// Set duration in milliseconds. Returns `*self`.
    pub fn duration(self: *Animation, ms: u32) *Animation {
        c.lv_anim_set_duration(&self.inner, ms);
        return self;
    }

    /// Set the delay before the animation starts. Returns `*self`.
    pub fn delay(self: *Animation, ms: u32) *Animation {
        c.lv_anim_set_delay(&self.inner, ms);
        return self;
    }

    /// Set the easing path callback. Returns `*self`.
    pub fn path(self: *Animation, cb: c.lv_anim_path_cb_t) *Animation {
        c.lv_anim_set_path_cb(&self.inner, cb);
        return self;
    }

    /// Set the repeat count. Returns `*self`.
    pub fn repeatCount(self: *Animation, count: u32) *Animation {
        c.lv_anim_set_repeat_count(&self.inner, count);
        return self;
    }

    /// Set the delay between repeats. Returns `*self`.
    pub fn repeatDelay(self: *Animation, ms: u32) *Animation {
        c.lv_anim_set_repeat_delay(&self.inner, ms);
        return self;
    }

    /// Set the reverse (playback) phase duration. Returns `*self`.
    pub fn playbackDuration(self: *Animation, ms: u32) *Animation {
        c.lv_anim_set_playback_duration(&self.inner, ms);
        return self;
    }

    /// Set the delay before the playback phase. Returns `*self`.
    pub fn playbackDelay(self: *Animation, ms: u32) *Animation {
        c.lv_anim_set_playback_delay(&self.inner, ms);
        return self;
    }

    /// Set the exec callback invoked every frame. Returns `*self`.
    pub fn execCb(self: *Animation, cb: c.lv_anim_exec_xcb_t) *Animation {
        c.lv_anim_set_exec_cb(&self.inner, cb);
        return self;
    }

    /// Set the ready callback invoked when the animation completes. Returns `*self`.
    pub fn readyCb(self: *Animation, cb: c.lv_anim_ready_cb_t) *Animation {
        c.lv_anim_set_ready_cb(&self.inner, cb);
        return self;
    }

    /// Start the animation. LVGL copies the inner state so the builder
    /// can go out of scope immediately after.
    pub fn start(self: *Animation) void {
        _ = c.lv_anim_start(&self.inner);
    }

    /// Stop any animations matching `(var, exec_cb)`.
    pub fn stop(var_: ?*anyopaque, exec_cb: c.lv_anim_exec_xcb_t) bool {
        return c.lv_anim_delete(var_, exec_cb);
    }
};

/// Linear easing.
pub fn pathLinear() c.lv_anim_path_cb_t {
    return c.lv_anim_path_linear;
}
/// Ease in.
pub fn pathEaseIn() c.lv_anim_path_cb_t {
    return c.lv_anim_path_ease_in;
}
/// Ease out.
pub fn pathEaseOut() c.lv_anim_path_cb_t {
    return c.lv_anim_path_ease_out;
}
/// Ease in-out.
pub fn pathEaseInOut() c.lv_anim_path_cb_t {
    return c.lv_anim_path_ease_in_out;
}
/// Overshoot — overshoots the end and settles back.
pub fn pathOvershoot() c.lv_anim_path_cb_t {
    return c.lv_anim_path_overshoot;
}
/// Bounce at the end.
pub fn pathBounce() c.lv_anim_path_cb_t {
    return c.lv_anim_path_bounce;
}
/// Step — jumps at the end.
pub fn pathStep() c.lv_anim_path_cb_t {
    return c.lv_anim_path_step;
}

// Shim callbacks adapting widget setters to the anim exec signature.
fn animSetXShim(var_: ?*anyopaque, v: i32) callconv(.C) void {
    c.lv_obj_set_x(@ptrCast(@alignCast(var_)), v);
}
fn animSetYShim(var_: ?*anyopaque, v: i32) callconv(.C) void {
    c.lv_obj_set_y(@ptrCast(@alignCast(var_)), v);
}
fn animSetWidthShim(var_: ?*anyopaque, v: i32) callconv(.C) void {
    c.lv_obj_set_width(@ptrCast(@alignCast(var_)), v);
}
fn animSetOpaShim(var_: ?*anyopaque, v: i32) callconv(.C) void {
    c.lv_obj_set_style_opa(@ptrCast(@alignCast(var_)), @intCast(v), c.LV_PART_MAIN);
}

/// Animate an object's X position to `to_x` over `duration_ms` (ease-out).
pub fn animateX(widget: anytype, to_x: i32, duration_ms: u32) void {
    const from = c.lv_obj_get_x(widget.obj);
    var a = Animation.init_();
    _ = a.target(widget.obj)
        .values(from, to_x)
        .duration(duration_ms)
        .path(pathEaseOut())
        .execCb(animSetXShim);
    a.start();
}

/// Animate an object's Y position to `to_y` over `duration_ms` (ease-out).
pub fn animateY(widget: anytype, to_y: i32, duration_ms: u32) void {
    const from = c.lv_obj_get_y(widget.obj);
    var a = Animation.init_();
    _ = a.target(widget.obj)
        .values(from, to_y)
        .duration(duration_ms)
        .path(pathEaseOut())
        .execCb(animSetYShim);
    a.start();
}

/// Animate an object's width to `to_w` over `duration_ms` (ease-out).
pub fn animateWidth(widget: anytype, to_w: i32, duration_ms: u32) void {
    const from = c.lv_obj_get_width(widget.obj);
    var a = Animation.init_();
    _ = a.target(widget.obj)
        .values(from, to_w)
        .duration(duration_ms)
        .path(pathEaseOut())
        .execCb(animSetWidthShim);
    a.start();
}

/// Fade an object's main-part opacity from current to `to_opa` over `duration_ms`.
pub fn animateOpa(widget: anytype, to_opa: u8, duration_ms: u32) void {
    const from: i32 = @intCast(c.lv_obj_get_style_opa(widget.obj, c.LV_PART_MAIN));
    var a = Animation.init_();
    _ = a.target(widget.obj)
        .values(from, @intCast(to_opa))
        .duration(duration_ms)
        .path(pathEaseInOut())
        .execCb(animSetOpaShim);
    a.start();
}

// =========================================================================
//  Style — embeds lv_style_t (NOT pointer-sized)
// =========================================================================

/// Standalone style object that wraps an embedded `lv_style_t`.
///
/// Unlike the inline style setters in `StyleMixin`, this type stores a
/// named style that can be applied to multiple objects. Must be initialized
/// with `init_()` and cleaned up with `deinit()`.
pub const Style = struct {
    style: c.lv_style_t,

    /// Initialize and return a new empty `Style`. Must call `deinit()` when done.
    pub fn init_() Style {
        var s: Style = undefined;
        c.lv_style_init(&s.style);
        return s;
    }

    /// Reset and free internal style resources.
    pub fn deinit(self: *Style) void {
        c.lv_style_reset(&self.style);
    }

    /// Return a pointer to the underlying `lv_style_t` for C interop.
    pub fn raw(self: *Style) *c.lv_style_t {
        return &self.style;
    }

    /// Set the background color property. Returns `self` for chaining.
    pub fn bgColor(self: *Style, col: Color) *Style {
        c.lv_style_set_bg_color(&self.style, col);
        return self;
    }

    /// Set the background opacity property (0–255). Returns `self` for chaining.
    pub fn bgOpa(self: *Style, opa: c.lv_opa_t) *Style {
        c.lv_style_set_bg_opa(&self.style, opa);
        return self;
    }

    /// Set the border color property. Returns `self` for chaining.
    pub fn borderColor(self: *Style, col: Color) *Style {
        c.lv_style_set_border_color(&self.style, col);
        return self;
    }

    /// Set the border width property in pixels. Returns `self` for chaining.
    pub fn borderWidth(self: *Style, w: i32) *Style {
        c.lv_style_set_border_width(&self.style, w);
        return self;
    }

    /// Set the corner radius property in pixels. Returns `self` for chaining.
    pub fn radius(self: *Style, r: i32) *Style {
        c.lv_style_set_radius(&self.style, r);
        return self;
    }

    /// Set equal padding on all four sides. Returns `self` for chaining.
    pub fn padAll(self: *Style, p: i32) *Style {
        c.lv_style_set_pad_all(&self.style, p);
        return self;
    }

    /// Set the text color property. Returns `self` for chaining.
    pub fn textColor(self: *Style, col: Color) *Style {
        c.lv_style_set_text_color(&self.style, col);
        return self;
    }

    /// Set the font property. Returns `self` for chaining.
    pub fn textFont(self: *Style, f: *const c.lv_font_t) *Style {
        c.lv_style_set_text_font(&self.style, f);
        return self;
    }
};

// =========================================================================
//  Layout helpers
// =========================================================================

/// Create a vertical flex `Box` (column layout) sized to its content.
///
/// Shorthand for `Box.create(parent_)` with `LV_FLEX_FLOW_COLUMN` and
/// `LV_SIZE_CONTENT` on both axes.
pub fn vbox(parent_: anytype) Box {
    const b = Box.create(parent_);
    c.lv_obj_set_flex_flow(b.obj, c.LV_FLEX_FLOW_COLUMN);
    c.lv_obj_set_size(b.obj, c.LV_SIZE_CONTENT, c.LV_SIZE_CONTENT);
    return b;
}

/// Create a horizontal flex `Box` (row layout) sized to its content.
///
/// Shorthand for `Box.create(parent_)` with `LV_FLEX_FLOW_ROW` and
/// `LV_SIZE_CONTENT` on both axes.
pub fn hbox(parent_: anytype) Box {
    const b = Box.create(parent_);
    c.lv_obj_set_flex_flow(b.obj, c.LV_FLEX_FLOW_ROW);
    c.lv_obj_set_size(b.obj, c.LV_SIZE_CONTENT, c.LV_SIZE_CONTENT);
    return b;
}
