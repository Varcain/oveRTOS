/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_transport Simulation Transport Layer
 * @brief Abstract transport for sim plugin events and commands.
 *
 * Provides a transport-agnostic interface so plugin code is identical
 * for POSIX (in-process ring buffers) and QEMU (shared-memory IPC).
 * @{
 */

#ifndef OVE_SIM_TRANSPORT_H
#define OVE_SIM_TRANSPORT_H

#include "ove_sim_plugin.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Transport operations vtable.
 */
struct ove_sim_transport_ops {
	/**
	 * @brief Open the transport.
	 *
	 * @param[in] t         Transport instance.
	 * @param[in] endpoint  Backend-specific endpoint string (path, URL, etc.).
	 * @return 0 on success, negative error code on failure.
	 */
	int (*open)(struct ove_sim_transport *t, const char *endpoint);

	/** @brief Close the transport and release resources. */
	void (*close)(struct ove_sim_transport *t);

	/**
	 * @brief Send an event from the firmware side to the dashboard.
	 *
	 * @param[in] t      Transport instance.
	 * @param[in] event  Event to send (header + payload).
	 * @return 0 on success, negative error code on failure.
	 */
	int (*send_event)(struct ove_sim_transport *t,
			  const struct ove_sim_event *event);

	/**
	 * @brief Receive a command from the dashboard.
	 *
	 * Blocks for up to @p timeout_ms.  Returns immediately if a command
	 * is already queued.
	 *
	 * @param[in]  t          Transport instance.
	 * @param[out] cmd        Buffer to receive the command into.
	 * @param[in]  cmd_size   Size of @p cmd buffer in bytes.
	 * @param[in]  timeout_ms Maximum wait time (0 = poll, UINT32_MAX = forever).
	 * @return 0 on success, OVE_ERR_TIMEOUT if no command arrived.
	 */
	int (*recv_cmd)(struct ove_sim_transport *t,
			struct ove_sim_cmd *cmd, size_t cmd_size,
			uint32_t timeout_ms);

	/* ── Host-side ops (used by the WS server / bridge) ────────── */

	/**
	 * @brief Read the next event from the firmware (host-side).
	 *
	 * Counterpart to @c send_event -- reads what firmware wrote.
	 * May be NULL if the transport has no in-process host side.
	 */
	int (*read_event)(struct ove_sim_transport *t,
			  void *buf, size_t buf_size,
			  uint16_t *out_len, uint32_t timeout_ms);

	/**
	 * @brief Inject a command from the host side (dashboard → firmware).
	 *
	 * Counterpart to @c recv_cmd -- writes what firmware will read.
	 * May be NULL if the transport has no in-process host side.
	 */
	int (*write_cmd)(struct ove_sim_transport *t,
			 const void *data, uint16_t len);

	/* ── High-bandwidth display/audio channels ─────────────────── */

	/**
	 * @brief Flush a display region to the dashboard.
	 *
	 * Carries XRGB8888 pixel data.  Transport decides delivery
	 * (WS mailbox, shared FB, semihosting file, etc.).
	 */
	int (*flush_display)(struct ove_sim_transport *t,
			     const void *fb, size_t fb_len,
			     uint16_t x1, uint16_t y1,
			     uint16_t x2, uint16_t y2);

	/**
	 * @brief Push PCM audio output to the dashboard.
	 */
	int (*push_audio)(struct ove_sim_transport *t,
			  const void *samples, size_t len,
			  uint32_t sample_rate, uint16_t channels,
			  uint16_t bit_depth);

	/**
	 * @brief Pull PCM audio input from the dashboard.
	 * @return Number of bytes read (0 if no data available).
	 */
	size_t (*pull_audio)(struct ove_sim_transport *t,
			     void *samples, size_t len);
};

/**
 * @brief Transport instance.
 */
struct ove_sim_transport {
	const struct ove_sim_transport_ops *ops;  /**< Vtable. */
	void                               *priv; /**< Backend-private state. */
};

/* ── Convenience inline wrappers ───────────────────────────────────── */

static inline int ove_sim_transport_open(struct ove_sim_transport *t,
					 const char *endpoint)
{
	return t->ops->open(t, endpoint);
}

static inline void ove_sim_transport_close(struct ove_sim_transport *t)
{
	t->ops->close(t);
}

static inline int ove_sim_transport_send_event(struct ove_sim_transport *t,
					       const struct ove_sim_event *ev)
{
	return t->ops->send_event(t, ev);
}

static inline int ove_sim_transport_recv_cmd(struct ove_sim_transport *t,
					     struct ove_sim_cmd *cmd,
					     size_t cmd_size,
					     uint32_t timeout_ms)
{
	return t->ops->recv_cmd(t, cmd, cmd_size, timeout_ms);
}

static inline int ove_sim_transport_read_event(struct ove_sim_transport *t,
					       void *buf, size_t buf_size,
					       uint16_t *out_len,
					       uint32_t timeout_ms)
{
	if (!t->ops->read_event)
		return -1;
	return t->ops->read_event(t, buf, buf_size, out_len, timeout_ms);
}

static inline int ove_sim_transport_write_cmd(struct ove_sim_transport *t,
					      const void *data, uint16_t len)
{
	if (!t->ops->write_cmd)
		return -1;
	return t->ops->write_cmd(t, data, len);
}

static inline int ove_sim_transport_flush_display(struct ove_sim_transport *t,
						  const void *fb, size_t fb_len,
						  uint16_t x1, uint16_t y1,
						  uint16_t x2, uint16_t y2)
{
	if (!t || !t->ops->flush_display)
		return -1;
	return t->ops->flush_display(t, fb, fb_len, x1, y1, x2, y2);
}

static inline int ove_sim_transport_push_audio(struct ove_sim_transport *t,
					       const void *samples, size_t len,
					       uint32_t sr, uint16_t ch,
					       uint16_t bd)
{
	if (!t || !t->ops->push_audio)
		return -1;
	return t->ops->push_audio(t, samples, len, sr, ch, bd);
}

static inline size_t ove_sim_transport_pull_audio(struct ove_sim_transport *t,
						  void *samples, size_t len)
{
	if (!t || !t->ops->pull_audio)
		return 0;
	return t->ops->pull_audio(t, samples, len);
}

/* ── Transport factory declarations ────────────────────────────────── */

/**
 * @brief Create the local shared-memory transport (host POSIX mode).
 *
 * Creates /dev/shm/ove-{sim,fb,audio} via mmap.  The external
 * dashboard bridge (ove-dashboard-bridge.py) reads the shmem.
 *
 * @param[out] t  Transport to initialise.
 * @return 0 on success, negative error code on failure.
 */
int ove_sim_transport_shm_local_create(struct ove_sim_transport *t);

/**
 * @brief Create the shared-memory transport -- host side (QEMU bridge).
 *
 * Opens /dev/shm/ove-sim via mmap for cross-process IPC.
 * Used by a host-side process that serves the WebSocket dashboard.
 *
 * @param[out] t  Transport to initialise.
 * @return 0 on success, negative error code on failure.
 */
int ove_sim_transport_shm_create(struct ove_sim_transport *t);

/**
 * @brief Create the shared-memory transport -- guest side (QEMU firmware).
 *
 * Opens /dev/shm/ove-sim, /dev/shm/ove-fb, /dev/shm/ove-audio via
 * semihosting for display/audio/event I/O from inside the QEMU guest.
 *
 * @param[out] t  Transport to initialise.
 * @return 0 on success, negative error code on failure.
 */
int ove_sim_transport_shm_guest_create(struct ove_sim_transport *t);

/**
 * @brief Get the global transport set by @c ove_sim_set_transport().
 */
struct ove_sim_transport *ove_sim_get_transport(void);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_TRANSPORT_H */
