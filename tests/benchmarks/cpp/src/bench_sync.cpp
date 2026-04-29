/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <ove/ove.hpp>
#include "ove_bench.hpp"

#include <atomic>
#include <optional>

/* --- Shared state (RAII — lives inside std::optional for bench lifecycle) --- */

static std::optional<ove::Mutex> bench_mtx;
static std::optional<ove::Semaphore> bench_sem;
static std::optional<ove::Event> bench_evt;
static std::optional<ove::CondVar> bench_cv;
static std::optional<ove::Mutex> bench_cv_mtx;
static std::optional<ove::RecursiveMutex> bench_rmtx;
static std::optional<ove::Thread<2048>> contention_th;
static std::atomic<bool> contention_done{false};
static std::atomic<uint32_t> contention_count{0};

/* --- Mutex lock/unlock --- */

static void mutex_lock_unlock_setup()
{
	bench_mtx.emplace();
}

static void mutex_lock_unlock_run()
{
	(void)bench_mtx->lock(OVE_WAIT_FOREVER);
	bench_mtx->unlock();
}

static void mutex_lock_unlock_teardown()
{
	bench_mtx.reset();
}

/* --- Mutex create/destroy --- */

static void mutex_create_destroy_run()
{
	ove::Mutex m;
}

/* --- Mutex contention (2-thread throughput) --- */

static void contention_thread(void *arg)
{
	(void)arg;
	while (!contention_done.load(std::memory_order_acquire)) {
		(void)bench_mtx->lock(OVE_WAIT_FOREVER);
		contention_count.fetch_add(1, std::memory_order_relaxed);
		bench_mtx->unlock();
	}
}

static void mutex_contention_setup()
{
	contention_done.store(false, std::memory_order_release);
	contention_count.store(0, std::memory_order_relaxed);
	bench_mtx.emplace();
	contention_th.emplace(contention_thread, nullptr, OVE_PRIO_NORMAL, "contention");
}

static void mutex_contention_run()
{
	(void)bench_mtx->lock(OVE_WAIT_FOREVER);
	contention_count.fetch_add(1, std::memory_order_relaxed);
	bench_mtx->unlock();
}

static void mutex_contention_teardown()
{
	contention_done.store(true, std::memory_order_release);
	ove::time::delay_ms(10);
	contention_th.reset();
	bench_mtx.reset();
}

/* --- Mutex memory --- */

static std::optional<ove::Mutex> mem_mutex;

static void mutex_memory_run()
{
	mem_mutex.emplace();
}

static void mutex_memory_teardown()
{
	mem_mutex.reset();
}

/* --- Semaphore take/give --- */

static void sem_take_give_setup()
{
	bench_sem.emplace(1, 1);
}

static void sem_take_give_run()
{
	(void)bench_sem->take(OVE_WAIT_FOREVER);
	bench_sem->give();
}

static void sem_take_give_teardown()
{
	bench_sem.reset();
}

/* --- Semaphore create/destroy --- */

static void sem_create_destroy_run()
{
	ove::Semaphore s(0, 1);
}

/* --- Semaphore memory --- */

static std::optional<ove::Semaphore> mem_sem;

static void sem_memory_run()
{
	mem_sem.emplace(0, 1);
}

static void sem_memory_teardown()
{
	mem_sem.reset();
}

/* --- Event signal/wait --- */

static std::optional<ove::Event> bench_evt_ack;
static std::optional<ove::Thread<1024>> evt_th;
static std::atomic<bool> evt_done{false};

static void evt_signaler(void *arg)
{
	(void)arg;
	while (!evt_done.load(std::memory_order_acquire)) {
		bench_evt->signal();
		(void)bench_evt_ack->wait(OVE_WAIT_FOREVER);
	}
}

static void event_signal_wait_setup()
{
	evt_done.store(false, std::memory_order_release);
	bench_evt.emplace();
	bench_evt_ack.emplace();
	evt_th.emplace(evt_signaler, nullptr, OVE_PRIO_NORMAL, "evt_sig");
}

static void event_signal_wait_run()
{
	(void)bench_evt->wait(OVE_WAIT_FOREVER);
	bench_evt_ack->signal();
}

static void event_signal_wait_teardown()
{
	evt_done.store(true, std::memory_order_release);
	bench_evt_ack->signal();
	ove::time::delay_ms(10);
	evt_th.reset();
	bench_evt.reset();
	bench_evt_ack.reset();
}

/* --- Event memory --- */

static std::optional<ove::Event> mem_event;

static void event_memory_run()
{
	mem_event.emplace();
}

static void event_memory_teardown()
{
	mem_event.reset();
}

/* --- Condvar signal/wait ---
 *
 * Condvar uses yield-based signaler + bounded cv_wait timeout — see
 * bench_sync.c for why an ack-pattern signaler deadlocks here. */

static std::optional<ove::Thread<1024>> cv_th;
static std::atomic<bool> cv_done{false};

static void cv_signaler(void *arg)
{
	(void)arg;
	while (!cv_done.load(std::memory_order_acquire)) {
		bench_cv->signal();
		ove::Thread<>::yield();
	}
}

static void condvar_signal_wait_setup()
{
	cv_done.store(false, std::memory_order_release);
	bench_cv_mtx.emplace();
	bench_cv.emplace();
	cv_th.emplace(cv_signaler, nullptr, OVE_PRIO_NORMAL, "cv_sig");
}

