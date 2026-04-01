// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Benchmark Application (Zig)
//!
//! Measures latency, throughput, and memory usage of all RTOS abstractions.
//! Output is formatted ASCII tables via the C harness (bench_output.c).
//!
//! The Zig suites exercise the raw C FFI layer directly so that the
//! measurements are comparable with the C benchmark application.

const std = @import("std");
const ove = @import("ove");
const c = ove.ffi;

// ---------------------------------------------------------------------------
// Benchmark types — mirror benchmark.h
// ---------------------------------------------------------------------------

const BenchType = enum(c_int) {
    latency = 0,
    throughput = 1,
    memory = 2,
};

const BenchCase = extern struct {
    name: [*:0]const u8,
    type: BenchType,
    setup: ?*const fn (?*anyopaque) callconv(.c) void,
    run: ?*const fn (?*anyopaque) callconv(.c) void,
    teardown: ?*const fn (?*anyopaque) callconv(.c) void,
    iterations: c_uint,
};

const BenchResult = extern struct {
    min_ns: u64,
    max_ns: u64,
    total_ns: u64,
    count: u32,
    ops_per_sec: u32,
    heap_delta: i32,
};

const BenchSuite = extern struct {
    name: [*:0]const u8,
    is_enabled: *const fn () callconv(.c) c_int,
    cases: [*]const BenchCase,
    case_count: c_uint,
};

// ---------------------------------------------------------------------------
// Harness functions (linked from bench_harness.c / bench_output.c)
// ---------------------------------------------------------------------------

extern "c" fn bench_run_case(bc: *const BenchCase, result: *BenchResult) void;
extern "c" fn bench_print_header(suite_name: [*:0]const u8) void;
extern "c" fn bench_print_result(bc: *const BenchCase, result: *const BenchResult) void;
extern "c" fn bench_print_footer() void;

// =========================================================================
// Suite: time
// =========================================================================

fn timeGetUsOverheadRun(_: ?*anyopaque) callconv(.c) void {
    var t: u64 = 0;
    _ = c.ove_time_get_us(&t);
}

fn delay1msRun(_: ?*anyopaque) callconv(.c) void {
    c.ove_time_delay_ms(1);
}

fn timeIsEnabled() callconv(.c) c_int {
    return if (@hasDecl(c, "CONFIG_OVE_TIME")) 1 else 0;
}

const time_cases = [_]BenchCase{
    .{
        .name = "time_get_us_overhead",
        .type = .latency,
        .setup = null,
        .run = &timeGetUsOverheadRun,
        .teardown = null,
        .iterations = 0,
    },
    .{
        .name = "delay_1ms",
        .type = .latency,
        .setup = null,
        .run = &delay1msRun,
        .teardown = null,
        .iterations = 100,
    },
};

export const bench_suite_time: BenchSuite = .{
    .name = "time",
    .is_enabled = &timeIsEnabled,
    .cases = &time_cases,
    .case_count = time_cases.len,
};

// =========================================================================
// Suite: thread
// =========================================================================

var thread_bench_th: c.ove_thread_t = null;
var thread_ping_sem: c.ove_sem_t = null;
var thread_pong_sem: c.ove_sem_t = null;
var thread_ctx_switch_done: volatile_int = 0;

const volatile_int = std.atomic.Value(c_int);

fn dummyThread(_: ?*anyopaque) callconv(.c) void {}

fn threadCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var th: c.ove_thread_t = null;
    var desc: c.struct_ove_thread_desc = .{
        .name = "bench_tmp",
        .entry = &dummyThread,
        .arg = null,
        .priority = c.OVE_PRIO_LOW,
        .stack_size = 0,
        .stack = null,
    };
    _ = c.ove_thread_create(&th, 1024, &desc);
    _ = c.ove_thread_destroy(th);
}

fn threadYieldRun(_: ?*anyopaque) callconv(.c) void {
    c.ove_thread_yield();
}

fn threadSleep1msRun(_: ?*anyopaque) callconv(.c) void {
    c.ove_thread_sleep_ms(1);
}

fn pongThread(_: ?*anyopaque) callconv(.c) void {
    while (thread_ctx_switch_done.load(.acquire) == 0) {
        _ = c.ove_sem_take(thread_pong_sem, c.OVE_WAIT_FOREVER);
        c.ove_sem_give(thread_ping_sem);
    }
}

