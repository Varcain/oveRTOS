// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! User-facing macros for the oveRTOS Rust SDK.
//!
//! All `#[macro_export]` macros are re-exported at crate root, so the
//! location of this file is purely organizational.

/// Generate `Debug`, `Drop`, and thread-safety impls for an oveRTOS handle
/// wrapper struct that stores a nullable handle in a field named `handle`.
///
/// # Variants
///
/// - `(Name, destroy_fn, deinit_fn)` — Send + Sync
/// - `(Name, destroy_fn, deinit_fn, send_only)` — Send only (no Sync)
/// - `(Name<const N: usize>, destroy_fn, deinit_fn)` — const-generic, Send + Sync
#[doc(hidden)]
#[macro_export]
macro_rules! ove_handle_impl {
    // Non-generic, Send + Sync
    ($name:ident, $destroy:ident, $deinit:ident) => {
        $crate::ove_handle_impl!(@debug $name);
        $crate::ove_handle_impl!(@drop $name, $destroy, $deinit);
        unsafe impl Send for $name {}
        unsafe impl Sync for $name {}
    };

    // Non-generic, Send only (no Sync)
    ($name:ident, $destroy:ident, $deinit:ident, send_only) => {
        $crate::ove_handle_impl!(@debug $name);
        $crate::ove_handle_impl!(@drop $name, $destroy, $deinit);
        unsafe impl Send for $name {}
    };

    // Const-generic, Send + Sync
    ($name:ident<const $N:ident: usize>, $destroy:ident, $deinit:ident) => {
        impl<const $N: usize> Drop for $name<$N> {
            fn drop(&mut self) {
                if self.handle.is_null() { return; }
                #[cfg(not(zero_heap))]
                unsafe { $crate::bindings::$destroy(self.handle) }
                #[cfg(zero_heap)]
                unsafe { $crate::bindings::$deinit(self.handle) }
            }
        }
        unsafe impl<const $N: usize> Send for $name<$N> {}
        unsafe impl<const $N: usize> Sync for $name<$N> {}
    };

    (@debug $name:ident) => {
        impl core::fmt::Debug for $name {
            fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
                f.debug_struct(stringify!($name))
                    .field("handle", &format_args!("{:p}", self.handle))
                    .finish()
            }
        }
    };

    (@drop $name:ident, $destroy:ident, $deinit:ident) => {
        impl Drop for $name {
            fn drop(&mut self) {
                if self.handle.is_null() { return; }
                #[cfg(not(zero_heap))]
                unsafe { $crate::bindings::$destroy(self.handle) }
                #[cfg(zero_heap)]
                unsafe { $crate::bindings::$deinit(self.handle) }
            }
        }
    };
}

/// Format arguments into a stack buffer and log via [`crate::log()`].
///
/// Uses a 128-byte stack buffer.  Output is silently truncated if it exceeds
/// the buffer capacity.
///
/// # Example
///
/// ```ignore
/// ove::log_fmt!("[I] count = {}\n", 42);
/// ```
#[macro_export]
macro_rules! log_fmt {
    ($($arg:tt)*) => {{
        use core::fmt::Write;
        let mut buf = [0u8; 128];
        let mut w = $crate::FmtBuf::new(&mut buf);
        let _ = write!(w, $($arg)*);
        $crate::log(w.as_bytes());
    }};
}

/// Internal helper: format a prefixed, newline-terminated log message and
/// write it to the console.  Uses a 256-byte stack buffer matching the C
/// `_OVE_LOG_OUTPUT` macro.
#[doc(hidden)]
#[macro_export]
macro_rules! _log_prefixed {
    ($prefix:expr, $($arg:tt)*) => {{
        use core::fmt::Write;
        let mut buf = [0u8; 256];
        let mut w = $crate::FmtBuf::new(&mut buf);
        let _ = write!(w, $prefix);
        let _ = write!(w, $($arg)*);
        let _ = write!(w, "\n");
        $crate::log(w.as_bytes());
    }};
}

