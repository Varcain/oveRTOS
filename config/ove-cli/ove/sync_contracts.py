# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Closed observations for the synchronization contract programs."""

CONSTRUCTORS = ("mutex_init", "recursive_attr_init", "recursive_attr_type",
                "recursive_mutex_init", "sem_init", "event_mutex_init",
                "event_cond_init", "condvar_init")
COUNTS = ("count_zero", "count_order", "count_initial_domain", "count_max_domain",
          "count_initial_ceiling_plus_one", "count_max_ceiling_plus_one")
WAIT_ERRORS = tuple(f"{family}_{mode}_error" for family in ("mutex", "condvar", "event", "sem")
                    for mode in ("forever", "timed"))
POLICY_ERRORS = ("event_wait_lock_error", "event_signal_lock_error",
                 "condvar_forever_eagain_error", "condvar_timed_eagain_error",
                 "event_forever_eagain_error", "event_timed_eagain_error",
                 "condvar_forever_timeout_error", "event_forever_timeout_error")
ALLOCATIONS = tuple("allocation_" + family for family in
                    ("mutex", "recursive", "sem", "event", "condvar"))
CPP = ("mutex_init", "recursive_init", "sem_init", "event_init", "condvar_init",
       "mutex_lock", "recursive_lock", "sem_acquire", "event_wait")
NATIVE_CONSTRUCTORS = ("mutex_init", "recursive_init", "sem_init", "event_init", "condvar_init")
NATIVE_ERRORS = ("mutex_error", "recursive_error", "sem_error", "event_error",
                 "mutex_forever_error", "recursive_forever_error", "sem_forever_error", "event_forever_error")
CV_CASES = ("cv_guard_error", "cv_reacquire_error", "cv_retire_error", "cv_wait_error",
            "cv_guard_timeout_error", "cv_reacquire_timeout_error", "cv_retire_timeout_error",
            "cv_clean_timeout", "cv_success", "cv_forever_timeout_error", "cv_entry_eintr",
            "cv_reacquire_eintr", "cv_retire_eintr", "cv_signal_guard_error", "cv_broadcast_guard_error")


def cases(zero):
    """Return the complete supported cases, excluding the common canary."""
    result = {
        "contracts_posix": CONSTRUCTORS + COUNTS + WAIT_ERRORS + POLICY_ERRORS + ("controls",),
        "contracts_cpp_release": CPP,
        "contracts_cpp_hook": CPP,
        "contracts_freertos": NATIVE_CONSTRUCTORS[:3] + COUNTS[:2] + ("controls",),
        "contracts_zephyr": NATIVE_CONSTRUCTORS + COUNTS[:2] + NATIVE_ERRORS + ("controls",),
        "contracts_nuttx": NATIVE_CONSTRUCTORS + ("condvar_second_init",) + COUNTS
                           + NATIVE_ERRORS + CV_CASES + ("controls",),
        "context_timer": ("controls",),
    }
    result["contracts_zephyr_core"] = result["contracts_zephyr"]
    if not zero:
        result["contracts_posix_heap"] = CONSTRUCTORS + ALLOCATIONS + COUNTS + ("controls",)
    for platform in ("freertos", "zephyr", "nuttx", "posix"):
        for features in ("disabled", "sync", "async", "both"):
            result[f"context_{platform}_{features}"] = ("controls",)
    return result


def _footer(case, failures, **scalars):
    return f"{case}: failures={failures} " + " ".join(f"{key}={value}" for key, value in scalars.items()) + "\n"


def _cv_record(case):
    normal = "G0:1:0/g1:1:1/m1:1:0/W1:0:0/M1:0:0/R1:1:0/r0:1:1"
    trace, status, held, waiters, posts, injected = normal, "IO", 1, 0, 0, 1
    if case in ("cv_guard_error", "cv_guard_timeout_error"):
        trace = "G0:1:0"
    elif case in ("cv_reacquire_error", "cv_reacquire_timeout_error"):
        trace, held = "G0:1:0/g1:1:1/m1:1:0/W1:0:0/M1:0:0/R1:0:0/r0:0:1", 0
    elif case in ("cv_retire_error", "cv_retire_timeout_error"):
        trace, waiters = "G0:1:0/g1:1:1/m1:1:0/W1:0:0/M1:0:0/R1:1:0", 1
    elif case in ("cv_signal_guard_error", "cv_broadcast_guard_error"):
        trace, status, held, waiters = "G1:-1:0", "void", -1, 1
    elif case == "cv_entry_eintr":
        trace = "G0:1:0/G0:1:0/g1:1:1/m1:1:0/W1:0:0/M1:0:0/R1:1:0/r0:1:1"
    elif case == "cv_reacquire_eintr":
        trace = "G0:1:0/g1:1:1/m1:1:0/W1:0:0/M1:0:0/M1:0:0/R1:1:0/r0:1:1"
    elif case == "cv_retire_eintr":
        trace = "G0:1:0/g1:1:1/m1:1:0/W1:0:0/M1:0:0/R1:1:0/R1:1:0/r0:1:1"
    if "timeout_error" in case:
        status = "BUSY"
    if case in ("cv_clean_timeout", "cv_entry_eintr", "cv_reacquire_eintr", "cv_retire_eintr"):
        status = "TIMEOUT"
    if case == "cv_clean_timeout":
        injected = 0
    if case == "cv_success":
        status, posts, injected = "OK", 1, 0
    return (f"cv: trace={trace} status={status} held={held} waiters={waiters} "
            f"posts={posts} injections={injected}\n")


