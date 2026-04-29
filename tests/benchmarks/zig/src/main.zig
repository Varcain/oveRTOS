// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Benchmark Application (Zig)
//!
//! Measures latency, throughput, and memory usage of all RTOS abstractions
//! through the safe Zig binding layer. Output is formatted ASCII tables
//! via the C harness (bench_output.c).

const std = @import("std");
const ove = @import("ove");
// Bench harness wrappers used to live at `ove.bench`; they're now bench-app-local.
const bench = @import("bench.zig");

const volatile_int = std.atomic.Value(i32);

// =========================================================================
// Suite: time
// =========================================================================

fn timeGetUsOverheadRun() void {
    _ = ove.time.getUs() catch return;
}

fn delay1msRun() void {
    ove.time.delayMs(1);
}

fn timeIsEnabled() bool {
    return true;
}

const time_case_get_us = bench.CaseSpec{
    .name = "time_get_us_overhead",
    .kind = .latency,
    .run = &timeGetUsOverheadRun,
    .inner_iters = 10,
};
const time_case_delay_1ms = bench.CaseSpec{
    .name = "delay_1ms",
    .kind = .latency,
    .run = &delay1msRun,
    .iterations = 100,
};

const time_cases = [_]bench.CBenchCase{
    bench.caseAudited("time", time_case_get_us),
    bench.caseAudited("time", time_case_delay_1ms),
};

const time_suite = bench.makeSuite(.{
    .name = "time",
    .enabled = &timeIsEnabled,
    .cases = &time_cases,
});

comptime {
    @export(&time_suite, .{ .name = "bench_suite_time" });
}

// =========================================================================
// Suite: thread
// =========================================================================

var thread_bench_th: ?ove.Thread = null;
var thread_ping_sem: ?ove.Semaphore = null;
var thread_pong_sem: ?ove.Semaphore = null;
var thread_ctx_switch_done: volatile_int = volatile_int.init(0);

fn dummyThread() void {}

fn threadCreateDestroyRun() void {
    var th = ove.Thread.spawn("bench_tmp", dummyThread, ove.thread.prio.low, 1024) catch return;
    th.destroy();
}

fn threadYieldRun() void {
    ove.Thread.yieldCpu();
}

fn threadSleep1msRun() void {
    ove.Thread.sleepMs(1);
}

fn pongThread() void {
    while (thread_ctx_switch_done.load(.acquire) == 0) {
        thread_pong_sem.?.take(ove.wait_forever) catch {};
        thread_ping_sem.?.give();
    }
}

fn ctxSwitchSetup() void {
    thread_ctx_switch_done.store(0, .release);
    thread_ping_sem = ove.Semaphore.create(0, 1) catch return;
    thread_pong_sem = ove.Semaphore.create(0, 1) catch return;
    thread_bench_th = ove.Thread.spawn("pong", pongThread, ove.thread.prio.normal, 2048) catch return;
}

fn ctxSwitchRun() void {
    thread_pong_sem.?.give();
    thread_ping_sem.?.take(ove.wait_forever) catch {};
}

