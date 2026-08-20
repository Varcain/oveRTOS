/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS-owned LXP host composition. Keep provider selection and the concrete
 * engine table out of applications.
 */

#include "ove/lxp_host.h"

#include "ove_config.h"
#include "ove/thread.h"

#include <string.h>

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

static int parse_ipv4(const char *text, uint8_t address[4])
{
	if (!text || !address)
		return OVE_ERR_INVALID_PARAM;
	for (unsigned octet = 0; octet < 4u; octet++) {
		unsigned value = 0u;
		unsigned digits = 0u;
		while (*text >= '0' && *text <= '9') {
			value = value * 10u + (unsigned)(*text - '0');
			if (value > 255u)
				return OVE_ERR_INVALID_PARAM;
			text++;
			digits++;
		}
		if (digits == 0u || (octet < 3u ? *text != '.' : *text != '\0'))
			return OVE_ERR_INVALID_PARAM;
		address[octet] = (uint8_t)value;
		if (octet < 3u)
			text++;
	}
	return OVE_OK;
}

static int address_present(const ove_sockaddr_t *address)
{
	return address->addr[0] || address->addr[1] || address->addr[2] || address->addr[3];
}

void ove_lxp_host_deinit(ove_lxp_host_t *host)
{
	if (!host)
		return;
#if defined(CONFIG_OVE_LINUX_NET)
	if (host->netif_up)
		ove_netif_down(host->netif);
	if (host->netif_initialized)
		ove_netif_deinit(host->netif);
#endif
	memset(host, 0, sizeof(*host));
}

int ove_lxp_host_init_cpio(ove_lxp_host_t *host, const ove_lxp_host_config_t *config)
{
	if (!host || !config)
		return OVE_ERR_INVALID_PARAM;
	memset(host, 0, sizeof(*host));

	lxp_netfs_config_t netfs;
	const lxp_netfs_config_t *netfs_config = NULL;
	if (config->netfs_config) {
		memset(&netfs, 0, sizeof(netfs));
		if (parse_ipv4(config->netfs_config->server_ipv4, netfs.server_ip) != OVE_OK)
			return OVE_ERR_INVALID_PARAM;
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
		int rc = ove_netif_init(&host->netif, &host->netif_storage);
		if (rc != OVE_OK)
			return rc;
		host->netif_initialized = 1u;
		rc = ove_netif_up(host->netif, config->netif_config);
		if (rc != OVE_OK) {
			ove_lxp_host_deinit(host);
			return rc;
		}
		host->netif_up = 1u;

		uint32_t waited_ms = 0u;
		for (;;) {
			ove_sockaddr_t address = {0};
			if (ove_netif_get_addr(host->netif, &address, NULL, NULL) == OVE_OK &&
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
		.rootfs_storage = config->rootfs_storage,
		.rootfs_capacity = config->rootfs_capacity,
		.rootfs_name_storage = config->rootfs_name_storage,
		.rootfs_name_capacity = config->rootfs_name_capacity,
		.netif = (lxp_netif_t)host->netif,
		.netfs_config = netfs_config,
	};
	int rc = lxp_host_init_cpio(&host->core, &lxp_config);
	if (rc != LXP_OK)
		ove_lxp_host_deinit(host);
	return rc;
}

int ove_lxp_host_netif_get_addr(const ove_lxp_host_t *host, ove_sockaddr_t *ip,
				ove_sockaddr_t *gateway, ove_sockaddr_t *netmask)
{
#if defined(CONFIG_OVE_LINUX_NET)
	if (!host || !host->netif_initialized)
		return OVE_ERR_NOT_SUPPORTED;
	return ove_netif_get_addr(host->netif, ip, gateway, netmask);
#else
	(void)host;
	(void)ip;
	(void)gateway;
	(void)netmask;
	return OVE_ERR_NOT_SUPPORTED;
#endif
}

int ove_lxp_host_run(const ove_lxp_host_t *host, const lxp_launch_config_t *config,
		     const char *path, int argc, const char *const argv[])
{
	if (!host)
		return LXP_RUN_ELAUNCH;
	return lxp_host_run(&host->core, config, path, argc, argv);
}
