// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Blocking MQTT 3.1.1 client with safe callback.
//!
//! [`Client`] wraps the oveRTOS MQTT handle with automatic cleanup and a
//! safe Rust `fn(&str, &[u8])` message callback.  The trampoline pattern
//! (same as [`crate::timer::Timer`]) converts the C callback into a safe
//! Rust function call.
//!
//! Works in both heap and zero-heap modes.
//!
//! ## Async alternative
//!
//! For async MQTT on top of [`crate::async_net`] use the
//! [`rust-mqtt`](https://crates.io/crates/rust-mqtt) crate from crates.io.
//! Supports MQTT 3.1.1 and 5.0, QoS 0/1/2, no_std mode. See
//! [`crate::async_net`]'s module docs for the full pairing recipe.

use crate::bindings;
use crate::error::{Error, Result};

// ---------------------------------------------------------------------------
// QoS
// ---------------------------------------------------------------------------

/// MQTT Quality of Service level.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Qos {
    /// At most once delivery (fire-and-forget).
    AtMostOnce = 0,
    /// At least once delivery (acknowledged).
    AtLeastOnce = 1,
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

/// MQTT connection configuration.
///
/// String fields (`host`, `client_id`, `username`, `password`) must be
/// null-terminated byte slices.
pub struct Config<'a> {
    /// Broker hostname or IP (null-terminated).
    pub host: &'a [u8],
    /// Broker port (typically 1883 or 8883 for TLS).
    pub port: u16,
    /// Client identifier (null-terminated).
    pub client_id: &'a [u8],
    /// Username for authentication (null-terminated, or `None`).
    pub username: Option<&'a [u8]>,
    /// Password for authentication (null-terminated, or `None`).
    pub password: Option<&'a [u8]>,
    /// Keep-alive interval in seconds.
    pub keep_alive_s: u16,
    /// Whether to use TLS for the connection.
    pub use_tls: bool,
}

// ---------------------------------------------------------------------------
// Callback trampoline
// ---------------------------------------------------------------------------

/// Message callback -- topic as `&str` (UTF-8 per MQTT spec), payload as `&[u8]`.
pub type MessageFn = fn(&str, &[u8]);

