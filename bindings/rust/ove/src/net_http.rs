// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Blocking HTTP/1.1 client with RAII response management.
//!
//! [`Client`] wraps the oveRTOS HTTP client handle with automatic resource
//! cleanup.  [`Response`] owns the heap-allocated body and headers returned
//! by `ove_http_get` / `ove_http_post` and frees them on drop.
//!
//! Works in both heap and zero-heap modes.
//!
//! ## Async alternative
//!
//! For async HTTP on top of [`crate::async_net`] use the
//! [`reqwless`](https://crates.io/crates/reqwless) crate from crates.io.
//! Supports HTTPS via `embedded-tls`, chunked encoding, redirects, and
//! produces an `embedded_io_async::Read` body. See [`crate::async_net`]'s
//! module docs for the full pairing recipe.

use core::fmt;

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
// Method
// ---------------------------------------------------------------------------

/// HTTP request method.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Method {
    /// HTTP GET.
    Get,
    /// HTTP POST.
    Post,
    /// HTTP PUT.
    Put,
    /// HTTP DELETE.
    Delete,
    /// HTTP PATCH.
    Patch,
}

impl Method {
    fn to_c(self) -> bindings::ove_http_method_t {
        match self {
            Method::Get => bindings::OVE_HTTP_GET,
            Method::Post => bindings::OVE_HTTP_POST,
            Method::Put => bindings::OVE_HTTP_PUT,
            Method::Delete => bindings::OVE_HTTP_DELETE,
            Method::Patch => bindings::OVE_HTTP_PATCH,
        }
    }
}

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

/// HTTP request header.
///
/// Both `name` and `value` must be null-terminated byte strings
/// (e.g. `b"Authorization\0"`, `b"Bearer token\0"`).
pub struct Header<'a> {
    pub name: &'a [u8],
    pub value: &'a [u8],
}

// ---------------------------------------------------------------------------
// ClientStorage
// ---------------------------------------------------------------------------

/// Backing storage for an HTTP client in zero-heap mode.
///
/// In heap mode this is a zero-size placeholder.  Pass a `&mut ClientStorage`
/// to [`Client::create`] — the borrow checker ensures the storage outlives
/// the client.
///
/// ```ignore
/// let mut storage = ClientStorage::new();
/// let client = Client::create(&mut storage)?;
/// ```
// FFI handle storage; the field is intentionally only addressed via raw
// pointers passed to C, so clippy's `field is never read` doesn't apply.
#[allow(dead_code)]
pub struct ClientStorage(bindings::ove_http_client_storage_t);

impl Default for ClientStorage {
    fn default() -> Self {
        Self::new()
    }
}

impl ClientStorage {
    /// Zero-initialised backing storage for a [`Client`] in zero-heap
    /// mode.  Place in a `static` and pass to [`Client::from_static`].
    pub fn new() -> Self {
        Self(unsafe { core::mem::zeroed() })
    }
}

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------

/// HTTP response with RAII lifetime for body and header buffers.
///
/// Returned by [`Client::get`] and [`Client::post`].  The body and header
/// memory is freed automatically when this value is dropped.
pub struct Response {
    raw: bindings::ove_http_response_t,
}

impl Response {
    /// HTTP status code (e.g. 200, 404).
    pub fn status(&self) -> i32 {
        self.raw.status
    }

    /// Response body as a byte slice (empty if no body).
    pub fn body(&self) -> &[u8] {
        if self.raw.body.is_null() || self.raw.body_len == 0 {
            return &[];
        }
        // SAFETY: null/zero-len handled above; `body` points to `body_len`
        // bytes owned by this `Response` and freed only on its `Drop`, so the
        // slice is valid for `&self`.
        unsafe { core::slice::from_raw_parts(self.raw.body as *const u8, self.raw.body_len) }
    }

    /// Raw response headers as a byte slice (empty if none).
    pub fn headers(&self) -> &[u8] {
        if self.raw.headers.is_null() || self.raw.headers_len == 0 {
            return &[];
        }
        // SAFETY: null/zero-len handled above; `headers` points to
        // `headers_len` bytes owned by this `Response` and freed only on its
        // `Drop`, so the slice is valid for `&self`.
        unsafe { core::slice::from_raw_parts(self.raw.headers as *const u8, self.raw.headers_len) }
    }
}

impl fmt::Debug for Response {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Response")
            .field("status", &self.raw.status)
            .field("body_len", &self.raw.body_len)
            .field("headers_len", &self.raw.headers_len)
            .finish()
    }
}

