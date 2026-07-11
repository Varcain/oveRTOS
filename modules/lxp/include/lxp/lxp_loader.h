/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef LXP_LOADER_H
#define LXP_LOADER_H

/**
 * @file loader.h
 * @defgroup lxp_loader Module Loader
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
 * Supported: ELFCLASS64 / x86-64 (host development) and ELFCLASS32 / EM_ARM
 * (Cortex-M target). ARM support currently covers data relocations
 * (R_ARM_ABS32 / REL32 / TARGET1 / PREL31); Thumb-2 instruction relocations
 * are not yet implemented. Other classes/machines return
 * @c LXP_ERR_NOT_SUPPORTED.
 *
 * @note Requires @c CONFIG_LXP_LOADER.
 * @{
 */

#include <stddef.h>
#include <stdint.h>

#include "lxp/lxp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum ELF sections a single module may contain. */
#define LXP_LOADER_MAX_SECTIONS 32

/** An imported (or exported) symbol: a name bound to an address. */
typedef struct lxp_loader_sym {
	const char *name; /**< Symbol name. */
	void *addr;	  /**< Resolved address. */
} lxp_loader_sym_t;

/**
 * @brief A loaded module.
 *
 * Allocate one per load (it may live in static storage). Fields are exposed
 * so the control block can be sized at build time, but they are an
 * implementation detail — use @c lxp_loader_sym() to query the module.
 *
 * The original @p image must remain valid for the lifetime of the module:
 * symbol lookup reads the module's symbol/string tables in place rather than
 * copying them.
 */
typedef struct lxp_module {
	const uint8_t *image;			 /**< Original ELF image (caller-owned). */
	size_t image_size;			 /**< Size of @c image. */
	uint8_t *region;			 /**< Destination region (caller-owned). */
	size_t region_size;			 /**< Size of @c region. */
	size_t region_used;			 /**< Bytes of @c region consumed by the load. */
	uint16_t n_sections;			 /**< Section count. */
	void *sec_addr[LXP_LOADER_MAX_SECTIONS]; /**< Runtime base per section. */
	const void *symtab;			 /**< Symbol table (within @c image). */
	uint32_t sym_count;			 /**< Number of symbols. */
	const char *strtab;			 /**< String table (within @c image). */
	uint32_t strtab_size;			 /**< Size of the string table, bytes. */
	uint8_t is_elf64;			 /**< Non-zero for ELFCLASS64, else ELFCLASS32. */
} lxp_module_t;

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
 * @return LXP_OK on success;
 *         LXP_ERR_INVALID_PARAM on bad arguments or a malformed image;
 *         LXP_ERR_NOT_SUPPORTED for an unsupported class/machine/relocation;
 *         LXP_ERR_NO_MEMORY if @p region is too small or the module has too
 *         many sections;
 *         LXP_ERR_NOT_FOUND if an undefined symbol is not in @p imports.
 * @note Requires @c CONFIG_LXP_LOADER.
 */
int lxp_loader_load(lxp_module_t *mod, const void *image, size_t image_size, void *region,
		    size_t region_size, const lxp_loader_sym_t *imports, size_t n_imports);

/**
 * @brief Resolve an exported (defined, global/weak) symbol by name.
 * @return The symbol's runtime address, or NULL if not found.
 */
void *lxp_loader_sym(const lxp_module_t *mod, const char *name);

/** @brief Bytes of the destination region consumed by the loaded module. */
size_t lxp_loader_image_size(const lxp_module_t *mod);

/* ── FDPIC program loader ───────────────────────────────────────────────── */

/**
 * @brief A loaded FDPIC program — the loaded-program control block.
 *
 * Unlike @c lxp_module_t (a relocatable object queried by symbol), this is a
 * fully-linked program: an entry point plus laid-out text/data/bss segments,
 * not an import/export symbol surface. It is the substrate beneath the Linux
 * personality's program loader; a freestanding FDPIC program can also be loaded
 * and called directly (no syscall environment required).
 *
 * The @c lxp_flat_t / "flat" name is historical: this struct was once shared
 * with the now-removed bFLT loader, but only FDPIC ELF programs are loaded now.
 */
