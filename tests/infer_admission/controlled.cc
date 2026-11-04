// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <ove/infer.h>
#include <ove/console.h>
#include <ove/time.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>

struct Stats {
	unsigned allocations, frees, live, attempts, setup, blocked, resolvers, interpreters;
	unsigned resolver_opens, resolver_closes, interpreter_opens, interpreter_closes;
	unsigned hooks, hook_errors, consumption_errors, invalid_frees;
};
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static Stats stats;
static struct {
	uintptr_t id;
	size_t bytes;
} allocations[128];
static int fail_alloc = -1, fail_register, fail_setup, fail_model;
static bool reserved, hold_setup, setup_entered, release_setup, tail_hooks;
static ove_model_t closing;
alignas(16) static const unsigned char model_bytes[16] = {1};
alignas(16) static unsigned char arena_a[256], arena_b[256], arena_nested[256];
static ove_model_storage_t storage_a{}, storage_b{}, storage_nested{};
static ove_model_config config()
{
	return {model_bytes, sizeof(model_bytes), sizeof(arena_a)};
}
extern "C" void *__real_malloc(size_t);
extern "C" void __real_free(void *);

static int nested_attempt()
{
	auto cfg = config();
	ove_model_t nested = nullptr;
	pthread_mutex_lock(&control);
	int previous = fail_alloc;
	fail_alloc = 0;
	++stats.hooks;
	unsigned before = stats.attempts;
	pthread_mutex_unlock(&control);
#ifdef OVE_HEAP_INFER
	int rc = ove_model_create(&nested, &cfg);
#else
	int rc = ove_model_init(&nested, &storage_nested, arena_nested, &cfg);
#endif
	pthread_mutex_lock(&control);
	if (rc != OVE_ERR_BUSY || stats.attempts != before)
		++stats.hook_errors;
	fail_alloc = previous;
	pthread_mutex_unlock(&control);
	return rc;
}
extern "C" void *__wrap_malloc(size_t size)
{
	pthread_mutex_lock(&control);
	++stats.attempts;
	if (fail_alloc == 0) {
		pthread_mutex_unlock(&control);
		return nullptr;
	}
	if (fail_alloc > 0)
		--fail_alloc;
	pthread_mutex_unlock(&control);
	void *pointer = __real_malloc(size);
	pthread_mutex_lock(&control);
	if (pointer) {
		unsigned slot = 0;
		while (slot < 128 && allocations[slot].id)
			++slot;
		if (slot == 128)
			abort();
		allocations[slot] = {reinterpret_cast<uintptr_t>(pointer), size};
		++stats.allocations;
		++stats.live;
	}
	pthread_mutex_unlock(&control);
	return pointer;
}
extern "C" void __wrap_free(void *pointer)
{
	if (!pointer)
		return;
	uintptr_t id = reinterpret_cast<uintptr_t>(pointer);
	pthread_mutex_lock(&control);
	unsigned slot = 0;
	while (slot < 128 && allocations[slot].id != id)
		++slot;
	if (slot == 128) {
		++stats.invalid_frees;
		pthread_mutex_unlock(&control);
		return;
	}
	bool hook = tail_hooks;
	pthread_mutex_unlock(&control);
	if (hook)
		nested_attempt();
	__real_free(pointer);
	pthread_mutex_lock(&control);
	allocations[slot] = {};
	++stats.frees;
	--stats.live;
	pthread_mutex_unlock(&control);
	if (hook)
		nested_attempt();
}
extern "C" int infer_probe_get_model()
{
	pthread_mutex_lock(&control);
	++stats.setup;
	// Guard wrongly admitted mutations before either placement object is touched.
	if (reserved || stats.resolvers || stats.interpreters) {
		++stats.blocked;
		pthread_mutex_unlock(&control);
		return 0;
	}
	if (fail_model) {
		pthread_mutex_unlock(&control);
		return 0;
	}
	reserved = true;
	if (hold_setup) {
		setup_entered = true;
		pthread_cond_broadcast(&changed);
		while (!release_setup)
			pthread_cond_wait(&changed, &control);
	}
	pthread_mutex_unlock(&control);
	return 1;
}
extern "C" int infer_probe_register()
{
	return fail_register;
}
extern "C" int infer_probe_allocate()
{
	return fail_setup;
}
extern "C" void infer_probe_resolver(int delta)
{
	pthread_mutex_lock(&control);
	bool hook = tail_hooks && delta < 0;
	pthread_mutex_unlock(&control);
	if (hook)
		nested_attempt();
	pthread_mutex_lock(&control);
	if (delta > 0) {
		++stats.resolvers;
		++stats.resolver_opens;
		reserved = false;
	} else {
		--stats.resolvers;
		++stats.resolver_closes;
	}
	pthread_mutex_unlock(&control);
}
extern "C" void infer_probe_interpreter(int delta)
{
	pthread_mutex_lock(&control);
	bool hook = tail_hooks && delta < 0;
	if (delta < 0 && closing && (closing->interpreter || closing->resolver))
		++stats.consumption_errors;
	pthread_mutex_unlock(&control);
	if (hook)
		nested_attempt();
	pthread_mutex_lock(&control);
	if (delta > 0) {
		++stats.interpreters;
		++stats.interpreter_opens;
	} else {
		--stats.interpreters;
		++stats.interpreter_closes;
	}
	pthread_mutex_unlock(&control);
}
extern "C" int ove_time_get_us(uint64_t *out)
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	*out = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
	return OVE_OK;
}
extern "C" void ove_console_write(const char *data, unsigned size)
{
	fwrite(data, 1, size, stderr);
}
#define CHECK(expression)                                                            \
	do {                                                                         \
		if (!(expression)) {                                                 \
			printf("FAIL %s:%d: %s\n", __func__, __LINE__, #expression); \
			return false;                                                \
		}                                                                    \
	} while (false)
static bool clean()
{
	CHECK(!stats.live && !stats.resolvers && !stats.interpreters && !reserved);
	CHECK(stats.allocations == stats.frees);
	CHECK(stats.resolver_opens == stats.resolver_closes &&
	      stats.interpreter_opens == stats.interpreter_closes);
	CHECK(!stats.hook_errors && !stats.consumption_errors && !stats.invalid_frees);
	return true;
}
static int make(bool heap, ove_model_t *out, ove_model_storage_t *storage, unsigned char *arena)
{
	auto cfg = config();
#ifdef OVE_HEAP_INFER
	if (heap)
		return ove_model_create(out, &cfg);
#else
	if (heap)
		return OVE_ERR_NOT_SUPPORTED;
#endif
	return ove_model_init(out, storage, arena, &cfg);
}
static void close_model(bool heap, ove_model_t model)
{
	closing = model;
#ifdef OVE_HEAP_INFER
	if (heap)
		ove_model_destroy(model);
	else
#else
	(void)heap;
#endif
		ove_model_deinit(model);
	closing = nullptr;
}
static bool pair(bool first_heap, bool next_heap)
{
	ove_model_t first = nullptr;
	CHECK(make(first_heap, &first, &storage_a, arena_a) == OVE_OK);
	memset(&storage_b, 0, sizeof(storage_b));
	memset(arena_b, 0xa5, sizeof(arena_b));
	auto bytes = storage_b;
	ove_model_t next = first;
	auto before = stats;
	int rc = make(next_heap, &next, &storage_b, arena_b);
	CHECK(rc == OVE_ERR_BUSY && next == first);
	CHECK(!memcmp(&bytes, &storage_b, sizeof(bytes)));
	CHECK(stats.attempts == before.attempts && stats.setup == before.setup);
	for (auto value : arena_b)
		CHECK(value == 0xa5);
	nested_attempt();
	CHECK(!stats.hook_errors);
	CHECK(ove_model_invoke(first) == OVE_OK);
	close_model(first_heap, first);
	return clean();
}
static bool busy_tiny(bool heap)
{
	ove_model_t active = nullptr;
	CHECK(make(false, &active, &storage_a, arena_a) == OVE_OK);
	for (size_t bytes : {size_t(0), size_t(1), size_t(63), size_t(64)}) {
		auto cfg = config();
		cfg.arena_size = bytes;
		memset(&storage_b, 0xa5, sizeof(storage_b));
		memset(arena_b, 0xa5, sizeof(arena_b));
		auto saved = storage_b;
		auto before = stats;
		ove_model_t candidate = active;
		int rc;
#ifdef OVE_HEAP_INFER
		if (heap)
			rc = ove_model_create(&candidate, &cfg);
		else
#else
		(void)heap;
#endif
			rc = ove_model_init(&candidate, &storage_b, arena_b + 1, &cfg);
		CHECK(rc == OVE_ERR_BUSY && candidate == active);
		CHECK(!memcmp(&saved, &storage_b, sizeof(saved)));
		CHECK(stats.attempts == before.attempts && stats.setup == before.setup);
		for (auto value : arena_b)
			CHECK(value == 0xa5);
	}
	close_model(false, active);
	CHECK(make(false, &active, &storage_a, arena_a) == OVE_OK);
	close_model(false, active);
	return clean();
}
static bool failure(int kind, bool heap)
{
	fail_alloc = kind == 0 ? 0 : kind == 1 ? 1 : -1;
	fail_model = kind == 2;
	fail_register = kind == 3;
	fail_setup = kind == 4;
	tail_hooks = heap && kind == 1;
	ove_model_t model = nullptr;
	int rc = make(heap, &model, &storage_a, arena_a);
	tail_hooks = false;
	CHECK(!stats.hook_errors);
	CHECK(rc == (kind < 2 ? OVE_ERR_NO_MEMORY : OVE_ERR_ML_FAILED));
	CHECK(model == nullptr);
	CHECK(!stats.live && !stats.resolvers && !stats.interpreters && !reserved);
	fail_alloc = -1;
	fail_model = fail_register = fail_setup = 0;
	CHECK(make(heap, &model, &storage_a, arena_a) == OVE_OK);
	close_model(heap, model);
	return clean();
}
static bool tails(bool heap, bool failed)
{
	ove_model_t model = nullptr;
	if (failed) {
		fail_setup = 1;
		tail_hooks = true;
		CHECK(make(heap, &model, &storage_a, arena_a) == OVE_ERR_ML_FAILED);
		tail_hooks = false;
		fail_setup = 0;
	} else {
		CHECK(make(heap, &model, &storage_a, arena_a) == OVE_OK);
		tail_hooks = true;
		close_model(heap, model);
		tail_hooks = false;
	}
	CHECK(stats.hooks >= 2);
	CHECK(!stats.hook_errors && !stats.consumption_errors);
	CHECK(make(heap, &model, &storage_a, arena_a) == OVE_OK);
	close_model(heap, model);
	return clean();
}
static bool old_static(bool destroy)
{
	ove_model_t old = nullptr, current = nullptr;
	CHECK(make(false, &old, &storage_a, arena_a) == OVE_OK);
	close_model(false, old);
	CHECK(!storage_a.interpreter && !storage_a.resolver);
	CHECK(make(false, &current, &storage_b, arena_b) == OVE_OK);
#ifdef OVE_HEAP_INFER
	if (destroy)
		ove_model_destroy(old);
	else
#else
	(void)destroy;
#endif
		ove_model_deinit(old);
	tail_hooks = true;
	nested_attempt();
	tail_hooks = false;
	CHECK(!stats.hook_errors && !stats.blocked);
	CHECK(ove_model_invoke(current) == OVE_OK);
	close_model(false, current);
	return clean();
}
#ifdef OVE_HEAP_INFER
static bool old_heap()
{
	ove_model_t old = nullptr, current = nullptr;
	CHECK(make(true, &old, &storage_a, arena_a) == OVE_OK);
	ove_model_deinit(old);
	CHECK(!old->interpreter && !old->resolver);
	CHECK(make(false, &current, &storage_b, arena_b) == OVE_OK);
	ove_model_destroy(old);
	nested_attempt();
	CHECK(!stats.hook_errors && !stats.blocked);
	close_model(false, current);
	return clean();
}
#endif
static bool consumption()
{
	ove_model_t model = nullptr;
	CHECK(make(false, &model, &storage_a, arena_a) == OVE_OK);
	close_model(false, model);
	CHECK(!storage_a.interpreter && !storage_a.resolver);
	return clean();
}
static bool invalid()
{
	auto cfg = config();
	ove_model_t model = nullptr;
	CHECK(ove_model_init(nullptr, &storage_a, arena_a, &cfg) == OVE_ERR_INVALID_PARAM);
	CHECK(ove_model_init(&model, nullptr, arena_a, &cfg) == OVE_ERR_INVALID_PARAM);
	cfg.arena_size = 0;
	CHECK(ove_model_init(&model, &storage_a, arena_a, &cfg) == OVE_ERR_NO_MEMORY);
	cfg = config();
	CHECK(ove_model_init(&model, &storage_a, arena_a, &cfg) == OVE_OK);
	close_model(false, model);
	return clean();
}
struct Candidate {
	ove_model_storage_t storage{};
	alignas(16) unsigned char arena[256]{};
	ove_model_t model = nullptr;
	int result = 1;
};
static Candidate candidates[16];
static unsigned finished;
static pthread_barrier_t claim_barrier;
extern "C" void infer_probe_claim_barrier()
{
	pthread_barrier_wait(&claim_barrier);
}
static void *construct_thread(void *argument)
{
	auto *c = static_cast<Candidate *>(argument);
	c->result = make(false, &c->model, &c->storage, c->arena);
	pthread_mutex_lock(&control);
	++finished;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&control);
	return nullptr;
}
static bool parallel()
{
	CHECK(!pthread_barrier_init(&claim_barrier, nullptr, 16));
	hold_setup = true;
	pthread_t threads[16];
	for (auto &thread : threads) {
		unsigned i = &thread - threads;
		CHECK(!pthread_create(&thread, nullptr, construct_thread, &candidates[i]));
	}
	pthread_mutex_lock(&control);
	while (!setup_entered || finished < 15)
		pthread_cond_wait(&changed, &control);
	release_setup = true;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&control);
	for (auto thread : threads)
		CHECK(!pthread_join(thread, nullptr));
	CHECK(!pthread_barrier_destroy(&claim_barrier));
	unsigned winners = 0, losers = 0;
	ove_model_t winner = nullptr;
	for (auto &candidate : candidates) {
		if (candidate.result == OVE_OK) {
			++winners;
			winner = candidate.model;
		} else if (candidate.result == OVE_ERR_BUSY)
			++losers;
	}
	CHECK(winners == 1 && losers == 15 && stats.setup == 1 && !stats.blocked);
	close_model(false, winner);
	return clean();
}
int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const char *name = argv[1];
	bool passed = false;
	if (!strcmp(name, "busy-tiny-static"))
		passed = busy_tiny(false);
	else if (!strcmp(name, "pair-static-static"))
		passed = pair(false, false);
	else if (!strcmp(name, "model-failure"))
		passed = failure(2, false);
	else if (!strcmp(name, "register-failure"))
		passed = failure(3, false);
	else if (!strcmp(name, "setup-failure"))
		passed = failure(4, false);
	else if (!strcmp(name, "static-tails"))
		passed = tails(false, false);
	else if (!strcmp(name, "static-failure-tails"))
		passed = tails(false, true);
	else if (!strcmp(name, "old-static-deinit"))
		passed = old_static(false);
	else if (!strcmp(name, "consume"))
		passed = consumption();
	else if (!strcmp(name, "invalid"))
		passed = invalid();
	else if (!strcmp(name, "parallel"))
		passed = parallel();
