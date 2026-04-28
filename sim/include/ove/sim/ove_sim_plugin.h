/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_plugin Simulation Plugin Interface
 * @brief Core plugin interface for the oveRTOS simulation framework.
 *
 * Plugins implement virtual peripherals (display, audio, LEDs, sensors,
 * bus devices) that are registered at startup and driven by the sim HAL.
 * The same plugin code works in both POSIX (in-process) and QEMU
 * (cross-process via shared memory) simulation modes.
 *
 * @note Follows the vtable pattern established by @ref ove_audio_node_ops.
 * @{
 */

#ifndef OVE_SIM_PLUGIN_H
#define OVE_SIM_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Plugin types ──────────────────────────────────────────────────── */

/** @brief Plugin type identifiers. */
enum ove_sim_plugin_type {
	OVE_SIM_PLUGIN_DISPLAY, /**< Display output (framebuffer capture). */
	OVE_SIM_PLUGIN_AUDIO,	/**< Audio I/O (PCM capture / injection). */
	OVE_SIM_PLUGIN_GPIO,	/**< GPIO port simulator. */
	OVE_SIM_PLUGIN_LED,	/**< LED state visualizer. */
	OVE_SIM_PLUGIN_I2C_DEV, /**< Virtual I2C device (sensor, EEPROM, ...). */
	OVE_SIM_PLUGIN_SPI_DEV, /**< Virtual SPI device (flash, display, ...). */
	OVE_SIM_PLUGIN_UART,	/**< Virtual UART / serial terminal. */
	OVE_SIM_PLUGIN_SENSOR,	/**< High-level sensor (accel, gyro, temp). */
	OVE_SIM_PLUGIN_BUTTON,	/**< Virtual button / touch input. */
	OVE_SIM_PLUGIN_NVS,	/**< Non-volatile storage simulator. */
};

/* ── Events and commands ───────────────────────────────────────────── */

/**
 * @brief Event pushed from a plugin to the host dashboard.
 *
 * Events are variable-length: the header is followed by @c data_len
 * bytes of plugin-specific payload.
 */
struct ove_sim_event {
	uint32_t plugin_id;    /**< Originating plugin instance. */
	uint32_t event_type;   /**< Plugin-specific event code. */
	uint32_t timestamp_ms; /**< Simulation time in milliseconds. */
	uint32_t data_len;     /**< Length of trailing payload in bytes. */
	uint8_t data[];	       /**< Plugin-specific payload. */
};

/**
 * @brief Command injected from the host dashboard into a plugin.
 *
 * Commands are variable-length: the header is followed by @c data_len
 * bytes of plugin-specific payload.
 */
struct ove_sim_cmd {
	uint32_t plugin_id; /**< Target plugin instance. */
	uint32_t cmd_type;  /**< Plugin-specific command code. */
	uint32_t data_len;  /**< Length of trailing payload in bytes. */
	uint8_t data[];	    /**< Plugin-specific payload. */
};

/* ── Plugin operations vtable ──────────────────────────────────────── */

/**
 * @brief Virtual function table for a simulation plugin.
 *
 * Each plugin kind provides an instance of this struct.  NULL callbacks
 * are treated as no-ops (except @c init, which must be provided).
 */
struct ove_sim_plugin_ops {
	/** Human-readable name: "display", "bme280", "ws2812", etc. */
	const char *name;

	/** Plugin type identifier. */
	enum ove_sim_plugin_type type;

	/**
	 * @brief Initialise the plugin instance.
	 *
	 * @param[in] ctx         Plugin-private context (allocated by caller).
	 * @param[in] config      Optional JSON/binary config from board.yaml.
	 * @param[in] config_len  Length of @p config in bytes, 0 if none.
	 * @return 0 on success, negative error code on failure.
	 */
	int (*init)(void *ctx, const void *config, size_t config_len);

	/**
	 * @brief Tear down the plugin and release resources.
	 * @param[in] ctx  Plugin-private context.
	 */
	void (*deinit)(void *ctx);

	/**
	 * @brief Handle an incoming command from the dashboard.
	 *
	 * @param[in] ctx  Plugin-private context.
	 * @param[in] cmd  Command to process.
	 * @return 0 on success, negative error code on failure.
	 */
	int (*handle_cmd)(void *ctx, const struct ove_sim_cmd *cmd);

	/**
	 * @brief Periodic tick callback.
	 *
	 * Called from the sim tick thread at approximately 1 kHz.
	 *
	 * @param[in] ctx         Plugin-private context.
	 * @param[in] elapsed_ms  Milliseconds since last tick.
	 */
	void (*tick)(void *ctx, uint32_t elapsed_ms);

	/**
	 * @brief Serialise current state for dashboard polling.
	 *
	 * Writes a JSON fragment describing the plugin's current state.
	 *
	 * @param[in]  ctx      Plugin-private context.
	 * @param[out] buf      Buffer to write state into.
	 * @param[in]  buf_len  Size of @p buf in bytes.
	 * @param[out] out_len  Actual bytes written.
	 * @return 0 on success, negative error code on failure.
	 */
	int (*get_state)(void *ctx, void *buf, size_t buf_len, size_t *out_len);
};

/* ── Registered plugin instance ────────────────────────────────────── */

/** Forward declaration. */
struct ove_sim_transport;

/**
 * @brief A registered plugin instance in the simulation registry.
 */
struct ove_sim_plugin {
	uint32_t id;			      /**< Unique plugin ID. */
	const struct ove_sim_plugin_ops *ops; /**< Vtable. */
	void *ctx;			      /**< Plugin-private context. */
	struct ove_sim_transport *transport;  /**< Transport for events. */
};

/* ── Registry API ──────────────────────────────────────────────────── */

/** Maximum number of concurrently registered plugins. */
#define OVE_SIM_MAX_PLUGINS 32

/**
 * @brief Register a plugin instance.
 *
 * @param[in] ops         Plugin vtable.
 * @param[in] ctx         Plugin-private context (ownership retained by caller).
 * @param[in] config      Optional config blob, or NULL.
 * @param[in] config_len  Config length in bytes.
 * @return Non-negative plugin ID on success, negative error code on failure.
 */
int ove_sim_plugin_register(const struct ove_sim_plugin_ops *ops, void *ctx, const void *config,
			    size_t config_len);

/**
 * @brief Tick all registered plugins.
 * @param[in] elapsed_ms  Milliseconds since last tick.
 */
void ove_sim_plugin_tick_all(uint32_t elapsed_ms);

/**
 * @brief Dispatch a command to the targeted plugin.
 * @param[in] cmd  Command with plugin_id identifying the target.
 * @return 0 on success, negative error code on failure.
 */
int ove_sim_plugin_dispatch_cmd(const struct ove_sim_cmd *cmd);

/**
 * @brief Emit an event from a plugin to the transport / dashboard.
 *
 * @param[in] plugin_id  Originating plugin's ID.
 * @param[in] event      Event to emit (header + payload).
 * @return 0 on success, negative error code on failure.
 */
int ove_sim_plugin_emit_event(uint32_t plugin_id, const struct ove_sim_event *event);

/**
 * @brief Set the transport used by all plugins for event emission.
 * @param[in] transport  Transport instance.
 */
void ove_sim_set_transport(struct ove_sim_transport *transport);

/**
 * @brief Get a registered plugin by ID.
 * @param[in] plugin_id  Plugin ID.
 * @return Pointer to the plugin, or NULL if not found.
 */
const struct ove_sim_plugin *ove_sim_plugin_get(uint32_t plugin_id);

/**
 * @brief Get the number of registered plugins.
 * @return Plugin count.
 */
int ove_sim_plugin_count(void);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_PLUGIN_H */
