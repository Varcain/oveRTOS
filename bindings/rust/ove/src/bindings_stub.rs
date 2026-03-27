// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Stub FFI bindings for docs.rs builds.
//!
//! This file is used in place of the bindgen-generated `ove_bindings.rs`
//! when building documentation on docs.rs (detected via `DOCS_RS` env var
//! and the `docsrs` cfg flag). It declares all types, constants, and
//! `extern "C"` function signatures used by the Rust wrappers so that
//! `cargo doc` succeeds without a real C toolchain or LVGL headers.

#![allow(non_upper_case_globals, non_camel_case_types, non_snake_case, dead_code)]

use core::ffi::c_void;

// ---------------------------------------------------------------------------
// Primitive aliases
// ---------------------------------------------------------------------------

pub type c_int   = i32;
pub type c_uint  = u32;
pub type c_ulong = u64;

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

pub const OVE_OK:                i32 = 0;
pub const OVE_ERR_NOT_REGISTERED: i32 = -1;
pub const OVE_ERR_INVALID_PARAM: i32 = -2;
pub const OVE_ERR_NO_MEMORY:     i32 = -3;
pub const OVE_ERR_TIMEOUT:       i32 = -4;
pub const OVE_ERR_NOT_SUPPORTED: i32 = -5;
pub const OVE_ERR_QUEUE_FULL:    i32 = -6;

pub const OVE_WAIT_FOREVER: u32 = u32::MAX;

// ---------------------------------------------------------------------------
// Thread state constants
// ---------------------------------------------------------------------------

pub const OVE_THREAD_STATE_RUNNING:    u32 = 0;
pub const OVE_THREAD_STATE_READY:      u32 = 1;
pub const OVE_THREAD_STATE_BLOCKED:    u32 = 2;
pub const OVE_THREAD_STATE_SUSPENDED:  u32 = 3;
pub const OVE_THREAD_STATE_TERMINATED: u32 = 4;
pub const OVE_THREAD_STATE_UNKNOWN:    u32 = 5;

// ---------------------------------------------------------------------------
// Thread priority constants
// ---------------------------------------------------------------------------

pub const OVE_PRIO_IDLE:         u32 = 0;
pub const OVE_PRIO_LOW:          u32 = 1;
pub const OVE_PRIO_BELOW_NORMAL: u32 = 2;
pub const OVE_PRIO_NORMAL:       u32 = 3;
pub const OVE_PRIO_ABOVE_NORMAL: u32 = 4;
pub const OVE_PRIO_HIGH:         u32 = 5;
pub const OVE_PRIO_REALTIME:     u32 = 6;
pub const OVE_PRIO_CRITICAL:     u32 = 7;

// ---------------------------------------------------------------------------
// Filesystem flags
// ---------------------------------------------------------------------------

pub const OVE_FS_O_READ:   u32 = 0x01;
pub const OVE_FS_O_WRITE:  u32 = 0x02;
pub const OVE_FS_O_CREATE: u32 = 0x04;
pub const OVE_FS_O_APPEND: u32 = 0x08;

pub const OVE_FS_SEEK_SET: u32 = 0;
pub const OVE_FS_SEEK_CUR: u32 = 1;
pub const OVE_FS_SEEK_END: u32 = 2;

// ---------------------------------------------------------------------------
// Event group flags
// ---------------------------------------------------------------------------

pub const OVE_EG_WAIT_ALL:      u32 = 0x01;
pub const OVE_EG_CLEAR_ON_EXIT: u32 = 0x02;

// ---------------------------------------------------------------------------
// Opaque handle types
// ---------------------------------------------------------------------------

pub type ove_thread_t     = *mut c_void;
pub type ove_mutex_t      = *mut c_void;
pub type ove_sem_t        = *mut c_void;
pub type ove_event_t      = *mut c_void;
pub type ove_condvar_t    = *mut c_void;
pub type ove_eventgroup_t = *mut c_void;
pub type ove_workqueue_t  = *mut c_void;
pub type ove_work_t       = *mut c_void;
pub type ove_stream_t     = *mut c_void;
pub type ove_watchdog_t   = *mut c_void;
pub type ove_file_t       = *mut c_void;
pub type ove_dir_t        = *mut c_void;
pub type ove_queue_t      = *mut c_void;
pub type ove_timer_t      = *mut c_void;

