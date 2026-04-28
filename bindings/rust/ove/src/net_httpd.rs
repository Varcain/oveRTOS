// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Embedded HTTP server with REST-style routing.
//!
//! Wraps the oveRTOS `ove_httpd_*` API.  The server runs on a background
//! task; routes are registered with [`route`] and dispatched to
//! [`Handler`] callbacks that receive a [`Request`] / [`Response`] pair.
//!
//! The server is a singleton (one per process), so start/stop/route are
//! free functions rather than methods on a struct.

use core::fmt;
use core::marker::PhantomData;

use crate::bindings;
use crate::error::{Error, Result};

// ---------------------------------------------------------------------------
// Handler type
// ---------------------------------------------------------------------------

/// Route handler callback.
///
/// This is the raw C function pointer type:
/// `fn(req: *mut ove_httpd_req_t, resp: *mut ove_httpd_resp_t) -> i32`.
pub type Handler = bindings::ove_httpd_handler_t;

// ---------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------

/// HTTP request passed to a route handler.
///
/// Borrows the underlying C request for the duration of the handler call.
/// Do not store this value beyond the handler invocation.
pub struct Request<'a> {
    raw: *mut bindings::ove_httpd_req_t,
    _marker: PhantomData<&'a ()>,
}

impl<'a> Request<'a> {
    /// Create a `Request` from a raw C pointer.
    ///
    /// # Safety
    /// `raw` must be a valid pointer obtained from an active httpd handler
    /// callback and must remain valid for the lifetime `'a`.
    pub unsafe fn from_raw(raw: *mut bindings::ove_httpd_req_t) -> Self {
        Self {
            raw,
            _marker: PhantomData,
        }
    }

    /// The HTTP method string (e.g. "GET", "POST").
    pub fn method(&self) -> &[u8] {
        let p = unsafe { bindings::ove_httpd_req_method(self.raw) };
        if p.is_null() {
            return &[];
        }
        unsafe { core::ffi::CStr::from_ptr(p).to_bytes() }
    }

    /// The full request path (e.g. "/api/leds/0").
    pub fn path(&self) -> &[u8] {
        let p = unsafe { bindings::ove_httpd_req_path(self.raw) };
        if p.is_null() {
            return &[];
        }
        unsafe { core::ffi::CStr::from_ptr(p).to_bytes() }
    }

    /// The query string after '?' (empty slice if none).
    pub fn query(&self) -> &[u8] {
        let p = unsafe { bindings::ove_httpd_req_query(self.raw) };
        if p.is_null() {
            return &[];
        }
        unsafe { core::ffi::CStr::from_ptr(p).to_bytes() }
    }

    /// The request body (empty slice if none).
    pub fn body(&self) -> &[u8] {
        let p = unsafe { bindings::ove_httpd_req_body(self.raw) };
        let len = unsafe { bindings::ove_httpd_req_body_len(self.raw) };
        if p.is_null() || len == 0 {
            return &[];
        }
        unsafe { core::slice::from_raw_parts(p as *const u8, len) }
    }

    /// A path segment by index.
    ///
    /// For path "/api/leds/0": segment 0 = "api", 1 = "leds", 2 = "0".
    /// Returns `None` if the index is out of range.
    pub fn segment(&self, idx: i32) -> Option<&[u8]> {
        let p = unsafe { bindings::ove_httpd_req_segment(self.raw, idx) };
        if p.is_null() {
            return None;
        }
        Some(unsafe { core::ffi::CStr::from_ptr(p).to_bytes() })
    }
}

impl fmt::Debug for Request<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Request")
            .field(
                "path",
                &core::str::from_utf8(self.path()).unwrap_or("<non-utf8>"),
            )
            .finish()
    }
}

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------

/// HTTP response writer passed to a route handler.
///
/// Borrows the underlying C response for the duration of the handler call.
/// Do not store this value beyond the handler invocation.
pub struct Response<'a> {
    raw: *mut bindings::ove_httpd_resp_t,
    _marker: PhantomData<&'a ()>,
}

impl<'a> Response<'a> {
    /// Create a `Response` from a raw C pointer.
    ///
    /// # Safety
    /// `raw` must be a valid pointer obtained from an active httpd handler
    /// callback and must remain valid for the lifetime `'a`.
    pub unsafe fn from_raw(raw: *mut bindings::ove_httpd_resp_t) -> Self {
        Self {
            raw,
            _marker: PhantomData,
        }
    }

    /// Send a JSON response.
    ///
    /// `json` must be a null-terminated byte string.
    ///
    /// # Errors
    /// Returns an error if the response cannot be sent.
    pub fn json(&self, status: i32, json: &[u8]) -> Result<()> {
        let rc =
            unsafe { bindings::ove_httpd_resp_json(self.raw, status, json.as_ptr() as *const _) };
        Error::from_code(rc)
    }

    /// Send an HTML response.
    ///
    /// # Errors
    /// Returns an error if the response cannot be sent.
    pub fn html(&self, status: i32, html: &[u8]) -> Result<()> {
        let rc = unsafe {
            bindings::ove_httpd_resp_html(self.raw, status, html.as_ptr() as *const _, html.len())
        };
        Error::from_code(rc)
    }

