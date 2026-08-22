/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS host policy and board services for LXP's NuttX Cortex-M port.
 * Native NuttX task, trap, scheduler-note, parking and MPU mechanics belong to
 * modules/lxp/ports/nuttx.
 */

#include <nuttx/arch.h>
#include <nuttx/version.h>

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "lxp/arch/cortex_m_memory.h"
#include "lxp/ports/nuttx.h"
#include "lxp_ove_thread_adapter.h"
#include "ove/build.h"
#include "ove/lxp_memory_layout.h"
#include "ove/time.h"
#include "ove_lxp_memory_contract.h"
#include "ove_nuttx_runtime.h"
#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
#include "ove/lxp_metrics.h"

uint32_t ove_lxp_metrics_counter_hz(void)
{
	return (uint32_t)up_perf_getfreq();
}
#endif

#define LXP_NUTTX_STACK_SIZE 1024u
#define LXP_ALIGN_UP(value, align) (((value) + (align) - 1u) & ~((align) - 1u))
#define PROG_REGIONS_BYTES ((size_t)LXP_NREG * LXP_PROG_REGION_SIZE)
#define DYN_POOLS_BYTES ((size_t)LXP_NREG * LXP_DYN_POOL_SIZE)

#if LXP_ENABLE_NETFS_EXEC
#define NUTTX_EXEC_STAGE_BYTES (256u * 1024u)
#endif

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)

/* The framebuffer occupies the bottom of SDRAM. The first 1 MiB MPU
 * subregion remains disabled; trusted NuttX metadata is placed in that cold
 * window and guest rows begin at the generated pool base above it. */
#define NUTTX_SDRAM_COLD_BASE 0xC0040000u
#define NUTTX_SDRAM_THREAD_SNAPSHOT_BASE \
	LXP_ALIGN_UP(NUTTX_SDRAM_COLD_BASE + sizeof(lxp_exec_capture_t) * LXP_NSLOT, 8u)
#define NUTTX_SDRAM_STACK_BASE \
	LXP_ALIGN_UP(NUTTX_SDRAM_THREAD_SNAPSHOT_BASE + sizeof(struct lxp_ove_thread_snapshot), 8u)
#if LXP_ENABLE_NETFS_EXEC
#define NUTTX_SDRAM_EXEC_STAGE_BASE 0xC00C0000u
#endif

static uint8_t *const g_dynamic_pools = (uint8_t *)OVE_LXP_GUEST_POOL_BASE;
static uint8_t *const g_program_regions = (uint8_t *)(OVE_LXP_GUEST_POOL_BASE + DYN_POOLS_BYTES);
static lxp_exec_capture_t *const g_exec_captures = (lxp_exec_capture_t *)NUTTX_SDRAM_COLD_BASE;
static struct lxp_ove_thread_snapshot *const g_thread_snapshot =
	(struct lxp_ove_thread_snapshot *)NUTTX_SDRAM_THREAD_SNAPSHOT_BASE;
static uint8_t *const g_slot_stacks = (uint8_t *)NUTTX_SDRAM_STACK_BASE;

_Static_assert(DYN_POOLS_BYTES + PROG_REGIONS_BYTES <= OVE_LXP_GUEST_POOL_SIZE,
	       "NuttX guest rows overflow external SDRAM");
#if LXP_ENABLE_NETFS_EXEC
_Static_assert(NUTTX_SDRAM_STACK_BASE + LXP_NUTTX_STACK_SIZE * LXP_NSLOT <=
		       NUTTX_SDRAM_EXEC_STAGE_BASE,
	       "trusted NuttX slot storage overlaps the remote-exec stage");
static uint8_t *const g_exec_stage = (uint8_t *)NUTTX_SDRAM_EXEC_STAGE_BASE;
_Static_assert(NUTTX_SDRAM_EXEC_STAGE_BASE + NUTTX_EXEC_STAGE_BYTES <= OVE_LXP_GUEST_POOL_BASE,
	       "NuttX remote-exec stage overlaps guest rows");
#else
_Static_assert(NUTTX_SDRAM_STACK_BASE + LXP_NUTTX_STACK_SIZE * LXP_NSLOT <= OVE_LXP_GUEST_POOL_BASE,
	       "trusted NuttX slot storage overlaps guest rows");
