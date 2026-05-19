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
//!
//! All primitives use the modernised allocator-aware API:
//!     var x: ove.Foo = undefined;
//!     x = try ove.Foo.create(bench_allocator, ...);
//!     defer x.deinit();
//!
//! Both heap and zero-heap modes share the same call shape; only the
//! backing allocator differs (page_allocator vs FixedBufferAllocator
//! over a static BSS arena).

const std = @import("std");
const ove = @import("ove");
const bench = @import("bench.zig");
pub const cyccnt = @import("bench_cyccnt.zig");

pub const std_options: std.Options = .{ .logFn = ove.log.logFn };

const volatile_int = std.atomic.Value(i32);

/// Comptime-resolved zero-heap mode flag.  `*_create_destroy` and
/// `*_memory` cases are gated out under zero-heap because the heap is
/// locked at boot and the create/destroy API isn't generated.
const is_zero_heap = @hasDecl(ove.ffi, "CONFIG_OVE_ZERO_HEAP");

// Static-backed allocator works for both heap and zero-heap modes.
// page_allocator would mmap in heap mode; FBA over BSS keeps every
// allocation in caller-owned static memory regardless of mode and
// avoids the substrate's libc-malloc heap entirely (which the bench
// doesn't measure).
var bench_arena_bytes: [16 * 1024]u8 = undefined;
var bench_fba: std.heap.FixedBufferAllocator = undefined;
var bench_allocator_initialised: bool = false;

fn benchAllocator() std.mem.Allocator {
    if (!bench_allocator_initialised) {
        bench_fba = std.heap.FixedBufferAllocator.init(&bench_arena_bytes);
        bench_allocator_initialised = true;
    }
    return bench_fba.allocator();
}

/// Helper for primitives whose `create(allocator, args...)` signature
/// matches the same `args...` tuple in both modes (Mutex / Sem / Event /
/// CondVar / Queue / EventGroup / Stream).
fn createInPlace(obj: anytype, args: anytype) !void {
    const T = @TypeOf(obj.*);
    obj.* = try @call(.auto, T.create, .{benchAllocator()} ++ args);
}

/// Thread-specific spawn helper: forwards to `Thread(N).spawn(allocator,
/// cfg, entry, .{})`.  Bench threads have no entry-fn arguments, so the
/// args tuple is always `.{}`.
fn spawnThreadInPlace(
    obj: anytype,
    comptime name: [:0]const u8,
    comptime priority: ove.Priority,
    comptime entry: fn () void,
) !void {
    const T = @TypeOf(obj.*);
    obj.* = try T.spawn(benchAllocator(), .{ .name = name, .priority = priority }, entry, .{});
}

/// Timer create helper: `Timer.create(allocator, cfg, callback, args)`.
fn createTimerInPlace(
    obj: *ove.Timer,
    comptime callback: anytype,
    comptime period_ms: u32,
    comptime mode: ove.timer.Mode,
) !void {
    obj.* = try ove.Timer.create(
        benchAllocator(),
        .{ .period_ms = period_ms, .mode = mode },
        callback,
        .{},
    );
}

/// Workqueue create helper: `Workqueue(N).create(allocator, name, priority)`.
fn createWqInPlace(
    obj: anytype,
    comptime name: [:0]const u8,
    comptime priority: ove.Priority,
) !void {
    const T = @TypeOf(obj.*);
    obj.* = try T.create(benchAllocator(), name, priority);
}

/// Work create helper: `Work.create(allocator, handler)`.
fn createWorkInPlace(obj: *ove.Work, comptime handler: anytype) !void {
    obj.* = try ove.Work.create(benchAllocator(), handler);
}

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

var thread_bench_th: ove.Thread(2048) = undefined;
var thread_bench_th_in: bool = false;
var thread_ping_sem: ove.Semaphore = undefined;
var thread_ping_sem_in: bool = false;
var thread_pong_sem: ove.Semaphore = undefined;
var thread_pong_sem_in: bool = false;
var thread_ctx_switch_done: volatile_int = volatile_int.init(0);

fn dummyThread() void {}

fn threadCreateDestroyRun() void {
    var th: ove.Thread(1024) = undefined;
    spawnThreadInPlace(&th, "bench_tmp", .low, dummyThread) catch return;
    th.deinit();
}

fn threadYieldRun() void {
    ove.thread.yieldCpu();
}

// Pure "who am I?" query — kernel-side TLS read with no scheduling
// side-effects.  Distinct from time_get_us_overhead and yield.
fn threadGetSelfRun() void {
    std.mem.doNotOptimizeAway(ove.thread.getSelf());
}

fn threadSleep1msRun() void {
    ove.thread.sleepMs(1);
}

