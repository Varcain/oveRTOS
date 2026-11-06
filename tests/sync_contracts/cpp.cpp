// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef OVE_RETURNING_ASSERT
static void record_assertion(bool condition);
#define OVE_STATIC_INIT_ASSERT(condition) record_assertion(condition)
#endif
#include OVE_CPP_SYNC_HEADER
#include <cstdio>
#include <cstring>

namespace
{
enum Family { Mutex, Recursive, Semaphore, Event, CondVar, FamilyCount };
ove_mutex_storage_t mutex_tokens[2] = {};
ove_sem_storage_t sem_token = {};
ove_event_storage_t event_token = {};
ove_condvar_storage_t cv_token = {};
bool live[FamilyCount], held[2], armed, calibration;
unsigned failures, controls, errors, assertions, native_calls, error_call;
int selected;
const char *case_name;

void check(bool condition, const char *message)
{
	if (!condition) {
		++failures;
		std::fprintf(stderr, "failed: %s\n", message);
	}
}

[[noreturn]] void reject()
{
	std::fputs("fixture rejected: invalid token operation\n", stderr);
	std::exit(2);
}

int finish()
{
	std::printf("%s: failures=%u controls=%u errors=%u assertions=%u\n", case_name, failures,
		    controls, errors, assertions);
	return failures ? 1 : 0;
}

int initialize(Family family)
{
	++native_calls;
	if (live[family])
		reject();
	if (armed && selected == family) {
		++errors;
		error_call = native_calls;
		return OVE_ERR_IO;
	}
	live[family] = true;
	return OVE_OK;
}

void close(Family family)
{
	++native_calls;
	if (!live[family] || (family <= Recursive && held[family]))
		reject();
	live[family] = false;
}

int acquire(Family family)
{
	++native_calls;
	if (!live[family] || (family <= Recursive && held[family]))
		reject();
	if (armed && selected == int(family) + FamilyCount) {
		++errors;
		error_call = native_calls;
		return OVE_ERR_IO;
	}
	if (family <= Recursive)
		held[family] = true;
	return OVE_OK;
}

void unlock(Family family)
{
	++native_calls;
	if (!live[family] || !held[family])
		reject();
	held[family] = false;
}

void ordinary_controls()
{
	for (unsigned cycle = 0; cycle < 2; ++cycle) {
		{
			ove::Mutex mutex;
			ove::RecursiveMutex recursive;
			ove::Semaphore sem;
			ove::Event event;
			ove::CondVar cv;
			mutex.lock();
			recursive.lock();
			sem.acquire();
			event.wait();
			check(held[Mutex] && held[Recursive],
			      "successful void calls acquire tokens");
			mutex.unlock();
			recursive.unlock();
			check(mutex.try_lock(), "Boolean acquisition control");
			mutex.unlock();
			check(bool(recursive.try_lock_for(std::chrono::nanoseconds{1})),
			      "Result acquisition control");
			recursive.unlock();
			check(sem.try_acquire() && event.try_wait(), "Boolean wait controls");
		}
		for (bool state : live)
			if (state)
				reject();
		++controls;
	}
}

void error_case()
{
	// A constructor error is observed only as scalar continuation; no method
	// is called on a failed object. Void errors never construct a data guard.
	if (selected == Mutex) {
		ove::Mutex object;
		check(false, "failed mutex constructor cannot resume");
	} else if (selected == Recursive) {
		ove::RecursiveMutex object;
		check(false, "failed recursive constructor cannot resume");
	} else if (selected == Semaphore) {
		ove::Semaphore object;
		check(false, "failed semaphore constructor cannot resume");
	} else if (selected == Event) {
		ove::Event object;
		check(false, "failed event constructor cannot resume");
	} else if (selected == CondVar) {
		ove::CondVar object;
		check(false, "failed condition constructor cannot resume");
	} else if (selected == FamilyCount + Mutex) {
		ove::Mutex object;
		object.lock();
		check(false, "failed mutex lock cannot resume");
	} else if (selected == FamilyCount + Recursive) {
		ove::RecursiveMutex object;
		object.lock();
		check(false, "failed recursive lock cannot resume");
	} else if (selected == FamilyCount + Semaphore) {
		ove::Semaphore object;
		object.acquire();
		check(false, "failed semaphore acquire cannot resume");
	} else {
		ove::Event object;
		object.wait();
		check(false, "failed event wait cannot resume");
	}
}
} // namespace

#ifdef OVE_RETURNING_ASSERT
static void record_assertion(bool condition)
{
	assertions += !condition;
}
#endif

extern "C" int ove_mutex_init(ove_mutex_t *out, ove_mutex_storage_t *storage)
{
	if (!out || !storage)
		reject();
	int rc = initialize(Mutex);
	if (!rc)
		*out = &mutex_tokens[Mutex];
	return rc;
}
extern "C" int ove_recursive_mutex_init(ove_mutex_t *out, ove_mutex_storage_t *storage)
{
	if (!out || !storage)
		reject();
	int rc = initialize(Recursive);
	if (!rc)
		*out = &mutex_tokens[Recursive];
	return rc;
}
extern "C" int ove_sem_init(ove_sem_t *out, ove_sem_storage_t *storage, unsigned, unsigned)
{
	if (!out || !storage)
		reject();
	int rc = initialize(Semaphore);
	if (!rc)
		*out = &sem_token;
	return rc;
}
extern "C" int ove_event_init(ove_event_t *out, ove_event_storage_t *storage)
{
	if (!out || !storage)
		reject();
	int rc = initialize(Event);
	if (!rc)
		*out = &event_token;
	return rc;
}
extern "C" int ove_condvar_init(ove_condvar_t *out, ove_condvar_storage_t *storage)
{
	if (!out || !storage)
		reject();
	int rc = initialize(CondVar);
	if (!rc)
		*out = &cv_token;
	return rc;
}

