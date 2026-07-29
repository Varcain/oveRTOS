/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Cortex-M cache geometry and bounded executable publication.
 *
 * This header deliberately uses only architected System Control Space
 * registers.  RTOS range-cache APIs are unsuitable for the LXP publication
 * boundary because some implementations turn a sufficiently large range into
 * a whole-cache operation.  Cleaning and invalidating one line at a time keeps
 * latency proportional to the copied text and leaves the walk interruptible.
 */

#ifndef OVE_CORTEX_M_CACHE_H
#define OVE_CORTEX_M_CACHE_H

#include <stddef.h>
#include <stdint.h>

struct ove_cortex_m_cache_geometry {
	uint32_t l1_type;
	size_t d_line_size;
	size_t i_line_size;
	size_t d_size;
	size_t i_size;
};

struct ove_cortex_m_cache_shape {
	size_t line_size;
	size_t ways;
	size_t sets;
	size_t size;
};

struct ove_cortex_m_cache_line_span {
	uintptr_t first;
	size_t count;
};

static inline int ove_cortex_m_cache_shape_decode(uint32_t ccsidr,
						  struct ove_cortex_m_cache_shape *shape)
{
	if (!shape)
		return -1;

	size_t line_size = (size_t)1u << ((ccsidr & 0x7u) + 4u);
	size_t ways = (size_t)((ccsidr >> 3) & 0x3ffu) + 1u;
	size_t sets = (size_t)((ccsidr >> 13) & 0x7fffu) + 1u;
	if (ways > SIZE_MAX / line_size || sets > SIZE_MAX / (line_size * ways))
		return -1;

	shape->line_size = line_size;
	shape->ways = ways;
	shape->sets = sets;
	shape->size = line_size * ways * sets;
	return 0;
}

static inline int ove_cortex_m_cache_line_span(uintptr_t base, size_t len, size_t line_size,
					       struct ove_cortex_m_cache_line_span *span)
{
	if (!span || len == 0u || line_size < 16u || (line_size & (line_size - 1u)) != 0u)
		return -1;
	if (len - 1u > UINTPTR_MAX - base)
		return -1;

	uintptr_t last = base + len - 1u;
	uintptr_t first = base & ~((uintptr_t)line_size - 1u);
	uintptr_t distance = last - first;
	if (distance / line_size >= SIZE_MAX)
		return -1;

	span->first = first;
	span->count = (size_t)(distance / line_size) + 1u;
	return 0;
}

#if defined(__arm__) || defined(__thumb__)

#define OVE_SCB_CCR (*(volatile uint32_t *)0xe000ed14u)
#define OVE_SCB_CLIDR (*(volatile uint32_t *)0xe000ed78u)
#define OVE_SCB_CCSIDR (*(volatile uint32_t *)0xe000ed80u)
#define OVE_SCB_CSSELR (*(volatile uint32_t *)0xe000ed84u)
#define OVE_SCB_ICIMVAU (*(volatile uint32_t *)0xe000ef58u)
#define OVE_SCB_DCCMVAU (*(volatile uint32_t *)0xe000ef64u)

#define OVE_SCB_CCR_DC (1u << 16)
#define OVE_SCB_CCR_IC (1u << 17)

static inline void ove_cortex_m_dsb(void)
{
	__asm volatile("dsb 0xf" ::: "memory");
}

static inline void ove_cortex_m_isb(void)
{
	__asm volatile("isb 0xf" ::: "memory");
}

static inline uint32_t ove_cortex_m_ccsidr_read(uint32_t csselr)
{
	OVE_SCB_CSSELR = csselr;
	ove_cortex_m_dsb();
	ove_cortex_m_isb();
	return OVE_SCB_CCSIDR;
}

/*
 * Read the live L1 geometry.  CSSELR is banked state shared by all contexts, so
 * protect only the selector/read/restore sequence.  Publication itself remains
 * fully preemptible.
 */
static inline int ove_cortex_m_cache_geometry_read(struct ove_cortex_m_cache_geometry *geometry)
{
	if (!geometry)
		return -1;

	uint32_t primask;
	__asm volatile("mrs %0, primask" : "=r"(primask));
	__asm volatile("cpsid i" ::: "memory");

	uint32_t saved_csselr = OVE_SCB_CSSELR;
	uint32_t clidr = OVE_SCB_CLIDR;
	uint32_t l1_type = clidr & 0x7u;
	uint32_t loc = (clidr >> 24) & 0x7u;
	uint32_t d_ccsidr = 0u;
	uint32_t i_ccsidr = 0u;

	if (l1_type == 2u || l1_type == 3u || l1_type == 4u)
		d_ccsidr = ove_cortex_m_ccsidr_read(0u);
	if (l1_type == 1u || l1_type == 3u)
		i_ccsidr = ove_cortex_m_ccsidr_read(1u);
	else if (l1_type == 4u)
		i_ccsidr = d_ccsidr;

	OVE_SCB_CSSELR = saved_csselr;
	ove_cortex_m_dsb();
	ove_cortex_m_isb();
	__asm volatile("msr primask, %0" : : "r"(primask) : "memory");

	if (l1_type > 4u || (loc == 0u) != (l1_type == 0u) || loc > 1u)
		return -1;

	struct ove_cortex_m_cache_shape d_shape = {0};
	struct ove_cortex_m_cache_shape i_shape = {0};
	if ((l1_type == 2u || l1_type == 3u || l1_type == 4u) &&
	    ove_cortex_m_cache_shape_decode(d_ccsidr, &d_shape) != 0)
		return -1;
	if ((l1_type == 1u || l1_type == 3u || l1_type == 4u) &&
	    ove_cortex_m_cache_shape_decode(i_ccsidr, &i_shape) != 0)
		return -1;

	geometry->l1_type = l1_type;
	geometry->d_line_size = d_shape.line_size;
	geometry->i_line_size = i_shape.line_size;
	geometry->d_size = d_shape.size;
	geometry->i_size = i_shape.size;
	return 0;
}

static inline int
ove_cortex_m_publish_executable(const struct ove_cortex_m_cache_geometry *geometry, uintptr_t base,
				size_t len)
{
	if (!geometry)
		return -1;

	uint32_t ccr = OVE_SCB_CCR;
	struct ove_cortex_m_cache_line_span d_span = {0};
	struct ove_cortex_m_cache_line_span i_span = {0};
	if ((ccr & OVE_SCB_CCR_DC) != 0u &&
	    ove_cortex_m_cache_line_span(base, len, geometry->d_line_size, &d_span) != 0)
		return -1;
	if ((ccr & OVE_SCB_CCR_IC) != 0u &&
	    ove_cortex_m_cache_line_span(base, len, geometry->i_line_size, &i_span) != 0)
		return -1;

	/* Order loader stores before clean-to-PoU, then publish them to the
	 * instruction side before invalidating matching I-cache lines. */
	ove_cortex_m_dsb();
	for (size_t i = 0; i < d_span.count; i++)
		OVE_SCB_DCCMVAU = (uint32_t)(d_span.first + i * geometry->d_line_size);
	ove_cortex_m_dsb();
	for (size_t i = 0; i < i_span.count; i++)
		OVE_SCB_ICIMVAU = (uint32_t)(i_span.first + i * geometry->i_line_size);
	ove_cortex_m_dsb();
	ove_cortex_m_isb();
	return 0;
}

#endif /* __arm__ || __thumb__ */

#endif /* OVE_CORTEX_M_CACHE_H */
