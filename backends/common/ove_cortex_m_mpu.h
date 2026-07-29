/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal PMSAv7 MPU descriptor decoding and live snapshot support. The pure
 * helpers are host-testable; the register reader is available only on ARM.
 */

#ifndef OVE_CORTEX_M_MPU_H
#define OVE_CORTEX_M_MPU_H

#include <stddef.h>
#include <stdint.h>

#define OVE_CORTEX_M_MPU_MAX_REGIONS 16u
#define OVE_CORTEX_M_MPU_CTRL_ENABLE (1u << 0)
#define OVE_CORTEX_M_MPU_CTRL_PRIVDEFENA (1u << 2)

struct ove_cortex_m_mpu_region {
	uint32_t rbar;
	uint32_t rasr;
	uint32_t base;
	uint64_t size;
	uint8_t subregion_disable;
	uint8_t texscb;
	uint8_t access;
	uint8_t execute_never;
	uint8_t enabled;
};

struct ove_cortex_m_mpu_snapshot {
	uint32_t ctrl;
	uint8_t count;
	struct ove_cortex_m_mpu_region regions[OVE_CORTEX_M_MPU_MAX_REGIONS];
};

static inline int ove_cortex_m_mpu_region_decode(uint32_t rbar, uint32_t rasr,
						 struct ove_cortex_m_mpu_region *region)
{
	if (!region)
		return -1;

	uint32_t size_field = (rasr >> 1) & 0x1fu;
	uint8_t enabled = (uint8_t)(rasr & 1u);
	if (enabled && size_field < 4u)
		return -1;

	uint64_t size = enabled ? (UINT64_C(1) << (size_field + 1u)) : 0u;
	uint32_t base = 0u;
	if (enabled && size < (UINT64_C(1) << 32))
		base = rbar & ~((uint32_t)size - 1u);

	*region = (struct ove_cortex_m_mpu_region){
		.rbar = rbar,
		.rasr = rasr,
		.base = base,
		.size = size,
		.subregion_disable = (uint8_t)(rasr >> 8),
		.texscb = (uint8_t)((rasr >> 16) & 0x3fu),
		.access = (uint8_t)((rasr >> 24) & 0x7u),
		.execute_never = (uint8_t)((rasr >> 28) & 1u),
		.enabled = enabled,
	};
	return 0;
}

static inline int ove_cortex_m_mpu_region_contains(
	const struct ove_cortex_m_mpu_region *region, uintptr_t base, size_t len)
{
	if (!region || !region->enabled || len == 0u ||
	    (uint64_t)base + len > UINT64_C(1) << 32)
		return 0;

	uint64_t first = base;
	uint64_t end = first + len;
	uint64_t region_first = region->base;
	uint64_t region_end = region_first + region->size;
	if (first < region_first || end > region_end)
		return 0;
	if (region->size < 256u || region->subregion_disable == 0u)
		return 1;

	uint64_t subregion_size = region->size / 8u;
	unsigned first_subregion = (unsigned)((first - region_first) / subregion_size);
	unsigned last_subregion = (unsigned)((end - 1u - region_first) / subregion_size);
	for (unsigned i = first_subregion; i <= last_subregion; i++)
		if ((region->subregion_disable & (1u << i)) != 0u)
			return 0;
	return 1;
}

static inline int ove_cortex_m_mpu_region_overlaps_enabled(
	const struct ove_cortex_m_mpu_region *region, uintptr_t base, size_t len)
{
	if (!region || !region->enabled || len == 0u ||
	    (uint64_t)base + len > UINT64_C(1) << 32)
		return 0;

	uint64_t first = base;
	uint64_t end = first + len;
	uint64_t region_first = region->base;
	uint64_t region_end = region_first + region->size;
	if (first >= region_end || end <= region_first)
		return 0;
	if (region->size < 256u || region->subregion_disable == 0u)
		return 1;

	uint64_t subregion_size = region->size / 8u;
	for (unsigned i = 0; i < 8u; i++) {
		if ((region->subregion_disable & (1u << i)) != 0u)
			continue;
		uint64_t subregion_first = region_first + i * subregion_size;
		uint64_t subregion_end = subregion_first + subregion_size;
		if (first < subregion_end && end > subregion_first)
			return 1;
	}
	return 0;
}

static inline int ove_cortex_m_mpu_region_matches(
	const struct ove_cortex_m_mpu_region *region, uintptr_t base, uint64_t size,
	uint8_t subregion_disable, uint8_t texscb, uint8_t access, uint8_t execute_never)
{
	return region && region->enabled && region->base == base && region->size == size &&
	       region->subregion_disable == subregion_disable && region->texscb == texscb &&
	       region->access == access && region->execute_never == execute_never;
}

#if defined(__arm__) || defined(__thumb__)

#define OVE_MPU_TYPE (*(volatile uint32_t *)0xe000ed90u)
#define OVE_MPU_CTRL (*(volatile uint32_t *)0xe000ed94u)
#define OVE_MPU_RNR (*(volatile uint32_t *)0xe000ed98u)
#define OVE_MPU_RBAR (*(volatile uint32_t *)0xe000ed9cu)
#define OVE_MPU_RASR (*(volatile uint32_t *)0xe000eda0u)

static inline int ove_cortex_m_mpu_snapshot_read(struct ove_cortex_m_mpu_snapshot *snapshot)
{
	if (!snapshot)
		return -1;

	uint32_t count = (OVE_MPU_TYPE >> 8) & 0xffu;
	if (count == 0u || count > OVE_CORTEX_M_MPU_MAX_REGIONS)
		return -1;

	uint32_t primask;
	__asm volatile("mrs %0, primask" : "=r"(primask));
	__asm volatile("cpsid i" ::: "memory");
	uint32_t saved_rnr = OVE_MPU_RNR;
	snapshot->ctrl = OVE_MPU_CTRL;
	snapshot->count = (uint8_t)count;
	for (uint32_t i = 0; i < count; i++) {
		OVE_MPU_RNR = i;
		__asm volatile("dsb 0xf\nisb 0xf" ::: "memory");
		if (ove_cortex_m_mpu_region_decode(OVE_MPU_RBAR, OVE_MPU_RASR,
						   &snapshot->regions[i]) != 0) {
			OVE_MPU_RNR = saved_rnr;
			__asm volatile("dsb 0xf\nisb 0xf" ::: "memory");
			__asm volatile("msr primask, %0" : : "r"(primask) : "memory");
			return -1;
		}
	}
	OVE_MPU_RNR = saved_rnr;
	__asm volatile("dsb 0xf\nisb 0xf" ::: "memory");
	__asm volatile("msr primask, %0" : : "r"(primask) : "memory");
	return 0;
}

#endif /* __arm__ || __thumb__ */

#endif /* OVE_CORTEX_M_MPU_H */
