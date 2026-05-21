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
use quote::{format_ident, quote};
use syn::parse::{Parse, ParseStream};
use syn::{ItemFn, LitInt, LitStr, Token, parse_macro_input};

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
        // executor stalls forever.  Spawn the executor on an ove::Thread,
        // then hand control to ove::run() which starts the backend
        // scheduler.  Once the scheduler is up, the executor thread is
        // dispatched and Executor::run takes over, blocking on
        // ove_event_wait between polls — yielding cleanly to the RTOS
        // scheduler when other tasks share the CPU.
        fn __ove_async_exec_thread() {
            let exec: &'static mut ::ove::async_runtime::Executor =
                ::ove::async_runtime::Executor::take()
                    .expect("ove::async_runtime::Executor::take called twice");
            exec.run(|spawner| {
                spawner
                    .spawn(#renamed_ident(spawner))
                    .expect("failed to spawn application entry task");
            });
        }

        #[unsafe(no_mangle)]
        pub extern "C" fn ove_main() {
            // In heap mode, Thread::spawn_simple mallocs the kernel
            // TCB + stack.  In zero-heap mode, the caller supplies a
            // static ThreadStorage<STACK_SIZE> slot to spawn_static_simple.
            #[cfg(not(zero_heap))]
            let _handle = ::ove::Thread::builder()
                .name(c"async-exec")
                .stack_size(8192)
                .spawn_simple(__ove_async_exec_thread)
                .expect("failed to create async-exec thread");
            #[cfg(zero_heap)]
            let _handle = {
                static EXEC_THREAD_STORAGE: ::ove::ThreadStorage<8192> =
                    ::ove::ThreadStorage::new();
                ::ove::Thread::builder()
                    .name(c"async-exec")
                    .spawn_static_simple(&EXEC_THREAD_STORAGE, __ove_async_exec_thread)
                    .expect("failed to create async-exec thread")
            };
            // Intentionally leak the join handle so its Drop doesn't
            // request_stop on the executor thread when ove_main "returns"
            // (it won't — ove::run never returns, but be explicit).
            ::core::mem::forget(_handle);
            ::ove::run();
        }
    };
    expanded.into()
}

// ── #[ove::thread] (G2) ─────────────────────────────────────────────

/// Args parsed from `#[ove::thread(stack_size = 4096, name = "gfx")]`.
struct ThreadArgs {
    stack_size: Option<usize>,
    name: Option<String>,
}

impl Parse for ThreadArgs {
    fn parse(input: ParseStream) -> syn::Result<Self> {
        let mut stack_size = None;
        let mut name = None;
        while !input.is_empty() {
            let ident: syn::Ident = input.parse()?;
            input.parse::<Token![=]>()?;
            match ident.to_string().as_str() {
                "stack_size" => {
                    let lit: LitInt = input.parse()?;
                    stack_size = Some(lit.base10_parse::<usize>()?);
                }
                "name" => {
                    let lit: LitStr = input.parse()?;
                    name = Some(lit.value());
                }
                other => {
                    return Err(syn::Error::new_spanned(
                        ident,
                        format!(
                            "unknown #[ove::thread] argument `{other}` (expected stack_size or name)"
                        ),
                    ));
                }
            }
            if !input.is_empty() {
                input.parse::<Token![,]>()?;
            }
        }
        Ok(Self { stack_size, name })
    }
}

/// Attribute macro: declare a function as a thread entry-point with a
/// statically-allocated stack. Calling the generated wrapper spawns
/// the thread once; subsequent calls return `Err(Error::Inval)`
/// (the underlying [`ove::ThreadStorage`] is single-use).
///
/// ```ignore
/// #[ove::thread(stack_size = 4096, name = "blinker")]
/// fn blink() {
///     loop {
///         ove::Thread::sleep_ms(500);
///         ove::printk!("tick\n");
///     }
/// }
///
/// fn ove_main() {
///     blink().expect("spawn blinker");
///     ove::run();
/// }
/// ```
///
/// Generates:
/// - A `pub fn <name>() -> ove::Result<()>` that, on first call,
///   creates the thread with a `static ThreadStorage<STACK_SIZE>`.
/// - The user's original function body is moved into a hidden
///   `__ove_thread_<name>_body` so the public `<name>` is the spawn
///   helper.
///
/// Defaults: `stack_size = 4096`, `name = "ove-thread"` (override via
/// the attribute args). The pool size is fixed at 1 — at most one
/// live instance of the thread per program. Calling the spawn helper
/// from inside the thread body itself is allowed but does nothing
/// (returns `Err`); use [`ove::Thread::builder`] for dynamic spawns.
///
/// Works in both heap and zero-heap modes: the storage is `static` so
/// no allocation happens at spawn time.
#[proc_macro_attribute]
pub fn thread(attr: TokenStream, item: TokenStream) -> TokenStream {
    let args = parse_macro_input!(attr as ThreadArgs);
    let input = parse_macro_input!(item as ItemFn);

    if input.sig.asyncness.is_some() {
        return syn::Error::new_spanned(
            &input.sig.fn_token,
            "#[ove::thread] cannot annotate `async fn` — use #[embassy_executor::task] instead",
        )
        .to_compile_error()
        .into();
    }
    if !input.sig.inputs.is_empty() {
        return syn::Error::new_spanned(
            &input.sig.inputs,
            "#[ove::thread] entry function must take no arguments",
        )
        .to_compile_error()
        .into();
    }
    if !matches!(input.sig.output, syn::ReturnType::Default) {
        return syn::Error::new_spanned(
            &input.sig.output,
            "#[ove::thread] entry function must return `()` (drop the `-> ()`)",
        )
        .to_compile_error()
        .into();
    }

    let stack_size = args.stack_size.unwrap_or(4096);
    let user_fn = &input.sig.ident;
    let body_ident = format_ident!("__ove_thread_{}_body", user_fn);
    let name_str = args
        .name
        .unwrap_or_else(|| format!("ove-{user_fn}"))
        .replace('\0', "");
    let name_cstr = syn::LitByteStr::new(format!("{name_str}\0").as_bytes(), user_fn.span());

    let user_vis = &input.vis;
    let user_attrs = &input.attrs;
    let user_block = &input.block;

    let expanded = quote! {
        // The original function body, hidden as the actual thread entry.
        #[doc(hidden)]
        #(#user_attrs)*
        fn #body_ident() #user_block

        // The spawn helper takes the original ident so users call e.g.
        // `blink()` to start their thread.
        #user_vis fn #user_fn() -> ::ove::Result<()> {
            static STORAGE: ::ove::ThreadStorage<#stack_size> = ::ove::ThreadStorage::new();
            static STARTED: ::core::sync::atomic::AtomicBool =
                ::core::sync::atomic::AtomicBool::new(false);

            if STARTED.swap(true, ::core::sync::atomic::Ordering::AcqRel) {
                return Err(::ove::Error::Inval);
            }
            // SAFETY: the LitByteStr above is null-terminated.
            let name = unsafe {
                ::core::ffi::CStr::from_bytes_with_nul_unchecked(#name_cstr)
            };
            let handle = ::ove::Thread::builder()
                .name(name)
                .spawn_static_simple(&STORAGE, #body_ident)?;
            // Detach: the thread runs until the body returns; Drop on
            // the JoinHandle would request_stop which is wrong for a
            // user-managed thread.
            ::core::mem::forget(handle);
            Ok(())
        }
    };
    expanded.into()
}
