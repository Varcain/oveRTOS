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
/// Heap mode (value-returning):
///
/// ```zig
/// var http = try ove.net_http.Client.create();
/// defer http.deinit();
/// var resp = try http.get("http://example.com/");
/// defer resp.destroy();
/// ```
///
/// Zero-heap mode (two-phase init):
///
/// ```zig
/// var http: ove.net_http.Client = undefined;
/// try http.init();
/// defer http.deinit();
/// ```
pub const Client = if (pin.zero_heap) ZeroHeapClient else HeapClient;

/// Configure the process-wide HTTPS TLS policy (see `ove_http_set_tls`).
///
/// Secure by default: with an empty `ca_cert` and `allow_insecure` false,
/// `https://` requests fail closed (the peer cannot be verified).  Call once
/// at startup before issuing requests; the CA bytes must outlive every
/// request.
pub fn setTls(ca_cert: []const u8, allow_insecure: bool) void {
    c.ove_http_set_tls(
        if (ca_cert.len == 0) null else ca_cert.ptr,
        ca_cert.len,
        @intFromBool(allow_insecure),
    );
}

const HeapClient = struct {
    handle: c.ove_http_client_t,

    pub fn create() Error!Client {
        var h: c.ove_http_client_t = null;
        try err.fromCode(c.ove_http_client_create(&h));
        return .{ .handle = h };
    }

    /// Idempotent — clears `handle` after destroy so a redundant
    /// `defer client.deinit()` after an explicit `deinit()` is safe.
    pub fn deinit(self: *Client) void {
        if (self.handle == null) return;
        c.ove_http_client_destroy(self.handle);
        self.handle = null;
    }

    /// Perform an HTTP GET request.
    pub fn get(self: *Client, url: [:0]const u8) Error!Response {
        var raw: c.ove_http_response_t = std.mem.zeroes(c.ove_http_response_t);
        try err.fromCode(c.ove_http_get(self.handle, url.ptr, &raw));
        return .{ .raw = raw };
    }

    /// Perform an HTTP POST request.
    pub fn post(self: *Client, url: [:0]const u8, content_type: [:0]const u8, body_data: []const u8) Error!Response {
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
        return requestExImpl(self.handle, method, url, content_type, body_data, hdrs);
    }
};

const ZeroHeapClient = struct {
    storage: c.ove_http_client_storage_t,
    handle: c.ove_http_client_t,
    tracker: pin.Tracker,

    pub fn init(self: *Client) Error!void {
        self.storage = std.mem.zeroes(c.ove_http_client_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_http_client_init(&self.handle, &self.storage));
        self.tracker.record(self);
    }

    pub fn deinit(self: *Client) void {
        self.tracker.assertSame(self, "ove.HttpClient");
        if (self.handle == null) return;
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

    pub fn requestEx(
        self: *Client,
        method: Method,
        url: [:0]const u8,
        content_type: ?[:0]const u8,
        body_data: ?[]const u8,
        hdrs: ?[]const Header,
    ) Error!Response {
        self.tracker.assertSame(self, "ove.HttpClient");
        return requestExImpl(self.handle, method, url, content_type, body_data, hdrs);
    }
};

fn requestExImpl(
    handle: c.ove_http_client_t,
    method: Method,
    url: [:0]const u8,
    content_type: ?[:0]const u8,
    body_data: ?[]const u8,
    hdrs: ?[]const Header,
) Error!Response {
    const ct_ptr: ?[*]const u8 = if (content_type) |ct| ct.ptr else null;
    const body_ptr: ?[*]const u8 = if (body_data) |b| b.ptr else null;
    const body_len: usize = if (body_data) |b| b.len else 0;

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
        handle,
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
