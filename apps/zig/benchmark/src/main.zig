// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Benchmark Application (Zig)
//!
//! Measures latency, throughput, and memory usage of all RTOS abstractions
//! using safe Zig bindings. Output is formatted ASCII tables via the C
//! harness (bench_output.c).

const std = @import("std");
const ove = @import("ove");

// ---------------------------------------------------------------------------
// Benchmark types — mirror benchmark.h
// ---------------------------------------------------------------------------

const BenchType = enum(i32) {
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
    iterations: u32,
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
    is_enabled: *const fn () callconv(.c) i32,
    cases: [*]const BenchCase,
    case_count: u32,
};

// ---------------------------------------------------------------------------
// Harness functions (linked from bench_harness.c / bench_output.c)
// ---------------------------------------------------------------------------

extern fn bench_run_case(bc: *const BenchCase, result: *BenchResult) void;
extern fn bench_print_header(suite_name: [*:0]const u8) void;
extern fn bench_print_result(bc: *const BenchCase, result: *const BenchResult) void;
extern fn bench_print_footer() void;

// =========================================================================
// Suite: time
// =========================================================================

fn timeGetUsOverheadRun(_: ?*anyopaque) callconv(.c) void {
    _ = ove.time_.getUs() catch return;
}

fn delay1msRun(_: ?*anyopaque) callconv(.c) void {
    ove.time_.delayMs(1);
}

fn timeIsEnabled() callconv(.c) i32 {
    return if (@hasDecl(ove.ffi, "CONFIG_OVE_TIME")) 1 else 0;
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

var thread_bench_th: ?ove.Thread = null;
var thread_ping_sem: ?ove.Semaphore = null;
var thread_pong_sem: ?ove.Semaphore = null;
var thread_ctx_switch_done: volatile_int = volatile_int.init(0);

const volatile_int = std.atomic.Value(i32);

fn dummyThread() void {}

fn threadCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var th = ove.Thread.spawn("bench_tmp", dummyThread, ove.thread.prio.low, 1024) catch return;
    th.destroy();
}

fn threadYieldRun(_: ?*anyopaque) callconv(.c) void {
    ove.Thread.yield_();
}

fn threadSleep1msRun(_: ?*anyopaque) callconv(.c) void {
    ove.Thread.sleepMs(1);
}

fn pongThread() void {
    while (thread_ctx_switch_done.load(.acquire) == 0) {
        thread_pong_sem.?.take(ove.wait_forever) catch {};
        thread_ping_sem.?.give();
    }
}

fn ctxSwitchSetup(_: ?*anyopaque) callconv(.c) void {
    thread_ctx_switch_done.store(0, .release);
    thread_ping_sem = ove.Semaphore.create(0, 1) catch return;
    thread_pong_sem = ove.Semaphore.create(0, 1) catch return;
    thread_bench_th = ove.Thread.spawn("pong", pongThread, ove.thread.prio.normal, 2048) catch return;
}

fn ctxSwitchRun(_: ?*anyopaque) callconv(.c) void {
    // One round-trip = 2 context switches
    thread_pong_sem.?.give();
    thread_ping_sem.?.take(ove.wait_forever) catch {};
}

fn ctxSwitchTeardown(_: ?*anyopaque) callconv(.c) void {
    thread_ctx_switch_done.store(1, .release);
    thread_pong_sem.?.give();
    ove.Thread.sleepMs(10);
    if (thread_bench_th) |*t| t.destroy();
    thread_bench_th = null;
    if (thread_ping_sem) |*s| s.destroy();
    thread_ping_sem = null;
    if (thread_pong_sem) |*s| s.destroy();
    thread_pong_sem = null;
}

