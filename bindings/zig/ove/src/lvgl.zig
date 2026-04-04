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
};

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
