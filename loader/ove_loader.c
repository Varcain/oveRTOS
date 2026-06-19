/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LOADER)

#include "ove/loader.h"

#include <string.h>

/*
 * Relocatable-ELF (ET_REL) loader.
 *
 * Lays each SHF_ALLOC section into the caller's region, resolves undefined
 * symbols against the import table, and applies relocations. All ELF fields
 * are read through memcpy into aligned locals so the image buffer need not be
 * aligned (keeps the loader UBSan-clean on arbitrary inputs).
 *
 * Phase-0: 64-bit ELF for the host architecture (x86-64). Other classes and
 * machines return OVE_ERR_NOT_SUPPORTED until their backends land.
 */

/* ── minimal ELF64 ──────────────────────────────────────────────────────── */

#define EI_CLASS 4
#define EI_DATA 5
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_REL 1
#define EM_X86_64 62

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHF_ALLOC 0x2

#define SHN_UNDEF 0
#define SHN_ABS 0xfff1

#define STB_GLOBAL 1
#define STB_WEAK 2

/* x86-64 relocation types. */
#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_PLT32 4
#define R_X86_64_32 10
#define R_X86_64_32S 11

typedef struct {
	unsigned char e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	uint32_t sh_name;
	uint32_t sh_type;
	uint64_t sh_flags;
	uint64_t sh_addr;
	uint64_t sh_offset;
	uint64_t sh_size;
	uint32_t sh_link;
	uint32_t sh_info;
	uint64_t sh_addralign;
	uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct {
	uint32_t st_name;
	uint8_t st_info;
	uint8_t st_other;
	uint16_t st_shndx;
	uint64_t st_value;
	uint64_t st_size;
} Elf64_Sym;

typedef struct {
	uint64_t r_offset;
	uint64_t r_info;
	int64_t r_addend;
} Elf64_Rela;

/* ── helpers ────────────────────────────────────────────────────────────── */

static void rd_shdr(const Elf64_Ehdr *eh, const uint8_t *img, unsigned i, Elf64_Shdr *out)
{
	memcpy(out, img + eh->e_shoff + (size_t)i * eh->e_shentsize, sizeof(*out));
}

static const char *sym_name(const ove_module_t *mod, uint32_t st_name)
{
	if (st_name >= mod->strtab_size)
		return NULL;
	return mod->strtab + st_name;
}

static int resolve_sym(const ove_module_t *mod, const Elf64_Sym *sym,
		       const ove_loader_sym_t *imports, size_t n_imports, uintptr_t *out)
{
	if (sym->st_shndx == SHN_UNDEF) {
		const char *name = sym_name(mod, sym->st_name);
		if (!name)
			return OVE_ERR_INVALID_PARAM;
		for (size_t i = 0; i < n_imports; i++) {
			if (imports[i].name && strcmp(imports[i].name, name) == 0) {
				*out = (uintptr_t)imports[i].addr;
				return OVE_OK;
			}
		}
		return OVE_ERR_NOT_FOUND;
	}
	if (sym->st_shndx == SHN_ABS) {
		*out = (uintptr_t)sym->st_value;
		return OVE_OK;
	}
	if (sym->st_shndx < mod->n_sections && mod->sec_addr[sym->st_shndx]) {
		*out = (uintptr_t)mod->sec_addr[sym->st_shndx] + sym->st_value;
		return OVE_OK;
	}
	return OVE_ERR_NOT_SUPPORTED; /* common / non-loaded section */
}

static int apply_rela(const ove_module_t *mod, unsigned tgt, const Elf64_Rela *rela,
		      const ove_loader_sym_t *imports, size_t n_imports)
{
	uint32_t symidx = (uint32_t)(rela->r_info >> 32);
	uint32_t type = (uint32_t)(rela->r_info & 0xffffffffu);
	if (symidx >= mod->sym_count)
		return OVE_ERR_INVALID_PARAM;

	Elf64_Sym sym;
	memcpy(&sym, (const uint8_t *)mod->symtab + (size_t)symidx * sizeof(Elf64_Sym),
	       sizeof(sym));

	uintptr_t S;
	int rc = resolve_sym(mod, &sym, imports, n_imports, &S);
	if (rc != OVE_OK)
		return rc;

	uint8_t *loc = (uint8_t *)mod->sec_addr[tgt] + rela->r_offset;
	int64_t A = rela->r_addend;
	int64_t P = (int64_t)(uintptr_t)loc;

	switch (type) {
	case R_X86_64_NONE:
		break;
	case R_X86_64_64: {
		uint64_t v = (uint64_t)((int64_t)S + A);
		memcpy(loc, &v, 8);
		break;
	}
	case R_X86_64_PC32:
	case R_X86_64_PLT32: {
		uint32_t v = (uint32_t)(int32_t)((int64_t)S + A - P);
		memcpy(loc, &v, 4);
		break;
	}
	case R_X86_64_32: {
		uint32_t v = (uint32_t)((int64_t)S + A);
		memcpy(loc, &v, 4);
		break;
	}
	case R_X86_64_32S: {
		int32_t v = (int32_t)((int64_t)S + A);
		memcpy(loc, &v, 4);
		break;
	}
	default:
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

/* ── public API ─────────────────────────────────────────────────────────── */

int ove_loader_load(ove_module_t *mod, const void *image, size_t image_size, void *region,
		    size_t region_size, const ove_loader_sym_t *imports, size_t n_imports)
{
	if (!mod || !image || !region || image_size < sizeof(Elf64_Ehdr))
		return OVE_ERR_INVALID_PARAM;
	if (n_imports && !imports)
		return OVE_ERR_INVALID_PARAM;

	const uint8_t *img = (const uint8_t *)image;
	Elf64_Ehdr eh;
	memcpy(&eh, img, sizeof(eh));

	if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' ||
	    eh.e_ident[3] != 'F')
		return OVE_ERR_INVALID_PARAM;
	if (eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB)
		return OVE_ERR_NOT_SUPPORTED;
	if (eh.e_type != ET_REL || eh.e_machine != EM_X86_64)
		return OVE_ERR_NOT_SUPPORTED;
	if (eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shnum == 0)
		return OVE_ERR_INVALID_PARAM;
	if (eh.e_shnum > OVE_LOADER_MAX_SECTIONS)
		return OVE_ERR_NO_MEMORY;
	if (eh.e_shoff + (size_t)eh.e_shnum * eh.e_shentsize > image_size)
		return OVE_ERR_INVALID_PARAM;

	memset(mod, 0, sizeof(*mod));
	mod->image = img;
	mod->image_size = image_size;
	mod->region = (uint8_t *)region;
	mod->region_size = region_size;
	mod->n_sections = eh.e_shnum;

	/* Pass 1 — place allocatable sections; capture symbol/string tables. */
	size_t off = 0;
	for (unsigned i = 0; i < eh.e_shnum; i++) {
		Elf64_Shdr sh;
		rd_shdr(&eh, img, i, &sh);

		if ((sh.sh_type == SHT_PROGBITS || sh.sh_type == SHT_NOBITS) &&
		    (sh.sh_flags & SHF_ALLOC) && sh.sh_size > 0) {
			size_t align = sh.sh_addralign ? (size_t)sh.sh_addralign : 1;
			off = (off + align - 1) & ~(align - 1);
			if (off + sh.sh_size > region_size)
				return OVE_ERR_NO_MEMORY;
			uint8_t *dst = mod->region + off;
			if (sh.sh_type == SHT_NOBITS) {
				memset(dst, 0, sh.sh_size);
			} else {
				if (sh.sh_offset + sh.sh_size > image_size)
					return OVE_ERR_INVALID_PARAM;
				memcpy(dst, img + sh.sh_offset, sh.sh_size);
			}
			mod->sec_addr[i] = dst;
			off += sh.sh_size;
		} else if (sh.sh_type == SHT_SYMTAB) {
			if (sh.sh_entsize != sizeof(Elf64_Sym))
				return OVE_ERR_NOT_SUPPORTED;
			if (sh.sh_offset + sh.sh_size > image_size)
				return OVE_ERR_INVALID_PARAM;
			mod->symtab = img + sh.sh_offset;
			mod->sym_count = (uint32_t)(sh.sh_size / sizeof(Elf64_Sym));
			if (sh.sh_link < eh.e_shnum) {
				Elf64_Shdr st;
				rd_shdr(&eh, img, sh.sh_link, &st);
				if (st.sh_offset + st.sh_size > image_size)
					return OVE_ERR_INVALID_PARAM;
				mod->strtab = (const char *)(img + st.sh_offset);
				mod->strtab_size = (uint32_t)st.sh_size;
			}
		}
	}
	mod->region_used = off;
	if (!mod->symtab || !mod->strtab)
		return OVE_ERR_INVALID_PARAM;

	/* Pass 2 — apply relocations. */
	for (unsigned i = 0; i < eh.e_shnum; i++) {
		Elf64_Shdr sh;
		rd_shdr(&eh, img, i, &sh);
		if (sh.sh_type != SHT_RELA)
			continue;
		unsigned tgt = sh.sh_info;
		if (tgt >= eh.e_shnum || !mod->sec_addr[tgt])
			continue; /* relocations into a non-loaded section */
		if (sh.sh_entsize != sizeof(Elf64_Rela))
			return OVE_ERR_NOT_SUPPORTED;
		if (sh.sh_offset + sh.sh_size > image_size)
			return OVE_ERR_INVALID_PARAM;

		size_t n = sh.sh_size / sizeof(Elf64_Rela);
		for (size_t r = 0; r < n; r++) {
			Elf64_Rela rela;
			memcpy(&rela, img + sh.sh_offset + r * sizeof(Elf64_Rela), sizeof(rela));
			int rc = apply_rela(mod, tgt, &rela, imports, n_imports);
			if (rc != OVE_OK)
				return rc;
		}
	}

	return OVE_OK;
}

void *ove_loader_sym(const ove_module_t *mod, const char *name)
{
	if (!mod || !name || !mod->symtab || !mod->strtab)
		return NULL;

	for (uint32_t i = 0; i < mod->sym_count; i++) {
		Elf64_Sym sym;
		memcpy(&sym, (const uint8_t *)mod->symtab + (size_t)i * sizeof(Elf64_Sym),
		       sizeof(sym));
		if (sym.st_shndx == SHN_UNDEF || sym.st_shndx >= mod->n_sections)
			continue;
		if (!mod->sec_addr[sym.st_shndx])
			continue;
		unsigned bind = (unsigned)(sym.st_info >> 4);
		if (bind != STB_GLOBAL && bind != STB_WEAK)
			continue;
		const char *sn = sym_name(mod, sym.st_name);
		if (sn && strcmp(sn, name) == 0)
			return (uint8_t *)mod->sec_addr[sym.st_shndx] + sym.st_value;
	}
	return NULL;
}

size_t ove_loader_image_size(const ove_module_t *mod)
{
	return mod ? mod->region_used : 0;
}

#endif /* CONFIG_OVE_LOADER */