fn pongThread() void {
    while (thread_ctx_switch_done.load(.acquire) == 0) {
        thread_pong_sem.wait();
        thread_ping_sem.post();
    }
}

fn ctxSwitchSetup() void {
    thread_ctx_switch_done.store(0, .release);
    thread_ping_sem = undefined;
    createInPlace(&thread_ping_sem, .{ 0, 1 }) catch return;
    thread_ping_sem_in = true;
    thread_pong_sem = undefined;
    createInPlace(&thread_pong_sem, .{ 0, 1 }) catch return;
    thread_pong_sem_in = true;
    thread_bench_th = undefined;
    spawnThreadInPlace(&thread_bench_th, "pong", .normal, pongThread) catch return;
    thread_bench_th_in = true;
}

fn ctxSwitchRun() void {
    thread_pong_sem.post();
    thread_ping_sem.wait();
}

fn ctxSwitchTeardown() void {
    thread_ctx_switch_done.store(1, .release);
    thread_pong_sem.post();
    ove.thread.sleepMs(10);
    if (thread_bench_th_in) thread_bench_th.deinit();
    thread_bench_th_in = false;
    if (thread_ping_sem_in) thread_ping_sem.deinit();
    thread_ping_sem_in = false;
    if (thread_pong_sem_in) thread_pong_sem.deinit();
    thread_pong_sem_in = false;
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
const thread_case_get_self = bench.CaseSpec{
    .name = "get_self",
    .kind = .latency,
    .run = &threadGetSelfRun,
};
const thread_case_ctx_switch = bench.CaseSpec{
    .name = "context_switch",
    .kind = .latency,
    .run = &ctxSwitchRun,
    .setup = &ctxSwitchSetup,
    .teardown = &ctxSwitchTeardown,
    .iterations = 500,
};

const thread_cases_full = [_]bench.CBenchCase{
    bench.caseAudited("thread", thread_case_create_destroy),
    bench.caseAudited("thread", thread_case_yield),
    bench.caseAudited("thread", thread_case_get_self),
    bench.caseAudited("thread", thread_case_sleep_1ms),
    bench.caseAudited("thread", thread_case_ctx_switch),
};
const thread_cases_zh = [_]bench.CBenchCase{
    bench.caseAudited("thread", thread_case_yield),
    bench.caseAudited("thread", thread_case_get_self),
    bench.caseAudited("thread", thread_case_sleep_1ms),
    bench.caseAudited("thread", thread_case_ctx_switch),
};
const thread_cases: []const bench.CBenchCase =
    if (is_zero_heap) &thread_cases_zh else &thread_cases_full;

const thread_suite = bench.makeSuite(.{
    .name = "thread",
    .enabled = &threadIsEnabled,
    .cases = thread_cases,
});

comptime {
    @export(&thread_suite, .{ .name = "bench_suite_thread" });
}

// =========================================================================
// Suite: sync
// =========================================================================

var sync_bench_mtx: ove.Mutex = undefined;
var sync_bench_mtx_in: bool = false;
var sync_bench_sem: ove.Semaphore = undefined;
var sync_bench_sem_in: bool = false;
var sync_bench_evt: ove.Event = undefined;
var sync_bench_evt_in: bool = false;
var sync_bench_evt_ack: ove.Event = undefined;
var sync_bench_evt_ack_in: bool = false;
var sync_bench_cv: ove.CondVar = undefined;
var sync_bench_cv_in: bool = false;
var sync_bench_cv_mtx: ove.Mutex = undefined;
var sync_bench_cv_mtx_in: bool = false;
var sync_bench_rmtx: ove.RecursiveMutex = undefined;
var sync_bench_rmtx_in: bool = false;
var sync_contention_th: ove.Thread(2048) = undefined;
var sync_contention_th_in: bool = false;
var sync_contention_done: volatile_int = volatile_int.init(0);
var sync_contention_count: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);
var sync_evt_th: ove.Thread(1024) = undefined;
var sync_evt_th_in: bool = false;
var sync_evt_done: volatile_int = volatile_int.init(0);
var sync_cv_th: ove.Thread(1024) = undefined;
var sync_cv_th_in: bool = false;
var sync_cv_done: volatile_int = volatile_int.init(0);
var sync_mem_mutex: ove.Mutex = undefined;
var sync_mem_mutex_in: bool = false;
var sync_mem_sem: ove.Semaphore = undefined;
var sync_mem_sem_in: bool = false;
var sync_mem_event: ove.Event = undefined;
var sync_mem_event_in: bool = false;
var sync_mem_condvar: ove.CondVar = undefined;
var sync_mem_condvar_in: bool = false;

// --- Mutex lock/unlock ---

