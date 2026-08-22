/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS-owned LXP host composition. Keep provider selection and the concrete
 * engine table out of applications.
 */

#include "lxp_ove_host_internal.h"

#include "ove_config.h"
#include "ove/thread.h"

#include <string.h>

#if defined(CONFIG_OVE_LINUX_ROOTFS_EXTERNAL)
#include "ove/lxp_memory_layout.h"
#else
#include "loader_rootfs_image.h"
#endif

extern const lxp_os_ops_t g_lxp_host_engine;

#if defined(CONFIG_OVE_LINUX_NET)
extern const lxp_net_ops_t g_lxp_host_net_ops;
#define OVE_LXP_NET_OPS (&g_lxp_host_net_ops)
#else
#define OVE_LXP_NET_OPS NULL
#endif

#if defined(CONFIG_OVE_LINUX_DEV)
extern const lxp_display_ops_t g_lxp_host_display_ops;
#define OVE_LXP_DISPLAY_OPS (&g_lxp_host_display_ops)
#else
#define OVE_LXP_DISPLAY_OPS NULL
#endif

#if defined(CONFIG_OVE_LINUX_FS)
extern const lxp_fs_ops_t g_lxp_host_fs_ops;
#define OVE_LXP_FS_OPS (&g_lxp_host_fs_ops)
#else
#define OVE_LXP_FS_OPS NULL
#endif

#if defined(CONFIG_OVE_LINUX_BLOCK)
extern const lxp_block_ops_t g_lxp_host_block_ops;
#define OVE_LXP_BLOCK_OPS (&g_lxp_host_block_ops)
#else
#define OVE_LXP_BLOCK_OPS NULL
#endif

#if defined(CONFIG_OVE_LINUX_NET)
static int address_present(const ove_sockaddr_t *address)
{
	return address->addr[0] || address->addr[1] || address->addr[2] || address->addr[3];
}
#endif

/* Reset live handles without clearing the fixed rootfs workspace. The CPIO
 * parser overwrites the indexed prefix and publishes an exact entry count. */
static void host_runtime_reset(ove_lxp_host_impl_t *host)
{
	memset(&host->core, 0, sizeof(host->core));
	memset(&host->netif_storage, 0, sizeof(host->netif_storage));
	host->netif = NULL;
	host->netif_initialized = 0u;
	host->netif_up = 0u;
}

void ove_lxp_host_deinit(ove_lxp_host_t *host)
{
	if (!host)
		return;
	ove_lxp_host_impl_t *impl = ove_lxp_host_private(host);
#if defined(CONFIG_OVE_LINUX_NET)
	if (impl->netif_up)
		ove_netif_down(impl->netif);
	if (impl->netif_initialized)
		ove_netif_deinit(impl->netif);
#endif
	host_runtime_reset(impl);
}

int ove_lxp_host_init(ove_lxp_host_t *host)
{
	if (!host)
		return OVE_ERR_INVALID_PARAM;
	ove_lxp_host_config_t config = {0};
#if defined(CONFIG_OVE_LINUX_ROOTFS_EXTERNAL)
	config.rootfs_image = (const void *)OVE_LXP_ROOTFS_BASE;
	config.rootfs_image_size = (size_t)OVE_LXP_ROOTFS_SIZE;
#else
	config.rootfs_image = ove_test_rootfs_cpio;
	config.rootfs_image_size = (size_t)ove_test_rootfs_cpio_len;
#endif

#if defined(CONFIG_OVE_LINUX_NET)
	ove_netif_config_t netif = {0};
#if defined(CONFIG_OVE_LINUX_NETIF_DHCP)
	netif.use_dhcp = 1;
#else
	if (ove_sockaddr_parse_ipv4(&netif.static_ip, CONFIG_OVE_LINUX_NETIF_IPV4_ADDRESS, 0) !=
		    OVE_OK ||
	    ove_sockaddr_parse_ipv4(&netif.netmask, CONFIG_OVE_LINUX_NETIF_IPV4_NETMASK, 0) !=
		    OVE_OK ||
	    ove_sockaddr_parse_ipv4(&netif.gateway, CONFIG_OVE_LINUX_NETIF_IPV4_GATEWAY, 0) !=
		    OVE_OK)
		return OVE_ERR_INVALID_PARAM;
#endif
	config.netif_config = &netif;
	config.netif_address_wait_ms = CONFIG_OVE_LINUX_NETIF_ADDRESS_WAIT_MS;
#endif

#if defined(CONFIG_OVE_LINUX_NETFS)
	const ove_lxp_netfs_config_t netfs = {
		.mountpoint = CONFIG_OVE_LINUX_NETFS_MOUNTPOINT,
		.server_ipv4 = CONFIG_OVE_LINUX_NETFS_SERVER_IP,
		.port = (uint16_t)CONFIG_OVE_LINUX_NETFS_PORT,
		.aname = CONFIG_OVE_LINUX_NETFS_ANAME,
		.uname = CONFIG_OVE_LINUX_NETFS_UNAME,
	};
	config.netfs_config = &netfs;
#endif

	return ove_lxp_host_init_cpio(host, &config);
}

