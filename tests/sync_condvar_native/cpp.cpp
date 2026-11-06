// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef OVE_CONDVAR_RETURNING_ASSERT
static void record_assertion(bool condition);
#define OVE_STATIC_INIT_ASSERT(condition) record_assertion(condition)
#endif
#include OVE_CPP_SYNC_HEADER
#include <cstdio>
#include <cstring>

namespace
{
// Only initialized external tokens and logical state are observed. These
// entry points never operate a native mutex or read a protected payload.
ove_mutex_storage_t mutex_token = {};
ove_condvar_storage_t cv_token = {};
bool mutex_live, cv_live, held, armed, predicate_case, calibration;
unsigned failures, controls, wait_calls, predicate_calls, late_predicates;
unsigned mutex_closes, cv_closes, failed_assertions;
int native_result;
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
	std::fprintf(stderr, "fixture rejected: unexpected C++ observer operation\n");
	std::exit(2);
}

int finish()
{
	std::printf("%s: %u failed checks\n", case_name, failures);
	return failures ? 1 : 0;
}

void reset()
{
	if (mutex_live || cv_live || held || armed)
		reject();
	wait_calls = predicate_calls = late_predicates = failed_assertions = 0;
	native_result = OVE_OK;
}

struct Clock {
	using duration = std::chrono::nanoseconds;
	using rep = duration::rep;
	using period = duration::period;
	using time_point = std::chrono::time_point<Clock>;
	static constexpr bool is_steady = true;
	static time_point now()
	{
		return time_point{};
	}
};

struct Predicate {
	bool operator()() const
	{
		++predicate_calls;
		if (!held) {
			// Observe dispatch without accessing any mutex-protected object.
			++late_predicates;
			return true;
		}
		return predicate_calls > 1;
	}
};

void ordinary_controls()
{
	for (int status : {OVE_OK, OVE_ERR_TIMEOUT}) {
		reset();
		const unsigned before_mutex = mutex_closes, before_cv = cv_closes;
		{
			ove::Mutex mutex;
			ove::CondVar cv;
			mutex.lock();
			cv.wait(mutex);
			check(wait_calls == 1 && held, "ordinary wait retains ownership");
			native_result = status;
			const bool ready = cv.try_wait_until(
				mutex, Clock::now() + std::chrono::seconds{1}, Predicate{});
			check(ready && held && predicate_calls == 2 && late_predicates == 0,
			      "success and clean timeout permit predicate access");
			native_result = OVE_ERR_IO;
			auto result = cv.try_wait_for(mutex, std::chrono::milliseconds{1});
			check(!result && result.error() == ove::Error::Io && !held,
			      "result overload preserves native error");
			// Logical cleanup only, after recording the unknown held-state.
			held = true;
			mutex.unlock();
		}
		check(mutex_closes == before_mutex + 1 && cv_closes == before_cv + 1,
		      "ordinary controls close both live tokens once");
		++controls;
	}
	reset();
}
} // namespace

#ifdef OVE_CONDVAR_RETURNING_ASSERT
static void record_assertion(bool condition)
{
	failed_assertions += !condition;
}
#endif

extern "C" int ove_mutex_init(ove_mutex_t *out, ove_mutex_storage_t *storage)
{
	if (!out || !storage || mutex_live)
		reject();
	mutex_live = true;
	*out = &mutex_token;
	return OVE_OK;
}

extern "C" int ove_condvar_init(ove_condvar_t *out, ove_condvar_storage_t *storage)
{
	if (!out || !storage || cv_live)
		reject();
	cv_live = true;
	*out = &cv_token;
	return OVE_OK;
}

extern "C" void ove_mutex_deinit(ove_mutex_t mutex)
{
	if (mutex != &mutex_token || !mutex_live || held)
		reject();
	mutex_live = false;
	++mutex_closes;
}

extern "C" void ove_condvar_deinit(ove_condvar_t cv)
{
	if (cv != &cv_token || !cv_live)
		reject();
	cv_live = false;
	++cv_closes;
}

#ifndef CONFIG_OVE_ZERO_HEAP
extern "C" int ove_mutex_create(ove_mutex_t *out)
{
	return ove_mutex_init(out, &mutex_token);
}
extern "C" int ove_condvar_create(ove_condvar_t *out)
{
	return ove_condvar_init(out, &cv_token);
}
extern "C" void ove_mutex_destroy(ove_mutex_t mutex)
{
	ove_mutex_deinit(mutex);
}
extern "C" void ove_condvar_destroy(ove_condvar_t cv)
{
	ove_condvar_deinit(cv);
}
#endif

extern "C" int ove_mutex_lock(ove_mutex_t mutex, uint64_t)
{
	if (mutex != &mutex_token || !mutex_live || held)
		reject();
	held = true;
	return OVE_OK;
}

extern "C" void ove_mutex_unlock(ove_mutex_t mutex)
{
	if (mutex != &mutex_token || !mutex_live || !held)
		reject();
	held = false;
}

extern "C" int ove_condvar_wait(ove_condvar_t cv, ove_mutex_t mutex, uint64_t)
{
	if (cv != &cv_token || mutex != &mutex_token || !cv_live || !mutex_live || !held)
		reject();
	++wait_calls;
	held = native_result == OVE_OK || native_result == OVE_ERR_TIMEOUT;
	return native_result;
}

// The linked production abort call reaches this recording-only endpoint.
// It never invokes abort or resumes the call with uncertain ownership.
extern "C" [[noreturn]] void __wrap_abort()
{
	if (!armed || native_result != OVE_ERR_IO || held || !mutex_live || !cv_live)
		reject();
	check(controls == 2 && wait_calls == 1, "terminal follows completed valid controls");
	check(predicate_calls == unsigned(predicate_case) && late_predicates == 0,
	      "terminal precedes any post-error predicate access");
#ifdef OVE_CONDVAR_RETURNING_ASSERT
	check(failed_assertions == 1, "returning diagnostic precedes terminal fallback");
#else
	check(failed_assertions == 0, "release terminal does not require an assertion");
#endif
	if (!calibration)
		std::printf("observed: terminal=1 controls=%u waits=%u predicates=%u late=%u\n",
			    controls, wait_calls, predicate_calls, late_predicates);
	// exit runs sanitizer shutdown; all external tokens remain statically live.
	std::exit(finish());
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	case_name = argv[1];
	calibration = std::strcmp(case_name, "canary") == 0;
	predicate_case = std::strcmp(case_name, "predicate_error") == 0;
	if (!calibration && !predicate_case && std::strcmp(case_name, "wait_error") != 0)
		return 2;
	if (calibration)
		check(false, "explicit fixture failure canary");
	ordinary_controls();
	{
		ove::Mutex mutex;
		ove::CondVar cv;
		mutex.lock();
		native_result = OVE_ERR_IO;
		armed = true;
		if (predicate_case) {
			(void)cv.try_wait_until(mutex, Clock::now() + std::chrono::seconds{1},
						Predicate{});
			check(late_predicates == 0, "native error cannot reevaluate predicate");
		} else {
			cv.wait(mutex);
		}
		check(false, "native error cannot resume caller");
		armed = false;
		if (!calibration)
			std::printf(
				"observed: terminal=0 controls=%u waits=%u predicates=%u late=%u\n",
				controls, wait_calls, predicate_calls, late_predicates);
		// Restore logical ownership before valid token-only cleanup on old code.
		held = true;
		mutex.unlock();
	}
	return finish();
}
