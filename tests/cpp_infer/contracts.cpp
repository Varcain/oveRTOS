// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef INFER_DEFAULT_ASSERT
static void record_assertion(bool condition);
#define OVE_STATIC_INIT_ASSERT(condition) record_assertion(condition)
#endif

#include <ove/infer.hpp>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace
{
// All native entry points below only record values. No SDK, allocator or
// advertised arena span is accessed, including on the unfixed wrapper.
ove_model_storage_t tokens[4] = {};
alignas(16) const unsigned char model_bytes[64] = {1};
unsigned init_calls, create_calls, close_calls, failed_assertions, next_token;
unsigned failures;
int native_status;
ove_model_config observed;
void *observed_arena;
ove_model_storage_t *observed_storage;
bool member_ended, observe_order, late_close;

void check(bool condition, const char *expression, unsigned line)
{
	if (!condition) {
		std::fprintf(stderr, "line %u: %s\n", line, expression);
		++failures;
	}
}
#define CHECK(condition) check(bool(condition), #condition, __LINE__)

void reset()
{
	init_calls = create_calls = close_calls = failed_assertions = next_token = 0;
	native_status = OVE_OK;
	observed = {};
	observed_arena = nullptr;
	observed_storage = nullptr;
	member_ended = observe_order = late_close = false;
}

ove_model_config config(size_t size)
{
	return {model_bytes, sizeof(model_bytes), size};
}

int publish(ove_model_t *out, const ove_model_config *cfg)
{
	observed = *cfg;
	if (native_status == OVE_OK)
		*out = &tokens[next_token++];
	return native_status;
}

void close(ove_model_t handle)
{
	// The handle always names external initialized tokens, never an ended
	// embedded record. The ordering oracle reads external flags only.
	CHECK(handle != nullptr);
	++close_calls;
	late_close |= observe_order && member_ended;
}
} // namespace

#ifndef INFER_DEFAULT_ASSERT
static void record_assertion(bool condition)
{
	failed_assertions += !condition;
}
#endif

// One TU only. This synthetic member destructor makes ordering observable;
// source/language analysis separately establishes the real trivial lifetime.
namespace ove::detail
{
template <> struct ModelEmbedded<256> {
	ove_model_storage_t storage = {};
	alignas(16) uint8_t arena[256] = {};
	~ModelEmbedded()
	{
		member_ended = true;
	}
};
} // namespace ove::detail

extern "C" int ove_model_init(ove_model_t *out, ove_model_storage_t *storage, void *arena,
			      const ove_model_config *cfg)
{
	++init_calls;
	observed_storage = storage;
	observed_arena = arena;
	return publish(out, cfg);
}
extern "C" void ove_model_deinit(ove_model_t handle)
{
	close(handle);
}
#ifndef CONFIG_OVE_ZERO_HEAP
extern "C" int ove_model_create(ove_model_t *out, const ove_model_config *cfg)
{
	++create_calls;
	return publish(out, cfg);
}
extern "C" void ove_model_destroy(ove_model_t handle)
{
	close(handle);
}
#endif

static_assert(!std::is_copy_constructible_v<ove::Model<256>>);
static_assert(!std::is_copy_assignable_v<ove::Model<256>>);
static_assert(!std::is_copy_constructible_v<ove::ModelStorage<256>>);
static_assert(!std::is_move_constructible_v<ove::ModelStorage<256>>);
static_assert(std::is_move_constructible_v<ove::Model<>>);
static_assert(std::is_move_assignable_v<ove::Model<>>);
#ifdef CONFIG_OVE_ZERO_HEAP
static_assert(!std::is_move_constructible_v<ove::Model<256>>);
static_assert(!std::is_move_assignable_v<ove::Model<256>>);
static_assert(!std::is_constructible_v<ove::Model<>, const ove_model_config &>);
#else
static_assert(std::is_move_constructible_v<ove::Model<256>>);
static_assert(std::is_move_assignable_v<ove::Model<256>>);
#endif

