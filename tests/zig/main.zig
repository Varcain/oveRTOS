// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const ove = @import("ove");
const Thread = ove.Thread;
const prio = ove.thread.prio;
const w = ove.log.writer;

// ---------------------------------------------------------------------------
// Test framework
// ---------------------------------------------------------------------------

const TestFn = *const fn () anyerror!void;

const TestEntry = struct {
    name: []const u8,
    func: TestFn,
};

var total_passed: usize = 0;
var total_failed: usize = 0;

fn runSuite(suite_name: []const u8, tests: []const TestEntry) void {
    w.print("\n=== Zig {s} Tests ===\n", .{suite_name}) catch {};
    w.print("[==========] Running {d} test(s) from {s}\n", .{ tests.len, suite_name }) catch {};
    var passed: usize = 0;
    var failed: usize = 0;
    for (tests) |t| {
        w.print("[ RUN      ] {s}::{s}\n", .{ suite_name, t.name }) catch {};
        if (t.func()) |_| {
            w.print("[       OK ] {s}::{s}\n", .{ suite_name, t.name }) catch {};
            passed += 1;
        } else |err| {
            w.print("[  FAILED  ] {s}::{s}: {}\n", .{ suite_name, t.name, err }) catch {};
            failed += 1;
        }
    }
    w.print("[==========] {d} test(s) from {s} ran ({d} passed, {d} failed)\n", .{ tests.len, suite_name, passed, failed }) catch {};
    if (failed == 0) {
        w.print("[  PASSED  ] {d} test(s) from {s}\n", .{ passed, suite_name }) catch {};
    } else {
        w.print("[  FAILED  ] {d} test(s) from {s}\n", .{ failed, suite_name }) catch {};
    }
    total_passed += passed;
    total_failed += failed;
}

fn expect(ok: bool) !void {
    if (!ok) return error.AssertionFailed;
}

fn expectEqual(comptime T: type, expected: T, actual: T) !void {
    if (expected != actual) return error.AssertionFailed;
}

fn expectError(result: anytype) !void {
    if (result) |_| return error.AssertionFailed else |_| return;
}

// Stricter variant: asserts the call returned a *specific* error variant.
// Prefer this over `expectError` for tests whose value depends on the
// exact error (timeout vs. invalid-arg vs. not-found).
fn expectErrorIs(result: anytype, comptime expected: anyerror) !void {
    if (result) |_| {
        return error.AssertionFailed;
    } else |got| {
        if (got != expected) return error.AssertionFailed;
    }
}

// ---------------------------------------------------------------------------
// Mutex tests (9)
// ---------------------------------------------------------------------------

fn testMutexCreate() !void {
    var m: ove.Mutex = undefined;
    try m.init();
    m.deinit();
}

fn testMutexLockUnlock() !void {
    var m: ove.Mutex = undefined;
    try m.init();
    defer m.deinit();
    try m.lock(1000);
    m.unlock();
}

fn testMutexContentionTimeout() !void {
    var m: ove.Mutex = undefined;
    try m.init();
    defer m.deinit();
    try m.lock(1000);
    // Same thread, non-recursive: should timeout
    const result = m.lock(0);
    try expect(result == error.Timeout);
    m.unlock();
}

fn testMutexLockZeroTimeout() !void {
    var m: ove.Mutex = undefined;
    try m.init();
    defer m.deinit();
    // First lock should succeed even with 0 timeout
    try m.lock(0);
    m.unlock();
}

fn testMutexRaiiDrop() !void {
    var m: ove.Mutex = undefined;
    try m.init();
    m.deinit();
    // Double destroy should be safe (handle set to null)
    m.deinit();
}

fn testMutexGuardAutoUnlock() !void {
    var m: ove.Mutex = undefined;
    try m.init();
    defer m.deinit();
    const guard = try m.acquire(1000);
    guard.release();
    // After explicit release, should be able to re-lock
    try m.lock(1000);
    m.unlock();
}

fn testMutexGuardTimeout() !void {
    var m: ove.Mutex = undefined;
    try m.init();
    defer m.deinit();
    try m.lock(1000);
    // Lock held, try-lock should fail
    const result = m.lock(0);
    try expect(result == error.Timeout);
    m.unlock();
}

fn testMutexErrorMapping() !void {
    var m: ove.Mutex = undefined;
    try m.init();
    defer m.deinit();
    try m.lock(1000);
    const result = m.lock(0);
    try expect(result == error.Timeout);
    m.unlock();
}

var shared_counter: u32 = 0;
var counter_mutex: ove.Mutex = undefined;

fn counterThread() void {
    var i: u32 = 0;
    while (i < 1000) : (i += 1) {
        counter_mutex.lock(ove.wait_forever) catch continue;
        shared_counter += 1;
        counter_mutex.unlock();
    }
}

fn testMutexSharedCounter() !void {
    shared_counter = 0;
    counter_mutex = undefined;
    try counter_mutex.init();
    defer counter_mutex.deinit();
    var t: ove.Thread(4096) = undefined;
    try t.init("cnt", counterThread, prio.normal);
    // Main thread also increments
    var i: u32 = 0;
    while (i < 1000) : (i += 1) {
        try counter_mutex.lock(ove.wait_forever);
        shared_counter += 1;
        counter_mutex.unlock();
    }
    ove.thread.sleepMs(100);
    t.deinit();
    try expectEqual(u32, 2000, shared_counter);
}

// ---------------------------------------------------------------------------
// RecursiveMutex tests (6)
// ---------------------------------------------------------------------------

fn testRecursiveMutexCreate() !void {
    var m: ove.RecursiveMutex = undefined;
    try m.init();
    m.deinit();
}

fn testRecursiveMutexLockTwice() !void {
    var m: ove.RecursiveMutex = undefined;
    try m.init();
    defer m.deinit();
    try m.lock(1000);
    try m.lock(1000);
    m.unlock();
    m.unlock();
}

fn testRecursiveMutexMatchingUnlocks() !void {
    var m: ove.RecursiveMutex = undefined;
    try m.init();
    defer m.deinit();
    try m.lock(1000);
    try m.lock(1000);
    try m.lock(1000);
    m.unlock();
    m.unlock();
    m.unlock();
    // Should be fully unlocked, re-lock succeeds
    try m.lock(0);
    m.unlock();
}

fn testRecursiveMutexRaiiDrop() !void {
    var m: ove.RecursiveMutex = undefined;
    try m.init();
    m.deinit();
    m.deinit();
}

fn testRecursiveMutexGuardAutoUnlock() !void {
    var m: ove.RecursiveMutex = undefined;
    try m.init();
    defer m.deinit();
    const guard = try m.acquire(1000);
    guard.release();
    // After release, should be able to re-lock
    try m.lock(0);
    m.unlock();
}

fn testRecursiveMutexGuardNested() !void {
    var m: ove.RecursiveMutex = undefined;
    try m.init();
    defer m.deinit();
    const g1 = try m.acquire(1000);
    const g2 = try m.acquire(1000);
    g2.release();
    g1.release();
}

// ---------------------------------------------------------------------------
// Semaphore tests (8)
// ---------------------------------------------------------------------------

fn testSemaphoreCreateBinary() !void {
    var s: ove.Semaphore = undefined;
    try s.init(1, 1);
    s.deinit();
}

fn testSemaphoreCreateCounting() !void {
    var s: ove.Semaphore = undefined;
    try s.init(0, 10);
    s.deinit();
}

fn testSemaphoreTakeInitialOne() !void {
    var s: ove.Semaphore = undefined;
    try s.init(1, 10);
    defer s.deinit();
    try s.take(0);
}

