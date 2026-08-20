/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS-facing per-launch policy for the LXP Linux personality.
 */
#ifndef OVE_LXP_LAUNCH_H
#define OVE_LXP_LAUNCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Host-side attribution for a guest process termination. */
#define OVE_LXP_EXIT_REASON_NONE 0
#define OVE_LXP_EXIT_REASON_NORMAL 1
#define OVE_LXP_EXIT_REASON_SIGNAL 2
#define OVE_LXP_EXIT_REASON_SIGNAL_DEPTH 3
#define OVE_LXP_EXIT_REASON_MEMORY_FAULT 4
#define OVE_LXP_EXIT_REASON_EXEC_RESOURCE 5
#define OVE_LXP_EXIT_REASON_EXEC_LOAD 6
#define OVE_LXP_EXIT_REASON_STATE_CORRUPTION 7
#define OVE_LXP_EXIT_REASON_HOST_TRANSITION 8

/** A stable copy of one guest's termination metadata. */
typedef struct ove_lxp_guest_exit_info {
	int slot;
	int pid;
	int ppid;
	int status;
	const char *comm; /**< Valid only for the duration of the callback. */
	uint8_t reason;  /**< @c OVE_LXP_EXIT_REASON_* */
	uint8_t signal;
	uint16_t _pad;
	uint32_t detail;
	uintptr_t address;
} ove_lxp_guest_exit_info_t;

typedef long (*ove_lxp_write_fn)(void *ctx, int fd, const void *buf, size_t len);
typedef long (*ove_lxp_read_fn)(void *ctx, int fd, void *buf, size_t len);
typedef void (*ove_lxp_guest_exit_fn)(const ove_lxp_guest_exit_info_t *info);
typedef long (*ove_lxp_rt_scope_read_fn)(void *ctx, char *buf, size_t cap);
typedef void (*ove_lxp_console_ready_fn)(const void *context);
typedef int (*ove_lxp_console_subscribe_fn)(void *ctx, ove_lxp_console_ready_fn ready,
					    const void *ready_context);
typedef void (*ove_lxp_console_unsubscribe_fn)(void *ctx);

/** Per-launch application policy translated by the oveRTOS host facade.
 * Zero initialization selects every optional default. */
typedef struct ove_lxp_launch_config {
	ove_lxp_write_fn write_fn;
	ove_lxp_read_fn read_fn;
	void *io_ctx;
	void (*on_enosys)(long nr);
	int (*console_poll)(void *ctx);
	const char *const *env;
	ove_lxp_guest_exit_fn on_guest_exit;
	uint16_t display_width;
	uint16_t display_height;
	ove_lxp_rt_scope_read_fn rt_scope_read;
	void *rt_scope_ctx;
	ove_lxp_console_subscribe_fn console_subscribe;
	ove_lxp_console_unsubscribe_fn console_unsubscribe;
} ove_lxp_launch_config_t;

/** Negative outcomes from @ref ove_lxp_host_run; non-negative values are the
 * init process's exit status. */
#define OVE_LXP_RUN_ELAUNCH (-1)
#define OVE_LXP_RUN_EEXEC (-2)
#define OVE_LXP_RUN_ETIMEOUT (-3)

#ifdef __cplusplus
}
#endif

#endif /* OVE_LXP_LAUNCH_H */