fn ctxSwitchSetup(_: ?*anyopaque) callconv(.c) void {
    thread_ctx_switch_done.store(0, .release);
    _ = c.ove_sem_create(&thread_ping_sem, 0, 1);
    _ = c.ove_sem_create(&thread_pong_sem, 0, 1);

    var desc: c.struct_ove_thread_desc = .{
        .name = "pong",
        .entry = &pongThread,
        .arg = null,
        .priority = c.OVE_PRIO_NORMAL,
        .stack_size = 0,
        .stack = null,
    };
    _ = c.ove_thread_create(&thread_bench_th, 2048, &desc);
}

fn ctxSwitchRun(_: ?*anyopaque) callconv(.c) void {
    // One round-trip = 2 context switches
    c.ove_sem_give(thread_pong_sem);
    _ = c.ove_sem_take(thread_ping_sem, c.OVE_WAIT_FOREVER);
}

fn ctxSwitchTeardown(_: ?*anyopaque) callconv(.c) void {
    thread_ctx_switch_done.store(1, .release);
    c.ove_sem_give(thread_pong_sem);
    c.ove_thread_sleep_ms(10);
    _ = c.ove_thread_destroy(thread_bench_th);
    c.ove_sem_destroy(thread_ping_sem);
    c.ove_sem_destroy(thread_pong_sem);
}

fn threadIsEnabled() callconv(.c) c_int {
    return if (@hasDecl(c, "CONFIG_OVE_SYNC")) 1 else 0;
}

const thread_cases = [_]BenchCase{
    .{
        .name = "create_destroy",
        .type = .latency,
        .setup = null,
        .run = &threadCreateDestroyRun,
        .teardown = null,
        .iterations = 200,
    },
    .{
        .name = "yield",
        .type = .latency,
        .setup = null,
        .run = &threadYieldRun,
        .teardown = null,
        .iterations = 0,
    },
    .{
        .name = "sleep_1ms",
        .type = .latency,
        .setup = null,
        .run = &threadSleep1msRun,
        .teardown = null,
        .iterations = 100,
    },
    .{
        .name = "context_switch",
        .type = .latency,
        .setup = &ctxSwitchSetup,
        .run = &ctxSwitchRun,
        .teardown = &ctxSwitchTeardown,
        .iterations = 500,
    },
};

export const bench_suite_thread: BenchSuite = .{
    .name = "thread",
    .is_enabled = &threadIsEnabled,
    .cases = &thread_cases,
    .case_count = thread_cases.len,
};

// =========================================================================
// Suite: sync
// =========================================================================

var sync_bench_mtx: c.ove_mutex_t = null;
var sync_bench_sem: c.ove_sem_t = null;
var sync_bench_evt: c.ove_event_t = null;
var sync_bench_cv: c.ove_condvar_t = null;
var sync_bench_cv_mtx: c.ove_mutex_t = null;
var sync_bench_rmtx: c.ove_mutex_t = null;
var sync_contention_th: c.ove_thread_t = null;
var sync_contention_done: volatile_int = 0;
var sync_contention_count: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);
var sync_evt_th: c.ove_thread_t = null;
var sync_evt_done: volatile_int = 0;
var sync_cv_th: c.ove_thread_t = null;
var sync_cv_done: volatile_int = 0;
var sync_mem_mutex: c.ove_mutex_t = null;
var sync_mem_sem: c.ove_sem_t = null;
var sync_mem_event: c.ove_event_t = null;
var sync_mem_condvar: c.ove_condvar_t = null;

// --- Mutex lock/unlock ---

fn mutexLockUnlockSetup(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_mutex_create(&sync_bench_mtx);
}

fn mutexLockUnlockRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_mutex_lock(sync_bench_mtx, c.OVE_WAIT_FOREVER);
    c.ove_mutex_unlock(sync_bench_mtx);
}

fn mutexLockUnlockTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_mutex_destroy(sync_bench_mtx);
}

// --- Mutex create/destroy ---

fn mutexCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var m: c.ove_mutex_t = null;
    _ = c.ove_mutex_create(&m);
    c.ove_mutex_destroy(m);
}

// --- Mutex contention (2-thread throughput) ---

fn contentionThread(_: ?*anyopaque) callconv(.c) void {
    while (sync_contention_done.load(.acquire) == 0) {
        _ = c.ove_mutex_lock(sync_bench_mtx, c.OVE_WAIT_FOREVER);
        _ = sync_contention_count.fetchAdd(1, .monotonic);
        c.ove_mutex_unlock(sync_bench_mtx);
    }
}

