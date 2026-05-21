// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Build-time configuration exposed as Rust `const`s and `cfg` flags.
//!
//! Every `CONFIG_OVE_*` symbol defined in `ove_config.h` lands here:
//!
//! - **Boolean symbols** (`CONFIG_OVE_FOO=y`) — appear as
//!   `pub const CONFIG_OVE_FOO: bool = true;` *and* a per-symbol cfg
//!   gate `config_ove_foo` is emitted by `build.rs`. You can write
//!   `#[cfg(config_ove_foo)]` to gate code on this symbol from the
//!   call site.
//! - **Integer symbols** (`CONFIG_OVE_PM_MAX_WAKE_SOURCES=8`) — appear
//!   as `pub const CONFIG_OVE_PM_MAX_WAKE_SOURCES: i64 = 8;` plus a
//!   matching `_USIZE` const (only when non-negative) for use as an
//!   array size or const-generic parameter.
//! - **String symbols** (`CONFIG_OVE_APP_NAME="my-app"`) — appear as
//!   `pub const CONFIG_OVE_APP_NAME: &str = "my-app";`.
//!
//! Symbols that aren't set (`# CONFIG_OVE_FOO is not set`) are
//! absent — use `#[cfg(not(config_ove_foo))]` to detect them.
//!
//! ## Example
//!
//! ```ignore
//! const SLOTS: usize = ove::config::CONFIG_OVE_PM_MAX_WAKE_SOURCES_USIZE;
//! let mut wakers: [Option<Waker>; SLOTS] = [const { None }; SLOTS];
//!
//! #[cfg(config_ove_async)]
//! fn ensure_async_runtime() { /* ... */ }
//! ```
//!
//! Mirrors zephyr-lang-rust's `zephyr::kconfig::CONFIG_*` pattern; the
//! oveRTOS `has_<module>` / `rtos_<backend>` / `board_<name>` cfg
//! flags continue to coexist as their own facets.

// `OVE_CONFIG_CONSTS` is set by `build.rs` and points at a generated
// file in `OUT_DIR`. The file consists of `pub const CONFIG_OVE_*: …`
// lines — see `build.rs`'s G3 block.
include!(env!("OVE_CONFIG_CONSTS"));