fn ctxSwitchTeardown() void {
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

fn threadIsEnabled() bool {
    return true;
}

const thread_case_create_destroy = bench.CaseSpec{
    .name = "create_destroy",
    .kind = .latency,
    .run = &threadCreateDestroyRun,
    .iterations = 200,
};
const thread_case_yield = bench.CaseSpec{
    .name = "yield",
    .kind = .latency,
    .run = &threadYieldRun,
};
const thread_case_sleep_1ms = bench.CaseSpec{
    .name = "sleep_1ms",
    .kind = .latency,
    .run = &threadSleep1msRun,
    .iterations = 100,
};
const thread_case_ctx_switch = bench.CaseSpec{
    .name = "context_switch",
    .kind = .latency,
    .run = &ctxSwitchRun,
    .setup = &ctxSwitchSetup,
    .teardown = &ctxSwitchTeardown,
    .iterations = 500,
};

const thread_cases = [_]bench.CBenchCase{
    bench.caseAudited("thread", thread_case_create_destroy),
    bench.caseAudited("thread", thread_case_yield),
    bench.caseAudited("thread", thread_case_sleep_1ms),
    bench.caseAudited("thread", thread_case_ctx_switch),
};

const thread_suite = bench.makeSuite(.{
    .name = "thread",
    .enabled = &threadIsEnabled,
    .cases = &thread_cases,
});

comptime {
    @export(&thread_suite, .{ .name = "bench_suite_thread" });
}

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
var sync_bench_evt_ack: ?ove.Event = null;
var sync_cv_th: ?ove.Thread = null;
var sync_cv_done: volatile_int = volatile_int.init(0);
var sync_mem_mutex: ?ove.Mutex = null;
var sync_mem_sem: ?ove.Semaphore = null;
var sync_mem_event: ?ove.Event = null;
var sync_mem_condvar: ?ove.CondVar = null;

// --- Mutex lock/unlock ---

fn mutexLockUnlockSetup() void {
    sync_bench_mtx = ove.Mutex.create() catch return;
}
fn mutexLockUnlockRun() void {
    sync_bench_mtx.?.lock(ove.wait_forever) catch {};
    sync_bench_mtx.?.unlock();
}
fn mutexLockUnlockTeardown() void {
    if (sync_bench_mtx) |*m| m.destroy();
    sync_bench_mtx = null;
}

// --- Mutex create/destroy ---

fn mutexCreateDestroyRun() void {
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
fn mutexContentionSetup() void {
    sync_contention_done.store(0, .release);
    sync_contention_count.store(0, .release);
    sync_bench_mtx = ove.Mutex.create() catch return;
    sync_contention_th = ove.Thread.spawn("contention", contentionThread, ove.thread.prio.normal, 2048) catch return;
}
fn mutexContentionRun() void {
    sync_bench_mtx.?.lock(ove.wait_forever) catch {};
    _ = sync_contention_count.fetchAdd(1, .monotonic);
    sync_bench_mtx.?.unlock();
}
fn mutexContentionTeardown() void {
    sync_contention_done.store(1, .release);
    ove.Thread.sleepMs(10);
    if (sync_contention_th) |*t| t.destroy();
    sync_contention_th = null;
    if (sync_bench_mtx) |*m| m.destroy();
    sync_bench_mtx = null;
}

// --- Mutex memory ---

fn mutexMemoryRun() void {
    sync_mem_mutex = ove.Mutex.create() catch return;
}
fn mutexMemoryTeardown() void {
    if (sync_mem_mutex) |*m| m.destroy();
    sync_mem_mutex = null;
}

// --- Semaphore take/give ---

fn semTakeGiveSetup() void {
    sync_bench_sem = ove.Semaphore.create(1, 1) catch return;
}
fn semTakeGiveRun() void {
    sync_bench_sem.?.take(ove.wait_forever) catch {};
    sync_bench_sem.?.give();
}
fn semTakeGiveTeardown() void {
    if (sync_bench_sem) |*s| s.destroy();
    sync_bench_sem = null;
}

// --- Semaphore create/destroy ---

fn semCreateDestroyRun() void {
    var s = ove.Semaphore.create(0, 1) catch return;
    s.destroy();
}

// --- Semaphore memory ---

fn semMemoryRun() void {
    sync_mem_sem = ove.Semaphore.create(0, 1) catch return;
}
fn semMemoryTeardown() void {
    if (sync_mem_sem) |*s| s.destroy();
    sync_mem_sem = null;
}

// --- Event signal/wait ---

fn evtSignaler() void {
    while (sync_evt_done.load(.acquire) == 0) {
        sync_bench_evt.?.signal();
        sync_bench_evt_ack.?.wait(ove.wait_forever) catch {};
    }
}
fn eventSignalWaitSetup() void {
    sync_evt_done.store(0, .release);
    sync_bench_evt = ove.Event.create() catch return;
    sync_bench_evt_ack = ove.Event.create() catch return;
    sync_evt_th = ove.Thread.spawn("evt_sig", evtSignaler, ove.thread.prio.normal, 1024) catch return;
}
fn eventSignalWaitRun() void {
    sync_bench_evt.?.wait(ove.wait_forever) catch {};
    sync_bench_evt_ack.?.signal();
}
fn eventSignalWaitTeardown() void {
    sync_evt_done.store(1, .release);
    if (sync_bench_evt_ack) |*e| e.signal();
    ove.Thread.sleepMs(10);
    if (sync_evt_th) |*t| t.destroy();
    sync_evt_th = null;
    if (sync_bench_evt) |*e| e.destroy();
    sync_bench_evt = null;
    if (sync_bench_evt_ack) |*e| e.destroy();
    sync_bench_evt_ack = null;
}

// --- Event memory ---

fn eventMemoryRun() void {
    sync_mem_event = ove.Event.create() catch return;
}
fn eventMemoryTeardown() void {
    if (sync_mem_event) |*e| e.destroy();
    sync_mem_event = null;
}

// --- Condvar signal/wait ---
//
// Condvar uses yield-based signaler + bounded cv_wait timeout — see
// bench_sync.c for why an ack-pattern signaler deadlocks here.

fn cvSignaler() void {
    while (sync_cv_done.load(.acquire) == 0) {
        sync_bench_cv.?.signal();
        ove.Thread.yieldCpu();
    }
}
fn condvarSignalWaitSetup() void {
    sync_cv_done.store(0, .release);
    sync_bench_cv_mtx = ove.Mutex.create() catch return;
    sync_bench_cv = ove.CondVar.create() catch return;
    sync_cv_th = ove.Thread.spawn("cv_sig", cvSignaler, ove.thread.prio.normal, 1024) catch return;
}
fn condvarSignalWaitRun() void {
    sync_bench_cv_mtx.?.lock(ove.wait_forever) catch {};
    sync_bench_cv.?.wait(sync_bench_cv_mtx.?, 10) catch {};
    sync_bench_cv_mtx.?.unlock();
}
fn condvarSignalWaitTeardown() void {
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

fn condvarMemoryRun() void {
    sync_mem_condvar = ove.CondVar.create() catch return;
}
fn condvarMemoryTeardown() void {
    if (sync_mem_condvar) |*cv| cv.destroy();
    sync_mem_condvar = null;
}

// --- Recursive mutex ---

fn rmtxLockUnlockSetup() void {
    sync_bench_rmtx = ove.RecursiveMutex.create() catch return;
}
fn rmtxLockUnlockRun() void {
    sync_bench_rmtx.?.lock(ove.wait_forever) catch {};
    sync_bench_rmtx.?.unlock();
}
fn rmtxLockUnlockTeardown() void {
    if (sync_bench_rmtx) |*m| m.destroy();
    sync_bench_rmtx = null;
}

fn syncIsEnabled() bool {
    return true;
}

const sync_case_mutex_memory = bench.CaseSpec{ .name = "mutex_memory", .kind = .memory, .run = &mutexMemoryRun, .teardown = &mutexMemoryTeardown };
const sync_case_sem_memory = bench.CaseSpec{ .name = "sem_memory", .kind = .memory, .run = &semMemoryRun, .teardown = &semMemoryTeardown };
const sync_case_event_memory = bench.CaseSpec{ .name = "event_memory", .kind = .memory, .run = &eventMemoryRun, .teardown = &eventMemoryTeardown };
const sync_case_condvar_memory = bench.CaseSpec{ .name = "condvar_memory", .kind = .memory, .run = &condvarMemoryRun, .teardown = &condvarMemoryTeardown };
const sync_case_mutex_lock_unlock = bench.CaseSpec{ .name = "mutex_lock_unlock", .kind = .latency, .run = &mutexLockUnlockRun, .setup = &mutexLockUnlockSetup, .teardown = &mutexLockUnlockTeardown };
const sync_case_mutex_create_destroy = bench.CaseSpec{ .name = "mutex_create_destroy", .kind = .latency, .run = &mutexCreateDestroyRun };
const sync_case_mutex_contention = bench.CaseSpec{ .name = "mutex_contention_2t", .kind = .throughput, .run = &mutexContentionRun, .setup = &mutexContentionSetup, .teardown = &mutexContentionTeardown };
const sync_case_sem_take_give = bench.CaseSpec{ .name = "sem_take_give", .kind = .latency, .run = &semTakeGiveRun, .setup = &semTakeGiveSetup, .teardown = &semTakeGiveTeardown };
const sync_case_sem_create_destroy = bench.CaseSpec{ .name = "sem_create_destroy", .kind = .latency, .run = &semCreateDestroyRun };
const sync_case_event_signal_wait = bench.CaseSpec{ .name = "event_signal_wait", .kind = .latency, .run = &eventSignalWaitRun, .setup = &eventSignalWaitSetup, .teardown = &eventSignalWaitTeardown, .iterations = 500 };
const sync_case_condvar_signal_wait = bench.CaseSpec{ .name = "condvar_signal_wait", .kind = .latency, .run = &condvarSignalWaitRun, .setup = &condvarSignalWaitSetup, .teardown = &condvarSignalWaitTeardown, .iterations = 500 };
const sync_case_rmtx_lock_unlock = bench.CaseSpec{ .name = "recursive_mutex_lock_unlock", .kind = .latency, .run = &rmtxLockUnlockRun, .setup = &rmtxLockUnlockSetup, .teardown = &rmtxLockUnlockTeardown };

const sync_cases = [_]bench.CBenchCase{
    bench.caseAudited("sync", sync_case_mutex_memory),
    bench.caseAudited("sync", sync_case_sem_memory),
    bench.caseAudited("sync", sync_case_event_memory),
    bench.caseAudited("sync", sync_case_condvar_memory),
    bench.caseAudited("sync", sync_case_mutex_lock_unlock),
    bench.caseAudited("sync", sync_case_mutex_create_destroy),
    bench.caseAudited("sync", sync_case_mutex_contention),
    bench.caseAudited("sync", sync_case_sem_take_give),
    bench.caseAudited("sync", sync_case_sem_create_destroy),
    bench.caseAudited("sync", sync_case_event_signal_wait),
    bench.caseAudited("sync", sync_case_condvar_signal_wait),
    bench.caseAudited("sync", sync_case_rmtx_lock_unlock),
};

const sync_suite = bench.makeSuite(.{
    .name = "sync",
    .enabled = &syncIsEnabled,
    .cases = &sync_cases,
});

comptime {
    @export(&sync_suite, .{ .name = "bench_suite_sync" });
}

// =========================================================================
// Suite: queue
// =========================================================================

var queue_send_recv_q: ?ove.Queue(u32, 16) = null;
var queue_throughput_q: ?ove.Queue(u32, 64) = null;
var queue_producer_th: ?ove.Thread = null;
var queue_throughput_done: volatile_int = volatile_int.init(0);
var queue_mem_q: ?ove.Queue(u32, 8) = null;

fn queueSendRecvSetup() void {
    queue_send_recv_q = ove.Queue(u32, 16).create() catch return;
}
fn queueSendRecvRun() void {
    var val: u32 = 42;
    queue_send_recv_q.?.send(&val, ove.wait_forever) catch return;
    _ = queue_send_recv_q.?.receive(ove.wait_forever) catch return;
}
fn queueSendRecvTeardown() void {
    if (queue_send_recv_q) |*q| q.destroy();
    queue_send_recv_q = null;
}

fn queueCreateDestroyRun() void {
    var q = ove.Queue(u32, 8).create() catch return;
    q.destroy();
}

fn queueProducerThread() void {
    var val: u32 = 0;
    while (queue_throughput_done.load(.acquire) == 0) {
        queue_throughput_q.?.send(&val, ove.wait_forever) catch {};
        val +%= 1;
    }
}
fn queueThroughputSetup() void {
    queue_throughput_done.store(0, .release);
    queue_throughput_q = ove.Queue(u32, 64).create() catch return;
    queue_producer_th = ove.Thread.spawn("q_prod", queueProducerThread, ove.thread.prio.normal, 2048) catch return;
}
fn queueThroughputRun() void {
    _ = queue_throughput_q.?.receive(ove.wait_forever) catch return;
}
fn queueThroughputTeardown() void {
    queue_throughput_done.store(1, .release);
    _ = queue_throughput_q.?.receive(100) catch 0;
    ove.Thread.sleepMs(10);
    if (queue_producer_th) |*t| t.destroy();
    queue_producer_th = null;
    if (queue_throughput_q) |*q| q.destroy();
    queue_throughput_q = null;
}

fn queueMemoryRun() void {
    queue_mem_q = ove.Queue(u32, 8).create() catch return;
}
fn queueMemoryTeardown() void {
    if (queue_mem_q) |*q| q.destroy();
    queue_mem_q = null;
}

fn queueIsEnabled() bool {
    return true;
}

const queue_case_memory = bench.CaseSpec{ .name = "memory", .kind = .memory, .run = &queueMemoryRun, .teardown = &queueMemoryTeardown };
const queue_case_send_receive = bench.CaseSpec{ .name = "send_receive", .kind = .latency, .run = &queueSendRecvRun, .setup = &queueSendRecvSetup, .teardown = &queueSendRecvTeardown };
const queue_case_create_destroy = bench.CaseSpec{ .name = "create_destroy", .kind = .latency, .run = &queueCreateDestroyRun };
const queue_case_throughput = bench.CaseSpec{ .name = "throughput_2t", .kind = .throughput, .run = &queueThroughputRun, .setup = &queueThroughputSetup, .teardown = &queueThroughputTeardown };

const queue_cases = [_]bench.CBenchCase{
    bench.caseAudited("queue", queue_case_memory),
    bench.caseAudited("queue", queue_case_send_receive),
    bench.caseAudited("queue", queue_case_create_destroy),
    bench.caseAudited("queue", queue_case_throughput),
};

const queue_suite = bench.makeSuite(.{
    .name = "queue",
    .enabled = &queueIsEnabled,
    .cases = &queue_cases,
});

comptime {
    @export(&queue_suite, .{ .name = "bench_suite_queue" });
}

// =========================================================================
// Suite: timer
// =========================================================================

var timer_bench_tmr: ?ove.Timer = null;
var timer_mem_tmr: ?ove.Timer = null;

fn timerDummyCb() void {}

fn timerCreateDestroyRun() void {
    var t = ove.Timer.create(timerDummyCb, 1000, false) catch return;
    t.destroy();
}

fn timerStartStopSetup() void {
    timer_bench_tmr = ove.Timer.create(timerDummyCb, 1000, false) catch return;
}
fn timerStartStopRun() void {
    timer_bench_tmr.?.start() catch {};
    timer_bench_tmr.?.stop() catch {};
}
fn timerStartStopTeardown() void {
    if (timer_bench_tmr) |*t| t.destroy();
    timer_bench_tmr = null;
}

fn timerMemoryRun() void {
    timer_mem_tmr = ove.Timer.create(timerDummyCb, 1000, false) catch return;
}
fn timerMemoryTeardown() void {
    if (timer_mem_tmr) |*t| t.destroy();
    timer_mem_tmr = null;
}

fn timerIsEnabled() bool {
    return true;
}

const timer_case_memory = bench.CaseSpec{ .name = "memory", .kind = .memory, .run = &timerMemoryRun, .teardown = &timerMemoryTeardown };
const timer_case_create_destroy = bench.CaseSpec{ .name = "create_destroy", .kind = .latency, .run = &timerCreateDestroyRun };
const timer_case_start_stop = bench.CaseSpec{ .name = "start_stop", .kind = .latency, .run = &timerStartStopRun, .setup = &timerStartStopSetup, .teardown = &timerStartStopTeardown };

const timer_cases = [_]bench.CBenchCase{
    bench.caseAudited("timer", timer_case_memory),
    bench.caseAudited("timer", timer_case_create_destroy),
    bench.caseAudited("timer", timer_case_start_stop),
};

const timer_suite = bench.makeSuite(.{
    .name = "timer",
    .enabled = &timerIsEnabled,
    .cases = &timer_cases,
});

comptime {
    @export(&timer_suite, .{ .name = "bench_suite_timer" });
}

// =========================================================================
// Suite: eventgroup
// =========================================================================

var eg_bench_eg: ?ove.EventGroup = null;
var eg_mem_eg: ?ove.EventGroup = null;

fn egSetGetSetup() void {
    eg_bench_eg = ove.EventGroup.create() catch return;
}
fn egSetGetRun() void {
    _ = eg_bench_eg.?.setBits(0x01);
    _ = eg_bench_eg.?.getBits();
    _ = eg_bench_eg.?.clearBits(0x01);
}
fn egSetGetTeardown() void {
    if (eg_bench_eg) |*eg| eg.destroy();
    eg_bench_eg = null;
}

fn egCreateDestroyRun() void {
    var eg = ove.EventGroup.create() catch return;
    eg.destroy();
}

fn egMemoryRun() void {
    eg_mem_eg = ove.EventGroup.create() catch return;
}
fn egMemoryTeardown() void {
    if (eg_mem_eg) |*eg| eg.destroy();
    eg_mem_eg = null;
}

fn eventgroupIsEnabled() bool {
    return true;
}

const eg_case_memory = bench.CaseSpec{ .name = "memory", .kind = .memory, .run = &egMemoryRun, .teardown = &egMemoryTeardown };
const eg_case_set_get = bench.CaseSpec{ .name = "set_get_bits", .kind = .latency, .run = &egSetGetRun, .setup = &egSetGetSetup, .teardown = &egSetGetTeardown };
const eg_case_create_destroy = bench.CaseSpec{ .name = "create_destroy", .kind = .latency, .run = &egCreateDestroyRun };

const eg_cases = [_]bench.CBenchCase{
    bench.caseAudited("eventgroup", eg_case_memory),
    bench.caseAudited("eventgroup", eg_case_set_get),
    bench.caseAudited("eventgroup", eg_case_create_destroy),
};

const eg_suite = bench.makeSuite(.{
    .name = "eventgroup",
    .enabled = &eventgroupIsEnabled,
    .cases = &eg_cases,
});

comptime {
    @export(&eg_suite, .{ .name = "bench_suite_eventgroup" });
}

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

fn wqCreateDestroyRun() void {
    var wq = ove.Workqueue.create("bench_wq", ove.thread.prio.normal, 2048) catch return;
    wq.destroy();
}

fn wqSubmitSetup() void {
    wq_work_sem = ove.Semaphore.create(0, 1) catch return;
    wq_bench_wq = ove.Workqueue.create("bench_wq", ove.thread.prio.normal, 2048) catch return;
    wq_bench_work = ove.Work.create(workHandler) catch return;
}
fn wqSubmitRun() void {
    wq_work_executed.store(0, .release);
    wq_bench_wq.?.submit(wq_bench_work.?) catch {};
    wq_work_sem.?.take(1000) catch {};
}
fn wqSubmitTeardown() void {
    if (wq_bench_work) |*w| w.free();
    wq_bench_work = null;
    if (wq_bench_wq) |*w| w.destroy();
    wq_bench_wq = null;
    if (wq_work_sem) |*s| s.destroy();
    wq_work_sem = null;
}

fn wqMemoryRun() void {
    wq_mem_wq = ove.Workqueue.create("bench_wq", ove.thread.prio.normal, 2048) catch return;
}
fn wqMemoryTeardown() void {
    if (wq_mem_wq) |*w| w.destroy();
    wq_mem_wq = null;
}

fn workqueueIsEnabled() bool {
    return true;
}

const wq_case_memory = bench.CaseSpec{ .name = "memory", .kind = .memory, .run = &wqMemoryRun, .teardown = &wqMemoryTeardown };
const wq_case_create_destroy = bench.CaseSpec{ .name = "create_destroy", .kind = .latency, .run = &wqCreateDestroyRun, .iterations = 200 };
const wq_case_submit_execute = bench.CaseSpec{ .name = "submit_execute", .kind = .latency, .run = &wqSubmitRun, .setup = &wqSubmitSetup, .teardown = &wqSubmitTeardown, .iterations = 500 };

const wq_cases = [_]bench.CBenchCase{
    bench.caseAudited("workqueue", wq_case_memory),
    bench.caseAudited("workqueue", wq_case_create_destroy),
    bench.caseAudited("workqueue", wq_case_submit_execute),
};

const wq_suite = bench.makeSuite(.{
    .name = "workqueue",
    .enabled = &workqueueIsEnabled,
    .cases = &wq_cases,
});

comptime {
    @export(&wq_suite, .{ .name = "bench_suite_workqueue" });
}

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

fn streamSendRecvSetup() void {
    stream_bench_strm = ove.Stream.create(STREAM_BUF_SIZE, 1) catch return;
    @memset(&stream_tx_buf, 0xAA);
}
fn streamSendRecvRun() void {
    _ = stream_bench_strm.?.send(&stream_tx_buf, ove.wait_forever) catch return;
    _ = stream_bench_strm.?.receive(&stream_rx_buf, ove.wait_forever) catch return;
}
fn streamSendRecvTeardown() void {
    if (stream_bench_strm) |*s| s.destroy();
    stream_bench_strm = null;
}

fn streamCreateDestroyRun() void {
    var s = ove.Stream.create(STREAM_BUF_SIZE, 1) catch return;
    s.destroy();
}

fn streamProducer() void {
    while (stream_done.load(.acquire) == 0) {
        _ = stream_bench_strm.?.send(&stream_tx_buf, ove.wait_forever) catch {};
    }
}
fn streamThroughputSetup() void {
    stream_done.store(0, .release);
    @memset(&stream_tx_buf, 0xBB);
    stream_bench_strm = ove.Stream.create(STREAM_BUF_SIZE, 1) catch return;
    stream_producer_th = ove.Thread.spawn("strm_prod", streamProducer, ove.thread.prio.normal, 2048) catch return;
}
fn streamThroughputRun() void {
    _ = stream_bench_strm.?.receive(&stream_rx_buf, ove.wait_forever) catch return;
}
fn streamThroughputTeardown() void {
    stream_done.store(1, .release);
    _ = stream_bench_strm.?.receive(&stream_rx_buf, 100) catch 0;
    ove.Thread.sleepMs(10);
    if (stream_producer_th) |*t| t.destroy();
    stream_producer_th = null;
    if (stream_bench_strm) |*s| s.destroy();
    stream_bench_strm = null;
}

fn streamMemoryRun() void {
    stream_mem_strm = ove.Stream.create(STREAM_BUF_SIZE, 1) catch return;
}
fn streamMemoryTeardown() void {
    if (stream_mem_strm) |*s| s.destroy();
    stream_mem_strm = null;
}

fn streamIsEnabled() bool {
    return true;
}

const stream_case_memory = bench.CaseSpec{ .name = "memory", .kind = .memory, .run = &streamMemoryRun, .teardown = &streamMemoryTeardown };
const stream_case_send_recv = bench.CaseSpec{ .name = "send_recv_64B", .kind = .latency, .run = &streamSendRecvRun, .setup = &streamSendRecvSetup, .teardown = &streamSendRecvTeardown };
const stream_case_create_destroy = bench.CaseSpec{ .name = "create_destroy", .kind = .latency, .run = &streamCreateDestroyRun };
const stream_case_throughput = bench.CaseSpec{ .name = "throughput", .kind = .throughput, .run = &streamThroughputRun, .setup = &streamThroughputSetup, .teardown = &streamThroughputTeardown };

const stream_cases = [_]bench.CBenchCase{
    bench.caseAudited("stream", stream_case_memory),
    bench.caseAudited("stream", stream_case_send_recv),
    bench.caseAudited("stream", stream_case_create_destroy),
    bench.caseAudited("stream", stream_case_throughput),
};

const stream_suite = bench.makeSuite(.{
    .name = "stream",
    .enabled = &streamIsEnabled,
    .cases = &stream_cases,
});

comptime {
    @export(&stream_suite, .{ .name = "bench_suite_stream" });
}

// =========================================================================
// Suite registry & runner
// =========================================================================

// bench_suite_native_posix / bench_suite_native_freertos live C-side
// (tests/benchmarks/c/src/bench_native_{posix,freertos}.c).  Each suite's
// is_enabled returns 0 on the wrong backend so only the active baseline
// runs at runtime.
extern const bench_suite_native_posix: bench.CBenchSuite;
extern const bench_suite_native_freertos: bench.CBenchSuite;
extern const bench_suite_native_nuttx: bench.CBenchSuite;
extern const bench_suite_native_zephyr: bench.CBenchSuite;

const suites = [_]*const bench.CBenchSuite{
    &time_suite,
    &thread_suite,
    &sync_suite,
    &queue_suite,
    &timer_suite,
    &eg_suite,
    &wq_suite,
    &stream_suite,
    &bench_suite_native_posix,
    &bench_suite_native_freertos,
    &bench_suite_native_nuttx,
    &bench_suite_native_zephyr,
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
        bench.runSuite(suite);
    }

    ove.log.inf("=== Benchmark complete ===", .{});
}

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
