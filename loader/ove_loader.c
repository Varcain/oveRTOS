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
 * Two architectures are supported:
 *   - ELFCLASS64 / EM_X86_64 — RELA (explicit addend). Host development arch.
 *   - ELFCLASS32 / EM_ARM    — REL (implicit addend). Cortex-M target arch;
 *                              data relocations only for now (Thumb-2
 *                              instruction relocations are TODO, pending
 *                              QEMU-execution validation).
 */

/* ── ELF identification / common ────────────────────────────────────────── */

#define EI_CLASS 4
#define EI_DATA 5
#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_REL 1
#define EM_X86_64 62
#define EM_ARM 40

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_REL 9
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

/* ARM relocation types (data subset). */
#define R_ARM_NONE 0
#define R_ARM_ABS32 2
#define R_ARM_REL32 3
#define R_ARM_TARGET1 38
#define R_ARM_PREL31 42

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

typedef struct {
	unsigned char e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint32_t e_entry;
	uint32_t e_phoff;
	uint32_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
	uint32_t sh_name;
	uint32_t sh_type;
	uint32_t sh_flags;
	uint32_t sh_addr;
	uint32_t sh_offset;
	uint32_t sh_size;
	uint32_t sh_link;
	uint32_t sh_info;
	uint32_t sh_addralign;
	uint32_t sh_entsize;
} Elf32_Shdr;

typedef struct {
	uint32_t st_name;
	uint32_t st_value;
	uint32_t st_size;
	uint8_t st_info;
	uint8_t st_other;
	uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
	uint32_t r_offset;
	uint32_t r_info;
} Elf32_Rel;

/* ── shared helpers ─────────────────────────────────────────────────────── */

static const char *sym_name(const ove_module_t *mod, uint32_t st_name)
{
	if (st_name >= mod->strtab_size)
		return NULL;
	return mod->strtab + st_name;
}

