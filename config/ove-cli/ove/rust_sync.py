# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Closed observations for Rust semaphore and event ownership contracts."""

FAMILIES = ("semaphore", "event")
COMMON = (
    "normal", "live", "concurrent_init", "concurrent_live", "teardown",
    "init_error", "cleaned_error", "forget", "drop_isr", "drop_masked",
    "drop_unknown", "construct_isr", "wait_isr", "relative_isr", "forever_isr",
    "post_isr", "named_producer", "named_producer_isr", "macro_reuse", "post_n",
    "post_n_isr", "handoff", "reuse",
)
CONTEXT_OPERATIONS = ("create", "try", "relative", "forever", "post", "named", "many")
CONTEXT_MODES = ("prestart", "masked", "resource", "unknown")


def cases(family, zero, optional):
    if family not in FAMILIES:
        raise ValueError(family)
    names = COMMON
    operations = CONTEXT_OPERATIONS + (("deadline",) if optional else ())
    names += tuple(f"context_{operation}_{mode}" for operation in operations for mode in CONTEXT_MODES)
    if family == "semaphore":
        names += ("invalid_counts",)
    if zero:
        names += ("raw_construct_isr", "raw_origin")
    if optional:
        names += ("deadline_isr",)
        names += ("notify",) if family == "semaphore" else ("actual_masked", "actual_masked_named", "actual_waker")
    return names


def scalars(family, name, zero, optional):
    """Derive counters from the closed case's objects, calls and cleanup paths."""
    if name not in cases(family, zero, optional) + ("canary",):
        raise ValueError(name)
    semaphore = family == "semaphore"
    objects, attempted, cleaned, retained = 2, 0, 0, 0
    checks, waits, task, isr, notify, workers, clocks = 17, 4, 2, 0, 0, 0, 0
    extra_objects, extra_checks, extra_waits, extra_task = 0, 0, 0, 0
    if name == "canary":
        extra_checks = 1
    elif name in ("live", "concurrent_live", "concurrent_init", "teardown"):
        extra_objects = 1 + int(not zero)
        extra_checks, extra_waits, extra_task = 4, 2, 1
        workers = int(name != "live")
        if name == "concurrent_init":
            extra_objects += 1
            extra_checks, extra_waits, extra_task = 9, 4, 2
        elif name == "teardown":
            extra_objects += 1
            extra_checks = 5
    elif name in ("init_error", "cleaned_error"):
        extra_objects, attempted = 1, 1
        cleaned = int(name == "cleaned_error")
        extra_checks, extra_waits, extra_task = 7, 2, 1
    elif name in ("forget", "drop_isr", "drop_masked", "drop_unknown"):
        extra_objects, retained = 1 + int(not zero), 1
        extra_checks = 5
    elif name == "construct_isr":
        extra_objects, extra_checks, extra_waits, extra_task = 1, 6, 2, 1
    elif name in ("wait_isr", "relative_isr", "forever_isr", "deadline_isr"):
        extra_objects, extra_checks, extra_waits, extra_task = 1, 4, 2, 1
        if name == "deadline_isr":
            extra_checks, clocks = 5, 1
    elif name in ("post_isr", "named_producer", "named_producer_isr"):
        extra_objects, extra_checks, extra_waits, extra_task = 1, 5, 3, 1
        task_context = name == "named_producer"
        extra_task += int(task_context)
        isr = int(not semaphore and not task_context)
        if name == "post_isr" and semaphore:
            extra_checks = 4
    elif name == "macro_reuse":
        extra_objects, extra_checks, extra_waits, extra_task = 2, 4, 4, 2
    elif name in ("post_n", "post_n_isr"):
        extra_objects = 1
        interrupt = name == "post_n_isr"
        permits = int(not semaphore) or (0 if interrupt else 2)
        extra_checks = 2 + int(interrupt) + int(interrupt and not semaphore) + permits
        extra_waits = permits + 1
        extra_task = 0 if interrupt else 2
        isr = 2 * int(interrupt and not semaphore)
    elif name == "handoff":
        extra_objects, extra_checks, extra_waits, extra_task, workers = 1, 3, 3, 2, 1
    elif name == "reuse":
        extra_objects, extra_checks, extra_waits, extra_task = 2, 6, 4, 2
    elif name == "raw_construct_isr":
        extra_objects, extra_checks, extra_waits, extra_task = 1, 4, 2, 1
    elif name == "raw_origin":
        extra_objects, extra_checks, extra_waits, extra_task = 2, 6, 4, 2
    elif name == "invalid_counts":
        extra_objects, attempted = 1, 1
        extra_checks, extra_waits, extra_task = 4, 2, 1
    elif name in ("actual_masked", "actual_masked_named", "actual_waker"):
        extra_objects, extra_checks, extra_waits, extra_task = 1, 7, 3, 2
    elif name == "notify":
        extra_objects, extra_checks, extra_waits, extra_task, notify = 1, 20, 7, 6, 4
    elif name.startswith("context_"):
        operation, mode = name[len("context_"):].rsplit("_", 1)
        resource = mode in ("prestart", "resource")
        producer = mode in ("prestart", "masked")
        extra_objects = 1
        if operation == "create":
            extra_objects += int(resource)
            extra_checks = 6 if resource else 5
            extra_waits, extra_task = 2 * extra_objects, extra_objects
        elif operation in ("try", "relative", "forever", "deadline"):
            extra_checks, extra_waits, extra_task = 6, 2, 1
            if operation == "deadline":
                extra_checks, clocks = 7, 1
        else:
            number = 2 if operation == "many" else 1
            permits = (number if semaphore else 1) if producer else 0
            extra_checks, extra_waits = 8 + permits, 3 + permits
            extra_task = 1 + number * int(producer)
    elif name != "normal":
        raise ValueError(name)
    objects += extra_objects
    checks += extra_checks
    waits += extra_waits
    task += extra_task
    before = [objects + attempted, (objects + attempted) * int(not zero),
              objects - retained + cleaned, (objects - retained) * int(not zero),
              0, 0, objects + cleaned, objects - retained + cleaned,
              waits, task, isr, notify, 0]
    after = list(before)
    after[7] += retained
    after[12] = retained
    return dict(checks=checks, failures=int(name == "canary"), retained=retained,
                cleanup=retained, workers=workers, before=before, after=after, clocks=clocks)


def expected(family, name, zero, optional):
    row = scalars(family, name, zero, optional)
    mode = "zero" if zero else "heap"
    text = "FAIL: early_calibration_survives_controls\n" if name == "canary" else ""
    text += (f"sync {mode}/{family} {name}: checks={row['checks']} failures={row['failures']} "
             f"retained={row['retained']} cleanup={row['cleanup']} workers={row['workers']},{row['workers']}\n"
             f"native before: {row['before']}\nnative after: {row['after']}\n"
             f"clock: calls={row['clocks']} forbidden=0\n")
    return row["failures"], text


def observation_valid(text, family, name, zero, optional, status, *, expected_failure=False):
    """Require the complete transcript and status, including persistent canaries."""
    try:
        code, wanted = expected(family, name, zero, optional)
    except ValueError:
        return False
    if name == "canary" and not expected_failure:
        return False
    return status == code and text == wanted