impl Drop for Response {
    fn drop(&mut self) {
        unsafe { bindings::ove_http_response_free(&mut self.raw) }
    }
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

/// HTTP/1.1 client.
///
/// Wraps `ove_http_client_t` with automatic cleanup on drop.
pub struct Client {
    handle: bindings::ove_http_client_t,
}

impl Client {
    /// Create a new HTTP client via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        let mut handle: bindings::ove_http_client_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_http_client_create(&mut handle) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `Client` and is not
    /// shared with another primitive.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(storage: *mut bindings::ove_http_client_storage_t) -> Result<Self> {
        let mut handle: bindings::ove_http_client_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_http_client_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create a client that works in both heap and zero-heap modes.
    ///
    /// In heap mode, allocates via the RTOS heap.  In zero-heap mode,
    /// uses caller-provided `storage`.  The borrow checker ensures the
    /// storage outlives the client.
    pub fn create(storage: &mut ClientStorage) -> Result<Self> {
        #[cfg(not(zero_heap))]
        {
            let _ = storage;
            Self::new()
        }
        #[cfg(zero_heap)]
        {
            unsafe { Self::from_static(&mut storage.0) }
        }
    }

    /// Perform an HTTP GET request.
    ///
    /// `url` must be a null-terminated URL (e.g. `b"http://example.com/path\0"`).
    ///
    /// # Errors
    /// Returns an error if the request fails at the transport or protocol level.
    pub fn get(&self, url: &[u8]) -> Result<Response> {
        let mut raw: bindings::ove_http_response_t = unsafe { core::mem::zeroed() };
        let rc = unsafe { bindings::ove_http_get(self.handle, url.as_ptr() as *const _, &mut raw) };
        Error::from_code(rc)?;
        Ok(Response { raw })
    }

    /// Perform an HTTP POST request.
    ///
    /// `url` and `content_type` must be null-terminated byte strings.
    ///
    /// # Errors
    /// Returns an error if the request fails at the transport or protocol level.
    pub fn post(&self, url: &[u8], content_type: &[u8], body: &[u8]) -> Result<Response> {
        let mut raw: bindings::ove_http_response_t = unsafe { core::mem::zeroed() };
        let rc = unsafe {
            bindings::ove_http_post(
                self.handle,
                url.as_ptr() as *const _,
                content_type.as_ptr() as *const _,
                body.as_ptr() as *const _,
                body.len(),
                &mut raw,
            )
        };
        Error::from_code(rc)?;
        Ok(Response { raw })
    }

    /// Perform an HTTP request with explicit method, optional body, and
    /// optional extra headers.
    ///
    /// `url` and `content_type` must be null-terminated byte strings.
    /// Each [`Header`] name/value must also be null-terminated.
    ///
    /// # Errors
    /// Returns an error if the request fails at the transport or protocol level.
    pub fn request_ex(
        &self,
        method: Method,
        url: &[u8],
        content_type: &[u8],
        body: &[u8],
        headers: &[Header<'_>],
    ) -> Result<Response> {
        // Convert Header slices into the C struct array on the stack.
        const MAX_HEADERS: usize = 16;
        let count = headers.len().min(MAX_HEADERS);
        let mut c_headers: [bindings::ove_http_header_t; MAX_HEADERS] =
            unsafe { core::mem::zeroed() };
        for i in 0..count {
            c_headers[i].name = headers[i].name.as_ptr() as *const _;
            c_headers[i].value = headers[i].value.as_ptr() as *const _;
        }

        let mut raw: bindings::ove_http_response_t = unsafe { core::mem::zeroed() };
        let rc = unsafe {
            bindings::ove_http_request_ex(
                self.handle,
                method.to_c(),
                url.as_ptr() as *const _,
                content_type.as_ptr() as *const _,
                body.as_ptr() as *const _,
                body.len(),
                c_headers.as_ptr(),
                count,
                &mut raw,
            )
        };
        Error::from_code(rc)?;
        Ok(Response { raw })
    }
}

crate::ove_handle_impl!(Client, ove_http_client_destroy, ove_http_client_deinit);

/// Configure the process-wide HTTPS TLS policy (see `ove_http_set_tls`).
///
/// Secure by default: with `ca_cert == None` and `allow_insecure == false`,
/// `https://` requests fail closed (the peer cannot be verified).  Call once
/// at startup before issuing requests.  The CA must outlive every request,
/// hence the `'static` bound.
pub fn set_tls(ca_cert: Option<&'static [u8]>, allow_insecure: bool) {
    let (ptr, len) = match ca_cert {
        Some(c) => (c.as_ptr(), c.len()),
        None => (core::ptr::null(), 0),
    };
    // SAFETY: a `'static` CA slice outlives all requests; the C side stores
    // the pointer for use during later TLS handshakes (it does not copy).
    unsafe { bindings::ove_http_set_tls(ptr, len, allow_insecure as i32) };
}