fn mutexContentionSetup(_: ?*anyopaque) callconv(.c) void {
    sync_contention_done.store(0, .release);
    sync_contention_count.store(0, .release);
    _ = c.ove_mutex_create(&sync_bench_mtx);

    var desc: c.struct_ove_thread_desc = .{
        .name = "contention",
        .entry = &contentionThread,
        .arg = null,
        .priority = c.OVE_PRIO_NORMAL,
        .stack_size = 0,
        .stack = null,
    };
    _ = c.ove_thread_create(&sync_contention_th, 2048, &desc);
}

fn mutexContentionRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_mutex_lock(sync_bench_mtx, c.OVE_WAIT_FOREVER);
    _ = sync_contention_count.fetchAdd(1, .monotonic);
    c.ove_mutex_unlock(sync_bench_mtx);
}

fn mutexContentionTeardown(_: ?*anyopaque) callconv(.c) void {
    sync_contention_done.store(1, .release);
    c.ove_thread_sleep_ms(10);
    _ = c.ove_thread_destroy(sync_contention_th);
    c.ove_mutex_destroy(sync_bench_mtx);
}

// --- Mutex memory ---

fn mutexMemoryRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_mutex_create(&sync_mem_mutex);
}

fn mutexMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_mutex_destroy(sync_mem_mutex);
}

// --- Semaphore take/give ---

fn semTakeGiveSetup(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_sem_create(&sync_bench_sem, 1, 1);
}

fn semTakeGiveRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_sem_take(sync_bench_sem, c.OVE_WAIT_FOREVER);
    c.ove_sem_give(sync_bench_sem);
}

fn semTakeGiveTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_sem_destroy(sync_bench_sem);
}

// --- Semaphore create/destroy ---

fn semCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var s: c.ove_sem_t = null;
    _ = c.ove_sem_create(&s, 0, 1);
    c.ove_sem_destroy(s);
}

// --- Semaphore memory ---

fn semMemoryRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_sem_create(&sync_mem_sem, 0, 1);
}

fn semMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_sem_destroy(sync_mem_sem);
}

// --- Event signal/wait ---

fn evtSignaler(_: ?*anyopaque) callconv(.c) void {
    while (sync_evt_done.load(.acquire) == 0) {
        c.ove_event_signal(sync_bench_evt);
        c.ove_thread_yield();
    }
}

fn eventSignalWaitSetup(_: ?*anyopaque) callconv(.c) void {
    sync_evt_done.store(0, .release);
    _ = c.ove_event_create(&sync_bench_evt);

    var desc: c.struct_ove_thread_desc = .{
        .name = "evt_sig",
        .entry = &evtSignaler,
        .arg = null,
        .priority = c.OVE_PRIO_NORMAL,
        .stack_size = 0,
        .stack = null,
    };
    _ = c.ove_thread_create(&sync_evt_th, 1024, &desc);
}

fn eventSignalWaitRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_event_wait(sync_bench_evt, 10);
}

fn eventSignalWaitTeardown(_: ?*anyopaque) callconv(.c) void {
    sync_evt_done.store(1, .release);
    c.ove_thread_sleep_ms(10);
    _ = c.ove_thread_destroy(sync_evt_th);
    c.ove_event_destroy(sync_bench_evt);
}

// --- Event memory ---

fn eventMemoryRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_event_create(&sync_mem_event);
}

fn eventMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_event_destroy(sync_mem_event);
}

// --- Condvar signal/wait ---

fn cvSignaler(_: ?*anyopaque) callconv(.c) void {
    while (sync_cv_done.load(.acquire) == 0) {
        c.ove_condvar_signal(sync_bench_cv);
        c.ove_thread_yield();
    }
}

fn condvarSignalWaitSetup(_: ?*anyopaque) callconv(.c) void {
    sync_cv_done.store(0, .release);
    _ = c.ove_mutex_create(&sync_bench_cv_mtx);
    _ = c.ove_condvar_create(&sync_bench_cv);

    var desc: c.struct_ove_thread_desc = .{
        .name = "cv_sig",
        .entry = &cvSignaler,
        .arg = null,
        .priority = c.OVE_PRIO_NORMAL,
        .stack_size = 0,
        .stack = null,
    };
    _ = c.ove_thread_create(&sync_cv_th, 1024, &desc);
}

fn condvarSignalWaitRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_mutex_lock(sync_bench_cv_mtx, c.OVE_WAIT_FOREVER);
    _ = c.ove_condvar_wait(sync_bench_cv, sync_bench_cv_mtx, 10);
    c.ove_mutex_unlock(sync_bench_cv_mtx);
}