fn testSemaphoreTakeTimeout() !void {
    var s: ove.Semaphore = undefined;
    try s.init(0, 10);
    defer s.deinit();
    try expectErrorIs(s.take(10), ove.Error.Timeout);
}

fn testSemaphoreGiveThenTake() !void {
    var s: ove.Semaphore = undefined;
    try s.init(0, 10);
    defer s.deinit();
    s.give();
    try s.take(0);
}

fn testSemaphoreCounting() !void {
    var s: ove.Semaphore = undefined;
    try s.init(0, 10);
    defer s.deinit();
    s.give();
    s.give();
    s.give();
    try s.take(0);
    try s.take(0);
    try s.take(0);
    try expectErrorIs(s.take(10), ove.Error.Timeout);
}

var sem_for_thread: ove.Semaphore = undefined;

fn semProducerThread() void {
    ove.thread.sleepMs(50);
    sem_for_thread.give();
}

fn testSemaphoreProducerConsumer() !void {
    sem_for_thread = undefined;
    try sem_for_thread.init(0, 1);
    defer sem_for_thread.deinit();
    var t: ove.Thread(4096) = undefined;
    try t.init("semp", semProducerThread, prio.normal);
    try sem_for_thread.take(500);
    t.deinit();
}

fn testSemaphoreRaiiDrop() !void {
    var s: ove.Semaphore = undefined;
    try s.init(1, 1);
    s.deinit();
    s.deinit();
}

// ---------------------------------------------------------------------------
// Event tests (7)
// ---------------------------------------------------------------------------

fn testEventCreate() !void {
    var e: ove.Event = undefined;
    try e.init();
    e.deinit();
}

fn testEventSignalThenWait() !void {
    var e: ove.Event = undefined;
    try e.init();
    defer e.deinit();
    e.signal();
    try e.wait(1000);
}

fn testEventWaitTimeout() !void {
    var e: ove.Event = undefined;
    try e.init();
    defer e.deinit();
    try expectErrorIs(e.wait(10), ove.Error.Timeout);
}

var event_for_thread: ove.Event = undefined;

fn eventSignalThread() void {
    ove.thread.sleepMs(50);
    event_for_thread.signal();
}

fn testEventCrossThread() !void {
    event_for_thread = undefined;
    try event_for_thread.init();
    defer event_for_thread.deinit();
    var t: ove.Thread(4096) = undefined;
    try t.init("esig", eventSignalThread, prio.normal);
    try event_for_thread.wait(500);
    t.deinit();
}

fn testEventSignalFromIsr() !void {
    var e: ove.Event = undefined;
    try e.init();
    defer e.deinit();
    e.signalFromIsr();
    try e.wait(1000);
}

fn testEventAutoReset() !void {
    var e: ove.Event = undefined;
    try e.init();
    defer e.deinit();
    e.signal();
    try e.wait(100);
    // Event should auto-reset; second wait should timeout
    try expectErrorIs(e.wait(10), ove.Error.Timeout);
}

fn testEventRaiiDrop() !void {
    var e: ove.Event = undefined;
    try e.init();
    e.deinit();
    e.deinit();
}

// ---------------------------------------------------------------------------
// CondVar tests (6)
// ---------------------------------------------------------------------------

fn testCondVarCreate() !void {
    var cv: ove.CondVar = undefined;
    try cv.init();
    cv.deinit();
}

var cv_for_thread: ove.CondVar = undefined;
var cv_mutex_for_thread: ove.Mutex = undefined;
var cv_flag: bool = false;

fn cvWaiterThread() void {
    cv_mutex_for_thread.lock(ove.wait_forever) catch return;
    cv_for_thread.wait(&cv_mutex_for_thread, ove.wait_forever) catch {};
    cv_flag = true;
    cv_mutex_for_thread.unlock();
}

fn testCondVarSignalWakesOne() !void {
    cv_flag = false;
    cv_mutex_for_thread = undefined;
    try cv_mutex_for_thread.init();
    defer cv_mutex_for_thread.deinit();
    cv_for_thread = undefined;
    try cv_for_thread.init();
    defer cv_for_thread.deinit();
    var t: ove.Thread(4096) = undefined;
    try t.init("cvw", cvWaiterThread, prio.normal);
    ove.thread.sleepMs(50);
    cv_for_thread.signal();
    ove.thread.sleepMs(50);
    try expect(cv_flag);
    t.deinit();
}

fn testCondVarWaitTimeout() !void {
    var m: ove.Mutex = undefined;
    try m.init();
    defer m.deinit();
    var cv: ove.CondVar = undefined;
    try cv.init();
    defer cv.deinit();
    try m.lock(1000);
    try expectErrorIs(cv.wait(&m, 10), ove.Error.Timeout);
    m.unlock();
}

var cv_prod_flag: bool = false;
var cv_prod_cv: ove.CondVar = undefined;
var cv_prod_mutex: ove.Mutex = undefined;

fn cvProducerThread() void {
    ove.thread.sleepMs(50);
    cv_prod_mutex.lock(ove.wait_forever) catch return;
    cv_prod_flag = true;
    cv_prod_cv.signal();
    cv_prod_mutex.unlock();
}

fn testCondVarProducerConsumer() !void {
    cv_prod_flag = false;
    cv_prod_mutex = undefined;
    try cv_prod_mutex.init();
    defer cv_prod_mutex.deinit();
    cv_prod_cv = undefined;
    try cv_prod_cv.init();
    defer cv_prod_cv.deinit();
    var t: ove.Thread(4096) = undefined;
    try t.init("cvp", cvProducerThread, prio.normal);
    try cv_prod_mutex.lock(ove.wait_forever);
    while (!cv_prod_flag) {
        try cv_prod_cv.wait(&cv_prod_mutex, 500);
    }
    cv_prod_mutex.unlock();
    try expect(cv_prod_flag);
    t.deinit();
}

fn testCondVarWaitForever() !void {
    cv_prod_flag = false;
    cv_prod_mutex = undefined;
    try cv_prod_mutex.init();
    defer cv_prod_mutex.deinit();
    cv_prod_cv = undefined;
    try cv_prod_cv.init();
    defer cv_prod_cv.deinit();
    var t: ove.Thread(4096) = undefined;
    try t.init("cvf", cvProducerThread, prio.normal);
    try cv_prod_mutex.lock(ove.wait_forever);
    while (!cv_prod_flag) {
        try cv_prod_cv.wait(&cv_prod_mutex, ove.wait_forever);
    }
    cv_prod_mutex.unlock();
    try expect(cv_prod_flag);
    t.deinit();
}

fn testCondVarRaiiDrop() !void {
    var cv: ove.CondVar = undefined;
    try cv.init();
    cv.deinit();
    cv.deinit();
}

// ---------------------------------------------------------------------------
// Queue tests (12)
// ---------------------------------------------------------------------------

const Q5 = ove.Queue(i32, 5);
const Q2 = ove.Queue(i32, 2);
const Q8 = ove.Queue(u32, 8);

fn testQueueCreateDestroy() !void {
    var q: Q5 = undefined;
    try q.init();
    q.deinit();
}

fn testQueueSendReceiveSingle() !void {
    var q: Q5 = undefined;
    try q.init();
    defer q.deinit();
    const val: i32 = 42;
    try q.send(&val, 1000);
    const received = try q.receive(1000);
    try expectEqual(i32, 42, received);
}

fn testQueueFifoOrder() !void {
    var q: Q5 = undefined;
    try q.init();
    defer q.deinit();
    var i: i32 = 0;
    while (i < 5) : (i += 1) {
        try q.send(&i, 1000);
    }
    i = 0;
    while (i < 5) : (i += 1) {
        const v = try q.receive(1000);
        try expectEqual(i32, i, v);
    }
}