fn threadIsEnabled() callconv(.c) i32 {
    return if (@hasDecl(ove.ffi, "CONFIG_OVE_SYNC")) 1 else 0;
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

var sync_bench_mtx: ?ove.Mutex = null;
var sync_bench_sem: ?ove.Semaphore = null;
var sync_bench_evt: ?ove.Event = null;
var sync_bench_cv: ?ove.CondVar = null;
var sync_bench_cv_mtx: ?ove.Mutex = null;
var sync_bench_rmtx: ?ove.RecursiveMutex = null;
var sync_contention_th: ?ove.Thread = null;
var sync_contention_done: volatile_int = volatile_int.init(0);
var sync_contention_count: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);
var sync_evt_th: ?ove.Thread = null;
var sync_evt_done: volatile_int = volatile_int.init(0);
var sync_cv_th: ?ove.Thread = null;
var sync_cv_done: volatile_int = volatile_int.init(0);
var sync_mem_mutex: ?ove.Mutex = null;
var sync_mem_sem: ?ove.Semaphore = null;
var sync_mem_event: ?ove.Event = null;
var sync_mem_condvar: ?ove.CondVar = null;

// --- Mutex lock/unlock ---

fn mutexLockUnlockSetup(_: ?*anyopaque) callconv(.c) void {
    sync_bench_mtx = ove.Mutex.create() catch return;
}

fn mutexLockUnlockRun(_: ?*anyopaque) callconv(.c) void {
    sync_bench_mtx.?.lock(ove.wait_forever) catch {};
    sync_bench_mtx.?.unlock();
}

fn mutexLockUnlockTeardown(_: ?*anyopaque) callconv(.c) void {
    if (sync_bench_mtx) |*m| m.destroy();
    sync_bench_mtx = null;
}

// --- Mutex create/destroy ---

fn mutexCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var m = ove.Mutex.create() catch return;
    m.destroy();
}

// --- Mutex contention (2-thread throughput) ---

fn contentionThread() void {
    while (sync_contention_done.load(.acquire) == 0) {
        sync_bench_mtx.?.lock(ove.wait_forever) catch {};
        _ = sync_contention_count.fetchAdd(1, .monotonic);
        sync_bench_mtx.?.unlock();
    }
}

fn mutexContentionSetup(_: ?*anyopaque) callconv(.c) void {
    sync_contention_done.store(0, .release);
    sync_contention_count.store(0, .release);
    sync_bench_mtx = ove.Mutex.create() catch return;
    sync_contention_th = ove.Thread.spawn("contention", contentionThread, ove.thread.prio.normal, 2048) catch return;
}

fn mutexContentionRun(_: ?*anyopaque) callconv(.c) void {
    sync_bench_mtx.?.lock(ove.wait_forever) catch {};
    _ = sync_contention_count.fetchAdd(1, .monotonic);
    sync_bench_mtx.?.unlock();
}

fn mutexContentionTeardown(_: ?*anyopaque) callconv(.c) void {
    sync_contention_done.store(1, .release);
    ove.Thread.sleepMs(10);
    if (sync_contention_th) |*t| t.destroy();
    sync_contention_th = null;
    if (sync_bench_mtx) |*m| m.destroy();
    sync_bench_mtx = null;
}

// --- Mutex memory ---

fn mutexMemoryRun(_: ?*anyopaque) callconv(.c) void {
    sync_mem_mutex = ove.Mutex.create() catch return;
}

fn mutexMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    if (sync_mem_mutex) |*m| m.destroy();
    sync_mem_mutex = null;
}

// --- Semaphore take/give ---

fn semTakeGiveSetup(_: ?*anyopaque) callconv(.c) void {
    sync_bench_sem = ove.Semaphore.create(1, 1) catch return;
}

fn semTakeGiveRun(_: ?*anyopaque) callconv(.c) void {
    sync_bench_sem.?.take(ove.wait_forever) catch {};
    sync_bench_sem.?.give();
}

fn semTakeGiveTeardown(_: ?*anyopaque) callconv(.c) void {
    if (sync_bench_sem) |*s| s.destroy();
    sync_bench_sem = null;
}

// --- Semaphore create/destroy ---

fn semCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var s = ove.Semaphore.create(0, 1) catch return;
    s.destroy();
}

