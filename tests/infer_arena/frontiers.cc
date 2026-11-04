// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ove/infer.h"
#include "ove/console.h"
#include "ove/time.h"
#include "tflm/ove_tflm_arena.h"
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>

static unsigned factories;
static bool cleared;
static OveTflmArena guarded_arena_create(uint8_t *arena, size_t bytes)
{
	++factories;
	cleared = true;
	for (size_t i = 0; i < bytes; ++i)
		cleared &= arena[i] == 0;
	// Stop before any SDK placement, graph access or teardown.
	return {};
}
#define ove_tflm_arena_create guarded_arena_create
#ifndef OVE_INFER_SOURCE
#define OVE_INFER_SOURCE "../../backends/common/ove_infer.cc"
#endif
#include OVE_INFER_SOURCE
#undef ove_tflm_arena_create
#include "models/sine_model.h"
extern "C" int ove_time_get_us(uint64_t *value)
{
	*value = 0;
	return OVE_OK;
}
extern "C" void ove_console_write(const char *, unsigned)
{
}
#define CHECK(condition)                                                  \
	do {                                                              \
		if (!(condition)) {                                       \
			std::printf("frontier failed: %s\n", #condition); \
			return 1;                                         \
		}                                                         \
	} while (false)
alignas(16) static uint8_t arena[8192];
alignas(16) static uint8_t copy[sizeof(sine_model_data) + 16];
static int run(const char *name)
{
	if (!std::strcmp(name, "alignment")) {
		std::memcpy(copy + 1, sine_model_data, sizeof(sine_model_data));
		ove_model_config cfg{copy + 1, sizeof(sine_model_data), sizeof(arena)};
		CHECK(model_config_status(&cfg) == OVE_ERR_INVALID_PARAM);
	} else if (!std::strcmp(name, "minimum")) {
		const size_t minimum = ove_tflm_arena_minimum();
		CHECK(minimum > 0);
		for (size_t n = 0; n < minimum; ++n)
			CHECK(model_arena_status(arena, n) == OVE_ERR_NO_MEMORY);
	} else if (!std::strcmp(name, "range")) {
		ove_model_config cfg{sine_model_data, sizeof(sine_model_data), size_t(INT_MAX) + 1};
		CHECK(model_config_status(&cfg) == OVE_ERR_INVALID_PARAM);
		cfg.arena_size = sizeof(arena);
		cfg.model_data = reinterpret_cast<const void *>(~uintptr_t(15));
		cfg.model_size = 32;
		CHECK(model_config_status(&cfg) == OVE_ERR_INVALID_PARAM);
		CHECK(model_arena_status(reinterpret_cast<void *>(~uintptr_t(15)), 32) ==
		      OVE_ERR_INVALID_PARAM);
	} else if (!std::strcmp(name, "zero") || !std::strcmp(name, "schema")) {
		std::memcpy(copy, sine_model_data, sizeof(sine_model_data));
		const bool schema = !std::strcmp(name, "schema");
		if (schema) {
			CHECK(copy[56] == 3 && copy[57] == 0 && copy[58] == 0 && copy[59] == 0);
			copy[56] = 4;
		}
		std::memset(arena, 0xa5, sizeof(arena));
		ove_model_storage_t model{};
		model.model_data = copy;
		model.arena = arena;
		model.arena_size = sizeof(arena);
		const int rc = model_setup(&model);
		CHECK(rc == (schema ? OVE_ERR_NOT_SUPPORTED : OVE_ERR_NO_MEMORY));
		CHECK(factories == (schema ? 0U : 1U));
		CHECK(schema || cleared);
	} else {
		return 2;
	}
	CHECK(factories <= 1);
	return 0;
}
int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const int rc = run(argv[1]);
	if (!rc)
		std::printf("frontier %s PASS\n", argv[1]);
	return rc;
}