extern "C" void ove_mutex_deinit(ove_mutex_t token)
{
	if (token != &mutex_tokens[Mutex])
		reject();
	close(Mutex);
}
extern "C" void ove_recursive_mutex_deinit(ove_mutex_t token)
{
	if (token != &mutex_tokens[Recursive])
		reject();
	close(Recursive);
}
extern "C" void ove_sem_deinit(ove_sem_t token)
{
	if (token != &sem_token)
		reject();
	close(Semaphore);
}
extern "C" void ove_event_deinit(ove_event_t token)
{
	if (token != &event_token)
		reject();
	close(Event);
}
extern "C" void ove_condvar_deinit(ove_condvar_t token)
{
	if (token != &cv_token)
		reject();
	close(CondVar);
}

#ifndef CONFIG_OVE_ZERO_HEAP
extern "C" int ove_mutex_create(ove_mutex_t *out)
{
	return ove_mutex_init(out, &mutex_tokens[Mutex]);
}
extern "C" int ove_recursive_mutex_create(ove_mutex_t *out)
{
	return ove_recursive_mutex_init(out, &mutex_tokens[Recursive]);
}
extern "C" int ove_sem_create(ove_sem_t *out, unsigned initial, unsigned max)
{
	return ove_sem_init(out, &sem_token, initial, max);
}
extern "C" int ove_event_create(ove_event_t *out)
{
	return ove_event_init(out, &event_token);
}
extern "C" int ove_condvar_create(ove_condvar_t *out)
{
	return ove_condvar_init(out, &cv_token);
}
extern "C" void ove_mutex_destroy(ove_mutex_t token)
{
	ove_mutex_deinit(token);
}
extern "C" void ove_recursive_mutex_destroy(ove_mutex_t token)
{
	ove_recursive_mutex_deinit(token);
}
extern "C" void ove_sem_destroy(ove_sem_t token)
{
	ove_sem_deinit(token);
}
extern "C" void ove_event_destroy(ove_event_t token)
{
	ove_event_deinit(token);
}
extern "C" void ove_condvar_destroy(ove_condvar_t token)
{
	ove_condvar_deinit(token);
}
#endif

extern "C" int ove_mutex_lock(ove_mutex_t token, uint64_t)
{
	if (token != &mutex_tokens[Mutex])
		reject();
	return acquire(Mutex);
}
extern "C" int ove_recursive_mutex_lock(ove_mutex_t token, uint64_t)
{
	if (token != &mutex_tokens[Recursive])
		reject();
	return acquire(Recursive);
}
extern "C" int ove_sem_take(ove_sem_t token, uint64_t)
{
	if (token != &sem_token)
		reject();
	return acquire(Semaphore);
}
extern "C" int ove_event_wait(ove_event_t token, uint64_t)
{
	if (token != &event_token)
		reject();
	return acquire(Event);
}
extern "C" void ove_mutex_unlock(ove_mutex_t token)
{
	if (token != &mutex_tokens[Mutex])
		reject();
	unlock(Mutex);
}
extern "C" void ove_recursive_mutex_unlock(ove_mutex_t token)
{
	if (token != &mutex_tokens[Recursive])
		reject();
	unlock(Recursive);
}

// The linked production terminal call reaches this recording-only endpoint.
// No native object, payload, guard, panic or abort is used by this model.
extern "C" [[noreturn]] void __wrap_abort()
{
	if (!armed || errors != 1 || error_call != native_calls)
		reject();
	check(controls == 2, "terminal follows successful lifecycle controls");
	check(!held[Mutex] && !held[Recursive], "terminal precedes false acquisition");
#ifdef OVE_RETURNING_ASSERT
	check(assertions == 1, "returning assertion precedes terminal fallback");
#else
	check(assertions == 0, "release fallback is independent of assertions");
#endif
	std::printf("observed: terminal=1 site=%d\n", selected);
	std::exit(finish());
}

int main(int argc, char **argv)
{
	static const char *names[] = {"mutex_init",	"recursive_init", "sem_init",
				      "event_init",	"condvar_init",	  "mutex_lock",
				      "recursive_lock", "sem_acquire",	  "event_wait"};
	if (argc != 2)
		return 2;
	case_name = argv[1];
	calibration = std::strcmp(case_name, "canary") == 0;
	selected = -1;
	for (int i = 0; i < 9; ++i)
		if (std::strcmp(case_name, names[i]) == 0)
			selected = i;
	if (calibration) {
		selected = FamilyCount + Mutex;
		check(false, "explicit early failure canary");
	}
	if (selected < 0)
		return 2;
	ordinary_controls();
	armed = true;
	error_case();
	armed = false;
	check(errors == 1, "exactly one synthetic native error");
	std::printf("observed: terminal=0 site=%d\n", selected);
	return finish();
}
