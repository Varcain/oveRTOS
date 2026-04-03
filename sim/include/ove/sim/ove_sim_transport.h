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

/* ── Transport factory declarations ────────────────────────────────── */

/**
 * @brief Create the in-process direct transport (POSIX mode).
 *
 * Uses SPSC ring buffers and a pthread condition variable for
 * signalling between the firmware threads and the WebSocket server.
 *
 * @param[out] t  Transport to initialise.
 * @return 0 on success, negative error code on failure.
 */
int ove_sim_transport_direct_create(struct ove_sim_transport *t);

/**
 * @brief Create the shared-memory transport (QEMU mode).
 *
 * Opens /dev/shm/ove-sim and uses SPSC rings for cross-process IPC
 * following the same layout as qemu_audio_shm.h / qemu_net_shm.h.
 *
 * @param[out] t  Transport to initialise.
 * @return 0 on success, negative error code on failure.
 */
int ove_sim_transport_shm_create(struct ove_sim_transport *t);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_TRANSPORT_H */