/// Log an informational message with `[I]` prefix and automatic newline.
///
/// Uses a 256-byte stack buffer.  Messages longer than ~250 characters
/// (after prefix and newline) are silently truncated.
///
/// Produces the same console output as the C `OVE_LOG_INF` macro.
///
/// # Example
///
/// ```ignore
/// ove::log_inf!("Consumer: count = {}", val);
/// // Output: [I] Consumer: count = 42\n
/// ```
#[macro_export]
macro_rules! log_inf {
    ($($arg:tt)*) => { $crate::_log_prefixed!("[I] ", $($arg)*) };
}

/// Log a warning message with `[W]` prefix and automatic newline.
///
/// Uses a 256-byte stack buffer.  Messages longer than ~250 characters
/// (after prefix and newline) are silently truncated.
///
/// Produces the same console output as the C `OVE_LOG_WRN` macro.
#[macro_export]
macro_rules! log_wrn {
    ($($arg:tt)*) => { $crate::_log_prefixed!("[W] ", $($arg)*) };
}

/// Log an error message with `[E]` prefix and automatic newline.
///
/// Uses a 256-byte stack buffer.  Messages longer than ~250 characters
/// (after prefix and newline) are silently truncated.
///
/// Produces the same console output as the C `OVE_LOG_ERR` macro.
#[macro_export]
macro_rules! log_err {
    ($($arg:tt)*) => { $crate::_log_prefixed!("[E] ", $($arg)*) };
}

/// Declare a safe accessor for TFLite model data generated by `convert.py`.
///
/// Generates the `extern "C"` linkage for the `_model_data` and
/// `_model_data_len` symbols and wraps them in a function returning
/// `&'static [u8]`.  No `extern` or `unsafe` needed in application code.
///
/// # Example
///
/// ```ignore
/// ove::model_data!(preprocessor_model,
///     g_audio_preprocessor_int8_model_data,
///     g_audio_preprocessor_int8_model_data_len);
///
/// let data: &[u8] = preprocessor_model();
/// ```
#[macro_export]
macro_rules! model_data {
    ($fn_name:ident, $data_sym:ident, $len_sym:ident) => {
        unsafe extern "C" {
            safe static $data_sym: u8;
            safe static $len_sym: u32;
        }
        fn $fn_name() -> &'static [u8] {
            unsafe { core::slice::from_raw_parts(&$data_sym, $len_sym as usize) }
        }
    };
}

/// Generate the `ove_main` entry point from a Rust closure or function.
///
/// # Example
///
/// ```ignore
/// ove::main!(app_main);
///
/// fn app_main() {
///     // create resources...
///     ove::run();
/// }
/// ```
///
/// # Object lifetime
///
/// Anything that worker threads access after `app_main` returns must have
/// `'static` storage — use [`crate::shared!`] / [`crate::shared_mut!`] or
/// plain `static` cells.  A stack-local value in `app_main` is dropped
/// when the function unwinds; a thread that kept a reference into it
/// then points at freed memory.  Same rule the Rust borrow checker
/// enforces when you try to return `&T` to a local — it's just less
/// visible here because workers, not callers, hold the dangling
/// reference.  On FreeRTOS the failure is immediate (scheduler
/// reclaims the main stack); on POSIX/NuttX/Zephyr the UB is latent.
#[macro_export]
macro_rules! main {
    ($entry:expr) => {
        #[unsafe(no_mangle)]
        pub extern "C" fn ove_main() {
            $entry();
        }
    };
}

// ---------------------------------------------------------------------------
// Unified creation macros
//
// These macros work in both heap and zero-heap modes. In heap mode they call
// `new()`. In zero-heap mode they declare function-local `static` storage
// and call `from_static()`.
// ---------------------------------------------------------------------------

