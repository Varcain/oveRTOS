/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_ws Simulation WebSocket Server
 * @brief Lightweight WebSocket + HTTP server for the sim dashboard.
 *
 * Runs in a dedicated pthread, serves static dashboard files over HTTP,
 * and upgrades to WebSocket for real-time binary frames (display,
 * audio, events, commands).
 * @{
 */

#ifndef OVE_SIM_WS_H
#define OVE_SIM_WS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum concurrent WebSocket connections. */
#define OVE_SIM_WS_MAX_CLIENTS 4

/**
 * @brief WebSocket frame types sent to clients.
 */
enum ove_sim_ws_frame_type {
	OVE_SIM_WS_FRAME_FB       = 0x01, /**< Framebuffer data. */
	OVE_SIM_WS_FRAME_AUDIO    = 0x02, /**< Audio PCM data. */
	OVE_SIM_WS_FRAME_EVENT    = 0x03, /**< Plugin event. */
	OVE_SIM_WS_FRAME_CMD      = 0x04, /**< Plugin command (from client). */
	OVE_SIM_WS_FRAME_STATE    = 0x05, /**< Plugin state JSON. */
	OVE_SIM_WS_FRAME_LOG      = 0x06, /**< Console log text (UTF-8). */
	OVE_SIM_WS_FRAME_INPUT    = 0x07, /**< Pointer input (x, y, pressed). */
};

struct ove_sim_transport;

/**
 * @brief Start the WebSocket server.
 *
 * Spawns a listener thread on @p port.  Serves static files from
 * @p dashboard_path over HTTP and upgrades /ws to WebSocket.
 *
 * @param[in] port            TCP port to bind (e.g. 8080).
 * @param[in] dashboard_path  Path to the dashboard static files directory.
 * @param[in] transport       Transport to read events from and write commands to.
 * @return 0 on success, negative error code on failure.
 */
int ove_sim_ws_start(uint16_t port, const char *dashboard_path,
		     struct ove_sim_transport *transport);

/**
 * @brief Stop the WebSocket server and close all connections.
 */
void ove_sim_ws_stop(void);

/**
 * @brief Broadcast a binary frame to all connected WebSocket clients.
 *
 * @param[in] type     Frame type tag (first 4 bytes of the WS message).
 * @param[in] payload  Frame payload.
 * @param[in] len      Payload length in bytes.
 * @return 0 on success, negative if no clients connected.
 */
int ove_sim_ws_broadcast(enum ove_sim_ws_frame_type type,
			 const void *payload, size_t len);

/**
 * @brief Check if any WebSocket clients are connected.
 * @return Non-zero if at least one client is connected.
 */
int ove_sim_ws_has_clients(void);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_WS_H */
