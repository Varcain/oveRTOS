/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/stream.h"
#include "ove/storage.h"
#include <zephyr/kernel.h>

/*
 * Backed by a plain ring buffer (ove/ring.h) rather than k_pipe: the stream
 * API is trigger-aware (a receive blocks until at least `trigger` bytes are
 * present, see ove/stream.h) and k_pipe has no "wake when >= N bytes" mode.
 * The ring is guarded by a k_spinlock — usable from both thread and ISR
 * context, unlike k_mutex, which the *_from_isr ops require — and two
 * counting semaphores provide the blocking wait/wake. Sem counts are only
 * hints (every waiter re-checks the ring under the lock after waking), so
 * spurious gives are harmless.
 */

static void stream_setup(struct ove_stream *s, unsigned char *buffer, size_t size, size_t trigger)
{
	ove_ring_setup(&s->ring, buffer, size);
	s->trigger = (trigger > 0) ? trigger : 1; /* 0 is treated as 1 */
	s->notify_cb = NULL;
	s->notify_ud = NULL;
	s->lock = (struct k_spinlock){0};
	k_sem_init(&s->data_sem, 0, K_SEM_MAX_LIMIT);
	k_sem_init(&s->space_sem, 0, K_SEM_MAX_LIMIT);
}

static k_timeout_t ns_to_ktimeout(uint64_t timeout_ns)
{
	if (timeout_ns == OVE_WAIT_FOREVER) {
		return K_FOREVER;
	}
	if (timeout_ns == 0) {
		return K_NO_WAIT;
	}
	return K_NSEC(timeout_ns);
}

/* ── _init / _deinit ────────────────────────────────────────────────── */

int ove_stream_init(ove_stream_t *stream, ove_stream_storage_t *storage, void *buffer, size_t size,
		    size_t trigger)
{
	if (stream == NULL || storage == NULL || buffer == NULL || size == 0) {
		return OVE_ERR_INVALID_PARAM;
	}

	stream_setup(storage, (unsigned char *)buffer, size, trigger);
	*stream = storage;
	return OVE_OK;
}

void ove_stream_deinit(ove_stream_t stream)
{
	(void)stream;
}

/* ── Operations ─────────────────────────────────────────────────────── */

int ove_stream_send(ove_stream_t stream, const void *data, size_t len, uint64_t timeout_ns,
		    size_t *bytes_sent)
{
	if (stream == NULL || (data == NULL && len > 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (len == 0) {
		if (bytes_sent != NULL) {
			*bytes_sent = 0;
		}
		return OVE_OK;
	}

	const unsigned char *src = (const unsigned char *)data;
	size_t written = 0;
	k_timepoint_t end = sys_timepoint_calc(ns_to_ktimeout(timeout_ns));

	while (written < len) {
		k_spinlock_key_t key = k_spin_lock(&stream->lock);
		if (stream->ring.count < stream->ring.size) {
			written += ove_ring_write(&stream->ring, src + written, len - written);
			bool have_trigger = stream->ring.count >= stream->trigger;
			k_spin_unlock(&stream->lock, key);
			if (have_trigger) {
				k_sem_give(&stream->data_sem);
			}
			continue;
		}
		k_spin_unlock(&stream->lock, key);

		if (sys_timepoint_expired(end)) {
			break;
		}
		(void)k_sem_take(&stream->space_sem, sys_timepoint_timeout(end));
	}

	if (written > 0 && stream->notify_cb != NULL) {
		stream->notify_cb(stream->notify_ud);
	}
	if (bytes_sent != NULL) {
		*bytes_sent = written;
	}
	return OVE_OK;
}

int ove_stream_receive(ove_stream_t stream, void *buf, size_t len, uint64_t timeout_ns,
		       size_t *bytes_received)
{
	if (stream == NULL || (buf == NULL && len > 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (len == 0) {
		if (bytes_received != NULL) {
			*bytes_received = 0;
		}
		return OVE_OK;
	}

	size_t received = 0;
	k_timepoint_t end = sys_timepoint_calc(ns_to_ktimeout(timeout_ns));

	/* Block until at least `trigger` bytes are present, then drain up to
	 * `len` (whatever is available) without blocking again. */
	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&stream->lock);
		if (stream->ring.count >= stream->trigger) {
			received = ove_ring_read(&stream->ring, (unsigned char *)buf, len);
			k_spin_unlock(&stream->lock, key);
			if (received > 0) {
				k_sem_give(&stream->space_sem);
			}
			break;
		}
		k_spin_unlock(&stream->lock, key);

		if (sys_timepoint_expired(end)) {
			break; /* timed out with < trigger bytes available */
		}
		if (k_sem_take(&stream->data_sem, sys_timepoint_timeout(end)) != 0) {
			break;
		}
	}

	if (bytes_received != NULL) {
		*bytes_received = received;
	}
	return OVE_OK;
}

int ove_stream_send_from_isr(ove_stream_t stream, const void *data, size_t len, size_t *bytes_sent)
{
	if (stream == NULL || (data == NULL && len > 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (len == 0) {
		if (bytes_sent != NULL) {
			*bytes_sent = 0;
		}
		return OVE_OK;
	}

	k_spinlock_key_t key = k_spin_lock(&stream->lock);
	size_t written = ove_ring_write(&stream->ring, (const unsigned char *)data, len);
	bool have_trigger = stream->ring.count >= stream->trigger;
	k_spin_unlock(&stream->lock, key);

	if (written > 0 && have_trigger) {
		k_sem_give(&stream->data_sem);
	}
	if (written > 0 && stream->notify_cb != NULL) {
		stream->notify_cb(stream->notify_ud);
	}
	if (bytes_sent != NULL) {
		*bytes_sent = written;
	}
	return OVE_OK;
}

int ove_stream_receive_from_isr(ove_stream_t stream, void *buf, size_t len, size_t *bytes_received)
{
	if (stream == NULL || (buf == NULL && len > 0)) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (len == 0) {
		if (bytes_received != NULL) {
			*bytes_received = 0;
		}
		return OVE_OK;
	}

	k_spinlock_key_t key = k_spin_lock(&stream->lock);
	size_t received = ove_ring_read(&stream->ring, (unsigned char *)buf, len);
	k_spin_unlock(&stream->lock, key);

	if (received > 0) {
		k_sem_give(&stream->space_sem);
	}
	if (bytes_received != NULL) {
		*bytes_received = received;
	}
	return OVE_OK;
}

int ove_stream_reset(ove_stream_t stream)
{
	if (stream == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	k_spinlock_key_t key = k_spin_lock(&stream->lock);
	ove_ring_clear(&stream->ring);
	k_spin_unlock(&stream->lock, key);

	k_sem_give(&stream->space_sem); /* wake any blocked sender */
	return OVE_OK;
}

size_t ove_stream_bytes_available(ove_stream_t stream)
{
	if (stream == NULL) {
		return 0;
	}

	k_spinlock_key_t key = k_spin_lock(&stream->lock);
	size_t count = stream->ring.count;
	k_spin_unlock(&stream->lock, key);
	return count;
}

size_t ove_stream_space_available(ove_stream_t stream)
{
	if (stream == NULL) {
		return 0;
	}

	k_spinlock_key_t key = k_spin_lock(&stream->lock);
	size_t space = stream->ring.size - stream->ring.count;
	k_spin_unlock(&stream->lock, key);
	return space;
}

int ove_stream_set_notify(ove_stream_t stream, ove_notify_cb cb, void *user_data)
{
	if (stream == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	stream->notify_cb = cb;
	stream->notify_ud = user_data;
	return OVE_OK;
}
