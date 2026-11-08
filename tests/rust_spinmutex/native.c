/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>

#ifndef OVE_SPIN_REAL_PROVIDER
#define OVE_SPIN_REAL_PROVIDER 0
#endif
#if OVE_SPIN_REAL_PROVIDER
uint64_t spin_real_irq_lock(void);
void spin_real_irq_unlock(uint64_t);

/* Compile-only aliases in the real provider check setup/lock outcomes before
 * the Rust callback can receive data. Other process mutexes are unaffected. */
#define CHECKED_CALL(name, declaration, arguments) \
	int spin_checked_##name declaration        \
	{                                          \
		int result = name arguments;       \
		if (result != 0)                   \
			exit(2);                   \
		return result;                     \
	}
CHECKED_CALL(pthread_once, (pthread_once_t * once, void (*init)(void)), (once, init))
CHECKED_CALL(pthread_mutexattr_init, (pthread_mutexattr_t * attr), (attr))
CHECKED_CALL(pthread_mutexattr_settype, (pthread_mutexattr_t * attr, int type), (attr, type))
CHECKED_CALL(pthread_mutexattr_destroy, (pthread_mutexattr_t * attr), (attr))
CHECKED_CALL(pthread_mutex_init, (pthread_mutex_t * mutex, const pthread_mutexattr_t *attr),
	     (mutex, attr))
CHECKED_CALL(pthread_mutex_lock, (pthread_mutex_t * mutex), (mutex))
CHECKED_CALL(pthread_mutex_unlock, (pthread_mutex_t * mutex), (mutex))
#undef CHECKED_CALL
#endif

/* All modeled keys are initialized scalar records, never native task tokens.
 * Invalid restoration is recorded before changing state. Independent cleanup
 * releases only valid keys in reverse order on the still-owning worker. */
struct key_record {
	uint64_t opaque, native;
	unsigned mask;
};
static _Thread_local struct key_record keys[64];
static _Thread_local unsigned depth, mask = 37;
static _Atomic uint32_t generation;
static _Atomic unsigned violations, interventions;

uint64_t ove_irq_lock(void)
{
	if (depth == 64)
		abort();
	uint64_t native = 0;
#if OVE_SPIN_REAL_PROVIDER
	native = spin_real_irq_lock();
#endif
	uint64_t serial = atomic_fetch_add_explicit(&generation, 1, memory_order_relaxed) + 1;
	struct key_record record = {.opaque = UINT64_C(0xc0de000000000000) | (serial << 8) | mask,
				    .native = native,
				    .mask = mask};
	keys[depth++] = record;
	mask = 1;
	return record.opaque;
}

void ove_irq_unlock(uint64_t key)
{
	if (!depth || keys[depth - 1].opaque != key) {
		atomic_fetch_add_explicit(&violations, 1, memory_order_relaxed);
		return;
	}
	struct key_record record = keys[--depth];
	mask = record.mask;
#if OVE_SPIN_REAL_PROVIDER
	spin_real_irq_unlock(record.native);
#endif
}

unsigned spin_depth(void)
{
	return depth;
}
unsigned spin_mask(void)
{
	return mask;
}
unsigned spin_violations(void)
{
	return atomic_load_explicit(&violations, memory_order_relaxed);
}
unsigned spin_interventions(void)
{
	return atomic_load_explicit(&interventions, memory_order_relaxed);
}
unsigned spin_real_enabled(void)
{
	return OVE_SPIN_REAL_PROVIDER;
}
void spin_cleanup(void)
{
	while (depth) {
		atomic_fetch_add_explicit(&interventions, 1, memory_order_relaxed);
		ove_irq_unlock(keys[depth - 1].opaque);
	}
}