static void condvar_signal_wait_run()
{
	(void)bench_cv_mtx->lock(OVE_WAIT_FOREVER);
	(void)bench_cv->wait(*bench_cv_mtx, 10);
	bench_cv_mtx->unlock();
}

static void condvar_signal_wait_teardown()
{
	cv_done.store(true, std::memory_order_release);
	bench_cv->signal();
	ove::time::delay_ms(10);
	cv_th.reset();
	bench_cv.reset();
	bench_cv_mtx.reset();
}

/* --- Condvar memory --- */

static std::optional<ove::CondVar> mem_condvar;

static void condvar_memory_run()
{
	mem_condvar.emplace();
}

static void condvar_memory_teardown()
{
	mem_condvar.reset();
}

/* --- Recursive mutex lock/unlock --- */

static void rmtx_lock_unlock_setup()
{
	bench_rmtx.emplace();
}

static void rmtx_lock_unlock_run()
{
	(void)bench_rmtx->lock(OVE_WAIT_FOREVER);
	bench_rmtx->unlock();
}

static void rmtx_lock_unlock_teardown()
{
	bench_rmtx.reset();
}

/* --- Suite --- */

static bool sync_is_enabled()
{
	return true;
}

// Memory tests first — before thread-heavy tests affect heap state.
static constexpr bench::CaseSpec mutex_memory_spec{
	.name = "mutex_memory",
	.kind = bench::Type::memory,
	.run = &mutex_memory_run,
	.teardown = &mutex_memory_teardown,
};
static constexpr bench::CaseSpec sem_memory_spec{
	.name = "sem_memory",
	.kind = bench::Type::memory,
	.run = &sem_memory_run,
	.teardown = &sem_memory_teardown,
};
static constexpr bench::CaseSpec event_memory_spec{
	.name = "event_memory",
	.kind = bench::Type::memory,
	.run = &event_memory_run,
	.teardown = &event_memory_teardown,
};
static constexpr bench::CaseSpec condvar_memory_spec{
	.name = "condvar_memory",
	.kind = bench::Type::memory,
	.run = &condvar_memory_run,
	.teardown = &condvar_memory_teardown,
};
static constexpr bench::CaseSpec mutex_lock_unlock_spec{
	.name = "mutex_lock_unlock",
	.kind = bench::Type::latency,
	.run = &mutex_lock_unlock_run,
	.setup = &mutex_lock_unlock_setup,
	.teardown = &mutex_lock_unlock_teardown,
};
static constexpr bench::CaseSpec mutex_create_destroy_spec{
	.name = "mutex_create_destroy",
	.kind = bench::Type::latency,
	.run = &mutex_create_destroy_run,
};
static constexpr bench::CaseSpec mutex_contention_spec{
	.name = "mutex_contention_2t",
	.kind = bench::Type::throughput,
	.run = &mutex_contention_run,
	.setup = &mutex_contention_setup,
	.teardown = &mutex_contention_teardown,
};
static constexpr bench::CaseSpec sem_take_give_spec{
	.name = "sem_take_give",
	.kind = bench::Type::latency,
	.run = &sem_take_give_run,
	.setup = &sem_take_give_setup,
	.teardown = &sem_take_give_teardown,
};
static constexpr bench::CaseSpec sem_create_destroy_spec{
	.name = "sem_create_destroy",
	.kind = bench::Type::latency,
	.run = &sem_create_destroy_run,
};
static constexpr bench::CaseSpec event_signal_wait_spec{
	.name = "event_signal_wait",
	.kind = bench::Type::latency,
	.run = &event_signal_wait_run,
	.setup = &event_signal_wait_setup,
	.teardown = &event_signal_wait_teardown,
	.iterations = 500,
};
static constexpr bench::CaseSpec condvar_signal_wait_spec{
	.name = "condvar_signal_wait",
	.kind = bench::Type::latency,
	.run = &condvar_signal_wait_run,
	.setup = &condvar_signal_wait_setup,
	.teardown = &condvar_signal_wait_teardown,
	.iterations = 500,
};
static constexpr bench::CaseSpec rmtx_lock_unlock_spec{
	.name = "recursive_mutex_lock_unlock",
	.kind = bench::Type::latency,
	.run = &rmtx_lock_unlock_run,
	.setup = &rmtx_lock_unlock_setup,
	.teardown = &rmtx_lock_unlock_teardown,
};

static constexpr bench_case_t sync_cases[] = {
	bench::case_<mutex_memory_spec>(),
	bench::case_<sem_memory_spec>(),
	bench::case_<event_memory_spec>(),
	bench::case_<condvar_memory_spec>(),
	bench::case_<mutex_lock_unlock_spec>(),
	bench::case_<mutex_create_destroy_spec>(),
	bench::case_<mutex_contention_spec>(),
	bench::case_<sem_take_give_spec>(),
	bench::case_<sem_create_destroy_spec>(),
	bench::case_<event_signal_wait_spec>(),
	bench::case_<condvar_signal_wait_spec>(),
	bench::case_<rmtx_lock_unlock_spec>(),
};

OVE_BENCH_SUITE(bench_suite_sync, "sync", sync_is_enabled, sync_cases)