// --- Semaphore memory ---

fn semMemoryRun(_: ?*anyopaque) callconv(.c) void {
    sync_mem_sem = ove.Semaphore.create(0, 1) catch return;
}

fn semMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    if (sync_mem_sem) |*s| s.destroy();
    sync_mem_sem = null;
}

// --- Event signal/wait ---

fn evtSignaler() void {
    while (sync_evt_done.load(.acquire) == 0) {
        sync_bench_evt.?.signal();
        ove.Thread.yield_();
    }
}

fn eventSignalWaitSetup(_: ?*anyopaque) callconv(.c) void {
    sync_evt_done.store(0, .release);
    sync_bench_evt = ove.Event.create() catch return;
    sync_evt_th = ove.Thread.spawn("evt_sig", evtSignaler, ove.thread.prio.normal, 1024) catch return;
}

fn eventSignalWaitRun(_: ?*anyopaque) callconv(.c) void {
    sync_bench_evt.?.wait(10) catch {};
}

fn eventSignalWaitTeardown(_: ?*anyopaque) callconv(.c) void {
    sync_evt_done.store(1, .release);
    ove.Thread.sleepMs(10);
    if (sync_evt_th) |*t| t.destroy();
    sync_evt_th = null;
    if (sync_bench_evt) |*e| e.destroy();
    sync_bench_evt = null;
}

// --- Event memory ---

fn eventMemoryRun(_: ?*anyopaque) callconv(.c) void {
    sync_mem_event = ove.Event.create() catch return;
}

fn eventMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    if (sync_mem_event) |*e| e.destroy();
    sync_mem_event = null;
}

// --- Condvar signal/wait ---

fn cvSignaler() void {
    while (sync_cv_done.load(.acquire) == 0) {
        sync_bench_cv.?.signal();
        ove.Thread.yield_();
    }
}

fn condvarSignalWaitSetup(_: ?*anyopaque) callconv(.c) void {
    sync_cv_done.store(0, .release);
    sync_bench_cv_mtx = ove.Mutex.create() catch return;
    sync_bench_cv = ove.CondVar.create() catch return;
    sync_cv_th = ove.Thread.spawn("cv_sig", cvSignaler, ove.thread.prio.normal, 1024) catch return;
}

fn condvarSignalWaitRun(_: ?*anyopaque) callconv(.c) void {
    sync_bench_cv_mtx.?.lock(ove.wait_forever) catch {};
    sync_bench_cv.?.wait(sync_bench_cv_mtx.?, 10) catch {};
    sync_bench_cv_mtx.?.unlock();
}

fn condvarSignalWaitTeardown(_: ?*anyopaque) callconv(.c) void {
    sync_cv_done.store(1, .release);
    sync_bench_cv.?.signal();
    ove.Thread.sleepMs(10);
    if (sync_cv_th) |*t| t.destroy();
    sync_cv_th = null;
    if (sync_bench_cv) |*cv| cv.destroy();
    sync_bench_cv = null;
    if (sync_bench_cv_mtx) |*m| m.destroy();
    sync_bench_cv_mtx = null;
}

// --- Condvar memory ---

fn condvarMemoryRun(_: ?*anyopaque) callconv(.c) void {
    sync_mem_condvar = ove.CondVar.create() catch return;
}

fn condvarMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    if (sync_mem_condvar) |*cv| cv.destroy();
    sync_mem_condvar = null;
}

// --- Recursive mutex lock/unlock ---

fn rmtxLockUnlockSetup(_: ?*anyopaque) callconv(.c) void {
    sync_bench_rmtx = ove.RecursiveMutex.create() catch return;
}

fn rmtxLockUnlockRun(_: ?*anyopaque) callconv(.c) void {
    sync_bench_rmtx.?.lock(ove.wait_forever) catch {};
    sync_bench_rmtx.?.unlock();
}

fn rmtxLockUnlockTeardown(_: ?*anyopaque) callconv(.c) void {
    if (sync_bench_rmtx) |*m| m.destroy();
    sync_bench_rmtx = null;
}

