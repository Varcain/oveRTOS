/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS board policy for LXP's portable CPU-memory contract. Cortex-M
 * geometry, executable publication, and live-cache validation are LXP-owned.
 */

#ifndef OVE_LXP_MEMORY_CONTRACT_H
#define OVE_LXP_MEMORY_CONTRACT_H

#include "lxp/arch/cortex_m_memory.h"

#define OVE_LXP_MEMORY_CONTRACT_UNCACHED_INITIALIZER                         \
	{                                                                    \
		.abi_version = LXP_CPU_MEMORY_CONTRACT_ABI_VERSION,           \
		.struct_size = sizeof(lxp_cpu_memory_contract_t),              \
		.model = LXP_CPU_MEM_UNCACHED,                                 \
		.normal_attrs = LXP_CPU_MEM_ATTR_NORMAL_NC_NSH,                \
	}

#define OVE_LXP_MEMORY_CONTRACT_STM32F746_INITIALIZER                        \
	{                                                                    \
		.abi_version = LXP_CPU_MEMORY_CONTRACT_ABI_VERSION,           \
		.struct_size = sizeof(lxp_cpu_memory_contract_t),              \
		.model = LXP_CPU_MEM_COHERENT_SAME_ATTRS,                      \
		.normal_attrs = LXP_CPU_MEM_ATTR_NORMAL_WBWA_NSH,              \
		.flags = LXP_CPU_MEMORY_DCACHE_ENABLED |                       \
			 LXP_CPU_MEMORY_ICACHE_ENABLED,                        \
		.dcache_line_size = 32u,                                       \
		.icache_line_size = 32u,                                       \
		.dcache_size = 4u * 1024u,                                     \
		.icache_size = 4u * 1024u,                                     \
	}

#endif /* OVE_LXP_MEMORY_CONTRACT_H */
