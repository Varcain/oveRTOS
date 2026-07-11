/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "lxp/lxp_config.h"

#if 1 /* the FDPIC loader is core to the lxp module */

#include "lxp/lxp_loader.h"

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
#define R_ARM_THM_CALL 10
#define R_ARM_THM_JUMP24 30
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

static const char *sym_name(const lxp_module_t *mod, uint32_t st_name)
{
	if (st_name >= mod->strtab_size)
		return NULL;
	return mod->strtab + st_name;
}

/* Resolve a symbol given its decoded fields (class-independent). */
static int resolve(const lxp_module_t *mod, uint16_t shndx, uint64_t value, uint32_t st_name,
		   const lxp_loader_sym_t *imports, size_t n_imports, uintptr_t *out)
{
	if (shndx == SHN_UNDEF) {
		const char *name = sym_name(mod, st_name);
		if (!name)
			return LXP_ERR_INVALID_PARAM;
		for (size_t i = 0; i < n_imports; i++) {
			if (imports[i].name && strcmp(imports[i].name, name) == 0) {
				*out = (uintptr_t)imports[i].addr;
				return LXP_OK;
			}
		}
		return LXP_ERR_NOT_FOUND;
	}
	if (shndx == SHN_ABS) {
		*out = (uintptr_t)value;
		return LXP_OK;
	}
	if (shndx < mod->n_sections && mod->sec_addr[shndx]) {
		*out = (uintptr_t)mod->sec_addr[shndx] + value;
		return LXP_OK;
	}
	return LXP_ERR_NOT_SUPPORTED; /* common / non-loaded section */
}

/* ── ELF64 / x86-64 (RELA) ──────────────────────────────────────────────── */

static void rd_shdr64(const Elf64_Ehdr *eh, const uint8_t *img, unsigned i, Elf64_Shdr *out)
{
	memcpy(out, img + eh->e_shoff + (size_t)i * eh->e_shentsize, sizeof(*out));
}

static int apply_rela64(const lxp_module_t *mod, unsigned tgt, const Elf64_Rela *rela,
			const lxp_loader_sym_t *imports, size_t n_imports)
{
	uint32_t symidx = (uint32_t)(rela->r_info >> 32);
	uint32_t type = (uint32_t)(rela->r_info & 0xffffffffu);
	if (symidx >= mod->sym_count)
		return LXP_ERR_INVALID_PARAM;

	Elf64_Sym sym;
	memcpy(&sym, (const uint8_t *)mod->symtab + (size_t)symidx * sizeof(Elf64_Sym),
	       sizeof(sym));

	uintptr_t S;
	int rc = resolve(mod, sym.st_shndx, sym.st_value, sym.st_name, imports, n_imports, &S);
	if (rc != LXP_OK)
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
		return LXP_ERR_NOT_SUPPORTED;
	}
	return LXP_OK;
}

/* Overflow-safe out-of-bounds test: is [off, off+size) NOT within [0, limit)? Guards a crafted ELF
 * whose off+size would wrap past the buffer. (The FDPIC loader below already casts to uint64_t before
 * adding; these module loaders used raw sums.) */
static int ld_oob(uint64_t off, uint64_t size, size_t limit)
{
	return size > (uint64_t)limit || off > (uint64_t)limit - size;
}

