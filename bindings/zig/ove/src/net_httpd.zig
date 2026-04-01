// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Embedded HTTP server (singleton).
//!
//! Routes are registered with typed Zig handler functions via a comptime
//! trampoline — no raw C function pointers in application code.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

// ---------------------------------------------------------------------------
// Request / ResponseWriter
// ---------------------------------------------------------------------------

/// HTTP request (opaque, borrowed for handler duration).
pub const Request = struct {
    raw: *c.ove_httpd_req_t,

    /// HTTP method string ("GET", "POST", ...).
    pub fn method(self: Request) [*:0]const u8 {
        return @ptrCast(c.ove_httpd_req_method(self.raw));
    }

    /// Full request path.
    pub fn path(self: Request) [*:0]const u8 {
        return @ptrCast(c.ove_httpd_req_path(self.raw));
    }

    /// Query string or null.
    pub fn query(self: Request) ?[*:0]const u8 {
        const p = c.ove_httpd_req_query(self.raw);
        if (p == null) return null;
        return @ptrCast(p);
    }

    /// POST body as a byte slice (empty if none).
    pub fn body(self: Request) []const u8 {
        const p = c.ove_httpd_req_body(self.raw);
        const len = c.ove_httpd_req_body_len(self.raw);
        if (p == null or len == 0) return &.{};
        const ptr: [*]const u8 = @ptrCast(p);
        return ptr[0..len];
    }

    /// Path segment by index (e.g. "/api/leds/0" → segment(2) = "0").
    pub fn segment(self: Request, idx: i32) ?[*:0]const u8 {
        const p = c.ove_httpd_req_segment(self.raw, idx);
        if (p == null) return null;
        return @ptrCast(p);
    }
};

/// HTTP response writer (opaque, borrowed for handler duration).
pub const ResponseWriter = struct {
    raw: *c.ove_httpd_resp_t,

    /// Send a JSON response.
    pub fn json(self: ResponseWriter, status_code: i32, json_str: [:0]const u8) Error!void {
        try err.fromCode(c.ove_httpd_resp_json(self.raw, status_code, json_str.ptr));
    }

    /// Send an HTML response.
    pub fn html(self: ResponseWriter, status_code: i32, html_str: []const u8) Error!void {
        try err.fromCode(c.ove_httpd_resp_html(self.raw, status_code, html_str.ptr, html_str.len));
    }

    /// Send a response with explicit content type.
    pub fn send(self: ResponseWriter, status_code: i32, content_type: [:0]const u8, body_data: []const u8) Error!void {
        try err.fromCode(c.ove_httpd_resp_send(self.raw, status_code, content_type.ptr, body_data.ptr, body_data.len));
    }

    /// Send a pre-gzipped response (adds Content-Encoding: gzip).
    pub fn sendGz(self: ResponseWriter, status_code: i32, content_type: [:0]const u8, body_data: []const u8) Error!void {
        try err.fromCode(c.ove_httpd_resp_send_gz(self.raw, status_code, content_type.ptr, body_data.ptr, body_data.len));
    }

    /// Send an error response.
    pub fn errResponse(self: ResponseWriter, status_code: i32, message: [:0]const u8) Error!void {
        try err.fromCode(c.ove_httpd_resp_error(self.raw, status_code, message.ptr));
    }
};

// ---------------------------------------------------------------------------
// Server functions
// ---------------------------------------------------------------------------

/// Server configuration.
pub const ServerConfig = struct {
    port: u16 = 80,
    max_body_size: i32 = 1024,
};

/// Start the HTTP server.
pub fn start(cfg: ServerConfig) Error!void {
    var cc: c.ove_httpd_config_t = std.mem.zeroes(c.ove_httpd_config_t);
    cc.port = cfg.port;
    cc.max_body_size = cfg.max_body_size;
    try err.fromCode(c.ove_httpd_start(&cc));
}

/// Stop the HTTP server.
pub fn stop() void {
    c.ove_httpd_stop();
}

/// Register a route with a typed Zig handler.
///
/// The handler is wrapped in a comptime trampoline that converts the
/// raw C pointers into `Request` and `ResponseWriter` values.
pub fn route(
    method_str: [:0]const u8,
    path_str: [:0]const u8,
    comptime handler: fn (Request, ResponseWriter) Error!void,
) Error!void {
    const Trampoline = struct {
        fn invoke(req: ?*c.ove_httpd_req_t, resp: ?*c.ove_httpd_resp_t) callconv(.c) c_int {
            const zig_req = Request{ .raw = req orelse return c.OVE_ERR_INVALID_PARAM };
            const zig_resp = ResponseWriter{ .raw = resp orelse return c.OVE_ERR_INVALID_PARAM };
            handler(zig_req, zig_resp) catch |e| {
                return switch (e) {
                    error.InvalidParam => c.OVE_ERR_INVALID_PARAM,
                    error.NoMemory => c.OVE_ERR_NO_MEMORY,
                    error.NotSupported => c.OVE_ERR_NOT_SUPPORTED,
                    else => -1,
                };
            };
            return 0;
        }
    };
    try err.fromCode(c.ove_httpd_route(method_str.ptr, path_str.ptr, &Trampoline.invoke));
}

/// Register the built-in Tasmota-style dashboard routes.
pub fn registerBuiltinRoutes() void {
    c.ove_httpd_register_builtin_routes();
}

/// Set the network interface used by built-in dashboard routes.
pub fn setNetif(handle: c.ove_netif_t) void {
    c.ove_httpd_set_netif(handle);
}

/// Append a log line to the httpd log ring buffer.
pub fn logAppend(line: [:0]const u8) void {
    c.ove_httpd_log_append(line.ptr);
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------

/// WebSocket support (available when CONFIG_OVE_NET_HTTPD_WS is enabled).
pub const ws = if (@hasDecl(c, "ove_httpd_ws_route")) struct {
    /// Register a WebSocket route.
    ///
    /// `on_message` and `on_close` may be null.
    pub fn route(
        path_str: [:0]const u8,
        on_message: c.ove_httpd_ws_handler_t,
        on_close: c.ove_httpd_ws_close_handler_t,
    ) Error!void {
        try err.fromCode(c.ove_httpd_ws_route(path_str.ptr, on_message, on_close));
    }

    /// Send a message to a WebSocket connection.
    pub fn send(conn: *c.ove_httpd_ws_conn_t, data: []const u8) Error!void {
        try err.fromCode(c.ove_httpd_ws_send(conn, data.ptr, data.len));
    }

    /// Broadcast a message to all WebSocket connections on a path.
    ///
    /// Returns the number of connections the message was sent to.
    pub fn broadcast(path_str: [:0]const u8, data: []const u8) c_int {
        return c.ove_httpd_ws_broadcast(path_str.ptr, data.ptr, data.len);
    }

    /// Return the number of active WebSocket connections.
    pub fn activeCount() c_int {
        return c.ove_httpd_ws_active_count();
    }
} else struct {};
