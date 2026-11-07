# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Literal metadata states and expected context flags."""

profiles = []


def add(name, provider, defines, cases, fallback=False):
    rows = []
    for case, flag, old_flag in cases:
        rows.append(
            {
                "case": case,
                "expected_flags": flag,
                "expected_resource_blocking": old_flag,
                "readonly": True,
                "native_effects": 0,
            }
        )
        if old_flag == 0 and not fallback:
            rows.append(
                {
                    "case": case + "__restored_valid",
                    "expected_flags": 7,
                    "expected_resource_blocking": 3,
                    "readonly": True,
                    "native_effects": 0,
                    "setup": "restore all metadata to ordinary supported state",
                }
            )
    profiles.append(
        {
            "profile": name,
            "provider": provider,
            "defines": defines,
            "config_fallback": fallback,
            "base_cases": len(cases),
            "queries_with_restored_controls": len(rows),
            "cases": rows,
        }
    )


f = [
    ("running", 7, 3),
    ("running_control_2_psp", 7, 3),
    ("prestart", 5, 1),
    ("prestart_primask_1", 5, 1),
    ("prestart_basepri_0x80", 5, 1),
    ("prestart_faultmask_1", 5, 1),
    ("prestart_isr", 0, 0),
    ("prestart_unprivileged", 0, 0),
    ("running_isr", 0, 0),
    ("running_unprivileged", 0, 0),
    ("running_primask_1", 4, 0),
    ("running_basepri_0x80", 4, 0),
    ("running_faultmask_1", 4, 0),
    ("scheduler_suspended", 4, 0),
    ("current_idle", 0, 0),
    ("final_ordinary", 7, 3),
]
fa = {
    "CONFIG_OVE_SYNC": 1,
    "__ARM_ARCH_7EM__": 1,
    "configNUMBER_OF_CORES": 1,
    "INCLUDE_xTaskGetSchedulerState": 1,
    "INCLUDE_xTaskGetIdleTaskHandle": 1,
}
add("f-arm7em-sync", "freertos", fa, f)
fb = dict(fa)
del fb["__ARM_ARCH_7EM__"]
fb["__ARM_ARCH_7M__"] = 1
small = [("running", 7, 3), ("prestart_masked", 5, 1), ("running_masked", 4, 0)]
add("f-arm7m-sync", "freertos", fb, small)
add("f-arm7em-async", "freertos", dict(fa, CONFIG_OVE_ASYNC=1), small)
for field, short in [
    ("INCLUDE_xTaskGetSchedulerState", "scheduler"),
    ("INCLUDE_xTaskGetIdleTaskHandle", "idle"),
]:
    for value, label in [(0, "zero"), (None, "absent_default_zero"), (2, "non_one")]:
        flags = dict(fa)
        if value is None:
            del flags[field]
        else:
            flags[field] = value
        add(
            "f-" + short + "-" + label,
            "freertos",
            flags,
            [("otherwise_running", 0, 0), ("otherwise_prestart", 0, 0)],
            fallback=True,
        )
flags = dict(fa)
del flags["__ARM_ARCH_7EM__"]
flags["__ARM_ARCH_6M__"] = 1
add(
    "f-unsupported-arch",
    "freertos",
    flags,
    [("otherwise_running", 0, 0), ("otherwise_prestart", 0, 0)],
    fallback=True,
)
add(
    "f-multicore",
    "freertos",
    dict(fa, configNUMBER_OF_CORES=2),
    [("otherwise_running", 0, 0), ("otherwise_prestart", 0, 0)],
    fallback=True,
)

za = {"CONFIG_OVE_SYNC": 1, "CONFIG_MULTITHREADING": 1, "__ARM_ARCH_7EM__": 1}
zarm = [
    ("ordinary_untagged", 7, 3),
    ("control_2_psp", 7, 3),
    ("user", 0, 0),
    ("pre_kernel", 0, 0),
    ("isr", 0, 0),
    ("unprivileged", 0, 0),
    ("primask_1", 4, 0),
    ("basepri_0x80", 4, 0),
    ("faultmask_1", 4, 0),
    ("null_current", 0, 0),
    ("idle_current", 0, 0),
    ("cooperative_prio_minus_1", 7, 3),
    ("scheduler_locked_255", 7, 3),
    ("final_ordinary", 7, 3),
]
znative = [
    ("ordinary_untagged", 7, 3),
    ("user", 0, 0),
    ("pre_kernel", 0, 0),
    ("isr", 0, 0),
    ("native_lock_1", 4, 0),
    ("null_current", 0, 0),
    ("idle_current", 0, 0),
    ("cooperative_prio_minus_1", 7, 3),
    ("scheduler_locked_255", 7, 3),
    ("final_ordinary", 7, 3),
]
add("z-arm-up", "zephyr", za, zarm)
zb = {
    "CONFIG_OVE_SYNC": 1,
    "CONFIG_OVE_ASYNC": 1,
    "CONFIG_MULTITHREADING": 1,
    "CONFIG_BOARD_NATIVE_SIM": 1,
}
add("z-native-up", "zephyr", zb, znative)
add("z-arm-smp", "zephyr", dict(za, CONFIG_SMP=1), zarm)
add("z-native-smp", "zephyr", dict(zb, CONFIG_SMP=1), znative)
flags = dict(za)
del flags["CONFIG_MULTITHREADING"]
add(
    "z-no-multithreading",
    "zephyr",
    flags,
    [("otherwise_ordinary", 0, 0), ("otherwise_masked", 0, 0)],
    fallback=True,
)
add(
    "z-unsupported-arch",
    "zephyr",
    {"CONFIG_OVE_SYNC": 1, "CONFIG_MULTITHREADING": 1},
    [("otherwise_ordinary", 0, 0), ("otherwise_masked", 0, 0)],
    fallback=True,
)

