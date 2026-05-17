// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! MQTT 3.1.1 client with comptime trampoline callbacks.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const pin = @import("pin.zig");

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

/// MQTT client.
///
/// Heap mode (value-returning):
///
/// ```zig
/// var mq = try ove.net_mqtt.Client.create();
/// defer mq.deinit();
/// try mq.connect(cfg, onMessage);
/// ```
///
/// Zero-heap mode (two-phase init):
///
/// ```zig
/// var mq: ove.net_mqtt.Client = undefined;
/// try mq.init();
/// defer mq.deinit();
/// ```
pub const Client = if (pin.zero_heap) ZeroHeapClient else HeapClient;

const HeapClient = struct {
    handle: c.ove_mqtt_client_t,

    pub fn create() Error!Client {
        var h: c.ove_mqtt_client_t = null;
        try err.fromCode(c.ove_mqtt_client_create(&h));
        return .{ .handle = h };
    }

    /// Idempotent — clears `handle` after destroy so a redundant
    /// `defer client.deinit()` after an explicit `deinit()` is safe.
    pub fn deinit(self: *Client) void {
        if (self.handle == null) return;
        c.ove_mqtt_client_destroy(self.handle);
        self.handle = null;
    }

    /// Connect with a simple callback (no context).
    pub fn connect(
        self: *Client,
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

    /// Connect with a typed context pointer.
    pub fn connectWithContext(
        comptime Context: type,
        self: *Client,
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

    pub fn disconnect(self: *Client) void {
        c.ove_mqtt_disconnect(self.handle);
    }

    pub fn publish(self: *Client, topic: [:0]const u8, payload: []const u8, qos: Qos) Error!void {
        try err.fromCode(c.ove_mqtt_publish(self.handle, topic.ptr, payload.ptr, payload.len, qos.toC()));
    }

    pub fn subscribe(self: *Client, topic: [:0]const u8, qos: Qos) Error!void {
        try err.fromCode(c.ove_mqtt_subscribe(self.handle, topic.ptr, qos.toC()));
    }

    pub fn unsubscribe(self: *Client, topic: [:0]const u8) Error!void {
        try err.fromCode(c.ove_mqtt_unsubscribe(self.handle, topic.ptr));
    }

    pub fn pollOnce(self: *Client, timeout_ns: u64) Error!void {
        try err.fromCode(c.ove_mqtt_loop(self.handle, timeout_ns));
    }
};

const ZeroHeapClient = struct {
    storage: c.ove_mqtt_client_storage_t,
    handle: c.ove_mqtt_client_t,
    tracker: pin.Tracker,

    pub fn init(self: *Client) Error!void {
        self.storage = std.mem.zeroes(c.ove_mqtt_client_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_mqtt_client_init(&self.handle, &self.storage));
        self.tracker.record(self);
    }

    pub fn deinit(self: *Client) void {
        self.tracker.assertSame(self, "ove.MqttClient");
        if (self.handle == null) return;
        c.ove_mqtt_client_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    /// Connect with a simple callback (no context).
    pub fn connect(
        self: *Client,
        cfg: Config,
        comptime callback: fn ([]const u8, []const u8) void,
    ) Error!void {
        self.tracker.assertSame(self, "ove.MqttClient");
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

    /// Connect with a typed context pointer.
    pub fn connectWithContext(
        comptime Context: type,
        self: *Client,
        ctx: *Context,
        cfg: Config,
        comptime callback: fn (*Context, []const u8, []const u8) void,
    ) Error!void {
        self.tracker.assertSame(self, "ove.MqttClient");
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

    pub fn disconnect(self: *Client) void {
        self.tracker.assertSame(self, "ove.MqttClient");
        c.ove_mqtt_disconnect(self.handle);
    }

    pub fn publish(self: *Client, topic: [:0]const u8, payload: []const u8, qos: Qos) Error!void {
        self.tracker.assertSame(self, "ove.MqttClient");
        try err.fromCode(c.ove_mqtt_publish(self.handle, topic.ptr, payload.ptr, payload.len, qos.toC()));
    }

    pub fn subscribe(self: *Client, topic: [:0]const u8, qos: Qos) Error!void {
        self.tracker.assertSame(self, "ove.MqttClient");
        try err.fromCode(c.ove_mqtt_subscribe(self.handle, topic.ptr, qos.toC()));
    }

    pub fn unsubscribe(self: *Client, topic: [:0]const u8) Error!void {
        self.tracker.assertSame(self, "ove.MqttClient");
        try err.fromCode(c.ove_mqtt_unsubscribe(self.handle, topic.ptr));
    }

    pub fn pollOnce(self: *Client, timeout_ns: u64) Error!void {
        self.tracker.assertSame(self, "ove.MqttClient");
        try err.fromCode(c.ove_mqtt_loop(self.handle, timeout_ns));
    }
};
