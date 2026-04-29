// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Bench-app-local macros: `bench_case!`, `bench_suite!`,
//! `__count_bench_cases!`.  Used to live in
//! `bindings/rust/ove/src/macros.rs` (re-exported as `ove::bench_case!`
//! etc.); moved here so the binding crate no longer carries any
//! benchmark plumbing.  Call sites use the unqualified macro path
//! (`bench_case!`) since `#[macro_export]` always exports at the
//! defining crate's root.

/// Declare a static [`bench::CBenchCase`](crate::bench::CBenchCase) from a
/// safe Rust `fn()` run (and optional setup/teardown) callback.
///
/// The macro emits module-scope `unsafe extern "C"` trampolines inside a
/// const-block initializer — so no extra symbols leak into the user's
/// namespace and the app never writes `unsafe extern "C"` itself.
///
/// # Example
/// ```ignore
/// fn time_get_us_overhead_run() { let _ = ove::time::get_us(); }
/// bench_case!(static TIME_GET_US_OVERHEAD: BenchCase = {
///     name: b"time_get_us_overhead\0",
///     kind: BenchType::Latency,
///     run: time_get_us_overhead_run,
/// });
/// ```
#[macro_export]
macro_rules! bench_case {
    ($vis:vis static $name:ident : BenchCase = {
        name: $byte_name:expr,
        kind: $kind:expr,
        run: $run:ident
        $(, setup: $setup:ident)?
        $(, teardown: $teardown:ident)?
        $(, iterations: $iter:expr)?
        $(, inner_iters: $inner:expr)?
        $(,)?
    }) => {
        $vis static $name: $crate::bench::CBenchCase = {
            unsafe extern "C" fn __run_tramp(_ctx: *mut core::ffi::c_void) { $run() }
            $(unsafe extern "C" fn __setup_tramp(_ctx: *mut core::ffi::c_void) { $setup() })?
            $(unsafe extern "C" fn __teardown_tramp(_ctx: *mut core::ffi::c_void) { $teardown() })?

            // Construct Option<fn> values — the `$(... $meta ...)?` groups
            // must include the metavar, so we use a no-op type annotation
            // referencing it to tie the repetition depth.
            let setup: Option<unsafe extern "C" fn(*mut core::ffi::c_void)> = {
                let s: Option<unsafe extern "C" fn(*mut core::ffi::c_void)> = None;
                $( let s: Option<unsafe extern "C" fn(*mut core::ffi::c_void)> = {
                    let _ = stringify!($setup);
                    Some(__setup_tramp)
                }; )?
                s
            };
            let teardown: Option<unsafe extern "C" fn(*mut core::ffi::c_void)> = {
                let t: Option<unsafe extern "C" fn(*mut core::ffi::c_void)> = None;
                $( let t: Option<unsafe extern "C" fn(*mut core::ffi::c_void)> = {
                    let _ = stringify!($teardown);
                    Some(__teardown_tramp)
                }; )?
                t
            };
            let iterations: u32 = {
                let i: u32 = 0;
                $( let i: u32 = $iter; )?
                i
            };
            let inner_iters: u32 = {
                let n: u32 = 0;
                $( let n: u32 = $inner; )?
                n
            };

            $crate::bench::CBenchCase {
                name: $byte_name.as_ptr() as *const core::ffi::c_char,
                bench_type: $kind,
                setup,
                run: Some(__run_tramp),
                teardown,
                iterations,
                inner_iters,
            }
        };
    };
}

/// Declare a `#[no_mangle] pub static <symbol>: CBenchSuite` that
/// aggregates the given bench cases and exposes them to the C harness.
///
/// `symbol` must exactly match the `extern const bench_suite_t` name the
/// C side expects (typically `bench_suite_<name>`).
///
/// `enabled` is a safe `fn() -> bool` — the macro wraps it in a C-ABI
/// trampoline.
///
/// # Example
/// ```ignore
/// fn time_is_enabled() -> bool { true }
/// bench_suite!(
///     symbol = bench_suite_time,
///     name = b"time\0",
///     enabled = time_is_enabled,
///     cases = [TIME_GET_US_OVERHEAD, DELAY_1MS],
/// );
/// ```
#[doc(hidden)]
#[macro_export]
macro_rules! __count_bench_cases {
    () => { 0usize };
    ($head:ident $(, $tail:ident)*) => {
        1usize + $crate::__count_bench_cases!($($tail),*)
    };
}

#[macro_export]
macro_rules! bench_suite {
    (
        symbol = $symbol:ident,
        name = $name_bytes:expr,
        enabled = $enabled_fn:ident,
        cases = [ $( $case:ident ),* $(,)? ] $(,)?
    ) => {
        #[unsafe(no_mangle)]
        #[allow(non_upper_case_globals)]
        pub static $symbol: $crate::bench::CBenchSuite = {
            const CASE_COUNT: usize = $crate::__count_bench_cases!($($case),*);
            static CASES: [$crate::bench::CBenchCase; CASE_COUNT] = [ $( $case ),* ];
            unsafe extern "C" fn __enabled_tramp() -> i32 {
                if $enabled_fn() { 1 } else { 0 }
            }
            $crate::bench::CBenchSuite {
                name: $name_bytes.as_ptr() as *const core::ffi::c_char,
                is_enabled: Some(__enabled_tramp),
                cases: CASES.as_ptr(),
                case_count: CASE_COUNT as u32,
            }
        };
    };
}
