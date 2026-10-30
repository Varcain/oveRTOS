// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Blocking embedded HTTP server with REST-style routing.
//!
//! Wraps the oveRTOS `ove_httpd_*` API.  The server runs on a background
//! task; routes are registered with [`route`] and dispatched to
//! [`Handler`] callbacks that receive a [`Request`] / [`Response`] pair.
//!
//! The server is a singleton (one per process), so start/stop/route are
//! free functions rather than methods on a struct.
//!
//! ## Async alternative
//!
//! For an async HTTP server on top of [`crate::async_net`] use
//! [`picoserve`](https://crates.io/crates/picoserve) from crates.io.
//! Macro-driven routing, JSON via `serde`, optional WebSocket upgrade.
//! See [`crate::async_net`]'s module docs for the full pairing recipe.

use core::ffi::CStr;
use core::fmt;
use core::marker::PhantomData;

use crate::bindings;
use crate::error::{Error, Result};

// SAFETY (module-wide contract for the `unsafe { bindings::ove_*(...) }` FFI
// calls below): any handle passed to the C API is non-null and refers to a
// live RTOS object — wrapper constructors establish validity via
// `Error::from_code`, and `Drop` (or an explicit `deinit`) is the only place
// a handle is released. Pointer and slice arguments reference caller-owned
// memory valid for the duration of the call; the C side copies whatever it
// retains and does not alias them past return (verified against the
// signatures in `include/ove/*.h`). Blocks that deviate — `transmute`, raw
// pointer casts from user data, slice reconstruction via `from_raw_parts`,
// or storing a callback across the FFI boundary — carry their own
// `// SAFETY:` comment.

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
        // SAFETY: null/zero-len handled above; `p` points to `len` bytes of
        // request body owned by the C request object, valid for `&self` (the
        // handler borrow that produced this `Request`).
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

    /// Send a JSON response: `json` is the whole body, as bytes.
    ///
    /// # Errors
    /// Returns an error if the response cannot be sent.
    pub fn json(&self, status: i32, json: &[u8]) -> Result<()> {
        self.send(status, c"application/json", json)
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
    /// # Errors
    /// Returns an error if the response cannot be sent.
    pub fn send(&self, status: i32, content_type: &CStr, body: &[u8]) -> Result<()> {
        let rc = unsafe {
            bindings::ove_httpd_resp_send(
                self.raw,
                status,
                content_type.as_ptr(),
                body.as_ptr() as *const _,
                body.len(),
            )
        };
        Error::from_code(rc)
    }

    /// Send a JSON error response carrying `msg`.
    ///
    /// # Errors
    /// Returns an error if the response cannot be sent.
    pub fn error(&self, status: i32, msg: &CStr) -> Result<()> {
        let rc = unsafe { bindings::ove_httpd_resp_error(self.raw, status, msg.as_ptr()) };
        Error::from_code(rc)
    }

    /// Send a pre-gzipped response (adds `Content-Encoding: gzip`).
    ///
    /// # Errors
    /// Returns an error if the response cannot be sent.
    pub fn send_gz(&self, status: i32, content_type: &CStr, body: &[u8]) -> Result<()> {
        let rc = unsafe {
            bindings::ove_httpd_resp_send_gz(
                self.raw,
                status,
                content_type.as_ptr(),
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
/// Spawns a background task that accepts connections.  Routes may be
/// registered with [`route`] before or after starting, from any thread.
/// Handlers run one at a time on its single `httpd` thread, so a slow
/// handler delays every client.
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
///
/// Waits for the server thread, so never call it from a handler.  Open
/// WebSocket connections get a close frame (1001) and their close callback.
pub fn stop() {
    unsafe { bindings::ove_httpd_stop() }
}

/// Register a route handler, e.g. `route(c"GET", c"/api/leds", handler)`.
///
/// The method and path are copied (up to 7 and 63 bytes).
///
/// # Errors
/// Returns an error if the route table is full.
pub fn route(method: &CStr, path: &CStr, handler: Handler) -> Result<()> {
    let rc = unsafe { bindings::ove_httpd_route(method.as_ptr(), path.as_ptr(), handler) };
    Error::from_code(rc)
}

/// Register the built-in dashboard routes (`/api/info`, `/api/leds`, etc.).
///
/// Adds the standard device management API, before or after [`start`].
pub fn register_builtin_routes() {
    unsafe { bindings::ove_httpd_register_builtin_routes() }
}

/// Append a log line to the httpd log ring buffer.
///
/// Call from a log output hook to capture lines for `GET /api/log`.
pub fn log_append(line: &CStr) {
    unsafe { bindings::ove_httpd_log_append(line.as_ptr()) }
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
    use core::ffi::CStr;

    use crate::bindings;
    use crate::error::{Error, Result};

    /// Register a WebSocket route, e.g. `route(c"/ws", on_message, on_close)`.
    ///
    /// The path is copied (up to 63 bytes).
    ///
    /// # Errors
    /// Returns an error if the route table is full.
    pub fn route(
        path: &CStr,
        on_message: bindings::ove_httpd_ws_handler_t,
        on_close: bindings::ove_httpd_ws_close_handler_t,
    ) -> Result<()> {
        let rc = unsafe { bindings::ove_httpd_ws_route(path.as_ptr(), on_message, on_close) };
        Error::from_code(rc)
    }

    /// Send a text message to a WebSocket connection.
    ///
    /// Any thread may call it.  If the send fails the connection is closing:
    /// later sends fail, and its close callback runs on the httpd thread
    /// shortly after, which is when `conn` stops being valid.  A frame the
    /// client does not take within a second fails the send, and every other
    /// sender waits for it meanwhile.
    ///
    /// # Errors
    /// Returns an error if the send fails.
    ///
    /// # Safety
    /// `conn` must be a connection handle the httpd passed to a handler of
    /// this route, and its close callback must not have run yet: the handle
    /// is not valid after that.
    pub unsafe fn send(conn: *mut bindings::ove_httpd_ws_conn_t, data: &[u8]) -> Result<()> {
        let rc =
            unsafe { bindings::ove_httpd_ws_send(conn, data.as_ptr() as *const _, data.len()) };
        Error::from_code(rc)
    }

    /// Broadcast a message to all WebSocket connections on a path, from any
    /// thread; a connection whose send fails is closing, as for [`send`].
    ///
    /// Returns the number of connections the message was sent to.
    pub fn broadcast(path: &CStr, data: &[u8]) -> i32 {
        unsafe {
            bindings::ove_httpd_ws_broadcast(path.as_ptr(), data.as_ptr() as *const _, data.len())
        }
    }

    /// Return the number of active WebSocket connections.
    pub fn active_count() -> i32 {
        unsafe { bindings::ove_httpd_ws_active_count() }
    }
}