/// Internal trampoline that converts the C callback into a safe Rust call.
///
/// The user's `fn(&str, &[u8])` is stored as the `user_data` pointer
/// (same pattern as [`crate::timer::Timer`]).
unsafe extern "C" fn mqtt_trampoline(
    topic: *const core::ffi::c_char,
    topic_len: usize,
    payload: *const core::ffi::c_void,
    payload_len: usize,
    user_data: *mut core::ffi::c_void,
) {
    // SAFETY: `user_data` was stored by `Client::connect` from a
    // `MessageFn` pointer (a `fn(&str, &[u8])`). Supported targets have
    // pointer-sized function pointers with a C-compatible ABI, so
    // round-tripping through `*mut c_void` is sound.
    let cb: MessageFn = unsafe { core::mem::transmute(user_data) };
    // SAFETY: `topic`/`payload` are valid for `topic_len`/`payload_len` bytes
    // as guaranteed by the MQTT client for the duration of this callback.
    let t = unsafe { core::slice::from_raw_parts(topic as *const u8, topic_len) };
    let p = unsafe { core::slice::from_raw_parts(payload as *const u8, payload_len) };
    // SAFETY: MQTT topic strings are UTF-8 per specification.
    let topic_str = unsafe { core::str::from_utf8_unchecked(t) };
    cb(topic_str, p);
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

/// Backing storage for an MQTT client in zero-heap mode.
///
/// ```ignore
/// let mut storage = ClientStorage::new();
/// let mut mqtt = Client::create(&mut storage)?;
/// ```
// FFI handle storage; the field is only addressed via raw pointers
// passed to C, so clippy's `field is never read` doesn't apply.
#[allow(dead_code)]
pub struct ClientStorage(bindings::ove_mqtt_client_storage_t);

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

/// MQTT 3.1.1 client.
///
/// Wraps `ove_mqtt_client_t` with automatic cleanup on drop.
pub struct Client {
    handle: bindings::ove_mqtt_client_t,
}

impl Client {
    /// Create a new MQTT client via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        let mut handle: bindings::ove_mqtt_client_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_mqtt_client_create(&mut handle) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `Client` and is not
    /// shared with another primitive.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(storage: *mut bindings::ove_mqtt_client_storage_t) -> Result<Self> {
        let mut handle: bindings::ove_mqtt_client_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_mqtt_client_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create a client that works in both heap and zero-heap modes.
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

    /// Connect to an MQTT broker.
    ///
    /// `on_message` is called for each incoming publish (topic + payload).
    ///
    /// # Errors
    /// Returns an error if the connection or MQTT handshake fails.
    pub fn connect(&mut self, cfg: &Config, on_message: MessageFn) -> Result<()> {
        let user_data = on_message as *mut core::ffi::c_void;

        let mut c: bindings::ove_mqtt_config_t = unsafe { core::mem::zeroed() };
        c.host = cfg.host.as_ptr() as *const _;
        c.port = cfg.port;
        c.client_id = cfg.client_id.as_ptr() as *const _;
        c.keep_alive_s = cfg.keep_alive_s;
        c.use_tls = cfg.use_tls as i32;
        c.on_message = Some(mqtt_trampoline);
        c.user_data = user_data;

        if let Some(u) = cfg.username {
            c.username = u.as_ptr() as *const _;
        }
        if let Some(p) = cfg.password {
            c.password = p.as_ptr() as *const _;
        }

        let rc = unsafe { bindings::ove_mqtt_connect(self.handle, &c) };
        Error::from_code(rc)
    }

    /// Disconnect from the MQTT broker.
    pub fn disconnect(&mut self) {
        unsafe { bindings::ove_mqtt_disconnect(self.handle) }
    }

    /// Publish a message on a topic.
    ///
    /// `topic` must be a null-terminated byte string.
    ///
    /// # Errors
    /// Returns an error if the publish fails.
    pub fn publish(&self, topic: &[u8], payload: &[u8], qos: Qos) -> Result<()> {
        let rc = unsafe {
            bindings::ove_mqtt_publish(
                self.handle,
                topic.as_ptr() as *const _,
                payload.as_ptr() as *const _,
                payload.len(),
                qos as bindings::ove_mqtt_qos_t,
            )
        };
        Error::from_code(rc)
    }

    /// Subscribe to a topic filter.
    ///
    /// `topic` must be a null-terminated byte string.
    ///
    /// # Errors
    /// Returns an error if the subscribe fails.
    pub fn subscribe(&self, topic: &[u8], qos: Qos) -> Result<()> {
        let rc = unsafe {
            bindings::ove_mqtt_subscribe(
                self.handle,
                topic.as_ptr() as *const _,
                qos as bindings::ove_mqtt_qos_t,
            )
        };
        Error::from_code(rc)
    }

    /// Unsubscribe from a topic filter.
    ///
    /// `topic` must be a null-terminated byte string.
    ///
    /// # Errors
    /// Returns an error if the unsubscribe fails.
    pub fn unsubscribe(&self, topic: &[u8]) -> Result<()> {
        let rc = unsafe { bindings::ove_mqtt_unsubscribe(self.handle, topic.as_ptr() as *const _) };
        Error::from_code(rc)
    }

    /// Process incoming packets and send keep-alive pings.
    ///
    /// Must be called periodically (in a loop or from a timer callback).
    ///
    /// # Errors
    /// Returns an error if the poll encounters a protocol or transport error.
    pub fn poll(&self, timeout: core::time::Duration) -> Result<()> {
        let rc = unsafe { bindings::ove_mqtt_loop(self.handle, crate::time::dur_to_ns(timeout)) };
        Error::from_code(rc)
    }
}

crate::ove_handle_impl!(
    Client,
    ove_mqtt_client_destroy,
    ove_mqtt_client_deinit,
    send_only
);
