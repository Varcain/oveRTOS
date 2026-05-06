# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""`ove benchmarks <platform>` — build, run, and report.

Drives the full benchmark pipeline for one platform across all four
binding crates (C, C++, Rust, Zig):
  1. Configure + build each `<board>.<rtos>.benchmark{,_cpp,_rust,_zig}`.
  2. Run the binary (POSIX: spawn locally; STM32: flash via openocd, tail
     the serial log written by an out-of-band picocom session).
  3. Capture each run's stdout to a per-binding log under
     `output/<board>/<rtos>/_benchmarks/<binding>.log`.
  4. Run `scripts/bench_compare.py` to emit `report.md` next to those
     logs (cross-binding comparison + within-run wrapper-vs-native).

Supported platforms:
  - posix                     → board=host,     rtos=posix
  - stm32f746g-discovery      → board=stm32f746, rtos=freertos

Serial capture for STM32 reads `$OVE_SERIAL_LOG` (default
`/tmp/serial.log`) — pre-existing picocom session is expected to be
appending to it.
"""

import os
import re
import shutil
import subprocess
import sys
import time

from .utils import logger
from .workspace import find_ove_dir

OVE_DIR = find_ove_dir()


# Logical platform name -> (make-prefix, board-dir, rtos-dir, runner)
_PLATFORMS = {
    "posix": {
        "make_prefix": "host.posix",
        "board": "host",
        "rtos": "posix",
        "runner": "posix",
    },
    "stm32f746g-discovery": {
        "make_prefix": "stm32f746.freertos",
        "board": "stm32f746",
        "rtos": "freertos",
        "runner": "stm32_flash_serial",
    },
    "stm32f746g-discovery-nuttx": {
        "make_prefix": "stm32f746.nuttx",
        "board": "stm32f746",
        "rtos": "nuttx",
        "runner": "stm32_flash_serial",
    },
    "stm32f746g-discovery-zephyr": {
        "make_prefix": "stm32f746.zephyr",
        "board": "stm32f746",
        "rtos": "zephyr",
        "runner": "stm32_flash_serial",
    },
}

# config_name (== make app target) -> binding tag (== JSON `binding` field)
_BINDINGS_HEAP = [
    ("benchmark",      "c"),
    ("benchmark_cpp",  "cpp"),
    ("benchmark_rust", "rust"),
    ("benchmark_zig",  "zig"),
]
# Zero-heap variants — the `_zh` suffix matches the config_name in
# tests/benchmarks/<lang>/zeroheap/app.yaml.
_BINDINGS_ZH = [
    ("benchmark_zh",      "c"),
    ("benchmark_cpp_zh",  "cpp"),
    ("benchmark_rust_zh", "rust"),
    ("benchmark_zig_zh",  "zig"),
]

# Per-binding wall-clock cap on a single bench run.  STM32 was 180 s
# but the long-running suites (8 suites × 1000 iterations each + the
# native_<rtos> tail with another 14 cases) need ~9-10 min on Zig and
# Rust.  Bumped to 720 s with margin.  POSIX runs are short.
_RUN_TIMEOUT_S = {
    "posix": 90,
    "stm32_flash_serial": 720,
}


def _output_dir(board, rtos, mode="heap"):
    """Per-RTOS bench output dir.  mode in {"heap", "zeroheap"}; the zh
    output goes alongside the heap output so both can coexist."""
    suffix = "_benchmarks" if mode == "heap" else "_benchmarks_zeroheap"
    return os.path.join(OVE_DIR, "output", board, rtos, suffix)


def _docs_page_path(rtos, mode):
    """Path of the published per-RTOS docs page for this mode.

    Display-name of the RTOS in the page title comes from
    bench_compare.py's _PAGE_RTOS_META; the file basename here just
    needs to match the docs-site/docs/benchmarks/ layout."""
    return os.path.join(OVE_DIR, "docs-site", "docs", "benchmarks",
                        f"{rtos}-{mode}.md")


def _build_one(make_prefix, app):
    """Configure + build one (platform, binding) pair via make."""
    target = f"{make_prefix}.{app}"
    logger.info(f"=== building {target} ===")
    rc = subprocess.call(["make", target, "all"], cwd=OVE_DIR)
    if rc != 0:
        raise RuntimeError(f"build failed for {target} (rc={rc})")


def _firmware_paths(board, rtos, app):
    """Return (workspace_dir, image_path) for an already-built bench."""
    ws = os.path.join(OVE_DIR, "output", board, rtos, app)
    if rtos == "posix":
        image = os.path.join(ws, "images", "ove_posix")
    else:
        image = os.path.join(ws, "images", "firmware.elf")
    return ws, image


def _run_posix(image, log_path, timeout):
    """Spawn the POSIX bench binary, capture stdout, exit as soon as the
    bench prints its completion marker (the binary keeps the sim
    dashboard loop running indefinitely after the bench thread exits)."""
    logger.info(f"running {image} → {log_path}")
    deadline = time.time() + timeout
    proc = subprocess.Popen(
        [image],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    saw_complete = False
    try:
        with open(log_path, "wb") as f:
            assert proc.stdout is not None
            while time.time() < deadline:
                line = proc.stdout.readline()
                if not line:
                    break
                f.write(line)
                f.flush()
                if b"=== Benchmark complete ===" in line:
                    saw_complete = True
                    break
    finally:
        try:
            proc.kill()
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
    if not saw_complete:
        logger.warning(f"bench did not print completion marker before "
                       f"timeout ({timeout}s)")


def _serial_log_size(serial_log):
    try:
        return os.path.getsize(serial_log)
    except FileNotFoundError:
        return 0


def _wait_until_bench_complete(serial_log, binding, start_offset, deadline):
    """Block until the bench emits its final-suite JSON envelope
    *after* `start_offset` (so a stale completion marker from a
    previous boot of the same binding doesn't satisfy the wait)."""
    # Last suite emitted is the active RTOS's native baseline:
    #   - FreeRTOS: `native_freertos`
    #   - NuttX:    `native_nuttx`
    # All native_* suites are present in every binary but only one is
    # enabled per RTOS (others have case_count=0); the active one's
    # JSON envelope is what we wait for.  The regex matches whichever
    # one fires first, so this works on both backends without needing
    # the caller to thread the RTOS name through.
    end_re = re.compile(
        rb'"binding":"' + binding.encode() +
        rb'","suite":"native_(?:freertos|nuttx|zephyr)"'
    )
    while time.time() < deadline:
        try:
            with open(serial_log, "rb") as f:
                f.seek(start_offset)
                fresh = f.read()
        except FileNotFoundError:
            fresh = b""
        if end_re.search(fresh):
            return True
        time.sleep(2)
    return False


def _slice_latest_boot(serial_log, app, start_offset):
    """Extract the latest boot's bench output for the given app config
    from bytes after `start_offset`.  If the just-flashed run never
    printed its boot banner (e.g. crashed before init logging), returns
    whatever bytes accumulated post-flash so the failure is visible."""
    if not os.path.isfile(serial_log):
        return ""
    with open(serial_log, "rb") as f:
        f.seek(start_offset)
        fresh = f.read()
    banner = f"ove: starting {app} ".encode()
    idx = fresh.rfind(banner)
    if idx < 0:
        return fresh.decode(errors="replace")
    return fresh[idx:].decode(errors="replace")


def _run_stm32(image, app, log_path, timeout, binding, rtos="freertos"):
    """Flash to STM32 via openocd, wait for the bench to complete,
    snapshot the latest boot from the picocom serial log."""
    serial_log = os.environ.get("OVE_SERIAL_LOG", "/tmp/serial.log")
    if not shutil.which("openocd"):
        raise RuntimeError("openocd not in PATH")

    flash_sh = os.path.join(OVE_DIR, "boards", "stm32f746g-discovery",
                            rtos, "flash.sh")
    # Snapshot serial-log size before flash so the wait loop ignores
    # stale completion markers from previous boots of the same binding.
    start_offset = _serial_log_size(serial_log)
    logger.info(f"flashing {image} via {flash_sh}")
    rc = subprocess.call([flash_sh, image], cwd=OVE_DIR)
    if rc != 0:
        raise RuntimeError(f"flash failed (rc={rc})")

    deadline = time.time() + timeout
    logger.info(f"waiting up to {timeout}s for bench to complete "
                f"(tailing {serial_log} from offset {start_offset})…")
    if not _wait_until_bench_complete(serial_log, binding,
                                      start_offset, deadline):
        logger.warning("bench did not signal completion within timeout — "
                       "snapshotting whatever the serial log has anyway")

    text = _slice_latest_boot(serial_log, app, start_offset)
    with open(log_path, "w") as f:
        f.write(text)
    n = text.count("###BENCH_JSON_BEGIN")
    logger.info(f"captured {n} JSON suites from this run")


def _generate_report(out_dir, log_paths, runner=None, mode="heap",
                     rtos=None):
    """Run scripts/bench_compare.py against the per-binding logs.

    `--page-mode {heap,zeroheap}` is passed for STM32 runs so the
    generated report.md drops directly into the per-RTOS docs-site page
    without manual header fixups.  POSIX runs keep the generic header
    (cross-RTOS, not currently surfaced in the docs site).  Writes
    report.md both to out_dir (debug artifact for this run) and, on
    STM32 runs, to docs-site/docs/benchmarks/<rtos>-<mode>.md (the
    published location)."""
    script = os.path.join(OVE_DIR, "scripts", "bench_compare.py")
    report = os.path.join(out_dir, "report.md")
    cmd = [sys.executable, script,
           "--input", *log_paths,
           "--output", report]
    if runner == "stm32_flash_serial":
        cmd += ["--page-mode", mode]
    logger.info("=== generating report ===")
    rc = subprocess.call(cmd, cwd=OVE_DIR)
    if rc != 0:
        raise RuntimeError(f"bench_compare.py failed (rc={rc})")
    if runner == "stm32_flash_serial" and rtos:
        docs_page = _docs_page_path(rtos, mode)
        os.makedirs(os.path.dirname(docs_page), exist_ok=True)
        shutil.copyfile(report, docs_page)
        logger.info(f"docs page updated: {docs_page}")
    return report


def cmd_benchmarks(args):
    platform = args.platform
    if platform not in _PLATFORMS:
        sys.stderr.write(
            f"unknown platform '{platform}'; "
            f"choose one of: {', '.join(sorted(_PLATFORMS))}\n")
        sys.exit(2)

    p = _PLATFORMS[platform]
    binding_filter = getattr(args, "binding", None)
    mode = "zeroheap" if getattr(args, "zeroheap", False) else "heap"
    bindings = _BINDINGS_ZH if mode == "zeroheap" else _BINDINGS_HEAP
    out_dir = _output_dir(p["board"], p["rtos"], mode=mode)
    os.makedirs(out_dir, exist_ok=True)

    # When --binding selects a subset, run only those but include every
    # previously-captured log in the report so the comparison still
    # spans all 4 bindings.
    log_paths = []
    for app, binding in bindings:
        log_path = os.path.join(out_dir, f"{binding}.log")
        if binding_filter and binding not in binding_filter:
            if os.path.isfile(log_path):
                log_paths.append(log_path)
            continue
        if not args.skip_build:
            _build_one(p["make_prefix"], app)
        _, image = _firmware_paths(p["board"], p["rtos"], app)
        if not os.path.isfile(image):
            raise RuntimeError(f"firmware not found at {image} "
                               f"(build={p['make_prefix']}.{app})")
        timeout = _RUN_TIMEOUT_S[p["runner"]]
        if p["runner"] == "posix":
            _run_posix(image, log_path, timeout)
        elif p["runner"] == "stm32_flash_serial":
            _run_stm32(image, app, log_path, timeout, binding,
                       rtos=p["rtos"])
        else:
            raise RuntimeError(f"unknown runner '{p['runner']}'")
        log_paths.append(log_path)

    report = _generate_report(out_dir, log_paths, runner=p["runner"],
                              mode=mode, rtos=p["rtos"])
    print(f"\nReport: {report}")
    print(f"Logs:   {out_dir}/<binding>.log  (one per c/cpp/rust/zig)")