    /// Send a response with arbitrary content type.
    ///
    /// `content_type` must be a null-terminated byte string.
    ///
    /// # Errors
    /// Returns an error if the response cannot be sent.
    pub fn send(&self, status: i32, content_type: &[u8], body: &[u8]) -> Result<()> {
        let rc = unsafe {
            bindings::ove_httpd_resp_send(
                self.raw,
                status,
                content_type.as_ptr() as *const _,
                body.as_ptr() as *const _,
                body.len(),
            )
        };
        Error::from_code(rc)
    }

    /// Send a JSON error response.
    ///
    /// `msg` must be a null-terminated byte string.
    ///
    /// # Errors
    /// Returns an error if the response cannot be sent.
    pub fn error(&self, status: i32, msg: &[u8]) -> Result<()> {
        let rc =
            unsafe { bindings::ove_httpd_resp_error(self.raw, status, msg.as_ptr() as *const _) };
        Error::from_code(rc)
    }

    /// Send a pre-gzipped response (adds `Content-Encoding: gzip`).
    ///
    /// `content_type` must be a null-terminated byte string.
    ///
    /// # Errors
    /// Returns an error if the response cannot be sent.
    pub fn send_gz(&self, status: i32, content_type: &[u8], body: &[u8]) -> Result<()> {
        let rc = unsafe {
            bindings::ove_httpd_resp_send_gz(
                self.raw,
                status,
                content_type.as_ptr() as *const _,
                body.as_ptr() as *const _,
                body.len(),
            )
        };
        Error::from_code(rc)
    }
}

impl fmt::Debug for Response<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Response")
            .field("raw", &format_args!("{:p}", self.raw))
            .finish()
    }
}

// ---------------------------------------------------------------------------
// Server free functions
// ---------------------------------------------------------------------------

/// Start the HTTP server on the given port.
///
/// Spawns a background task that accepts connections.  Register routes
/// with [`route`] before or after starting.
///
/// # Errors
/// Returns an error if the server cannot bind or start.
pub fn start(port: u16, max_body: i32) -> Result<()> {
    let cfg = bindings::ove_httpd_config_t {
        port,
        max_body_size: max_body,
    };
    let rc = unsafe { bindings::ove_httpd_start(&cfg) };
    Error::from_code(rc)
}

/// Stop the HTTP server and close the listening socket.
pub fn stop() {
    unsafe { bindings::ove_httpd_stop() }
}

/// Register a route handler.
///
/// `method` and `path` must be null-terminated byte strings
/// (e.g. `b"GET\0"`, `b"/api/leds\0"`).
///
/// # Errors
/// Returns an error if the route table is full.
pub fn route(method: &[u8], path: &[u8], handler: Handler) -> Result<()> {
    let rc = unsafe {
        bindings::ove_httpd_route(
            method.as_ptr() as *const _,
            path.as_ptr() as *const _,
            handler,
        )
    };
    Error::from_code(rc)
}

/// Register the built-in dashboard routes (`/api/info`, `/api/leds`, etc.).
///
/// Call after [`start`] to add the standard device management API.
pub fn register_builtin_routes() {
    unsafe { bindings::ove_httpd_register_builtin_routes() }
}

/// Append a log line to the httpd log ring buffer.
///
/// Call from a log output hook to capture lines for `GET /api/log`.
pub fn log_append(line: &[u8]) {
    unsafe { bindings::ove_httpd_log_append(line.as_ptr() as *const _) }
}

/// Bind the HTTP server to a specific network interface.
pub fn set_netif(handle: bindings::ove_netif_t) {
    unsafe { bindings::ove_httpd_set_netif(handle) }
}

// ---------------------------------------------------------------------------
// WebSocket support
// ---------------------------------------------------------------------------

#[cfg(has_net_httpd_ws)]
pub mod ws {
    use crate::bindings;
    use crate::error::{Error, Result};

    /// Register a WebSocket route.
    ///
    /// `path` must be a null-terminated byte string (e.g. `b"/ws\0"`).
    ///
    /// # Errors
    /// Returns an error if the route table is full.
    pub fn route(
        path: &[u8],
        on_message: bindings::ove_httpd_ws_handler_t,
        on_close: bindings::ove_httpd_ws_close_handler_t,
    ) -> Result<()> {
        let rc = unsafe {
            bindings::ove_httpd_ws_route(path.as_ptr() as *const _, on_message, on_close)
        };
        Error::from_code(rc)
    }

    /// Send a text message to a WebSocket connection.
    ///
    /// # Errors
    /// Returns an error if the send fails.
    pub fn send(conn: *mut bindings::ove_httpd_ws_conn_t, data: &[u8]) -> Result<()> {
        let rc =
            unsafe { bindings::ove_httpd_ws_send(conn, data.as_ptr() as *const _, data.len()) };
        Error::from_code(rc)
    }

    /// Broadcast a message to all WebSocket connections on a path.
    ///
    /// `path` must be a null-terminated byte string.
    ///
    /// Returns the number of connections the message was sent to.
    pub fn broadcast(path: &[u8], data: &[u8]) -> i32 {
        unsafe {
            bindings::ove_httpd_ws_broadcast(
                path.as_ptr() as *const _,
                data.as_ptr() as *const _,
                data.len(),
            )
        }
    }

    /// Return the number of active WebSocket connections.
    pub fn active_count() -> i32 {
        unsafe { bindings::ove_httpd_ws_active_count() }
    }
}