static int load64(lxp_module_t *mod, const uint8_t *img, size_t image_size, void *region,
		  size_t region_size, const lxp_loader_sym_t *imports, size_t n_imports)
{
	Elf64_Ehdr eh;
	memcpy(&eh, img, sizeof(eh));

	if (eh.e_type != ET_REL || eh.e_machine != EM_X86_64)
		return LXP_ERR_NOT_SUPPORTED;
	if (eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shnum == 0)
		return LXP_ERR_INVALID_PARAM;
	if (eh.e_shnum > LXP_LOADER_MAX_SECTIONS)
		return LXP_ERR_NO_MEMORY;
	if (ld_oob(eh.e_shoff, (uint64_t)eh.e_shnum * eh.e_shentsize, image_size))
		return LXP_ERR_INVALID_PARAM;

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
			if (ld_oob(off, sh.sh_size, region_size))
				return LXP_ERR_NO_MEMORY;
			uint8_t *dst = mod->region + off;
			if (sh.sh_type == SHT_NOBITS) {
				memset(dst, 0, sh.sh_size);
			} else {
				if (ld_oob(sh.sh_offset, sh.sh_size, image_size))
					return LXP_ERR_INVALID_PARAM;
				memcpy(dst, img + sh.sh_offset, sh.sh_size);
			}
			mod->sec_addr[i] = dst;
			off += sh.sh_size;
		} else if (sh.sh_type == SHT_SYMTAB) {
			if (sh.sh_entsize != sizeof(Elf64_Sym))
				return LXP_ERR_NOT_SUPPORTED;
			if (ld_oob(sh.sh_offset, sh.sh_size, image_size))
				return LXP_ERR_INVALID_PARAM;
			mod->symtab = img + sh.sh_offset;
			mod->sym_count = (uint32_t)(sh.sh_size / sizeof(Elf64_Sym));
			if (sh.sh_link < eh.e_shnum) {
				Elf64_Shdr st;
				rd_shdr64(&eh, img, sh.sh_link, &st);
				if (ld_oob(st.sh_offset, st.sh_size, image_size))
					return LXP_ERR_INVALID_PARAM;
				mod->strtab = (const char *)(img + st.sh_offset);
				mod->strtab_size = (uint32_t)st.sh_size;
			}
		}
	}
	mod->region_used = off;
	if (!mod->symtab || !mod->strtab)
		return LXP_ERR_INVALID_PARAM;

	for (unsigned i = 0; i < eh.e_shnum; i++) {
		Elf64_Shdr sh;
		rd_shdr64(&eh, img, i, &sh);
		if (sh.sh_type != SHT_RELA)
			continue;
		unsigned tgt = sh.sh_info;
		if (tgt >= eh.e_shnum || !mod->sec_addr[tgt])
			continue;
		if (sh.sh_entsize != sizeof(Elf64_Rela))
			return LXP_ERR_NOT_SUPPORTED;
		if (ld_oob(sh.sh_offset, sh.sh_size, image_size))
			return LXP_ERR_INVALID_PARAM;
		size_t n = sh.sh_size / sizeof(Elf64_Rela);
		for (size_t r = 0; r < n; r++) {
			Elf64_Rela rela;
			memcpy(&rela, img + sh.sh_offset + r * sizeof(Elf64_Rela), sizeof(rela));
			int rc = apply_rela64(mod, tgt, &rela, imports, n_imports);
			if (rc != LXP_OK)
				return rc;
		}
	}
	return LXP_OK;
}

/* ── ELF32 / ARM (REL) ──────────────────────────────────────────────────── */

static void rd_shdr32(const Elf32_Ehdr *eh, const uint8_t *img, unsigned i, Elf32_Shdr *out)
{
	memcpy(out, img + eh->e_shoff + (size_t)i * eh->e_shentsize, sizeof(*out));
}

/*
 * Allocate an 8-byte Thumb veneer ("ldr.w pc, [pc, #0]; .word target") in the
 * spare tail of the load region and return its even entry address (0 if the
 * region is full). Used when a Thumb BL/B.W target is beyond the +/-16 MB
 * reach of a direct branch (e.g. a firmware import from a distant load
 * region). The target word keeps its Thumb bit, so the long branch via PC
 * stays in Thumb state.
 */
static uintptr_t arm_veneer(lxp_module_t *mod, uintptr_t target)
{
	size_t off = (mod->region_used + 3u) & ~(size_t)3u;
	if (off + 8 > mod->region_size)
		return 0;
	uint8_t *v = mod->region + off;
	uint16_t hw1 = 0xf8df, hw2 = 0xf000;
	uint32_t t = (uint32_t)target;
	memcpy(v, &hw1, 2);
	memcpy(v + 2, &hw2, 2);
	memcpy(v + 4, &t, 4);
	mod->region_used = off + 8;
	return (uintptr_t)v;
}

