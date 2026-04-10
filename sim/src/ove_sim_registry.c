/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/sim/ove_sim_plugin.h"
#include "ove/sim/ove_sim_transport.h"
#include "ove/types.h"

#include <string.h>

/* ── Plugin registry ───────────────────────────────────────────────── */

static struct ove_sim_plugin plugins[OVE_SIM_MAX_PLUGINS];
static int plugin_count;
static struct ove_sim_transport *global_transport;

int ove_sim_plugin_register(const struct ove_sim_plugin_ops *ops,
			    void *ctx, const void *config,
			    size_t config_len)
{
	if (!ops || !ops->init)
		return OVE_ERR_INVALID_PARAM;

	if (plugin_count >= OVE_SIM_MAX_PLUGINS)
		return OVE_ERR_NO_MEMORY;

	int id = plugin_count;
	struct ove_sim_plugin *p = &plugins[id];

	p->id = (uint32_t)id;
	p->ops = ops;
	p->ctx = ctx;
	p->transport = global_transport;

	int ret = ops->init(ctx, config, config_len);
	if (ret != OVE_OK)
		return ret;

	plugin_count++;
	return id;
}

void ove_sim_plugin_tick_all(uint32_t elapsed_ms)
{
	for (int i = 0; i < plugin_count; i++) {
		if (plugins[i].ops->tick)
			plugins[i].ops->tick(plugins[i].ctx, elapsed_ms);
	}
}

int ove_sim_plugin_dispatch_cmd(const struct ove_sim_cmd *cmd)
{
	if (!cmd)
		return OVE_ERR_INVALID_PARAM;

	/* Try exact plugin ID first. */
	if (cmd->plugin_id < (uint32_t)plugin_count) {
		struct ove_sim_plugin *p = &plugins[cmd->plugin_id];
		if (p->ops->handle_cmd)
			return p->ops->handle_cmd(p->ctx, cmd);
		return OVE_OK;
	}

	/* Plugin ID out of range — broadcast to all plugins.
	 * Each plugin's handle_cmd checks cmd_type and ignores
	 * commands it doesn't recognize. */
	for (int i = 0; i < plugin_count; i++) {
		if (plugins[i].ops->handle_cmd)
			plugins[i].ops->handle_cmd(plugins[i].ctx, cmd);
	}
	return OVE_OK;
}

int ove_sim_plugin_emit_event(uint32_t plugin_id,
			      const struct ove_sim_event *event)
{
	if (!event)
		return OVE_ERR_INVALID_PARAM;

	if (plugin_id >= (uint32_t)plugin_count)
		return OVE_ERR_INVALID_PARAM;

	struct ove_sim_plugin *p = &plugins[plugin_id];

	if (p->transport)
		return ove_sim_transport_send_event(p->transport, event);

	return OVE_OK;
}

void ove_sim_set_transport(struct ove_sim_transport *transport)
{
	global_transport = transport;

	/* Update all already-registered plugins. */
	for (int i = 0; i < plugin_count; i++)
		plugins[i].transport = transport;
}

struct ove_sim_transport *ove_sim_get_transport(void)
{
	return global_transport;
}

const struct ove_sim_plugin *ove_sim_plugin_get(uint32_t plugin_id)
{
	if (plugin_id >= (uint32_t)plugin_count)
		return NULL;

	return &plugins[plugin_id];
}

int ove_sim_plugin_count(void)
{
	return plugin_count;
}

/* Weak default — dashboard bridge reads logs from shmem/pipe. */
__attribute__((weak))
void ove_sim_log_broadcast(const char *msg, unsigned int len)
{
	(void)msg;
	(void)len;
}