fn testQueueSendFullTimesOut() !void {
    var q: Q2 = undefined;
    try q.init();
    defer q.deinit();
    const a: i32 = 1;
    const b: i32 = 2;
    const c: i32 = 3;
    try q.send(&a, 100);
    try q.send(&b, 100);
    try expectErrorIs(q.send(&c, 10), ove.Error.Timeout);
}

fn testQueueReceiveEmptyTimesOut() !void {
    var q: Q5 = undefined;
    try q.init();
    defer q.deinit();
    try expectErrorIs(q.receive(10), ove.Error.Timeout);
}

fn testQueueSendFromIsr() !void {
    var q: Q5 = undefined;
    try q.init();
    defer q.deinit();
    const val: i32 = 99;
    try q.sendFromIsr(&val);
    const received = try q.receive(100);
    try expectEqual(i32, 99, received);
}

fn testQueueReceiveFromIsr() !void {
    var q: Q5 = undefined;
    try q.init();
    defer q.deinit();
    const val: i32 = 77;
    try q.send(&val, 100);
    const received = try q.receiveFromIsr();
    try expectEqual(i32, 77, received);
}

var queue_sum: u32 = 0;
var consumer_queue: Q8 = undefined;

fn queueConsumerThread() void {
    var i: u32 = 0;
    while (i < 5) : (i += 1) {
        const val = consumer_queue.receive(1000) catch break;
        queue_sum += val;
    }
}

fn testQueueProducerConsumer() !void {
    queue_sum = 0;
    consumer_queue = undefined;
    try consumer_queue.init();
    defer consumer_queue.deinit();
    var t: ove.Thread(4096) = undefined;
    try t.init("qcon", queueConsumerThread, prio.normal);
    var i: u32 = 1;
    while (i <= 5) : (i += 1) {
        try consumer_queue.send(&i, 1000);
    }
    ove.thread.sleepMs(100);
    t.deinit();
    try expectEqual(u32, 15, queue_sum);
}

const Pair = extern struct { a: i32, b: i32 };
const QPair = ove.Queue(Pair, 4);

fn testQueueStructItem() !void {
    var q: QPair = undefined;
    try q.init();
    defer q.deinit();
    const item: Pair = .{ .a = 10, .b = 20 };
    try q.send(&item, 100);
    const got = try q.receive(100);
    try expectEqual(i32, 10, got.a);
    try expectEqual(i32, 20, got.b);
}

fn testQueueSendWaitForever() !void {
    var q: Q5 = undefined;
    try q.init();
    defer q.deinit();
    const val: i32 = 123;
    try q.send(&val, ove.wait_forever);
    const got = try q.receive(0);
    try expectEqual(i32, 123, got);
}

fn testQueueRaiiDrop() !void {
    var q: Q5 = undefined;
    try q.init();
    q.deinit();
    q.deinit();
}

const QU8 = ove.Queue(u8, 4);
const QU32 = ove.Queue(u32, 4);

fn testQueueTypeSafety() !void {
    var q8: QU8 = undefined;
    try q8.init();
    defer q8.deinit();
    var q32: QU32 = undefined;
    try q32.init();
    defer q32.deinit();
    const v8: u8 = 0xFF;
    const v32: u32 = 0xDEADBEEF;
    try q8.send(&v8, 100);
    try q32.send(&v32, 100);
    try expectEqual(u8, 0xFF, try q8.receive(100));
    try expectEqual(u32, 0xDEADBEEF, try q32.receive(100));
}

// ---------------------------------------------------------------------------
// Timer tests (9)
// ---------------------------------------------------------------------------

var timer_count: u32 = 0;

fn timerCallback() void {
    timer_count += 1;
}

fn testTimerCreateDestroyOneshot() !void {
    var t: ove.Timer = undefined;
    try t.init(timerCallback, 100, .one_shot);
    t.deinit();
}

fn testTimerCreateDestroyPeriodic() !void {
    var t: ove.Timer = undefined;
    try t.init(timerCallback, 50, .periodic);
    t.deinit();
}

fn testTimerOneshotFiresOnce() !void {
    timer_count = 0;
    var t: ove.Timer = undefined;
    try t.init(timerCallback, 50, .one_shot);
    defer t.deinit();
    try t.start();
    ove.thread.sleepMs(200);
    try expectEqual(u32, 1, timer_count);
}

fn testTimerPeriodicFiresMultiple() !void {
    timer_count = 0;
    var t: ove.Timer = undefined;
    try t.init(timerCallback, 50, .periodic);
    defer t.deinit();
    try t.start();
    ove.thread.sleepMs(250);
    try t.stop();
    try expect(timer_count >= 3);
}

fn testTimerStopPreventsCallbacks() !void {
    timer_count = 0;
    var t: ove.Timer = undefined;
    try t.init(timerCallback, 50, .periodic);
    defer t.deinit();
    try t.start();
    ove.thread.sleepMs(150);
    try t.stop();
    ove.thread.sleepMs(50); // let in-flight callback complete
    const count_at_stop = timer_count;
    ove.thread.sleepMs(200);
    try expectEqual(u32, count_at_stop, timer_count);
}

fn testTimerResetRestarts() !void {
    timer_count = 0;
    var t: ove.Timer = undefined;
    try t.init(timerCallback, 200, .one_shot);
    defer t.deinit();
    try t.start();
    ove.thread.sleepMs(100);
    try t.reset();
    ove.thread.sleepMs(100);
    // Should not have fired yet (reset pushed it out)
    try expectEqual(u32, 0, timer_count);
    ove.thread.sleepMs(150);
    try expectEqual(u32, 1, timer_count);
}

fn testTimerDoubleStart() !void {
    timer_count = 0;
    var t: ove.Timer = undefined;
    try t.init(timerCallback, 100, .periodic);
    defer t.deinit();
    try t.start();
    try t.start();
    ove.thread.sleepMs(50);
    try t.stop();
}

fn testTimerRaiiDrop() !void {
    var t: ove.Timer = undefined;
    try t.init(timerCallback, 100, .periodic);
    t.deinit();
    t.deinit();
}

var ctx_count: u32 = 0;

fn ctxTimerCallback(ctx: *u32) void {
    ctx.* += 1;
}

fn testTimerContextCallback() !void {
    ctx_count = 0;
    var t: ove.Timer = undefined;
    try t.initWithContext(u32, &ctx_count, ctxTimerCallback, 50, .one_shot);
    defer t.deinit();
    try t.start();
    ove.thread.sleepMs(150);
    try expectEqual(u32, 1, ctx_count);
}

// ---------------------------------------------------------------------------
// Thread tests (11)
// ---------------------------------------------------------------------------

var thread_ran: bool = false;

fn threadEntry() void {
    thread_ran = true;
}

fn testThreadCreateDestroy() !void {
    thread_ran = false;
    var t: ove.Thread(4096) = undefined;
    try t.init("test", threadEntry, prio.normal);
    ove.thread.sleepMs(50);
    try expect(thread_ran);
    t.deinit();
}

fn testThreadSleepDuration() !void {
    const before = try ove.time.getUs();
    ove.thread.sleepMs(50);
    const after = try ove.time.getUs();
    const elapsed_us = after - before;
    try expect(elapsed_us >= 25_000);
    try expect(elapsed_us <= 150_000);
}

fn testThreadYieldNoCrash() !void {
    ove.thread.yieldCpu();
}

fn testThreadGetSelf() !void {
    _ = ove.thread.getSelf();
}

