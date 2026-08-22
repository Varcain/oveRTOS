/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS host policy and board services for LXP's FreeRTOS MPU port.
 * Native FreeRTOS task/trap/MPU mechanics belong to modules/lxp/ports/freertos.
 */

#include "FreeRTOS.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>

#include "lxp/arch/cortex_m_memory.h"
#include "lxp/arch/cortex_m_mpu.h"
#include "lxp/ports/freertos.h"
#include "lxp_ove_thread_adapter.h"
#include "ove/build.h"
#include "lxp_ove_memory_layout.h"
#include "ove/time.h"
#include "ove_freertos_tick.h"
#include "ove_lxp_memory_contract.h"
#include "ove_config.h"

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
#include "bsp.h"
#include "stm32f7xx.h"

void ove_freertos_lxp_host_fatal(uint32_t cfsr, uint32_t hfsr, uint32_t pc);
#endif

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
#include "ove/lxp_metrics.h"

uint32_t ove_lxp_metrics_counter_hz(void)
{
	return SystemCoreClock;
}
#endif

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
#define LXP_EXT_STORAGE_SECTION ".sdram_bss.lxp"
#else
#define LXP_EXT_STORAGE_SECTION ".psram.lxp"
#endif

/* Largest-alignment rows come first so every PMSAv7 row remains aligned for
 * odd region counts. Board linker scripts place this cold object in external
 * memory and keep it out of the firmware image. */
struct lxp_ove_freertos_guest_storage {
	uint8_t dyn_pools[LXP_NREG][LXP_DYN_POOL_SIZE];
	uint8_t prog_regions[LXP_NREG][LXP_PROG_REGION_SIZE];
	lxp_exec_capture_t exec_captures[LXP_NSLOT];
	struct lxp_ove_thread_snapshot thread_snapshot;
#if LXP_ENABLE_NETFS_EXEC
	uint8_t netfs_exec_stage[256u * 1024u];
#endif
};

static struct lxp_ove_freertos_guest_storage g_lxp_storage
	__attribute__((section(LXP_EXT_STORAGE_SECTION), aligned(LXP_DYN_POOL_SIZE)));
_Static_assert(offsetof(struct lxp_ove_freertos_guest_storage, prog_regions) %
			       LXP_PROG_REGION_SIZE ==
		       0u,
	       "program rows must be aligned to their MPU region size");

#if defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
_Static_assert(OVE_LXP_ROOTFS_END == OVE_LXP_GUEST_POOL_BASE,
	       "AN500 rootfs and guest-pool ranges must be adjacent");
_Static_assert(sizeof(g_lxp_storage) <= OVE_LXP_GUEST_POOL_SIZE,
	       "AN500 guest storage overflows its generated pool");
#endif

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
static struct lxp_cortex_m_cache_geometry g_cache_geometry;
#endif

static int host_thread_list(struct lxp_thread_info *out, size_t max_count, size_t *actual_count,
			    lxp_freertos_slot_lookup_t slot_lookup)
{
	return lxp_ove_thread_snapshot_read(&g_lxp_storage.thread_snapshot, out, max_count,
					    actual_count, slot_lookup);
}

#define LXP_SYSTEM_VERSION \
	"FreeRTOS " tskKERNEL_VERSION_NUMBER " ove-" OVE_BUILD_OVERTOS_REV " lxp-" OVE_BUILD_LXP_REV
_Static_assert(sizeof(LXP_SYSTEM_VERSION) <= 65u, "uname version exceeds Linux utsname field");

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
static int host_random_fill(void *buf, size_t len)
{
	return bsp_random_fill(buf, len);
}

static void host_cache_clean(const void *base, size_t len)
{
	if (!len)
		return;
	uintptr_t addr = (uintptr_t)base;
	if (addr > UINTPTR_MAX - (len - 1u))
		return;
	uintptr_t start = addr & ~(uintptr_t)31u;
	uintptr_t end = ((addr + len - 1u) & ~(uintptr_t)31u) + 32u;
	SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
}

static void host_cache_invalidate(const void *base, size_t len)
{
	if (!len)
		return;
	uintptr_t addr = (uintptr_t)base;
	if (addr > UINTPTR_MAX - (len - 1u))
		return;
	uintptr_t start = addr & ~(uintptr_t)31u;
	uintptr_t end = ((addr + len - 1u) & ~(uintptr_t)31u) + 32u;
	SCB_CleanInvalidateDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
}

static int host_validate_static_mpu(void)
{
	struct lxp_cortex_m_mpu_snapshot snapshot;
	if (lxp_cortex_m_mpu_snapshot_read(&snapshot) != 0 ||
	    snapshot.count != configTOTAL_MPU_REGIONS ||
	    (snapshot.ctrl & (LXP_CORTEX_M_MPU_CTRL_ENABLE | LXP_CORTEX_M_MPU_CTRL_PRIVDEFENA)) !=
		    (LXP_CORTEX_M_MPU_CTRL_ENABLE | LXP_CORTEX_M_MPU_CTRL_PRIVDEFENA))
		return 0;

	const uintptr_t framebuffer_base = 0xc0000000u;
	const size_t framebuffer_size = 480u * 272u * 2u;
	const uintptr_t storage_base = (uintptr_t)&g_lxp_storage;
	const size_t storage_size = sizeof(g_lxp_storage);
	if (storage_base < framebuffer_base + framebuffer_size || storage_base > 0xc07ff800u ||
	    storage_size > 0xc07ff800u - storage_base)
		return 0;

	for (unsigned i = 0; i < snapshot.count; i++)
		if (lxp_cortex_m_mpu_region_overlaps_enabled(&snapshot.regions[i], framebuffer_base,
							     framebuffer_size) ||
		    lxp_cortex_m_mpu_region_overlaps_enabled(&snapshot.regions[i], storage_base,
							     storage_size))
			return 0;

	/* LXP's FreeRTOS kernel patch returns the broad peripheral descriptor to
	 * the task profile. Guests receive devices only through explicit policy. */
	for (unsigned i = 0; i < snapshot.count; i++)
		if ((snapshot.regions[i].access & 0x2u) != 0u &&
		    lxp_cortex_m_mpu_region_overlaps_enabled(&snapshot.regions[i], 0x40000000u,
							     0x20000000u))
			return 0;
	return 1;
}