/// Create a [`crate::Mutex`] that works in both heap and zero-heap modes.
#[cfg(has_sync)]
#[macro_export]
macro_rules! mutex {
    () => {{
        #[cfg(not(zero_heap))]
        {
            $crate::Mutex::new().unwrap()
        }
        #[cfg(zero_heap)]
        {
            static mut _S: $crate::ffi::ove_mutex_storage_t = unsafe { core::mem::zeroed() };
            unsafe { $crate::Mutex::from_static(core::ptr::addr_of_mut!(_S)) }.unwrap()
        }
    }};
}

/// Create a [`crate::RecursiveMutex`] that works in both heap and zero-heap modes.
#[cfg(has_sync)]
#[macro_export]
macro_rules! recursive_mutex {
    () => {{
        #[cfg(not(zero_heap))]
        {
            $crate::RecursiveMutex::new().unwrap()
        }
        #[cfg(zero_heap)]
        {
            static mut _S: $crate::ffi::ove_mutex_storage_t = unsafe { core::mem::zeroed() };
            unsafe { $crate::RecursiveMutex::from_static(core::ptr::addr_of_mut!(_S)) }.unwrap()
        }
    }};
}

/// Create a [`crate::Semaphore`] that works in both heap and zero-heap modes.
#[cfg(has_sync)]
#[macro_export]
macro_rules! semaphore {
    ($initial:expr, $max:expr) => {{
        #[cfg(not(zero_heap))]
        {
            $crate::Semaphore::new($initial, $max).unwrap()
        }
        #[cfg(zero_heap)]
        {
            static mut _S: $crate::ffi::ove_sem_storage_t = unsafe { core::mem::zeroed() };
            unsafe { $crate::Semaphore::from_static(core::ptr::addr_of_mut!(_S), $initial, $max) }
                .unwrap()
        }
    }};
}

/// Create an [`crate::Event`] that works in both heap and zero-heap modes.
#[cfg(has_sync)]
#[macro_export]
macro_rules! event {
    () => {{
        #[cfg(not(zero_heap))]
        {
            $crate::Event::new().unwrap()
        }
        #[cfg(zero_heap)]
        {
            static mut _S: $crate::ffi::ove_event_storage_t = unsafe { core::mem::zeroed() };
            unsafe { $crate::Event::from_static(core::ptr::addr_of_mut!(_S)) }.unwrap()
        }
    }};
}

/// Create a [`crate::CondVar`] that works in both heap and zero-heap modes.
#[cfg(has_sync)]
#[macro_export]
macro_rules! condvar {
    () => {{
        #[cfg(not(zero_heap))]
        {
            $crate::CondVar::new().unwrap()
        }
        #[cfg(zero_heap)]
        {
            static mut _S: $crate::ffi::ove_condvar_storage_t = unsafe { core::mem::zeroed() };
            unsafe { $crate::CondVar::from_static(core::ptr::addr_of_mut!(_S)) }.unwrap()
        }
    }};
}

/// Create an [`crate::EventGroup`] that works in both heap and zero-heap modes.
#[cfg(has_eventgroup)]
#[macro_export]
macro_rules! eventgroup {
    () => {{
        #[cfg(not(zero_heap))]
        {
            $crate::EventGroup::new().unwrap()
        }
        #[cfg(zero_heap)]
        {
            static mut _S: $crate::ffi::ove_eventgroup_storage_t = unsafe { core::mem::zeroed() };
            unsafe { $crate::EventGroup::from_static(core::ptr::addr_of_mut!(_S)) }.unwrap()
        }
    }};
}