#ifdef OVE_HEAP_INFER
	else if (!strcmp(name, "busy-tiny-heap"))
		passed = busy_tiny(true);
	else if (!strcmp(name, "pair-heap-heap"))
		passed = pair(true, true);
	else if (!strcmp(name, "pair-heap-static"))
		passed = pair(true, false);
	else if (!strcmp(name, "pair-static-heap"))
		passed = pair(false, true);
	else if (!strcmp(name, "header-failure"))
		passed = failure(0, true);
	else if (!strcmp(name, "arena-failure"))
		passed = failure(1, true);
	else if (!strcmp(name, "heap-model-failure"))
		passed = failure(2, true);
	else if (!strcmp(name, "heap-register-failure"))
		passed = failure(3, true);
	else if (!strcmp(name, "heap-setup-failure"))
		passed = failure(4, true);
	else if (!strcmp(name, "heap-tails"))
		passed = tails(true, false);
	else if (!strcmp(name, "heap-failure-tails"))
		passed = tails(true, true);
	else if (!strcmp(name, "old-static-destroy"))
		passed = old_static(true);
	else if (!strcmp(name, "old-heap-destroy"))
		passed = old_heap();
#endif
	else
		return 2;
	printf("controlled admission %s: %s\n", name, passed ? "PASS" : "FAIL");
	return passed ? 0 : 1;
}