fn mutexLockUnlockSetup() void {
    sync_bench_mtx = undefined;
    createInPlace(&sync_bench_mtx, .{}) catch return;
    sync_bench_mtx_in = true;
}
fn mutexLockUnlockRun() void {
    sync_bench_mtx.lock();
    sync_bench_mtx.unlock();
}
fn mutexLockUnlockTeardown() void {
    if (sync_bench_mtx_in) sync_bench_mtx.deinit();
    sync_bench_mtx_in = false;
}

// --- Mutex create/destroy ---

fn mutexCreateDestroyRun() void {
    var m: ove.Mutex = undefined;
    createInPlace(&m, .{}) catch return;
    m.deinit();
}

// --- Mutex contention (2-thread throughput) ---

fn contentionThread() void {
    while (sync_contention_done.load(.acquire) == 0) {
        sync_bench_mtx.lock();
        _ = sync_contention_count.fetchAdd(1, .monotonic);
        sync_bench_mtx.unlock();
    }
}
fn mutexContentionSetup() void {
    sync_contention_done.store(0, .release);
    sync_contention_count.store(0, .release);
    sync_bench_mtx = undefined;
    createInPlace(&sync_bench_mtx, .{}) catch return;
    sync_bench_mtx_in = true;
    sync_contention_th = undefined;
    spawnThreadInPlace(&sync_contention_th, "contention", .normal, contentionThread) catch return;
    sync_contention_th_in = true;
}
fn mutexContentionRun() void {
    sync_bench_mtx.lock();
    _ = sync_contention_count.fetchAdd(1, .monotonic);
    sync_bench_mtx.unlock();
}
fn mutexContentionTeardown() void {
    sync_contention_done.store(1, .release);
    ove.thread.sleepMs(10);
    if (sync_contention_th_in) sync_contention_th.deinit();
    sync_contention_th_in = false;
    if (sync_bench_mtx_in) sync_bench_mtx.deinit();
    sync_bench_mtx_in = false;
}

// --- Mutex memory ---

fn mutexMemoryRun() void {
    sync_mem_mutex = undefined;
    createInPlace(&sync_mem_mutex, .{}) catch return;
    sync_mem_mutex_in = true;
}
fn mutexMemoryTeardown() void {
    if (sync_mem_mutex_in) sync_mem_mutex.deinit();
    sync_mem_mutex_in = false;
}

// --- Semaphore take/give ---

fn semTakeGiveSetup() void {
    sync_bench_sem = undefined;
    createInPlace(&sync_bench_sem, .{ 1, 1 }) catch return;
    sync_bench_sem_in = true;
}
fn semTakeGiveRun() void {
    sync_bench_sem.wait();
    sync_bench_sem.post();
}
fn semTakeGiveTeardown() void {
    if (sync_bench_sem_in) sync_bench_sem.deinit();
    sync_bench_sem_in = false;
}

// --- Semaphore create/destroy ---

fn semCreateDestroyRun() void {
    var s: ove.Semaphore = undefined;
    createInPlace(&s, .{ 0, 1 }) catch return;
    s.deinit();
}

// --- Semaphore memory ---

fn semMemoryRun() void {
    sync_mem_sem = undefined;
    createInPlace(&sync_mem_sem, .{ 0, 1 }) catch return;
    sync_mem_sem_in = true;
}
fn semMemoryTeardown() void {
    if (sync_mem_sem_in) sync_mem_sem.deinit();
    sync_mem_sem_in = false;
}

// --- Event signal/wait ---

fn evtSignaler() void {
    while (sync_evt_done.load(.acquire) == 0) {
        sync_bench_evt.signal();
        sync_bench_evt_ack.wait();
    }
}
fn eventSignalWaitSetup() void {
    sync_evt_done.store(0, .release);
    sync_bench_evt = undefined;
    createInPlace(&sync_bench_evt, .{}) catch return;
    sync_bench_evt_in = true;
    sync_bench_evt_ack = undefined;
    createInPlace(&sync_bench_evt_ack, .{}) catch return;
    sync_bench_evt_ack_in = true;
    sync_evt_th = undefined;
    spawnThreadInPlace(&sync_evt_th, "evt_sig", .normal, evtSignaler) catch return;
    sync_evt_th_in = true;
}
fn eventSignalWaitRun() void {
    sync_bench_evt.wait();
    sync_bench_evt_ack.signal();
}
fn eventSignalWaitTeardown() void {
    sync_evt_done.store(1, .release);
    if (sync_bench_evt_ack_in) sync_bench_evt_ack.signal();
    ove.thread.sleepMs(10);
    if (sync_evt_th_in) sync_evt_th.deinit();
    sync_evt_th_in = false;
    if (sync_bench_evt_in) sync_bench_evt.deinit();
    sync_bench_evt_in = false;
    if (sync_bench_evt_ack_in) sync_bench_evt_ack.deinit();
    sync_bench_evt_ack_in = false;
}

