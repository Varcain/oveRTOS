# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Lint rule: every C example in the cookbook compiles against the real API.

Why this exists
---------------
The cookbook pages under docs-site/docs/cookbook/ show complete programs, but
nothing ever built them, so they drifted onto functions that do not exist.
This rule pulls out each ```c block, prepends `#include "ove/ove.h"`, and runs
a syntax-only compile against include/ with tests/docs/ove_config.h (every
module the cookbook uses switched on). LVGL comes from dl/lvgl when it has
been downloaded, else from the stub header the host tests use.

A block that is deliberately an excerpt is skipped when the line before its
fence is `<!-- ove:fragment -->`.

_PENDING lists pages still being rewritten. A pending page that compiles is
reported too, so the list can only shrink.
"""

import os
import re
import shutil
import subprocess
import tempfile

_PAGES_DIR = "docs-site/docs/cookbook"
_FRAGMENT_MARK = "<!-- ove:fragment -->"
_BLOCK_RE = re.compile(r"^```c[ \t]*\n(.*?)^```[ \t]*$", re.MULTILINE | re.DOTALL)

# page -> why it does not compile yet
_PENDING = {
    "https-post.md": "uses ove_net_is_up/ove_tls_set_trust_store; never calls ove_run()",
    "inference.md": "written against an ove_infer_* API that does not exist",
    "lvgl-thread-safe.md": "uses OVE_TIMER_PERIODIC",
    "periodic-sensor.md": "uses ove_time_now_us",
    "pm-gpio-wake.md": "written against ove_gpio_open/ove_pm_state_set, which do not exist",
    "shell-command.md": "written against an OVE_SHELL_CMD_REGISTER/ove_shell_printf API",
    "uart-worker.md": "written against ove_uart_open/ove_uart_set_rx_callback",
}


def _blocks(text):
    """Yield (line, code) for each ```c block not marked as a fragment."""
    for m in _BLOCK_RE.finditer(text):
        line = text.count("\n", 0, m.start()) + 1
        before = text[:m.start()].rstrip("\n").rsplit("\n", 1)[-1].strip()
        if before == _FRAGMENT_MARK:
            continue
        yield line, m.group(1)


def _include_args(ove_dir):
    def inc(*parts):
        return "-I" + os.path.join(ove_dir, *parts)

    args = [inc("tests", "docs"), inc("include"), inc("tests", "host", "posix", "include")]
    if os.path.isfile(os.path.join(ove_dir, "dl", "lvgl", "lvgl.h")):
        args += [inc("dl", "lvgl"), inc("dl"), inc("tests", "host"), "-DLV_CONF_INCLUDE_SIMPLE"]
    else:
        args += [inc("tests", "backends", "stub", "lvgl"), inc("tests", "backends", "stub")]
    return args


def _compile(cc, args, code):
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write('#include "ove/ove.h"\n')
        f.write(code)
        path = f.name
    try:
        r = subprocess.run([cc, "-std=c11", "-fsyntax-only", "-Wall", "-Werror",
                            "-Wno-unused-function", "-Wno-unused-variable"] + args + [path],
                           capture_output=True, text=True)
    finally:
        os.unlink(path)
    out = (r.stdout + r.stderr).replace(path, "<snippet>")
    first = next((ln for ln in out.splitlines() if "error" in ln), out.strip()[:200])
    return r.returncode == 0, first


def check(ove_dir):
    """Returns (status, detail): status is "OK", "FAIL" or "SKIP"."""
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        return "SKIP", "no host C compiler"
    pages_dir = os.path.join(ove_dir, _PAGES_DIR)
    args = _include_args(ove_dir)
    problems = []
    compiled = 0
    for page in sorted(os.listdir(pages_dir)):
        if not page.endswith(".md"):
            continue
        with open(os.path.join(pages_dir, page), encoding="utf-8") as f:
            text = f.read()
        page_ok = True
        for line, code in _blocks(text):
            ok, err = _compile(cc, args, code)
            compiled += 1
            if not ok:
                page_ok = False
                if page not in _PENDING:
                    problems.append(f"{page}:{line}: {err}")
        if page in _PENDING and page_ok:
            problems.append(f"{page}: compiles now; remove it from _PENDING")
    if problems:
        return "FAIL", "; ".join(problems)
    return "OK", f"{compiled} blocks, {len(_PENDING)} pages pending"