static int apply_rel_arm(lxp_module_t *mod, unsigned tgt, const Elf32_Rel *rel,
			 const lxp_loader_sym_t *imports, size_t n_imports)
{
	uint32_t symidx = rel->r_info >> 8;
	uint32_t type = rel->r_info & 0xff;
	if (symidx >= mod->sym_count)
		return LXP_ERR_INVALID_PARAM;

	Elf32_Sym sym;
	memcpy(&sym, (const uint8_t *)mod->symtab + (size_t)symidx * sizeof(Elf32_Sym),
	       sizeof(sym));

	uintptr_t S;
	int rc = resolve(mod, sym.st_shndx, sym.st_value, sym.st_name, imports, n_imports, &S);
	if (rc != LXP_OK)
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
	case R_ARM_THM_CALL:
	case R_ARM_THM_JUMP24: {
		/* Thumb-2 BL / B.W: a 32-bit instruction in two little-endian
		 * halfwords. Decode the current signed 25-bit displacement (the
		 * REL addend), recompute it against the resolved target (Thumb
		 * bit stripped — the branch stays in Thumb state), and re-encode
		 * the S/J1/J2/imm10/imm11 fields while preserving the opcode. A
		 * target beyond +/-16 MB is routed through a veneer. */
		uint16_t hw1, hw2;
		memcpy(&hw1, loc, 2);
		memcpy(&hw2, loc + 2, 2);
		uint32_t bs = (hw1 >> 10) & 1;
		uint32_t j1 = (hw2 >> 13) & 1;
		uint32_t j2 = (hw2 >> 11) & 1;
		uint32_t i1 = (~(j1 ^ bs)) & 1;
		uint32_t i2 = (~(j2 ^ bs)) & 1;
		uint32_t raw = (bs << 24) | (i1 << 23) | (i2 << 22) |
			       ((uint32_t)(hw1 & 0x3ff) << 12) | ((uint32_t)(hw2 & 0x7ff) << 1);
		int32_t addend = (raw & 0x01000000u) ? (int32_t)(raw | 0xfe000000u) : (int32_t)raw;
		int32_t want = (int32_t)((S & ~(uintptr_t)1) - (uintptr_t)loc) + addend;
		if (want < -0x01000000 || want > 0x00fffffe) {
			uintptr_t v = arm_veneer(mod, S);
			if (!v)
				return LXP_ERR_NO_MEMORY;
			want = (int32_t)(v - (uintptr_t)loc) + addend;
		}
		uint32_t u = (uint32_t)want;
		uint32_t ns = (u >> 24) & 1;
		uint32_t ni1 = (u >> 23) & 1;
		uint32_t ni2 = (u >> 22) & 1;
		uint32_t nj1 = ((ni1 ^ 1) ^ ns) & 1;
		uint32_t nj2 = ((ni2 ^ 1) ^ ns) & 1;
		hw1 = (uint16_t)((hw1 & ~0x07ffu) | (ns << 10) | ((u >> 12) & 0x3ff));
		hw2 = (uint16_t)((hw2 & ~0x2fffu) | (nj1 << 13) | (nj2 << 11) | ((u >> 1) & 0x7ff));
		memcpy(loc, &hw1, 2);
		memcpy(loc + 2, &hw2, 2);
		break;
	}
	default:
		/* MOVW/MOVT absolute and GOT-based relocations are not yet
		 * implemented. */
		return LXP_ERR_NOT_SUPPORTED;
	}
	return LXP_OK;
}

static int load32_arm(lxp_module_t *mod, const uint8_t *img, size_t image_size, void *region,
		      size_t region_size, const lxp_loader_sym_t *imports, size_t n_imports)
{
	Elf32_Ehdr eh;
	memcpy(&eh, img, sizeof(eh));

	if (eh.e_type != ET_REL || eh.e_machine != EM_ARM)
		return LXP_ERR_NOT_SUPPORTED;
	if (eh.e_shentsize != sizeof(Elf32_Shdr) || eh.e_shnum == 0)
		return LXP_ERR_INVALID_PARAM;
	if (eh.e_shnum > LXP_LOADER_MAX_SECTIONS)
		return LXP_ERR_NO_MEMORY;
	if (ld_oob(eh.e_shoff, (uint64_t)eh.e_shnum * eh.e_shentsize, image_size))
		return LXP_ERR_INVALID_PARAM;

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
			if (ld_oob(off, sh.sh_size, region_size))
				return LXP_ERR_NO_MEMORY;
			uint8_t *dst = mod->region + off;
			if (sh.sh_type == SHT_NOBITS) {
				memset(dst, 0, sh.sh_size);
			} else {
				if (ld_oob(sh.sh_offset, sh.sh_size, image_size))
					return LXP_ERR_INVALID_PARAM;
				memcpy(dst, img + sh.sh_offset, sh.sh_size);
			}
			mod->sec_addr[i] = dst;
			off += sh.sh_size;
		} else if (sh.sh_type == SHT_SYMTAB) {
			if (sh.sh_entsize != sizeof(Elf32_Sym))
				return LXP_ERR_NOT_SUPPORTED;
			if (ld_oob(sh.sh_offset, sh.sh_size, image_size))
				return LXP_ERR_INVALID_PARAM;
			mod->symtab = img + sh.sh_offset;
			mod->sym_count = (uint32_t)(sh.sh_size / sizeof(Elf32_Sym));
			if (sh.sh_link < eh.e_shnum) {
				Elf32_Shdr st;
				rd_shdr32(&eh, img, sh.sh_link, &st);
				if (ld_oob(st.sh_offset, st.sh_size, image_size))
					return LXP_ERR_INVALID_PARAM;
				mod->strtab = (const char *)(img + st.sh_offset);
				mod->strtab_size = st.sh_size;
			}
		}
	}
	mod->region_used = off;
	if (!mod->symtab || !mod->strtab)
		return LXP_ERR_INVALID_PARAM;

	for (unsigned i = 0; i < eh.e_shnum; i++) {
		Elf32_Shdr sh;
		rd_shdr32(&eh, img, i, &sh);
		if (sh.sh_type != SHT_REL)
			continue;
		unsigned tgt = sh.sh_info;
		if (tgt >= eh.e_shnum || !mod->sec_addr[tgt])
			continue;
		if (sh.sh_entsize != sizeof(Elf32_Rel))
			return LXP_ERR_NOT_SUPPORTED;
		if (ld_oob(sh.sh_offset, sh.sh_size, image_size))
			return LXP_ERR_INVALID_PARAM;
		size_t n = sh.sh_size / sizeof(Elf32_Rel);
		for (size_t r = 0; r < n; r++) {
			Elf32_Rel rel;
			memcpy(&rel, img + sh.sh_offset + r * sizeof(Elf32_Rel), sizeof(rel));
			int rc = apply_rel_arm(mod, tgt, &rel, imports, n_imports);
			if (rc != LXP_OK)
				return rc;
		}
	}
	return LXP_OK;
}