// --- Sync suite ---

fn syncIsEnabled() callconv(.c) i32 {
    return if (@hasDecl(ove.ffi, "CONFIG_OVE_SYNC")) 1 else 0;
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

var queue_send_recv_q: ?ove.Queue(u32, 16) = null;
var queue_throughput_q: ?ove.Queue(u32, 64) = null;
var queue_producer_th: ?ove.Thread = null;
var queue_throughput_done: volatile_int = volatile_int.init(0);
var queue_mem_q: ?ove.Queue(u32, 8) = null;

// --- send/receive latency ---

fn queueSendRecvSetup(_: ?*anyopaque) callconv(.c) void {
    queue_send_recv_q = ove.Queue(u32, 16).create() catch return;
}

fn queueSendRecvRun(_: ?*anyopaque) callconv(.c) void {
    var val: u32 = 42;
    queue_send_recv_q.?.send(&val, ove.wait_forever) catch return;
    _ = queue_send_recv_q.?.receive(ove.wait_forever) catch return;
}

fn queueSendRecvTeardown(_: ?*anyopaque) callconv(.c) void {
    if (queue_send_recv_q) |*q| q.destroy();
    queue_send_recv_q = null;
}

// --- create/destroy ---

fn queueCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var q = ove.Queue(u32, 8).create() catch return;
    q.destroy();
}

// --- 2-thread throughput ---

fn queueProducerThread() void {
    var val: u32 = 0;
    while (queue_throughput_done.load(.acquire) == 0) {
        queue_throughput_q.?.send(&val, ove.wait_forever) catch {};
        val +%= 1;
    }
}

fn queueThroughputSetup(_: ?*anyopaque) callconv(.c) void {
    queue_throughput_done.store(0, .release);
    queue_throughput_q = ove.Queue(u32, 64).create() catch return;
    queue_producer_th = ove.Thread.spawn("q_prod", queueProducerThread, ove.thread.prio.normal, 2048) catch return;
}

fn queueThroughputRun(_: ?*anyopaque) callconv(.c) void {
    _ = queue_throughput_q.?.receive(ove.wait_forever) catch return;
}

fn queueThroughputTeardown(_: ?*anyopaque) callconv(.c) void {
    queue_throughput_done.store(1, .release);
    // Drain queue so producer unblocks
    _ = queue_throughput_q.?.receive(100) catch 0;
    ove.Thread.sleepMs(10);
    if (queue_producer_th) |*t| t.destroy();
    queue_producer_th = null;
    if (queue_throughput_q) |*q| q.destroy();
    queue_throughput_q = null;
}

// --- memory ---

fn queueMemoryRun(_: ?*anyopaque) callconv(.c) void {
    queue_mem_q = ove.Queue(u32, 8).create() catch return;
}

fn queueMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    if (queue_mem_q) |*q| q.destroy();
    queue_mem_q = null;
}

// --- Queue suite ---

fn queueIsEnabled() callconv(.c) i32 {
    return if (@hasDecl(ove.ffi, "CONFIG_OVE_QUEUE")) 1 else 0;
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

var timer_bench_tmr: ?ove.Timer = null;
var timer_mem_tmr: ?ove.Timer = null;

fn timerDummyCb() void {}

// --- create/destroy ---

fn timerCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var t = ove.Timer.create(timerDummyCb, 1000, false) catch return;
    t.destroy();
}

// --- start/stop ---

fn timerStartStopSetup(_: ?*anyopaque) callconv(.c) void {
    timer_bench_tmr = ove.Timer.create(timerDummyCb, 1000, false) catch return;
}

fn timerStartStopRun(_: ?*anyopaque) callconv(.c) void {
    timer_bench_tmr.?.start() catch {};
    timer_bench_tmr.?.stop() catch {};
}

fn timerStartStopTeardown(_: ?*anyopaque) callconv(.c) void {
    if (timer_bench_tmr) |*t| t.destroy();
    timer_bench_tmr = null;
}

