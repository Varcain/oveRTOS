// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Proc-macros for the oveRTOS Rust binding.
//!
//! Currently provides only `#[ove::main]`, which marks an `fn` as the
//! application entry point.  Expands to an `extern "C" fn ove_main()`
//! trampoline that calls the annotated function — matches the shape
//! of `#[tokio::main]` / `#[embassy_executor::main]` from the
//! Rust async ecosystem.

use proc_macro::TokenStream;
use quote::quote;
use syn::{ItemFn, parse_macro_input};

/// Attribute macro: mark a function as the oveRTOS application entry
/// point.
///
/// Generates an `#[unsafe(no_mangle)] pub extern "C" fn ove_main()`
/// trampoline that calls the annotated function.  Replaces the
/// legacy `ove::main!(fn_name)` declarative macro.
///
/// # Constraints
/// The annotated function must have signature `fn() -> ()` (no
/// arguments, no return value).  Async signatures are reserved for a
/// future async-executor integration and currently rejected.
///
/// # Example
///
/// ```ignore
/// #[ove::main]
/// fn app_main() {
///     ove::log::try_init();
///     log::info!("hello");
///     ove::run();
/// }
/// ```
#[proc_macro_attribute]
pub fn main(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as ItemFn);

    // Validate: must be `fn` (not async), must take no args, must
    // return `()` (or be `-> ()` which parses as Default for ReturnType).
    if input.sig.asyncness.is_some() {
        return syn::Error::new_spanned(
            &input.sig,
            "#[ove::main] does not support async fn (reserved for future async-executor integration)",
        )
        .to_compile_error()
        .into();
    }
    if !input.sig.inputs.is_empty() {
        return syn::Error::new_spanned(
            &input.sig.inputs,
            "#[ove::main] entry function must take no arguments",
        )
        .to_compile_error()
        .into();
    }
    if !matches!(input.sig.output, syn::ReturnType::Default) {
        return syn::Error::new_spanned(
            &input.sig.output,
            "#[ove::main] entry function must return `()` (write the body, drop the `-> ()`)",
        )
        .to_compile_error()
        .into();
    }

    let user_fn_name = &input.sig.ident;
    let expanded = quote! {
        #input

        #[unsafe(no_mangle)]
        pub extern "C" fn ove_main() {
            #user_fn_name();
        }
    };
    expanded.into()
}