/* ── public API ─────────────────────────────────────────────────────────── */

int lxp_loader_load(lxp_module_t *mod, const void *image, size_t image_size, void *region,
		    size_t region_size, const lxp_loader_sym_t *imports, size_t n_imports)
{
	if (!mod || !image || !region || image_size < sizeof(Elf32_Ehdr))
		return LXP_ERR_INVALID_PARAM;
	if (n_imports && !imports)
		return LXP_ERR_INVALID_PARAM;

	const uint8_t *img = (const uint8_t *)image;
	if (img[0] != 0x7f || img[1] != 'E' || img[2] != 'L' || img[3] != 'F')
		return LXP_ERR_INVALID_PARAM;
	if (img[EI_DATA] != ELFDATA2LSB)
		return LXP_ERR_NOT_SUPPORTED;

	memset(mod, 0, sizeof(*mod));
	mod->image = img;
	mod->image_size = image_size;
	mod->region = (uint8_t *)region;
	mod->region_size = region_size;

	if (img[EI_CLASS] == ELFCLASS64) {
		if (image_size < sizeof(Elf64_Ehdr))
			return LXP_ERR_INVALID_PARAM;
		return load64(mod, img, image_size, region, region_size, imports, n_imports);
	}
	if (img[EI_CLASS] == ELFCLASS32)
		return load32_arm(mod, img, image_size, region, region_size, imports, n_imports);
	return LXP_ERR_NOT_SUPPORTED;
}

void *lxp_loader_sym(const lxp_module_t *mod, const char *name)
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
			/* On ARM, a Thumb function symbol already carries the
			 * Thumb bit (bit 0) in st_value, so the address is
			 * directly callable as a function pointer. */
			return (uint8_t *)mod->sec_addr[shndx] + value;
	}
	return NULL;
}

size_t lxp_loader_image_size(const lxp_module_t *mod)
{
	return mod ? mod->region_used : 0;
}

/* ── FDPIC (ARM ELFOSABI_ARM_FDPIC) ELF loader ───────────────────────────────
 * FDPIC binaries self-relocate: _start reads the elf32_fdpic_loadmap (passed in r7),
 * calls __self_reloc() to apply the .rofixup function-descriptor list, and derives
 * r9 (the GOT) itself. So we do the kernel's binfmt_elf_fdpic job — load the PT_LOAD
 * segments + build the loadmap for r7 — PLUS apply the .rel.dyn relocations ld.so
 * would (R_ARM_RELATIVE + R_ARM_FUNCDESC_VALUE): __self_reloc covers only the small
 * .rofixup, not .rel.dyn. Static (no DT_NEEDED) → we ignore PT_INTERP, no ld.so.
 * TEXT-SHARING: the RO/executable segment is mapped IN-PLACE from the image (the cpio) —
 * one copy shared by every process, since FDPIC text never self-relocates (all relocations
 * land in the per-process GOT/data). Only the RW segment(s) are copied into the region, so
 * the two are independently biased — the loadmap carries each segment's runtime address and
 * fdpic_rt() resolves a vaddr through it. Little-endian. */