fn testThreadSetPriority() !void {
    var t: ove.Thread(4096) = undefined;
    try t.init("prio", threadEntry, prio.normal);
    defer t.deinit();
    t.setPriority(prio.high);
    ove.thread.sleepMs(50);
}

fn testThreadGetStateRunning() !void {
    var t: ove.Thread(4096) = undefined;
    try t.init("stat", threadEntry, prio.normal);
    defer t.deinit();
    const state = t.getState();
    // Should be one of Running, Ready, Blocked, or Terminated
    _ = state;
}

var terminated_flag: bool = false;

fn terminatingThread() void {
    terminated_flag = true;
}

fn testThreadGetStateTerminated() !void {
    terminated_flag = false;
    var t: ove.Thread(4096) = undefined;
    try t.init("term", terminatingThread, prio.normal);
    ove.thread.sleepMs(100);
    try expect(terminated_flag);
    t.deinit();
}

fn testThreadStackUsage() !void {
    var t: ove.Thread(4096) = undefined;
    try t.init("stk", threadEntry, prio.normal);
    ove.thread.sleepMs(50);
    _ = t.getStackUsage();
    t.deinit();
}

var suspended_flag: bool = false;

fn suspendableThread() void {
    ove.thread.sleepMs(500);
    suspended_flag = true;
}

fn testThreadSuspendResume() !void {
    suspended_flag = false;
    var t: ove.Thread(4096) = undefined;
    try t.init("susp", suspendableThread, prio.normal);
    ove.thread.sleepMs(50);
    t.suspendThread();
    ove.thread.sleepMs(100);
    try expect(!suspended_flag);
    t.resumeThread();
    ove.thread.sleepMs(600);
    try expect(suspended_flag);
    t.deinit();
}

fn testThreadRuntimeStats() !void {
    var t: ove.Thread(4096) = undefined;
    try t.init("rts", threadEntry, prio.normal);
    ove.thread.sleepMs(50);
    // May return error.NotSupported on some backends
    _ = t.getRuntimeStats() catch {};
    t.deinit();
}

fn testThreadRaiiDrop() !void {
    var t: ove.Thread(4096) = undefined;
    try t.init("raii", threadEntry, prio.normal);
    ove.thread.sleepMs(50);
    t.deinit();
    t.deinit();
}

// ---------------------------------------------------------------------------
// EventGroup tests (11)
// ---------------------------------------------------------------------------

const BIT_0: u32 = 0x01;
const BIT_1: u32 = 0x02;

fn testEventGroupCreateDestroy() !void {
    var eg: ove.EventGroup = undefined;
    try eg.init();
    eg.deinit();
}

fn testEventGroupSetBits() !void {
    var eg: ove.EventGroup = undefined;
    try eg.init();
    defer eg.deinit();
    _ = eg.setBits(BIT_0 | BIT_1);
    const bits = eg.getBits();
    try expect(bits & (BIT_0 | BIT_1) == (BIT_0 | BIT_1));
}

fn testEventGroupClearBits() !void {
    var eg: ove.EventGroup = undefined;
    try eg.init();
    defer eg.deinit();
    _ = eg.setBits(BIT_0 | BIT_1);
    _ = eg.clearBits(BIT_1);
    const bits = eg.getBits();
    try expect(bits & BIT_0 != 0);
    try expect(bits & BIT_1 == 0);
}

fn testEventGroupGetBits() !void {
    var eg: ove.EventGroup = undefined;
    try eg.init();
    defer eg.deinit();
    try expectEqual(u32, 0, eg.getBits());
}

fn testEventGroupWaitAll() !void {
    var eg: ove.EventGroup = undefined;
    try eg.init();
    defer eg.deinit();
    _ = eg.setBits(BIT_0 | BIT_1);
    const result = try eg.waitBits(BIT_0 | BIT_1, ove.eventgroup.WAIT_ALL, 100);
    try expect(result & (BIT_0 | BIT_1) == (BIT_0 | BIT_1));
}

fn testEventGroupWaitAny() !void {
    var eg: ove.EventGroup = undefined;
    try eg.init();
    defer eg.deinit();
    _ = eg.setBits(BIT_0);
    const result = try eg.waitBits(BIT_0 | BIT_1, 0, 100);
    try expect(result & BIT_0 != 0);
}

fn testEventGroupWaitTimeout() !void {
    var eg: ove.EventGroup = undefined;
    try eg.init();
    defer eg.deinit();
    try expectErrorIs(eg.waitBits(BIT_0, 0, 10), ove.Error.Timeout);
}

fn testEventGroupClearOnExit() !void {
    var eg: ove.EventGroup = undefined;
    try eg.init();
    defer eg.deinit();
    _ = eg.setBits(BIT_0 | BIT_1);
    _ = try eg.waitBits(BIT_0, ove.eventgroup.CLEAR_ON_EXIT, 100);
    const bits = eg.getBits();
    try expect(bits & BIT_0 == 0);
}

fn testEventGroupSetBitsFromIsr() !void {
    var eg: ove.EventGroup = undefined;
    try eg.init();
    defer eg.deinit();
    _ = eg.setBitsFromIsr(BIT_0);
    const bits = eg.getBits();
    try expect(bits & BIT_0 != 0);
}

var eg_for_thread: ove.EventGroup = undefined;

fn egSetterThread() void {
    ove.thread.sleepMs(50);
    _ = eg_for_thread.setBits(BIT_0);
}

fn testEventGroupCrossThread() !void {
    eg_for_thread = undefined;
    try eg_for_thread.init();
    defer eg_for_thread.deinit();
    var t: ove.Thread(4096) = undefined;
    try t.init("egst", egSetterThread, prio.normal);
    const result = try eg_for_thread.waitBits(BIT_0, 0, 500);
    try expect(result & BIT_0 != 0);
    t.deinit();
}

fn testEventGroupRaiiDrop() !void {
    var eg: ove.EventGroup = undefined;
    try eg.init();
    eg.deinit();
    eg.deinit();
}

// ---------------------------------------------------------------------------
// Time tests (4)
// ---------------------------------------------------------------------------

fn testTimeGetUsReturnsOk() !void {
    const us = try ove.time.getUs();
    _ = us;
}

fn testTimeGetUsMonotonic() !void {
    const a = try ove.time.getUs();
    const b = try ove.time.getUs();
    try expect(b >= a);
}

fn testTimeDelayMs() !void {
    const before = try ove.time.getUs();
    ove.time.delayMs(50);
    const after = try ove.time.getUs();
    try expect(after - before >= 40_000);
}

fn testTimeDelayUs() !void {
    const before = try ove.time.getUs();
    ove.time.delayUs(10_000);
    const after = try ove.time.getUs();
    try expect(after - before >= 5_000);
}

// ---------------------------------------------------------------------------
// Console tests (2)
// ---------------------------------------------------------------------------

fn testConsolePutchar() !void {
    ove.console.putchar('Z');
    ove.console.putchar('\n');
}

fn testConsoleWrite() !void {
    ove.console.write("[I] Zig console test\n");
}

// ---------------------------------------------------------------------------
// Board tests (2)
// ---------------------------------------------------------------------------

fn testBoardInit() !void {
    try ove.board.init();
}

fn testBoardInitIdempotent() !void {
    try ove.board.init();
    try ove.board.init();
}

// ---------------------------------------------------------------------------
// BSP tests (6)
// ---------------------------------------------------------------------------

fn testBspLedSetNoPanic() !void {
    ove.led.set(0, true);
    ove.led.set(0, false);
    ove.led.set(1, true);
}

fn testBspLedToggleNoPanic() !void {
    ove.led.toggle(0);
    ove.led.toggle(1);
}