fn condvarSignalWaitTeardown(_: ?*anyopaque) callconv(.c) void {
    sync_cv_done.store(1, .release);
    c.ove_condvar_signal(sync_bench_cv);
    c.ove_thread_sleep_ms(10);
    _ = c.ove_thread_destroy(sync_cv_th);
    c.ove_condvar_destroy(sync_bench_cv);
    c.ove_mutex_destroy(sync_bench_cv_mtx);
}

// --- Condvar memory ---

fn condvarMemoryRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_condvar_create(&sync_mem_condvar);
}

fn condvarMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_condvar_destroy(sync_mem_condvar);
}

// --- Recursive mutex lock/unlock ---

fn rmtxLockUnlockSetup(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_recursive_mutex_create(&sync_bench_rmtx);
}

fn rmtxLockUnlockRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_recursive_mutex_lock(sync_bench_rmtx, c.OVE_WAIT_FOREVER);
    c.ove_recursive_mutex_unlock(sync_bench_rmtx);
}

fn rmtxLockUnlockTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_recursive_mutex_destroy(sync_bench_rmtx);
}

// --- Sync suite ---

fn syncIsEnabled() callconv(.c) c_int {
    return if (@hasDecl(c, "CONFIG_OVE_SYNC")) 1 else 0;
}

const sync_cases = [_]BenchCase{
    // Memory tests first -- before thread-heavy tests affect heap state
    .{
        .name = "mutex_memory",
        .type = .memory,
        .setup = null,
        .run = &mutexMemoryRun,
        .teardown = &mutexMemoryTeardown,
        .iterations = 0,
    },
    .{
        .name = "sem_memory",
        .type = .memory,
        .setup = null,
        .run = &semMemoryRun,
        .teardown = &semMemoryTeardown,
        .iterations = 0,
    },
    .{
        .name = "event_memory",
        .type = .memory,
        .setup = null,
        .run = &eventMemoryRun,
        .teardown = &eventMemoryTeardown,
        .iterations = 0,
    },
    .{
        .name = "condvar_memory",
        .type = .memory,
        .setup = null,
        .run = &condvarMemoryRun,
        .teardown = &condvarMemoryTeardown,
        .iterations = 0,
    },
    .{
        .name = "mutex_lock_unlock",
        .type = .latency,
        .setup = &mutexLockUnlockSetup,
        .run = &mutexLockUnlockRun,
        .teardown = &mutexLockUnlockTeardown,
        .iterations = 0,
    },
    .{
        .name = "mutex_create_destroy",
        .type = .latency,
        .setup = null,
        .run = &mutexCreateDestroyRun,
        .teardown = null,
        .iterations = 0,
    },
    .{
        .name = "mutex_contention_2t",
        .type = .throughput,
        .setup = &mutexContentionSetup,
        .run = &mutexContentionRun,
        .teardown = &mutexContentionTeardown,
        .iterations = 0,
    },
    .{
        .name = "sem_take_give",
        .type = .latency,
        .setup = &semTakeGiveSetup,
        .run = &semTakeGiveRun,
        .teardown = &semTakeGiveTeardown,
        .iterations = 0,
    },
    .{
        .name = "sem_create_destroy",
        .type = .latency,
        .setup = null,
        .run = &semCreateDestroyRun,
        .teardown = null,
        .iterations = 0,
    },
    .{
        .name = "event_signal_wait",
        .type = .latency,
        .setup = &eventSignalWaitSetup,
        .run = &eventSignalWaitRun,
        .teardown = &eventSignalWaitTeardown,
        .iterations = 500,
    },
    .{
        .name = "condvar_signal_wait",
        .type = .latency,
        .setup = &condvarSignalWaitSetup,
        .run = &condvarSignalWaitRun,
        .teardown = &condvarSignalWaitTeardown,
        .iterations = 500,
    },
    .{
        .name = "recursive_mutex_lock_unlock",
        .type = .latency,
        .setup = &rmtxLockUnlockSetup,
        .run = &rmtxLockUnlockRun,
        .teardown = &rmtxLockUnlockTeardown,
        .iterations = 0,
    },
};

export const bench_suite_sync: BenchSuite = .{
    .name = "sync",
    .is_enabled = &syncIsEnabled,
    .cases = &sync_cases,
    .case_count = sync_cases.len,
};

// =========================================================================
// Suite: queue
// =========================================================================

var queue_bench_q: c.ove_queue_t = null;
var queue_producer_th: c.ove_thread_t = null;
var queue_throughput_done: volatile_int = 0;
var queue_mem_q: c.ove_queue_t = null;

// --- send/receive latency ---