/* Read little-endian halfword / word from a (possibly unaligned) byte pointer. */
static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

#define ELF_ET_DYN 3u
#define ELF_EM_ARM 40u
#define ELF_OSABI_ARM_FDPIC 65u /* e_ident[EI_OSABI] for ARM uClinux FDPIC */
#define ELF_EF_ARM_FDPIC 0x10000000u
#define ELF_PT_LOAD 1u
#define ELF_PT_DYNAMIC 2u
#define ELF_PT_INTERP 3u
#define ELF_PF_X 1u
#define ELF_DT_NULL 0u
#define ELF_DT_NEEDED 1u /* a shared-library dependency → the exec is dynamic, needs ld.so */
#define ELF_DT_PLTGOT 3u
#define ELF_DT_SYMTAB 6u
#define ELF_DT_REL 17u
#define ELF_DT_RELSZ 18u
#define ELF_DT_RELENT 19u
/* ARM relocation types (ELF for the ARM Architecture). */
#define ELF_R_ARM_ABS32 2u
#define ELF_R_ARM_RELATIVE 23u
#define ELF_R_ARM_FUNCDESC 163u
#define ELF_R_ARM_FUNCDESC_VALUE 164u

/* The runtime address of a link-time vaddr via the FDPIC loadmap, where each segment is
 * independently biased: the executable segment maps IN-PLACE from the image (the cpio — one
 * copy shared by every process, since FDPIC text never self-relocates: all relocations land
 * in the per-process GOT/data), the RW segment(s) into the per-process region. 0 if unmapped. */
static uint32_t fdpic_rt(const uint8_t *lm, int nseg, uint32_t vaddr)
{
	for (int i = 0; i < nseg; i++) {
		const uint8_t *e = lm + 4 + (size_t)i * 12;
		uint32_t addr = le32(e), pv = le32(e + 4), msz = le32(e + 8);
		if (vaddr >= pv && vaddr < pv + msz)
			return addr + (vaddr - pv);
	}
	return 0;
}

