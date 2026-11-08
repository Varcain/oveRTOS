# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Complete observations for exclusive async queue setup and wake delivery."""
import re


def expected(zero, canary=False):
    mode = "zero" if zero else "heap"
    text = "FAIL: early_calibration_survives_controls\n" if canary else ""
    text += (f"async queue {mode}: checks={15 + int(canary)} "
             f"failures={int(canary)} wakes=2 workers=1,1\n")
    return int(canary), text


def observation_valid(text, zero, status, *, expected_canary=False):
    code, wanted = expected(zero, expected_canary)
    return status == code and text == wanted


def doctests_valid(text, status):
    runs = re.findall(r"^running (\d+) tests?$", text, re.M)
    if status != 0 or sum(map(int, runs)) != 5:
        return False
    rows = re.findall(r"^test (.*AsyncQueue.*::arm.*) \.\.\. ok$", text, re.M)
    summaries = re.findall(
        r"^test result: ok\. (\d+) passed; 0 failed; 0 ignored; 0 measured; "
        r"\d+ filtered out; finished in [0-9.]+s$", text, re.M)
    return (len(rows) == len(set(rows)) == 5 and sum(map(int, summaries)) == 5
            and sum("compile fail" in row for row in rows) == 4)
