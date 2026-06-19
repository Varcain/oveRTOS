/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LOADER_H
#define OVE_LOADER_H

/**
 * @file loader.h
 * @defgroup ove_loader Module Loader
 * @ingroup ove_mem
 * @brief Runtime loader for relocatable native code modules.
 *
 * Loads a relocatable ELF object (@c ET_REL — a freestanding @c .o, the same
 * shape a kernel module or a Zephyr LLEXT extension has) from a memory image
 * into a caller-supplied region, resolves its undefined symbols against a
 * caller-provided import table, applies relocations, and exposes the loaded
 * symbols by name. This is the reusable substrate beneath dynamically-loaded
 * plugins / OTA code modules — and, later, beneath the Linux personality's
 * program loader.
 *
 * The loader is backend-independent: it neither allocates nor changes memory
 * protection. The caller supplies the destination @p region and guarantees it
 * is executable before any loaded function is called (e.g. an MPU-RX region
 * on target, an @c mprotect'd mapping on a host). On architectures with a
 * split I/D cache the caller is also responsible for the post-load i-cache
 * sync.
 *
 * Phase-0 scope: 64-bit ELF (@c ELFCLASS64) for the host architecture. Other
 * classes/machines return @c OVE_ERR_NOT_SUPPORTED until their relocation
 * backends land.
 *
 * @note Requires @c CONFIG_OVE_LOADER.
 * @{
 */

#include <stddef.h>
#include <stdint.h>

#include "ove/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum ELF sections a single module may contain. */
#define OVE_LOADER_MAX_SECTIONS 32

/** An imported (or exported) symbol: a name bound to an address. */
typedef struct ove_loader_sym {
	const char *name; /**< Symbol name. */
	void *addr;	  /**< Resolved address. */
} ove_loader_sym_t;

/**
 * @brief A loaded module.
 *
 * Allocate one per load (it may live in static storage). Fields are exposed
 * so the control block can be sized at build time, but they are an
 * implementation detail — use @c ove_loader_sym() to query the module.
 *
 * The original @p image must remain valid for the lifetime of the module:
 * symbol lookup reads the module's symbol/string tables in place rather than
 * copying them.
 */
typedef struct ove_module {
	const uint8_t *image;			 /**< Original ELF image (caller-owned). */
	size_t image_size;			 /**< Size of @c image. */
	uint8_t *region;			 /**< Destination region (caller-owned). */
	size_t region_size;			 /**< Size of @c region. */
	size_t region_used;			 /**< Bytes of @c region consumed by the load. */
	uint16_t n_sections;			 /**< Section count. */
	void *sec_addr[OVE_LOADER_MAX_SECTIONS]; /**< Runtime base per section. */
	const void *symtab;			 /**< Symbol table (within @c image). */
	uint32_t sym_count;			 /**< Number of symbols. */
	const char *strtab;			 /**< String table (within @c image). */
	uint32_t strtab_size;			 /**< Size of the string table, bytes. */
} ove_module_t;

/**
 * @brief Load a relocatable ELF object into @p region.
 *
 * @param[out] mod          Module control block to fill.
 * @param[in]  image        ELF @c ET_REL image.
 * @param[in]  image_size   Size of @p image in bytes.
 * @param[in]  region       Destination for the module's allocatable sections.
 *                          Must be executable before any loaded code runs.
 * @param[in]  region_size  Size of @p region in bytes.
 * @param[in]  imports      Symbols the module may reference (undefined symbols
 *                          are resolved against this table). May be NULL when
 *                          @p n_imports is 0.
 * @param[in]  n_imports    Number of entries in @p imports.
 * @return OVE_OK on success;
 *         OVE_ERR_INVALID_PARAM on bad arguments or a malformed image;
 *         OVE_ERR_NOT_SUPPORTED for an unsupported class/machine/relocation;
 *         OVE_ERR_NO_MEMORY if @p region is too small or the module has too
 *         many sections;
 *         OVE_ERR_NOT_FOUND if an undefined symbol is not in @p imports.
 * @note Requires @c CONFIG_OVE_LOADER.
 */
int ove_loader_load(ove_module_t *mod, const void *image, size_t image_size, void *region,
		    size_t region_size, const ove_loader_sym_t *imports, size_t n_imports);

/**
 * @brief Resolve an exported (defined, global/weak) symbol by name.
 * @return The symbol's runtime address, or NULL if not found.
 */
void *ove_loader_sym(const ove_module_t *mod, const char *name);

/** @brief Bytes of the destination region consumed by the loaded module. */
size_t ove_loader_image_size(const ove_module_t *mod);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_LOADER_H */