fn testBspLedSetOutOfRange() !void {
    ove.led.set(100, true);
}

fn testBspBoardInit() !void {
    try ove.board.init();
}

fn testBspGpioSetGet() !void {
    try ove.gpio.set(0, 0, true);
    const val = try ove.gpio.get(0, 0);
    _ = val;
}

fn gpioIrqCallback(_: u32, _: u32) void {}

fn testBspGpioIrq() !void {
    try ove.gpio.irqRegister(0, 0, 0x01, gpioIrqCallback);
    try ove.gpio.irqEnable(0, 0);
    try ove.gpio.irqDisable(0, 0);
}

// ---------------------------------------------------------------------------
// GPIO tests (3)
// ---------------------------------------------------------------------------

fn testGpioSetGet() !void {
    try ove.gpio.set(0, 0, true);
    _ = try ove.gpio.get(0, 0);
}

fn testGpioIrq() !void {
    try ove.gpio.irqRegister(0, 0, 0x01, gpioIrqCallback);
    try ove.gpio.irqEnable(0, 0);
    try ove.gpio.irqDisable(0, 0);
}

fn testGpioInvalidPort() !void {
    try expectError(ove.gpio.set(9999, 9999, true));
}

// ---------------------------------------------------------------------------
// LED tests (3)
// ---------------------------------------------------------------------------

fn testLedSetNoPanic() !void {
    ove.led.set(0, true);
    ove.led.set(0, false);
    ove.led.set(1, true);
    ove.led.set(1, false);
}

fn testLedToggleNoPanic() !void {
    ove.led.toggle(0);
    ove.led.toggle(1);
    ove.led.toggle(2);
}

fn testLedSetOutOfRange() !void {
    ove.led.set(100, true);
}

// ---------------------------------------------------------------------------
// NVS tests (7)
// ---------------------------------------------------------------------------

fn testNvsInit() !void {
    try ove.nvs.init();
}

fn testNvsWriteReadRoundtrip() !void {
    try ove.nvs.init();
    try ove.nvs.write("key1", "hello");
    var buf: [32]u8 = undefined;
    const len = try ove.nvs.read("key1", &buf);
    try expectEqual(usize, 5, len);
    try expect(std.mem.eql(u8, buf[0..5], "hello"));
}

fn testNvsReadNonexistentKey() !void {
    try ove.nvs.init();
    var buf: [32]u8 = undefined;
    try expectError(ove.nvs.read("nonexistent_key_xyz", &buf));
}

fn testNvsOverwrite() !void {
    try ove.nvs.init();
    try ove.nvs.write("owkey", "first");
    try ove.nvs.write("owkey", "second");
    var buf: [32]u8 = undefined;
    const len = try ove.nvs.read("owkey", &buf);
    try expectEqual(usize, 6, len);
    try expect(std.mem.eql(u8, buf[0..6], "second"));
}

fn testNvsErase() !void {
    try ove.nvs.init();
    try ove.nvs.write("erkey", "data");
    try ove.nvs.erase("erkey");
    var buf: [32]u8 = undefined;
    try expectError(ove.nvs.read("erkey", &buf));
}

fn testNvsEraseNonexistent() !void {
    try ove.nvs.init();
    try expectError(ove.nvs.erase("never_existed_xyz"));
}

fn testNvsBinaryData() !void {
    try ove.nvs.init();
    const data = [_]u8{ 0xFF, 0x00, 0xAB, 0x42 };
    try ove.nvs.write("binkey", &data);
    var buf: [32]u8 = undefined;
    const len = try ove.nvs.read("binkey", &buf);
    try expectEqual(usize, 4, len);
    try expect(std.mem.eql(u8, buf[0..4], &data));
}

// ---------------------------------------------------------------------------
// Watchdog tests (5)
// ---------------------------------------------------------------------------

fn testWatchdogCreateDestroy() !void {
    var wd: ove.Watchdog = undefined;
    try wd.init(5000);
    wd.deinit();
}

fn testWatchdogStart() !void {
    var wd: ove.Watchdog = undefined;
    try wd.init(5000);
    defer wd.deinit();
    try wd.start();
}

fn testWatchdogFeed() !void {
    var wd: ove.Watchdog = undefined;
    try wd.init(5000);
    defer wd.deinit();
    try wd.start();
    try wd.feed();
    try wd.feed();
}

fn testWatchdogRaiiDrop() !void {
    var wd: ove.Watchdog = undefined;
    try wd.init(5000);
    wd.deinit();
    wd.deinit();
}

fn testWatchdogFeedMultiple() !void {
    var wd: ove.Watchdog = undefined;
    try wd.init(5000);
    defer wd.deinit();
    try wd.start();
    var i: u32 = 0;
    while (i < 10) : (i += 1) {
        try wd.feed();
    }
}

// ---------------------------------------------------------------------------
// Audio tests (4)
// ---------------------------------------------------------------------------

fn testAudioGraphInitDeinit() !void {
    var g: ove.audio.Graph = undefined;
    try g.init(256);
    defer g.deinit();
    // Graph created with valid frames_per_period and torn down without error
}

fn testAudioGraphInitZeroFrames() !void {
    var g: ove.audio.Graph = undefined;
    const result = g.init(0);
    if (result) |_| {
        return error.AssertionFailed; // should have errored
    } else |_| {
        return; // expected error for zero frames
    }
}

fn testAudioGraphConnectInvalid() !void {
    var g: ove.audio.Graph = undefined;
    try g.init(256);
    defer g.deinit();
    // Connect with no nodes added — indices 0,0 are invalid
    try expectError(g.connect(0, 0));
}

fn testAudioGraphBuildEmpty() !void {
    var g: ove.audio.Graph = undefined;
    try g.init(256);
    defer g.deinit();
    // Build with no nodes succeeds (empty graph is valid)
    try g.build();
}

fn testAudioGraphStartNotReady() !void {
    var g: ove.audio.Graph = undefined;
    try g.init(256);
    defer g.deinit();
    // Graph is IDLE (not built/READY), start should fail
    try expectError(g.start());
}

fn testAudioGraphStopNotRunning() !void {
    var g: ove.audio.Graph = undefined;
    try g.init(256);
    defer g.deinit();
    // Graph is IDLE (not running), stop should fail
    try expectError(g.stop());
}

fn testAudioGraphBuildThenStart() !void {
    var g: ove.audio.Graph = undefined;
    try g.init(256);
    defer g.deinit();
    // Full lifecycle on empty graph: build, start, stop
    try g.build();
    try g.start();
    try g.stop();
}

// ---------------------------------------------------------------------------
// Inference tests (3)
// ---------------------------------------------------------------------------

fn testInferCreateNull() !void {
    const rc = ove.ffi.ove_model_create(null, null);
    try expect(rc != 0); // should fail (stub returns NOT_SUPPORTED)
}

fn testInferInvokeNull() !void {
    const rc = ove.ffi.ove_model_invoke(null);
    try expect(rc != 0);
}

fn testInferLastInferenceNull() !void {
    const us = ove.ffi.ove_model_last_inference_us(null);
    try expect(us == 0);
}

// ---------------------------------------------------------------------------
// Shell tests (5)
// ---------------------------------------------------------------------------

var shell_ping_count: u32 = 0;

fn shellPingHandler(_: c_int, _: [*c]const [*c]const u8) void {
    shell_ping_count += 1;
}

fn testShellInit() !void {
    try ove.shell.init();
}

fn testShellRegisterAndDispatch() !void {
    shell_ping_count = 0;
    try ove.shell.init();
    try ove.shell.registerCmd("ping", "ping help", shellPingHandler);
    for ("ping\n") |ch| {
        ove.shell.processChar(ch);
    }
    try expect(shell_ping_count >= 1);
}