na = {"CONFIG_OVE_SYNC": 1, "CONFIG_BUILD_FLAT": 1, "__ARM_ARCH_7EM__": 1}
states = [
    ("powerup", 0, 0),
    ("boot", 0, 0),
    ("tasklists", 0, 0),
    ("memory", 0, 0),
    ("hardware", 0, 0),
    ("osready", 7, 3),
    ("idleloop", 7, 3),
    ("panic", 0, 0),
]
narm = states + [
    ("control_2_psp", 7, 3),
    ("isr", 0, 0),
    ("unprivileged", 0, 0),
    ("primask_1", 4, 0),
    ("basepri_0x80", 4, 0),
    ("faultmask_1", 4, 0),
    ("null_current", 0, 0),
    ("idle_pid", 0, 0),
    ("lockcount_1", 7, 3),
    ("final_ordinary", 7, 3),
]
nsim = states + [
    ("signal_mask_low", 4, 0),
    ("signal_mask_high_1ull_shift_40", 4, 0),
    ("isr", 0, 0),
    ("null_current", 0, 0),
    ("idle_pid", 0, 0),
    ("lockcount_1", 7, 3),
    ("final_ordinary", 7, 3),
]
add("n-arm-flat-up", "nuttx", na, narm)
add(
    "n-sim-flat-up",
    "nuttx",
    {
        "CONFIG_OVE_SYNC": 1,
        "CONFIG_OVE_ASYNC": 1,
        "CONFIG_BUILD_FLAT": 1,
        "CONFIG_ARCH_SIM": 1,
    },
    nsim,
)
add(
    "n-smp-unsupported",
    "nuttx",
    dict(na, CONFIG_SMP=1),
    [("otherwise_ordinary", 0, 0), ("otherwise_masked", 0, 0)],
    fallback=True,
)
flags = dict(na)
del flags["CONFIG_BUILD_FLAT"]
add(
    "n-not-flat",
    "nuttx",
    flags,
    [("otherwise_ordinary", 0, 0), ("otherwise_masked", 0, 0)],
    fallback=True,
)
add(
    "n-unsupported-arch",
    "nuttx",
    {"CONFIG_OVE_SYNC": 1, "CONFIG_BUILD_FLAT": 1},
    [("otherwise_ordinary", 0, 0), ("otherwise_masked", 0, 0)],
    fallback=True,
)

add(
    "p-sync",
    "posix",
    {"CONFIG_OVE_SYNC": 1},
    [("ordinary", 7, 3), ("modeled_isr", 0, 0), ("after_leave_isr", 7, 3)],
)
add(
    "p-async",
    "posix",
    {"CONFIG_OVE_SYNC": 1, "CONFIG_OVE_ASYNC": 1},
    [
        ("ordinary", 7, 3),
        ("modeled_isr", 0, 0),
        ("depth_1", 4, 0),
        ("depth_2", 4, 0),
        ("depth_1ull_shift_32", 4, 0),
        ("isr_and_depth", 0, 0),
        ("restored_ordinary", 7, 3),
    ],
)
add("header-disabled", "header_only", {}, [("disabled_query", 0, 0)], fallback=True)
add(
    "header-async-only",
    "header_only",
    {"CONFIG_OVE_ASYNC": 1},
    [("sync_disabled_query", 0, 0)],
    fallback=True,
)


PROFILE_NAMES = (
    "f-arm7em-sync",
    "f-arm7m-sync",
    "f-arm7em-async",
    "f-scheduler-zero",
    "f-scheduler-absent_default_zero",
    "f-scheduler-non_one",
    "f-idle-zero",
    "f-idle-absent_default_zero",
    "f-idle-non_one",
    "f-unsupported-arch",
    "f-multicore",
    "z-arm-up",
    "z-native-up",
    "z-arm-smp",
    "z-native-smp",
    "z-no-multithreading",
    "z-unsupported-arch",
    "n-arm-flat-up",
    "n-sim-flat-up",
    "n-smp-unsupported",
    "n-not-flat",
    "n-unsupported-arch",
    "p-sync",
    "p-async",
    "header-disabled",
    "header-async-only",
)


def validate_profiles():
    if tuple(row["profile"] for row in profiles) != PROFILE_NAMES:
        raise ValueError("context profile set changed")
    if len(profiles) != 26 or sum(len(row["cases"]) for row in profiles) != 212:
        raise ValueError("incomplete context matrix")
    for row in profiles:
        names = [case["case"] for case in row["cases"]]
        if len(names) != len(set(names)):
            raise ValueError("duplicate context case")
