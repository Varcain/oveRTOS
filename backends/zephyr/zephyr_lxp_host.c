/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS host policy and board services for LXP's Zephyr Cortex-M port.
 * Native K_USER thread, trap, memory-domain, parking and fault-containment
 * mechanics belong to modules/lxp/ports/zephyr.
 */

#include <zephyr/kernel.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/time_units.h>
#include <zephyr/version.h>

#include <stddef.h>
#include <stdint.h>

#include "lxp/arch/cortex_m_memory.h"
#include "lxp/arch/cortex_m_mpu.h"
#include "lxp/ports/zephyr.h"
#include "lxp_ove_thread_adapter.h"
#include "ove/build.h"
#include "ove/lxp_memory_layout.h"
#include "ove/time.h"
#include "ove_lxp_memory_contract.h"
#include "ove_zephyr_priority.h"
#include "ove_config.h"

#if defined(CONFIG_OVE_FB)
#include "ove/hal/hal_fb.h"
#endif

BUILD_ASSERT(CONFIG_SYSTEM_WORKQUEUE_PRIORITY == OVE_ZEPHYR_PRIO_SYSTEM_WORKQUEUE,
	     "system workqueue priority must match the Zephyr priority contract");
BUILD_ASSERT(OVE_ZEPHYR_PRIO_CRITICAL < OVE_ZEPHYR_PRIO_LXP_COORDINATOR &&
		     OVE_ZEPHYR_PRIO_LXP_COORDINATOR < OVE_ZEPHYR_PRIO_LXP_GUEST,
	     "critical, coordinator, and guest priorities must remain ordered");
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
BUILD_ASSERT(IS_ENABLED(CONFIG_CSPRNG_ENABLED) && IS_ENABLED(CONFIG_HARDWARE_DEVICE_CS_GENERATOR),
	     "STM32 Linux guests require a hardware-backed CSPRNG");
#endif
#if defined(CONFIG_NETWORKING)
BUILD_ASSERT(IS_ENABLED(CONFIG_NET_TC_THREAD_PREEMPTIVE),
	     "Linux network traffic classes must be preemptible");
BUILD_ASSERT(CONFIG_NET_TC_RX_THREAD_BASE_PRIO == OVE_ZEPHYR_PRIO_NET_TC &&
		     CONFIG_NET_TC_TX_THREAD_BASE_PRIO == OVE_ZEPHYR_PRIO_NET_TC,
	     "network traffic-class priorities must match the Zephyr priority contract");
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
BUILD_ASSERT(IS_ENABLED(CONFIG_ETH_STM32_HAL_RX_THREAD_PREEMPTIVE),
	     "STM32 Ethernet RX must be preemptible");
BUILD_ASSERT(CONFIG_ETH_STM32_HAL_RX_THREAD_PRIO == OVE_ZEPHYR_PRIO_ABOVE_NORMAL,
	     "STM32 Ethernet RX priority must match the Zephyr priority contract");
#endif
#endif

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
#define OVE_PROG_RAM_NODE DT_NODELABEL(sdram1)
#else
#define OVE_PROG_RAM_NODE DT_NODELABEL(lxp_pool_ram)
#endif

#if defined(CONFIG_MPU_REQUIRES_POWER_OF_TWO_ALIGNMENT)
#define LXP_EXT_STORAGE_ALIGN LXP_DYN_POOL_SIZE
#else
#define LXP_EXT_STORAGE_ALIGN 32
#endif

/* Largest-alignment rows come first. The board linker places this NOLOAD
 * object in external RAM, so its capacity has no firmware-image cost. */
struct ove_lxp_zephyr_storage {
	uint8_t dyn_pools[LXP_NREG][LXP_DYN_POOL_SIZE];
	uint8_t prog_regions[LXP_NREG][LXP_PROG_REGION_SIZE];
	lxp_exec_capture_t exec_captures[LXP_NSLOT];
	struct lxp_ove_thread_snapshot thread_snapshot;
#if LXP_ENABLE_NETFS_EXEC
	uint8_t netfs_exec_stage[256u * 1024u];
#endif
};

static struct ove_lxp_zephyr_storage
	g_lxp_storage Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(OVE_PROG_RAM_NODE))
		__aligned(LXP_EXT_STORAGE_ALIGN);
_Static_assert(offsetof(struct ove_lxp_zephyr_storage, prog_regions) % LXP_PROG_REGION_SIZE == 0u,
	       "program rows must be aligned to their MPU region size");

#if defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN521)
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(lxp_rootfs_ram)) == OVE_LXP_ROOTFS_BASE,
	     "generated rootfs base must match devicetree");
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(lxp_rootfs_ram)) == OVE_LXP_ROOTFS_SIZE,
	     "generated rootfs size must match devicetree");
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(lxp_pool_ram)) == OVE_LXP_GUEST_POOL_BASE,
	     "generated guest-pool base must match devicetree");
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(lxp_pool_ram)) == OVE_LXP_GUEST_POOL_SIZE,
	     "generated guest-pool size must match devicetree");
BUILD_ASSERT(OVE_LXP_ROOTFS_END == OVE_LXP_GUEST_POOL_BASE,
	     "AN521 rootfs and guest-pool ranges must be adjacent");