pub type ove_eventbits_t = u32;

// ---------------------------------------------------------------------------
// Storage types — opaque blobs large enough for any backend
// ---------------------------------------------------------------------------

#[repr(C)]
pub struct ove_mutex_storage_t      { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_sem_storage_t        { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_event_storage_t      { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_condvar_storage_t    { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_thread_storage_t     { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_queue_storage_t      { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_timer_storage_t      { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_eventgroup_storage_t { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_workqueue_storage_t  { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_work_storage_t       { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_stream_storage_t     { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_watchdog_storage_t   { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_file_storage_t       { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_dir_storage_t        { _opaque: [u8; 256] }

// ---------------------------------------------------------------------------
// Struct types
// ---------------------------------------------------------------------------

#[repr(C)]
pub struct ove_thread_stats {
    pub runtime_us:       u64,
    pub cpu_percent_x100: u32,
}

#[repr(C)]
pub struct ove_thread_desc {
    pub name:       *const core::ffi::c_char,
    pub entry:      Option<unsafe extern "C" fn(*mut c_void)>,
    pub arg:        *mut c_void,
    pub priority:   u32,
    pub stack_size: usize,
    pub stack:      *mut c_void,
}

#[repr(C)]
pub struct ove_dirent {
    pub name:   [core::ffi::c_char; 256],
    pub size:   u32,
    pub is_dir: i32,
}

#[repr(C)]
pub struct ove_audio_fmt {
    pub sample_rate: u32,
    pub channels:    u32,
    pub sample_fmt:  u32,
}

#[repr(C)]
pub struct ove_audio_buf {
    pub data:   *mut c_void,
    pub frames: u32,
    pub fmt:    *const ove_audio_fmt,
}

#[repr(C)]
pub struct ove_audio_node_ops {
    pub configure: Option<unsafe extern "C" fn(*mut c_void, *const ove_audio_fmt, *mut ove_audio_fmt) -> i32>,
    pub start:     Option<unsafe extern "C" fn(*mut c_void) -> i32>,
    pub stop:      Option<unsafe extern "C" fn(*mut c_void) -> i32>,
    pub process:   Option<unsafe extern "C" fn(*mut c_void, *const ove_audio_buf, *mut ove_audio_buf) -> i32>,
    pub destroy:   Option<unsafe extern "C" fn(*mut c_void)>,
}

#[repr(C)]
pub struct ove_audio_node {
    pub name:    *const core::ffi::c_char,
    pub type_:   u32,
    pub ops:     *const ove_audio_node_ops,
    pub ctx:     *mut c_void,
    pub out_fmt: ove_audio_fmt,
}

#[repr(C)]
pub struct ove_audio_edge {
    pub from: u32,
    pub to:   u32,
}

#[repr(C)]
pub struct ove_audio_graph_stats {
    pub cycles:         u32,
    pub underruns:      u32,
    pub overruns:       u32,
    pub node_errors:    u32,
    pub max_process_us: u32,
    pub avg_process_us: u32,
}

#[repr(C)]
pub struct ove_audio_graph {
    pub nodes:            [ove_audio_node; 16],
    pub node_count:       u32,
    pub edges:            [ove_audio_edge; 16],
    pub edge_count:       u32,
    pub exec_order:       [u32; 16],
    pub exec_count:       u32,
    pub buffers:          [ove_audio_buf; 16],
    pub buf_storage:      *mut c_void,
    pub frames_per_period: u32,
    pub state:            u32,
    pub stats:            ove_audio_graph_stats,
}

#[repr(C)]
pub struct ove_audio_device_cfg {
    pub transport:        u32,
    pub fmt:              ove_audio_fmt,
    pub num_buffers:      u32,
    pub thread_priority:  u32,
    pub thread_stack_size: u32,
    pub transport_cfg:    [u8; 16], /* union: i2s/pdm/sdl2 */
}

#[repr(C)]
pub struct ove_shell_cmd {
    pub name:    *const core::ffi::c_char,
    pub help:    *const core::ffi::c_char,
    pub handler: Option<unsafe extern "C" fn(i32, *const *const core::ffi::c_char)>,
}

// ---------------------------------------------------------------------------
// Function pointer types
// ---------------------------------------------------------------------------

pub type ove_thread_fn      = Option<unsafe extern "C" fn(*mut c_void)>;
pub type ove_timer_fn       = Option<unsafe extern "C" fn(ove_timer_t, *mut c_void)>;
pub type ove_work_fn        = Option<unsafe extern "C" fn(ove_work_t)>;
/* ove_audio_process_fn removed — replaced by graph node vtable */
pub type ove_gpio_irq_cb    = Option<unsafe extern "C" fn(u32, u32, *mut c_void)>;
pub type ove_shell_cmd_fn   = Option<unsafe extern "C" fn(i32, *const *const core::ffi::c_char)>;

// ---------------------------------------------------------------------------
// extern "C" — oveRTOS API
// ---------------------------------------------------------------------------

unsafe extern "C" {

    // --- app / scheduler ---
    pub fn ove_run();
    pub fn ove_app_run() -> i32;

    // --- thread ---
    pub fn ove_thread_init(
        handle:  *mut ove_thread_t,
        storage: *mut ove_thread_storage_t,
        desc:    *const ove_thread_desc,
    ) -> i32;
    pub fn ove_thread_deinit(handle: ove_thread_t) -> i32;
    pub fn ove_thread_create_(
        handle: *mut ove_thread_t,
        desc:   *const ove_thread_desc,
    ) -> i32;
    pub fn ove_thread_destroy(handle: ove_thread_t) -> i32;
    pub fn ove_thread_get_self() -> ove_thread_t;
    pub fn ove_thread_set_priority(handle: ove_thread_t, prio: u32);
    pub fn ove_thread_sleep_ms(ms: u32);
    pub fn ove_thread_yield();
    pub fn ove_thread_suspend(handle: ove_thread_t);
    pub fn ove_thread_resume(handle: ove_thread_t);
    pub fn ove_thread_get_stack_usage(handle: ove_thread_t) -> usize;
    pub fn ove_thread_get_state(handle: ove_thread_t) -> u32;
    pub fn ove_thread_get_runtime_stats(
        handle: ove_thread_t,
        stats:  *mut ove_thread_stats,
    ) -> i32;

    // --- mutex ---
    pub fn ove_mutex_init(mtx: *mut ove_mutex_t, storage: *mut ove_mutex_storage_t) -> i32;
    pub fn ove_mutex_deinit(mtx: ove_mutex_t);
    pub fn ove_mutex_create(mtx: *mut ove_mutex_t) -> i32;
    pub fn ove_mutex_destroy(mtx: ove_mutex_t);
    pub fn ove_mutex_lock(mtx: ove_mutex_t, timeout_ms: u32) -> i32;
    pub fn ove_mutex_unlock(mtx: ove_mutex_t);

    // --- recursive mutex ---
    pub fn ove_recursive_mutex_init(
        mtx:     *mut ove_mutex_t,
        storage: *mut ove_mutex_storage_t,
    ) -> i32;
    pub fn ove_recursive_mutex_create(mtx: *mut ove_mutex_t) -> i32;
    pub fn ove_recursive_mutex_destroy(mtx: ove_mutex_t);
    pub fn ove_recursive_mutex_lock(mtx: ove_mutex_t, timeout_ms: u32) -> i32;
    pub fn ove_recursive_mutex_unlock(mtx: ove_mutex_t);

    // --- semaphore ---
    pub fn ove_sem_init(
        sem:     *mut ove_sem_t,
        storage: *mut ove_sem_storage_t,
        initial: u32,
        max:     u32,
    ) -> i32;
    pub fn ove_sem_deinit(sem: ove_sem_t);
    pub fn ove_sem_create(sem: *mut ove_sem_t, initial: u32, max: u32) -> i32;
    pub fn ove_sem_destroy(sem: ove_sem_t);
    pub fn ove_sem_take(sem: ove_sem_t, timeout_ms: u32) -> i32;
    pub fn ove_sem_give(sem: ove_sem_t);

    // --- event ---
    pub fn ove_event_init(
        evt:     *mut ove_event_t,
        storage: *mut ove_event_storage_t,
    ) -> i32;
    pub fn ove_event_deinit(evt: ove_event_t);
    pub fn ove_event_create(evt: *mut ove_event_t) -> i32;
    pub fn ove_event_destroy(evt: ove_event_t);
    pub fn ove_event_wait(evt: ove_event_t, timeout_ms: u32) -> i32;
    pub fn ove_event_signal(evt: ove_event_t);
    pub fn ove_event_signal_from_isr(evt: ove_event_t);

    // --- condvar ---
    pub fn ove_condvar_init(
        cv:      *mut ove_condvar_t,
        storage: *mut ove_condvar_storage_t,
    ) -> i32;
    pub fn ove_condvar_deinit(cv: ove_condvar_t);
    pub fn ove_condvar_create(cv: *mut ove_condvar_t) -> i32;
    pub fn ove_condvar_destroy(cv: ove_condvar_t);
    pub fn ove_condvar_wait(cv: ove_condvar_t, mtx: ove_mutex_t, timeout_ms: u32) -> i32;
    pub fn ove_condvar_signal(cv: ove_condvar_t);
    pub fn ove_condvar_broadcast(cv: ove_condvar_t);

    // --- queue ---
    pub fn ove_queue_init(
        q:         *mut ove_queue_t,
        storage:   *mut ove_queue_storage_t,
        buffer:    *mut c_void,
        item_size: usize,
        max_items: u32,
    ) -> i32;
    pub fn ove_queue_deinit(q: ove_queue_t);
    pub fn ove_queue_create(q: *mut ove_queue_t, item_size: usize, max_items: u32) -> i32;
    pub fn ove_queue_destroy(q: ove_queue_t);
    pub fn ove_queue_send(q: ove_queue_t, data: *const c_void, timeout_ms: u32) -> i32;
    pub fn ove_queue_receive(q: ove_queue_t, buf: *mut c_void, timeout_ms: u32) -> i32;
    pub fn ove_queue_send_from_isr(q: ove_queue_t, data: *const c_void) -> i32;
    pub fn ove_queue_receive_from_isr(q: ove_queue_t, buf: *mut c_void) -> i32;

    // --- timer ---
    pub fn ove_timer_init(
        timer:     *mut ove_timer_t,
        storage:   *mut ove_timer_storage_t,
        callback:  ove_timer_fn,
        user_data: *mut c_void,
        period_ms: u32,
        one_shot:  i32,
    ) -> i32;
    pub fn ove_timer_deinit(timer: ove_timer_t);
    pub fn ove_timer_create(
        timer:     *mut ove_timer_t,
        callback:  ove_timer_fn,
        user_data: *mut c_void,
        period_ms: u32,
        one_shot:  i32,
    ) -> i32;
    pub fn ove_timer_destroy(timer: ove_timer_t);
    pub fn ove_timer_start(timer: ove_timer_t) -> i32;
    pub fn ove_timer_stop(timer: ove_timer_t) -> i32;
    pub fn ove_timer_reset(timer: ove_timer_t) -> i32;

    // --- event group ---
    pub fn ove_eventgroup_init(
        eg:      *mut ove_eventgroup_t,
        storage: *mut ove_eventgroup_storage_t,
    ) -> i32;
    pub fn ove_eventgroup_deinit(eg: ove_eventgroup_t);
    pub fn ove_eventgroup_create(eg: *mut ove_eventgroup_t) -> i32;
    pub fn ove_eventgroup_destroy(eg: ove_eventgroup_t);
    pub fn ove_eventgroup_set_bits(eg: ove_eventgroup_t, bits: ove_eventbits_t) -> ove_eventbits_t;
    pub fn ove_eventgroup_clear_bits(eg: ove_eventgroup_t, bits: ove_eventbits_t) -> ove_eventbits_t;
    pub fn ove_eventgroup_wait_bits(
        eg:         ove_eventgroup_t,
        bits:       ove_eventbits_t,
        flags:      u32,
        timeout_ms: u32,
        result:     *mut ove_eventbits_t,
    ) -> i32;
    pub fn ove_eventgroup_set_bits_from_isr(
        eg:   ove_eventgroup_t,
        bits: ove_eventbits_t,
    ) -> ove_eventbits_t;
    pub fn ove_eventgroup_get_bits(eg: ove_eventgroup_t) -> ove_eventbits_t;

    // --- workqueue ---
    pub fn ove_workqueue_init(
        wq:        *mut ove_workqueue_t,
        storage:   *mut ove_workqueue_storage_t,
        name:      *const core::ffi::c_char,
        priority:  u32,
        stack_size: usize,
        stack:     *mut c_void,
    ) -> i32;
    pub fn ove_workqueue_deinit(wq: ove_workqueue_t);
    pub fn ove_workqueue_create(
        wq:         *mut ove_workqueue_t,
        name:       *const core::ffi::c_char,
        priority:   u32,
        stack_size: usize,
    ) -> i32;
    pub fn ove_workqueue_destroy(wq: ove_workqueue_t);
    pub fn ove_work_init_static(
        work:    *mut ove_work_t,
        storage: *mut ove_work_storage_t,
        handler: ove_work_fn,
    ) -> i32;
    pub fn ove_work_init(work: *mut ove_work_t, handler: ove_work_fn) -> i32;
    pub fn ove_work_free(work: ove_work_t);
    pub fn ove_work_submit(wq: ove_workqueue_t, work: ove_work_t) -> i32;
    pub fn ove_work_submit_delayed(wq: ove_workqueue_t, work: ove_work_t, delay_ms: u32) -> i32;
    pub fn ove_work_cancel(work: ove_work_t) -> i32;

    // --- stream ---
    pub fn ove_stream_init(
        stream:  *mut ove_stream_t,
        storage: *mut ove_stream_storage_t,
        buffer:  *mut c_void,
        size:    usize,
        trigger: usize,
    ) -> i32;
    pub fn ove_stream_deinit(stream: ove_stream_t);
    pub fn ove_stream_create(stream: *mut ove_stream_t, size: usize, trigger: usize) -> i32;
    pub fn ove_stream_destroy(stream: ove_stream_t);
    pub fn ove_stream_send(
        stream:     ove_stream_t,
        data:       *const c_void,
        len:        usize,
        timeout_ms: u32,
        bytes_sent: *mut usize,
    ) -> i32;
    pub fn ove_stream_receive(
        stream:         ove_stream_t,
        buf:            *mut c_void,
        len:            usize,
        timeout_ms:     u32,
        bytes_received: *mut usize,
    ) -> i32;
    pub fn ove_stream_send_from_isr(
        stream:     ove_stream_t,
        data:       *const c_void,
        len:        usize,
        bytes_sent: *mut usize,
    ) -> i32;
    pub fn ove_stream_receive_from_isr(
        stream:         ove_stream_t,
        buf:            *mut c_void,
        len:            usize,
        bytes_received: *mut usize,
    ) -> i32;
    pub fn ove_stream_reset(stream: ove_stream_t) -> i32;
    pub fn ove_stream_bytes_available(stream: ove_stream_t) -> usize;

    // --- watchdog ---
    pub fn ove_watchdog_init(
        wdt:        *mut ove_watchdog_t,
        storage:    *mut ove_watchdog_storage_t,
        timeout_ms: u32,
    ) -> i32;
    pub fn ove_watchdog_deinit(wdt: ove_watchdog_t);
    pub fn ove_watchdog_create(wdt: *mut ove_watchdog_t, timeout_ms: u32) -> i32;
    pub fn ove_watchdog_destroy(wdt: ove_watchdog_t);
    pub fn ove_watchdog_start(wdt: ove_watchdog_t) -> i32;
    pub fn ove_watchdog_feed(wdt: ove_watchdog_t) -> i32;

    // --- filesystem ---
    pub fn ove_fs_mount(dev_path: *const core::ffi::c_char, mount_point: *const core::ffi::c_char) -> i32;
    pub fn ove_fs_open(file: *mut ove_file_t, path: *const core::ffi::c_char, flags: i32) -> i32;
    pub fn ove_fs_close(file: ove_file_t) -> i32;
    pub fn ove_fs_read(
        file:       ove_file_t,
        buf:        *mut c_void,
        count:      usize,
        bytes_read: *mut usize,
    ) -> i32;
    pub fn ove_fs_write(
        file:          ove_file_t,
        buf:           *const c_void,
        count:         usize,
        bytes_written: *mut usize,
    ) -> i32;
    pub fn ove_fs_opendir(dir: *mut ove_dir_t, path: *const core::ffi::c_char) -> i32;
    pub fn ove_fs_closedir(dir: ove_dir_t) -> i32;
    pub fn ove_fs_readdir(dir: ove_dir_t, entry: *mut ove_dirent) -> i32;

    // --- audio graph ---
    pub fn ove_audio_graph_init(g: *mut ove_audio_graph, frames_per_period: u32) -> i32;
    pub fn ove_audio_graph_deinit(g: *mut ove_audio_graph);
    pub fn ove_audio_graph_add_node(g: *mut ove_audio_graph, ops: *const ove_audio_node_ops, ctx: *mut c_void, name: *const core::ffi::c_char, node_type: u32) -> i32;
    pub fn ove_audio_graph_connect(g: *mut ove_audio_graph, from: u32, to: u32) -> i32;
    pub fn ove_audio_graph_build(g: *mut ove_audio_graph) -> i32;
    pub fn ove_audio_graph_start(g: *mut ove_audio_graph) -> i32;
    pub fn ove_audio_graph_stop(g: *mut ove_audio_graph) -> i32;
    pub fn ove_audio_graph_process(g: *mut ove_audio_graph) -> i32;
    pub fn ove_audio_graph_get_stats(g: *const ove_audio_graph, stats: *mut ove_audio_graph_stats) -> i32;
    pub fn ove_audio_device_source(g: *mut ove_audio_graph, cfg: *const ove_audio_device_cfg, name: *const core::ffi::c_char) -> i32;
    pub fn ove_audio_device_sink(g: *mut ove_audio_graph, cfg: *const ove_audio_device_cfg, name: *const core::ffi::c_char) -> i32;

    // --- NVS ---
    pub fn ove_nvs_init() -> i32;
    pub fn ove_nvs_read(
        key:     *const core::ffi::c_char,
        buf:     *mut c_void,
        buf_len: usize,
        out_len: *mut usize,
    ) -> i32;
    pub fn ove_nvs_write(key: *const core::ffi::c_char, data: *const c_void, len: usize) -> i32;
    pub fn ove_nvs_erase(key: *const core::ffi::c_char) -> i32;

    // --- shell ---
    pub fn ove_shell_init() -> i32;
    pub fn ove_shell_register_cmd(cmd: *const ove_shell_cmd) -> i32;
    pub fn ove_shell_process_char(c: i32);

    // --- console ---
    pub fn ove_console_getchar() -> i32;
    pub fn ove_console_putchar(c: i32);
    pub fn ove_console_write(buf: *const core::ffi::c_char, len: u32);

    // --- GPIO ---
    pub fn ove_gpio_configure(port: u32, pin: u32, mode: u32) -> i32;
    pub fn ove_gpio_set(port: u32, pin: u32, value: i32) -> i32;
    pub fn ove_gpio_get(port: u32, pin: u32) -> i32;
    pub fn ove_gpio_irq_register(
        port:      u32,
        pin:       u32,
        mode:      u32,
        callback:  ove_gpio_irq_cb,
        user_data: *mut c_void,
    ) -> i32;
    pub fn ove_gpio_irq_enable(port: u32, pin: u32) -> i32;
    pub fn ove_gpio_irq_disable(port: u32, pin: u32) -> i32;

    // --- LED ---
    pub fn ove_led_set(led: u32, on: i32);
    pub fn ove_led_toggle(led: u32);
    pub fn ove_led_count() -> u32;

    // --- board ---
    pub fn ove_board_init() -> i32;
    pub fn ove_board_name() -> *const core::ffi::c_char;

    // --- time ---
    pub fn ove_time_get_us(out: *mut u64) -> i32;
    pub fn ove_time_delay_ms(ms: u32);
    pub fn ove_time_delay_us(us: u32);

    // --- LVGL integration ---
    pub fn ove_lvgl_init() -> i32;
    pub fn ove_lvgl_lock();
    pub fn ove_lvgl_unlock();
    pub fn ove_lvgl_tick(ms: u32);
    pub fn ove_lvgl_handler();

} // extern "C" oveRTOS

// ---------------------------------------------------------------------------
// LVGL types — opaque stubs
// ---------------------------------------------------------------------------

/// Opaque LVGL object. All access goes through `lv_obj_*` functions.
#[repr(C)]
pub struct lv_obj_t {
    _opaque: [u8; 256],
}

/// LVGL color (BGR888 layout to match LVGL v9 `lv_color_t`).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct lv_color_t {
    pub blue:  u8,
    pub green: u8,
    pub red:   u8,
}

/// Opaque LVGL font descriptor.
#[repr(C)]
pub struct lv_font_t {
    _opaque: [u8; 64],
}

/// Opaque LVGL style.
#[repr(C)]
pub struct lv_style_t {
    _opaque: [u8; 128],
}

/// LVGL event callback function pointer type.
pub type lv_event_cb_t = Option<unsafe extern "C" fn(*mut c_void)>;

// ---------------------------------------------------------------------------
// LVGL constants
// ---------------------------------------------------------------------------

pub const LV_OBJ_FLAG_HIDDEN:     u32 = 1 << 0;
pub const LV_OBJ_FLAG_CLICKABLE:  u32 = 1 << 1;
pub const LV_OBJ_FLAG_SCROLLABLE: u32 = 1 << 2;

pub const LV_EVENT_CLICKED:       u32 = 7;
pub const LV_EVENT_VALUE_CHANGED: u32 = 28;

pub const LV_FLEX_FLOW_ROW:    u32 = 0;
pub const LV_FLEX_FLOW_COLUMN: u32 = 1;

// ---------------------------------------------------------------------------
// LVGL font statics
// ---------------------------------------------------------------------------

unsafe extern "C" {
    pub static lv_font_montserrat_14: lv_font_t;
    pub static lv_font_montserrat_32: lv_font_t;
}

// ---------------------------------------------------------------------------
// extern "C" — LVGL functions
// ---------------------------------------------------------------------------

unsafe extern "C" {

    // core object
    pub fn lv_obj_create(parent: *mut lv_obj_t) -> *mut lv_obj_t;
    pub fn lv_obj_delete(obj: *mut lv_obj_t);
    pub fn lv_obj_clean(obj: *mut lv_obj_t);
    pub fn lv_obj_get_parent(obj: *mut lv_obj_t) -> *mut lv_obj_t;
    pub fn lv_obj_get_child_count(obj: *mut lv_obj_t) -> u32;
    pub fn lv_obj_get_width(obj: *mut lv_obj_t) -> i32;
    pub fn lv_obj_get_height(obj: *mut lv_obj_t) -> i32;
    pub fn lv_obj_set_size(obj: *mut lv_obj_t, w: i32, h: i32);
    pub fn lv_obj_set_width(obj: *mut lv_obj_t, w: i32);
    pub fn lv_obj_set_height(obj: *mut lv_obj_t, h: i32);
    pub fn lv_obj_set_pos(obj: *mut lv_obj_t, x: i32, y: i32);
    pub fn lv_obj_center(obj: *mut lv_obj_t);
    pub fn lv_obj_align(obj: *mut lv_obj_t, align: u8, x_ofs: i32, y_ofs: i32);
    pub fn lv_obj_set_user_data(obj: *mut lv_obj_t, data: *mut c_void);
    pub fn lv_obj_get_user_data(obj: *mut lv_obj_t) -> *mut c_void;
    pub fn lv_obj_add_flag(obj: *mut lv_obj_t, f: u32);
    pub fn lv_obj_remove_flag(obj: *mut lv_obj_t, f: u32);
    pub fn lv_obj_add_state(obj: *mut lv_obj_t, state: u16);
    pub fn lv_obj_remove_state(obj: *mut lv_obj_t, state: u16);
    pub fn lv_obj_add_event_cb(
        obj:       *mut lv_obj_t,
        event_cb:  lv_event_cb_t,
        filter:    u32,
        user_data: *mut c_void,
    );
    pub fn lv_obj_add_style(obj: *mut lv_obj_t, style: *mut lv_style_t, selector: u32);
    pub fn lv_obj_set_flex_flow(obj: *mut lv_obj_t, flow: u32);

    // inline styles
    pub fn lv_obj_set_style_bg_color(obj: *mut lv_obj_t, color: lv_color_t, selector: u32);
    pub fn lv_obj_set_style_bg_opa(obj: *mut lv_obj_t, opa: u8, selector: u32);
    pub fn lv_obj_set_style_border_color(obj: *mut lv_obj_t, color: lv_color_t, selector: u32);
    pub fn lv_obj_set_style_border_width(obj: *mut lv_obj_t, w: i32, selector: u32);
    pub fn lv_obj_set_style_radius(obj: *mut lv_obj_t, r: i32, selector: u32);
    pub fn lv_obj_set_style_pad_top(obj: *mut lv_obj_t, p: i32, selector: u32);
    pub fn lv_obj_set_style_pad_bottom(obj: *mut lv_obj_t, p: i32, selector: u32);
    pub fn lv_obj_set_style_pad_left(obj: *mut lv_obj_t, p: i32, selector: u32);
    pub fn lv_obj_set_style_pad_right(obj: *mut lv_obj_t, p: i32, selector: u32);
    pub fn lv_obj_set_style_pad_row(obj: *mut lv_obj_t, g: i32, selector: u32);
    pub fn lv_obj_set_style_pad_column(obj: *mut lv_obj_t, g: i32, selector: u32);
    pub fn lv_obj_set_style_text_color(obj: *mut lv_obj_t, color: lv_color_t, selector: u32);
    pub fn lv_obj_set_style_text_font(
        obj:      *mut lv_obj_t,
        font:     *const lv_font_t,
        selector: u32,
    );

    // style objects
    pub fn lv_style_init(style: *mut lv_style_t);
    pub fn lv_style_reset(style: *mut lv_style_t);
    pub fn lv_style_set_bg_color(style: *mut lv_style_t, color: lv_color_t);
    pub fn lv_style_set_bg_opa(style: *mut lv_style_t, opa: u8);
    pub fn lv_style_set_radius(style: *mut lv_style_t, r: i32);
    pub fn lv_style_set_border_color(style: *mut lv_style_t, color: lv_color_t);
    pub fn lv_style_set_border_width(style: *mut lv_style_t, w: i32);
    pub fn lv_style_set_pad_top(style: *mut lv_style_t, p: i32);
    pub fn lv_style_set_pad_bottom(style: *mut lv_style_t, p: i32);
    pub fn lv_style_set_pad_left(style: *mut lv_style_t, p: i32);
    pub fn lv_style_set_pad_right(style: *mut lv_style_t, p: i32);
    pub fn lv_style_set_text_color(style: *mut lv_style_t, color: lv_color_t);
    pub fn lv_style_set_text_font(style: *mut lv_style_t, font: *const lv_font_t);

    // color helpers
    pub fn lv_palette_main(palette: u32) -> lv_color_t;

    // screen
    pub fn lv_screen_active() -> *mut lv_obj_t;

    // label widget
    pub fn lv_label_create(parent: *mut lv_obj_t) -> *mut lv_obj_t;
    pub fn lv_label_set_text(label: *mut lv_obj_t, text: *const core::ffi::c_char);
    pub fn lv_label_set_text_static(label: *mut lv_obj_t, text: *const core::ffi::c_char);

    // bar widget
    pub fn lv_bar_create(parent: *mut lv_obj_t) -> *mut lv_obj_t;
    pub fn lv_bar_set_value(bar: *mut lv_obj_t, value: i32, anim: i32);
    pub fn lv_bar_set_range(bar: *mut lv_obj_t, min: i32, max: i32);

} // extern "C" LVGL
