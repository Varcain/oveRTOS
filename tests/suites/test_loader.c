/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include "ove/loader.h"

#include <string.h>
#include <sys/mman.h>

/* Generated: a real relocatable object built from suites/loader_mod.c. */
#include "loader_mod_image.h"

#ifdef OVE_TEST_HAVE_ARM
/* Generated: a real ELF32/ARM object built from suites/loader_mod_arm.c. */
#include "loader_mod_arm_image.h"
#endif

#define REGION_BYTES (64u * 1024u)

/* The symbol the test module imports. */
static long host_mul(long a, long b)
{
	return a * b;
}

static void *map_rw(size_t n)
{
	void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}

/* ── rejection paths ─────────────────────────────────────────────────── */

static void test_loader_rejects_bad(void **state)
{
	(void)state;
	static uint8_t region[1024];
	ove_module_t mod;
	unsigned char tiny[4] = {0x7f, 'E', 'L', 'F'};
	unsigned char garbage[64];
	memset(garbage, 0xAB, sizeof(garbage));

	assert_int_equal(ove_loader_load(NULL, ove_loader_test_mod, ove_loader_test_mod_len, region,
					 sizeof(region), NULL, 0),
			 OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_loader_load(&mod, NULL, 64, region, sizeof(region), NULL, 0),
			 OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_loader_load(&mod, tiny, sizeof(tiny), region, sizeof(region), NULL, 0),
			 OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_loader_load(&mod, garbage, sizeof(garbage), region, sizeof(region),
					 NULL, 0),
			 OVE_ERR_INVALID_PARAM);
}

static void test_loader_missing_import(void **state)
{
	(void)state;
	static uint8_t region[REGION_BYTES];
	ove_module_t mod;
	/* No imports: the module references host_mul -> unresolved. */
	assert_int_equal(ove_loader_load(&mod, ove_loader_test_mod, ove_loader_test_mod_len, region,
					 sizeof(region), NULL, 0),
			 OVE_ERR_NOT_FOUND);
}

static void test_loader_region_too_small(void **state)
{
	(void)state;
	uint8_t region[16];
	ove_module_t mod;
	ove_loader_sym_t imports[] = {{"host_mul", (void *)host_mul}};
	assert_int_equal(ove_loader_load(&mod, ove_loader_test_mod, ove_loader_test_mod_len, region,
					 sizeof(region), imports, 1),
			 OVE_ERR_NO_MEMORY);
}

/* ── load + relocate + execute ───────────────────────────────────────── */

static void test_loader_load_and_execute(void **state)
{
	(void)state;
	void *region = map_rw(REGION_BYTES);
	assert_non_null(region);

	ove_loader_sym_t imports[] = {{"host_mul", (void *)host_mul}};
	ove_module_t mod;
	assert_int_equal(ove_loader_load(&mod, ove_loader_test_mod, ove_loader_test_mod_len, region,
					 REGION_BYTES, imports, 1),
			 OVE_OK);
	assert_true(ove_loader_image_size(&mod) > 0);

	/* Make the loaded region read+execute before running its code. */
	assert_int_equal(mprotect(region, REGION_BYTES, PROT_READ | PROT_EXEC), 0);

	/* Exported data symbol resolved + section loaded. */
	long *marker = (long *)ove_loader_sym(&mod, "g_marker");
	assert_non_null(marker);
	assert_int_equal(*marker, 0x1234);

	/* Import relocation (R_X86_64_64) applied into .data. */
	void **host_ref = (void **)ove_loader_sym(&mod, "g_host_ref");
	assert_non_null(host_ref);
	assert_ptr_equal(*host_ref, (void *)host_mul);

	/* Pure exported function — proves loaded code executes. */
	long (*dbl)(long) = (long (*)(long))ove_loader_sym(&mod, "mod_double");
	assert_non_null(dbl);
	assert_int_equal(dbl(21), 42);

	/* Exported function that calls the import + reads internal data. */
	long (*compute)(long) = (long (*)(long))ove_loader_sym(&mod, "mod_compute");
	assert_non_null(compute);
	assert_int_equal(compute(10), 10 * 3 + 7);

	munmap(region, REGION_BYTES);
}

#ifdef OVE_TEST_HAVE_ARM
/* The symbol the ARM module imports. */
static int ext_sym_storage;

/* ELF32/ARM data relocations, verified by value (ARM code is not executed on
 * the host; the written low-32 bits of each resolved address are checked). */
static void test_loader_arm_data_relocs(void **state)
{
	(void)state;
	static uint8_t region[8192];
	ove_loader_sym_t imports[] = {{"ext_sym", &ext_sym_storage}};
	ove_module_t mod;
	assert_int_equal(ove_loader_load(&mod, ove_loader_test_mod_arm, ove_loader_test_mod_arm_len,
					 region, sizeof(region), imports, 1),
			 OVE_OK);
	assert_int_equal(mod.is_elf64, 0);

	/* Internal data section loaded verbatim. */
	uint32_t *g_data = (uint32_t *)ove_loader_sym(&mod, "g_data");
	assert_non_null(g_data);
	assert_int_equal(g_data[0], 11);
	assert_int_equal(g_data[1], 22);

	/* R_ARM_ABS32 to an import. */
	uint32_t *g_to_ext = (uint32_t *)ove_loader_sym(&mod, "g_to_ext");
	assert_non_null(g_to_ext);
	assert_int_equal(*g_to_ext, (uint32_t)(uintptr_t)&ext_sym_storage);

	/* R_ARM_ABS32 to an internal symbol with addend (&g_data[1]). */
	uint32_t *g_to_data = (uint32_t *)ove_loader_sym(&mod, "g_to_data");
	assert_non_null(g_to_data);
	assert_int_equal(*g_to_data, (uint32_t)(uintptr_t)&g_data[1]);
}
#endif /* OVE_TEST_HAVE_ARM */

int test_loader_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_loader_rejects_bad),
		cmocka_unit_test(test_loader_missing_import),
		cmocka_unit_test(test_loader_region_too_small),
		cmocka_unit_test(test_loader_load_and_execute),
#ifdef OVE_TEST_HAVE_ARM
		cmocka_unit_test(test_loader_arm_data_relocs),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