fn queueSendRecvSetup(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_queue_create(&queue_bench_q, @sizeOf(u32), 16);
}

fn queueSendRecvRun(_: ?*anyopaque) callconv(.c) void {
    var val: u32 = 42;
    var buf: u32 = 0;
    _ = c.ove_queue_send(queue_bench_q, &val, c.OVE_WAIT_FOREVER);
    _ = c.ove_queue_receive(queue_bench_q, &buf, c.OVE_WAIT_FOREVER);
}

fn queueSendRecvTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_queue_destroy(queue_bench_q);
}

// --- create/destroy ---

fn queueCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var q: c.ove_queue_t = null;
    _ = c.ove_queue_create(&q, @sizeOf(u32), 8);
    c.ove_queue_destroy(q);
}

// --- 2-thread throughput ---

fn queueProducerThread(_: ?*anyopaque) callconv(.c) void {
    var val: u32 = 0;
    while (queue_throughput_done.load(.acquire) == 0) {
        _ = c.ove_queue_send(queue_bench_q, &val, c.OVE_WAIT_FOREVER);
        val +%= 1;
    }
}

fn queueThroughputSetup(_: ?*anyopaque) callconv(.c) void {
    queue_throughput_done.store(0, .release);
    _ = c.ove_queue_create(&queue_bench_q, @sizeOf(u32), 64);

    var desc: c.struct_ove_thread_desc = .{
        .name = "q_prod",
        .entry = &queueProducerThread,
        .arg = null,
        .priority = c.OVE_PRIO_NORMAL,
        .stack_size = 0,
        .stack = null,
    };
    _ = c.ove_thread_create(&queue_producer_th, 2048, &desc);
}

fn queueThroughputRun(_: ?*anyopaque) callconv(.c) void {
    var buf: u32 = 0;
    _ = c.ove_queue_receive(queue_bench_q, &buf, c.OVE_WAIT_FOREVER);
}

fn queueThroughputTeardown(_: ?*anyopaque) callconv(.c) void {
    queue_throughput_done.store(1, .release);
    // Drain queue so producer unblocks
    var buf: u32 = 0;
    _ = c.ove_queue_receive(queue_bench_q, &buf, 100);
    c.ove_thread_sleep_ms(10);
    _ = c.ove_thread_destroy(queue_producer_th);
    c.ove_queue_destroy(queue_bench_q);
}

// --- memory ---

fn queueMemoryRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_queue_create(&queue_mem_q, @sizeOf(u32), 8);
}

fn queueMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_queue_destroy(queue_mem_q);
}

// --- Queue suite ---

fn queueIsEnabled() callconv(.c) c_int {
    return if (@hasDecl(c, "CONFIG_OVE_QUEUE")) 1 else 0;
}

const queue_cases = [_]BenchCase{
    .{
        .name = "memory",
        .type = .memory,
        .setup = null,
        .run = &queueMemoryRun,
        .teardown = &queueMemoryTeardown,
        .iterations = 0,
    },
    .{
        .name = "send_receive",
        .type = .latency,
        .setup = &queueSendRecvSetup,
        .run = &queueSendRecvRun,
        .teardown = &queueSendRecvTeardown,
        .iterations = 0,
    },
    .{
        .name = "create_destroy",
        .type = .latency,
        .setup = null,
        .run = &queueCreateDestroyRun,
        .teardown = null,
        .iterations = 0,
    },
    .{
        .name = "throughput_2t",
        .type = .throughput,
        .setup = &queueThroughputSetup,
        .run = &queueThroughputRun,
        .teardown = &queueThroughputTeardown,
        .iterations = 0,
    },
};

export const bench_suite_queue: BenchSuite = .{
    .name = "queue",
    .is_enabled = &queueIsEnabled,
    .cases = &queue_cases,
    .case_count = queue_cases.len,
};

// =========================================================================
// Suite: timer
// =========================================================================

var timer_bench_tmr: c.ove_timer_t = null;
var timer_mem_tmr: c.ove_timer_t = null;

fn timerDummyCb(_: c.ove_timer_t, _: ?*anyopaque) callconv(.c) void {}

// --- create/destroy ---

fn timerCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var t: c.ove_timer_t = null;
    _ = c.ove_timer_create(&t, &timerDummyCb, null, 1000, 0);
    c.ove_timer_destroy(t);
}

// --- start/stop ---

fn timerStartStopSetup(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_timer_create(&timer_bench_tmr, &timerDummyCb, null, 1000, 0);
}

