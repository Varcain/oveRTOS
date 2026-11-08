/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_SYNC_CONTRACT_ZEPHYR_H
#define OVE_SYNC_CONTRACT_ZEPHYR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Complete host model objects, not measurements of target ABI. */
struct k_thread {
	uintptr_t opaque;
};
struct k_msgq {
	uintptr_t opaque;
	uint32_t max_msgs;
};
struct k_timer {
	uintptr_t opaque;
};
struct k_work {
	uintptr_t opaque;
};
struct k_event {
	uintptr_t opaque;
};
struct k_work_delayable {
	uintptr_t opaque;
};
struct k_work_q {
	uintptr_t opaque;
};
struct k_spinlock {
	uintptr_t opaque;
};
typedef uint8_t k_thread_stack_t;
struct k_obj_core {
	struct k_obj_core *next;
	bool linked;
};
struct k_mutex {
	struct k_thread *owner;
	unsigned lock_count;
	bool initialized;
#ifdef CONFIG_OBJ_CORE_MUTEX
	struct k_obj_core obj_core;
#endif
};
struct k_sem {
	unsigned count, limit;
	bool initialized;
#ifdef CONFIG_OBJ_CORE_SEM
	struct k_obj_core obj_core;
#endif
};
struct k_condvar {
	bool initialized;
#ifdef CONFIG_OBJ_CORE_CONDVAR
	struct k_obj_core obj_core;
#endif
};
typedef struct {
	int64_t ticks;
} k_timeout_t;
#define K_FOREVER ((k_timeout_t){.ticks = -1})
#define K_NO_WAIT ((k_timeout_t){.ticks = 0})
#define K_NSEC(ns) ((k_timeout_t){.ticks = (int64_t)(ns)})
int k_mutex_init(struct k_mutex *);
int k_mutex_lock(struct k_mutex *, k_timeout_t);
int k_mutex_unlock(struct k_mutex *);
int k_sem_init(struct k_sem *, unsigned, unsigned);
int k_sem_take(struct k_sem *, k_timeout_t);
void k_sem_give(struct k_sem *);
int k_condvar_init(struct k_condvar *);
int k_condvar_wait(struct k_condvar *, struct k_mutex *, k_timeout_t);
int k_condvar_signal(struct k_condvar *);
int k_condvar_broadcast(struct k_condvar *);
void k_obj_core_unlink(struct k_obj_core *);
void *k_malloc(size_t);
void k_free(void *);
bool k_is_in_isr(void);
#endif