/// Create a [`crate::audio::Graph`] that works in both heap and zero-heap modes.
///
/// Mirrors the C `ove_audio_graph_create(pg, frames, nodes, channels,
/// sample_bytes)` macro: in zero-heap mode emits a per-call-site `static`
/// backing array sized by `nodes * frames * channels * sample_bytes` bytes
/// and attaches it to the graph automatically; in heap mode just calls
/// [`crate::audio::Graph::new`].  Returns [`crate::Result`].
///
/// All arguments must be constant expressions.
///
/// # Example
/// ```ignore
/// // 2 non-sink nodes (source + processor), 512 frames, mono, S16 (2 bytes).
/// let mut graph = ove::audio_graph!(512, 2, 1, 2)?;
/// ```
#[cfg(has_audio)]
#[macro_export]
macro_rules! audio_graph {
    ($frames:expr, $nodes:expr, $channels:expr, $sample_bytes:expr) => {{
        #[cfg(not(zero_heap))]
        {
            $crate::audio::Graph::new($frames)
        }
        #[cfg(zero_heap)]
        {
            const _STORAGE_BYTES: usize =
                ($nodes) * ($frames) as usize * ($channels) * ($sample_bytes);
            #[repr(C, align(4))]
            struct _AudioBufStorage([u8; _STORAGE_BYTES]);
            static mut _AUDIO_BUF: _AudioBufStorage = _AudioBufStorage([0; _STORAGE_BYTES]);
            let slice: &'static mut [u8] = unsafe {
                core::slice::from_raw_parts_mut(
                    core::ptr::addr_of_mut!(_AUDIO_BUF) as *mut u8,
                    _STORAGE_BYTES,
                )
            };
            $crate::audio::Graph::new_with_storage($frames, slice)
        }
    }};
}

/// Create a [`crate::Queue`] that works in both heap and zero-heap modes.
///
/// # Example
/// ```ignore
/// let q = ove::queue!(u32, 8);
/// ```
#[cfg(has_queue)]
#[macro_export]
macro_rules! queue {
    ($T:ty, $N:expr) => {{
        #[cfg(not(zero_heap))]
        {
            $crate::Queue::<$T, $N>::new().unwrap()
        }
        #[cfg(zero_heap)]
        {
            static mut _S: $crate::ffi::ove_queue_storage_t = unsafe { core::mem::zeroed() };
            static mut _B: [core::mem::MaybeUninit<$T>; $N] =
                unsafe { core::mem::MaybeUninit::uninit().assume_init() };
            unsafe {
                $crate::Queue::<$T, $N>::from_static(
                    core::ptr::addr_of_mut!(_S),
                    core::ptr::addr_of_mut!(_B) as *mut _,
                )
            }
            .unwrap()
        }
    }};
}

/// Create a [`crate::Timer`] that works in both heap and zero-heap modes.
///
/// # Example
/// ```ignore
/// let t = ove::timer!(my_callback, 100, false);
/// ```
#[cfg(has_timer)]
#[macro_export]
macro_rules! timer {
    ($callback:expr, $period_ms:expr, $one_shot:expr) => {{
        #[cfg(not(zero_heap))]
        {
            $crate::Timer::new($callback, $period_ms, $one_shot).unwrap()
        }
        #[cfg(zero_heap)]
        {
            static mut _S: $crate::ffi::ove_timer_storage_t = unsafe { core::mem::zeroed() };
            unsafe {
                $crate::Timer::from_static(
                    core::ptr::addr_of_mut!(_S),
                    $callback,
                    $period_ms,
                    $one_shot,
                )
            }
            .unwrap()
        }
    }};
}

