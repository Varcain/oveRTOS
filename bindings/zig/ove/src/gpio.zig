// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// GPIO pin direction and pull-resistor mode (maps to `ove_gpio_mode_t`).
pub const Mode = c.ove_gpio_mode_t;

/// GPIO interrupt trigger mode (maps to `ove_gpio_irq_mode_t`).
pub const IrqMode = c.ove_gpio_irq_mode_t;

/// Configure a GPIO pin's direction and pull-resistor mode.
///
/// `port` and `pin` identify the GPIO port/pin on the target hardware.
/// `mode` controls input, output, pull-up, pull-down, etc.
/// Returns `Error` if the configuration is invalid or the pin is unavailable.
pub fn configure(port: u32, pin: u32, mode: Mode) Error!void {
    try err.fromCode(c.ove_gpio_configure(port, pin, mode));
}

/// Drive a GPIO output pin high (`value = true`) or low (`value = false`).
///
/// Returns `Error` if the pin is not configured as an output.
pub fn set(port: u32, pin: u32, value: bool) Error!void {
    try err.fromCode(c.ove_gpio_set(port, pin, if (value) @as(c_int, 1) else @as(c_int, 0)));
}

/// Read the current logical level of a GPIO pin.
///
/// Returns `true` for high, `false` for low.
/// Returns `Error` if the pin is unavailable or an I/O error occurs.
pub fn get(port: u32, pin: u32) Error!bool {
    const val = c.ove_gpio_get(port, pin);
    if (val < 0) {
        try err.fromCode(val);
        unreachable;
    }
    return val != 0;
}

/// Register an interrupt callback for a GPIO pin edge or level event.
///
/// `mode` selects the trigger (rising edge, falling edge, both, level, etc.).
/// `callback` receives the `port` and `pin` of the triggering pin and must not
/// perform any blocking RTOS operations (it runs in interrupt context).
///
/// Returns `Error` if the pin does not support interrupts or registration fails.
pub fn irqRegister(
    port: u32,
    pin: u32,
    mode: IrqMode,
    comptime callback: fn (u32, u32) void,
) Error!void {
    const Trampoline = struct {
        fn invoke(p: c_uint, pn: c_uint, _: ?*anyopaque) callconv(.c) void {
            callback(@intCast(p), @intCast(pn));
        }
    };
    try err.fromCode(c.ove_gpio_irq_register(
        port,
        pin,
        mode,
        &Trampoline.invoke,
        null,
    ));
}

/// Enable a previously registered GPIO interrupt on the given pin.
///
/// Returns `Error` if no interrupt was registered or enabling fails.
pub fn irqEnable(port: u32, pin: u32) Error!void {
    try err.fromCode(c.ove_gpio_irq_enable(port, pin));
}

/// Disable a GPIO interrupt without unregistering the callback.
///
/// The callback can be re-enabled with `irqEnable()`.
/// Returns `Error` if the operation fails.
pub fn irqDisable(port: u32, pin: u32) Error!void {
    try err.fromCode(c.ove_gpio_irq_disable(port, pin));
}