/* Resolve a symbol given its decoded fields (class-independent). */
static int resolve(const ove_module_t *mod, uint16_t shndx, uint64_t value, uint32_t st_name,
		   const ove_loader_sym_t *imports, size_t n_imports, uintptr_t *out)
{
	if (shndx == SHN_UNDEF) {
		const char *name = sym_name(mod, st_name);
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
	if (shndx == SHN_ABS) {
		*out = (uintptr_t)value;
		return OVE_OK;
	}
	if (shndx < mod->n_sections && mod->sec_addr[shndx]) {
		*out = (uintptr_t)mod->sec_addr[shndx] + value;
		return OVE_OK;
	}
	return OVE_ERR_NOT_SUPPORTED; /* common / non-loaded section */
}

/* ── ELF64 / x86-64 (RELA) ──────────────────────────────────────────────── */

static void rd_shdr64(const Elf64_Ehdr *eh, const uint8_t *img, unsigned i, Elf64_Shdr *out)
{
	memcpy(out, img + eh->e_shoff + (size_t)i * eh->e_shentsize, sizeof(*out));
}

static int apply_rela64(const ove_module_t *mod, unsigned tgt, const Elf64_Rela *rela,
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
	int rc = resolve(mod, sym.st_shndx, sym.st_value, sym.st_name, imports, n_imports, &S);
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

static int load64(ove_module_t *mod, const uint8_t *img, size_t image_size, void *region,
		  size_t region_size, const ove_loader_sym_t *imports, size_t n_imports)
{
	Elf64_Ehdr eh;
	memcpy(&eh, img, sizeof(eh));

	if (eh.e_type != ET_REL || eh.e_machine != EM_X86_64)
		return OVE_ERR_NOT_SUPPORTED;
	if (eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shnum == 0)
		return OVE_ERR_INVALID_PARAM;
	if (eh.e_shnum > OVE_LOADER_MAX_SECTIONS)
		return OVE_ERR_NO_MEMORY;
	if (eh.e_shoff + (size_t)eh.e_shnum * eh.e_shentsize > image_size)
		return OVE_ERR_INVALID_PARAM;

	mod->is_elf64 = 1;
	mod->n_sections = eh.e_shnum;

	size_t off = 0;
	for (unsigned i = 0; i < eh.e_shnum; i++) {
		Elf64_Shdr sh;
		rd_shdr64(&eh, img, i, &sh);
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
				rd_shdr64(&eh, img, sh.sh_link, &st);
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

	for (unsigned i = 0; i < eh.e_shnum; i++) {
		Elf64_Shdr sh;
		rd_shdr64(&eh, img, i, &sh);
		if (sh.sh_type != SHT_RELA)
			continue;
		unsigned tgt = sh.sh_info;
		if (tgt >= eh.e_shnum || !mod->sec_addr[tgt])
			continue;
		if (sh.sh_entsize != sizeof(Elf64_Rela))
			return OVE_ERR_NOT_SUPPORTED;
		if (sh.sh_offset + sh.sh_size > image_size)
			return OVE_ERR_INVALID_PARAM;
		size_t n = sh.sh_size / sizeof(Elf64_Rela);
		for (size_t r = 0; r < n; r++) {
			Elf64_Rela rela;
			memcpy(&rela, img + sh.sh_offset + r * sizeof(Elf64_Rela), sizeof(rela));
			int rc = apply_rela64(mod, tgt, &rela, imports, n_imports);
			if (rc != OVE_OK)
				return rc;
		}
	}
	return OVE_OK;
}

/* ── ELF32 / ARM (REL) ──────────────────────────────────────────────────── */

static void rd_shdr32(const Elf32_Ehdr *eh, const uint8_t *img, unsigned i, Elf32_Shdr *out)
{
	memcpy(out, img + eh->e_shoff + (size_t)i * eh->e_shentsize, sizeof(*out));
}

static int apply_rel_arm(const ove_module_t *mod, unsigned tgt, const Elf32_Rel *rel,
			 const ove_loader_sym_t *imports, size_t n_imports)
{
	uint32_t symidx = rel->r_info >> 8;
	uint32_t type = rel->r_info & 0xff;
	if (symidx >= mod->sym_count)
		return OVE_ERR_INVALID_PARAM;

	Elf32_Sym sym;
	memcpy(&sym, (const uint8_t *)mod->symtab + (size_t)symidx * sizeof(Elf32_Sym),
	       sizeof(sym));

	uintptr_t S;
	int rc = resolve(mod, sym.st_shndx, sym.st_value, sym.st_name, imports, n_imports, &S);
	if (rc != OVE_OK)
		return rc;

	uint8_t *loc = (uint8_t *)mod->sec_addr[tgt] + rel->r_offset;
	uint32_t A; /* REL: implicit addend read from the place. */
	memcpy(&A, loc, 4);
	uint32_t P = (uint32_t)(uintptr_t)loc;

	switch (type) {
	case R_ARM_NONE:
		break;
	case R_ARM_ABS32:
	case R_ARM_TARGET1: {
		uint32_t v = (uint32_t)S + A;
		memcpy(loc, &v, 4);
		break;
	}
	case R_ARM_REL32: {
		uint32_t v = (uint32_t)S + A - P;
		memcpy(loc, &v, 4);
		break;
	}
	case R_ARM_PREL31: {
		uint32_t v = ((uint32_t)S + A - P) & 0x7fffffffu;
		v |= (A & 0x80000000u);
		memcpy(loc, &v, 4);
		break;
	}
	default:
		/* Thumb-2 instruction relocations (THM_CALL / MOVW / MOVT / ...)
		 * are not yet implemented — they need QEMU-execution validation
		 * rather than un-runnable bit-twiddling. */
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

static int load32_arm(ove_module_t *mod, const uint8_t *img, size_t image_size, void *region,
		      size_t region_size, const ove_loader_sym_t *imports, size_t n_imports)
{
	Elf32_Ehdr eh;
	memcpy(&eh, img, sizeof(eh));

	if (eh.e_type != ET_REL || eh.e_machine != EM_ARM)
		return OVE_ERR_NOT_SUPPORTED;
	if (eh.e_shentsize != sizeof(Elf32_Shdr) || eh.e_shnum == 0)
		return OVE_ERR_INVALID_PARAM;
	if (eh.e_shnum > OVE_LOADER_MAX_SECTIONS)
		return OVE_ERR_NO_MEMORY;
	if (eh.e_shoff + (size_t)eh.e_shnum * eh.e_shentsize > image_size)
		return OVE_ERR_INVALID_PARAM;

	mod->is_elf64 = 0;
	mod->n_sections = eh.e_shnum;

	size_t off = 0;
	for (unsigned i = 0; i < eh.e_shnum; i++) {
		Elf32_Shdr sh;
		rd_shdr32(&eh, img, i, &sh);
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
			if (sh.sh_entsize != sizeof(Elf32_Sym))
				return OVE_ERR_NOT_SUPPORTED;
			if (sh.sh_offset + sh.sh_size > image_size)
				return OVE_ERR_INVALID_PARAM;
			mod->symtab = img + sh.sh_offset;
			mod->sym_count = (uint32_t)(sh.sh_size / sizeof(Elf32_Sym));
			if (sh.sh_link < eh.e_shnum) {
				Elf32_Shdr st;
				rd_shdr32(&eh, img, sh.sh_link, &st);
				if (st.sh_offset + st.sh_size > image_size)
					return OVE_ERR_INVALID_PARAM;
				mod->strtab = (const char *)(img + st.sh_offset);
				mod->strtab_size = st.sh_size;
			}
		}
	}
	mod->region_used = off;
	if (!mod->symtab || !mod->strtab)
		return OVE_ERR_INVALID_PARAM;

	for (unsigned i = 0; i < eh.e_shnum; i++) {
		Elf32_Shdr sh;
		rd_shdr32(&eh, img, i, &sh);
		if (sh.sh_type != SHT_REL)
			continue;
		unsigned tgt = sh.sh_info;
		if (tgt >= eh.e_shnum || !mod->sec_addr[tgt])
			continue;
		if (sh.sh_entsize != sizeof(Elf32_Rel))
			return OVE_ERR_NOT_SUPPORTED;
		if (sh.sh_offset + sh.sh_size > image_size)
			return OVE_ERR_INVALID_PARAM;
		size_t n = sh.sh_size / sizeof(Elf32_Rel);
		for (size_t r = 0; r < n; r++) {
			Elf32_Rel rel;
			memcpy(&rel, img + sh.sh_offset + r * sizeof(Elf32_Rel), sizeof(rel));
			int rc = apply_rel_arm(mod, tgt, &rel, imports, n_imports);
			if (rc != OVE_OK)
				return rc;
		}
	}
	return OVE_OK;
}

/* ── public API ─────────────────────────────────────────────────────────── */

int ove_loader_load(ove_module_t *mod, const void *image, size_t image_size, void *region,
		    size_t region_size, const ove_loader_sym_t *imports, size_t n_imports)
{
	if (!mod || !image || !region || image_size < sizeof(Elf32_Ehdr))
		return OVE_ERR_INVALID_PARAM;
	if (n_imports && !imports)
		return OVE_ERR_INVALID_PARAM;

	const uint8_t *img = (const uint8_t *)image;
	if (img[0] != 0x7f || img[1] != 'E' || img[2] != 'L' || img[3] != 'F')
		return OVE_ERR_INVALID_PARAM;
	if (img[EI_DATA] != ELFDATA2LSB)
		return OVE_ERR_NOT_SUPPORTED;

	memset(mod, 0, sizeof(*mod));
	mod->image = img;
	mod->image_size = image_size;
	mod->region = (uint8_t *)region;
	mod->region_size = region_size;

	if (img[EI_CLASS] == ELFCLASS64) {
		if (image_size < sizeof(Elf64_Ehdr))
			return OVE_ERR_INVALID_PARAM;
		return load64(mod, img, image_size, region, region_size, imports, n_imports);
	}
	if (img[EI_CLASS] == ELFCLASS32)
		return load32_arm(mod, img, image_size, region, region_size, imports, n_imports);
	return OVE_ERR_NOT_SUPPORTED;
}

void *ove_loader_sym(const ove_module_t *mod, const char *name)
{
	if (!mod || !name || !mod->symtab || !mod->strtab)
		return NULL;

	size_t entsize = mod->is_elf64 ? sizeof(Elf64_Sym) : sizeof(Elf32_Sym);
	for (uint32_t i = 0; i < mod->sym_count; i++) {
		const uint8_t *p = (const uint8_t *)mod->symtab + (size_t)i * entsize;
		uint16_t shndx;
		uint64_t value;
		uint32_t st_name;
		uint8_t info;
		if (mod->is_elf64) {
			Elf64_Sym s;
			memcpy(&s, p, sizeof(s));
			shndx = s.st_shndx;
			value = s.st_value;
			st_name = s.st_name;
			info = s.st_info;
		} else {
			Elf32_Sym s;
			memcpy(&s, p, sizeof(s));
			shndx = s.st_shndx;
			value = s.st_value;
			st_name = s.st_name;
			info = s.st_info;
		}
		if (shndx == SHN_UNDEF || shndx >= mod->n_sections || !mod->sec_addr[shndx])
			continue;
		unsigned bind = (unsigned)(info >> 4);
		if (bind != STB_GLOBAL && bind != STB_WEAK)
			continue;
		const char *sn = sym_name(mod, st_name);
		if (sn && strcmp(sn, name) == 0)
			return (uint8_t *)mod->sec_addr[shndx] + value;
	}
	return NULL;
}

size_t ove_loader_image_size(const ove_module_t *mod)
{
	return mod ? mod->region_used : 0;
}

#endif /* CONFIG_OVE_LOADER */
