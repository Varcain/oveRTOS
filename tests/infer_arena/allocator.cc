// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OVE_ARENA_SOURCE
#define OVE_ARENA_SOURCE "../../backends/common/tflm/ove_tflm_arena.cc"
#endif
#include OVE_ARENA_SOURCE
#include "ove/infer.h"
#include "ove/console.h"
#include "ove/time.h"
#include <cstdio>
#include <cstring>

extern "C" int ove_time_get_us(uint64_t *value)
{
	*value = 0;
	return OVE_OK;
}
extern "C" void ove_console_write(const char *, unsigned)
{
}

#define CHECK(condition)                                                              \
	do {                                                                          \
		if (!(condition)) {                                                   \
			std::printf("allocation assertion failed: %s\n", #condition); \
			return 1;                                                     \
		}                                                                     \
	} while (false)

alignas(16) static uint8_t bytes[1024];

static int overlap()
{
	OveArenaAllocator owned(bytes, 512);
	auto *temporary = owned.AllocateTemp(384, 16);
	CHECK(temporary == bytes);
	auto *persistent = owned.AllocatePersistentBuffer(192, 16);
	CHECK(persistent == nullptr && owned.exhausted());
	owned.DeallocateTemp(temporary);
	CHECK(owned.ResetTempAllocations() == kTfLiteOk);
	CHECK(owned.AllocatePersistentBuffer(16, 16) != nullptr && owned.exhausted());

	// The base control only compares addresses; overlapping storage is never used.
	tflite::SingleArenaBufferAllocator base(bytes, 512);
	auto *base_temporary = base.AllocateTemp(384, 16);
	auto *base_persistent = base.AllocatePersistentBuffer(192, 16);
	CHECK(base_temporary == bytes && base_persistent != nullptr);
	CHECK(reinterpret_cast<uintptr_t>(base_persistent) <
	      reinterpret_cast<uintptr_t>(base_temporary) + 384);
	return 0;
}

static int entry(const char *kind)
{
	OveArenaAllocator allocator(bytes, 512);
	tflite::IPersistentBufferAllocator &persistent = allocator;
	tflite::INonPersistentBufferAllocator &transient = allocator;
	if (!std::strcmp(kind, "persistent")) {
		CHECK(persistent.AllocatePersistentBuffer(600, 16) == nullptr);
	} else if (!std::strcmp(kind, "temp")) {
		CHECK(transient.AllocateTemp(600, 16) == nullptr);
		auto *small = transient.AllocateTemp(16, 16);
		CHECK(small != nullptr);
		transient.DeallocateTemp(small);
		CHECK(transient.ResetTempAllocations() == kTfLiteOk);
	} else if (!std::strcmp(kind, "resizable")) {
		CHECK(transient.AllocateResizableBuffer(600, 16) == nullptr);
	} else if (!std::strcmp(kind, "resize")) {
		CHECK(transient.ResizeBuffer(bytes, 600, 16) != kTfLiteOk);
	} else if (!std::strcmp(kind, "reserve")) {
		CHECK(transient.ReserveNonPersistentOverlayMemory(600, 16) != kTfLiteOk);
	} else {
		return 2;
	}
	CHECK(allocator.exhausted());
	CHECK(transient.ReserveNonPersistentOverlayMemory(32, 16) == kTfLiteOk);
	CHECK(allocator.exhausted());
	return 0;
}

static int planner(const char *kind)
{
	OveArenaAllocator allocator(bytes, 512);
	OveMemoryPlanner plan(allocator);
	alignas(16) uint8_t scratch[128]{};
	const int one_entry = static_cast<int>(tflite::GreedyMemoryPlanner::per_buffer_size());
	if (!std::strcmp(kind, "entries") || !std::strcmp(kind, "offline-entries")) {
		CHECK(plan.Init(scratch, one_entry) == kTfLiteOk);
		CHECK(plan.AddBuffer(16, 0, 1) == kTfLiteOk);
		const auto rc = !std::strcmp(kind, "entries") ? plan.AddBuffer(16, 0, 1)
							      : plan.AddBuffer(16, 0, 1, 16);
		CHECK(rc != kTfLiteOk && allocator.exhausted());
		CHECK(plan.GetBufferCount() == 1);
	} else if (!std::strcmp(kind, "plan")) {
		CHECK(allocator.AllocatePersistentBuffer(128, 16) != nullptr);
		CHECK(plan.Init(scratch, 2 * one_entry) == kTfLiteOk);
		CHECK(plan.AddBuffer(256, 0, 1) == kTfLiteOk);
		CHECK(plan.AddBuffer(256, 0, 1) == kTfLiteOk);
		int offset = 771;
		CHECK(plan.GetOffsetForBuffer(0, &offset) != kTfLiteOk);
		CHECK(offset == 771 && allocator.exhausted());
	} else if (!std::strcmp(kind, "live-scratch")) {
		CHECK(allocator.AllocatePersistentBuffer(128, 16) != nullptr);
		auto *all_remaining = allocator.AllocateTemp(384, 16);
		CHECK(all_remaining != nullptr && allocator.GetAvailableMemory(1) == 0);
		CHECK(plan.Init(all_remaining, 384) == kTfLiteOk);
		CHECK(plan.AddBuffer(64, 0, 1) == kTfLiteOk);
		int offset = 771;
		CHECK(plan.GetOffsetForBuffer(0, &offset) == kTfLiteOk);
		CHECK(offset == 0 && !allocator.exhausted());
		allocator.DeallocateTemp(all_remaining);
		CHECK(allocator.ResetTempAllocations() == kTfLiteOk);
	} else {
		return 2;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	int rc;
	if (!std::strcmp(argv[1], "overlap"))
		rc = overlap();
	else if (!std::strcmp(argv[1], "entries") || !std::strcmp(argv[1], "offline-entries") ||
		 !std::strcmp(argv[1], "plan") || !std::strcmp(argv[1], "live-scratch"))
		rc = planner(argv[1]);
	else if (!std::strcmp(argv[1], "layout")) {
		std::printf(
			"layout allocator=%zu/%zu planner=%zu/%zu micro=%zu minimum=%zu scratch=%zu\n",
			sizeof(OveArenaAllocator), sizeof(tflite::SingleArenaBufferAllocator),
			sizeof(OveMemoryPlanner), sizeof(tflite::GreedyMemoryPlanner),
			sizeof(tflite::MicroAllocator), ove_tflm_arena_minimum(),
			12 * sizeof(tflite::internal::ScratchBufferRequest));
		return 0;
	} else
		rc = entry(argv[1]);
	if (rc == 0)
		std::printf("allocator %s PASS\n", argv[1]);
	return rc;
}