// --- Event memory ---

fn eventMemoryRun() void {
    sync_mem_event = undefined;
    createInPlace(&sync_mem_event, .{}) catch return;
    sync_mem_event_in = true;
}
fn eventMemoryTeardown() void {
    if (sync_mem_event_in) sync_mem_event.deinit();
    sync_mem_event_in = false;
}

// --- Condvar signal/wait ---

fn cvSignaler() void {
    while (sync_cv_done.load(.acquire) == 0) {
        sync_bench_cv.signal();
        ove.thread.yieldCpu();
    }
}
fn condvarSignalWaitSetup() void {
    sync_cv_done.store(0, .release);
    sync_bench_cv_mtx = undefined;
    createInPlace(&sync_bench_cv_mtx, .{}) catch return;
    sync_bench_cv_mtx_in = true;
    sync_bench_cv = undefined;
    createInPlace(&sync_bench_cv, .{}) catch return;
    sync_bench_cv_in = true;
    sync_cv_th = undefined;
    spawnThreadInPlace(&sync_cv_th, "cv_sig", .normal, cvSignaler) catch return;
    sync_cv_th_in = true;
}
fn condvarSignalWaitRun() void {
    sync_bench_cv_mtx.lock();
    sync_bench_cv.timedWait(sync_bench_cv_mtx, .millis(10)) catch {};
    sync_bench_cv_mtx.unlock();
}
fn condvarSignalWaitTeardown() void {
    sync_cv_done.store(1, .release);
    sync_bench_cv.signal();
    ove.thread.sleepMs(10);
    if (sync_cv_th_in) sync_cv_th.deinit();
    sync_cv_th_in = false;
    if (sync_bench_cv_in) sync_bench_cv.deinit();
    sync_bench_cv_in = false;
    if (sync_bench_cv_mtx_in) sync_bench_cv_mtx.deinit();
    sync_bench_cv_mtx_in = false;
}

// --- Condvar memory ---

fn condvarMemoryRun() void {
    sync_mem_condvar = undefined;
    createInPlace(&sync_mem_condvar, .{}) catch return;
    sync_mem_condvar_in = true;
}
fn condvarMemoryTeardown() void {
    if (sync_mem_condvar_in) sync_mem_condvar.deinit();
    sync_mem_condvar_in = false;
}

// --- Recursive mutex ---