typedef struct lxp_flat {
	uint8_t *region;     /**< Destination region (caller-owned; must be RX). */
	size_t region_size;  /**< Size of @c region. */
	size_t region_used;  /**< Bytes consumed (text + data + bss). */
	uintptr_t entry;     /**< Runtime entry address; directly callable (carries
			      *   the ARM Thumb bit where applicable). */
	uintptr_t text_base; /**< Runtime base of the text segment (== @c region). */
	size_t text_size;    /**< Text segment size. */
	uintptr_t data_base; /**< Runtime base of the data segment. */
	size_t data_size;    /**< Initialised-data size. */
	size_t bss_size;     /**< Zero-initialised data size. */
	size_t stack_size;   /**< Stack size the program requests. */
	int is_fdpic;	     /**< Non-zero if loaded from an FDPIC ELF (PIC, self-relocating). */
	uintptr_t loadmap;   /**< FDPIC: the elf32_fdpic_loadmap to pass in r7 at entry (the
			      *   crt _start self-relocates from it). */
	uintptr_t phdr;	     /**< FDPIC: runtime address of the program headers (AT_PHDR). */
	int phnum;	     /**< FDPIC: number of program headers (AT_PHNUM). */
	int is_dynamic;	     /**< FDPIC: non-zero if the exec has DT_NEEDED (needs ld.so). The
			      *   personality must then ALSO load the interpreter + enter it. */
	uintptr_t got;	     /**< FDPIC: the GOT base (DT_PLTGOT-relative); for the exec it is
			      *   what ld.so installs as the program GOT. */
	uintptr_t dynamic;   /**< FDPIC: runtime address of PT_DYNAMIC (_DYNAMIC). For the
			      *   INTERPRETER this is r9 at entry — uClibc-ng's FDPIC
			      *   dl_boot_ldso_dyn_pointer, which DL_BOOT_COMPUTE_DYN uses as the
			      *   dpnt (NOT the GOT). 0 if the object has no PT_DYNAMIC. */
	uintptr_t interp_loadmap; /**< FDPIC dynamic: the interpreter (ld.so) loadmap → r8 at
				   *   entry; 0 for static. Filled by the launcher, not the
				   *   loader (which loads one object at a time). */
	int region_exec;     /**< A @c copy_text load put the program's own text INTO @c region
			      *   (a remote/RAM exec), so the engine must map the region EXECUTABLE
			      *   (RWX — W^X-relaxed for this process). 0 for the normal XIP-text load. */
} lxp_flat_t;

/**
 * @brief Load a static FDPIC (ARM @c EF_ARM_FDPIC) ELF executable into @p region.
 *
 * FDPIC is position-independent with independently-placed segments addressed through a
 * per-process GOT (the "FDPIC register" r9). This loads the @c PT_LOAD segments, applies
 * the @c DT_REL dynamic relocations (incl. @c R_ARM_RELATIVE / @c R_ARM_FUNCDESC_VALUE),
 * and reports @c prog->got — the GOT base the launcher must place in r9 before @c entry.
 * Scope: static FDPIC (no @c PT_INTERP / dynamic loader). This format lets
 * multiple processes later share one read-only text copy.
 *
 * @param[out] prog        Program control block to fill (@c is_fdpic set, @c got reported).
 * @param[in]  image       FDPIC ELF image (caller-owned; only read during the load).
 * @param[in]  image_size  Size of @p image in bytes.
 * @param[in]  region      Destination; must be executable before @c entry runs.
 * @param[in]  region_size Size of @p region in bytes.
 * @param[in]  is_interp   Non-zero when loading the interpreter (ld.so): the personality
 *                         loads its segments + loadmap but applies NO .rel.dyn (ld.so
 *                         self-relocates). A dynamic exec (DT_NEEDED) likewise skips relocs
 *                         (auto-detected, @c is_dynamic reported) for ld.so to apply.
 * @return LXP_OK on success; LXP_ERR_INVALID_PARAM on a malformed/non-FDPIC image;
 *         LXP_ERR_NOT_SUPPORTED for an unhandled reloc; LXP_ERR_NO_MEMORY if too small.
 * @note Requires @c CONFIG_LXP_LOADER.
 */
int lxp_loader_load_fdpic(lxp_flat_t *prog, const void *image, size_t image_size, void *region,
			  size_t region_size, int is_interp, int copy_text);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* LXP_LOADER_H */
