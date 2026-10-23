/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * The ring is guarded by a critical section, which interrupts can enter,
 * not a mutex: the _from_isr calls share it with the thread paths.  Threads
 * wait on two semaphores, posted as hints once a write reaches the trigger
 * level or a read frees room; a woken thread rechecks the ring.  A timed
 * wait keeps one deadline, so wakeups that bring too little do not extend
 * it.
 */

#include "ove/stream.h"
#include "ove/storage.h"
#include "ove_ns_to_ticks.h"
#include <nuttx/irq.h>
#include <nuttx/semaphore.h>
#include <nuttx/clock.h>
#include <nuttx/wdog.h>
#include <stdbool.h>

/* Inside the critical section: whether to post @p sem once it is left.  At
 * most one token is kept, so a burst of writes or reads wakes one waiter
 * instead of queueing a post for each. */
static bool hint_wanted(sem_t *sem)
{
	int value = 0;
	(void)nxsem_get_value(sem, &value);
	return value <= 0;
}

/* A wait too long for a watchdog to time is a wait forever: wd_start refuses
 * a delay of WDOG_MAX_DELAY ticks or more, nxsem_tickwait takes 32 bits, and
 * a 32-bit tick deadline that far out would already look passed. */
static uint64_t wait_limit(uint64_t timeout_ns)
{
	const uint64_t max_ticks = (uint64_t)WDOG_MAX_DELAY < UINT32_MAX ? (uint64_t)WDOG_MAX_DELAY
									 : UINT32_MAX;
	if (timeout_ns != OVE_WAIT_FOREVER && timeout_ns / NSEC_PER_TICK >= max_ticks - 1) {
		return OVE_WAIT_FOREVER;
	}
	return timeout_ns;
}

static clock_t deadline_for(uint64_t timeout_ns)
{
	return timeout_ns == OVE_WAIT_FOREVER ? 0
					      : clock_systime_ticks() + ove_ns_to_ticks(timeout_ns);
}

static bool deadline_passed(uint64_t timeout_ns, clock_t deadline)
{
	return timeout_ns != OVE_WAIT_FOREVER && clock_compare(deadline, clock_systime_ticks());
}

