/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM sampling profiler backend.
 *
 * WebAssembly pthreads have no signals and no raw PCs, so the POSIX
 * SIGRTMIN + backtrace(3) model does not port. Instead:
 *
 *  1. The sim-debug pump calls ove_backend_profiler_sample_tick() at
 *     CONFIG_OVE_PROFILER_HZ. That sets a per-thread `profiler_pending`
 *     flag on every RUNNING thread tracked by the WASM thread backend.
 *
 *  2. Each thread checks its own flag at every yield point (sleep, yield,
 *     sync-primitive wait). When pending, it calls
 *     emscripten_get_callstack() against its own stack, parses the
 *     returned human-readable callstack string into function names,
 *     maps each name to a synthetic pseudo-PC via an internal hash
 *     table, and pushes an ove_profiler_sample to the shared ring.
 *
 *  3. As new function names are encountered, their (pseudo_pc, name)
 *     pairs are queued for emission so the dashboard can populate its
 *     symbol dictionary and render flame / flat-top views using the
 *     same JSON shape the POSIX bridge synthesises from nm output.
 *
 * Bias caveat: threads caught in a tight, yield-free loop never sample
 * themselves. Surface this in the profiler panel header.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PROFILER

#include <emscripten/emscripten.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ove/profiler.h"
#include "ove/thread.h"
#include "ove/thread_state_stats.h"
#include "ove/types.h"

#include "ove_profiler_ring.h"
#include "ove_storage_wasm.h"

#ifndef CONFIG_OVE_PROFILER_HZ
#define CONFIG_OVE_PROFILER_HZ 100
#endif

/* Hooks supplied by wasm_thread.c */
extern struct ove_thread *ove_backend_thread_current_struct(void);
extern size_t ove_backend_profiler_flag_running(void);

/* ── Symbol table: function-name → pseudo-PC ─────────────────────── */

/*
 * Fixed-size open-addressing hash. When the arena or table fills up,
 * new frames hash to pseudo_pc == 0 and the dashboard renders them as
 * "0x0" — preferred over a malloc storm in the sample hot path.
 */
#define WASM_SYM_TABLE_CAP 512
#define WASM_SYM_NAME_MAX 96
#define WASM_SYM_ARENA_BYTES (WASM_SYM_TABLE_CAP * WASM_SYM_NAME_MAX)

struct wasm_sym_entry {
	uint32_t pseudo_pc; /* 0 == empty slot */
	uint32_t name_off;  /* offset into sym_arena */
	uint16_t name_len;
	uint16_t _pad;
};

static struct wasm_sym_entry sym_table[WASM_SYM_TABLE_CAP];
static char sym_arena[WASM_SYM_ARENA_BYTES];
static size_t sym_arena_used;
static uint32_t sym_next_pseudo_pc = 1;
static pthread_mutex_t sym_lock = PTHREAD_MUTEX_INITIALIZER;

/* Pending new symbols waiting to be drained by sim_profiler_tick. */
#define NEW_SYM_QUEUE_CAP 64
static uint32_t new_sym_queue[NEW_SYM_QUEUE_CAP];
static size_t new_sym_queue_count;

static uint32_t djb2(const char *s, size_t len)
{
	uint32_t h = 5381;
	for (size_t i = 0; i < len; i++)
		h = ((h << 5) + h) + (uint8_t)s[i];
	return h;
}

/*
 * Look up an ASCII function name; insert if new. Returns a non-zero
 * pseudo-PC on success, 0 on table / arena exhaustion.
 */