int lxp_loader_load_fdpic(lxp_flat_t *prog, const void *image, size_t image_size, void *region,
			  size_t region_size, int is_interp, int copy_text)
{
	if (!prog || !image || !region || image_size < 52u /* Elf32_Ehdr */)
		return LXP_ERR_INVALID_PARAM;
	const uint8_t *img = (const uint8_t *)image;
	if (img[0] != 0x7f || img[1] != 'E' || img[2] != 'L' || img[3] != 'F' || img[4] != 1u)
		return LXP_ERR_INVALID_PARAM; /* not ELF / not ELFCLASS32 */
	if (le16(img + 16) != ELF_ET_DYN || le16(img + 18) != ELF_EM_ARM)
		return LXP_ERR_INVALID_PARAM;
	/* FDPIC marker: EI_OSABI (byte 7) == ELFOSABI_ARM_FDPIC (uClinux-fdpic uses this);
	 * some toolchains also set EF_ARM_FDPIC in e_flags. */
	if (img[7] != ELF_OSABI_ARM_FDPIC && !(le32(img + 36) & ELF_EF_ARM_FDPIC))
		return LXP_ERR_NOT_SUPPORTED; /* not an FDPIC image */

	uint32_t e_entry = le32(img + 24);
	uint32_t e_phoff = le32(img + 28);
	uint16_t e_phentsize = le16(img + 42);
	uint16_t e_phnum = le16(img + 44);
	if ((uint64_t)e_phoff + (uint64_t)e_phnum * e_phentsize > image_size)
		return LXP_ERR_INVALID_PARAM;

	/* Pass 1: classify the PT_LOADs. The single executable segment is shared IN-PLACE from
	 * the image (the cpio); the RW segment(s) are packed into the per-process region. Also
	 * locate the dynamic table. PT_INTERP is ignored (we never run a nested loader). */
	uint32_t text_sz = 0, text_off = 0;
	int have_text = 0;
	uint32_t rw_lo = 0xffffffffu, rw_hi = 0;
	uint32_t dyn_off = 0, dyn_sz = 0;
	int nload = 0;
	for (uint16_t i = 0; i < e_phnum; i++) {
		const uint8_t *ph = img + e_phoff + (size_t)i * e_phentsize;
		uint32_t p_type = le32(ph);
		if (p_type == ELF_PT_DYNAMIC) {
			dyn_off = le32(ph + 8); /* p_vaddr of the dynamic table */
			dyn_sz = le32(ph + 20); /* p_memsz */
		}
		if (p_type != ELF_PT_LOAD)
			continue;
		nload++;
		uint32_t p_off = le32(ph + 4), v = le32(ph + 8), msz = le32(ph + 20);
		if (le32(ph + 24) & ELF_PF_X) {
			/* the RO/executable segment — shared in-place from the image (or, for a
			 * copy_text load, copied into the region so it can run from RAM) */
			text_sz = msz;
			text_off = p_off;
			have_text = 1;
		} else {
			if (v < rw_lo)
				rw_lo = v;
			if (v + msz > rw_hi)
				rw_hi = v + msz;
		}
	}
	if (!have_text || nload < 1)
		return LXP_ERR_INVALID_PARAM;
	uint32_t rw_span = (rw_hi > rw_lo) ? (rw_hi - rw_lo) : 0;
	uint32_t rw_a = (rw_span + 3u) & ~3u; /* the loadmap follows the RW block, 4-aligned */
	uint32_t loadmap_sz = 4u + (uint32_t)nload * 12u;
	/* A copy_text load reserves the region's head for the program's own text (copied in below,
	 * run from RAM because its image is not the executable XIP window); the RW block + loadmap +
	 * pool follow it. A normal load leaves the text shared in-place from the image (text_a = 0). */
	uint32_t text_a = copy_text ? ((text_sz + 15u) & ~15u) : 0;
	if ((uint64_t)text_a + rw_a + loadmap_sz > region_size)
		return LXP_ERR_NO_MEMORY;

	/* For a normal load the region holds ONLY the RW block (text shared in-place) + the loadmap +
	 * the descriptor pool. For a copy_text load the region head additionally holds the text. */
	uint8_t *base = (uint8_t *)region + text_a;
	if (rw_span)
		memset(base, 0, rw_span); /* clear so each RW segment's bss tail is zero */

	/* The loadmap (elf32_fdpic_loadmap: u16 version=0, u16 nsegs, then nsegs ×
	 * {u32 addr, u32 p_vaddr, u32 p_memsz}) sits just past the RW block; _start reads it
	 * through r7 and __self_reloc derives each segment's bias from it. */
	uint8_t *lm = base + rw_a;
	{
		uint16_t ver = 0, ns = (uint16_t)nload;
		memcpy(lm, &ver, 2);
		memcpy(lm + 2, &ns, 2);
	}

	/* Pass 2: the executable segment points IN-PLACE at the image (the cpio); each RW segment
	 * is copied into the region at base + (p_vaddr - rw_lo). The loadmap records each segment's
	 * runtime address (text → cpio, RW → region) so __self_reloc biases them independently. */
	uint32_t data_v = 0, data_fsz = 0, data_msz = 0;
	int si = 0;
	for (uint16_t i = 0; i < e_phnum; i++) {
		const uint8_t *ph = img + e_phoff + (size_t)i * e_phentsize;
		if (le32(ph) != ELF_PT_LOAD)
			continue;
		uint32_t p_off = le32(ph + 4), p_vaddr = le32(ph + 8);
		uint32_t p_filesz = le32(ph + 16), p_memsz = le32(ph + 20);
		uint32_t p_flags = le32(ph + 24);
		if ((uint64_t)p_off + p_filesz > image_size)
			return LXP_ERR_INVALID_PARAM;
		uint32_t seg_addr;
		if (p_flags & ELF_PF_X) {
			if (copy_text) {
				/* copy the text into the region head (region[0], reserved above) so it
				 * runs from RAM — the image is a RAM staging buffer, not the XIP window. */
				memcpy((uint8_t *)region, img + p_off, p_filesz);
				seg_addr = (uint32_t)(uintptr_t)region;
			} else {
				seg_addr = (uint32_t)(uintptr_t)(img + p_off); /* shared in-place from the cpio */
			}
		} else {
			uint8_t *d = base + (p_vaddr - rw_lo);
			memcpy(d, img + p_off, p_filesz);
			seg_addr = (uint32_t)(uintptr_t)d;
			data_v = p_vaddr;
			data_fsz = p_filesz;
			data_msz = p_memsz;
		}
		uint8_t *s = lm + 4 + (size_t)si * 12;
		memcpy(s, &seg_addr, 4);
		memcpy(s + 4, &p_vaddr, 4);
		memcpy(s + 8, &p_memsz, 4);
		si++;
	}

	/* Walk the dynamic table for the relocation table, the symbol table + the GOT. */
	uint32_t rel_v = 0, rel_sz = 0, rel_ent = 8, pltgot_v = 0, sym_v = 0;
	int is_dynamic = 0;
	if (dyn_off) {
		const uint8_t *dp = (const uint8_t *)(uintptr_t)fdpic_rt(lm, nload, dyn_off);
		for (uint32_t o = 0; dp && o + 8 <= dyn_sz; o += 8) {
			uint32_t tag = le32(dp + o), val = le32(dp + o + 4);
			if (tag == ELF_DT_NULL)
				break;
			else if (tag == ELF_DT_NEEDED)
				is_dynamic = 1; /* a .so dependency → ld.so relocates it, not us */
			else if (tag == ELF_DT_REL)
				rel_v = val;
			else if (tag == ELF_DT_RELSZ)
				rel_sz = val;
			else if (tag == ELF_DT_RELENT)
				rel_ent = val;
			else if (tag == ELF_DT_PLTGOT)
				pltgot_v = val;
			else if (tag == ELF_DT_SYMTAB)
				sym_v = val;
		}
	}

	/* Apply the .rel.dyn relocations ld.so would (the .rofixup is left to _start's
	 * __self_reloc): R_ARM_RELATIVE/ABS32 rebase a word; R_ARM_FUNCDESC_VALUE fills a
	 * {func, got} descriptor in place; R_ARM_FUNCDESC stores the address of a symbol's
	 * canonical descriptor — the FUNCDESC_VALUE slot if one exists, else a fresh
	 * descriptor we allocate (as ld.so's _dl_funcdesc_for does), since most defined
	 * functions (busybox applets, _init, libc helpers) have a FUNCDESC but no
	 * FUNCDESC_VALUE. Addresses resolve through the loadmap (fdpic_rt): rel/symtab are read
	 * from the in-place text, GOT slots written in the per-process RW region. */
	uint32_t got_base = fdpic_rt(lm, nload, pltgot_v);
	const uint8_t *rel0 = (const uint8_t *)(uintptr_t)fdpic_rt(lm, nload, rel_v);
	/* A descriptor pool just past the loadmap (per-process, in the region); at most one per
	 * reloc (upper bound). */
	uint32_t pool_off = ((uint32_t)(rw_a + loadmap_sz) + 7u) & ~7u;
	uint32_t max_fd = rel_ent ? rel_sz / rel_ent : 0;
	if ((uint64_t)pool_off + (uint64_t)max_fd * 8u > region_size)
		return LXP_ERR_NO_MEMORY;
	uint8_t *fdpool = base + pool_off;
	uint32_t pool_used = 0;
	/* Skip for the interpreter (ld.so self-relocs via _start's __self_reloc + _dl_start's
	 * bootstrap) and for a dynamic exec (ld.so relocates it after loading its .so deps);
	 * applying .rel.dyn here would double-bias R_ARM_RELATIVE. Only a STATIC exec relocates
	 * here (nothing else would). Every address goes through fdpic_rt: the rel/symtab are read
	 * from the in-place text, the relocated GOT slots written in the per-process RW region. */
	for (uint32_t o = 0; !is_interp && !is_dynamic && rel0 && o + rel_ent <= rel_sz; o += rel_ent) {
		const uint8_t *r = rel0 + o;
		uint32_t r_offset = le32(r), r_info = le32(r + 4);
		uint32_t r_type = r_info & 0xffu, r_sym = r_info >> 8;
		uint8_t *where = (uint8_t *)(uintptr_t)fdpic_rt(lm, nload, r_offset);
		if (!where)
			continue;
		if (r_type == ELF_R_ARM_RELATIVE || r_type == ELF_R_ARM_ABS32) {
			uint32_t w = fdpic_rt(lm, nload, le32(where)); /* rebase the target vaddr */
			memcpy(where, &w, 4);
		} else if (r_type == ELF_R_ARM_FUNCDESC_VALUE) {
			/* A function descriptor {func, got} (matches uClibc ld.so): func =
			 * st_value (carries the Thumb bit) PLUS the in-place addend ONLY for a
			 * STB_LOCAL symbol, mapped through the loadmap; got = the module GOT. An
			 * UNDEFINED (weak EH) symbol → null {0,0} so the crt's guarded call skips it. */
			const uint8_t *sym =
				(const uint8_t *)(uintptr_t)fdpic_rt(lm, nload, sym_v) + r_sym * 16u;
			uint32_t w0 = 0, w1 = 0;
			if (sym_v && le16(sym + 14) != 0) { /* st_shndx != SHN_UNDEF */
				uint32_t fnv = le32(sym + 4);
				if ((sym[12] >> 4) == 0) /* ELF_ST_BIND == STB_LOCAL */
					fnv += le32(where);
				w0 = fdpic_rt(lm, nload, fnv);
				w1 = got_base;
			}
			memcpy(where, &w0, 4);
			memcpy(where + 4, &w1, 4);
		} else if (r_type == ELF_R_ARM_FUNCDESC) {
			/* point this GOT slot at the symbol's canonical descriptor (the slot a
			 * FUNCDESC_VALUE relocates) — but NULL for an undefined weak symbol, so a
			 * caller's "if (funcptr) call" guard skips it rather than dereferencing a
			 * {0,0} descriptor and branching to 0. */
			const uint8_t *sym =
				(const uint8_t *)(uintptr_t)fdpic_rt(lm, nload, sym_v) + r_sym * 16u;
			uint32_t descr = 0;
			if (sym_v && le16(sym + 14) != 0) { /* defined (st_shndx != SHN_UNDEF) */
				for (uint32_t p = 0; p + rel_ent <= rel_sz; p += rel_ent) {
					uint32_t i2 = le32(rel0 + p + 4);
					if ((i2 & 0xffu) == ELF_R_ARM_FUNCDESC_VALUE &&
					    (i2 >> 8) == r_sym) {
						descr = fdpic_rt(lm, nload, le32(rel0 + p));
						break;
					}
				}
				if (!descr && pool_used + 8u <= max_fd * 8u) {
					/* no FUNCDESC_VALUE for this symbol (busybox applets, _init,
					 * libc helpers) — synthesize its descriptor {func = st_value +
					 * addend (loadmap-mapped), got}, as ld.so's _dl_funcdesc_for. */
					uint8_t *d = fdpool + pool_used;
					uint32_t fn =
						fdpic_rt(lm, nload, le32(sym + 4) + le32(where));
					memcpy(d, &fn, 4);
					memcpy(d + 4, &got_base, 4);
					descr = (uint32_t)(uintptr_t)d;
					pool_used += 8;
				}
			}
			memcpy(where, &descr, 4);
		}
	}

	prog->region = base;
	prog->region_size = region_size;
	/* bytes consumed from the TRUE region base: the reserved+copied text (copy_text only) plus
	 * the RW block + loadmap + descriptor pool. The launcher lays the stack out above this. */
	prog->region_used = text_a + pool_off + pool_used;
	prog->text_base = copy_text ? (uintptr_t)region	       /* copied into the region head */
				    : (uintptr_t)(img + text_off); /* shared IN-PLACE from the cpio */
	prog->text_size = text_sz;
	prog->data_base = (uintptr_t)fdpic_rt(lm, nload, data_v); /* RW block in the region */
	prog->data_size = data_fsz;
	prog->bss_size = data_msz - data_fsz;
	prog->stack_size = 0x4000u;
	prog->entry = (uintptr_t)fdpic_rt(lm, nload, e_entry);
	prog->is_fdpic = 1;
	prog->loadmap = (uintptr_t)lm; /* passed in r7; _start self-relocates from it */
	/* program headers live in the file-offset-0 text segment: in the image for a shared load,
	 * or in the copied region text for a copy_text load. */
	prog->phdr = copy_text ? ((uintptr_t)region + (e_phoff - text_off))
			       : (uintptr_t)(img + e_phoff);
	prog->phnum = e_phnum;
	prog->region_exec = copy_text; /* the engine maps the region executable for a RAM-text exec */
	prog->is_dynamic = is_dynamic; /* exec with DT_NEEDED → caller loads + enters ld.so */
	prog->got = got_base;	       /* DT_PLTGOT base */
	/* PT_DYNAMIC runtime addr — for an interpreter this is r9 at entry (uClibc-ng's FDPIC
	 * dl_boot_ldso_dyn_pointer, which DL_BOOT_COMPUTE_DYN uses as the dynamic-table ptr). */
	prog->dynamic = dyn_off ? (uintptr_t)fdpic_rt(lm, nload, dyn_off) : 0;
	return LXP_OK;
}

#endif /* CONFIG_LXP_LOADER */
