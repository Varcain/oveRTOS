// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Proc-macros for the oveRTOS Rust binding.
//!
//! Provides `#[ove::main]`, which marks an `fn` as the application
//! entry point.  Two shapes are accepted:
//!
//!   - **Sync**: `fn app_main()` — expands to a no-arg `extern "C" fn
//!     ove_main()` trampoline that calls the user function directly.
//!     This is the historical shape and works without the `async`
//!     feature.
//!   - **Async**: `async fn app_main(spawner: Spawner)` — expands to a
//!     trampoline that takes the global `Executor`, builds an
//!     `embassy_executor::task`, and runs the executor forever.
//!     Requires the `async` Cargo feature on the `ove` crate (and
//!     `CONFIG_OVE_ASYNC=y` on the C substrate, enforced by a
//!     `compile_error!` in `ove::lib.rs`).

use proc_macro::TokenStream;
use quote::quote;
use syn::{ItemFn, parse_macro_input};

/// Attribute macro: mark a function as the oveRTOS application entry
/// point.
///
/// Generates an `#[unsafe(no_mangle)] pub extern "C" fn ove_main()`
/// trampoline that calls the annotated function.
///
/// # Forms
///
/// Sync:
/// ```ignore
/// #[ove::main]
/// fn app_main() {
///     ove::log::try_init();
///     log::info!("hello");
///     ove::run();
/// }
/// ```
///
/// Async (requires `ove` feature `async`):
/// ```ignore
/// use embassy_executor::Spawner;
///
/// #[ove::main]
/// async fn app_main(spawner: Spawner) {
///     spawner.must_spawn(my_task());
///     // Executor::run() never returns; ove::run() is implicit.
/// }
/// ```
#[proc_macro_attribute]
pub fn main(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as ItemFn);

    if !matches!(input.sig.output, syn::ReturnType::Default) {
        return syn::Error::new_spanned(
            &input.sig.output,
            "#[ove::main] entry function must return `()` (drop the `-> ()`)",
        )
        .to_compile_error()
        .into();
    }

    if input.sig.asyncness.is_some() {
        return expand_async(input);
    }
    if !input.sig.inputs.is_empty() {
        return syn::Error::new_spanned(
            &input.sig.inputs,
            "#[ove::main] sync entry function must take no arguments \
             (use an `async fn(spawner: Spawner)` for async entry)",
        )
        .to_compile_error()
        .into();
    }
    expand_sync(input)
}

fn expand_sync(input: ItemFn) -> TokenStream {
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

fn expand_async(input: ItemFn) -> TokenStream {
    if input.sig.inputs.len() != 1 {
        return syn::Error::new_spanned(
            &input.sig.inputs,
            "#[ove::main] async entry function must take exactly one \
             argument of type `embassy_executor::Spawner`",
        )
        .to_compile_error()
        .into();
    }

    let user_fn_name = &input.sig.ident;
    // Rename the user fn to a task-decorated wrapper so it can be
    // spawned on the executor. The trampoline takes the executor (via
    // a function-local mutable slot upgraded to 'static lifetime) and
    // runs it forever.
    let renamed_ident = syn::Ident::new(
        &format!("__ove_async_entry_{user_fn_name}"),
        user_fn_name.span(),
    );

    let mut user_sig = input.sig.clone();
    user_sig.ident = renamed_ident.clone();
    let user_block = &input.block;
    let user_attrs = &input.attrs;
    let user_vis = &input.vis;

    let expanded = quote! {
        #(#user_attrs)*
        #[::embassy_executor::task]
        #user_vis #user_sig #user_block

        // The async executor must run after vTaskStartScheduler (FreeRTOS)
        // / pthread_create (POSIX) / k_thread_create (Zephyr) so the
        // backend's timer service task, ISR plumbing, and waitqueues are
        // alive — otherwise our ove_timer callback never fires and the
        // executor stalls in WFE forever.  Spawn the executor on an
        // ove::Thread, then hand control to ove::run() which starts the
        // backend scheduler.  Once the scheduler is up, the executor
        // thread is dispatched and Executor::run takes over.
        fn __ove_async_exec_thread() {
            let exec: &'static mut ::ove::async_runtime::Executor =
                ::ove::heap::Box::leak(::ove::heap::Box::new(
                    ::ove::async_runtime::Executor::new(),
                ));
            exec.run(|spawner| {
                spawner
                    .spawn(#renamed_ident(spawner))
                    .expect("failed to spawn application entry task");
            });
        }

        #[unsafe(no_mangle)]
        pub extern "C" fn ove_main() {
            let _handle = ::ove::Thread::builder()
                .name(c"async-exec")
                .stack_size(8192)
                .spawn_simple(__ove_async_exec_thread)
                .expect("failed to create async-exec thread");
            // Intentionally leak the join handle so its Drop doesn't
            // request_stop on the executor thread when ove_main "returns"
            // (it won't — ove::run never returns, but be explicit).
            ::core::mem::forget(_handle);
            ::ove::run();
        }
    };
    expanded.into()
}