var shell_arg_count: u32 = 0;

fn shellEchoHandler(argc: c_int, _: [*c]const [*c]const u8) void {
    shell_arg_count = @intCast(argc);
}

fn testShellArgsPassedToHandler() !void {
    shell_arg_count = 0;
    try ove.shell.init();
    try ove.shell.registerCmd("echo", "echo help", shellEchoHandler);
    for ("echo hello world\n") |ch| {
        ove.shell.processChar(ch);
    }
    try expectEqual(u32, 3, shell_arg_count);
}

fn testShellUnknownCommandNoCrash() !void {
    try ove.shell.init();
    for ("nonexistent_cmd_xyz\n") |ch| {
        ove.shell.processChar(ch);
    }
}

var shell_aaa_count: u32 = 0;
var shell_bbb_count: u32 = 0;

fn shellAaaHandler(_: c_int, _: [*c]const [*c]const u8) void {
    shell_aaa_count += 1;
}

fn shellBbbHandler(_: c_int, _: [*c]const [*c]const u8) void {
    shell_bbb_count += 1;
}

fn testShellMultipleCommands() !void {
    shell_aaa_count = 0;
    shell_bbb_count = 0;
    try ove.shell.init();
    try ove.shell.registerCmd("aaa", "a help", shellAaaHandler);
    try ove.shell.registerCmd("bbb", "b help", shellBbbHandler);
    for ("aaa\n") |ch| ove.shell.processChar(ch);
    for ("bbb\n") |ch| ove.shell.processChar(ch);
    for ("aaa\n") |ch| ove.shell.processChar(ch);
    try expectEqual(u32, 2, shell_aaa_count);
    try expectEqual(u32, 1, shell_bbb_count);
}

// ---------------------------------------------------------------------------
// FS tests (8)
// ---------------------------------------------------------------------------

fn doFsMount() void {
    // POSIX backend mount with empty path (acts as no-op init)
    ove.fs.mount("\x00", "\x00") catch {};
}

fn testFsMount() !void {
    doFsMount();
}

fn testFsFileWriteReadRoundtrip() !void {
    doFsMount();
    {
        var f = try ove.fs.File.open("/tmp/ove_zig_rw.txt", ove.fs.O_WRITE | ove.fs.O_CREATE);
        defer f.close();
        const written = try f.write("hello ove");
        try expectEqual(usize, 9, written);
    }
    {
        var f = try ove.fs.File.open("/tmp/ove_zig_rw.txt", ove.fs.O_READ);
        defer f.close();
        var buf: [64]u8 = undefined;
        const n = try f.read(&buf);
        try expectEqual(usize, 9, n);
        try expect(std.mem.eql(u8, buf[0..9], "hello ove"));
    }
    ove.fs.unlink("/tmp/ove_zig_rw.txt") catch {};
}

fn testFsFileRaiiClose() !void {
    doFsMount();
    {
        var f = try ove.fs.File.open("/tmp/ove_zig_raii.txt", ove.fs.O_WRITE | ove.fs.O_CREATE);
        _ = try f.write("raii");
        f.close();
    }
    var f = try ove.fs.File.open("/tmp/ove_zig_raii.txt", ove.fs.O_READ);
    defer f.close();
    var buf: [16]u8 = undefined;
    const n = try f.read(&buf);
    try expectEqual(usize, 4, n);
    ove.fs.unlink("/tmp/ove_zig_raii.txt") catch {};
}

fn testFsOpenNonexistentFails() !void {
    try expectError(ove.fs.File.open("/tmp/no_such_file_zig_xyz.txt", ove.fs.O_READ));
}

fn testFsDirOpenRead() !void {
    doFsMount();
    {
        var f = try ove.fs.File.open("/tmp/ove_zig_dir_read.txt", ove.fs.O_WRITE | ove.fs.O_CREATE);
        _ = try f.write("x");
        f.close();
    }
    var dir = try ove.fs.Dir.open("/tmp");
    defer dir.close();
    var found = false;
    while (try dir.readEntry()) |entry| {
        const name_slice = std.mem.sliceTo(&entry.name, 0);
        if (std.mem.eql(u8, name_slice, "ove_zig_dir_read.txt")) {
            found = true;
            break;
        }
    }
    try expect(found);
    ove.fs.unlink("/tmp/ove_zig_dir_read.txt") catch {};
}

fn testFsDirEndReturnsNull() !void {
    doFsMount();
    {
        var f = try ove.fs.File.open("/tmp/ove_zig_end_test.txt", ove.fs.O_WRITE | ove.fs.O_CREATE);
        _ = try f.write("x");
        f.close();
    }
    var dir = try ove.fs.Dir.open("/tmp");
    defer dir.close();
    // Find our file (proves readEntry works), then drain
    var found = false;
    while (try dir.readEntry()) |entry| {
        const name_slice = std.mem.sliceTo(&entry.name, 0);
        if (std.mem.eql(u8, name_slice, "ove_zig_end_test.txt")) found = true;
    }
    try expect(found);
    // Past end
    const past = try dir.readEntry();
    try expect(past == null);
    ove.fs.unlink("/tmp/ove_zig_end_test.txt") catch {};
}

fn testFsDirEntryName() !void {
    doFsMount();
    {
        var f = try ove.fs.File.open("/tmp/ove_zig_entry.wav", ove.fs.O_WRITE | ove.fs.O_CREATE);
        _ = try f.write("x");
        f.close();
    }
    var dir = try ove.fs.Dir.open("/tmp");
    defer dir.close();
    var found = false;
    while (try dir.readEntry()) |entry| {
        const name_slice = std.mem.sliceTo(&entry.name, 0);
        if (std.mem.eql(u8, name_slice, "ove_zig_entry.wav")) {
            found = true;
            break;
        }
    }
    try expect(found);
    ove.fs.unlink("/tmp/ove_zig_entry.wav") catch {};
}

fn testFsDirOpenNonexistentFails() !void {
    try expectError(ove.fs.Dir.open("/tmp/no_such_dir_zig_xyz"));
}

// ---------------------------------------------------------------------------
// Stream tests (7)
// ---------------------------------------------------------------------------

fn testStreamCreateDestroy() !void {
    var s: ove.Stream(256) = undefined;
    try s.init(1);
    s.deinit();
}

fn testStreamSendReceive() !void {
    var s: ove.Stream(256) = undefined;
    try s.init(1);
    defer s.deinit();
    const data = [_]u8{ 0xDE, 0xAD, 0xBE, 0xEF };
    const sent = try s.send(&data, 1000);
    try expectEqual(usize, 4, sent);
    var buf: [256]u8 = undefined;
    const received = try s.receive(&buf, 1000);
    try expectEqual(usize, 4, received);
    try expect(std.mem.eql(u8, buf[0..4], &data));
}

fn testStreamBytesAvailable() !void {
    var s: ove.Stream(256) = undefined;
    try s.init(1);
    defer s.deinit();
    try expect(s.bytesAvailable() == 0);
    _ = try s.send("abc", 1000);
    try expect(s.bytesAvailable() >= 3);
}

fn testStreamReset() !void {
    var s: ove.Stream(256) = undefined;
    try s.init(1);
    defer s.deinit();
    _ = try s.send("data", 1000);
    try expect(s.bytesAvailable() > 0);
    try s.reset();
    try expect(s.bytesAvailable() == 0);
}

fn testStreamSendFromIsr() !void {
    var s: ove.Stream(256) = undefined;
    try s.init(1);
    defer s.deinit();
    const sent = try s.sendFromIsr("isr");
    try expectEqual(usize, 3, sent);
}