fn timerStartStopRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_timer_start(timer_bench_tmr);
    _ = c.ove_timer_stop(timer_bench_tmr);
}

fn timerStartStopTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_timer_destroy(timer_bench_tmr);
}

// --- memory ---

fn timerMemoryRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_timer_create(&timer_mem_tmr, &timerDummyCb, null, 1000, 0);
}

fn timerMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_timer_destroy(timer_mem_tmr);
}

// --- Timer suite ---

fn timerIsEnabled() callconv(.c) c_int {
    return if (@hasDecl(c, "CONFIG_OVE_TIMER")) 1 else 0;
}

const timer_cases = [_]BenchCase{
    .{
        .name = "memory",
        .type = .memory,
        .setup = null,
        .run = &timerMemoryRun,
        .teardown = &timerMemoryTeardown,
        .iterations = 0,
    },
    .{
        .name = "create_destroy",
        .type = .latency,
        .setup = null,
        .run = &timerCreateDestroyRun,
        .teardown = null,
        .iterations = 0,
    },
    .{
        .name = "start_stop",
        .type = .latency,
        .setup = &timerStartStopSetup,
        .run = &timerStartStopRun,
        .teardown = &timerStartStopTeardown,
        .iterations = 0,
    },
};

export const bench_suite_timer: BenchSuite = .{
    .name = "timer",
    .is_enabled = &timerIsEnabled,
    .cases = &timer_cases,
    .case_count = timer_cases.len,
};

// =========================================================================
// Suite: eventgroup
// =========================================================================

var eg_bench_eg: c.ove_eventgroup_t = null;
var eg_mem_eg: c.ove_eventgroup_t = null;

// --- set/get bits ---

fn egSetGetSetup(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_eventgroup_create(&eg_bench_eg);
}

fn egSetGetRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_eventgroup_set_bits(eg_bench_eg, 0x01);
    _ = c.ove_eventgroup_get_bits(eg_bench_eg);
    _ = c.ove_eventgroup_clear_bits(eg_bench_eg, 0x01);
}

fn egSetGetTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_eventgroup_destroy(eg_bench_eg);
}

// --- create/destroy ---

fn egCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var eg: c.ove_eventgroup_t = null;
    _ = c.ove_eventgroup_create(&eg);
    c.ove_eventgroup_destroy(eg);
}

// --- memory ---

fn egMemoryRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_eventgroup_create(&eg_mem_eg);
}

fn egMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_eventgroup_destroy(eg_mem_eg);
}

// --- EventGroup suite ---

fn eventgroupIsEnabled() callconv(.c) c_int {
    return if (@hasDecl(c, "CONFIG_OVE_EVENTGROUP")) 1 else 0;
}

const eventgroup_cases = [_]BenchCase{
    .{
        .name = "memory",
        .type = .memory,
        .setup = null,
        .run = &egMemoryRun,
        .teardown = &egMemoryTeardown,
        .iterations = 0,
    },
    .{
        .name = "set_get_bits",
        .type = .latency,
        .setup = &egSetGetSetup,
        .run = &egSetGetRun,
        .teardown = &egSetGetTeardown,
        .iterations = 0,
    },
    .{
        .name = "create_destroy",
        .type = .latency,
        .setup = null,
        .run = &egCreateDestroyRun,
        .teardown = null,
        .iterations = 0,
    },
};

export const bench_suite_eventgroup: BenchSuite = .{
    .name = "eventgroup",
    .is_enabled = &eventgroupIsEnabled,
    .cases = &eventgroup_cases,
    .case_count = eventgroup_cases.len,
};

// =========================================================================
// Suite: workqueue
// =========================================================================

var wq_bench_wq: c.ove_workqueue_t = null;
var wq_bench_work: c.ove_work_t = null;
var wq_work_executed: volatile_int = 0;
var wq_work_sem: c.ove_sem_t = null;
var wq_bench_work_storage: c.ove_work_storage_t = std.mem.zeroes(c.ove_work_storage_t);
var wq_mem_wq: c.ove_workqueue_t = null;

fn workHandler(_: c.ove_work_t) callconv(.c) void {
    wq_work_executed.store(1, .release);
    c.ove_sem_give(wq_work_sem);
}

// --- create/destroy ---

fn wqCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var wq: c.ove_workqueue_t = null;
    _ = c.ove_workqueue_create(&wq, "bench_wq", c.OVE_PRIO_NORMAL, 2048);
    c.ove_workqueue_destroy(wq);
}

// --- submit/execute ---