static uint32_t sym_intern(const char *name, size_t name_len)
{
	if (name_len == 0)
		return 0;
	if (name_len > WASM_SYM_NAME_MAX - 1)
		name_len = WASM_SYM_NAME_MAX - 1;

	uint32_t h = djb2(name, name_len);

	pthread_mutex_lock(&sym_lock);
	for (int i = 0; i < WASM_SYM_TABLE_CAP; i++) {
		uint32_t idx = (h + (uint32_t)i) % WASM_SYM_TABLE_CAP;
		struct wasm_sym_entry *e = &sym_table[idx];

		if (e->pseudo_pc == 0) {
			if (sym_arena_used + name_len + 1 > WASM_SYM_ARENA_BYTES) {
				pthread_mutex_unlock(&sym_lock);
				return 0;
			}
			e->name_off = (uint32_t)sym_arena_used;
			e->name_len = (uint16_t)name_len;
			memcpy(sym_arena + sym_arena_used, name, name_len);
			sym_arena[sym_arena_used + name_len] = '\0';
			sym_arena_used += name_len + 1;
			e->pseudo_pc = sym_next_pseudo_pc++;

			if (new_sym_queue_count < NEW_SYM_QUEUE_CAP)
				new_sym_queue[new_sym_queue_count++] = idx;

			uint32_t pc = e->pseudo_pc;
			pthread_mutex_unlock(&sym_lock);
			return pc;
		}

		if (e->name_len == name_len &&
		    memcmp(sym_arena + e->name_off, name, name_len) == 0) {
			uint32_t pc = e->pseudo_pc;
			pthread_mutex_unlock(&sym_lock);
			return pc;
		}
	}
	pthread_mutex_unlock(&sym_lock);
	return 0;
}

/*
 * Parse the multi-line string emscripten_get_callstack() writes. Each
 * line looks roughly like "    at funcname (anything)\n" with
 * EM_LOG_NO_PATHS — we extract whatever sits between "at " and the
 * next space or '('. Returns number of frames captured (≤ max_pcs).
 */
static uint8_t parse_callstack(char *buf, uintptr_t *pcs, uint8_t max_pcs)
{
	uint8_t depth = 0;
	char *p = buf;

	while (depth < max_pcs && *p) {
		char *newline = strchr(p, '\n');
		if (newline)
			*newline = '\0';

		char *at = strstr(p, "at ");
		if (at) {
			at += 3;
			char *end = at;
			while (*end && *end != ' ' && *end != '(' && *end != '\t')
				end++;
			size_t name_len = (size_t)(end - at);
			if (name_len > 0) {
				uint32_t pc = sym_intern(at, name_len);
				if (pc != 0)
					pcs[depth++] = (uintptr_t)pc;
			}
		}

		if (!newline)
			break;
		p = newline + 1;
	}
	return depth;
}

/* ── Rate control (shared with POSIX shape) ──────────────────────── */

static atomic_int profiler_running;
static atomic_uint sample_divisor = 1;
static atomic_uint sample_counter;

/* ── Backend API ─────────────────────────────────────────────────── */

int ove_backend_profiler_start(void)
{
	int expected = 0;
	if (!atomic_compare_exchange_strong_explicit(&profiler_running, &expected, 1,
						     memory_order_acq_rel, memory_order_relaxed))
		return OVE_OK;
	return OVE_OK;
}

void ove_backend_profiler_stop(void)
{
	atomic_store_explicit(&profiler_running, 0, memory_order_release);
}

void ove_backend_profiler_sample_tick(void)
{
	if (!atomic_load_explicit(&profiler_running, memory_order_acquire))
		return;

	unsigned div = atomic_load_explicit(&sample_divisor, memory_order_acquire);
	if (div > 1) {
		unsigned c =
			atomic_fetch_add_explicit(&sample_counter, 1, memory_order_acq_rel) + 1;
		if ((c % div) != 0)
			return;
	}

	/* Flag every RUNNING thread. Each will self-capture on its next
	 * yield point (sleep / yield / sync wait). */
	(void)ove_backend_profiler_flag_running();
}

void ove_backend_profiler_set_rate(uint32_t hz)
{
	if (hz == 0 || hz > (uint32_t)CONFIG_OVE_PROFILER_HZ)
		hz = (uint32_t)CONFIG_OVE_PROFILER_HZ;
	unsigned div = (unsigned)((uint32_t)CONFIG_OVE_PROFILER_HZ / hz);
	if (div == 0)
		div = 1;
	atomic_store_explicit(&sample_divisor, div, memory_order_release);
	atomic_store_explicit(&sample_counter, 0, memory_order_release);
}