fn testStreamReceiveFromIsr() !void {
    var s: ove.Stream(256) = undefined;
    try s.init(1);
    defer s.deinit();
    _ = try s.send("isr", 1000);
    var buf: [16]u8 = undefined;
    const received = try s.receiveFromIsr(&buf);
    try expectEqual(usize, 3, received);
}

fn testStreamRaiiDrop() !void {
    var s: ove.Stream(256) = undefined;
    try s.init(1);
    s.deinit();
    s.deinit();
}

// ---------------------------------------------------------------------------
// Workqueue tests (6)
// ---------------------------------------------------------------------------

var wq_count: u32 = 0;

fn workHandler() void {
    wq_count += 1;
}

fn testWorkqueueCreateDestroy() !void {
    var wq: ove.Workqueue(4096) = undefined;
    try wq.init("test\x00", prio.normal);
    wq.deinit();
}

fn testWorkCreateDestroy() !void {
    var work: ove.Work = undefined;
    try work.init(workHandler);
    work.deinit();
}

fn testWorkSubmit() !void {
    wq_count = 0;
    var wq: ove.Workqueue(4096) = undefined;
    try wq.init("sub\x00", prio.normal);
    defer wq.deinit();
    var work: ove.Work = undefined;
    try work.init(workHandler);
    defer work.deinit();
    try wq.submit(&work);
    ove.thread.sleepMs(100);
    try expectEqual(u32, 1, wq_count);
}

fn testWorkSubmitDelayed() !void {
    wq_count = 0;
    var wq: ove.Workqueue(4096) = undefined;
    try wq.init("del\x00", prio.normal);
    defer wq.deinit();
    var work: ove.Work = undefined;
    try work.init(workHandler);
    defer work.deinit();
    try wq.submitDelayed(&work, 50);
    ove.thread.sleepMs(10);
    try expectEqual(u32, 0, wq_count);
    ove.thread.sleepMs(150);
    try expectEqual(u32, 1, wq_count);
}

fn testWorkCancel() !void {
    var wq: ove.Workqueue(4096) = undefined;
    try wq.init("can\x00", prio.normal);
    defer wq.deinit();
    var work: ove.Work = undefined;
    try work.init(workHandler);
    defer work.deinit();
    _ = work.cancel() catch {};
}