fn wqSubmitSetup(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_sem_create(&wq_work_sem, 0, 1);
    _ = c.ove_workqueue_create(&wq_bench_wq, "bench_wq", c.OVE_PRIO_NORMAL, 2048);
    _ = c.ove_work_init_static(&wq_bench_work, &wq_bench_work_storage, &workHandler);
}

fn wqSubmitRun(_: ?*anyopaque) callconv(.c) void {
    wq_work_executed.store(0, .release);
    _ = c.ove_work_submit(wq_bench_wq, wq_bench_work);
    _ = c.ove_sem_take(wq_work_sem, 1000);
}

fn wqSubmitTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_workqueue_destroy(wq_bench_wq);
    c.ove_sem_destroy(wq_work_sem);
}

// --- memory ---

fn wqMemoryRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_workqueue_create(&wq_mem_wq, "bench_wq", c.OVE_PRIO_NORMAL, 2048);
}

fn wqMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_workqueue_destroy(wq_mem_wq);
}

// --- Workqueue suite ---

fn workqueueIsEnabled() callconv(.c) c_int {
    return if (@hasDecl(c, "CONFIG_OVE_WORKQUEUE")) 1 else 0;
}

const workqueue_cases = [_]BenchCase{
    .{
        .name = "memory",
        .type = .memory,
        .setup = null,
        .run = &wqMemoryRun,
        .teardown = &wqMemoryTeardown,
        .iterations = 0,
    },
    .{
        .name = "create_destroy",
        .type = .latency,
        .setup = null,
        .run = &wqCreateDestroyRun,
        .teardown = null,
        .iterations = 200,
    },
    .{
        .name = "submit_execute",
        .type = .latency,
        .setup = &wqSubmitSetup,
        .run = &wqSubmitRun,
        .teardown = &wqSubmitTeardown,
        .iterations = 500,
    },
};

export const bench_suite_workqueue: BenchSuite = .{
    .name = "workqueue",
    .is_enabled = &workqueueIsEnabled,
    .cases = &workqueue_cases,
    .case_count = workqueue_cases.len,
};

// =========================================================================
// Suite: stream
// =========================================================================

const STREAM_BUF_SIZE: usize = 256;
const STREAM_MSG_SIZE: usize = 64;

var stream_bench_strm: c.ove_stream_t = null;
var stream_producer_th: c.ove_thread_t = null;
var stream_done: volatile_int = 0;
var stream_tx_buf: [STREAM_MSG_SIZE]u8 = [_]u8{0} ** STREAM_MSG_SIZE;
var stream_rx_buf: [STREAM_MSG_SIZE]u8 = [_]u8{0} ** STREAM_MSG_SIZE;
var stream_mem_strm: c.ove_stream_t = null;

// --- send/receive 64B ---

fn streamSendRecvSetup(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_stream_create(&stream_bench_strm, STREAM_BUF_SIZE, 1);
    @memset(&stream_tx_buf, 0xAA);
}

fn streamSendRecvRun(_: ?*anyopaque) callconv(.c) void {
    var sent: usize = 0;
    var received: usize = 0;
    _ = c.ove_stream_send(stream_bench_strm, &stream_tx_buf, STREAM_MSG_SIZE, c.OVE_WAIT_FOREVER, &sent);
    _ = c.ove_stream_receive(stream_bench_strm, &stream_rx_buf, STREAM_MSG_SIZE, c.OVE_WAIT_FOREVER, &received);
}

fn streamSendRecvTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_stream_destroy(stream_bench_strm);
}

// --- create/destroy ---

fn streamCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var s: c.ove_stream_t = null;
    _ = c.ove_stream_create(&s, STREAM_BUF_SIZE, 1);
    c.ove_stream_destroy(s);
}

// --- throughput ---

fn streamProducer(_: ?*anyopaque) callconv(.c) void {
    while (stream_done.load(.acquire) == 0) {
        var sent: usize = 0;
        _ = c.ove_stream_send(stream_bench_strm, &stream_tx_buf, STREAM_MSG_SIZE, c.OVE_WAIT_FOREVER, &sent);
    }
}

fn streamThroughputSetup(_: ?*anyopaque) callconv(.c) void {
    stream_done.store(0, .release);
    @memset(&stream_tx_buf, 0xBB);
    _ = c.ove_stream_create(&stream_bench_strm, STREAM_BUF_SIZE, 1);

    var desc: c.struct_ove_thread_desc = .{
        .name = "strm_prod",
        .entry = &streamProducer,
        .arg = null,
        .priority = c.OVE_PRIO_NORMAL,
        .stack_size = 0,
        .stack = null,
    };
    _ = c.ove_thread_create(&stream_producer_th, 2048, &desc);
}