def expected(kind, mode, case, expected_failure=False):
    """Bind every printed field and byte to a known kind, mode and case."""
    if mode not in ("heap", "zero", "heap-san", "zero-san"):
        raise ValueError("unknown storage/instrumentation profile")
    registry = cases(mode.startswith("zero"))
    if kind not in registry or case not in registry[kind] + ("canary",):
        raise ValueError("unsupported synchronization observation")
    if expected_failure and case != "canary":
        raise ValueError("only the named canary is a calibration")
    failures = int(expected_failure)
    prefix = "failed: explicit early failure canary\n" if expected_failure else ""
    if kind.startswith("contracts_cpp_"):
        site = 5 if case == "canary" else CPP.index(case)
        text = f"observed: terminal=1 site={site}\n" + _footer(
            case, failures, controls=2, errors=1, assertions=int(kind.endswith("hook")))
    elif kind == "contracts_posix":
        controls, injected, entries, destroys = 2, 0, 14, 14
        if case in CONSTRUCTORS:
            controls, injected = 4, 1
            entries, destroys = {
                "mutex_init": (17, 16), "recursive_attr_init": (16, 16),
                "recursive_attr_type": (16, 16), "recursive_mutex_init": (17, 16),
                "sem_init": (17, 16), "event_mutex_init": (19, 18),
                "event_cond_init": (20, 19), "condvar_init": (19, 18),
            }[case]
        elif case in COUNTS:
            controls, entries, destroys = 3, 16, 16
        elif case in WAIT_ERRORS + POLICY_ERRORS:
            injected = 1
            entries = destroys = 16 if case.startswith(("event_", "condvar_")) else 15
        text = _footer(case, failures, controls=controls, injected=injected,
                       native_inits=entries, destroys=destroys, safety=0)
    elif kind == "contracts_posix_heap":
        controls, injected, allocations, attempts, frees, entries, destroys = 7, 0, 5, 5, 5, 20, 20
        if case in CONSTRUCTORS:
            controls, injected, allocations, attempts, frees = 12, 1, 11, 11, 11
            entries, destroys = (26, 26) if case in ("recursive_attr_init", "recursive_attr_type") else (27, 26)
            if case == "event_cond_init":
                entries, destroys = 28, 27
        elif case in ALLOCATIONS:
            controls, allocations, attempts, frees, entries, destroys = 12, 10, 11, 10, 26, 26
        elif case in COUNTS:
            controls, allocations, attempts, frees, entries, destroys = 12, 11, 11, 11, 26, 26
        text = _footer(case, failures, controls=controls, injected=injected,
                       allocations=allocations, attempts=attempts, free_requests=frees,
                       actual_frees=allocations, native_inits=entries, destroys=destroys,
                       guard_interventions=0, retained_live=0)
    elif kind.startswith("contracts_"):
        backend = kind.removeprefix("contracts_")
        entries = {"freertos": 8, "zephyr": 12, "zephyr_core": 12, "nuttx": 14}[backend]
        controls, repairs = 10, 12 if backend == "zephyr" else 0
        extra = ""
        if case in NATIVE_CONSTRUCTORS or case == "condvar_second_init":
            cv = case.startswith("condvar_") and backend == "nuttx"
            entries += (2 if case == "condvar_second_init" else 1) + (4 if cv else 2)
            controls += 2
            repairs += 2 if backend == "zephyr" else 0
        elif case in COUNTS:
            entries += 2
            controls += 2
            repairs += 2 if backend == "zephyr" else 0
        elif case in NATIVE_ERRORS:
            entries += 2
            controls += 1
            repairs += 2 if backend == "zephyr" else 1
        elif case in CV_CASES:
            entries += 4 if case in ("cv_signal_guard_error", "cv_broadcast_guard_error") else 5
            controls += 1
            repairs += 2 if case in ("cv_signal_guard_error", "cv_broadcast_guard_error") else 3
            extra = _cv_record(case)
        text = extra + _footer(case, failures, controls=controls, init_attempts=entries,
                               safety=0, private_repairs=repairs)
    elif kind == "context_timer":
        text = (f"{case}: failures={failures} callbacks=1,1 first=1,1 recheck=1 enters=1,1 "
                "leaves=1,1 after_leave=0,0 outside=1,1,1 stack_calls=1 stack_bytes=524288 stack_status=0 "
                "applied_bytes=67108864 apply_status=0 readback=67108864 size_status=0 "
                "guard_bytes=4096 guard_status=0 guard_read_status=0 create_attempts=2 creates=2 joins=2 "
                "ready=1,1 regions=1,1 guards=1,1 frames=1,1 tls_distinct=1 disjoint=1 "
                "attr=0,0 ranges=0,0 guard_query=0,0 attr_destroy=0,0\n")
    else:
        features = kind.rsplit("_", 1)[1]
        active = features != "disabled"
        calls = 4 if features in ("async", "both") else 0
        text = _footer(case, failures, outside=0, lock_controls=2 if active else 1,
                       native_locks=calls, native_unlocks=calls, bad_native=0)
    return failures, prefix + text


def observation_valid(text, kind, mode, case, rc, expected_failure=False):
    try:
        status, output = expected(kind, mode, case, expected_failure)
    except ValueError:
        return False
    return rc == status and text == output