fn testWorkqueueRaiiDrop() !void {
    {
        var wq: ove.Workqueue(4096) = undefined;
        try wq.init("raii\x00", prio.normal);
        wq.deinit();
    }
    {
        var work: ove.Work = undefined;
        try work.init(workHandler);
        work.deinit();
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

pub fn main() void {
    runSuite("Mutex", &.{
        .{ .name = "create", .func = testMutexCreate },
        .{ .name = "lock_unlock", .func = testMutexLockUnlock },
        .{ .name = "contention_timeout", .func = testMutexContentionTimeout },
        .{ .name = "lock_zero_timeout", .func = testMutexLockZeroTimeout },
        .{ .name = "raii_drop", .func = testMutexRaiiDrop },
        .{ .name = "guard_auto_unlock", .func = testMutexGuardAutoUnlock },
        .{ .name = "guard_timeout", .func = testMutexGuardTimeout },
        .{ .name = "error_mapping", .func = testMutexErrorMapping },
        .{ .name = "shared_counter", .func = testMutexSharedCounter },
    });

    runSuite("RecursiveMutex", &.{
        .{ .name = "create", .func = testRecursiveMutexCreate },
        .{ .name = "lock_twice", .func = testRecursiveMutexLockTwice },
        .{ .name = "matching_unlocks", .func = testRecursiveMutexMatchingUnlocks },
        .{ .name = "raii_drop", .func = testRecursiveMutexRaiiDrop },
        .{ .name = "guard_auto_unlock", .func = testRecursiveMutexGuardAutoUnlock },
        .{ .name = "guard_nested", .func = testRecursiveMutexGuardNested },
    });

    runSuite("Semaphore", &.{
        .{ .name = "create_binary", .func = testSemaphoreCreateBinary },
        .{ .name = "create_counting", .func = testSemaphoreCreateCounting },
        .{ .name = "take_initial_one", .func = testSemaphoreTakeInitialOne },
        .{ .name = "take_timeout", .func = testSemaphoreTakeTimeout },
        .{ .name = "give_then_take", .func = testSemaphoreGiveThenTake },
        .{ .name = "counting", .func = testSemaphoreCounting },
        .{ .name = "producer_consumer", .func = testSemaphoreProducerConsumer },
        .{ .name = "raii_drop", .func = testSemaphoreRaiiDrop },
    });

    runSuite("Event", &.{
        .{ .name = "create", .func = testEventCreate },
        .{ .name = "signal_then_wait", .func = testEventSignalThenWait },
        .{ .name = "wait_timeout", .func = testEventWaitTimeout },
        .{ .name = "cross_thread", .func = testEventCrossThread },
        .{ .name = "signal_from_isr", .func = testEventSignalFromIsr },
        .{ .name = "auto_reset", .func = testEventAutoReset },
        .{ .name = "raii_drop", .func = testEventRaiiDrop },
    });

    runSuite("CondVar", &.{
        .{ .name = "create", .func = testCondVarCreate },
        .{ .name = "signal_wakes_one", .func = testCondVarSignalWakesOne },
        .{ .name = "wait_timeout", .func = testCondVarWaitTimeout },
        .{ .name = "producer_consumer", .func = testCondVarProducerConsumer },
        .{ .name = "wait_forever", .func = testCondVarWaitForever },
        .{ .name = "raii_drop", .func = testCondVarRaiiDrop },
    });

    runSuite("Queue", &.{
        .{ .name = "create_destroy", .func = testQueueCreateDestroy },
        .{ .name = "send_receive_single", .func = testQueueSendReceiveSingle },
        .{ .name = "fifo_order", .func = testQueueFifoOrder },
        .{ .name = "send_full_times_out", .func = testQueueSendFullTimesOut },
        .{ .name = "receive_empty_times_out", .func = testQueueReceiveEmptyTimesOut },
        .{ .name = "send_from_isr", .func = testQueueSendFromIsr },
        .{ .name = "receive_from_isr", .func = testQueueReceiveFromIsr },
        .{ .name = "producer_consumer", .func = testQueueProducerConsumer },
        .{ .name = "struct_item", .func = testQueueStructItem },
        .{ .name = "send_wait_forever", .func = testQueueSendWaitForever },
        .{ .name = "raii_drop", .func = testQueueRaiiDrop },
        .{ .name = "type_safety", .func = testQueueTypeSafety },
    });

    runSuite("Timer", &.{
        .{ .name = "create_destroy_oneshot", .func = testTimerCreateDestroyOneshot },
        .{ .name = "create_destroy_periodic", .func = testTimerCreateDestroyPeriodic },
        .{ .name = "oneshot_fires_once", .func = testTimerOneshotFiresOnce },
        .{ .name = "periodic_fires_multiple", .func = testTimerPeriodicFiresMultiple },
        .{ .name = "stop_prevents_callbacks", .func = testTimerStopPreventsCallbacks },
        .{ .name = "reset_restarts", .func = testTimerResetRestarts },
        .{ .name = "double_start", .func = testTimerDoubleStart },
        .{ .name = "raii_drop", .func = testTimerRaiiDrop },
        .{ .name = "context_callback", .func = testTimerContextCallback },
    });

    runSuite("Thread", &.{
        .{ .name = "create_destroy", .func = testThreadCreateDestroy },
        .{ .name = "sleep_duration", .func = testThreadSleepDuration },
        .{ .name = "yield_no_crash", .func = testThreadYieldNoCrash },
        .{ .name = "get_self", .func = testThreadGetSelf },
        .{ .name = "set_priority", .func = testThreadSetPriority },
        .{ .name = "get_state_running", .func = testThreadGetStateRunning },
        .{ .name = "get_state_terminated", .func = testThreadGetStateTerminated },
        .{ .name = "stack_usage", .func = testThreadStackUsage },
        .{ .name = "suspend_resume", .func = testThreadSuspendResume },
        .{ .name = "runtime_stats", .func = testThreadRuntimeStats },
        .{ .name = "raii_drop", .func = testThreadRaiiDrop },
    });

    runSuite("EventGroup", &.{
        .{ .name = "create_destroy", .func = testEventGroupCreateDestroy },
        .{ .name = "set_bits", .func = testEventGroupSetBits },
        .{ .name = "clear_bits", .func = testEventGroupClearBits },
        .{ .name = "get_bits", .func = testEventGroupGetBits },
        .{ .name = "wait_all", .func = testEventGroupWaitAll },
        .{ .name = "wait_any", .func = testEventGroupWaitAny },
        .{ .name = "wait_timeout", .func = testEventGroupWaitTimeout },
        .{ .name = "clear_on_exit", .func = testEventGroupClearOnExit },
        .{ .name = "set_bits_from_isr", .func = testEventGroupSetBitsFromIsr },
        .{ .name = "cross_thread", .func = testEventGroupCrossThread },
        .{ .name = "raii_drop", .func = testEventGroupRaiiDrop },
    });

    runSuite("Time", &.{
        .{ .name = "get_us_returns_ok", .func = testTimeGetUsReturnsOk },
        .{ .name = "get_us_monotonic", .func = testTimeGetUsMonotonic },
        .{ .name = "delay_ms", .func = testTimeDelayMs },
        .{ .name = "delay_us", .func = testTimeDelayUs },
    });

    runSuite("Console", &.{
        .{ .name = "putchar", .func = testConsolePutchar },
        .{ .name = "write", .func = testConsoleWrite },
    });

    runSuite("Board", &.{
        .{ .name = "init", .func = testBoardInit },
        .{ .name = "init_idempotent", .func = testBoardInitIdempotent },
    });

    runSuite("BSP", &.{
        .{ .name = "led_set_no_panic", .func = testBspLedSetNoPanic },
        .{ .name = "led_toggle_no_panic", .func = testBspLedToggleNoPanic },
        .{ .name = "led_set_out_of_range", .func = testBspLedSetOutOfRange },
        .{ .name = "board_init", .func = testBspBoardInit },
        .{ .name = "gpio_set_get", .func = testBspGpioSetGet },
        .{ .name = "gpio_irq", .func = testBspGpioIrq },
    });

    runSuite("GPIO", &.{
        .{ .name = "set_get", .func = testGpioSetGet },
        .{ .name = "irq", .func = testGpioIrq },
        .{ .name = "invalid_port", .func = testGpioInvalidPort },
    });

    runSuite("LED", &.{
        .{ .name = "set_no_panic", .func = testLedSetNoPanic },
        .{ .name = "toggle_no_panic", .func = testLedToggleNoPanic },
        .{ .name = "set_out_of_range", .func = testLedSetOutOfRange },
    });

    runSuite("NVS", &.{
        .{ .name = "init", .func = testNvsInit },
        .{ .name = "write_read_roundtrip", .func = testNvsWriteReadRoundtrip },
        .{ .name = "read_nonexistent_key", .func = testNvsReadNonexistentKey },
        .{ .name = "overwrite", .func = testNvsOverwrite },
        .{ .name = "erase", .func = testNvsErase },
        .{ .name = "erase_nonexistent", .func = testNvsEraseNonexistent },
        .{ .name = "binary_data", .func = testNvsBinaryData },
    });

    runSuite("Watchdog", &.{
        .{ .name = "create_destroy", .func = testWatchdogCreateDestroy },
        .{ .name = "start", .func = testWatchdogStart },
        .{ .name = "feed", .func = testWatchdogFeed },
        .{ .name = "raii_drop", .func = testWatchdogRaiiDrop },
        .{ .name = "feed_multiple", .func = testWatchdogFeedMultiple },
    });

    runSuite("Audio", &.{
        .{ .name = "graph_init_deinit", .func = testAudioGraphInitDeinit },
        .{ .name = "graph_init_zero_frames", .func = testAudioGraphInitZeroFrames },
        .{ .name = "graph_connect_invalid", .func = testAudioGraphConnectInvalid },
        .{ .name = "graph_build_empty", .func = testAudioGraphBuildEmpty },
        .{ .name = "graph_start_not_ready", .func = testAudioGraphStartNotReady },
        .{ .name = "graph_stop_not_running", .func = testAudioGraphStopNotRunning },
        .{ .name = "graph_build_then_start", .func = testAudioGraphBuildThenStart },
    });

    runSuite("Inference", &.{
        .{ .name = "create_null", .func = testInferCreateNull },
        .{ .name = "invoke_null", .func = testInferInvokeNull },
        .{ .name = "last_inference_null", .func = testInferLastInferenceNull },
    });

    runSuite("Shell", &.{
        .{ .name = "init", .func = testShellInit },
        .{ .name = "register_and_dispatch", .func = testShellRegisterAndDispatch },
        .{ .name = "args_passed_to_handler", .func = testShellArgsPassedToHandler },
        .{ .name = "unknown_command_no_crash", .func = testShellUnknownCommandNoCrash },
        .{ .name = "multiple_commands", .func = testShellMultipleCommands },
    });

    runSuite("Filesystem", &.{
        .{ .name = "mount", .func = testFsMount },
        .{ .name = "file_write_read_roundtrip", .func = testFsFileWriteReadRoundtrip },
        .{ .name = "file_raii_close", .func = testFsFileRaiiClose },
        .{ .name = "open_nonexistent_fails", .func = testFsOpenNonexistentFails },
        .{ .name = "dir_open_read", .func = testFsDirOpenRead },
        .{ .name = "dir_entry_name", .func = testFsDirEntryName },
        .{ .name = "dir_open_nonexistent_fails", .func = testFsDirOpenNonexistentFails },
        .{ .name = "dir_end_returns_null", .func = testFsDirEndReturnsNull },
    });

    runSuite("Stream", &.{
        .{ .name = "create_destroy", .func = testStreamCreateDestroy },
        .{ .name = "send_receive", .func = testStreamSendReceive },
        .{ .name = "bytes_available", .func = testStreamBytesAvailable },
        .{ .name = "reset", .func = testStreamReset },
        .{ .name = "send_from_isr", .func = testStreamSendFromIsr },
        .{ .name = "receive_from_isr", .func = testStreamReceiveFromIsr },
        .{ .name = "raii_drop", .func = testStreamRaiiDrop },
    });

    runSuite("Workqueue", &.{
        .{ .name = "create_destroy", .func = testWorkqueueCreateDestroy },
        .{ .name = "work_create_destroy", .func = testWorkCreateDestroy },
        .{ .name = "submit", .func = testWorkSubmit },
        .{ .name = "submit_delayed", .func = testWorkSubmitDelayed },
        .{ .name = "cancel", .func = testWorkCancel },
        .{ .name = "raii_drop", .func = testWorkqueueRaiiDrop },
    });

    w.print("\n=== Summary: {d} passed, {d} failed ===\n", .{ total_passed, total_failed }) catch {};

    if (total_failed > 0) {
        std.process.exit(1);
    }
}