fn streamThroughputRun(_: ?*anyopaque) callconv(.c) void {
    var received: usize = 0;
    _ = c.ove_stream_receive(stream_bench_strm, &stream_rx_buf, STREAM_MSG_SIZE, c.OVE_WAIT_FOREVER, &received);
}

fn streamThroughputTeardown(_: ?*anyopaque) callconv(.c) void {
    stream_done.store(1, .release);
    // Drain so producer can unblock
    var received: usize = 0;
    _ = c.ove_stream_receive(stream_bench_strm, &stream_rx_buf, STREAM_MSG_SIZE, 100, &received);
    c.ove_thread_sleep_ms(10);
    _ = c.ove_thread_destroy(stream_producer_th);
    c.ove_stream_destroy(stream_bench_strm);
}

// --- memory ---

fn streamMemoryRun(_: ?*anyopaque) callconv(.c) void {
    _ = c.ove_stream_create(&stream_mem_strm, STREAM_BUF_SIZE, 1);
}

fn streamMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    c.ove_stream_destroy(stream_mem_strm);
}

// --- Stream suite ---

fn streamIsEnabled() callconv(.c) c_int {
    return if (@hasDecl(c, "CONFIG_OVE_STREAM")) 1 else 0;
}

const stream_cases = [_]BenchCase{
    .{
        .name = "memory",
        .type = .memory,
        .setup = null,
        .run = &streamMemoryRun,
        .teardown = &streamMemoryTeardown,
        .iterations = 0,
    },
    .{
        .name = "send_recv_64B",
        .type = .latency,
        .setup = &streamSendRecvSetup,
        .run = &streamSendRecvRun,
        .teardown = &streamSendRecvTeardown,
        .iterations = 0,
    },
    .{
        .name = "create_destroy",
        .type = .latency,
        .setup = null,
        .run = &streamCreateDestroyRun,
        .teardown = null,
        .iterations = 0,
    },
    .{
        .name = "throughput",
        .type = .throughput,
        .setup = &streamThroughputSetup,
        .run = &streamThroughputRun,
        .teardown = &streamThroughputTeardown,
        .iterations = 0,
    },
};

export const bench_suite_stream: BenchSuite = .{
    .name = "stream",
    .is_enabled = &streamIsEnabled,
    .cases = &stream_cases,
    .case_count = stream_cases.len,
};

// =========================================================================
// Suite registry and runner
// =========================================================================

const suites = [_]*const BenchSuite{
    &bench_suite_time,
    &bench_suite_thread,
    &bench_suite_sync,
    &bench_suite_queue,
    &bench_suite_timer,
    &bench_suite_eventgroup,
    &bench_suite_workqueue,
    &bench_suite_stream,
};

fn benchmarkRunner() void {
    ove.log.inf("=== oveRTOS Benchmark Suite ===", .{});

    const iterations: c_int = if (@hasDecl(c, "CONFIG_OVE_BENCHMARK_ITERATIONS"))
        c.CONFIG_OVE_BENCHMARK_ITERATIONS
    else
        1000;
    const warmup: c_int = if (@hasDecl(c, "CONFIG_OVE_BENCHMARK_WARMUP"))
        c.CONFIG_OVE_BENCHMARK_WARMUP
    else
        100;
    ove.log.inf("Iterations: {d}  Warmup: {d}", .{ iterations, warmup });

    for (suites) |suite| {
        if (suite.is_enabled() == 0) {
            ove.log.inf("Suite '{s}': SKIPPED (module disabled)", .{suite.name});
            continue;
        }

        bench_print_header(suite.name);

        const cases: [*]const BenchCase = suite.cases;
        var i: c_uint = 0;
        while (i < suite.case_count) : (i += 1) {
            var result: BenchResult = undefined;
            bench_run_case(&cases[i], &result);
            bench_print_result(&cases[i], &result);
        }

        bench_print_footer();
    }

    ove.log.inf("=== Benchmark complete ===", .{});
}

// ---------------------------------------------------------------------------
// App entry point
// ---------------------------------------------------------------------------

fn appMain() void {
    ove.log.inf("Benchmark app: init", .{});

    _ = ove.Thread.spawn("bench_run", benchmarkRunner, ove.thread.prio.normal, 8192) catch {
        ove.log.err("Failed to create benchmark thread", .{});
        return;
    };

    ove.run();

    ove.log.inf("Benchmark app: shutdown", .{});
}

comptime {
    ove.exportMain(appMain);
}