static int host_prepare(void)
{
	/* The LXP trap posts a FreeRTOS semaphore from SVCall context. */
	NVIC_SetPriority(SVCall_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	return 0;
}

static int host_validate_memory_contract(const lxp_cpu_memory_contract_t *declared,
					 const struct lxp_cortex_m_cache_geometry *geometry)
{
	return lxp_cortex_m_memory_contract_matches_cache(declared, geometry) &&
			       host_validate_static_mpu()
		       ? LXP_OK
		       : LXP_ERR_INVALID_PARAM;
}
#define HOST_MEMORY_VALIDATOR host_validate_memory_contract
#else
/* Deterministic and explicitly non-cryptographic: QEMU is a development port. */
static int host_random_fill(void *buf, size_t len)
{
	static uint32_t state = 0x6f766572u;
	uint8_t *out = buf;
	for (size_t i = 0; i < len; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		out[i] = (uint8_t)(state >> 24);
	}
	return LXP_OK;
}

static int host_prepare(void)
{
	uintptr_t storage_base = (uintptr_t)&g_lxp_storage;
	uintptr_t storage_end = storage_base + sizeof(g_lxp_storage);
	return storage_base >= OVE_LXP_GUEST_POOL_BASE && storage_end <= OVE_LXP_GUEST_POOL_END
		       ? 0
		       : -1;
}

#define HOST_MEMORY_VALIDATOR ove_lxp_validate_uncached_memory_contract
#endif

const lxp_freertos_port_config_t g_lxp_freertos_port_config = {
	.abi_version = LXP_FREERTOS_PORT_CONFIG_ABI_VERSION,
	.struct_size = sizeof(lxp_freertos_port_config_t),
	.program_regions = &g_lxp_storage.prog_regions[0][0],
	.program_region_stride = LXP_PROG_REGION_SIZE,
	.program_region_count = LXP_NREG,
	.dynamic_pools = &g_lxp_storage.dyn_pools[0][0],
	.dynamic_pool_stride = LXP_DYN_POOL_SIZE,
	.dynamic_pool_count = LXP_NREG,
	.exec_captures = g_lxp_storage.exec_captures,
	.exec_capture_count = LXP_NSLOT,
#if LXP_ENABLE_NETFS_EXEC
	.exec_stage = g_lxp_storage.netfs_exec_stage,
	.exec_stage_size = sizeof(g_lxp_storage.netfs_exec_stage),
#endif
	.guest_memory_texscb = configTEX_S_C_B_SRAM,
#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
	.rootfs_region_count = 1u,
	.rootfs_regions =
		{
			{
				.base = OVE_LXP_ROOTFS_MPU0_BASE,
				.size = OVE_LXP_ROOTFS_MPU0_SIZE,
				.texscb = 0x02u,
			},
		},
#elif defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
	.rootfs_region_count = 2u,
	.rootfs_regions =
		{
			{
				.base = OVE_LXP_ROOTFS_MPU0_BASE,
				.size = OVE_LXP_ROOTFS_MPU0_SIZE,
				.texscb = configTEX_S_C_B_SRAM,
			},
			{
				.base = OVE_LXP_ROOTFS_MPU1_BASE,
				.size = OVE_LXP_ROOTFS_MPU1_SIZE,
				.texscb = configTEX_S_C_B_SRAM,
			},
		},
#endif
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	.coordinator_cacheable_map = 1u,
#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
	.coordinator_rootfs_region = 2u,
#else
	.coordinator_rootfs_region = UINT8_MAX,
#endif
	.cpu_memory_contract = OVE_LXP_MEMORY_CONTRACT_STM32F746_INITIALIZER,
	.cache_geometry = &g_cache_geometry,
	.cache_clean = host_cache_clean,
	.cache_invalidate = host_cache_invalidate,
	.host_fatal = ove_freertos_lxp_host_fatal,
#else
	.coordinator_rootfs_region = UINT8_MAX,
	.cpu_memory_contract = OVE_LXP_MEMORY_CONTRACT_UNCACHED_INITIALIZER,
#endif
	.guest_quantum_ms = CONFIG_OVE_LINUX_GUEST_QUANTUM_MS,
	.host_prepare = host_prepare,
	.tick_subscribe = ove_freertos_tick_subscribe,
	.tick_unsubscribe = ove_freertos_tick_unsubscribe,
	.time_us = ove_time_get_us,
	.time_ns = ove_time_get_ns,
	.thread_list = host_thread_list,
	.mem_stats = lxp_ove_mem_stats_read,
	.system_version = LXP_SYSTEM_VERSION,
	.random_fill = host_random_fill,
	.validate_memory_contract = HOST_MEMORY_VALIDATOR,
#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	.svc_cycle_counter = (volatile const uint32_t *)0xe0001004u,
#endif
};
