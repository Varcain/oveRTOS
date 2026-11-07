# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Closed observations for the scoped Rust synchronization contract program."""
import re

LIFECYCLE = (
    "controls", "live", "init_overlap", "drop_overlap", "failure", "cleaned_error",
    "forget_owner", "construct_isr", "construct_masked", "drop_isr", "drop_masked",
    "raw", "macros",
)
SCOPES = {
    "mutex": ("forget", "error", "drop", "reentry", "masked_release", "blocked_entry", "startup_entry"),
    "recursive": ("forget", "error", "nested", "masked_release", "blocked_entry", "startup_entry"),
}
COMMON = (
    "native_flags", "genuine_contender", "cv_second_owner", "cv_real_wake", "value_busy",
    "value_error", "value_retained_drop", "cv_held_error", "cv_unheld_error",
    "cv_propagated_held_error", "cv_blocked_wait", "cv_timeout_forget", "cv_signal_context",
)
CANARIES = tuple(f"{family}_canary" for family in ("mutex", "recursive", "condvar"))
UNIT_CASES = ("reservation_cap_precedes_native_entry", "pending_reservation_rolls_back_after_timeout")


def unit_observation_valid(text, name, status):
    if name not in UNIT_CASES or status != 0:
        return False
    prefix = f"\nrunning 1 test\ntest sync::scoped_tests::{name} ... ok\n\n"
    footer = ("test result: ok. 1 passed; 0 failed; 0 ignored; 0 measured; "
              "5 filtered out; finished in ")
    return re.fullmatch(re.escape(prefix + footer) + r"[0-9]+\.[0-9]{2}s\n\n", text) is not None


def cases(zero, optional):
    names = tuple(f"{family}_{case}" for family in ("mutex", "recursive", "condvar") for case in LIFECYCLE)
    names += tuple(f"scope_{family}_{case}" for family, suffixes in SCOPES.items() for case in suffixes)
    names += COMMON
    if not zero:
        names += ("value_into_inner", "value_retained_into_inner")
    if optional:
        names += ("scope_mutex_deadline_entry", "scope_recursive_deadline_entry",
                  "cv_deadline_context", "cv_predicate_context", "actual_masked_release")
    return names


def scalars(name, zero, optional):
    """Derive every ordinary footer field from the closed source case protocol."""
    if name not in cases(zero, optional) + CANARIES:
        raise ValueError(name)
    init, locks, unlocks, checks, retained = 4, 0, 0, 10, 0
    if name in CANARIES:
        checks += 1
    elif any(name.startswith(family + "_") for family in ("mutex", "recursive", "condvar")):
        suffix = name.split("_", 1)[1]
        if suffix == "live":
            init += 1 if zero else 2
            checks += 2
        elif suffix in ("init_overlap", "drop_overlap"):
            init += 1 if zero else 2
            checks += 3
        elif suffix in ("failure", "cleaned_error"):
            init += 1 if suffix == "failure" else 2
            checks += 2
        elif suffix == "forget_owner":
            init += 1 if zero else 2
            checks += 2
            retained = 1
        elif suffix.startswith("construct_"):
            init += 1
            checks += 3
        elif suffix.startswith("drop_"):
            init += 1
            checks += 4 if zero else 2
            retained = 1
        elif suffix == "raw":
            init += 2
            checks += 3 if zero else 2
        elif suffix == "macros":
            init += 2
            checks += 1
        elif suffix != "controls":
            raise ValueError(name)
    elif name.startswith("scope_"):
        _, family, suffix = name.split("_", 2)
        init += 1
        locks = unlocks = 1
        if suffix in ("blocked_entry", "startup_entry", "deadline_entry"):
            checks += 3
        else:
            locks += 2 if suffix in ("reentry", "nested") else 1
            unlocks = locks
            if family == "mutex":
                checks += {"forget": 3, "error": 3, "drop": 3, "reentry": 4, "masked_release": 2}[suffix]
            else:
                checks += {"forget": 3, "error": 2, "nested": 3, "masked_release": 2}[suffix]
    else:
        extra = {
            "native_flags": (0, 0, 0, 5 if optional else 4, 0),
            "actual_masked_release": (1, 1, 1, 2, 0),
            "genuine_contender": (1, 2, 2, 7, 0),
            "cv_second_owner": (2, 3, 2, 7, 0),
            "cv_real_wake": (2, 2, 2, 5, 0),
            "value_busy": (1 if zero else 2, 0, 0, 3, 0),
            "value_error": (1, 0, 0, 2, 0),
            "value_retained_drop": (1, 0, 0, 1, 1),
            "value_into_inner": (1, 0, 0, 2, 0),
            "value_retained_into_inner": (1, 0, 0, 3, 1),
            "cv_held_error": (2, 2, 2, 6, 0),
            "cv_unheld_error": (2, 2, 1, 6, 0),
            "cv_propagated_held_error": (2, 2, 2, 6, 0),
            "cv_blocked_wait": (2, 2, 2, 7, 0),
            "cv_timeout_forget": (2, 1, 1, 2, 0),
            "cv_signal_context": (1, 0, 0, 2, 0),
            "cv_deadline_context": (2, 2, 2, 6, 0),
            "cv_predicate_context": (2, 2, 2, 6, 0),
        }
        add_init, locks, unlocks, add_checks, retained = extra[name]
        init += add_init
        checks += add_checks
    return dict(failures=int(name in CANARIES), checks=checks, init=init, deinit=init,
                locks=locks, unlocks=unlocks, duplicates=0, forbidden=0,
                retirements=0, retained=retained, harness_cleanup=retained)


def expected(name, zero, optional):
    row = scalars(name, zero, optional)
    text = ("failed: explicit early failure canary\n" if name in CANARIES else "")
    text += name + ": " + " ".join(f"{key}={value}" for key, value in row.items()) + "\n"
    return int(row["failures"] != 0), text


def observation_valid(text, name, zero, optional, status, *, expected_failure=False):
    """Reject malformed, partial, reset and unexpected nonzero observations."""
    try:
        rc, wanted = expected(name, zero, optional)
    except ValueError:
        return False
    if name in CANARIES and not expected_failure:
        return False
    return status == rc and text == wanted and not re.search(r"AddressSanitizer|runtime error:|panicked at", text)