uint32_t ove_backend_profiler_get_max_hz(void)
{
	return (uint32_t)CONFIG_OVE_PROFILER_HZ;
}

/*
 * Called from every yield point in wasm_thread.c (and via the trace
 * marker hooks in posix_sync.c when TRACE_MARKERS is also on).
 */
void ove_backend_profiler_check(void)
{
	if (!atomic_load_explicit(&profiler_running, memory_order_acquire))
		return;

	struct ove_thread *t = ove_backend_thread_current_struct();
	if (!t || !t->profiler_pending)
		return;
	t->profiler_pending = 0;

	char buf[4096];
	int n = emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_NO_PATHS, buf, sizeof(buf));
	if (n <= 0)
		return;
	/* emscripten_get_callstack NUL-terminates within maxbytes, but keep
	 * a belt-and-braces cap in case the runtime ever returns the
	 * requested size without the terminator. */
	if ((size_t)n >= sizeof(buf))
		buf[sizeof(buf) - 1] = '\0';

	struct ove_profiler_sample s;
	memset(&s, 0, sizeof(s));
	s.ts_us = ove_state_stats_now_us();
	s.tid = (uint32_t)(uintptr_t)t;
	s.state = (uint8_t)t->state;
	s.depth = parse_callstack(buf, s.pcs, (uint8_t)CONFIG_OVE_PROFILER_MAX_DEPTH);

	if (s.depth > 0)
		(void)ove_profiler_ring_push(&s);
}

/*
 * Drain any newly-interned symbols as a JSON array of
 * [pseudo_pc, pseudo_pc+1, "name"] triples compatible with the existing
 * PROFILE_SUB_SYMBOLS payload shape. Returns bytes written (0 if
 * nothing to emit or buffer too small).
 */
size_t ove_backend_profiler_drain_symbols(char *out, size_t out_max)
{
	if (!out || out_max < 3)
		return 0;

	pthread_mutex_lock(&sym_lock);
	size_t queued = new_sym_queue_count;
	if (queued == 0) {
		pthread_mutex_unlock(&sym_lock);
		return 0;
	}

	size_t w = 0;
	out[w++] = '[';

	size_t emitted = 0;
	for (size_t i = 0; i < queued; i++) {
		struct wasm_sym_entry *e = &sym_table[new_sym_queue[i]];
		const char *name = sym_arena + e->name_off;
		/* Worst-case JSON per entry: '[4294967295,4294967296,"…"],'
		 * == 2 × 10 digits + 2 brackets + 2 quotes + comma + name
		 * ≈ name_len + 32. Stop when next entry may not fit. */
		size_t need = (size_t)e->name_len + 32;
		if (w + need >= out_max)
			break;

		int wrote = snprintf(out + w, out_max - w, "%s[%u,%u,\"%.*s\"]",
				     emitted == 0 ? "" : ",", (unsigned)e->pseudo_pc,
				     (unsigned)(e->pseudo_pc + 1), (int)e->name_len, name);
		if (wrote < 0 || (size_t)wrote >= out_max - w)
			break;
		w += (size_t)wrote;
		emitted++;
	}

	out[w++] = ']';

	/* Drop the emitted entries from the queue; keep any overflow for
	 * the next drain. */
	if (emitted < queued) {
		memmove(new_sym_queue, new_sym_queue + emitted,
			(queued - emitted) * sizeof(new_sym_queue[0]));
		new_sym_queue_count = queued - emitted;
	} else {
		new_sym_queue_count = 0;
	}
	pthread_mutex_unlock(&sym_lock);

	if (emitted == 0)
		return 0;
	return w;
}

#endif /* CONFIG_OVE_PROFILER */