int ove_lxp_host_init_cpio(ove_lxp_host_t *host, const ove_lxp_host_config_t *config)
{
	if (!host || !config)
		return OVE_ERR_INVALID_PARAM;
	ove_lxp_host_impl_t *impl = ove_lxp_host_private(host);
	host_runtime_reset(impl);

	lxp_netfs_config_t netfs;
	const lxp_netfs_config_t *netfs_config = NULL;
	if (config->netfs_config) {
		ove_sockaddr_t server;
		memset(&netfs, 0, sizeof(netfs));
		if (ove_sockaddr_parse_ipv4(&server, config->netfs_config->server_ipv4, 0) !=
		    OVE_OK)
			return OVE_ERR_INVALID_PARAM;
		memcpy(netfs.server_ip, server.addr, sizeof(netfs.server_ip));
		netfs.mountpoint = config->netfs_config->mountpoint;
		netfs.port = config->netfs_config->port;
		netfs.aname = config->netfs_config->aname;
		netfs.uname = config->netfs_config->uname;
		if (!lxp_netfs_config_valid(&netfs))
			return OVE_ERR_INVALID_PARAM;
		netfs_config = &netfs;
	}

#if defined(CONFIG_OVE_LINUX_NET)
	if (config->netif_config) {
		int rc = ove_netif_init(&impl->netif, &impl->netif_storage);
		if (rc != OVE_OK)
			return rc;
		impl->netif_initialized = 1u;
		rc = ove_netif_up(impl->netif, config->netif_config);
		if (rc != OVE_OK) {
			ove_lxp_host_deinit(host);
			return rc;
		}
		impl->netif_up = 1u;

		uint32_t waited_ms = 0u;
		for (;;) {
			ove_sockaddr_t address = {0};
			if (ove_netif_get_addr(impl->netif, &address, NULL, NULL) == OVE_OK &&
			    address_present(&address))
				break;
			if (waited_ms >= config->netif_address_wait_ms)
				break;
			uint32_t delay_ms = config->netif_address_wait_ms - waited_ms;
			if (delay_ms > 50u)
				delay_ms = 50u;
			ove_thread_sleep_ms(delay_ms);
			waited_ms += delay_ms;
		}
	}
#else
	if (config->netif_config) {
		ove_lxp_host_deinit(host);
		return OVE_ERR_NOT_SUPPORTED;
	}
#endif

	const lxp_host_config_t lxp_config = {
		.os_ops = &g_lxp_host_engine,
		.net_ops = OVE_LXP_NET_OPS,
		.display_ops = OVE_LXP_DISPLAY_OPS,
		.fs_ops = OVE_LXP_FS_OPS,
		.block_ops = OVE_LXP_BLOCK_OPS,
		.rootfs_image = config->rootfs_image,
		.rootfs_image_size = config->rootfs_image_size,
		.rootfs_storage = impl->rootfs_files,
		.rootfs_capacity = OVE_LXP_ROOTFS_FILE_CAPACITY,
		.rootfs_name_storage = impl->rootfs_names,
		.rootfs_name_capacity = OVE_LXP_ROOTFS_NAME_CAPACITY,
		.netif = (lxp_netif_t)impl->netif,
		.netfs_config = netfs_config,
	};
	int rc = lxp_host_init_cpio(&impl->core, &lxp_config);
	if (rc != LXP_OK)
		ove_lxp_host_deinit(host);
	return rc;
}

