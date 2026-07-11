/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of the lxp module (the OS-agnostic Linux personality).
 *
 * Vendored foundation types — a self-contained subset of what the personality
 * used to draw from <ove/types.h> / <ove/thread.h>. The module owns these so it
 * builds against ANY host (no oveRTOS headers required). The numeric values are
 * pinned identical to their OVE originals so a host that bridges to an existing
 * oveRTOS stack needs zero translation.
 */

#ifndef LXP_TYPES_H
#define LXP_TYPES_H

#include <stddef.h>
#include <stdint.h>

#if !defined(__GNUC__) && !defined(__clang__)
#error "lxp requires a GCC-compatible compiler (gcc or clang)."
#endif

/* ---- public-API annotation macros ------------------------------------------ */
#if defined(__BINDGEN__) || defined(__ZIG_CIMPORT__) || defined(__EMSCRIPTEN__) || \
	defined(__LXP_LINT__)
#define LXP_NONNULL(...)
#define LXP_NODISCARD
#else
#define LXP_NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))
#define LXP_NODISCARD __attribute__((warn_unused_result))
#endif

/**
 * @brief lxp result / error codes.
 *
 * Zero (@c LXP_OK) on success, negative on error. The numeric values match
 * their `LXP_ERR_*` originals exactly (pinned by the static-asserts below) so
 * an oveRTOS host port is a zero-translation pass-through and the guest-errno
 * mapping is unaffected.
 */
typedef enum lxp_err {
	LXP_OK = 0,
	LXP_ERR_NOT_REGISTERED = -1,
	LXP_ERR_INVALID_PARAM = -2,
	LXP_ERR_NO_MEMORY = -3,
	LXP_ERR_TIMEOUT = -4,
	LXP_ERR_NOT_SUPPORTED = -5,
	LXP_ERR_QUEUE_FULL = -6,
	LXP_ERR_ML_FAILED = -7,
	LXP_ERR_NET_REFUSED = -8,
	LXP_ERR_NET_UNREACHABLE = -9,
	LXP_ERR_NET_ADDR_IN_USE = -10,
	LXP_ERR_NET_RESET = -11,
	LXP_ERR_NET_DNS_FAIL = -12,
	LXP_ERR_NET_CLOSED = -13,
	LXP_ERR_BUS_NACK = -14,
	LXP_ERR_BUS_BUSY = -15,
	LXP_ERR_BUS_ERROR = -16,
	LXP_ERR_QUEUE_EMPTY = -17,
	LXP_ERR_WOULD_BLOCK = -18,
	LXP_ERR_EOF = -19,
	LXP_ERR_INVAL = -20,
	LXP_ERR_NOT_FOUND = -21,
} lxp_err_t;

/** @brief Timeout value that means "block indefinitely". */
#define LXP_WAIT_FOREVER UINT64_MAX

/* ---- ABI pins (a re-numbering fails to compile) ---------------------------- */
#if defined(__cplusplus)
#define LXP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define LXP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
LXP_STATIC_ASSERT(LXP_ERR_INVALID_PARAM == -2, "LXP_ERR_INVALID_PARAM drifted");
LXP_STATIC_ASSERT(LXP_ERR_NO_MEMORY == -3, "LXP_ERR_NO_MEMORY drifted");
LXP_STATIC_ASSERT(LXP_ERR_TIMEOUT == -4, "LXP_ERR_TIMEOUT drifted");
LXP_STATIC_ASSERT(LXP_ERR_NOT_SUPPORTED == -5, "LXP_ERR_NOT_SUPPORTED drifted");
LXP_STATIC_ASSERT(LXP_ERR_NET_REFUSED == -8, "LXP_ERR_NET_REFUSED drifted");
LXP_STATIC_ASSERT(LXP_ERR_NET_UNREACHABLE == -9, "LXP_ERR_NET_UNREACHABLE drifted");
LXP_STATIC_ASSERT(LXP_ERR_NET_ADDR_IN_USE == -10, "LXP_ERR_NET_ADDR_IN_USE drifted");
LXP_STATIC_ASSERT(LXP_ERR_NET_RESET == -11, "LXP_ERR_NET_RESET drifted");
LXP_STATIC_ASSERT(LXP_ERR_NET_DNS_FAIL == -12, "LXP_ERR_NET_DNS_FAIL drifted");
LXP_STATIC_ASSERT(LXP_ERR_NET_CLOSED == -13, "LXP_ERR_NET_CLOSED drifted");
LXP_STATIC_ASSERT(LXP_ERR_NOT_FOUND == -21, "LXP_ERR_NOT_FOUND drifted");
LXP_STATIC_ASSERT(LXP_WAIT_FOREVER == UINT64_MAX, "LXP_WAIT_FOREVER drifted");

/* ---- thread introspection (for the ps/top /proc snapshot) ------------------ */
/** @brief Execution state of a host kernel thread. Values match ove_thread_state_t. */
typedef enum lxp_thread_state {
	LXP_THREAD_STATE_RUNNING = 0,
	LXP_THREAD_STATE_READY,
	LXP_THREAD_STATE_BLOCKED,
	LXP_THREAD_STATE_SUSPENDED,
	LXP_THREAD_STATE_TERMINATED,
	LXP_THREAD_STATE_UNKNOWN,
} lxp_thread_state_t;

/** @brief Cumulative time per thread state (microseconds). */
struct lxp_thread_state_times {
	uint64_t running_us;
	uint64_t ready_us;
	uint64_t blocked_us;
	uint64_t suspended_us;
};

/**
 * @brief Snapshot of one host kernel thread. Layout matches struct
 *        lxp_thread_info so an oveRTOS port fills it field-for-field.
 */
struct lxp_thread_info {
	const char *name;
	lxp_thread_state_t state;
	int priority;
	size_t stack_used;
	size_t stack_size;
	uint32_t cpu_percent_x100;
	struct lxp_thread_state_times state_times;
};

#endif /* LXP_TYPES_H */
