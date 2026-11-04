/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ove_tflm_arena.h"

#include <algorithm>
#include <new>

#include "tensorflow/lite/micro/arena_allocator/single_arena_buffer_allocator.h"
#include "tensorflow/lite/micro/memory_planner/greedy_memory_planner.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/tflite_bridge/flatbuffer_conversions_bridge.h"

class OveArenaAllocator : public tflite::SingleArenaBufferAllocator
{
	using Base = tflite::SingleArenaBufferAllocator;

      public:
	OveArenaAllocator(uint8_t *arena, size_t bytes) : Base(arena, bytes), capacity_(bytes)
	{
	}

	uint8_t *AllocatePersistentBuffer(size_t bytes, size_t alignment) override
	{
		const size_t available = GetAvailableMemory(1);
		/* Check before the base forms tail - bytes; its head bound does not
		 * account for live temporary allocations. */
		if (alignment == 0 || bytes > available || alignment - 1 > available - bytes) {
			mark_exhausted();
#ifdef OVE_TFLM_ARENA_OBSERVER
			OVE_TFLM_ARENA_OBSERVER(*this, "persistent", bytes, alignment, nullptr,
						kTfLiteError);
#endif
			return nullptr;
		}
		auto *result = track(Base::AllocatePersistentBuffer(bytes, alignment));
#ifdef OVE_TFLM_ARENA_OBSERVER
		OVE_TFLM_ARENA_OBSERVER(*this, "persistent", bytes, alignment, result,
					result ? kTfLiteOk : kTfLiteError);
#endif
		return result;
	}

	uint8_t *AllocateTemp(size_t bytes, size_t alignment) override
	{
		auto *result = track(Base::AllocateTemp(bytes, alignment));
#ifdef OVE_TFLM_ARENA_OBSERVER
		OVE_TFLM_ARENA_OBSERVER(*this, "temp", bytes, alignment, result,
					result ? kTfLiteOk : kTfLiteError);
#endif
		return result;
	}

	uint8_t *AllocateResizableBuffer(size_t bytes, size_t alignment) override
	{
		auto *result = track(Base::AllocateResizableBuffer(bytes, alignment));
#ifdef OVE_TFLM_ARENA_OBSERVER
		OVE_TFLM_ARENA_OBSERVER(*this, "resizable", bytes, alignment, result,
					result ? kTfLiteOk : kTfLiteError);
#endif
		return result;
	}

	TfLiteStatus ResizeBuffer(uint8_t *buffer, size_t bytes, size_t alignment) override
	{
		const TfLiteStatus result = track(Base::ResizeBuffer(buffer, bytes, alignment));
#ifdef OVE_TFLM_ARENA_OBSERVER
		OVE_TFLM_ARENA_OBSERVER(*this, "resize", bytes, alignment, buffer, result);
#endif
		return result;
	}

	TfLiteStatus ReserveNonPersistentOverlayMemory(size_t bytes, size_t alignment) override
	{
		const TfLiteStatus result =
			track(Base::ReserveNonPersistentOverlayMemory(bytes, alignment));
#ifdef OVE_TFLM_ARENA_OBSERVER
		OVE_TFLM_ARENA_OBSERVER(*this, "reserve", bytes, alignment, nullptr, result);
#endif
		return result;
	}

	void mark_exhausted()
	{
		exhausted_ = true;
	}

	bool exhausted() const
	{
		return exhausted_;
	}

	size_t capacity() const
	{
		return capacity_;
	}

	/* Instances live in caller arena storage, never a separate heap object. */
	void *operator new(size_t) = delete;

      private:
	/* Match the pinned base's static-memory destructor convention. */
	void operator delete(void *) noexcept
	{
	}

	uint8_t *track(uint8_t *buffer)
	{
		if (!buffer)
			mark_exhausted();
		return buffer;
	}

	TfLiteStatus track(TfLiteStatus status)
	{
		if (status != kTfLiteOk)
			mark_exhausted();
		return status;
	}

	size_t capacity_;
	bool exhausted_ = false;
};

class OveMemoryPlanner : public tflite::GreedyMemoryPlanner
{
	using Base = tflite::GreedyMemoryPlanner;

      public:
	explicit OveMemoryPlanner(OveArenaAllocator &arena) : arena_(arena)
	{
	}

	TfLiteStatus Init(unsigned char *scratch, int bytes) override
	{
		const TfLiteStatus status = Base::Init(scratch, bytes);
		if (status != kTfLiteOk) {
			arena_.mark_exhausted();
			return status;
		}
		max_entries_ = bytes / Base::per_buffer_size();
#ifdef OVE_TFLM_PLANNER_OBSERVER
		OVE_TFLM_PLANNER_OBSERVER(arena_, "init", scratch, bytes, Base::GetBufferCount(),
					  0);
#endif
		return kTfLiteOk;
	}