/// Create a [`crate::Thread`] that works in both heap and zero-heap modes.
///
/// Uses the safe `fn()` entry pattern (trampoline). The name is
/// automatically null-terminated.
///
/// # Example
/// ```ignore
/// let t = ove::thread!("worker", my_entry, Priority::Normal, 4096);
/// ```
#[macro_export]
macro_rules! thread {
    ($name:expr, $entry:expr, $prio:expr, $stack:expr) => {{
        #[cfg(not(zero_heap))]
        {
            $crate::Thread::spawn(concat!($name, "\0").as_bytes(), $entry, $prio, $stack).unwrap()
        }
        #[cfg(all(zero_heap, not(rtos_zephyr)))]
        {
            static mut _S: $crate::ffi::ove_thread_storage_t = unsafe { core::mem::zeroed() };
            // Align to 8 bytes (ARM AAPCS stack alignment requirement).
            #[repr(C, align(8))]
            struct AlignedStack([u8; $stack]);
            static mut _STACK: AlignedStack = AlignedStack([0u8; $stack]);
            unsafe {
                $crate::Thread::spawn_static(
                    core::ptr::addr_of_mut!(_S),
                    core::ptr::addr_of_mut!(_STACK) as *mut _,
                    concat!($name, "\0").as_bytes(),
                    $entry,
                    $prio,
                    $stack,
                )
            }
            .unwrap()
        }
        #[cfg(all(zero_heap, rtos_zephyr))]
        {
            static mut _S: $crate::ffi::ove_thread_storage_t = unsafe { core::mem::zeroed() };
            // Zephyr with MPU needs power-of-2 aligned stacks.
            // Add MPU guard region (128 bytes for FPU), round total
            // to next power of 2. align(8192) covers stacks up to
            // ~4000 usable bytes (the common embedded case).
            const _STACK_TOTAL: usize = ($stack + 128usize).next_power_of_two();
            #[repr(C, align(8192))]
            struct ZStack([u8; _STACK_TOTAL]);
            static mut _STACK: ZStack = ZStack([0u8; _STACK_TOTAL]);
            unsafe {
                $crate::Thread::spawn_static(
                    core::ptr::addr_of_mut!(_S),
                    core::ptr::addr_of_mut!(_STACK) as *mut _,
                    concat!($name, "\0").as_bytes(),
                    $entry,
                    $prio,
                    $stack,
                )
            }
            .unwrap()
        }
    }};
}

/// Create a [`crate::Stream`] that works in both heap and zero-heap modes.
///
/// # Example
/// ```ignore
/// let s = ove::stream!(256, 1);
/// ```
#[cfg(has_stream)]
#[macro_export]
macro_rules! stream {
    ($N:expr, $trigger:expr) => {{
        #[cfg(not(zero_heap))]
        {
            $crate::Stream::<$N>::new($trigger).unwrap()
        }
        #[cfg(zero_heap)]
        {
            static mut _S: $crate::ffi::ove_stream_storage_t = unsafe { core::mem::zeroed() };
            static mut _B: [u8; $N] = [0u8; $N];
            unsafe {
                $crate::Stream::<$N>::from_static(
                    core::ptr::addr_of_mut!(_S),
                    core::ptr::addr_of_mut!(_B) as *mut _,
                    $trigger,
                )
            }
            .unwrap()
        }
    }};
}

/// Create a [`crate::Workqueue`] that works in both heap and zero-heap modes.
///
/// # Example
/// ```ignore
/// let wq = ove::workqueue!("myq", Priority::Normal, 4096);
/// ```
#[cfg(has_workqueue)]
#[macro_export]
macro_rules! workqueue {
    ($name:expr, $prio:expr, $stack:expr) => {{
        #[cfg(not(zero_heap))]
        {
            $crate::Workqueue::new(concat!($name, "\0").as_bytes(), $prio, $stack).unwrap()
        }
        #[cfg(zero_heap)]
        {
            static mut _S: $crate::ffi::ove_workqueue_storage_t = unsafe { core::mem::zeroed() };
            static mut _STACK: [u8; $stack] = [0u8; $stack];
            unsafe {
                $crate::Workqueue::from_static(
                    core::ptr::addr_of_mut!(_S),
                    concat!($name, "\0").as_bytes(),
                    $prio,
                    $stack,
                    core::ptr::addr_of_mut!(_STACK) as *mut _,
                )
            }
            .unwrap()
        }
    }};
}

/// Wrap a safe Rust `fn()` into an `ove_work_fn` C trampoline.
///
/// The oveRTOS C work handler has no `user_data` slot, so this just
/// generates a `unsafe extern "C"` trampoline that calls the supplied
/// safe function. Use this with [`crate::work!`] so app code never has to write
/// `unsafe extern "C"` itself.
///
/// # Example
/// ```ignore
/// fn on_work() { /* ... */ }
/// let w = ove::work!(ove::work_handler!(on_work));
/// ```
#[cfg(has_workqueue)]
#[macro_export]
macro_rules! work_handler {
    ($f:ident) => {{
        unsafe extern "C" fn _tramp(_w: $crate::ffi::ove_work_t) {
            $f();
        }
        Some(_tramp as unsafe extern "C" fn($crate::ffi::ove_work_t))
    }};
}

