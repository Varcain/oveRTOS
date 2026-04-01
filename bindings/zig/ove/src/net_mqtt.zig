// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! MQTT 3.1.1 client with comptime trampoline callbacks.
//!
//! Supports both a simple callback (`fn([]const u8, []const u8) void`)
//! and a typed-context variant following the `Timer.createWithContext`
//! pattern.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// MQTT QoS level.
pub const Qos = enum {
    at_most_once,
    at_least_once,

    fn toC(self: Qos) c.ove_mqtt_qos_t {
        return switch (self) {
            .at_most_once => c.OVE_MQTT_QOS0,
            .at_least_once => c.OVE_MQTT_QOS1,
        };
    }
};

/// MQTT connection configuration.
pub const Config = struct {
    host: [:0]const u8,
    port: u16 = 1883,
    client_id: [:0]const u8,
    username: ?[:0]const u8 = null,
    password: ?[:0]const u8 = null,
    keep_alive_s: u16 = 30,
    use_tls: bool = false,
};

/// MQTT client.
pub const Client = struct {
    handle: c.ove_mqtt_client_t,

    /// Create an MQTT client.
    pub fn create() Error!Client {
        var h: c.ove_mqtt_client_t = null;
        if (comptime @hasDecl(c, "ove_mqtt_client_create")) {
            try err.fromCode(c.ove_mqtt_client_create(&h));
        } else {
            const S = struct {
                var storage: c.ove_mqtt_client_storage_t = std.mem.zeroes(c.ove_mqtt_client_storage_t);
            };
            try err.fromCode(c.ove_mqtt_client_init(&h, &S.storage));
        }
        return .{ .handle = h };
    }

    /// Destroy the MQTT client.
    pub fn destroy(self: *Client) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_mqtt_client_destroy"))
            c.ove_mqtt_client_destroy(self.handle)
        else
            c.ove_mqtt_client_deinit(self.handle);
        self.handle = null;
    }

    /// Connect with a simple callback (no context).
    ///
    /// `callback` receives topic and payload as slices.
    pub fn connect(
        self: Client,
        cfg: Config,
        comptime callback: fn ([]const u8, []const u8) void,
    ) Error!void {
        const Trampoline = struct {
            fn invoke(
                topic: [*c]const u8,
                topic_len: usize,
                payload: ?*const anyopaque,
                payload_len: usize,
                _: ?*anyopaque,
            ) callconv(.c) void {
                const t: []const u8 = topic[0..topic_len];
                const p: [*]const u8 = @ptrCast(payload orelse return);
                callback(t, p[0..payload_len]);
            }
        };
        var cc = fillConfig(cfg);
        cc.on_message = &Trampoline.invoke;
        cc.user_data = null;
        try err.fromCode(c.ove_mqtt_connect(self.handle, &cc));
    }

    /// Connect with a typed context pointer (comptime trampoline pattern).
    pub fn connectWithContext(
        comptime Context: type,
        self: Client,
        ctx: *Context,
        cfg: Config,
        comptime callback: fn (*Context, []const u8, []const u8) void,
    ) Error!void {
        const Trampoline = struct {
            fn invoke(
                topic: [*c]const u8,
                topic_len: usize,
                payload: ?*const anyopaque,
                payload_len: usize,
                user_data: ?*anyopaque,
            ) callconv(.c) void {
                const t: []const u8 = topic[0..topic_len];
                const p: [*]const u8 = @ptrCast(payload orelse return);
                const ptr: *Context = @ptrCast(@alignCast(user_data));
                callback(ptr, t, p[0..payload_len]);
            }
        };
        var cc = fillConfig(cfg);
        cc.on_message = &Trampoline.invoke;
        cc.user_data = @ptrCast(ctx);
        try err.fromCode(c.ove_mqtt_connect(self.handle, &cc));
    }

    /// Disconnect from the broker.
    pub fn disconnect(self: Client) void {
        c.ove_mqtt_disconnect(self.handle);
    }

    /// Publish a message.
    pub fn publish(self: Client, topic: [:0]const u8, payload: []const u8, qos: Qos) Error!void {
        try err.fromCode(c.ove_mqtt_publish(self.handle, topic.ptr, payload.ptr, payload.len, qos.toC()));
    }

    /// Subscribe to a topic filter.
    pub fn subscribe(self: Client, topic: [:0]const u8, qos: Qos) Error!void {
        try err.fromCode(c.ove_mqtt_subscribe(self.handle, topic.ptr, qos.toC()));
    }

    /// Unsubscribe from a topic filter.
    pub fn unsubscribe(self: Client, topic: [:0]const u8) Error!void {
        try err.fromCode(c.ove_mqtt_unsubscribe(self.handle, topic.ptr));
    }

    /// Process incoming packets and send keep-alive.  Call periodically.
    pub fn loop_(self: Client, timeout_ms: u32) Error!void {
        try err.fromCode(c.ove_mqtt_loop(self.handle, timeout_ms));
    }

    // -- internal helpers --

    fn fillConfig(cfg: Config) c.ove_mqtt_config_t {
        var cc: c.ove_mqtt_config_t = std.mem.zeroes(c.ove_mqtt_config_t);
        cc.host = cfg.host.ptr;
        cc.port = cfg.port;
        cc.client_id = cfg.client_id.ptr;
        cc.keep_alive_s = cfg.keep_alive_s;
        cc.use_tls = if (cfg.use_tls) 1 else 0;
        if (cfg.username) |u| cc.username = u.ptr;
        if (cfg.password) |p| cc.password = p.ptr;
        return cc;
    }
};