static void wait_hint(sem_t *sem, uint64_t timeout_ns, clock_t deadline)
{
	if (timeout_ns == OVE_WAIT_FOREVER) {
		(void)nxsem_wait_uninterruptible(sem);
		return;
	}
	clock_t now = clock_systime_ticks();
	if (!clock_compare(deadline, now))
		(void)nxsem_tickwait_uninterruptible(sem, (uint32_t)(deadline - now));
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_stream_init(ove_stream_t *stream, ove_stream_storage_t *storage, void *buffer, size_t size,
		    size_t trigger)
{
	if (stream == NULL || storage == NULL || buffer == NULL || size == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	ove_ring_setup(&storage->ring, (unsigned char *)buffer, size);
	storage->trigger = (trigger > 0) ? trigger : 1; /* 0 is treated as 1 */
	storage->notify_cb = NULL;
	storage->notify_ud = NULL;
	nxsem_init(&storage->data_sem, 0, 0);
	nxsem_init(&storage->space_sem, 0, 0);
#ifdef CONFIG_PRIORITY_INHERITANCE
	/* Posted from interrupts: no holders (nxsem_init already leaves them
	 * so; this keeps it if that changes). */
	nxsem_set_protocol(&storage->data_sem, SEM_PRIO_NONE);
	nxsem_set_protocol(&storage->space_sem, SEM_PRIO_NONE);
#endif

	*stream = storage;
	return OVE_OK;
}

void ove_stream_deinit(ove_stream_t stream)
{
	if (stream != NULL) {
		nxsem_destroy(&stream->space_sem);
		nxsem_destroy(&stream->data_sem);
	}
}

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_stream_send(ove_stream_t stream, const void *data, size_t len, uint64_t timeout_ns,
		    size_t *bytes_sent)
{
	struct ove_stream *ns = stream;
	if (ns == NULL || (data == NULL && len > 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (len == 0) {
		if (bytes_sent != NULL) {
			*bytes_sent = 0;
		}
		return OVE_OK;
	}

	const unsigned char *src = data;
	size_t written = 0;
	timeout_ns = wait_limit(timeout_ns);
	clock_t deadline = deadline_for(timeout_ns);

	while (written < len) {
		bool wrote = false;
		bool post = false;

		irqstate_t flags = enter_critical_section();
		if (ns->ring.count < ns->ring.size) {
			written += ove_ring_write(&ns->ring, src + written, len - written);
			post = ns->ring.count >= ns->trigger && hint_wanted(&ns->data_sem);
			wrote = true;
		}
		leave_critical_section(flags);

		if (post) {
			nxsem_post(&ns->data_sem);
		}
		if (wrote) {
			continue;
		}
		if (deadline_passed(timeout_ns, deadline)) {
			break;
		}
		wait_hint(&ns->space_sem, timeout_ns, deadline);
	}

	irqstate_t flags = enter_critical_section();
	ove_notify_cb notify_cb = ns->notify_cb;
	void *notify_ud = ns->notify_ud;
	leave_critical_section(flags);
	if (written > 0 && notify_cb != NULL) {
		notify_cb(notify_ud);
	}

	if (bytes_sent != NULL) {
		*bytes_sent = written;
	}
	return OVE_OK;
}

int ove_stream_receive(ove_stream_t stream, void *buf, size_t len, uint64_t timeout_ns,
		       size_t *bytes_received)
{
	struct ove_stream *ns = stream;
	if (ns == NULL || (buf == NULL && len > 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (len == 0) {
		if (bytes_received != NULL) {
			*bytes_received = 0;
		}
		return OVE_OK;
	}

	size_t received = 0;
	timeout_ns = wait_limit(timeout_ns);
	clock_t deadline = deadline_for(timeout_ns);

	/* Wait until at least `trigger` bytes are present, then take up to `len`
	 * of what is there without waiting again. */
	for (;;) {
		bool got = false;
		bool post = false;

		irqstate_t flags = enter_critical_section();
		if (ns->ring.count >= ns->trigger) {
			received = ove_ring_read(&ns->ring, (unsigned char *)buf, len);
			post = received > 0 && hint_wanted(&ns->space_sem);
			got = true;
		}
		leave_critical_section(flags);

		if (post) {
			nxsem_post(&ns->space_sem);
		}
		if (got || deadline_passed(timeout_ns, deadline)) {
			break;
		}
		wait_hint(&ns->data_sem, timeout_ns, deadline);
	}

	if (bytes_received != NULL) {
		*bytes_received = received;
	}
	return OVE_OK;
}

int ove_stream_send_from_isr(ove_stream_t stream, const void *data, size_t len, size_t *bytes_sent)
{
	struct ove_stream *ns = stream;
	if (ns == NULL || (data == NULL && len > 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (len == 0) {
		if (bytes_sent != NULL) {
			*bytes_sent = 0;
		}
		return OVE_OK;
	}

	irqstate_t flags = enter_critical_section();
	size_t written = ove_ring_write(&ns->ring, (const unsigned char *)data, len);
	bool post = written > 0 && ns->ring.count >= ns->trigger && hint_wanted(&ns->data_sem);
	ove_notify_cb notify_cb = ns->notify_cb;
	void *notify_ud = ns->notify_ud;
	leave_critical_section(flags);

	if (post) {
		nxsem_post(&ns->data_sem);
	}
	if (written > 0 && notify_cb != NULL) {
		notify_cb(notify_ud);
	}
	if (bytes_sent != NULL) {
		*bytes_sent = written;
	}
	return OVE_OK;
}

int ove_stream_receive_from_isr(ove_stream_t stream, void *buf, size_t len, size_t *bytes_received)
{
	struct ove_stream *ns = stream;
	if (ns == NULL || (buf == NULL && len > 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (len == 0) {
		if (bytes_received != NULL) {
			*bytes_received = 0;
		}
		return OVE_OK;
	}

	irqstate_t flags = enter_critical_section();
	size_t read_bytes = ove_ring_read(&ns->ring, (unsigned char *)buf, len);
	bool post = read_bytes > 0 && hint_wanted(&ns->space_sem);
	leave_critical_section(flags);

	if (post) {
		nxsem_post(&ns->space_sem);
	}
	if (bytes_received != NULL) {
		*bytes_received = read_bytes;
	}
	return OVE_OK;
}

int ove_stream_reset(ove_stream_t stream)
{
	struct ove_stream *ns = stream;
	if (ns == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	irqstate_t flags = enter_critical_section();
	ove_ring_clear(&ns->ring);
	bool post = hint_wanted(&ns->space_sem);
	leave_critical_section(flags);

	if (post) {
		nxsem_post(&ns->space_sem);
	}
	return OVE_OK;
}

size_t ove_stream_bytes_available(ove_stream_t stream)
{
	struct ove_stream *ns = stream;
	if (ns == NULL) {
		return 0;
	}

	irqstate_t flags = enter_critical_section();
	size_t count = ns->ring.count;
	leave_critical_section(flags);
	return count;
}

size_t ove_stream_space_available(ove_stream_t stream)
{
	struct ove_stream *ns = stream;
	if (ns == NULL) {
		return 0;
	}

	irqstate_t flags = enter_critical_section();
	size_t space = ns->ring.size - ns->ring.count;
	leave_critical_section(flags);
	return space;
}

int ove_stream_set_notify(ove_stream_t stream, ove_notify_cb cb, void *user_data)
{
	struct ove_stream *ns = stream;
	if (ns == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	irqstate_t flags = enter_critical_section();
	ns->notify_cb = cb;
	ns->notify_ud = user_data;
	leave_critical_section(flags);
	return OVE_OK;
}