/// Create a [`crate::Work`] item that works in both heap and zero-heap modes.
///
/// The handler must be an `ove_work_fn` (an `unsafe extern "C" fn`). To
/// avoid writing that yourself, wrap a safe `fn()` with [`work_handler!`].
#[cfg(has_workqueue)]
#[macro_export]
macro_rules! work {
    ($handler:expr) => {{
        #[cfg(not(zero_heap))]
        {
            $crate::Work::new($handler).unwrap()
        }
        #[cfg(zero_heap)]
        {
            static mut _S: $crate::ffi::ove_work_storage_t = unsafe { core::mem::zeroed() };
            unsafe { $crate::Work::from_static(core::ptr::addr_of_mut!(_S), $handler) }.unwrap()
        }
    }};
}

/// Create a [`crate::Watchdog`] that works in both heap and zero-heap modes.
#[cfg(has_watchdog)]
#[macro_export]
macro_rules! watchdog {
    ($timeout_ms:expr) => {{
        #[cfg(not(zero_heap))]
        {
            $crate::Watchdog::new($timeout_ms).unwrap()
        }
        #[cfg(zero_heap)]
        {
            static mut _S: $crate::ffi::ove_watchdog_storage_t = unsafe { core::mem::zeroed() };
            unsafe { $crate::Watchdog::from_static(core::ptr::addr_of_mut!(_S), $timeout_ms) }
                .unwrap()
        }
    }};
}

/// Declare a `static` wrapped in [`crate::StaticCell`] for cross-thread shared state.
///
/// # Example
/// ```ignore
/// ove::shared!(QUEUE: Queue<u32, 8>);
/// // then use QUEUE.send(...) directly — Deref eliminates .get()
/// ```
#[macro_export]
macro_rules! shared {
    ($vis:vis $name:ident : $ty:ty) => {
        $vis static $name: $crate::StaticCell<$ty> = $crate::StaticCell::new();
    };
}

/// Declare a `static` wrapped in [`crate::StaticMut`] for single-owner mutable state.
///
/// # Example
/// ```ignore
/// ove::shared_mut!(ENGINE: DspEngine);
/// ```
#[macro_export]
macro_rules! shared_mut {
    ($vis:vis $name:ident : $ty:ty) => {
        $vis static $name: $crate::StaticMut<$ty> = $crate::StaticMut::new();
    };
}

///
/// Bundles a `&'static StaticCell<T>` of shared state with a safe
/// `fn(&T, EventCtx)` callback. Pass the resulting static to
/// [`EventTarget::on_with`](crate::lvgl::EventTarget::on_with) or
/// [`EventTarget::on_clicked_with`](crate::lvgl::EventTarget::on_clicked_with).
///
/// # Example
/// ```ignore
/// struct NavState { page: i32 }
/// ove::shared!(NAV: ove::lvgl::LvCell<NavState>);
/// fn on_next(state: &ove::lvgl::LvCell<NavState>, _e: ove::lvgl::EventCtx<'_>) {
///     state.update(|s| NavState { page: s.page + 1 });
/// }
/// ove::event_handler!(NAV_NEXT: LvCell<NavState> = &NAV, on_next);
/// // ...later: button.on_clicked_with(&NAV_NEXT);
/// ```
#[cfg(has_lvgl)]
#[macro_export]
macro_rules! event_handler {
    ($vis:vis $name:ident : $t:ty = $cell:expr, $fn:expr) => {
        $vis static $name: $crate::lvgl::EventHandler<$t> =
            $crate::lvgl::EventHandler::new($cell, $fn);
    };
}
