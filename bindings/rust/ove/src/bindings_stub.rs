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
pub type ove_model_t      = *mut c_void;
pub type ove_netif_t      = *mut c_void;
pub type ove_socket_t     = *mut c_void;
pub type ove_http_client_t = *mut c_void;
pub type ove_mqtt_client_t = *mut c_void;
pub type ove_tls_t        = *mut c_void;
pub type ove_uart_t       = *mut c_void;
pub type ove_spi_t        = *mut c_void;
pub type ove_i2c_t        = *mut c_void;
pub type ove_i2s_t        = *mut c_void;

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
#[repr(C)]
pub struct ove_model_storage_t      { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_netif_storage_t      { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_socket_storage_t     { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_http_client_storage_t { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_mqtt_client_storage_t { _opaque: [u8; 256] }
#[repr(C)]
pub struct ove_tls_storage_t        { _opaque: [u8; 256] }

// ---------------------------------------------------------------------------
// Networking types
// ---------------------------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ove_sockaddr_t {
    pub family: u8,
    pub port: u16,
    pub addr: [u8; 16],
}

#[repr(C)]
pub struct ove_netif_config_t {
    pub use_dhcp: core::ffi::c_int,
    pub static_ip: ove_sockaddr_t,
    pub gateway: ove_sockaddr_t,
    pub netmask: ove_sockaddr_t,
    pub dns: ove_sockaddr_t,
}

pub type ove_http_method_t = u32;
pub const OVE_HTTP_GET: ove_http_method_t = 0;
pub const OVE_HTTP_POST: ove_http_method_t = 1;
pub const OVE_HTTP_PUT: ove_http_method_t = 2;
pub const OVE_HTTP_DELETE: ove_http_method_t = 3;
pub const OVE_HTTP_PATCH: ove_http_method_t = 4;

#[repr(C)]
pub struct ove_http_response_t {
    pub status: i32,
    pub body: *mut core::ffi::c_char,
    pub body_len: usize,
    pub headers: *mut core::ffi::c_char,
    pub headers_len: usize,
}

#[repr(C)]
pub struct ove_http_header_t {
    pub name: *const core::ffi::c_char,
    pub value: *const core::ffi::c_char,
}

pub type ove_mqtt_qos_t = u32;
pub const OVE_MQTT_QOS0: ove_mqtt_qos_t = 0;
pub const OVE_MQTT_QOS1: ove_mqtt_qos_t = 1;

#[repr(C)]
pub struct ove_mqtt_config_t {
    pub host: *const core::ffi::c_char,
    pub port: u16,
    pub client_id: *const core::ffi::c_char,
    pub username: *const core::ffi::c_char,
    pub password: *const core::ffi::c_char,
    pub keep_alive_s: u16,
    pub use_tls: core::ffi::c_int,
    pub on_message: Option<unsafe extern "C" fn(*const core::ffi::c_char, usize, *const c_void, usize, *mut c_void)>,
    pub user_data: *mut c_void,
}

pub type ove_httpd_handler_t = Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> core::ffi::c_int>;
pub type ove_httpd_req_t = c_void;
pub type ove_httpd_resp_t = c_void;
pub type ove_httpd_ws_handler_t = Option<unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> core::ffi::c_int>;
pub type ove_httpd_ws_close_handler_t = Option<unsafe extern "C" fn(*mut c_void)>;
pub type ove_httpd_ws_conn_t = c_void;

#[repr(C)]
pub struct ove_tls_config_t {
    pub ca_cert: *const u8,
    pub ca_cert_len: usize,
    pub hostname: *const core::ffi::c_char,
}

#[repr(C)]
pub struct ove_model_config {
    pub model_data: *const u8,
    pub model_size: usize,
    pub arena_size: usize,
}

// ---------------------------------------------------------------------------
// GPIO IRQ mode
// ---------------------------------------------------------------------------

pub type ove_gpio_irq_mode_t = u32;

// ---------------------------------------------------------------------------
// PM types
// ---------------------------------------------------------------------------

pub type ove_pm_state_t = u32;
pub type ove_pm_wake_type_t = u32;
pub type ove_pm_domain_t = u32;
pub type ove_pm_event_t = u32;

pub type ove_pm_policy_fn = Option<unsafe extern "C" fn(ove_pm_state_t, u32, *mut c_void) -> ove_pm_state_t>;
pub type ove_pm_notify_fn = Option<unsafe extern "C" fn(ove_pm_event_t, ove_pm_state_t, ove_pm_state_t, *mut c_void)>;

#[repr(C)]
pub struct ove_pm_cfg {
    pub idle_threshold_ms: u32,
    pub standby_threshold_ms: u32,
    pub deep_sleep_threshold_ms: u32,
}

#[repr(C)]
pub struct ove_pm_wake_src {
    pub type_: ove_pm_wake_type_t,
    pub __bindgen_anon_1: ove_pm_wake_src__anon,
}

#[repr(C)]
pub union ove_pm_wake_src__anon {
    pub gpio: ove_pm_wake_src__gpio,
    pub timer: ove_pm_wake_src__timer,
    pub uart: ove_pm_wake_src__uart,
    pub rtc: ove_pm_wake_src__rtc,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ove_pm_wake_src__gpio {
    pub port: u32,
    pub pin: u32,
    pub edge: ove_gpio_irq_mode_t,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ove_pm_wake_src__timer {
    pub timeout_ms: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ove_pm_wake_src__uart {
    pub instance: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ove_pm_wake_src__rtc {
    pub alarm_ms: u32,
}

#[repr(C)]
pub struct ove_pm_stats {
    pub current_state: ove_pm_state_t,
    pub time_in_state_ms: [u64; 4],
    pub transition_count: [u32; 4],
    pub total_runtime_ms: u64,
}

// ---------------------------------------------------------------------------
// Bus driver types
// ---------------------------------------------------------------------------

#[repr(C)]
pub struct ove_spi_cs {
    pub port: u32,
    pub pin: u32,
    pub active_low: i32,
}

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
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ove_audio_node_type {
    OVE_AUDIO_NODE_SOURCE    = 0,
    OVE_AUDIO_NODE_PROCESSOR = 1,
    OVE_AUDIO_NODE_SINK      = 2,
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
    pub type_:   ove_audio_node_type,
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
pub type ove_shell_output_hook_t = Option<unsafe extern "C" fn(*const core::ffi::c_char)>;
pub type ove_thread_state_t = u32;
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
    pub fn ove_audio_graph_add_node(g: *mut ove_audio_graph, ops: *const ove_audio_node_ops, ctx: *mut c_void, name: *const core::ffi::c_char, node_type: ove_audio_node_type) -> i32;
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

    // --- infer (ML model) ---
    pub fn ove_model_create(m: *mut ove_model_t, cfg: *const ove_model_config) -> i32;
    pub fn ove_model_destroy(m: ove_model_t);
    pub fn ove_model_init(m: *mut ove_model_t, storage: *mut ove_model_storage_t, arena: *mut u8, cfg: *const ove_model_config) -> i32;
    pub fn ove_model_deinit(m: ove_model_t);
    pub fn ove_model_invoke(m: ove_model_t) -> i32;
    pub fn ove_model_last_inference_us(m: ove_model_t) -> u64;

    // --- networking ---
    pub fn ove_sockaddr_ipv4(addr: *mut ove_sockaddr_t, a: u8, b: u8, c: u8, d: u8, port: u16);
    pub fn ove_dns_resolve(hostname: *const core::ffi::c_char, addr: *mut ove_sockaddr_t, timeout_ms: u32) -> i32;
    pub fn ove_netif_init(netif: *mut ove_netif_t, storage: *mut ove_netif_storage_t) -> i32;
    pub fn ove_netif_deinit(netif: ove_netif_t);
    pub fn ove_netif_create(netif: *mut ove_netif_t) -> i32;
    pub fn ove_netif_destroy(netif: ove_netif_t);
    pub fn ove_netif_up(netif: ove_netif_t, cfg: *const ove_netif_config_t) -> i32;
    pub fn ove_netif_down(netif: ove_netif_t);
    pub fn ove_netif_get_addr(netif: ove_netif_t, ip: *mut ove_sockaddr_t, gw: *mut ove_sockaddr_t, nm: *mut ove_sockaddr_t) -> i32;
    pub fn ove_socket_open(sock: *mut ove_socket_t, storage: *mut ove_socket_storage_t, af: u8, sock_type: u8) -> i32;
    pub fn ove_socket_close(sock: ove_socket_t);
    pub fn ove_socket_create(sock: *mut ove_socket_t, af: u8, sock_type: u8) -> i32;
    pub fn ove_socket_destroy(sock: ove_socket_t);
    pub fn ove_socket_connect(sock: ove_socket_t, addr: *const ove_sockaddr_t, timeout_ms: u32) -> i32;
    pub fn ove_socket_bind(sock: ove_socket_t, addr: *const ove_sockaddr_t) -> i32;
    pub fn ove_socket_listen(sock: ove_socket_t, backlog: i32) -> i32;
    pub fn ove_socket_accept(sock: ove_socket_t, client: *mut ove_socket_t, client_storage: *mut ove_socket_storage_t, timeout_ms: u32) -> i32;
    pub fn ove_socket_send(sock: ove_socket_t, data: *const c_void, len: usize, sent: *mut usize) -> i32;
    pub fn ove_socket_recv(sock: ove_socket_t, buf: *mut c_void, len: usize, received: *mut usize, timeout_ms: u32) -> i32;
    pub fn ove_socket_sendto(sock: ove_socket_t, data: *const c_void, len: usize, sent: *mut usize, dest: *const ove_sockaddr_t) -> i32;
    pub fn ove_socket_recvfrom(sock: ove_socket_t, buf: *mut c_void, len: usize, received: *mut usize, src: *mut ove_sockaddr_t, timeout_ms: u32) -> i32;
    pub fn ove_http_client_init(client: *mut ove_http_client_t, storage: *mut ove_http_client_storage_t) -> i32;
    pub fn ove_http_client_deinit(client: ove_http_client_t);
    pub fn ove_http_client_create(client: *mut ove_http_client_t) -> i32;
    pub fn ove_http_client_destroy(client: ove_http_client_t);
    pub fn ove_http_get(client: ove_http_client_t, url: *const core::ffi::c_char, resp: *mut ove_http_response_t) -> i32;
    pub fn ove_http_post(client: ove_http_client_t, url: *const core::ffi::c_char, ct: *const core::ffi::c_char, body: *const c_void, body_len: usize, resp: *mut ove_http_response_t) -> i32;
    pub fn ove_http_request_ex(client: ove_http_client_t, method: ove_http_method_t, url: *const core::ffi::c_char, ct: *const core::ffi::c_char, body: *const c_void, body_len: usize, headers: *const ove_http_header_t, header_count: usize, resp: *mut ove_http_response_t) -> i32;
    pub fn ove_http_response_free(resp: *mut ove_http_response_t);
    pub fn ove_mqtt_client_init(client: *mut ove_mqtt_client_t, storage: *mut ove_mqtt_client_storage_t) -> i32;
    pub fn ove_mqtt_client_deinit(client: ove_mqtt_client_t);
    pub fn ove_mqtt_client_create(client: *mut ove_mqtt_client_t) -> i32;
    pub fn ove_mqtt_client_destroy(client: ove_mqtt_client_t);
    pub fn ove_mqtt_connect(client: ove_mqtt_client_t, cfg: *const ove_mqtt_config_t) -> i32;
    pub fn ove_mqtt_disconnect(client: ove_mqtt_client_t);
    pub fn ove_mqtt_publish(client: ove_mqtt_client_t, topic: *const core::ffi::c_char, payload: *const c_void, payload_len: usize, qos: ove_mqtt_qos_t) -> i32;
    pub fn ove_mqtt_subscribe(client: ove_mqtt_client_t, topic: *const core::ffi::c_char, qos: ove_mqtt_qos_t) -> i32;
    pub fn ove_mqtt_unsubscribe(client: ove_mqtt_client_t, topic: *const core::ffi::c_char) -> i32;
    pub fn ove_mqtt_loop(client: ove_mqtt_client_t, timeout_ms: u32) -> i32;
    pub fn ove_httpd_start(cfg: *const c_void) -> i32;
    pub fn ove_httpd_stop();
    pub fn ove_httpd_route(method: *const core::ffi::c_char, path: *const core::ffi::c_char, handler: ove_httpd_handler_t) -> i32;
    pub fn ove_httpd_register_builtin_routes();
    pub fn ove_httpd_req_method(req: *mut c_void) -> *const core::ffi::c_char;
    pub fn ove_httpd_req_path(req: *mut c_void) -> *const core::ffi::c_char;
    pub fn ove_httpd_req_query(req: *mut c_void) -> *const core::ffi::c_char;
    pub fn ove_httpd_req_body(req: *mut c_void) -> *const core::ffi::c_char;
    pub fn ove_httpd_req_body_len(req: *mut c_void) -> usize;
    pub fn ove_httpd_req_segment(req: *mut c_void, idx: i32) -> *const core::ffi::c_char;
    pub fn ove_httpd_resp_json(resp: *mut c_void, status: i32, json: *const core::ffi::c_char) -> i32;
    pub fn ove_httpd_resp_html(resp: *mut c_void, status: i32, html: *const core::ffi::c_char, len: usize) -> i32;
    pub fn ove_httpd_resp_send(resp: *mut c_void, status: i32, ct: *const core::ffi::c_char, body: *const c_void, len: usize) -> i32;
    pub fn ove_httpd_resp_send_gz(resp: *mut c_void, status: i32, ct: *const core::ffi::c_char, body: *const c_void, len: usize) -> i32;
    pub fn ove_httpd_resp_error(resp: *mut c_void, status: i32, msg: *const core::ffi::c_char) -> i32;
    pub fn ove_httpd_log_append(line: *const core::ffi::c_char);
    pub fn ove_httpd_set_netif(netif: ove_netif_t);
    pub fn ove_httpd_ws_route(path: *const core::ffi::c_char, on_msg: ove_httpd_ws_handler_t, on_close: ove_httpd_ws_close_handler_t) -> i32;
    pub fn ove_httpd_ws_send(conn: *mut ove_httpd_ws_conn_t, data: *const c_void, len: usize) -> i32;
    pub fn ove_httpd_ws_broadcast(path: *const core::ffi::c_char, data: *const c_void, len: usize) -> i32;
    pub fn ove_httpd_ws_active_count() -> i32;
    pub fn ove_tls_init(tls: *mut ove_tls_t, storage: *mut ove_tls_storage_t) -> i32;
    pub fn ove_tls_deinit(tls: ove_tls_t);
    pub fn ove_tls_create(tls: *mut ove_tls_t) -> i32;
    pub fn ove_tls_destroy(tls: ove_tls_t);
    pub fn ove_tls_handshake(tls: ove_tls_t, sock: ove_socket_t, cfg: *const ove_tls_config_t) -> i32;
    pub fn ove_tls_send(tls: ove_tls_t, data: *const c_void, len: usize, sent: *mut usize) -> i32;
    pub fn ove_tls_recv(tls: ove_tls_t, buf: *mut c_void, len: usize, received: *mut usize) -> i32;
    pub fn ove_tls_close(tls: ove_tls_t);
    pub fn ove_shell_set_output_hook(hook: ove_shell_output_hook_t);
    pub fn ove_sntp_sync(cfg: *const c_void) -> i32;
    pub fn ove_sntp_get_offset_us(offset_us: *mut i64) -> i32;
    pub fn ove_sntp_get_utc(utc_s: *mut u32) -> i32;

    // --- PM ---
    pub fn ove_pm_init(cfg: *const ove_pm_cfg) -> i32;
    pub fn ove_pm_deinit();
    pub fn ove_pm_set_state(state: ove_pm_state_t) -> i32;
    pub fn ove_pm_get_state() -> ove_pm_state_t;
    pub fn ove_pm_activity();
    pub fn ove_pm_wake_register(src: *const ove_pm_wake_src) -> i32;
    pub fn ove_pm_wake_unregister(src: *const ove_pm_wake_src) -> i32;
    pub fn ove_pm_domain_request(domain: ove_pm_domain_t) -> i32;
    pub fn ove_pm_domain_release(domain: ove_pm_domain_t) -> i32;
    pub fn ove_pm_domain_get_refcount(domain: ove_pm_domain_t) -> i32;
    pub fn ove_pm_set_policy(policy: ove_pm_policy_fn, user_data: *mut c_void) -> i32;
    pub fn ove_pm_notify_register(cb: ove_pm_notify_fn, user_data: *mut c_void) -> i32;
    pub fn ove_pm_notify_unregister(cb: ove_pm_notify_fn, user_data: *mut c_void) -> i32;
    pub fn ove_pm_get_stats(stats: *mut ove_pm_stats) -> i32;
    pub fn ove_pm_reset_stats() -> i32;
    pub fn ove_pm_set_budget(target: u32) -> i32;
    pub fn ove_pm_get_budget_status(actual: *mut u32) -> i32;

    // --- UART ---
    pub fn ove_uart_write(uart: ove_uart_t, data: *const c_void, len: usize, sent: *mut usize) -> i32;
    pub fn ove_uart_read(uart: ove_uart_t, buf: *mut c_void, len: usize, received: *mut usize, timeout_ms: u32) -> i32;
    pub fn ove_uart_flush(uart: ove_uart_t) -> i32;
    pub fn ove_uart_bytes_available(uart: ove_uart_t) -> usize;

    // --- SPI ---
    pub fn ove_spi_transfer(spi: ove_spi_t, cs: *const ove_spi_cs, tx: *const c_void, rx: *mut c_void, len: usize) -> i32;
    pub fn ove_spi_write(spi: ove_spi_t, cs: *const ove_spi_cs, data: *const c_void, len: usize) -> i32;
    pub fn ove_spi_read(spi: ove_spi_t, cs: *const ove_spi_cs, buf: *mut c_void, len: usize) -> i32;

    // --- I2C ---
    pub fn ove_i2c_write(i2c: ove_i2c_t, addr: u16, data: *const c_void, len: usize, timeout_ms: u32) -> i32;
    pub fn ove_i2c_read(i2c: ove_i2c_t, addr: u16, buf: *mut c_void, len: usize, timeout_ms: u32) -> i32;
    pub fn ove_i2c_write_read(i2c: ove_i2c_t, addr: u16, tx: *const c_void, tx_len: usize, rx: *mut c_void, rx_len: usize, timeout_ms: u32) -> i32;
    pub fn ove_i2c_probe(i2c: ove_i2c_t, addr: u16, timeout_ms: u32) -> i32;
    pub fn ove_i2c_reg_write(i2c: ove_i2c_t, addr: u16, reg: u8, data: *const c_void, len: usize, timeout_ms: u32) -> i32;
    pub fn ove_i2c_reg_read(i2c: ove_i2c_t, addr: u16, reg: u8, buf: *mut c_void, len: usize, timeout_ms: u32) -> i32;

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
/// LVGL event code type.
pub type lv_event_code_t = u32;

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
