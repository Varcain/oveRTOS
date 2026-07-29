/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS declarations and live-cache checks for LXP's portable CPU-memory
 * contract. Native MPU descriptor checks remain engine-owned.
 */

#ifndef OVE_LXP_MEMORY_CONTRACT_H
#define OVE_LXP_MEMORY_CONTRACT_H

#include "lxp/lxp_port.h"
#include "ove_cortex_m_cache.h"

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

#if defined(__arm__) || defined(__thumb__)
static inline int ove_lxp_memory_contract_matches_cache(
	const lxp_cpu_memory_contract_t *contract,
	const struct ove_cortex_m_cache_geometry *geometry)
{
	if (!contract || !geometry)
		return 0;

	uint32_t flags = 0u;
	if ((OVE_SCB_CCR & OVE_SCB_CCR_DC) != 0u)
		flags |= LXP_CPU_MEMORY_DCACHE_ENABLED;
	if ((OVE_SCB_CCR & OVE_SCB_CCR_IC) != 0u)
		flags |= LXP_CPU_MEMORY_ICACHE_ENABLED;

	return contract->flags == flags &&
	       contract->dcache_line_size == geometry->d_line_size &&
	       contract->icache_line_size == geometry->i_line_size &&
	       contract->dcache_size == geometry->d_size &&
	       contract->icache_size == geometry->i_size;
}
#endif

#endif /* OVE_LXP_MEMORY_CONTRACT_H */
