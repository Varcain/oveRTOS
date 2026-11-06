// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <ove/infer.h>
#include <ove/storage.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Recording only: no model parser, arena traversal, allocator or worker. */
_Alignas(16) const unsigned char infer_fixture_model[64] = {0x31, 0x42, 0x53, 0x64};
static ove_model_storage_t tokens[4];
static unsigned int counts[5], next_token, failed_checks;
static bool occupied;
static int selected_error;
static struct ove_model_config observed;
static void *observed_arena;
static ove_model_storage_t *saved_storage;
static unsigned char saved_bytes[sizeof(ove_model_storage_t)];

void infer_fixture_reset(void)
{
	/* Session state resets must not erase the process's accumulated oracles. */
	memset(tokens, 0, sizeof(tokens));
	memset(counts, 0, sizeof(counts));
	next_token = 0;
	occupied = false;
	selected_error = OVE_OK;
	observed = (struct ove_model_config){0};
	observed_arena = NULL;
	saved_storage = NULL;
}

void infer_fixture_failure(int status)
{
	selected_error = status;
}

unsigned int infer_fixture_count(unsigned int which)
{
	return which < 5 ? counts[which] : 0;
}

void infer_fixture_check(bool condition, const char *expression)
{
	if (!condition) {
		++failed_checks;
		(void)fprintf(stderr, "failed: %s\n", expression);
	}
}

uint8_t infer_fixture_finish(const char *name)
{
	(void)printf("%s: %u failed checks\n", name, failed_checks);
	return failed_checks ? 1 : 0;
}

void infer_fixture_pin_check(bool violation)
{
	++counts[3];
	counts[4] += violation;
}

bool infer_fixture_header_unchanged(void)
{
	return saved_storage && memcmp(saved_storage, saved_bytes, sizeof(saved_bytes)) == 0;
}

size_t infer_fixture_capacity(void)
{
	return observed.arena_size;
}

const void *infer_fixture_data(void)
{
	return observed.model_data;
}

size_t infer_fixture_length(void)
{
	return observed.model_size;
}

void *infer_fixture_arena(void)
{
	return observed_arena;
}

static int install(ove_model_t *out, ove_model_storage_t *storage, void *arena,
		   const struct ove_model_config *cfg)
{
	observed = *cfg;
	observed_arena = arena;
	if (selected_error != OVE_OK)
		return selected_error;
	if (occupied)
		return OVE_ERR_BUSY;
	occupied = true;
	/* Initialize all observed bytes, including padding; never touch the arena. */
	memset(storage, 0x5a, sizeof(*storage));
	storage->model_data = cfg->model_data;
	storage->model_size = cfg->model_size;
	storage->arena = arena;
	storage->arena_size = cfg->arena_size;
	storage->interpreter = NULL;
	storage->resolver = NULL;
	storage->last_invoke_us = 0;
	storage->heap_allocated = 0;
	saved_storage = storage;
	memcpy(saved_bytes, storage, sizeof(saved_bytes));
	*out = &tokens[next_token++];
	return OVE_OK;
}

int ove_model_init(ove_model_t *model, ove_model_storage_t *storage, void *arena,
		   const struct ove_model_config *cfg)
{
	++counts[0];
	return install(model, storage, arena, cfg);
}

static void close_record(ove_model_t model)
{
	/* Handle tokens are external initialized backing, never dereferenced. */
	infer_fixture_check(model != NULL, "close has a handle");
	++counts[2];
	occupied = false;
}

void ove_model_deinit(ove_model_t model)
{
	close_record(model);
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_model_create(ove_model_t *model, const struct ove_model_config *cfg)
{
	++counts[1];
	/* A fake native-owned record, with no allocation or arena access. */
	return install(model, &tokens[3], NULL, cfg);
}

void ove_model_destroy(ove_model_t model)
{
	close_record(model);
}
#endif