	TfLiteStatus AddBuffer(int bytes, int first, int last) override
	{
		if (!entry_available())
			return kTfLiteError;
		const TfLiteStatus status = Base::AddBuffer(bytes, first, last);
#ifdef OVE_TFLM_PLANNER_OBSERVER
		if (status == kTfLiteOk)
			OVE_TFLM_PLANNER_OBSERVER(arena_, "add", nullptr, 0, Base::GetBufferCount(),
						  0);
#endif
		return status;
	}

	TfLiteStatus AddBuffer(int bytes, int first, int last, int offset) override
	{
		if (!entry_available())
			return kTfLiteError;
		const TfLiteStatus status = Base::AddBuffer(bytes, first, last, offset);
#ifdef OVE_TFLM_PLANNER_OBSERVER
		if (status == kTfLiteOk)
			OVE_TFLM_PLANNER_OBSERVER(arena_, "add", nullptr, 0, Base::GetBufferCount(),
						  0);
#endif
		return status;
	}

	TfLiteStatus GetOffsetForBuffer(int index, int *offset) override
	{
		/* Planner scratch occupies the free gap at this point. Compare the
		 * complete plan with arena capacity, before CommitPlan forms pointers. */
		const size_t persistent = arena_.GetPersistentUsedBytes();
#ifdef OVE_TFLM_PLANNER_OBSERVER
		OVE_TFLM_PLANNER_OBSERVER(arena_, "plan", nullptr, 0, Base::GetBufferCount(),
					  Base::GetMaximumMemorySize());
#endif
		if (persistent > arena_.capacity() ||
		    Base::GetMaximumMemorySize() > arena_.capacity() - persistent) {
			arena_.mark_exhausted();
#ifdef OVE_TFLM_PLANNER_OBSERVER
			OVE_TFLM_PLANNER_OBSERVER(arena_, "reject", nullptr, 0,
						  Base::GetBufferCount(),
						  Base::GetMaximumMemorySize());
#endif
			return kTfLiteError;
		}
		return Base::GetOffsetForBuffer(index, offset);
	}

	/* Instances live in caller arena storage, never a separate heap object. */
	void *operator new(size_t) = delete;

      private:
	/* Match the pinned base's static-memory destructor convention. */
	void operator delete(void *) noexcept
	{
	}

	bool entry_available()
	{
		if (max_entries_ == 0 || Base::GetBufferCount() >= max_entries_) {
			arena_.mark_exhausted();
			return false;
		}
		return true;
	}

	OveArenaAllocator &arena_;
	int max_entries_ = 0;
};

size_t ove_tflm_arena_minimum()
{
	/* At the pinned revision this public sum contains the private builtin-data
	 * allocator, along with these three publicly sized types. No SDK copy is
	 * needed. Its sole extra member is a pointer, with the base's alignment. */
	const size_t builtin_size = tflite::MicroAllocator::GetDefaultTailUsage(true) -
				    sizeof(tflite::SingleArenaBufferAllocator) -
				    sizeof(tflite::MicroAllocator) -
				    sizeof(tflite::SubgraphAllocations);
	constexpr size_t builtin_alignment =
		std::max(alignof(tflite::TfLiteBridgeBuiltinDataAllocator),
			 alignof(tflite::IPersistentBufferAllocator *));
	static_assert(alignof(OveArenaAllocator) <= 16 && alignof(OveMemoryPlanner) <= 16 &&
		      alignof(tflite::MicroAllocator) <= 16 && builtin_alignment <= 16);
	return sizeof(OveArenaAllocator) + alignof(OveArenaAllocator) - 1 +
	       sizeof(OveMemoryPlanner) + alignof(OveMemoryPlanner) - 1 +
	       sizeof(tflite::MicroAllocator) + alignof(tflite::MicroAllocator) - 1 + builtin_size +
	       builtin_alignment - 1;
}

OveTflmArena ove_tflm_arena_create(uint8_t *arena, size_t bytes)
{
	if (!arena || bytes < ove_tflm_arena_minimum())
		return {};
	OveArenaAllocator temporary(arena, bytes);
	auto *buffer_bytes = temporary.AllocatePersistentBuffer(sizeof(OveArenaAllocator),
								alignof(OveArenaAllocator));
	if (!buffer_bytes)
		return {};
	auto *buffer = ::new (buffer_bytes) OveArenaAllocator(temporary);
	auto *planner_bytes = buffer->AllocatePersistentBuffer(sizeof(OveMemoryPlanner),
							       alignof(OveMemoryPlanner));
	if (!planner_bytes)
		return {buffer, nullptr};
	auto *planner = ::new (planner_bytes) OveMemoryPlanner(*buffer);
	return {buffer, tflite::MicroAllocator::Create(buffer, planner)};
}

bool ove_tflm_arena_exhausted(const OveTflmArena &arena)
{
	return arena.buffer && arena.buffer->exhausted();
}
