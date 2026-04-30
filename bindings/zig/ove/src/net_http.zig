// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! HTTP/1.1 client with RAII response management.
//!
//! `Client` wraps the oveRTOS HTTP client handle.  `Response` owns the
//! heap-allocated body and headers returned by `get`/`post` and frees
//! them on `destroy()`.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const pin = @import("pin.zig");

// ---------------------------------------------------------------------------
// Method
// ---------------------------------------------------------------------------

/// HTTP request method.
pub const Method = enum(c_int) {
    get = c.OVE_HTTP_GET,
    post = c.OVE_HTTP_POST,
    put = c.OVE_HTTP_PUT,
    delete = c.OVE_HTTP_DELETE,
    patch = c.OVE_HTTP_PATCH,
};

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

/// HTTP request header (name-value pair).
///
/// Both `name` and `value` must be sentinel-terminated strings.
pub const Header = struct {
    name: [*:0]const u8,
    value: [*:0]const u8,
};

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------

/// HTTP response.  Body and headers are heap-allocated by the C layer;
/// call `destroy()` (typically via `defer`) to free them.
pub const Response = struct {
    raw: c.ove_http_response_t,

    /// HTTP status code (e.g. 200, 404).
    pub fn status(self: Response) i32 {
        return self.raw.status;
    }

    /// Response body as a byte slice (empty if no body).
    pub fn body(self: Response) []const u8 {
        if (self.raw.body == null or self.raw.body_len == 0) return &.{};
        const ptr: [*]const u8 = @ptrCast(self.raw.body);
        return ptr[0..self.raw.body_len];
    }

    /// Raw response headers as a byte slice.
    pub fn headers(self: Response) []const u8 {
        if (self.raw.headers == null or self.raw.headers_len == 0) return &.{};
        const ptr: [*]const u8 = @ptrCast(self.raw.headers);
        return ptr[0..self.raw.headers_len];
    }

    /// Free body and header buffers.  Safe to call multiple times.
    pub fn destroy(self: *Response) void {
        c.ove_http_response_free(&self.raw);
    }
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

/// HTTP/1.1 client.
///
/// ```zig
/// var http: ove.HttpClient = undefined;
/// try http.init();
/// defer http.deinit();
/// var resp = try http.get("http://example.com/");
/// defer resp.destroy();
/// ```
pub const Client = struct {
    storage: pin.Storage(c.ove_http_client_storage_t),
    handle: c.ove_http_client_t,
    tracker: pin.Tracker,

    pub fn init(self: *Client) Error!void {
        self.storage = pin.zeroStorage(c.ove_http_client_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_http_client_create(&self.handle));
        } else {
            try err.fromCode(c.ove_http_client_init(&self.handle, &self.storage));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *Client) void {
        self.tracker.assertSame(self, "ove.HttpClient");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_http_client_destroy(self.handle)
        else
            c.ove_http_client_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    /// Perform an HTTP GET request.
    pub fn get(self: *Client, url: [:0]const u8) Error!Response {
        self.tracker.assertSame(self, "ove.HttpClient");
        var raw: c.ove_http_response_t = std.mem.zeroes(c.ove_http_response_t);
        try err.fromCode(c.ove_http_get(self.handle, url.ptr, &raw));
        return .{ .raw = raw };
    }

    /// Perform an HTTP POST request.
    pub fn post(self: *Client, url: [:0]const u8, content_type: [:0]const u8, body_data: []const u8) Error!Response {
        self.tracker.assertSame(self, "ove.HttpClient");
        var raw: c.ove_http_response_t = std.mem.zeroes(c.ove_http_response_t);
        try err.fromCode(c.ove_http_post(self.handle, url.ptr, content_type.ptr, body_data.ptr, body_data.len, &raw));
        return .{ .raw = raw };
    }

    /// Perform an HTTP request with explicit method, optional body, and
    /// optional extra headers.
    pub fn requestEx(
        self: *Client,
        method: Method,
        url: [:0]const u8,
        content_type: ?[:0]const u8,
        body_data: ?[]const u8,
        hdrs: ?[]const Header,
    ) Error!Response {
        self.tracker.assertSame(self, "ove.HttpClient");
        const ct_ptr: ?[*]const u8 = if (content_type) |ct| ct.ptr else null;
        const body_ptr: ?[*]const u8 = if (body_data) |b| b.ptr else null;
        const body_len: usize = if (body_data) |b| b.len else 0;

        // Convert Header slice to C struct array on the stack.
        const max_headers = 16;
        var c_headers: [max_headers]c.ove_http_header_t = std.mem.zeroes([max_headers]c.ove_http_header_t);
        var header_count: usize = 0;
        if (hdrs) |h| {
            header_count = @min(h.len, max_headers);
            for (0..header_count) |i| {
                c_headers[i].name = h[i].name;
                c_headers[i].value = h[i].value;
            }
        }

        var raw: c.ove_http_response_t = std.mem.zeroes(c.ove_http_response_t);
        try err.fromCode(c.ove_http_request_ex(
            self.handle,
            @intCast(@intFromEnum(method)),
            url.ptr,
            ct_ptr,
            body_ptr,
            body_len,
            &c_headers,
            header_count,
            &raw,
        ));
        return .{ .raw = raw };
    }
};