#endif
#if defined(LXP_WFS_POOL_BASE)
#define NUTTX_GUEST_STORAGE_END (OVE_LXP_GUEST_POOL_BASE + DYN_POOLS_BYTES + PROG_REGIONS_BYTES)
_Static_assert(NUTTX_GUEST_STORAGE_END <= (uintptr_t)LXP_WFS_POOL_BASE,
	       "NuttX guest rows overlap the fixed tmpfs pool");
_Static_assert((uintptr_t)LXP_WFS_POOL_BASE + (size_t)LXP_WFS_POOL <= OVE_LXP_GUEST_POOL_END,
	       "NuttX tmpfs pool exceeds external SDRAM");
#endif

static struct lxp_cortex_m_cache_geometry g_cache_geometry;

#elif defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)

_Static_assert(OVE_LXP_ROOTFS_END == OVE_LXP_GUEST_POOL_BASE,
	       "AN500 rootfs and guest-pool ranges must be adjacent");
_Static_assert(DYN_POOLS_BYTES + PROG_REGIONS_BYTES <= OVE_LXP_GUEST_POOL_SIZE,
	       "AN500 guest rows overflow mps.ram");
_Static_assert(DYN_POOLS_BYTES % LXP_PROG_REGION_SIZE == 0u,
	       "dynamic rows must align the following program rows");

static uint8_t *const g_dynamic_pools = (uint8_t *)OVE_LXP_GUEST_POOL_BASE;
static uint8_t *const g_program_regions = (uint8_t *)(OVE_LXP_GUEST_POOL_BASE + DYN_POOLS_BYTES);
static lxp_exec_capture_t g_exec_captures[LXP_NSLOT];
static struct lxp_ove_thread_snapshot g_thread_snapshot_storage;
static uint8_t g_slot_stacks[LXP_NSLOT][LXP_NUTTX_STACK_SIZE] __attribute__((aligned(8)));
static struct lxp_ove_thread_snapshot *const g_thread_snapshot = &g_thread_snapshot_storage;
#if LXP_ENABLE_NETFS_EXEC
static uint8_t *const g_exec_stage =
	(uint8_t *)(OVE_LXP_GUEST_POOL_BASE + DYN_POOLS_BYTES + PROG_REGIONS_BYTES);
_Static_assert(DYN_POOLS_BYTES + PROG_REGIONS_BYTES + NUTTX_EXEC_STAGE_BYTES <=
		       OVE_LXP_GUEST_POOL_SIZE,
	       "AN500 remote-exec stage exceeds the guest pool");
#endif

#else
#error "LXP's production NuttX port requires a board memory policy"
#endif

static int host_thread_list(struct lxp_thread_info *out, size_t max_count, size_t *actual_count,
			    lxp_nuttx_slot_lookup_t slot_lookup)
{
	return lxp_ove_thread_snapshot_read(g_thread_snapshot, out, max_count, actual_count,
					    slot_lookup);
}

#define LXP_SYSTEM_VERSION \
	"NuttX " CONFIG_VERSION_STRING " ove-" OVE_BUILD_OVERTOS_REV " lxp-" OVE_BUILD_LXP_REV
_Static_assert(sizeof(LXP_SYSTEM_VERSION) <= 65u, "uname version exceeds Linux utsname field");

static const char *host_system_version(void)
{
	return LXP_SYSTEM_VERSION;
}

static uint64_t host_runtime_us(int32_t pid)
{
	uint64_t cycles = 0;
	return ove_nuttx_runtime_get((pid_t)pid, &cycles, NULL) == 0
		       ? ove_nuttx_runtime_cycles_to_us(cycles)
		       : 0u;
}

static void host_runtime_reset(int32_t pid)
{
	ove_nuttx_runtime_reset((pid_t)pid);
}

static void host_runtime_start(int32_t pid)
{
	ove_nuttx_runtime_start((pid_t)pid);
}

static void host_runtime_stop(int32_t pid)
{
	ove_nuttx_runtime_stop((pid_t)pid);
}