BUILD_ASSERT(sizeof(g_lxp_storage) <= OVE_LXP_GUEST_POOL_SIZE,
	     "AN521 guest storage overflows its generated pool");
#endif

static int host_thread_list(struct lxp_thread_info *out, size_t max_count, size_t *actual_count,
			    lxp_zephyr_slot_lookup_t slot_lookup)
{
	return lxp_ove_thread_snapshot_read(&g_lxp_storage.thread_snapshot, out, max_count,
					    actual_count, slot_lookup);
}

#define LXP_SYSTEM_VERSION \
	"Zephyr " KERNEL_VERSION_STRING " ove-" OVE_BUILD_OVERTOS_REV " lxp-" OVE_BUILD_LXP_REV
_Static_assert(sizeof(LXP_SYSTEM_VERSION) <= 65u, "uname version exceeds Linux utsname field");

static int host_random_fill(void *buf, size_t len)
{
	if (!buf && len != 0u)
		return LXP_ERR_INVALID_PARAM;
	return sys_csrand_get(buf, len) == 0 ? LXP_OK : LXP_ERR_BUS_ERROR;
}

static int host_prepare(void)
{
	/* The caller of lxp_run is the privileged coordinator. Its priority is a
	 * seam invariant, not an application concern: it must preempt a guest that
	 * has just parked in the SVC return trampoline. The former inline design
	 * inherited this value from Zephyr main; an app-owned oveRTOS thread must
	 * acquire it explicitly here. */
	k_thread_priority_set(k_current_get(), OVE_ZEPHYR_PRIO_LXP_COORDINATOR);
	return 0;
}

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
static struct lxp_cortex_m_cache_geometry g_cache_geometry;

static int host_validate_static_mpu(void)
{
	struct lxp_cortex_m_mpu_snapshot snapshot;
	if (lxp_cortex_m_mpu_snapshot_read(&snapshot) != 0 || snapshot.count != 8u ||
	    (snapshot.ctrl & (LXP_CORTEX_M_MPU_CTRL_ENABLE | LXP_CORTEX_M_MPU_CTRL_PRIVDEFENA)) !=
		    (LXP_CORTEX_M_MPU_CTRL_ENABLE | LXP_CORTEX_M_MPU_CTRL_PRIVDEFENA))
		return 0;

	const struct lxp_cortex_m_mpu_region *sdram = NULL;
	for (unsigned i = 0; i < snapshot.count; i++)
		if (lxp_cortex_m_mpu_region_matches(&snapshot.regions[i], 0xc0000000u,
						    8u * 1024u * 1024u, 0u, 0x0bu, 1u, 1u)) {
			if (sdram)
				return 0;
			sdram = &snapshot.regions[i];
		}
	if (!sdram || !lxp_cortex_m_mpu_region_contains(sdram, (uintptr_t)&g_lxp_storage,
							sizeof(g_lxp_storage)))
		return 0;
#if defined(CONFIG_OVE_FB)
	uintptr_t framebuffer = (uintptr_t)ove_hal_fb_buffer();
	uintptr_t storage = (uintptr_t)&g_lxp_storage;
	size_t framebuffer_size = 480u * 272u * 2u;
	if (framebuffer == 0u ||
	    !lxp_cortex_m_mpu_region_contains(sdram, framebuffer, framebuffer_size) ||
	    !(storage + sizeof(g_lxp_storage) <= framebuffer ||
	      framebuffer + framebuffer_size <= storage))
		return 0;
#endif
	return 1;
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
#define HOST_MEMORY_VALIDATOR ove_lxp_validate_uncached_memory_contract
#endif

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
#include "ove/lxp_metrics.h"

uint32_t ove_lxp_metrics_counter_hz(void)
{
	return sys_clock_hw_cycles_per_sec();
}
#endif

const lxp_zephyr_port_config_t g_lxp_zephyr_port_config = {
	.abi_version = LXP_ZEPHYR_PORT_CONFIG_ABI_VERSION,
	.struct_size = sizeof(lxp_zephyr_port_config_t),
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
#if defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN521)
	.rootfs_base = OVE_LXP_ROOTFS_BASE,
	.rootfs_size = OVE_LXP_ROOTFS_SIZE,
	.rootfs_partition_enabled = 1u,
	.guest_memory_texscb = 0x08u,
	.cpu_memory_contract = OVE_LXP_MEMORY_CONTRACT_UNCACHED_INITIALIZER,
#else
	.guest_memory_texscb = 0x0bu,
	.cpu_memory_contract = OVE_LXP_MEMORY_CONTRACT_STM32F746_INITIALIZER,
	.cache_geometry = &g_cache_geometry,
#endif
	.guest_priority = OVE_ZEPHYR_PRIO_LXP_GUEST,
	.quantum_priority = OVE_ZEPHYR_PRIO_NET_TC,
	.guest_quantum_ms = CONFIG_OVE_LINUX_GUEST_QUANTUM_MS,
	.host_prepare = host_prepare,
	.time_us = ove_time_get_us,
	.time_ns = ove_time_get_ns,
	.thread_list = host_thread_list,
	.mem_stats = lxp_ove_mem_stats_read,
	.system_version = LXP_SYSTEM_VERSION,
	.random_fill = host_random_fill,
	.validate_memory_contract = HOST_MEMORY_VALIDATOR,
};