namespace
{
void capacity()
{
	for (size_t size : {size_t(0), size_t(129), size_t(256), size_t(257),
			    std::numeric_limits<size_t>::max()}) {
#if defined(INFER_DEFAULT_ASSERT) && !defined(NDEBUG)
		if (size == 0)
			continue;
#ifdef CONFIG_OVE_ZERO_HEAP
		if (size > 256)
			continue;
#endif
#endif
		reset();
		if (size == 0)
			native_status = OVE_ERR_INVALID_PARAM;
		const auto cfg = config(size);
		bool rejected = false;
#ifdef CONFIG_OVE_ZERO_HEAP
		rejected = size > 256;
#endif
		{
			ove::Model<256> model(cfg);
			CHECK((model.handle() == nullptr) == (rejected || size == 0));
			CHECK(init_calls + create_calls == unsigned(!rejected));
			if (!rejected) {
				CHECK(observed.model_data == cfg.model_data);
				CHECK(observed.model_size == cfg.model_size);
				CHECK(observed.arena_size == size);
#ifdef CONFIG_OVE_ZERO_HEAP
				CHECK(init_calls == 1 && create_calls == 0);
				CHECK(observed_storage != nullptr && observed_arena != nullptr);
				CHECK(reinterpret_cast<uintptr_t>(observed_arena) % 16 == 0);
#else
				CHECK(create_calls == 1 && init_calls == 0);
#endif
			}
			CHECK(cfg.arena_size == size);
#ifndef INFER_DEFAULT_ASSERT
			CHECK(failed_assertions == unsigned(rejected || size == 0));
#endif
		}
		CHECK(close_calls == unsigned(!rejected && size != 0));
	}
}

void lifetime()
{
	reset();
	observe_order = true;
	{
		ove::Model<256> model(config(256));
		CHECK(model.handle() != nullptr);
		CHECK(!member_ended && close_calls == 0);
	}
	CHECK(close_calls == 1);
	CHECK(!late_close);
#ifdef CONFIG_OVE_ZERO_HEAP
	CHECK(member_ended);
#else
	CHECK(!member_ended);
#endif
}

void errors()
{
#if !defined(INFER_DEFAULT_ASSERT) || defined(NDEBUG)
	for (int status : {OVE_ERR_INVALID_PARAM, OVE_ERR_NO_MEMORY, OVE_ERR_BUSY,
			   OVE_ERR_NOT_SUPPORTED, OVE_ERR_ML_FAILED}) {
		reset();
		native_status = status;
		{
			ove::Model<256> model(config(129));
			CHECK(model.handle() == nullptr);
			CHECK(init_calls + create_calls == 1);
#ifndef INFER_DEFAULT_ASSERT
			CHECK(failed_assertions == 1);
#endif
		}
		CHECK(close_calls == 0);
	}
#endif
}

void storage()
{
	for (int status : {OVE_OK, OVE_ERR_INVALID_PARAM, OVE_ERR_NO_MEMORY, OVE_ERR_BUSY,
			   OVE_ERR_NOT_SUPPORTED, OVE_ERR_ML_FAILED}) {
		reset();
		native_status = status;
		ove::ModelStorage<256> backing;
		{
			auto model = backing.load(model_bytes, sizeof(model_bytes));
			CHECK(model.has_value() == (status == OVE_OK));
			if (!model)
				CHECK(model.error() == static_cast<ove::Error>(status));
			CHECK(init_calls == 1 && create_calls == 0);
			CHECK(observed.arena_size == 256);
			CHECK(observed.model_data == model_bytes);
			CHECK(observed.model_size == sizeof(model_bytes));
			CHECK(failed_assertions == 0);
		}
		CHECK(close_calls == unsigned(status == OVE_OK));
	}
}

void moves()
{
	reset();
	ove::ModelStorage<256> first, second;
	{
		auto a = first.load(model_bytes, sizeof(model_bytes));
		auto b = second.load(model_bytes, sizeof(model_bytes));
		CHECK(a && b);
		const auto handle = a->handle();
		ove::Model<> moved(std::move(*a));
		CHECK(a->handle() == nullptr && moved.handle() == handle);
		*b = std::move(moved);
		CHECK(close_calls == 1 && moved.handle() == nullptr && b->handle() == handle);
		auto *alias = &*b;
		*b = std::move(*alias);
		CHECK(close_calls == 1 && b->handle() == handle);
	}
	CHECK(close_calls == 2);
#ifndef CONFIG_OVE_ZERO_HEAP
	reset();
	{
		ove::Model<> first_model(config(512)), second_model(config(512));
		const auto handle = first_model.handle();
		ove::Model<> moved(std::move(first_model));
		second_model = std::move(moved);
		CHECK(create_calls == 2 && init_calls == 0 && close_calls == 1);
		CHECK(first_model.handle() == nullptr && moved.handle() == nullptr);
		CHECK(second_model.handle() == handle);
	}
	CHECK(close_calls == 2);
#endif
}
} // namespace

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	if (std::strcmp(argv[1], "capacity") == 0)
		capacity();
	else if (std::strcmp(argv[1], "lifetime") == 0)
		lifetime();
	else if (std::strcmp(argv[1], "errors") == 0)
		errors();
	else if (std::strcmp(argv[1], "storage") == 0)
		storage();
	else if (std::strcmp(argv[1], "moves") == 0)
		moves();
	else
		return 2;
	std::printf("%s: %u failed checks\n", argv[1], failures);
	return failures ? 1 : 0;
}