fn rmtxLockUnlockSetup() void {
    sync_bench_rmtx = undefined;
    createInPlace(&sync_bench_rmtx, .{}) catch return;
    sync_bench_rmtx_in = true;
}
fn rmtxLockUnlockRun() void {
    sync_bench_rmtx.lock();
    sync_bench_rmtx.unlock();
}
fn rmtxLockUnlockTeardown() void {
    if (sync_bench_rmtx_in) sync_bench_rmtx.deinit();
    sync_bench_rmtx_in = false;
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

const sync_cases_full = [_]bench.CBenchCase{
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
const sync_cases_zh = [_]bench.CBenchCase{
    bench.caseAudited("sync", sync_case_mutex_lock_unlock),
    bench.caseAudited("sync", sync_case_mutex_contention),
    bench.caseAudited("sync", sync_case_sem_take_give),
    bench.caseAudited("sync", sync_case_event_signal_wait),
    bench.caseAudited("sync", sync_case_condvar_signal_wait),
    bench.caseAudited("sync", sync_case_rmtx_lock_unlock),
};
const sync_cases: []const bench.CBenchCase =
    if (is_zero_heap) &sync_cases_zh else &sync_cases_full;

const sync_suite = bench.makeSuite(.{
    .name = "sync",
    .enabled = &syncIsEnabled,
    .cases = sync_cases,
});

comptime {
    @export(&sync_suite, .{ .name = "bench_suite_sync" });
}

// =========================================================================
// Suite: queue
// =========================================================================

var queue_send_recv_q: ove.Queue(u32, 16) = undefined;
var queue_send_recv_q_in: bool = false;
var queue_throughput_q: ove.Queue(u32, 64) = undefined;
var queue_throughput_q_in: bool = false;
var queue_producer_th: ove.Thread(2048) = undefined;
var queue_producer_th_in: bool = false;
var queue_throughput_done: volatile_int = volatile_int.init(0);
var queue_mem_q: ove.Queue(u32, 8) = undefined;
var queue_mem_q_in: bool = false;

fn queueSendRecvSetup() void {
    queue_send_recv_q = undefined;
    createInPlace(&queue_send_recv_q, .{}) catch return;
    queue_send_recv_q_in = true;
}
fn queueSendRecvRun() void {
    var val: u32 = 42;
    queue_send_recv_q.send(&val);
    _ = queue_send_recv_q.recv();
}
fn queueSendRecvTeardown() void {
    if (queue_send_recv_q_in) queue_send_recv_q.deinit();
    queue_send_recv_q_in = false;
}

fn queueCreateDestroyRun() void {
    var q: ove.Queue(u32, 8) = undefined;
    createInPlace(&q, .{}) catch return;
    q.deinit();
}

fn queueProducerThread() void {
    var val: u32 = 0;
    while (queue_throughput_done.load(.acquire) == 0) {
        queue_throughput_q.send(&val);
        val +%= 1;
    }
}
fn queueThroughputSetup() void {
    queue_throughput_done.store(0, .release);
    queue_throughput_q = undefined;
    createInPlace(&queue_throughput_q, .{}) catch return;
    queue_throughput_q_in = true;
    queue_producer_th = undefined;
    spawnThreadInPlace(&queue_producer_th, "q_prod", .normal, queueProducerThread) catch return;
    queue_producer_th_in = true;
}
fn queueThroughputRun() void {
    _ = queue_throughput_q.recv();
}
fn queueThroughputTeardown() void {
    queue_throughput_done.store(1, .release);
    _ = queue_throughput_q.recvFor(.millis(100)) catch 0;
    ove.thread.sleepMs(10);
    if (queue_producer_th_in) queue_producer_th.deinit();
    queue_producer_th_in = false;
    if (queue_throughput_q_in) queue_throughput_q.deinit();
    queue_throughput_q_in = false;
}

fn queueMemoryRun() void {
    queue_mem_q = undefined;
    createInPlace(&queue_mem_q, .{}) catch return;
    queue_mem_q_in = true;
}
fn queueMemoryTeardown() void {
    if (queue_mem_q_in) queue_mem_q.deinit();
    queue_mem_q_in = false;
}

fn queueIsEnabled() bool {
    return true;
}

const queue_case_memory = bench.CaseSpec{ .name = "memory", .kind = .memory, .run = &queueMemoryRun, .teardown = &queueMemoryTeardown };
const queue_case_send_receive = bench.CaseSpec{ .name = "send_receive", .kind = .latency, .run = &queueSendRecvRun, .setup = &queueSendRecvSetup, .teardown = &queueSendRecvTeardown };
const queue_case_create_destroy = bench.CaseSpec{ .name = "create_destroy", .kind = .latency, .run = &queueCreateDestroyRun };
const queue_case_throughput = bench.CaseSpec{ .name = "throughput_2t", .kind = .throughput, .run = &queueThroughputRun, .setup = &queueThroughputSetup, .teardown = &queueThroughputTeardown };

const queue_cases_full = [_]bench.CBenchCase{
    bench.caseAudited("queue", queue_case_memory),
    bench.caseAudited("queue", queue_case_send_receive),
    bench.caseAudited("queue", queue_case_create_destroy),
    bench.caseAudited("queue", queue_case_throughput),
};
const queue_cases_zh = [_]bench.CBenchCase{
    bench.caseAudited("queue", queue_case_send_receive),
    bench.caseAudited("queue", queue_case_throughput),
};
const queue_cases: []const bench.CBenchCase =
    if (is_zero_heap) &queue_cases_zh else &queue_cases_full;

const queue_suite = bench.makeSuite(.{
    .name = "queue",
    .enabled = &queueIsEnabled,
    .cases = queue_cases,
});

comptime {
    @export(&queue_suite, .{ .name = "bench_suite_queue" });
}

// =========================================================================
// Suite: timer
// =========================================================================

var timer_bench_tmr: ove.Timer = undefined;
var timer_bench_tmr_in: bool = false;
var timer_mem_tmr: ove.Timer = undefined;
var timer_mem_tmr_in: bool = false;

fn timerDummyCb() void {}

fn timerCreateDestroyRun() void {
    var t: ove.Timer = undefined;
    createTimerInPlace(&t, timerDummyCb, 1000, .periodic) catch return;
    t.deinit();
}

fn timerStartStopSetup() void {
    timer_bench_tmr = undefined;
    createTimerInPlace(&timer_bench_tmr, timerDummyCb, 1000, .periodic) catch return;
    timer_bench_tmr_in = true;
}
fn timerStartStopRun() void {
    timer_bench_tmr.start() catch {};
    timer_bench_tmr.stop() catch {};
}
fn timerStartStopTeardown() void {
    if (timer_bench_tmr_in) timer_bench_tmr.deinit();
    timer_bench_tmr_in = false;
}

fn timerMemoryRun() void {
    timer_mem_tmr = undefined;
    createTimerInPlace(&timer_mem_tmr, timerDummyCb, 1000, .periodic) catch return;
    timer_mem_tmr_in = true;
}
fn timerMemoryTeardown() void {
    if (timer_mem_tmr_in) timer_mem_tmr.deinit();
    timer_mem_tmr_in = false;
}

fn timerIsEnabled() bool {
    return true;
}

const timer_case_memory = bench.CaseSpec{ .name = "memory", .kind = .memory, .run = &timerMemoryRun, .teardown = &timerMemoryTeardown };
const timer_case_create_destroy = bench.CaseSpec{ .name = "create_destroy", .kind = .latency, .run = &timerCreateDestroyRun };
const timer_case_start_stop = bench.CaseSpec{ .name = "start_stop", .kind = .latency, .run = &timerStartStopRun, .setup = &timerStartStopSetup, .teardown = &timerStartStopTeardown };

const timer_cases_full = [_]bench.CBenchCase{
    bench.caseAudited("timer", timer_case_memory),
    bench.caseAudited("timer", timer_case_create_destroy),
    bench.caseAudited("timer", timer_case_start_stop),
};
const timer_cases_zh = [_]bench.CBenchCase{
    bench.caseAudited("timer", timer_case_start_stop),
};
const timer_cases: []const bench.CBenchCase =
    if (is_zero_heap) &timer_cases_zh else &timer_cases_full;

const timer_suite = bench.makeSuite(.{
    .name = "timer",
    .enabled = &timerIsEnabled,
    .cases = timer_cases,
});

comptime {
    @export(&timer_suite, .{ .name = "bench_suite_timer" });
}

// =========================================================================
// Suite: eventgroup
// =========================================================================

var eg_bench_eg: ove.EventGroup = undefined;
var eg_bench_eg_in: bool = false;
var eg_mem_eg: ove.EventGroup = undefined;
var eg_mem_eg_in: bool = false;

fn egSetGetSetup() void {
    eg_bench_eg = undefined;
    createInPlace(&eg_bench_eg, .{}) catch return;
    eg_bench_eg_in = true;
}
fn egSetGetRun() void {
    _ = eg_bench_eg.setBits(0x01);
    _ = eg_bench_eg.getBits();
    _ = eg_bench_eg.clearBits(0x01);
}
fn egSetGetTeardown() void {
    if (eg_bench_eg_in) eg_bench_eg.deinit();
    eg_bench_eg_in = false;
}

fn egCreateDestroyRun() void {
    var eg: ove.EventGroup = undefined;
    createInPlace(&eg, .{}) catch return;
    eg.deinit();
}

fn egMemoryRun() void {
    eg_mem_eg = undefined;
    createInPlace(&eg_mem_eg, .{}) catch return;
    eg_mem_eg_in = true;
}
fn egMemoryTeardown() void {
    if (eg_mem_eg_in) eg_mem_eg.deinit();
    eg_mem_eg_in = false;
}

fn eventgroupIsEnabled() bool {
    return true;
}

const eg_case_memory = bench.CaseSpec{ .name = "memory", .kind = .memory, .run = &egMemoryRun, .teardown = &egMemoryTeardown };
const eg_case_set_get = bench.CaseSpec{ .name = "set_get_bits", .kind = .latency, .run = &egSetGetRun, .setup = &egSetGetSetup, .teardown = &egSetGetTeardown };
const eg_case_create_destroy = bench.CaseSpec{ .name = "create_destroy", .kind = .latency, .run = &egCreateDestroyRun };

const eg_cases_full = [_]bench.CBenchCase{
    bench.caseAudited("eventgroup", eg_case_memory),
    bench.caseAudited("eventgroup", eg_case_set_get),
    bench.caseAudited("eventgroup", eg_case_create_destroy),
};
const eg_cases_zh = [_]bench.CBenchCase{
    bench.caseAudited("eventgroup", eg_case_set_get),
};
const eg_cases: []const bench.CBenchCase =
    if (is_zero_heap) &eg_cases_zh else &eg_cases_full;

const eg_suite = bench.makeSuite(.{
    .name = "eventgroup",
    .enabled = &eventgroupIsEnabled,
    .cases = eg_cases,
});

comptime {
    @export(&eg_suite, .{ .name = "bench_suite_eventgroup" });
}

// =========================================================================
// Suite: workqueue
// =========================================================================

var wq_bench_wq: ove.Workqueue(2048) = undefined;
var wq_bench_wq_in: bool = false;
var wq_bench_work: ove.Work = undefined;
var wq_bench_work_in: bool = false;
var wq_work_executed: volatile_int = volatile_int.init(0);
var wq_work_sem: ove.Semaphore = undefined;
var wq_work_sem_in: bool = false;
var wq_mem_wq: ove.Workqueue(2048) = undefined;
var wq_mem_wq_in: bool = false;

fn workHandler() void {
    wq_work_executed.store(1, .release);
    wq_work_sem.post();
}

fn wqCreateDestroyRun() void {
    var wq: ove.Workqueue(2048) = undefined;
    createWqInPlace(&wq, "bench_wq", .normal) catch return;
    wq.deinit();
}

fn wqSubmitSetup() void {
    wq_work_sem = undefined;
    createInPlace(&wq_work_sem, .{ 0, 1 }) catch return;
    wq_work_sem_in = true;
    wq_bench_wq = undefined;
    createWqInPlace(&wq_bench_wq, "bench_wq", .normal) catch return;
    wq_bench_wq_in = true;
    wq_bench_work = undefined;
    createWorkInPlace(&wq_bench_work, workHandler) catch return;
    wq_bench_work_in = true;
}
fn wqSubmitRun() void {
    wq_work_executed.store(0, .release);
    wq_bench_wq.submit(&wq_bench_work) catch {};
    wq_work_sem.timedWait(.millis(1000)) catch {};
}
fn wqSubmitTeardown() void {
    if (wq_bench_work_in) wq_bench_work.deinit();
    wq_bench_work_in = false;
    if (wq_bench_wq_in) wq_bench_wq.deinit();
    wq_bench_wq_in = false;
    if (wq_work_sem_in) wq_work_sem.deinit();
    wq_work_sem_in = false;
}

fn wqMemoryRun() void {
    wq_mem_wq = undefined;
    createWqInPlace(&wq_mem_wq, "bench_wq", .normal) catch return;
    wq_mem_wq_in = true;
}
fn wqMemoryTeardown() void {
    if (wq_mem_wq_in) wq_mem_wq.deinit();
    wq_mem_wq_in = false;
}

fn workqueueIsEnabled() bool {
    return true;
}

const wq_case_memory = bench.CaseSpec{ .name = "memory", .kind = .memory, .run = &wqMemoryRun, .teardown = &wqMemoryTeardown };
const wq_case_create_destroy = bench.CaseSpec{ .name = "create_destroy", .kind = .latency, .run = &wqCreateDestroyRun, .iterations = 200 };
const wq_case_submit_execute = bench.CaseSpec{ .name = "submit_execute", .kind = .latency, .run = &wqSubmitRun, .setup = &wqSubmitSetup, .teardown = &wqSubmitTeardown, .iterations = 500 };

const wq_cases_full = [_]bench.CBenchCase{
    bench.caseAudited("workqueue", wq_case_memory),
    bench.caseAudited("workqueue", wq_case_create_destroy),
    bench.caseAudited("workqueue", wq_case_submit_execute),
};
const wq_cases_zh = [_]bench.CBenchCase{
    bench.caseAudited("workqueue", wq_case_submit_execute),
};
const wq_cases: []const bench.CBenchCase =
    if (is_zero_heap) &wq_cases_zh else &wq_cases_full;

const wq_suite = bench.makeSuite(.{
    .name = "workqueue",
    .enabled = &workqueueIsEnabled,
    .cases = wq_cases,
});

comptime {
    @export(&wq_suite, .{ .name = "bench_suite_workqueue" });
}

// =========================================================================
// Suite: stream
// =========================================================================

const STREAM_BUF_SIZE: usize = 256;
const STREAM_MSG_SIZE: usize = 64;

var stream_bench_strm: ove.Stream(STREAM_BUF_SIZE) = undefined;
var stream_bench_strm_in: bool = false;
var stream_producer_th: ove.Thread(2048) = undefined;
var stream_producer_th_in: bool = false;
var stream_done: volatile_int = volatile_int.init(0);
var stream_tx_buf: [STREAM_MSG_SIZE]u8 = [_]u8{0} ** STREAM_MSG_SIZE;
var stream_rx_buf: [STREAM_MSG_SIZE]u8 = [_]u8{0} ** STREAM_MSG_SIZE;
var stream_mem_strm: ove.Stream(STREAM_BUF_SIZE) = undefined;
var stream_mem_strm_in: bool = false;

fn streamSendRecvSetup() void {
    stream_bench_strm = undefined;
    createInPlace(&stream_bench_strm, .{1}) catch return;
    stream_bench_strm_in = true;
    @memset(&stream_tx_buf, 0xAA);
}
fn streamSendRecvRun() void {
    _ = stream_bench_strm.send(&stream_tx_buf);
    _ = stream_bench_strm.recv(&stream_rx_buf);
}
fn streamSendRecvTeardown() void {
    if (stream_bench_strm_in) stream_bench_strm.deinit();
    stream_bench_strm_in = false;
}

fn streamCreateDestroyRun() void {
    var s: ove.Stream(STREAM_BUF_SIZE) = undefined;
    createInPlace(&s, .{1}) catch return;
    s.deinit();
}

fn streamProducer() void {
    while (stream_done.load(.acquire) == 0) {
        _ = stream_bench_strm.send(&stream_tx_buf);
    }
}
fn streamThroughputSetup() void {
    stream_done.store(0, .release);
    @memset(&stream_tx_buf, 0xBB);
    stream_bench_strm = undefined;
    createInPlace(&stream_bench_strm, .{1}) catch return;
    stream_bench_strm_in = true;
    stream_producer_th = undefined;
    spawnThreadInPlace(&stream_producer_th, "strm_prod", .normal, streamProducer) catch return;
    stream_producer_th_in = true;
}
fn streamThroughputRun() void {
    _ = stream_bench_strm.recv(&stream_rx_buf);
}
fn streamThroughputTeardown() void {
    stream_done.store(1, .release);
    _ = stream_bench_strm.recvFor(&stream_rx_buf, .millis(100)) catch 0;
    ove.thread.sleepMs(10);
    if (stream_producer_th_in) stream_producer_th.deinit();
    stream_producer_th_in = false;
    if (stream_bench_strm_in) stream_bench_strm.deinit();
    stream_bench_strm_in = false;
}

fn streamMemoryRun() void {
    stream_mem_strm = undefined;
    createInPlace(&stream_mem_strm, .{1}) catch return;
    stream_mem_strm_in = true;
}
fn streamMemoryTeardown() void {
    if (stream_mem_strm_in) stream_mem_strm.deinit();
    stream_mem_strm_in = false;
}

fn streamIsEnabled() bool {
    return true;
}

const stream_case_memory = bench.CaseSpec{ .name = "memory", .kind = .memory, .run = &streamMemoryRun, .teardown = &streamMemoryTeardown };
const stream_case_send_recv = bench.CaseSpec{ .name = "send_recv_64B", .kind = .latency, .run = &streamSendRecvRun, .setup = &streamSendRecvSetup, .teardown = &streamSendRecvTeardown };
const stream_case_create_destroy = bench.CaseSpec{ .name = "create_destroy", .kind = .latency, .run = &streamCreateDestroyRun };
const stream_case_throughput = bench.CaseSpec{ .name = "throughput", .kind = .throughput, .run = &streamThroughputRun, .setup = &streamThroughputSetup, .teardown = &streamThroughputTeardown };

const stream_cases_full = [_]bench.CBenchCase{
    bench.caseAudited("stream", stream_case_memory),
    bench.caseAudited("stream", stream_case_send_recv),
    bench.caseAudited("stream", stream_case_create_destroy),
    bench.caseAudited("stream", stream_case_throughput),
};
const stream_cases_zh = [_]bench.CBenchCase{
    bench.caseAudited("stream", stream_case_send_recv),
    bench.caseAudited("stream", stream_case_throughput),
};
const stream_cases: []const bench.CBenchCase =
    if (is_zero_heap) &stream_cases_zh else &stream_cases_full;

const stream_suite = bench.makeSuite(.{
    .name = "stream",
    .enabled = &streamIsEnabled,
    .cases = stream_cases,
});

comptime {
    @export(&stream_suite, .{ .name = "bench_suite_stream" });
}

// =========================================================================
// Suite registry & runner
// =========================================================================

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
    std.log.info("=== oveRTOS Benchmark Suite ===", .{});

    const iterations: i32 = if (@hasDecl(ove.ffi, "CONFIG_OVE_BENCHMARK_ITERATIONS"))
        ove.ffi.CONFIG_OVE_BENCHMARK_ITERATIONS
    else
        1000;
    const warmup: i32 = if (@hasDecl(ove.ffi, "CONFIG_OVE_BENCHMARK_WARMUP"))
        ove.ffi.CONFIG_OVE_BENCHMARK_WARMUP
    else
        100;
    std.log.info("Iterations: {d}  Warmup: {d}", .{ iterations, warmup });

    for (suites) |suite| {
        bench.runSuite(suite);
    }

    std.log.info("=== Benchmark complete ===", .{});
}

var bench_runner_th: ove.Thread(8192) = undefined;

fn appMain() void {
    std.log.info("Benchmark app: init", .{});

    spawnThreadInPlace(&bench_runner_th, "bench_run", .normal, benchmarkRunner) catch {
        std.log.err("Failed to create benchmark thread", .{});
        return;
    };

    // The bench creates+destroys kernel resources during measurement
    // (helper threads in setup, queues/timers in *_create_destroy cases),
    // so we bypass `ove.run()`'s zero-heap auto-lock and start the
    // scheduler directly — matching the C/CPP benches.  Without this,
    // NuttX zero-heap traps `pthread_create`'s kmm_zalloc with ENOMEM
    // and hangs Rust+Zig benches in `ctxSwitchSetup`.
    ove.startScheduler();

    std.log.info("Benchmark app: shutdown", .{});
}

comptime {
    ove.exportMain(appMain);
}
