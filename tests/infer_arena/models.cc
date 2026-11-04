// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ove/infer.h"
#include "ove/console.h"
#include "ove/time.h"
#include "tflm/ove_tflm_arena.h"
#include "tensorflow/lite/c/common.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

static void allocation_event(OveArenaAllocator &, const char *, size_t, size_t, uint8_t *,
			     TfLiteStatus);
static void planner_event(OveArenaAllocator &, const char *, unsigned char *, size_t, int, size_t);
#define OVE_TFLM_ARENA_OBSERVER allocation_event
#define OVE_TFLM_PLANNER_OBSERVER planner_event
#include "../../backends/common/tflm/ove_tflm_arena.cc"
#ifndef OVE_INFER_SOURCE
#define OVE_INFER_SOURCE "../../backends/common/ove_infer.cc"
#endif
#include OVE_INFER_SOURCE
#include "models/sine_model.h"
#include "g_audio_preprocessor_int8_model_data.h"
#include "g_micro_speech_quantized_model_data.h"

extern "C" int ove_time_get_us(uint64_t *value)
{
	*value = 0;
	return OVE_OK;
}
extern "C" void ove_console_write(const char *, unsigned)
{
}

#define CHECK(condition)                                                         \
	do {                                                                     \
		if (!(condition)) {                                              \
			std::printf("model assertion failed: %s\n", #condition); \
			return 1;                                                \
		}                                                                \
	} while (false)

struct Profile {
	bool active = false, valid = true, allocations_ok = true, rejected = false;
	unsigned inits = 0;
	OveArenaAllocator *owner = nullptr;
	const char *last_kind = "";
	uint8_t *last_result = nullptr;
	size_t last_bytes = 0, last_alignment = 0, peak = 0, before_last = 0;
	size_t prefix = 0, entries = 0, persistent_at_plan = 0, head = 0;
	uintptr_t base = 0;
	size_t capacity = 0;
};
static Profile profile;

static void allocation_event(OveArenaAllocator &owner, const char *kind, size_t count,
			     size_t alignment, uint8_t *result, TfLiteStatus status)
{
	if (!profile.active)
		return;
	if (status != kTfLiteOk)
		profile.allocations_ok = false;
	if (alignment == 0 || alignment > 16 || (alignment & (alignment - 1)))
		profile.valid = false;
	if (profile.inits)
		return;
	profile.before_last = profile.peak;
	profile.peak = std::max(profile.peak, owner.GetNonPersistentUsedBytes());
	profile.owner = &owner;
	profile.last_kind = kind;
	profile.last_result = result;
	profile.last_bytes = count;
	profile.last_alignment = alignment;
}

static void planner_event(OveArenaAllocator &owner, const char *event, unsigned char *scratch,
			  size_t count, int entries, size_t head)
{
	if (!profile.active)
		return;
	if (entries < 0) {
		profile.valid = false;
		return;
	}
	if (!std::strcmp(event, "init")) {
		if (profile.inits || profile.owner != &owner || !profile.allocations_ok ||
		    std::strcmp(profile.last_kind, "temp") || profile.last_result != scratch ||
		    profile.last_bytes != count || profile.last_alignment != 16) {
			profile.valid = false;
			return;
		}
		const uintptr_t base = reinterpret_cast<uintptr_t>(owner.GetOverlayMemoryAddress());
		const uintptr_t pointer = reinterpret_cast<uintptr_t>(scratch);
		const size_t capacity = owner.capacity();
		if ((base & 15) || (capacity & 15) ||
		    capacity > std::numeric_limits<uintptr_t>::max() - base || pointer < base ||
		    pointer > base + capacity || count > base + capacity - pointer) {
			profile.valid = false;
			return;
		}
		profile.inits = 1;
		profile.base = base;
		profile.capacity = capacity;
		profile.prefix = pointer - base;
		profile.peak = profile.before_last;
		profile.persistent_at_plan = owner.GetPersistentUsedBytes();
	} else {
		if (profile.inits != 1 || profile.owner != &owner) {
			profile.valid = false;
			return;
		}
		profile.entries = std::max(profile.entries, static_cast<size_t>(entries));
		if (!std::strcmp(event, "plan") || !std::strcmp(event, "reject")) {
			if (owner.GetPersistentUsedBytes() != profile.persistent_at_plan)
				profile.valid = false;
			profile.head = head;
		}
		if (!std::strcmp(event, "reject"))
			profile.rejected = true;
	}
}

struct Allocation {
	uintptr_t address;
	bool freed;
};
static Allocation allocations[16];
static unsigned allocation_count;
static bool tracking, allocations_valid = true;
extern "C" void *__real_malloc(size_t);
extern "C" void __real_free(void *);
extern "C" void *__wrap_malloc(size_t bytes)
{
	void *pointer = __real_malloc(bytes);
	// Candidate measurements require the same 16-byte front/tail residues.
	// Refuse an unexpected host allocator result before any SDK entry.
	if (pointer && (reinterpret_cast<uintptr_t>(pointer) & 15)) {
		__real_free(pointer);
		pointer = nullptr;
	}
	if (tracking) {
		if (!pointer || allocation_count == 16)
			allocations_valid = false;
		else {
			allocations[allocation_count++] = {reinterpret_cast<uintptr_t>(pointer),
							   false};
			std::memset(pointer, 0xa5, bytes);
		}
	}
	return pointer;
}
extern "C" void __wrap_free(void *pointer)
{
	if (tracking && pointer) {
		unsigned i = 0;
		for (; i < allocation_count; ++i)
			if (allocations[i].address == reinterpret_cast<uintptr_t>(pointer) &&
			    !allocations[i].freed) {
				allocations[i].freed = true;
				break;
			}
		if (i == allocation_count)
			allocations_valid = false;
	}
	__real_free(pointer);
}

static constexpr size_t generous = 131072;
alignas(16) static uint8_t arena[generous + 16];
static ove_model_storage_t storage;
struct Asset {
	const void *data;
	size_t size;
};
static Asset asset(const char *name)
{
	if (!std::strcmp(name, "preprocessor"))
		return {g_audio_preprocessor_int8_model_data,
			g_audio_preprocessor_int8_model_data_len};
	if (!std::strcmp(name, "classifier"))
		return {g_micro_speech_quantized_model_data,
			g_micro_speech_quantized_model_data_len};
	if (!std::strcmp(name, "sine"))
		return {sine_model_data, sizeof(sine_model_data)};
	return {};
}

static int create(ove_model_t *model, Asset data, size_t capacity, bool heap, size_t offset = 0)
{
	const ove_model_config cfg{data.data, data.size, capacity};
#ifdef OVE_HEAP_INFER
	if (heap)
		return ove_model_create(model, &cfg);
#else
	if (heap)
		return OVE_ERR_NOT_SUPPORTED;
#endif
	return ove_model_init(model, &storage, arena + offset, &cfg);
}
static void close(ove_model_t model, bool heap)
{
	profile.active = false;
	profile.owner = nullptr;
	profile.last_result = nullptr;
#ifdef OVE_HEAP_INFER
	if (heap) {
		ove_model_destroy(model);
		return;
	}
#endif
	ove_model_deinit(model);
}
static bool released()
{
	if (!allocations_valid)
		return false;
	for (unsigned i = 0; i < allocation_count; ++i)
		if (!allocations[i].freed)
			return false;
	return true;
}

struct Measurement {
	size_t persistent, total, peak, prefix, entries, plan_persistent, head;
};
static int measure(Asset data, bool heap, Measurement &facts)
{
	std::memset(arena, 0xa5, sizeof(arena));
	profile = {};
	profile.active = true;
	ove_model_t model = nullptr;
	const int rc = create(&model, data, generous, heap);
	if (rc != OVE_OK || !model) {
		profile.active = false;
		profile.owner = nullptr;
		profile.last_result = nullptr;
		return 2;
	}
	auto *owner = profile.owner;
	bool good = profile.valid && profile.allocations_ok && profile.inits == 1 && owner &&
		    !owner->exhausted() && !profile.rejected && profile.entries &&
		    profile.base == reinterpret_cast<uintptr_t>(model->arena) &&
		    owner->GetOverlayMemoryAddress() == model->arena &&
		    model->arena_size == generous;
	if (good) {
		facts = {owner->GetPersistentUsedBytes(),
			 ove_model_arena_used(model),
			 profile.peak,
			 profile.prefix,
			 profile.entries,
			 profile.persistent_at_plan,
			 profile.head};
		good = facts.total == facts.persistent + facts.head &&
		       profile.capacity == generous && profile.head &&
		       facts.plan_persistent <= facts.persistent;
	}
	close(model, heap);
	return good && released() ? 0 : 2;
}

static int candidate(Asset data, bool heap, const Measurement &facts)
{
	const size_t cost = tflite::GreedyMemoryPlanner::per_buffer_size();
	CHECK(cost && facts.prefix <= generous);
	CHECK(facts.entries <= (generous - facts.prefix) / cost);
	const size_t required = std::max(facts.peak, facts.prefix + facts.entries * cost);
	CHECK(required <= generous - 16 && facts.persistent < generous - required - 16);
	const size_t lower = facts.persistent + required + 16;
	const size_t upper = std::min(facts.total, facts.plan_persistent + facts.head);
	CHECK(lower < upper && lower <= generous - 16);
	const size_t capacity = (lower + 16) & ~size_t(15);
	if (capacity >= upper) {
		std::printf("no eligible aligned candidate lower=%zu upper=%zu\n", lower, upper);
		return 2;
	}
	profile = {};
	profile.active = true;
	ove_model_t model = nullptr;
	const int rc = create(&model, data, capacity, heap);
	const bool published = model != nullptr;
	if (model)
		close(model, heap);
	// The failed heap arena is gone. Inspect recorded scalars only.
	profile.active = false;
	profile.owner = nullptr;
	profile.last_result = nullptr;
	CHECK(rc == OVE_ERR_NO_MEMORY && !published && released());
	CHECK(profile.valid && profile.allocations_ok && profile.inits == 1 && profile.rejected);
	CHECK(profile.entries == facts.entries && profile.prefix == facts.prefix &&
	      profile.persistent_at_plan == facts.plan_persistent && profile.head == facts.head);
	std::printf("candidate persistent=%zu setup=%zu prefix=%zu entries=%zu plan_persistent=%zu "
		    "head=%zu total=%zu lower=%zu capacity=%zu upper=%zu rejected=1\n",
		    facts.persistent, facts.peak, facts.prefix, facts.entries,
		    facts.plan_persistent, facts.head, facts.total, lower, capacity, upper);
	Measurement recovery{};
	CHECK(measure(data, heap, recovery) == 0);
	CHECK(recovery.persistent == facts.persistent && recovery.total == facts.total);
	return 0;
}

static int tiny(bool heap)
{
	const size_t minimum = ove_tflm_arena_minimum();
	CHECK(minimum + 32 < sizeof(arena));
	CHECK(4 * 15 + 16 < 12 * sizeof(tflite::internal::ScratchBufferRequest));
	unsigned count = 0;
	CHECK(tflite::MicroArenaBufferAlignment() == 16);
	const size_t residues = heap ? 1 : 16;
	for (size_t offset = 0; offset < residues; ++offset) {
		for (size_t size = 0; size <= minimum + 16; ++size) {
			CHECK(released());
			allocation_count = 0;
			tracking = heap;
			std::memset(arena, 0xa5, sizeof(arena));
			ove_model_t model = nullptr;
			CHECK(create(&model, asset("sine"), size, heap, offset) ==
			      OVE_ERR_NO_MEMORY);
			CHECK(model == nullptr && released());
			if (heap)
				CHECK(allocation_count == (size < minimum ? 0 : 2));
			else
				CHECK(ove_model_arena_used(&storage) == 0);
			const size_t effective_start = (offset + 15) & ~size_t(15);
			for (size_t i = 0; i < effective_start; ++i)
				CHECK(arena[i] == 0xa5);
			++count;
		}
	}
	tracking = false;
	std::printf("tiny heap=%d minimum=%zu residues=%zu cases=%u\n", heap, minimum, residues,
		    count);
	return 0;
}

static int query(bool heap, bool unaligned)
{
	std::memset(arena, 0xa5, sizeof(arena));
	ove_model_t model = nullptr;
	const int rc = create(&model, asset("sine"), 8192, heap, unaligned ? 1 : 0);
	if (rc != OVE_OK || !model)
		return 1;
	auto *interpreter = static_cast<tflite::MicroInterpreter *>(model->interpreter);
	const size_t padding = unaligned ? 15 : 0;
	const size_t reported = ove_model_arena_used(model);
	const bool good = reported == interpreter->arena_used_bytes() + padding &&
			  reported > padding && (!unaligned || arena[1] == 0xa5);
	close(model, heap);
	CHECK(good && ove_model_arena_used(nullptr) == 0 && released());
	if (!heap)
		CHECK(ove_model_arena_used(&storage) == 0);
	std::printf("query heap=%d padding=%zu used=%zu PASS\n", heap, padding, reported);
	return 0;
}

static int contract(const char *name, bool heap)
{
	profile = {};
	std::memset(arena, 0xa5, sizeof(arena));
	alignas(16) uint8_t copy[sizeof(sine_model_data) + 16]{};
	std::vector<uint8_t> backing;
	// The pinned dependency removes the builder's null-allocator fallback.
	flatbuffers::DefaultAllocator fixture_allocator;
	flatbuffers::FlatBufferBuilder builder(1024, &fixture_allocator, false);
	Asset data = asset("sine");
	int expected;
	if (!std::strcmp(name, "unaligned")) {
		std::memcpy(copy + 1, sine_model_data, sizeof(sine_model_data));
		data = {copy + 1, sizeof(sine_model_data)};
		expected = OVE_ERR_INVALID_PARAM;
	} else if (!std::strcmp(name, "version")) {
		std::memcpy(copy, sine_model_data, sizeof(sine_model_data));
		CHECK(copy[56] == 3 && copy[57] == 0 && copy[58] == 0 && copy[59] == 0);
		copy[56] = 4;
		data = {copy, sizeof(sine_model_data)};
		expected = OVE_ERR_NOT_SUPPORTED;
	} else if (!std::strcmp(name, "unregistered")) {
		const auto shape = builder.CreateVector<int32_t>({1});
		std::vector<flatbuffers::Offset<tflite::Tensor>> tensors;
		for (unsigned j = 0; j < 3; ++j)
			tensors.push_back(tflite::CreateTensor(builder, shape,
							       tflite::TensorType_FLOAT32, 0));
		const auto in = builder.CreateVector<int32_t>({0});
		const auto middle = builder.CreateVector<int32_t>({1});
		const auto add_in = builder.CreateVector<int32_t>({1, 0});
		const auto out = builder.CreateVector<int32_t>({2});
		const auto options =
			tflite::CreateAddOptions(builder, tflite::ActivationFunctionType_NONE);
		const auto first = tflite::CreateOperator(builder, 0, in, middle);
		const auto second = tflite::CreateOperator(builder, 1, add_in, out,
							   tflite::BuiltinOptions_AddOptions,
							   options.Union());
		const auto operators = builder.CreateVector<flatbuffers::Offset<tflite::Operator>>(
			{first, second});
		const auto graph = tflite::CreateSubGraph(builder, builder.CreateVector(tensors),
							  in, out, operators);
		const auto codes = builder.CreateVector<flatbuffers::Offset<tflite::OperatorCode>>(
			{tflite::CreateOperatorCode(
				 builder, static_cast<int8_t>(tflite::BuiltinOperator_SIN), 0, 1,
				 tflite::BuiltinOperator_SIN),
			 tflite::CreateOperatorCode(
				 builder, static_cast<int8_t>(tflite::BuiltinOperator_ADD), 0, 1,
				 tflite::BuiltinOperator_ADD)});
		const auto graphs =
			builder.CreateVector<flatbuffers::Offset<tflite::SubGraph>>({graph});
		const auto buffers = builder.CreateVector<flatbuffers::Offset<tflite::Buffer>>(
			{tflite::CreateBuffer(builder)});
		tflite::FinishModelBuffer(builder, tflite::CreateModel(builder, 3, codes, graphs,
								       {}, buffers));
		backing.assign(builder.GetSize() + 15, 0);
		auto *aligned = reinterpret_cast<uint8_t *>(
			(reinterpret_cast<uintptr_t>(backing.data()) + 15) & ~uintptr_t(15));
		std::memcpy(aligned, builder.GetBufferPointer(), builder.GetSize());
		flatbuffers::Verifier verifier(aligned, builder.GetSize());
		CHECK(tflite::VerifyModelBuffer(verifier));
		data = {aligned, builder.GetSize()};
		expected = OVE_ERR_ML_FAILED;
		profile.active = true;
	} else {
		return 2;
	}
	tracking = true;
	ove_model_t model = nullptr;
	const int rc = create(&model, data, generous, heap);
	const bool published = model != nullptr;
	if (model)
		close(model, heap);
	profile.active = false;
	profile.owner = nullptr;
	profile.last_result = nullptr;
	CHECK(rc == expected && !published && released());
	CHECK(profile.valid && profile.allocations_ok && profile.inits == 0);
	Measurement recovery{};
	CHECK(measure(asset("sine"), heap, recovery) == 0);
	std::printf("contract %s heap=%d recovery=1 PASS\n", name, heap);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	if (!std::strcmp(argv[1], "tiny-static"))
		return tiny(false);
#ifdef OVE_HEAP_INFER
	if (!std::strcmp(argv[1], "tiny-heap"))
		return tiny(true);
#endif
	if (argc != 4)
		return 2;
	const bool heap = !std::strcmp(argv[3], "heap");
	if (!heap && std::strcmp(argv[3], "static"))
		return 2;
#ifndef OVE_HEAP_INFER
	if (heap)
		return 2;
#endif
	if (!std::strcmp(argv[1], "contract"))
		return contract(argv[2], heap);
	tracking = true;
	if (!std::strcmp(argv[1], "query")) {
		const bool unaligned = !std::strcmp(argv[2], "unaligned");
		if (heap && unaligned)
			return 2;
		return query(heap, unaligned);
	}
	if (std::strcmp(argv[1], "measure") && std::strcmp(argv[1], "candidate"))
		return 2;
	const Asset data = asset(argv[2]);
	if (!data.data)
		return 2;
	Measurement facts{};
	CHECK(measure(data, heap, facts) == 0);
	std::printf("measurement model=%s heap=%d persistent=%zu total=%zu setup=%zu prefix=%zu "
		    "entries=%zu plan_persistent=%zu head=%zu\n",
		    argv[2], heap, facts.persistent, facts.total, facts.peak, facts.prefix,
		    facts.entries, facts.plan_persistent, facts.head);
	if (!std::strcmp(argv[1], "measure"))
		return 0;
	if (!std::strcmp(argv[1], "candidate"))
		return candidate(data, heap, facts);
	return 2;
}