// --- memory ---

fn timerMemoryRun(_: ?*anyopaque) callconv(.c) void {
    timer_mem_tmr = ove.Timer.create(timerDummyCb, 1000, false) catch return;
}

fn timerMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    if (timer_mem_tmr) |*t| t.destroy();
    timer_mem_tmr = null;
}

// --- Timer suite ---

fn timerIsEnabled() callconv(.c) i32 {
    return if (@hasDecl(ove.ffi, "CONFIG_OVE_TIMER")) 1 else 0;
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

var eg_bench_eg: ?ove.EventGroup = null;
var eg_mem_eg: ?ove.EventGroup = null;

// --- set/get bits ---

fn egSetGetSetup(_: ?*anyopaque) callconv(.c) void {
    eg_bench_eg = ove.EventGroup.create() catch return;
}

fn egSetGetRun(_: ?*anyopaque) callconv(.c) void {
    _ = eg_bench_eg.?.setBits(0x01);
    _ = eg_bench_eg.?.getBits();
    _ = eg_bench_eg.?.clearBits(0x01);
}

fn egSetGetTeardown(_: ?*anyopaque) callconv(.c) void {
    if (eg_bench_eg) |*eg| eg.destroy();
    eg_bench_eg = null;
}

// --- create/destroy ---

fn egCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var eg = ove.EventGroup.create() catch return;
    eg.destroy();
}

// --- memory ---

fn egMemoryRun(_: ?*anyopaque) callconv(.c) void {
    eg_mem_eg = ove.EventGroup.create() catch return;
}

fn egMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    if (eg_mem_eg) |*eg| eg.destroy();
    eg_mem_eg = null;
}

// --- EventGroup suite ---

fn eventgroupIsEnabled() callconv(.c) i32 {
    return if (@hasDecl(ove.ffi, "CONFIG_OVE_EVENTGROUP")) 1 else 0;
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

var wq_bench_wq: ?ove.Workqueue = null;
var wq_bench_work: ?ove.Work = null;
var wq_work_executed: volatile_int = volatile_int.init(0);
var wq_work_sem: ?ove.Semaphore = null;
var wq_mem_wq: ?ove.Workqueue = null;

fn workHandler() void {
    wq_work_executed.store(1, .release);
    wq_work_sem.?.give();
}

// --- create/destroy ---

fn wqCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var wq = ove.Workqueue.create("bench_wq", ove.thread.prio.normal, 2048) catch return;
    wq.destroy();
}

// --- submit/execute ---

fn wqSubmitSetup(_: ?*anyopaque) callconv(.c) void {
    wq_work_sem = ove.Semaphore.create(0, 1) catch return;
    wq_bench_wq = ove.Workqueue.create("bench_wq", ove.thread.prio.normal, 2048) catch return;
    wq_bench_work = ove.Work.create(workHandler) catch return;
}

fn wqSubmitRun(_: ?*anyopaque) callconv(.c) void {
    wq_work_executed.store(0, .release);
    wq_bench_wq.?.submit(wq_bench_work.?) catch {};
    wq_work_sem.?.take(1000) catch {};
}

fn wqSubmitTeardown(_: ?*anyopaque) callconv(.c) void {
    if (wq_bench_work) |*w| w.free();
    wq_bench_work = null;
    if (wq_bench_wq) |*w| w.destroy();
    wq_bench_wq = null;
    if (wq_work_sem) |*s| s.destroy();
    wq_work_sem = null;
}

// --- memory ---

fn wqMemoryRun(_: ?*anyopaque) callconv(.c) void {
    wq_mem_wq = ove.Workqueue.create("bench_wq", ove.thread.prio.normal, 2048) catch return;
}

fn wqMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    if (wq_mem_wq) |*w| w.destroy();
    wq_mem_wq = null;
}

// --- Workqueue suite ---