static void host_runtime_switch(int32_t pid)
{
	ove_nuttx_runtime_switch((pid_t)pid);
}

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
static int host_validate_memory_contract(const lxp_cpu_memory_contract_t *declared,
					 const struct lxp_cortex_m_cache_geometry *geometry)
{
	return lxp_cortex_m_memory_contract_matches_cache(declared, geometry)
		       ? LXP_OK
		       : LXP_ERR_INVALID_PARAM;
}
#else
static int host_validate_memory_contract(const lxp_cpu_memory_contract_t *declared,
					 const struct lxp_cortex_m_cache_geometry *geometry)
{
	(void)declared;
	(void)geometry;
	return (LXP_CORTEX_M_SCB_CCR & LXP_CORTEX_M_SCB_CCR_DC) == 0u ? LXP_OK
								      : LXP_ERR_INVALID_PARAM;
}
#endif

const lxp_nuttx_port_config_t g_lxp_nuttx_port_config = {
	.abi_version = LXP_NUTTX_PORT_CONFIG_ABI_VERSION,
	.struct_size = sizeof(lxp_nuttx_port_config_t),
	.program_regions = g_program_regions,
	.program_region_stride = LXP_PROG_REGION_SIZE,
	.program_region_count = LXP_NREG,
	.dynamic_pools = g_dynamic_pools,
	.dynamic_pool_stride = LXP_DYN_POOL_SIZE,
	.dynamic_pool_count = LXP_NREG,
	.exec_captures = g_exec_captures,
	.exec_capture_count = LXP_NSLOT,
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	.slot_stacks = g_slot_stacks,
#else
	.slot_stacks = &g_slot_stacks[0][0],
#endif
	.slot_stack_stride = LXP_NUTTX_STACK_SIZE,
	.slot_stack_size = LXP_NUTTX_STACK_SIZE,
	.slot_stack_count = LXP_NSLOT,
#if LXP_ENABLE_NETFS_EXEC
	.exec_stage = g_exec_stage,
	.exec_stage_size = NUTTX_EXEC_STAGE_BYTES,
#endif
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	.code_region = {.base = 0x08000000u, .size = 1024u * 1024u, .texscb = 0x02u, .enabled = 1u},
	.pool_region = {.base = 0xc0000000u,
			.size = 8u * 1024u * 1024u,
			.texscb = 0x0bu,
			.subregion_disable = 1u,
			.enabled = 1u},
#if defined(CONFIG_OVE_LINUX_ROOTFS_QSPI)
	.rootfs_region = {.base = OVE_LXP_ROOTFS_BASE,
			  .size = OVE_LXP_ROOTFS_SIZE,
			  .texscb = 0x02u,
			  .enabled = 1u},
#endif
	.guest_memory_texscb = 0x0bu,
	.trusted_tcb_base = 0x20000000u,
	.trusted_tcb_end = 0x20080000u,
	.cpu_memory_contract = OVE_LXP_MEMORY_CONTRACT_STM32F746_INITIALIZER,
	.cache_geometry = &g_cache_geometry,
#else
	.code_region = {.base = 0x00000000u,
			.size = 2u * 1024u * 1024u,
			.texscb = 0x08u,
			.enabled = 1u},
	.pool_region = {.base = OVE_LXP_GUEST_POOL_BASE,
			.size = OVE_LXP_GUEST_POOL_SIZE,
			.texscb = 0x08u,
			.enabled = 1u},
	.rootfs_region = {.base = OVE_LXP_ROOTFS_BASE,
			  .size = OVE_LXP_ROOTFS_SIZE,
			  .texscb = 0x08u,
			  .enabled = 1u},
	.guest_memory_texscb = 0x08u,
	.trusted_tcb_base = 0x20000000u,
	.trusted_tcb_end = 0x20400000u,
	.cpu_memory_contract = OVE_LXP_MEMORY_CONTRACT_UNCACHED_INITIALIZER,
#endif
	.guest_priority = 60u,
	.time_us = ove_time_get_us,
	.time_ns = ove_time_get_ns,
	.host_thread_list = host_thread_list,
	.mem_stats = lxp_ove_mem_stats_read,
	.system_version = host_system_version,
	.validate_memory_contract = host_validate_memory_contract,
	.runtime_reset = host_runtime_reset,
	.runtime_start = host_runtime_start,
	.runtime_stop = host_runtime_stop,
	.runtime_switch = host_runtime_switch,
	.runtime_us = host_runtime_us,
};