int ove_lxp_host_netif_get_addr(const ove_lxp_host_t *host, ove_sockaddr_t *ip,
				ove_sockaddr_t *gateway, ove_sockaddr_t *netmask)
{
#if defined(CONFIG_OVE_LINUX_NET)
	if (!host)
		return OVE_ERR_NOT_SUPPORTED;
	const ove_lxp_host_impl_t *impl = ove_lxp_host_private_const(host);
	if (!impl->netif_initialized)
		return OVE_ERR_NOT_SUPPORTED;
	return ove_netif_get_addr(impl->netif, ip, gateway, netmask);
#else
	(void)host;
	(void)ip;
	(void)gateway;
	(void)netmask;
	return OVE_ERR_NOT_SUPPORTED;
#endif
}

static uint8_t guest_exit_reason(uint8_t reason)
{
	switch (reason) {
	case LXP_EXIT_REASON_NORMAL:
		return OVE_LXP_EXIT_REASON_NORMAL;
	case LXP_EXIT_REASON_SIGNAL:
		return OVE_LXP_EXIT_REASON_SIGNAL;
	case LXP_EXIT_REASON_SIGNAL_DEPTH:
		return OVE_LXP_EXIT_REASON_SIGNAL_DEPTH;
	case LXP_EXIT_REASON_MEMORY_FAULT:
		return OVE_LXP_EXIT_REASON_MEMORY_FAULT;
	case LXP_EXIT_REASON_EXEC_RESOURCE:
		return OVE_LXP_EXIT_REASON_EXEC_RESOURCE;
	case LXP_EXIT_REASON_EXEC_LOAD:
		return OVE_LXP_EXIT_REASON_EXEC_LOAD;
	case LXP_EXIT_REASON_STATE_CORRUPTION:
		return OVE_LXP_EXIT_REASON_STATE_CORRUPTION;
	case LXP_EXIT_REASON_HOST_TRANSITION:
		return OVE_LXP_EXIT_REASON_HOST_TRANSITION;
	default:
		return OVE_LXP_EXIT_REASON_NONE;
	}
}

static void guest_exit_notify(void *ctx, const lxp_guest_exit_info_t *info)
{
	const ove_lxp_launch_config_t *config = ctx;
	if (!config || !config->on_guest_exit || !info)
		return;
	const ove_lxp_guest_exit_info_t translated = {
		.slot = info->slot,
		.pid = info->pid,
		.ppid = info->ppid,
		.status = info->status,
		.comm = info->comm,
		.reason = guest_exit_reason(info->reason),
		.signal = info->signal,
		.detail = info->detail,
		.address = info->address,
	};
	config->on_guest_exit(&translated);
}

int ove_lxp_host_run(const ove_lxp_host_t *host, const ove_lxp_launch_config_t *config,
		     const char *path, int argc, const char *const argv[])
{
	if (!host)
		return OVE_LXP_RUN_ELAUNCH;
	const ove_lxp_host_impl_t *impl = ove_lxp_host_private_const(host);
	lxp_launch_config_t launch;
	const lxp_launch_config_t *translated = NULL;
	if (config) {
		launch = (lxp_launch_config_t){
			.write_fn = config->write_fn,
			.read_fn = config->read_fn,
			.io_ctx = config->io_ctx,
			.on_enosys = config->on_enosys,
			.console_poll = config->console_poll,
			.env = config->env,
			.on_guest_exit = config->on_guest_exit ? guest_exit_notify : NULL,
			.guest_exit_ctx = (void *)config,
			.display_width = config->display_width,
			.display_height = config->display_height,
			.rt_scope_read = config->rt_scope_read,
			.rt_scope_ctx = config->rt_scope_ctx,
			.console_subscribe = config->console_subscribe,
			.console_unsubscribe = config->console_unsubscribe,
		};
		translated = &launch;
	}
	int rc = lxp_host_run(&impl->core, translated, path, argc, argv);
	switch (rc) {
	case LXP_RUN_ELAUNCH:
		return OVE_LXP_RUN_ELAUNCH;
	case LXP_RUN_EEXEC:
		return OVE_LXP_RUN_EEXEC;
	case LXP_RUN_ETIMEOUT:
		return OVE_LXP_RUN_ETIMEOUT;
	default:
		return rc;
	}
}