fn workqueueIsEnabled() callconv(.c) i32 {
    return if (@hasDecl(ove.ffi, "CONFIG_OVE_WORKQUEUE")) 1 else 0;
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

var stream_bench_strm: ?ove.Stream = null;
var stream_producer_th: ?ove.Thread = null;
var stream_done: volatile_int = volatile_int.init(0);
var stream_tx_buf: [STREAM_MSG_SIZE]u8 = [_]u8{0} ** STREAM_MSG_SIZE;
var stream_rx_buf: [STREAM_MSG_SIZE]u8 = [_]u8{0} ** STREAM_MSG_SIZE;
var stream_mem_strm: ?ove.Stream = null;

// --- send/receive 64B ---

fn streamSendRecvSetup(_: ?*anyopaque) callconv(.c) void {
    stream_bench_strm = ove.Stream.create(STREAM_BUF_SIZE, 1) catch return;
    @memset(&stream_tx_buf, 0xAA);
}

fn streamSendRecvRun(_: ?*anyopaque) callconv(.c) void {
    _ = stream_bench_strm.?.send(&stream_tx_buf, ove.wait_forever) catch return;
    _ = stream_bench_strm.?.receive(&stream_rx_buf, ove.wait_forever) catch return;
}

fn streamSendRecvTeardown(_: ?*anyopaque) callconv(.c) void {
    if (stream_bench_strm) |*s| s.destroy();
    stream_bench_strm = null;
}

// --- create/destroy ---

fn streamCreateDestroyRun(_: ?*anyopaque) callconv(.c) void {
    var s = ove.Stream.create(STREAM_BUF_SIZE, 1) catch return;
    s.destroy();
}

// --- throughput ---

fn streamProducer() void {
    while (stream_done.load(.acquire) == 0) {
        _ = stream_bench_strm.?.send(&stream_tx_buf, ove.wait_forever) catch {};
    }
}

fn streamThroughputSetup(_: ?*anyopaque) callconv(.c) void {
    stream_done.store(0, .release);
    @memset(&stream_tx_buf, 0xBB);
    stream_bench_strm = ove.Stream.create(STREAM_BUF_SIZE, 1) catch return;
    stream_producer_th = ove.Thread.spawn("strm_prod", streamProducer, ove.thread.prio.normal, 2048) catch return;
}

fn streamThroughputRun(_: ?*anyopaque) callconv(.c) void {
    _ = stream_bench_strm.?.receive(&stream_rx_buf, ove.wait_forever) catch return;
}

fn streamThroughputTeardown(_: ?*anyopaque) callconv(.c) void {
    stream_done.store(1, .release);
    // Drain so producer can unblock
    _ = stream_bench_strm.?.receive(&stream_rx_buf, 100) catch 0;
    ove.Thread.sleepMs(10);
    if (stream_producer_th) |*t| t.destroy();
    stream_producer_th = null;
    if (stream_bench_strm) |*s| s.destroy();
    stream_bench_strm = null;
}

// --- memory ---

fn streamMemoryRun(_: ?*anyopaque) callconv(.c) void {
    stream_mem_strm = ove.Stream.create(STREAM_BUF_SIZE, 1) catch return;
}

fn streamMemoryTeardown(_: ?*anyopaque) callconv(.c) void {
    if (stream_mem_strm) |*s| s.destroy();
    stream_mem_strm = null;
}

// --- Stream suite ---

fn streamIsEnabled() callconv(.c) i32 {
    return if (@hasDecl(ove.ffi, "CONFIG_OVE_STREAM")) 1 else 0;
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

    const iterations: i32 = if (@hasDecl(ove.ffi, "CONFIG_OVE_BENCHMARK_ITERATIONS"))
        ove.ffi.CONFIG_OVE_BENCHMARK_ITERATIONS
    else
        1000;
    const warmup: i32 = if (@hasDecl(ove.ffi, "CONFIG_OVE_BENCHMARK_WARMUP"))
        ove.ffi.CONFIG_OVE_BENCHMARK_WARMUP
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
        var i: u32 = 0;
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
