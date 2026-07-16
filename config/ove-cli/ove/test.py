# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Test target management."""

import logging
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from .utils import run, nproc, apply_defconfig_overlay, atomic_symlink
from .manifest import load_manifest, get_component
from .workspace import find_ove_dir

logger = logging.getLogger("ove")


@dataclass
class TestResults:
    suite: str
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    failed_names: list = None

    def __post_init__(self):
        if self.failed_names is None:
            self.failed_names = []


def _parse_cmocka(stdout: str) -> TestResults:
    """Parse CMocka test output and return aggregated results.

    Sums every ``[  PASSED  ] N test(s).`` line into the passed total.
    For failures, CMocka emits a per-suite total in the form
    ``[  FAILED  ] tests: N test(s), listed below:`` followed by one
    ``[  FAILED  ] <test_name>`` per failing test — we count via the
    aggregate line so the count matches CMocka's own bookkeeping, then
    collect the individual names.  We avoid double-counting by
    skipping the count regex on lines that begin ``[  FAILED  ] tests:``.
    """
    passed = failed = 0
    failed_names = []
    for line in stdout.splitlines():
        m = re.match(r'\[\s+PASSED\s+\]\s+(\d+)\s+test', line)
        if m:
            passed += int(m.group(1))
            continue
        # Aggregate failure line: "[  FAILED  ] <group>: N test(s), listed
        # below:".  <group> is the cmocka array name (the macro stringifies
        # it) — almost always "tests", but match ANY name with \w+ so a
        # suite that renames its array still has its count summed instead of
        # falling through to the name regex below (which would capture the
        # group name and leave failed==0 — a silent pass on the sim paths
        # that can't fall back to a process exit code).
        m = re.match(r'\[\s+FAILED\s+\]\s+\w+:\s+(\d+)\s+test', line)
        if m:
            failed += int(m.group(1))
            continue
        # Older / alternative form: "[  FAILED  ] N test(s)" with no group prefix.
        m = re.match(r'\[\s+FAILED\s+\]\s+(\d+)\s+test', line)
        if m:
            failed += int(m.group(1))
            continue
        # Individual failure name (e.g. "[  FAILED  ] test_foo" or a
        # "[  FAILED  ] GROUP SETUP" error line) — no "<group>: N test" prefix.
        m = re.match(r'\[\s+FAILED\s+\]\s+([A-Za-z_]\w*)', line)
        if m:
            failed_names.append(m.group(1))
    return TestResults(suite="", passed=passed, failed=failed,
                       failed_names=failed_names)


def _summary_failures(stdout: str):
    """Return the firmware's self-reported group-failure count, or None.

    Every test firmware ends a *complete* run with a line of the form

        === Summary: N test group(s) had failures ===

    emitted by tests/stub_main.c and every per-sim main (QEMU/Renode/native
    NuttX/Zephyr) after the suite runner returns.  Its presence is proof the
    run reached the end; N is the firmware's own authoritative failure tally
    (it includes cmocka GROUP SETUP/TEARDOWN errors, which surface as
    ``total_errors`` and so never appear in the aggregate ``[  FAILED  ]
    tests: N`` line that _parse_cmocka counts).

    Returns the integer N, or None when the line is absent — i.e. an
    incomplete run (fault, hang, or early exit).  Used by the sim drivers
    that have no usable process exit code (Renode exits 0 unconditionally;
    the native NuttX sim idles until a timeout), where scraping
    ``[  PASSED  ]`` lines alone scores a partial run as a pass.

    The last occurrence wins so a reset-and-resume firmware (the HW
    watchdog suite reboots on boot #1) is scored on its final summary.
    """
    matches = re.findall(
        r'=== Summary:\s*(\d+)\s+test group\(s\) had failures ===', stdout)
    return int(matches[-1]) if matches else None


def _run_test_binary(cmd, suite, **kwargs) -> TestResults:
    """Run a test binary, print its output, and parse CMocka results."""
    result = subprocess.run(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, **kwargs)
    output = result.stdout or ""
    print(output, end="")
    parsed = _parse_cmocka(output)
    parsed.suite = suite
    if parsed.passed == 0 and parsed.failed == 0:
        # Binary produced no CMocka output — treat exit code as pass/fail
        if result.returncode == 0:
            parsed.passed = 1
        else:
            parsed.failed = 1
    if result.returncode != 0 and parsed.failed == 0:
        parsed.failed = 1
    return parsed


def _venv_env(ove_dir, base_env=None):
    """Return an env dict with .venv/bin and config/scripts prepended to PATH."""
    env = dict(base_env or os.environ)
    venv_bin = os.path.join(ove_dir, ".venv", "bin")
    scripts_bin = os.path.join(ove_dir, "config", "scripts")
    env["PATH"] = venv_bin + os.pathsep + scripts_bin + os.pathsep + env.get("PATH", "")
    return env


def _cmake_build(src_dir, build_dir, extra_args=None):
    """Configure + build a CMake project."""
    cmake = shutil.which("cmake") or "cmake"
    os.makedirs(build_dir, exist_ok=True)
    cmd = [cmake]
    if extra_args:
        cmd.extend(extra_args)
    cmd.append(src_dir)
    run(cmd, cwd=build_dir)
    run([cmake, "--build", build_dir, f"-j{nproc()}"])


def _ensure_kcov(ove_dir):
    """Ensure kcov is built locally, return path to its binary.

    Pinned to `libraries.kcov.version` in manifest.yaml. Source is cloned
    into `dl/kcov-<tag>` and installed under `output/tools/kcov-<tag>/`.
    Before invoking cmake we preflight the Ubuntu-side build deps and,
    if any are missing, print the exact apt-get one-liner and exit.
    """
    manifest = load_manifest(ove_dir)
    kcov_version = get_component(manifest, "libraries", "kcov", "version")
    kcov_url = get_component(manifest, "libraries", "kcov", "url")
    if not kcov_version or not kcov_url:
        logger.error("manifest.yaml missing libraries.kcov entry")
        sys.exit(1)

    install_dir = os.path.join(ove_dir, "output", "tools",
                               f"kcov-{kcov_version}")
    kcov_bin = os.path.join(install_dir, "bin", "kcov")
    if os.path.isfile(kcov_bin):
        return kcov_bin

    # Preflight: check required deps + tools. Keyed by Ubuntu package.
    # We check pkg-config first (authoritative on multi-arch layouts like
    # /usr/include/x86_64-linux-gnu/) and fall back to direct header paths
    # for packages that don't ship a .pc file (binutils-dev, libiberty-dev).
    required_tools = {
        "cmake":           "cmake",
        "pkg-config":      "pkg-config",
        "build-essential": "g++",
    }
    missing_pkgs = []
    for pkg, tool in required_tools.items():
        if shutil.which(tool) is None:
            missing_pkgs.append(pkg)

    pc = shutil.which("pkg-config")

    def _has_pc(module):
        if not pc:
            return False
        return subprocess.run([pc, "--exists", module]).returncode == 0

    def _has_header(name):
        # Try standard /usr/include first, then multi-arch /usr/include/<triplet>/.
        if os.path.isfile(f"/usr/include/{name}"):
            return True
        import glob as _glob
        return bool(_glob.glob(f"/usr/include/*-linux-*/{name}"))

    required_pkgconfig = {
        "libssl-dev":           "openssl",
        "libcurl4-openssl-dev": "libcurl",
        "libelf-dev":           "libelf",
        "zlib1g-dev":           "zlib",
        "libdw-dev":            "libdw",
    }
    for pkg, mod in required_pkgconfig.items():
        if not _has_pc(mod):
            missing_pkgs.append(pkg)

    required_headers = {
        "binutils-dev":  "bfd.h",
        "libiberty-dev": "libiberty/demangle.h",
    }
    for pkg, hdr in required_headers.items():
        if not _has_header(hdr):
            missing_pkgs.append(pkg)

    if missing_pkgs:
        apt_line = "sudo apt-get install -y " + " ".join(missing_pkgs)
        logger.error("kcov build requires packages not present on this host:")
        for pkg in missing_pkgs:
            logger.error("  - %s", pkg)
        logger.error("")
        logger.error("Install with:")
        logger.error("  %s", apt_line)
        sys.exit(1)

    dl_dir = os.path.join(ove_dir, "dl")
    os.makedirs(dl_dir, exist_ok=True)
    src_dir = os.path.join(dl_dir, f"kcov-{kcov_version}")
    if not os.path.isdir(src_dir):
        logger.info("Cloning kcov %s...", kcov_version)
        run(["git", "clone", "--depth", "1", "-b", kcov_version,
              kcov_url, src_dir])

    build_dir = os.path.join(ove_dir, "output", "tools",
                             f"kcov-{kcov_version}-build")
    logger.info("Building kcov %s", kcov_version)
    os.makedirs(build_dir, exist_ok=True)
    run(["cmake", "-S", src_dir, "-B", build_dir,
          f"-DCMAKE_INSTALL_PREFIX={install_dir}",
          "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", build_dir, "-j", str(nproc())])
    run(["cmake", "--install", build_dir])

    if not os.path.isfile(kcov_bin):
        logger.error("kcov build completed but binary not found at %s",
                     kcov_bin)
        sys.exit(1)
    return kcov_bin


def _ensure_arm_toolchain(ove_dir):
    """Download ARM toolchain + picolibc source if needed, return tc dir.

    Picolibc is the libc for FreeRTOS builds (the arm-gnu-toolchain ships
    only newlib).  Source is fetched here; the meson build runs at CMake
    configure time inside cmake/toolchains/arm-cortex-m7.cmake via
    cmake/PicolibcBuild.cmake.
    """
    from .download import download_toolchain, download_picolibc

    dl_dir = os.path.join(ove_dir, "dl")
    toolchains_dir = os.path.join(ove_dir, "output", "toolchains")
    os.makedirs(dl_dir, exist_ok=True)
    os.makedirs(toolchains_dir, exist_ok=True)

    manifest = load_manifest(ove_dir)

    # Picolibc source (small, idempotent — no-op if already cloned).
    if not download_picolibc({}, dl_dir, manifest=manifest):
        logger.error("picolibc source download failed")
        sys.exit(1)

    # Check sentinel first
    sentinel = os.path.join(toolchains_dir, "path.txt")
    if os.path.isfile(sentinel):
        tc_dir = Path(sentinel).read_text().strip()
        if os.path.isfile(os.path.join(tc_dir, "bin", "arm-none-eabi-gcc")):
            return tc_dir

    ok = download_toolchain({}, dl_dir, toolchains_dir, manifest=manifest)
    if not ok:
        logger.error("ARM toolchain download failed")
        sys.exit(1)

    tc_dir = Path(sentinel).read_text().strip()
    return tc_dir


def _zephyr_sdk_build_env(ove_dir, env=None):
    """Ensure the pinned Zephyr SDK and return an env for ARM Zephyr builds."""
    from .download import download_zephyr_sdk, zephyr_sdk_env

    dl_dir = os.path.join(ove_dir, "dl")
    toolchains_dir = os.path.join(ove_dir, "output", "toolchains")
    os.makedirs(dl_dir, exist_ok=True)
    os.makedirs(toolchains_dir, exist_ok=True)

    manifest = load_manifest(ove_dir)
    sdk_dir = download_zephyr_sdk(dl_dir, toolchains_dir, manifest=manifest)
    if sdk_dir is None:
        logger.error("Zephyr SDK unavailable")
        sys.exit(1)
    return zephyr_sdk_env(env or os.environ, sdk_dir)


def _ensure_stm32f746_renode_deps(ove_dir):
    """Clone STM32CubeF7 + lwIP into dl/ if missing.

    The renode-stm32f746-{freertos,freertos-zeroheap} test trees pull
    these directly from dl/ via `file(GLOB)` and `FATAL_ERROR` if
    absent. A configured workspace gets them via `download_freertos`;
    the test harness runs without a `.config` (CI hits these jobs on a
    bare checkout) so we mirror those fetches here, keyed off the same
    manifest entries.
    """
    from .download import git_clone
    from .manifest import get_component, load_manifest
    from .utils import hashed_dir
    dl_dir = os.path.join(ove_dir, "dl")
    os.makedirs(dl_dir, exist_ok=True)
    manifest = load_manifest(ove_dir)
    existing = os.listdir(dl_dir)

    if not any(n.startswith("STM32CubeF7-") for n in existing):
        url = get_component(manifest, "rtos", "freertos", "stm32cubef7", "url")
        tag = get_component(manifest, "rtos", "freertos", "stm32cubef7", "version")
        submodules = [
            "Drivers/STM32F7xx_HAL_Driver",
            "Drivers/CMSIS/Device/ST/STM32F7xx",
            "Drivers/BSP/STM32746G-Discovery",
            "Drivers/BSP/Components/Common",
            "Drivers/BSP/Components/wm8994",
            "Drivers/BSP/Components/ft5336",
            "Drivers/BSP/Components/stmpe811",
            "Drivers/BSP/Components/rk043fn48h",
        ]
        dest, _, _ = hashed_dir(dl_dir, "STM32CubeF7", tag, None)
        if not git_clone(url, tag, dest, "STM32CubeF7", submodules=submodules):
            logger.error("STM32CubeF7 download failed")
            sys.exit(1)

    if not any(n.startswith("lwip-") for n in existing):
        url = get_component(manifest, "libraries", "lwip", "url")
        tag = get_component(manifest, "libraries", "lwip", "version")
        if url and tag:
            dest, _, _ = hashed_dir(dl_dir, "lwip", tag, None)
            if not git_clone(url, tag, dest, "lwIP"):
                logger.error("lwIP download failed")
                sys.exit(1)


def test_stub(ove_dir, output_dir):
    """Build and run stub backend tests."""
    build = os.path.join(output_dir, "tests", "stub")
    logger.info("Building stub tests")
    _cmake_build(os.path.join(ove_dir, "tests"), build)
    logger.info("Running stub tests")
    return _run_test_binary([os.path.join(build, "ove_test_stub")], "stub")


def _sanitize_extra_args(flags):
    return [
        f"-DCMAKE_C_FLAGS={flags}",
        f"-DCMAKE_CXX_FLAGS={flags}",
        f"-DCMAKE_EXE_LINKER_FLAGS={flags}",
        "-DCMAKE_BUILD_TYPE=Debug",
    ]


def _stub_sanitize(ove_dir, output_dir, *, zero_heap):
    """Shared body for the heap and zero-heap UBSan+ASan stub runs."""
    label = "stub-sanitize-zh" if zero_heap else "stub-sanitize"
    build = os.path.join(output_dir, "tests",
                         "stub_sanitize_zh" if zero_heap else "stub_sanitize")
    sanitize_flags = (
        "-fsanitize=undefined,address "
        "-fno-omit-frame-pointer "
        "-fno-sanitize-recover=all"
    )
    # Zero-heap twin: define CONFIG_OVE_ZERO_HEAP so every object routes
    # through the static-storage *_init() APIs instead of the heap *_create()
    # path the default run exercises.  Passed as a -D inside CMAKE_C_FLAGS so
    # it reaches every translation unit — same effect as the
    # ove_test_stub_coverage_zh target in tests/CMakeLists.txt, but under
    # ASan+UBSan rather than gcov.
    if zero_heap:
        sanitize_flags += " -DCONFIG_OVE_ZERO_HEAP=1"
    mode = "zero-heap" if zero_heap else "heap"
    logger.info("Building C stub tests (%s) with -fsanitize=undefined,address",
                mode)
    _cmake_build(os.path.join(ove_dir, "tests"), build,
                 extra_args=_sanitize_extra_args(sanitize_flags))
    logger.info("Running C stub tests (%s) under sanitizers", mode)
    env = dict(os.environ)
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    env["ASAN_OPTIONS"] = "halt_on_error=1:detect_leaks=1:strict_string_checks=1"
    return _run_test_binary(
        [os.path.join(build, "ove_test_stub")], label, env=env)


def test_stub_sanitize(ove_dir, output_dir):
    """Build C-side stub tests with UBSan + ASan, then run them.

    Direct analog of `test_cpp_sanitize` and the Rust `make miri` job —
    catches UB at the substrate layer (uninitialized reads, OOB writes,
    use-after-free, signed overflow, alignment violations) on the host
    POSIX target without needing an embedded toolchain.
    """
    return _stub_sanitize(ove_dir, output_dir, zero_heap=False)


def test_stub_sanitize_zh(ove_dir, output_dir):
    """UBSan + ASan over the zero-heap (static-storage) stub build.

    The default sanitize run exercises only the heap *_create() path; this
    twin defines CONFIG_OVE_ZERO_HEAP so the *_init() static-storage APIs
    (mutex/sem/queue/timer/workqueue/stream/eventgroup/...) are checked for
    UAF/UB too — they otherwise get only -O0 gcov coverage, no sanitizer.
    """
    return _stub_sanitize(ove_dir, output_dir, zero_heap=True)


def test_stub_tsan(ove_dir, output_dir):
    """Build C-side stub tests with ThreadSanitizer, then run them.

    TSan catches data races (concurrent unsynchronized access to
    shared memory) that UBSan + ASan miss.  TSan is mutually exclusive
    with ASan (both override malloc) so it lives in its own build, and
    we explicitly turn off the tests/CMakeLists.txt asan-variant target
    that would otherwise pick up the thread flag and conflict.
    """
    build = os.path.join(output_dir, "tests", "stub_tsan")
    flags = (
        "-fsanitize=thread -fno-omit-frame-pointer -fno-sanitize-recover=all"
    )
    logger.info("Building C stub tests with -fsanitize=thread")
    extra = _sanitize_extra_args(flags)
    extra.append("-DOVE_TEST_BUILD_ASAN=OFF")
    _cmake_build(os.path.join(ove_dir, "tests"), build, extra_args=extra)
    logger.info("Running C stub tests under TSan")
    env = dict(os.environ)
    # report_thread_leaks=0: tests that intentionally let threads finish
    # without explicit join trip TSan's leak bookkeeping; that's a test-
    # harness artefact, not a real issue.  All race-detection categories
    # remain on.
    env["TSAN_OPTIONS"] = ("halt_on_error=1:second_deadlock_stack=1:"
                           "report_thread_leaks=0")
    return _run_test_binary(
        [os.path.join(build, "ove_test_stub")], "stub-tsan", env=env)


def test_stub_msan(ove_dir, output_dir):
    """Build C-side stub tests with MemorySanitizer, then run them.

    MSan catches uninitialized-memory reads — bugs UBSan/ASan don't
    see and `-Wmaybe-uninitialized` only catches at compile time when
    flow is statically obvious.  MSan requires every linked TU
    (including any C deps that read memory the test wrote) to be
    instrumented; on Ubuntu the system libc isn't, so suppressions
    are expected for libc internals.  C-only — libstdc++/libc++ are
    rarely msan-instrumented on stock distros, so we don't extend
    this to the C++ wrapper tests.
    """
    if not shutil.which("clang"):
        logger.warning("MSan requires clang; skipping test-stub-msan")
        return ("stub-msan", "SKIP", "clang not installed")
    build = os.path.join(output_dir, "tests", "stub_msan")
    flags = (
        "-fsanitize=memory -fsanitize-memory-track-origins=2 "
        "-fno-omit-frame-pointer -fno-sanitize-recover=all"
    )
    extra_args = [
        "-DCMAKE_C_COMPILER=clang",
        "-DCMAKE_CXX_COMPILER=clang++",
        f"-DCMAKE_C_FLAGS={flags}",
        f"-DCMAKE_CXX_FLAGS={flags}",
        f"-DCMAKE_EXE_LINKER_FLAGS={flags}",
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DOVE_TEST_BUILD_ASAN=OFF",
    ]
    logger.info("Building C stub tests with -fsanitize=memory (clang)")
    _cmake_build(os.path.join(ove_dir, "tests"), build, extra_args=extra_args)
    logger.info("Running C stub tests under MSan")
    env = dict(os.environ)
    # halt_on_error: any uninit-read aborts.  exit_code=1 ensures the test
    # runner sees a failure even if MSan would otherwise just warn.
    env["MSAN_OPTIONS"] = "halt_on_error=1:exit_code=1:print_stats=0"
    return _run_test_binary(
        [os.path.join(build, "ove_test_stub")], "stub-msan", env=env)


def test_cpp(ove_dir, output_dir):
    """Build and run C++ binding tests."""
    build = os.path.join(output_dir, "tests", "cpp")
    logger.info("Building C++ tests")
    _cmake_build(os.path.join(ove_dir, "tests", "cpp"), build)
    logger.info("Running C++ tests")
    return _run_test_binary([os.path.join(build, "ove_test_cpp")], "cpp")


def _cpp_sanitize(ove_dir, output_dir, *, zero_heap):
    """Shared body for the heap and zero-heap UBSan+ASan C++ runs."""
    label = "cpp-sanitize-zh" if zero_heap else "cpp-sanitize"
    build = os.path.join(output_dir, "tests",
                         "cpp_sanitize_zh" if zero_heap else "cpp_sanitize")
    sanitize_flags = (
        "-fsanitize=undefined,address "
        "-fno-omit-frame-pointer "
        "-fno-sanitize-recover=all"
    )
    # Zero-heap twin: define CONFIG_OVE_ZERO_HEAP so the C++ wrappers route
    # through the static-storage *_init() APIs instead of the heap path the
    # default run exercises.  Passed as a -D inside CMAKE_CXX_FLAGS (via
    # _sanitize_extra_args) so it reaches every TU — same effect as the
    # ove_test_cpp_coverage_zh target, but under ASan+UBSan rather than gcov.
    if zero_heap:
        sanitize_flags += " -DCONFIG_OVE_ZERO_HEAP=1"
    mode = "zero-heap" if zero_heap else "heap"
    logger.info("Building C++ tests (%s) with -fsanitize=undefined,address",
                mode)
    _cmake_build(os.path.join(ove_dir, "tests", "cpp"), build,
                 extra_args=_sanitize_extra_args(sanitize_flags))
    logger.info("Running C++ tests (%s) under sanitizers", mode)
    env = dict(os.environ)
    # halt_on_error: any sanitizer hit aborts immediately so the test
    # runner sees a non-zero exit.  detect_leaks: catch leaks at exit.
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    env["ASAN_OPTIONS"] = "halt_on_error=1:detect_leaks=1:strict_string_checks=1"
    return _run_test_binary(
        [os.path.join(build, "ove_test_cpp")], label, env=env)


def test_cpp_sanitize(ove_dir, output_dir):
    """Build C++ tests with UBSan + ASan, then run them.

    Direct analog of the Rust `make miri` job — catches binding-side UB
    (uninitialized reads, use-after-free, alignment violations, integer
    overflow under signed-overflow rules) on the host POSIX target,
    without needing an embedded toolchain.

    The firmware-side `OVE_CXX_NOEXCEPT_NORTTI` build profile does not
    apply here — host tests build with full C++ runtime so sanitizer
    libs link cleanly.
    """
    return _cpp_sanitize(ove_dir, output_dir, zero_heap=False)


def test_cpp_sanitize_zh(ove_dir, output_dir):
    """UBSan + ASan over the zero-heap (static-storage) C++ build.

    The default C++ sanitize run exercises only the heap path; this twin
    defines CONFIG_OVE_ZERO_HEAP so the binding's *_init() static-storage
    constructors are checked for UAF/UB too — they otherwise get only -O0
    gcov coverage (ove_test_cpp_coverage_zh), no sanitizer.  C analog:
    test_stub_sanitize_zh.
    """
    return _cpp_sanitize(ove_dir, output_dir, zero_heap=True)


def test_cpp_tsan(ove_dir, output_dir):
    """Build C++ tests with ThreadSanitizer, then run them.

    Companion to test_stub_tsan — catches races in the C++ RAII wrapper
    layer (e.g. a Mutex moved across threads, an Event whose `signal()`
    races with `wait()` due to a missed memory barrier in a stub).
    Mutually exclusive with ASan; lives in its own build dir.
    """
    build = os.path.join(output_dir, "tests", "cpp_tsan")
    flags = (
        "-fsanitize=thread -fno-omit-frame-pointer -fno-sanitize-recover=all"
    )
    logger.info("Building C++ tests with -fsanitize=thread")
    _cmake_build(os.path.join(ove_dir, "tests", "cpp"), build,
                 extra_args=_sanitize_extra_args(flags))
    logger.info("Running C++ tests under TSan")
    env = dict(os.environ)
    env["TSAN_OPTIONS"] = ("halt_on_error=1:second_deadlock_stack=1:"
                           "report_thread_leaks=0")
    return _run_test_binary(
        [os.path.join(build, "ove_test_cpp")], "cpp-tsan", env=env)


def _rust_test_env(ove_dir, output_dir, target_dir, *, zero_heap=False):
    """Return (rust_dir, env) set up to build tests/rust/ against the
    rust_stub CMake library. test_rust, test_rust_coverage and
    test_rust_zeroheap use this.

    zero_heap=True builds the stub with CONFIG_OVE_ZERO_HEAP and points
    OVE_GEN_DIR at a generated ove_config.h carrying that define, so both the
    ove crate and the test crate light up their zero-heap (`zero_heap` cfg)
    static-storage paths against a matching stub. build.rs text-greps the
    config, so the define must be literal (not pulled in via #include)."""
    suffix = "_zh" if zero_heap else ""
    stub_build = os.path.join(output_dir, "tests", "rust_stub" + suffix)
    _cmake_build(
        os.path.join(ove_dir, "tests", "rust", "stub_cmake"),
        stub_build,
        extra_args=(["-DOVE_STUB_ZERO_HEAP=ON"] if zero_heap else None))

    gen_dir = os.path.join(ove_dir, "tests")
    if zero_heap:
        gen_dir = os.path.join(output_dir, "tests", "rust_zh_gen")
        os.makedirs(gen_dir, exist_ok=True)
        with open(os.path.join(ove_dir, "tests", "ove_config.h")) as f:
            base_cfg = f.read()
        zh_cfg = base_cfg.replace(
            "#endif /* OVE_CONFIG_H */",
            "#define CONFIG_OVE_ZERO_HEAP 1\n\n#endif /* OVE_CONFIG_H */")
        with open(os.path.join(gen_dir, "ove_config.h"), "w") as f:
            f.write(zh_cfg)

    rust_dir = os.path.join(ove_dir, "tests", "rust")
    env = dict(os.environ)
    env.update({
        "OVE_DIR": ove_dir,
        "OVE_GEN_DIR": gen_dir,
        "RUST_IS_NATIVE": "1",
        "STUB_LIB_DIR": stub_build,
        "OVE_STORAGE_SIZES": os.path.join(stub_build,
                                               "ove_storage_sizes.env"),
        "LV_CONF_PATH": os.path.join(ove_dir, "boards", "host",
                                      "posix"),
        "LVGL_INCLUDE_PATH": os.path.join(ove_dir, "tests", "backends",
                                           "stub", "lvgl"),
        "LVGL_PARENT_PATH": os.path.join(ove_dir, "tests", "backends",
                                          "stub"),
        "CARGO_TARGET_DIR": target_dir,
    })
    env["RUSTFLAGS"] = (env.get("RUSTFLAGS", "").strip() +
                        " --cfg ove_test").strip()
    return rust_dir, env


def test_rust(ove_dir, output_dir):
    """Build and run Rust tests."""
    target_dir = os.path.join(output_dir, "tests", "rust")
    logger.info("Building Rust stub library")
    rust_dir, env = _rust_test_env(ove_dir, output_dir, target_dir)
    logger.info("Building Rust tests")
    run(["cargo", "build", "--release"], env=env, cwd=rust_dir)
    logger.info("Running Rust tests")
    return _run_test_binary(
        [os.path.join(target_dir, "release", "ove-tests")], "rust")


def test_rust_zeroheap(ove_dir, output_dir):
    """Build and run the Rust tests in zero-heap (static-storage) mode.

    Exercises the binding's `create(&STORAGE, …)` / `from_static` paths (the
    test_zero_heap suite, gated `#[cfg(zero_heap)]`) that the default heap-mode
    `test_rust` compiles out — the heap suites use `::new()` constructors that
    don't exist under CONFIG_OVE_ZERO_HEAP and are themselves gated off."""
    target_dir = os.path.join(output_dir, "tests", "rust_zh")
    logger.info("Building Rust stub library (zero-heap)")
    rust_dir, env = _rust_test_env(ove_dir, output_dir, target_dir,
                                   zero_heap=True)
    logger.info("Building Rust tests (zero-heap)")
    run(["cargo", "build", "--release"], env=env, cwd=rust_dir)
    logger.info("Running Rust tests (zero-heap)")
    return _run_test_binary(
        [os.path.join(target_dir, "release", "ove-tests")], "rust-zeroheap")


def test_rust_coverage(ove_dir, output_dir):
    """Build and run Rust tests under LLVM source-based coverage, emit lcov.

    Rust uses a different instrumentation format (.profraw) than GCC gcov.
    We produce an lcov-format tracefile so the top-level `make coverage`
    pipeline can merge it with the C/C++ gcov data. Output files:
      <cov_dir>/profraw/coverage-*.profraw  — raw per-process counters
      <cov_dir>/coverage.profdata           — merged counters
      <cov_dir>/coverage.filtered.info      — lcov format, filtered to oveRTOS
    """
    cov_dir = os.path.join(output_dir, "tests", "rust_coverage")
    target_dir = os.path.join(cov_dir, "target")
    profraw_dir = os.path.join(cov_dir, "profraw")
    os.makedirs(profraw_dir, exist_ok=True)

    logger.info("Building Rust stub library (coverage)")
    rust_dir, env = _rust_test_env(ove_dir, output_dir, target_dir)
    # A fresh build with instrumentation — existing release artifacts in
    # output/tests/rust don't have the profile sections we need.
    env["RUSTFLAGS"] = env.get("RUSTFLAGS", "") + " -C instrument-coverage"
    env["LLVM_PROFILE_FILE"] = os.path.join(
        profraw_dir, "coverage-%p-%m.profraw")

    logger.info("Building Rust tests (coverage)")
    run(["cargo", "build", "--release"], env=env, cwd=rust_dir)
    logger.info("Running Rust tests (coverage)")
    binary = os.path.join(target_dir, "release", "ove-tests")
    result = _run_test_binary([binary], "rust", env=env)

    # Also exercise the zero-heap suite under the same instrumentation so the
    # binding's static-storage create()/from_static paths are counted — the C
    # stub coverage runs heap + zero-heap twins for the same reason.  Its
    # .profraw lands in the same profraw_dir (distinct name → merged below),
    # and its binary is passed to llvm-cov as a second `-object`.
    zh_target = os.path.join(cov_dir, "target_zh")
    _, zh_env = _rust_test_env(ove_dir, output_dir, zh_target, zero_heap=True)
    zh_env["RUSTFLAGS"] = zh_env.get("RUSTFLAGS", "") + " -C instrument-coverage"
    zh_env["LLVM_PROFILE_FILE"] = os.path.join(
        profraw_dir, "coverage-zh-%p-%m.profraw")
    logger.info("Building Rust tests (coverage, zero-heap)")
    run(["cargo", "build", "--release"], env=zh_env, cwd=rust_dir)
    logger.info("Running Rust tests (coverage, zero-heap)")
    zh_binary = os.path.join(zh_target, "release", "ove-tests")
    zh_result = _run_test_binary([zh_binary], "rust-zeroheap", env=zh_env)
    # Fold the zero-heap verdict into the coverage run's result.
    result.failed += zh_result.failed
    result.failed_names += zh_result.failed_names

    # Rust's raw profile format moves with the compiler's bundled LLVM; the
    # system llvm-profdata will frequently be older and reject the .profraw.
    # Prefer the toolchain-bundled tools under $(rustc --print sysroot).
    try:
        sysroot = subprocess.check_output(
            ["rustc", "--print", "sysroot"], text=True).strip()
    except Exception:
        sysroot = ""
    rust_bin = ""
    if sysroot:
        import glob
        matches = glob.glob(os.path.join(
            sysroot, "lib", "rustlib", "*", "bin", "llvm-profdata"))
        if matches:
            rust_bin = os.path.dirname(matches[0])
    llvm_profdata = (os.path.join(rust_bin, "llvm-profdata") if rust_bin
                     else shutil.which("llvm-profdata"))
    llvm_cov = (os.path.join(rust_bin, "llvm-cov") if rust_bin
                else shutil.which("llvm-cov"))
    if not llvm_profdata or not os.path.isfile(llvm_profdata) \
            or not llvm_cov or not os.path.isfile(llvm_cov):
        logger.error("llvm-profdata/llvm-cov not found — install with "
                     "`rustup component add llvm-tools-preview`")
        return result

    profdata = os.path.join(cov_dir, "coverage.profdata")
    import glob
    profraws = sorted(glob.glob(os.path.join(profraw_dir, "*.profraw")))
    if not profraws:
        logger.error("no .profraw files produced — did the binary run?")
        return result
    run([llvm_profdata, "merge", "-sparse", *profraws, "-o", profdata])

    info = os.path.join(cov_dir, "coverage.info")
    with open(info, "w") as f:
        subprocess.run(
            [llvm_cov, "export", "--format=lcov",
             "--instr-profile=" + profdata, binary, "-object", zh_binary],
            stdout=f, check=True)

    filtered = os.path.join(cov_dir, "coverage.filtered.info")
    _lcov_filter_ove_sources("lcov", info, filtered, ove_dir,
                             extra_ignore=["format"])
    logger.info("Rust coverage: %s", filtered)
    return result


def _find_zig(ove_dir):
    """Find the zig binary — toolchain dir first, download if needed, then PATH."""
    import glob
    pattern = os.path.join(ove_dir, "output", "toolchains", "zig-*", "zig")
    matches = glob.glob(pattern)
    if matches:
        return matches[0]

    # Download the toolchain into output/toolchains like the example build does
    from .download import download_zig_toolchain
    manifest = load_manifest(ove_dir)
    dl_dir = os.path.join(ove_dir, "dl")
    toolchains_dir = os.path.join(ove_dir, "output", "toolchains")
    os.makedirs(dl_dir, exist_ok=True)
    os.makedirs(toolchains_dir, exist_ok=True)
    if download_zig_toolchain({}, dl_dir, toolchains_dir, manifest=manifest):
        matches = glob.glob(pattern)
        if matches:
            return matches[0]

    zig = shutil.which("zig")
    if zig:
        return zig
    logger.error("zig compiler not found")
    sys.exit(1)


def _build_zig_test_binary(ove_dir, output_dir, *, debug=False,
                           output_subdir=None, zero_heap=False):
    """Build tests/zig/main.zig into an executable; return (exe_path, cwd).

    Shared by test_zig, test_zig_coverage, and test_zig_debug.  When
    `debug=True` we pass `-ODebug` so kcov's DWARF-driven source
    attribution produces useful line-level data and Zig's safety
    checks stay live across the whole binary.  `output_subdir` lets a
    caller distinguish between debug builds with different intents
    (coverage vs plain debug-mode test) so they don't clobber each
    other's artefacts under output/tests/.
    """
    stub_build = os.path.join(output_dir, "tests",
                              "zig_stub_zh" if zero_heap else "zig_stub")
    logger.info("Building Zig stub library%s",
                " (zero-heap)" if zero_heap else "")
    _cmake_build(
        os.path.join(ove_dir, "tests", "rust", "stub_cmake"),
        stub_build,
        extra_args=(["-DOVE_STUB_ZERO_HEAP=ON"] if zero_heap else None))

    # Zero-heap: generate an ove_config.h carrying CONFIG_OVE_ZERO_HEAP and put
    # its dir first on the include path so the ove module's @cImport sees it —
    # `pin.zero_heap = @hasDecl(c, "CONFIG_OVE_ZERO_HEAP")` then flips on, and
    # the primitives' create() route through the static-storage *_init() APIs.
    zh_include = []
    if zero_heap:
        zh_dir = os.path.join(output_dir, "tests", "zig_zh_gen")
        os.makedirs(zh_dir, exist_ok=True)
        with open(os.path.join(ove_dir, "tests", "ove_config.h")) as f:
            base_cfg = f.read()
        zh_cfg = base_cfg.replace(
            "#endif /* OVE_CONFIG_H */",
            "#define CONFIG_OVE_ZERO_HEAP 1\n\n#endif /* OVE_CONFIG_H */")
        with open(os.path.join(zh_dir, "ove_config.h"), "w") as f:
            f.write(zh_cfg)
        # storage.h's @ZIG_CIMPORT zero-heap path embeds each storage struct by
        # size, so it needs OVE_SIZEOF_*/OVE_ALIGNOF_* defines.  The stub build
        # emits ove_storage_sizes.env (KEY=VALUE); convert to `#define OVE_KEY
        # VALUE` (same mapping as config/cmake/ove_zig.cmake's generator).
        with open(os.path.join(stub_build, "ove_storage_sizes.env")) as f:
            sizes = f.read()
        with open(os.path.join(zh_dir, "zig_storage_sizes.h"), "w") as out:
            out.write("/* Auto-generated storage sizes for the zero-heap Zig "
                      "test build. */\n")
            for line in sizes.splitlines():
                line = line.strip()
                if "=" in line and not line.startswith("#"):
                    key, val = line.split("=", 1)
                    out.write(f"#define OVE_{key} {val}\n")
        zh_include = ["-I" + zh_dir]

    zig = _find_zig(ove_dir)
    zig_test_dir = os.path.join(ove_dir, "tests", "zig")
    zig_bindings = os.path.join(ove_dir, "bindings", "zig", "ove",
                                "src", "root.zig")
    if output_subdir is None:
        output_subdir = ("zig_coverage" if debug
                         else "zig_zh" if zero_heap else "zig")
    zig_output = os.path.join(output_dir, "tests", output_subdir)
    os.makedirs(zig_output, exist_ok=True)
    zig_exe = os.path.join(zig_output, "ove_test_zig")

    include_args = zh_include + [
        "-I" + os.path.join(ove_dir, "include"),
        "-I" + os.path.join(ove_dir, "tests"),
        "-I" + os.path.join(ove_dir, "backends", "posix", "include"),
        "-I" + os.path.join(ove_dir, "tests", "backends", "stub", "lvgl"),
        "-I" + os.path.join(ove_dir, "tests", "backends", "stub"),
        "-I" + os.path.join(ove_dir, "boards", "host", "posix"),
    ]

    logger.info("Building Zig tests%s",
                " (debug/coverage)" if debug else "")
    opt_flag = "-ODebug" if debug else "-OReleaseSafe"
    cmd = [
        zig, "build-exe",
        "-target", "native-native-gnu",
        opt_flag,
        "--dep", "ove",
        "-Mroot=" + os.path.join(zig_test_dir, "main.zig"),
        "-Move=" + zig_bindings,
    ] + include_args + [
        "-L" + stub_build, "-love_stub",
        "-lpthread", "-lrt",
        "-lc",
        "-femit-bin=" + zig_exe,
    ]
    # Zig 0.14/0.15 defaulted to a self-hosted x86_64 Debug backend whose
    # DWARF was unreadable to kcov (issue #25368), so we used to force
    # `-fllvm`.  Verified 0.16 emits kcov-attributable DWARF directly;
    # the workaround was retired in the 0.16 bump.
    run(cmd, cwd=zig_test_dir)
    return zig_exe, zig_test_dir


def test_zig(ove_dir, output_dir):
    """Build and run Zig binding tests."""
    zig_exe, _ = _build_zig_test_binary(ove_dir, output_dir)
    logger.info("Running Zig tests")
    return _run_test_binary([zig_exe], "zig")


def test_zig_zeroheap(ove_dir, output_dir):
    """Build and run the Zig binding tests in zero-heap (static-storage) mode.

    The Zig primitives' `create(allocator)` already allocate the storage from
    the caller's allocator and call the static `ove_*_init()` APIs, so the
    core suites run unchanged with CONFIG_OVE_ZERO_HEAP defined.  Suites using
    zero-heap-incompatible type variants (audio Graph buf-storage, net, infer)
    are `comptime`-gated off in main.zig via `ove.pin.zero_heap`."""
    zig_exe, _ = _build_zig_test_binary(ove_dir, output_dir, zero_heap=True)
    logger.info("Running Zig tests (zero-heap)")
    return _run_test_binary([zig_exe], "zig-zeroheap")


def test_zig_debug(ove_dir, output_dir):
    """Build and run Zig binding tests under -ODebug.

    ReleaseSafe (the default for `make test-zig`) keeps integer-overflow,
    bounds, alignment, and unreachable-code checks but folds them as
    branches the optimizer may dead-code if it proves safety.  Debug mode
    keeps every check live and additionally enables the LLVM-level
    sanitizer hooks Zig's stdlib emits — useful for catching defects in
    paths that ReleaseSafe optimization may have erased.

    This is the Zig analog of the C/C++ `*-sanitize` jobs: a separate
    test mode whose CI signal is "the safety net is intact" rather than
    "the production build works."
    """
    zig_exe, _ = _build_zig_test_binary(ove_dir, output_dir, debug=True,
                                        output_subdir="zig_debug")
    logger.info("Running Zig tests (Debug mode)")
    return _run_test_binary([zig_exe], "zig-debug")


def _cobertura_to_lcov(xml_path, out_path):
    """Convert a kcov-produced cobertura.xml to an lcov tracefile.

    kcov v43 doesn't emit lcov natively — it writes cobertura.xml, JSON,
    and HTML. We walk the XML and produce `SF:` / `DA:` / `LF:` / `LH:`
    records so the result can be merged with the other backends'
    tracefiles via `lcov --add-tracefile`. Returns the number of source
    files emitted.
    """
    import xml.etree.ElementTree as ET
    tree = ET.parse(xml_path)
    root = tree.getroot()
    # Cobertura stores file paths relative to <sources>/<source>; take the
    # first entry (kcov emits one) so the resulting SF: records are
    # absolute and genhtml can locate them.
    source_prefix = ""
    src_el = root.find("./sources/source")
    if src_el is not None and src_el.text:
        source_prefix = src_el.text.rstrip("/") + "/"
    files_written = 0
    with open(out_path, "w") as out:
        for cls in root.iter("class"):
            filename = cls.get("filename")
            lines_el = cls.find("lines")
            if not filename or lines_el is None:
                continue
            entries = [(int(ln.get("number")), int(ln.get("hits", "0")))
                       for ln in lines_el.iter("line")
                       if ln.get("number")]
            if not entries:
                continue
            if not os.path.isabs(filename):
                filename = source_prefix + filename
            out.write(f"SF:{filename}\n")
            hits = 0
            for number, count in entries:
                out.write(f"DA:{number},{count}\n")
                if count > 0:
                    hits += 1
            out.write(f"LF:{len(entries)}\n")
            out.write(f"LH:{hits}\n")
            out.write("end_of_record\n")
            files_written += 1
    return files_written


def test_zig_coverage(ove_dir, output_dir):
    """Build and run Zig binding tests under kcov; emit lcov tracefile.

    Zig has no native source-based coverage; we wrap the debug-built
    test binary with kcov (locally built per manifest, see _ensure_kcov).
    kcov walks DWARF via ptrace and writes cobertura.xml + HTML; we
    convert to lcov so the result merges with the other backends.
    """
    kcov = _ensure_kcov(ove_dir)
    # Heap build only — intentionally NO zero-heap coverage twin here (unlike
    # the C stub's coverage_zh and rust-coverage's zero-heap pass).  For Zig
    # the primitives' `create(allocator)` is mode-agnostic: it always allocates
    # storage from the caller's allocator and calls the static `ove_*_init()`,
    # so the static-storage path is identical in both modes and already covered
    # by this heap run.  The only genuinely zero-heap-only Zig code is the
    # `ZeroHeap*` type variants (Watchdog/infer.Model/net.*), which the
    # zero-heap suite (test_zig_zeroheap) gates OUT — so a zero-heap kcov pass
    # would merely add those untested variants at 0%, lowering the number
    # without covering anything new.  (Rust differed: its `from_static` is
    # distinct binding code the heap run never touched, hence rust-coverage's
    # zero-heap pass.)
    zig_exe, _ = _build_zig_test_binary(ove_dir, output_dir, debug=True)

    cov_dir = os.path.join(output_dir, "tests", "zig_coverage", "kcov")
    shutil.rmtree(cov_dir, ignore_errors=True)
    os.makedirs(cov_dir, exist_ok=True)

    logger.info("Running Zig tests under kcov")
    # Ziggit #3421 recipe: --include-pattern (substring match) works where
    # --include-path (absolute paths) can yield empty reports for Zig binaries.
    result = _run_test_binary(
        [kcov, "--include-pattern=bindings/zig/ove/src",
         cov_dir, zig_exe], "zig-coverage")

    import glob as _glob
    candidates = _glob.glob(os.path.join(cov_dir, "ove_test_zig*",
                                          "cobertura.xml"))
    if not candidates:
        logger.error("kcov did not emit cobertura.xml under %s", cov_dir)
        return result
    cobertura = candidates[0]

    raw_lcov = os.path.join(output_dir, "tests", "zig_coverage",
                            "coverage.info")
    n_files = _cobertura_to_lcov(cobertura, raw_lcov)

    filtered = os.path.join(output_dir, "tests", "zig_coverage",
                            "coverage.filtered.info")
    lcov = shutil.which("lcov")
    if n_files > 0 and lcov:
        _lcov_filter_ove_sources(lcov, raw_lcov, filtered, ove_dir,
                                 extra_ignore=["format"])
    else:
        shutil.copy(raw_lcov, filtered)

    if n_files == 0:
        logger.warning(
            "kcov attributed 0 Zig source files — kcov's DWARF parser may "
            "have regressed against the current Zig codegen.  If reverting "
            "to LLVM via `-fllvm` in `_build_zig_test_binary` restores "
            "attribution, that's the workaround (cf. Zig issue #25368).  "
            "Emitted empty tracefile: %s", filtered)
    else:
        logger.info("Zig coverage: %s (%d files)", filtered, n_files)
    return result


def _lcov_filter_ove_sources(lcov, raw, filtered, ove_dir, *,
                             extra_ignore=None):
    """Reduce a raw lcov tracefile to oveRTOS source files only.

    We allowlist (--extract) the four first-party directories instead of
    denylisting (--remove) known-bad paths. This catches every source of
    pollution by construction — RTOS internals (Zephyr's posix arch, NuttX
    libs, FreeRTOS kernel), SDK libc headers (e.g. zephyr-sdk-*/
    arm-zephyr-eabi/sys-include), CMocka, generated files in build
    artifacts, and any new third-party code that lands in dl/ later.
    """
    # Canonicalize — lcov matches --extract patterns literally against the
    # tracefile's SF: paths, which are already canonical. A stray `..` in
    # ove_dir would silently match nothing.
    ove_dir = os.path.realpath(ove_dir)
    patterns = [
        f"{ove_dir}/src/*",
        f"{ove_dir}/backends/*",
        f"{ove_dir}/bindings/*",
        f"{ove_dir}/include/*",
    ]
    ignore = ["unused", "empty", "inconsistent"]
    if extra_ignore:
        ignore.extend(extra_ignore)
    run([lcov, "--extract", raw, *patterns,
         "--rc", "branch_coverage=1",
         "--output-file", filtered,
         "--ignore-errors", ",".join(ignore)])


def _clean_gcda(root):
    """Remove stale .gcda under `root` before a coverage run. Leftover
    counters from a previous run (or a previous coverage variant) would
    accumulate into the next merge and inflate the line-hit numbers.
    `lcov --capture --directory <root>` walks the whole tree, so we
    sweep the whole tree to match."""
    removed = 0
    for dirpath, _, files in os.walk(root):
        for f in files:
            if f.endswith(".gcda"):
                try:
                    os.unlink(os.path.join(dirpath, f))
                    removed += 1
                except OSError:
                    pass
    if removed:
        logger.debug("Removed %d stale .gcda file(s) under %s", removed, root)


def _nuttx_fetch_sources(ove_dir):
    """Fetch the manifest-pinned NuttX kernel + apps + CMocka into dl/.

    Returns (nuttx_src, apps_dl, cmocka_dl) — absolute paths to the cached
    upstream trees. The kernel tree is used directly as a CMake source dir
    (CMake builds out-of-tree, so it stays clean); the apps tree is the
    pristine cache that callers copy into a per-variant build dir to
    register external/ove_test/ without cross-variant interference."""
    import hashlib
    dl_dir = os.path.join(ove_dir, "dl")
    manifest = load_manifest(ove_dir)
    tag = get_component(manifest, "rtos", "nuttx", "kernel", "version")
    nuttx_url = get_component(manifest, "rtos", "nuttx", "kernel", "url")
    apps_url = get_component(manifest, "rtos", "nuttx", "apps", "url")
    tag_hash = hashlib.sha256(tag.encode()).hexdigest()[:8]

    nuttx_hash = os.path.join(dl_dir, f"nuttx-{tag_hash}")
    if not os.path.isdir(nuttx_hash):
        logger.debug(f"Cloning NuttX {tag}...")
        run(["git", "clone", "--depth", "1", "-b", tag, nuttx_url,
             nuttx_hash])
    nuttx_link = os.path.join(dl_dir, "nuttx")
    atomic_symlink(nuttx_hash, nuttx_link)

    apps_hash = os.path.join(dl_dir, f"nuttx-apps-{tag_hash}")
    if not os.path.isdir(apps_hash):
        logger.debug(f"Cloning NuttX apps {tag}...")
        run(["git", "clone", "--depth", "1", "-b", tag, apps_url,
             apps_hash])
    apps_link = os.path.join(dl_dir, "nuttx-apps")
    atomic_symlink(apps_hash, apps_link)

    cmocka_dl = os.path.join(dl_dir, "cmocka")
    if not os.path.isdir(cmocka_dl):
        logger.debug("Cloning CMocka...")
        run(["git", "clone", "--depth", "1", "-b", "cmocka-1.1.7",
             "https://gitlab.com/cmocka/cmocka.git", cmocka_dl])

    return nuttx_hash, apps_hash, cmocka_dl


def _nuttx_variant_config_dir(ove_dir, app_subdir):
    """Resolve the per-variant tests/sim/<dir>/ that holds ove_config.h.

    Heap variants share tests/sim/nuttx/ove_config.h (it's RTOS-keyed:
    CONFIG_OVE_RTOS_NUTTX=1, no zero-heap define). Zero-heap variants
    have their own ove_config.h next to the variant's nuttx_app
    fixture. Falling back to tests/sim/nuttx/ when the variant dir has
    no ove_config.h matches what the legacy Makefiles did via
    `CFLAGS += -I$(OVE_DIR)/tests/sim/nuttx`."""
    candidate = os.path.join(ove_dir, "tests", "sim", app_subdir)
    if os.path.isfile(os.path.join(candidate, "ove_config.h")):
        return candidate
    return os.path.join(ove_dir, "tests", "sim", "nuttx")


def _nuttx_register_test_app(ove_dir, apps_build, app_subdir):
    """Stage tests/sim/<app_subdir>/nuttx_app as nuttx-apps/external/ove_test
    in `apps_build`. Drops both an `add_subdirectory(ove_test)` CMakeLists
    and an `external/Kconfig` source line so the kernel's CMake-driven
    Kconfig sees CONFIG_EXTERNAL_OVE_TEST."""
    ext_dir = os.path.join(apps_build, "external")
    os.makedirs(ext_dir, exist_ok=True)
    test_dest = os.path.join(ext_dir, "ove_test")
    if os.path.exists(test_dest):
        shutil.rmtree(test_dest)
    shutil.copytree(
        os.path.join(ove_dir, "tests", "sim", app_subdir, "nuttx_app"),
        test_dest)
    with open(os.path.join(ext_dir, "Kconfig"), "w") as f:
        f.write('source "$APPSDIR/external/ove_test/Kconfig"\n')
    with open(os.path.join(ext_dir, "CMakeLists.txt"), "w") as f:
        f.write('add_subdirectory(ove_test)\n')


def _nuttx_cmake_build(ove_dir, *, nuttx_src, apps_build, build_dir,
                       board_config, defconfig_overlays, coverage,
                       variant_dir, extra_env=None):
    """Configure + build a NuttX target via CMake. Two-phase to thread our
    test defconfig overlays through Kconfig:

      1. cmake configure → generates build_dir/.config from the board
         defconfig + olddefconfig.
      2. apply_defconfig_overlay() + olddefconfig (kconfiglib) merges our
         overlays into .config.
      3. cmake re-configure → nuttx_mkconfig sees .config != .config.prev
         and regenerates include/nuttx/config.h, then cmake --build runs
         the kernel + libapps build.

    All build artifacts (objects, dep files, .gcno/.gcda for coverage) live
    under build_dir/CMakeFiles/.../*.o — no scatter into the OVE source
    tree, no manual dep tracking required."""
    env = _venv_env(ove_dir)
    if extra_env:
        env.update(extra_env)

    cmake_defs = [
        f"-DBOARD_CONFIG={board_config}",
        f"-DOVE_DIR={ove_dir}",
        f"-DNUTTX_APPS_DIR={apps_build}",
        # _OVE_VARIANT_DIR holds the variant's own ove_config.h (with
        # CONFIG_OVE_RTOS_NUTTX=1 etc.). The sim variant borrows the
        # nuttx-qemu app fixture but supplies tests/sim/nuttx/ for its
        # ove_config.h, so the variant dir is decoupled from app_subdir.
        f"-D_OVE_VARIANT_DIR={variant_dir}",
    ]
    if coverage:
        cmake_defs.append("-DOVE_COVERAGE=ON")

    logger.debug(f"cmake configure: {board_config} -> {build_dir}")
    # --no-warn-unused-cli silences "Manually-specified variables were not
    # used" for OVE_DIR / OVE_COVERAGE — they're consumed in the
    # nuttx-apps subtree (external/ove_test/CMakeLists.txt) which CMake
    # processes after the top-level pass that emits the warning.
    run(["cmake", "-G", "Ninja", "--no-warn-unused-cli",
         "-S", nuttx_src, "-B", build_dir, *cmake_defs], env=env)

    nuttx_config = os.path.join(build_dir, ".config")
    for overlay in defconfig_overlays:
        if os.path.isfile(overlay):
            apply_defconfig_overlay(nuttx_config, overlay)

    # olddefconfig with the env vars NuttX's CMake-driven Kconfig expects;
    # mirrors KCONFIG_ENV from cmake/menuconfig.cmake. APPSBINDIR points
    # at the *generated* apps Kconfig under build_dir, not the source
    # tree — CMake's nuttx_generate_kconfig() emits a top-level
    # nuttx-apps/Kconfig there that aggregates all enabled subdir
    # Kconfigs (the source tree has no such file).
    kconfig_env = dict(env)
    kconfig_env.update({
        "KCONFIG_CONFIG": nuttx_config,
        "EXTERNALDIR": "dummy",
        "APPSDIR": apps_build,
        "DRIVERS_PLATFORM_DIR": "dummy",
        "APPSBINDIR": os.path.join(build_dir, "nuttx-apps"),
        "BINDIR": build_dir,
    })
    olddefconfig = os.path.join(ove_dir, ".venv", "bin", "olddefconfig")
    run([olddefconfig], cwd=nuttx_src, env=kconfig_env)

    # Re-run configure: nuttx_mkconfig.cmake compares .config vs .config.prev
    # and regenerates derived headers when they differ. -S must match the
    # original source dir or CMake errors out on the cache mismatch.
    run(["cmake", "-S", nuttx_src, "-B", build_dir,
         "--no-warn-unused-cli"], env=env)
    run(["cmake", "--build", build_dir, "-j", str(nproc())], env=env)


def _run_nuttx_sim(ove_dir, output_dir, *, build_subdir, label,
                    coverage=False):
    """Shared driver for test_nuttx and test_nuttx_coverage.

    When `coverage=True` the app is built with -DOVE_COVERAGE=ON, stale
    `.gcda` counters under OVE_DIR are removed before the run, and the
    `__gcov_dump()` hook in main.c flushes counters on clean exit. .gcda
    files land under <build>/CMakeFiles/.../*.gcda — lcov --directory
    walks ove_dir to collect them."""
    build_base = os.path.join(output_dir, "tests", build_subdir)
    apps_build = os.path.join(build_base, "nuttx-apps")
    build_dir = os.path.join(build_base, "build")
    nuttx_exe = os.path.join(build_dir, "nuttx")

    logger.info("Building %s", label)
    os.makedirs(build_base, exist_ok=True)

    nuttx_src, apps_dl, _ = _nuttx_fetch_sources(ove_dir)
    if not os.path.isdir(apps_build):
        logger.debug("Copying NuttX apps to build tree...")
        shutil.copytree(apps_dl, apps_build, symlinks=True)
    _nuttx_register_test_app(ove_dir, apps_build, "nuttx-qemu")

    if coverage:
        _clean_gcda(ove_dir)

    _nuttx_cmake_build(
        ove_dir,
        nuttx_src=nuttx_src,
        apps_build=apps_build,
        build_dir=build_dir,
        board_config="sim:nsh",
        defconfig_overlays=[
            os.path.join(ove_dir, "tests", "sim", "nuttx",
                         "nuttx_sim_defconfig"),
        ],
        coverage=coverage,
        variant_dir=_nuttx_variant_config_dir(ove_dir, "nuttx"),
    )

    logger.info("Running %s", label)
    # NuttX sim doesn't exit when init task returns; use timeout + output parsing
    try:
        result = subprocess.run(
            [nuttx_exe], timeout=60, capture_output=True, text=True)
        stdout = result.stdout
    except subprocess.TimeoutExpired as e:
        stdout = e.stdout.decode() if e.stdout else ""
    print(stdout, end="")

    parsed = _parse_cmocka(stdout)
    parsed.suite = label
    # The native NuttX sim never self-exits (it idles after the init task
    # returns), so the timeout above is the *normal* exit path and
    # result.returncode tells us nothing.  Require the firmware's end-of-run
    # summary line as proof the run completed (see _summary_failures): its
    # absence means the run faulted/hung mid-way — some suites may have
    # printed [ PASSED ] before stopping, which must NOT score as a pass.
    # A non-zero N is the firmware's own failure tally.
    n = _summary_failures(stdout)
    if n is None:
        logger.error("%s: no completion summary — run stopped early", label)
        parsed.failed = max(parsed.failed, 1)
        parsed.failed_names.append("<incomplete run: no summary>")
    elif n > 0:
        parsed.failed = max(parsed.failed, n)
    if parsed.failed > 0 or parsed.passed == 0:
        logger.error("%s had failures", label)
        parsed.failed = max(parsed.failed, 1)
    return parsed


def test_nuttx(ove_dir, output_dir):
    """Build and run NuttX sim tests (uses NuttX sim board)."""
    return _run_nuttx_sim(ove_dir, output_dir,
                          build_subdir="nuttx",
                          label="nuttx-sim")


def test_nuttx_coverage(ove_dir, output_dir):
    """NuttX sim tests with --coverage; emit lcov tracefile.

    `.gcda` files land under <build>/CMakeFiles/.../*.gcda; lcov
    --directory walks ove_dir to collect them (the build dir is under
    ove_dir/output) and writes the filtered tracefile into the coverage
    build dir.
    """
    result = _run_nuttx_sim(ove_dir, output_dir,
                            build_subdir="nuttx_coverage",
                            label="nuttx-sim-coverage",
                            coverage=True)

    lcov = shutil.which("lcov")
    if not lcov:
        logger.error("lcov not found — skipping NuttX coverage capture")
        return result

    build = os.path.join(output_dir, "tests", "nuttx_coverage")
    info = os.path.join(build, "coverage.info")
    filtered = os.path.join(build, "coverage.filtered.info")
    run([lcov, "--directory", ove_dir, "--capture",
         "--test-name", "nuttx",
         "--rc", "branch_coverage=1",
         "--output-file", info,
         "--ignore-errors", "mismatch,gcov,source,empty,inconsistent"])
    _lcov_filter_ove_sources(lcov, info, filtered, ove_dir,
                             extra_ignore=["format"])
    logger.info("NuttX coverage: %s", filtered)
    return result


def _run_zephyr_native_sim(ove_dir, output_dir, *, build_subdir, label,
                            extra_conf=None):
    """Shared driver for test_zephyr and test_zephyr_coverage.

    `extra_conf` is applied as Zephyr's EXTRA_CONF_FILE so Kconfig overlays
    (e.g. CONFIG_COVERAGE=y) can be layered on top of tests/sim/zephyr/prj.conf
    without duplicating the baseline build graph.
    """
    import hashlib
    build = os.path.join(output_dir, "tests", build_subdir)
    dl_dir = os.path.join(ove_dir, "dl")
    west = os.path.join(ove_dir, ".venv", "bin", "west")

    manifest = load_manifest(ove_dir)
    default_rev = get_component(manifest, "rtos", "zephyr", "version")
    zephyr_url = get_component(manifest, "rtos", "zephyr", "url")
    dl_hash = hashlib.sha256(default_rev.encode()).hexdigest()[:8]

    logger.info("Building Zephyr native_sim tests (%s)", label)
    os.makedirs(build, exist_ok=True)

    hash_dir = os.path.join(dl_dir, f"zephyr-workspace-{dl_hash}")
    if not os.path.isdir(os.path.join(hash_dir, "zephyr")):
        logger.debug("Zephyr workspace not found -- downloading...")
        run([west, "init", "-m",
              zephyr_url,
              "--mr", "main", hash_dir])
        run(["git", "-C", os.path.join(hash_dir, "zephyr"),
              "checkout", default_rev])
        run([west, "update"], cwd=hash_dir)

    link = os.path.join(build, "zephyr-workspace")
    atomic_symlink(hash_dir, link)

    env = dict(os.environ)
    env["ZEPHYR_BASE"] = os.path.join(link, "zephyr")
    env["ZEPHYR_TOOLCHAIN_VARIANT"] = "host"
    build_cmd = [
        west, "build",
        "-b", "native_sim/native/64",
        "-d", build,
        os.path.join(ove_dir, "tests", "sim", "zephyr"),
    ]
    if extra_conf:
        build_cmd.extend(["--", f"-DEXTRA_CONF_FILE={extra_conf}"])
    run(build_cmd, env=env)

    logger.info("Running Zephyr native_sim tests (%s)", label)
    return _run_test_binary(
        [os.path.join(build, "zephyr", "zephyr.exe")], label)


def test_zephyr(ove_dir, output_dir):
    """Build and run Zephyr native_sim tests."""
    return _run_zephyr_native_sim(ove_dir, output_dir,
                                   build_subdir="zephyr",
                                   label="zephyr-native-sim")


def test_zephyr_coverage(ove_dir, output_dir):
    """Zephyr native_sim tests with CONFIG_COVERAGE=y; emit lcov tracefile.

    Zephyr drops .gcda alongside .gcno in the build tree when main() returns,
    so we just run the binary and `lcov --capture`. Output:
      <cov_dir>/coverage.filtered.info
    """
    overlay = os.path.join(ove_dir, "tests", "sim", "zephyr",
                            "overlay-coverage.conf")
    result = _run_zephyr_native_sim(
        ove_dir, output_dir,
        build_subdir="zephyr_coverage",
        label="zephyr-native-sim-coverage",
        extra_conf=overlay)

    lcov = shutil.which("lcov")
    if not lcov:
        logger.error("lcov not found — skipping Zephyr coverage capture")
        return result

    build = os.path.join(output_dir, "tests", "zephyr_coverage")
    info = os.path.join(build, "coverage.info")
    filtered = os.path.join(build, "coverage.filtered.info")
    run([lcov, "--directory", build, "--capture",
         "--test-name", "zephyr",
         "--rc", "branch_coverage=1",
         "--output-file", info,
         "--ignore-errors", "mismatch,gcov,source,empty,inconsistent"])
    _lcov_filter_ove_sources(lcov, info, filtered, ove_dir,
                             extra_ignore=["format"])
    logger.info("Zephyr coverage: %s", filtered)
    return result


# ── FreeRTOS QEMU shared driver ────────────────────────────────────────
def _run_freertos_qemu(ove_dir, output_dir, *, src_subdir, binary, label,
                       coverage=False):
    """Build and run a FreeRTOS QEMU ARM test variant."""
    tc_dir = _ensure_arm_toolchain(ove_dir)
    build = os.path.join(output_dir, "tests", label)
    # Coverage builds must start from a clean tree: the toolchain and
    # --coverage flags aren't in the cmake cache, and stale .gcno files
    # will mislead lcov into reporting 0% on already-instrumented objects.
    if coverage:
        shutil.rmtree(build, ignore_errors=True)
    logger.info(f"Building {label}")
    cmake_args = [f"-DOVE_TOOLCHAIN_DIR={tc_dir}"]
    if coverage:
        cmake_args.append("-DOVE_TEST_BUILD_COVERAGE=ON")
    _cmake_build(os.path.join(ove_dir, "tests", "sim", src_subdir),
                 build, extra_args=cmake_args)
    logger.info(f"Running {label}")
    qemu_run = os.path.join(ove_dir, "boards", "qemu-mps2-an500",
                            "qemu-run.sh")
    return _run_test_binary(
        [qemu_run, os.path.join(build, binary),
         "--headless", "--timeout", "45"], label)


def test_qemu_freertos(ove_dir, output_dir):
    """Build and run FreeRTOS QEMU ARM tests."""
    return _run_freertos_qemu(ove_dir, output_dir,
                              src_subdir="freertos-qemu",
                              binary="ove_test_freertos_qemu",
                              label="qemu-freertos")


def test_qemu_freertos_zeroheap(ove_dir, output_dir):
    """Build and run FreeRTOS QEMU ARM tests (zero-heap mode)."""
    return _run_freertos_qemu(ove_dir, output_dir,
                              src_subdir="freertos-qemu-zeroheap",
                              binary="ove_test_freertos_qemu_zeroheap",
                              label="qemu-freertos-zeroheap")


# ── Renode STM32F746 shared driver ─────────────────────────────────────
def _ensure_renode(ove_dir):
    """Locate (and, if needed, download) the Renode binary pinned in
    manifest.yaml (tools.renode).  Delegates to download.download_renode
    so `ove download` / `ove ensure-toolchain renode` / the test runner
    all go through one path.  Returns the launcher path or None; None is
    treated as SKIP, not FAIL.
    """
    from .download import download_renode
    from .manifest import load_manifest

    dl_dir = os.path.join(ove_dir, "dl")
    tools_dir = os.path.join(ove_dir, "output", "tools")
    os.makedirs(dl_dir, exist_ok=True)
    os.makedirs(tools_dir, exist_ok=True)
    manifest = load_manifest(ove_dir)
    return download_renode(dl_dir, tools_dir, manifest=manifest)


def _build_renode_stm32f746(ove_dir, output_dir, *, src_subdir, binary, label,
                            extra_cmake=None):
    """Build an STM32F746 FreeRTOS test firmware via the existing CMake
    sim tree.  Returns the path to the produced ELF.

    `extra_cmake` is a list of extra `-D…` flags forwarded to CMake.
    The HW runner uses this to pass `-DOVE_HW=ON`, which the FreeRTOS
    sim CMakeLists routes through to the toolchain shim selection +
    stdio backend selection.
    """
    tc_dir = _ensure_arm_toolchain(ove_dir)
    _ensure_stm32f746_renode_deps(ove_dir)
    build = os.path.join(output_dir, "tests", label)
    logger.info(f"Building {label}")
    cmake_args = [f"-DOVE_TOOLCHAIN_DIR={tc_dir}"]
    if extra_cmake:
        cmake_args.extend(extra_cmake)
    _cmake_build(os.path.join(ove_dir, "tests", "sim", src_subdir),
                 build, extra_args=cmake_args)
    return os.path.join(build, f"{binary}.elf")


def _run_renode_stm32f746(ove_dir, output_dir, *, src_subdir, binary, label):
    """Build an STM32F746 test firmware and run it under Renode.

    Output is captured via a SemihostingUart attached by test.resc and
    written to <build>/uart.log; the existing `_parse_cmocka` reads that
    for pass/fail tallies.  Semihosting SYS_EXIT_EXTENDED is unsupported
    in Renode 1.16.x, so the firmware falls through after the test
    summary and we rely on `emulation RunFor` inside test.resc to bound
    simulated time; the harness caps wall-clock at 300 s.
    """
    elf = _build_renode_stm32f746(ove_dir, output_dir,
                                  src_subdir=src_subdir,
                                  binary=binary, label=label)
    resc = os.path.join(ove_dir, "tests", "sim", src_subdir, "test.resc")
    return _renode_run_elf(ove_dir, output_dir,
                           label=label, elf=elf, resc=resc)


def _renode_run_elf(ove_dir, output_dir, *, label, elf, resc):
    """Launch a pre-built ELF under Renode using a sim's test.resc.

    Shared body of `_run_renode_stm32f746` and the Zephyr/NuttX-Renode
    drivers below — only the build step varies between RTOSes.  See
    `_run_renode_stm32f746` for the rationale on capture + parsing.
    """
    renode = _ensure_renode(ove_dir)
    if renode is None:
        logger.warning(f"{label}: Renode unavailable — skipping")
        return TestResults(suite=label, passed=0, failed=0, failed_names=[])

    build = os.path.join(output_dir, "tests", label)
    uart_out = os.path.join(build, "uart.log")
    renode_log = os.path.join(build, "renode.log")
    for f in (uart_out, renode_log):
        try:
            os.remove(f)
        except OSError:
            pass

    logger.info(f"Running {label}")
    cmd = [renode, "--console", "--disable-xwt", "--plain",
           "-e", f"$bin = @{elf}",
           "-e", f"$uart_out = @{uart_out}",
           "-e", f"$rlog = @{renode_log}",
           "-e", f"include @{resc}",
           "-e", "quit"]
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        logger.error(f"{label}: Renode timed out after 300 s")
        return TestResults(suite=label, passed=0, failed=1,
                           failed_names=["<renode timeout>"])

    output = ""
    if os.path.isfile(uart_out):
        with open(uart_out, encoding="utf-8", errors="replace") as f:
            output = f.read()
    print(output, end="")
    parsed = _parse_cmocka(output)
    parsed.suite = label
    # Renode ignores ARM semihosting exit (the process exits 0 regardless),
    # so r.returncode can't distinguish a clean finish from a fault/hang.
    # Require the firmware's end-of-run summary line as proof of completion
    # (see _summary_failures): its absence means the run stopped early — a
    # partial run that printed some [ PASSED ] lines before faulting would
    # otherwise score green.  N>0 is the firmware's own tally (includes
    # cmocka GROUP errors _parse_cmocka cannot see).  This subsumes the old
    # "no cmocka output -> fail" check (no output -> no summary -> fail).
    n = _summary_failures(output)
    if n is None:
        parsed.failed = max(parsed.failed, 1)
        parsed.failed_names.append("<incomplete run: no summary>")
    elif n > 0:
        parsed.failed = max(parsed.failed, n)
    # Defence-in-depth: a non-zero Renode exit (e.g. tool error) still fails.
    if r.returncode != 0 and parsed.failed == 0:
        parsed.failed = 1
    return parsed


def _run_renode_stm32f746_nuttx(ove_dir, output_dir, *, app_subdir, label):
    """Build a NuttX stm32f746g-disco firmware and run it under Renode.

    Mirrors `_run_nuttx_qemu`'s build steps verbatim — only diverges in
    the CMake board target and the runner. Kept separate rather than
    parameterising `_run_nuttx_qemu` because that helper's QEMU
    invocation path is QEMU-specific.
    """
    elf = _build_renode_stm32f746_nuttx(ove_dir, output_dir,
                                        app_subdir=app_subdir, label=label)
    resc = os.path.join(ove_dir, "tests", "sim", app_subdir, "test.resc")
    return _renode_run_elf(ove_dir, output_dir,
                           label=label, elf=elf, resc=resc)


def _build_renode_stm32f746_nuttx(ove_dir, output_dir, *, app_subdir, label):
    """Build a NuttX stm32f746g-disco firmware via CMake. Returns the path
    to the produced ELF (NuttX names it `nuttx`, no suffix). The HW
    runner reuses this verbatim — NuttX's stm32f7 backend already writes
    via USART1 by default, so no extra knobs."""
    tc_dir = _ensure_arm_toolchain(ove_dir)
    build_base = os.path.join(output_dir, "tests", label)
    apps_build = os.path.join(build_base, "nuttx-apps")
    build_dir = os.path.join(build_base, "build")

    logger.info(f"Building {label}")
    os.makedirs(build_base, exist_ok=True)

    nuttx_src, apps_dl, _ = _nuttx_fetch_sources(ove_dir)
    if not os.path.isdir(apps_build):
        shutil.copytree(apps_dl, apps_build, symlinks=True)
    _nuttx_register_test_app(ove_dir, apps_build, app_subdir)

    extra_env = {
        "PATH": os.path.join(tc_dir, "bin") + os.pathsep + os.environ["PATH"],
    }
    overlays = [
        os.path.join(ove_dir, "tests", "sim", app_subdir,
                     "nuttx_test_defconfig"),
    ]

    _nuttx_cmake_build(
        ove_dir,
        nuttx_src=nuttx_src,
        apps_build=apps_build,
        build_dir=build_dir,
        board_config="stm32f746g-disco:nsh",
        defconfig_overlays=overlays,
        coverage=False,
        variant_dir=_nuttx_variant_config_dir(ove_dir, app_subdir),
        extra_env=extra_env,
    )

    return os.path.join(build_dir, "nuttx")


def test_renode_stm32f746_nuttx(ove_dir, output_dir):
    """Build + run the STM32F746 NuttX heap test firmware under Renode."""
    return _run_renode_stm32f746_nuttx(ove_dir, output_dir,
        app_subdir="renode-stm32f746-nuttx",
        label="renode-stm32f746-nuttx")


def test_renode_stm32f746_nuttx_zeroheap(ove_dir, output_dir):
    """Build + run the STM32F746 NuttX zero-heap firmware under Renode."""
    return _run_renode_stm32f746_nuttx(ove_dir, output_dir,
        app_subdir="renode-stm32f746-nuttx-zeroheap",
        label="renode-stm32f746-nuttx-zeroheap")


def _build_renode_stm32f746_zephyr(ove_dir, output_dir, *, src_subdir, label,
                                   extra_cmake=None):
    """Build a Zephyr stm32f746g_disco firmware via west.  Returns the
    path to the produced zephyr.elf.

    `extra_cmake` is a list of extra `-D…` flags forwarded to the
    CMake invocation.  The HW runner uses this to pass `-DOVE_HW=ON`.
    """
    import hashlib
    dl_dir = os.path.join(ove_dir, "dl")
    build = os.path.join(output_dir, "tests", label)
    west = os.path.join(ove_dir, ".venv", "bin", "west")

    manifest = load_manifest(ove_dir)
    default_rev = get_component(manifest, "rtos", "zephyr", "version")
    zephyr_url = get_component(manifest, "rtos", "zephyr", "url")
    dl_hash = hashlib.sha256(default_rev.encode()).hexdigest()[:8]

    logger.info(f"Building {label}")
    os.makedirs(build, exist_ok=True)

    hash_dir = os.path.join(dl_dir, f"zephyr-workspace-{dl_hash}")
    if not os.path.isdir(os.path.join(hash_dir, "zephyr")):
        logger.debug("Zephyr workspace not found -- downloading...")
        run([west, "init", "-m", zephyr_url, "--mr", "main", hash_dir])
        run(["git", "-C", os.path.join(hash_dir, "zephyr"),
             "checkout", default_rev])
        run([west, "update"], cwd=hash_dir)

    link = os.path.join(build, "zephyr-workspace")
    atomic_symlink(hash_dir, link)

    env = _zephyr_sdk_build_env(ove_dir)
    env["ZEPHYR_BASE"] = os.path.join(link, "zephyr")
    cmd = [west, "build",
           "-b", "stm32f746g_disco",
           "-d", build,
           os.path.join(ove_dir, "tests", "sim", src_subdir)]
    if extra_cmake:
        cmd.extend(["--", *extra_cmake])
    run(cmd, env=env)
    return os.path.join(build, "zephyr", "zephyr.elf")


def _run_renode_stm32f746_zephyr(ove_dir, output_dir, *, src_subdir, label):
    """Build a Zephyr stm32f746g_disco firmware and run it under Renode.

    Reuses `_run_zephyr_qemu`'s workspace setup almost verbatim — only
    diverges in BOARD (stm32f746g_disco vs mps2/an500) and runner
    (Renode vs qemu-run.sh).  Kept as a separate helper rather than
    parameterising `_run_zephyr_qemu` because the latter's QEMU
    invocation path is QEMU-specific.
    """
    elf = _build_renode_stm32f746_zephyr(ove_dir, output_dir,
                                         src_subdir=src_subdir, label=label)
    resc = os.path.join(ove_dir, "tests", "sim", src_subdir, "test.resc")
    return _renode_run_elf(ove_dir, output_dir,
                           label=label, elf=elf, resc=resc)


def test_renode_stm32f746_zephyr(ove_dir, output_dir):
    """Build + run the STM32F746 Zephyr heap test firmware under Renode."""
    return _run_renode_stm32f746_zephyr(ove_dir, output_dir,
        src_subdir="renode-stm32f746-zephyr",
        label="renode-stm32f746-zephyr")


def test_renode_stm32f746_zephyr_zeroheap(ove_dir, output_dir):
    """Build + run the STM32F746 Zephyr zero-heap firmware under Renode."""
    return _run_renode_stm32f746_zephyr(ove_dir, output_dir,
        src_subdir="renode-stm32f746-zephyr-zeroheap",
        label="renode-stm32f746-zephyr-zeroheap")


def test_renode_stm32f746_freertos(ove_dir, output_dir):
    """Build + run the STM32F746 heap-mode test firmware under Renode."""
    return _run_renode_stm32f746(ove_dir, output_dir,
        src_subdir="renode-stm32f746-freertos",
        binary="ove_test_renode_stm32f746_freertos",
        label="renode-stm32f746-freertos")


def test_renode_stm32f746_freertos_zeroheap(ove_dir, output_dir):
    """Build + run the STM32F746 zero-heap test firmware under Renode."""
    return _run_renode_stm32f746(ove_dir, output_dir,
        src_subdir="renode-stm32f746-freertos-zeroheap",
        binary="ove_test_renode_stm32f746_freertos_zeroheap",
        label="renode-stm32f746-freertos-zeroheap")


# ── Hardware-in-the-loop targets (manual-only) ────────────────────────
#
# These flash a real STM32F746G-Discovery board with the same firmware
# the matching Renode target builds, then read CMocka output back over
# USART1 (the on-board ST-Link VCP).  They are intentionally NOT in any
# group list (SIM_TESTS / QEMU_TESTS / RENODE_TESTS) so `make test-all`
# and the GitHub Actions CI never touch them.  Run them manually with
# the env var pointing at your board:
#
#     OVE_HW_SERIAL_PORT=/dev/ttyACM0 make test-hw-stm32f746-freertos
#
# The serial-port path is the only required knob.  An optional
# OVE_HW_TIMEOUT (seconds) bounds the wall-clock read window per run;
# default 120, plenty for the largest variant.

def _run_hw_stm32f746(ove_dir, output_dir, *, label, elf):
    """Flash a pre-built ELF onto the STM32F746G-Discovery via OpenOCD,
    then capture USART1 output via pyserial and parse the CMocka
    summary.  Returns a `TestResults`.

    Reads `OVE_HW_SERIAL_PORT` (required) and `OVE_HW_TIMEOUT`
    (optional, seconds, default 120) from the environment.

    Reuses the OpenOCD invocation pattern from
    boards/stm32f746g-discovery/freertos/flash.sh — `openocd` is taken
    from PATH (system-installed; matches what flash.sh assumes).
    """
    serial_port = os.environ.get("OVE_HW_SERIAL_PORT")
    if not serial_port:
        logger.error(
            f"{label}: OVE_HW_SERIAL_PORT is not set — point it at the "
            f"board's USART1 VCP, e.g. OVE_HW_SERIAL_PORT=/dev/ttyACM0")
        return TestResults(suite=label, passed=0, failed=1,
                           failed_names=["<OVE_HW_SERIAL_PORT unset>"])

    try:
        import serial as pyserial
    except ImportError:
        logger.error(
            f"{label}: pyserial not installed in the venv.  Run "
            f"'pip install pyserial>=3.5' (or 'ove doctor' to see other "
            f"missing HW deps).")
        return TestResults(suite=label, passed=0, failed=1,
                           failed_names=["<pyserial missing>"])

    openocd = shutil.which("openocd")
    if openocd is None:
        logger.error(
            f"{label}: openocd not found on PATH.  Install via your "
            f"package manager (e.g. 'apt install openocd').")
        return TestResults(suite=label, passed=0, failed=1,
                           failed_names=["<openocd missing>"])

    # Open the serial port BEFORE flashing.  OpenOCD's `reset exit`
    # releases the CPU; if pyserial isn't already listening we miss the
    # first lines (early test summary frames have been observed within
    # ~20 ms of release on this board).
    try:
        ser = pyserial.Serial(serial_port, 115200, timeout=1)
    except (pyserial.SerialException, OSError) as e:
        logger.error(f"{label}: failed to open {serial_port}: {e}")
        return TestResults(suite=label, passed=0, failed=1,
                           failed_names=[f"<serial open: {e}>"])

    try:
        # Drain BEFORE flashing.  Two passes: the first clears bytes
        # already in the kernel's tty buffer, then we sleep ~150 ms so
        # any in-flight bytes still queued in the ST-LINK USB pipeline
        # land in the kernel buffer, then we drain again.  Without the
        # second pass, leftover bytes from the previous firmware's
        # post-summary tail (especially on Zephyr/NuttX, which boot
        # fast and start printing again) get mixed into the parsed
        # output and double-count CMocka frames (we saw +11 phantom
        # tests on FreeRTOS heap with single-pass drain).
        #
        # Drain only happens pre-flash — once OpenOCD halts the CPU
        # for programming, the prior firmware can't emit anything new,
        # so the buffer stays clean through the reset.  After OpenOCD
        # exits we read straight away to catch the new firmware's
        # boot output from the very first byte.
        ser.reset_input_buffer()
        time.sleep(0.15)
        ser.reset_input_buffer()

        logger.info(f"{label}: flashing {os.path.basename(elf)} via OpenOCD")
        flash_cmd = [openocd,
                     "-f", "board/stm32f7discovery.cfg",
                     "-c", f"program {elf} verify reset exit"]
        try:
            subprocess.run(flash_cmd, check=True, timeout=120,
                           stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        except subprocess.CalledProcessError as e:
            logger.error(f"{label}: OpenOCD failed (exit {e.returncode})")
            if e.stdout:
                print(e.stdout, end="")
            return TestResults(suite=label, passed=0, failed=1,
                               failed_names=["<openocd flash failed>"])
        except subprocess.TimeoutExpired:
            logger.error(f"{label}: OpenOCD timed out")
            return TestResults(suite=label, passed=0, failed=1,
                               failed_names=["<openocd timeout>"])

        timeout_s = int(os.environ.get("OVE_HW_TIMEOUT", "120"))
        logger.info(f"{label}: reading {serial_port} @ 115200 "
                    f"(up to {timeout_s}s)")

        # Read line-by-line; mirror to stdout for live progress.  Stop
        # when the firmware emits the CMocka end-of-run sentinel — same
        # line _parse_cmocka uses to compute pass/fail tallies.
        deadline = time.monotonic() + timeout_s
        captured = []
        summary_re = re.compile(r"^=== Summary: \d+ test group\(s\)")
        seen_frame = False
        while time.monotonic() < deadline:
            line = ser.readline()
            if not line:
                continue
            try:
                text = line.decode("utf-8", errors="replace")
            except Exception:
                text = repr(line)
            print(text, end="")
            captured.append(text)
            if "[==========]" in text:
                seen_frame = True
            if seen_frame and summary_re.match(text):
                break
        else:
            logger.error(f"{label}: serial read timed out after {timeout_s}s")
            output = "".join(captured)
            parsed = _parse_cmocka(output)
            parsed.suite = label
            if parsed.failed == 0:
                parsed.failed = 1
                parsed.failed_names.append("<hw serial timeout>")
            return parsed

        output = "".join(captured)
    finally:
        ser.close()

    parsed = _parse_cmocka(output)
    parsed.suite = label
    if parsed.passed == 0 and parsed.failed == 0:
        parsed.failed = 1
        parsed.failed_names.append("<no cmocka output>")
    return parsed


def test_hw_stm32f746_freertos(ove_dir, output_dir):
    """Flash + run the STM32F746 FreeRTOS heap firmware on real hardware."""
    elf = _build_renode_stm32f746(ove_dir, output_dir,
        src_subdir="renode-stm32f746-freertos",
        binary="ove_test_renode_stm32f746_freertos",
        label="hw-stm32f746-freertos",
        extra_cmake=["-DOVE_HW=ON"])
    return _run_hw_stm32f746(ove_dir, output_dir,
                             label="hw-stm32f746-freertos", elf=elf)


def test_hw_stm32f746_freertos_zeroheap(ove_dir, output_dir):
    """Flash + run the STM32F746 FreeRTOS zero-heap firmware on hardware."""
    elf = _build_renode_stm32f746(ove_dir, output_dir,
        src_subdir="renode-stm32f746-freertos-zeroheap",
        binary="ove_test_renode_stm32f746_freertos_zeroheap",
        label="hw-stm32f746-freertos-zeroheap",
        extra_cmake=["-DOVE_HW=ON"])
    return _run_hw_stm32f746(ove_dir, output_dir,
                             label="hw-stm32f746-freertos-zeroheap", elf=elf)


def test_hw_stm32f746_zephyr(ove_dir, output_dir):
    """Flash + run the STM32F746 Zephyr heap firmware on real hardware."""
    elf = _build_renode_stm32f746_zephyr(ove_dir, output_dir,
        src_subdir="renode-stm32f746-zephyr",
        label="hw-stm32f746-zephyr",
        extra_cmake=["-DOVE_HW=ON"])
    return _run_hw_stm32f746(ove_dir, output_dir,
                             label="hw-stm32f746-zephyr", elf=elf)


def test_hw_stm32f746_zephyr_zeroheap(ove_dir, output_dir):
    """Flash + run the STM32F746 Zephyr zero-heap firmware on hardware."""
    elf = _build_renode_stm32f746_zephyr(ove_dir, output_dir,
        src_subdir="renode-stm32f746-zephyr-zeroheap",
        label="hw-stm32f746-zephyr-zeroheap",
        extra_cmake=["-DOVE_HW=ON"])
    return _run_hw_stm32f746(ove_dir, output_dir,
                             label="hw-stm32f746-zephyr-zeroheap", elf=elf)


def test_hw_stm32f746_nuttx(ove_dir, output_dir):
    """Flash + run the STM32F746 NuttX heap firmware on real hardware."""
    elf = _build_renode_stm32f746_nuttx(ove_dir, output_dir,
        app_subdir="renode-stm32f746-nuttx",
        label="hw-stm32f746-nuttx")
    return _run_hw_stm32f746(ove_dir, output_dir,
                             label="hw-stm32f746-nuttx", elf=elf)


def test_hw_stm32f746_nuttx_zeroheap(ove_dir, output_dir):
    """Flash + run the STM32F746 NuttX zero-heap firmware on hardware."""
    elf = _build_renode_stm32f746_nuttx(ove_dir, output_dir,
        app_subdir="renode-stm32f746-nuttx-zeroheap",
        label="hw-stm32f746-nuttx-zeroheap")
    return _run_hw_stm32f746(ove_dir, output_dir,
                             label="hw-stm32f746-nuttx-zeroheap", elf=elf)


def test_qemu_freertos_coverage(ove_dir, output_dir):
    """Build and run FreeRTOS QEMU tests with --coverage; emit lcov tracefile.

    libgcov's default writer calls fopen/fwrite/fclose on absolute paths
    embedded at compile time (-fprofile-abs-path). Newlib's rdimon.specs
    routes those through ARM semihosting, so QEMU writes the .gcda files
    straight to the host build tree alongside the .gcno. We then run
    lcov --capture on the build dir and filter out non-oveRTOS paths.
    """
    label = "qemu-freertos-coverage"
    result = _run_freertos_qemu(ove_dir, output_dir,
                                src_subdir="freertos-qemu",
                                binary="ove_test_freertos_qemu",
                                label=label, coverage=True)
    build = os.path.join(output_dir, "tests", label)
    cov_dir = os.path.join(output_dir, "tests", "qemu_freertos_coverage")
    os.makedirs(cov_dir, exist_ok=True)
    raw = os.path.join(cov_dir, "coverage.info")
    filtered = os.path.join(cov_dir, "coverage.filtered.info")
    tc_dir = _ensure_arm_toolchain(ove_dir)
    gcov = os.path.join(tc_dir, "bin", "arm-none-eabi-gcov")
    lcov = shutil.which("lcov")
    if not lcov:
        logger.error("lcov not installed; skipping capture")
        return result
    run([lcov, "--capture", "--directory", build,
         "--gcov-tool", gcov,
         "--output-file", raw, "--rc", "branch_coverage=1",
         "--ignore-errors", "mismatch,source,gcov,unused,inconsistent"])
    _lcov_filter_ove_sources(lcov, raw, filtered, ove_dir)
    logger.info("FreeRTOS QEMU coverage: %s", filtered)
    return result


# ── NuttX QEMU shared driver ───────────────────────────────────────────
def _run_nuttx_qemu(ove_dir, output_dir, *, app_subdir, label, coverage=False,
                    board_config="mps2-an500:nsh"):
    """Build and run a NuttX QEMU ARM test variant via CMake.

    `app_subdir` is the tests/sim/<dir>/ holding nuttx_app/ and
    nuttx_test_defconfig (e.g. "nuttx-qemu" or "nuttx-qemu-zeroheap").
    `board_config` selects the NuttX base config — "mps2-an500:nsh" (flat) by
    default, or "mps2-an500:knsh" for the CONFIG_BUILD_PROTECTED variant.
    """
    tc_dir = _ensure_arm_toolchain(ove_dir)
    build_base = os.path.join(output_dir, "tests", label)
    apps_build = os.path.join(build_base, "nuttx-apps")
    build_dir = os.path.join(build_base, "build")
    nuttx_exe = os.path.join(build_dir, "nuttx")

    logger.info(f"Building {label}")
    os.makedirs(build_base, exist_ok=True)

    nuttx_src, apps_dl, _ = _nuttx_fetch_sources(ove_dir)
    if not os.path.isdir(apps_build):
        logger.debug("Copying NuttX apps to build tree...")
        shutil.copytree(apps_dl, apps_build, symlinks=True)
    _nuttx_register_test_app(ove_dir, apps_build, app_subdir)

    # arm-none-eabi-gcc must be discoverable for CMake's compiler probe.
    extra_env = {
        "PATH": os.path.join(tc_dir, "bin") + os.pathsep + os.environ["PATH"],
    }

    overlays = [
        os.path.join(ove_dir, "tests", "sim", app_subdir,
                     "nuttx_test_defconfig"),
    ]
    if coverage:
        overlays.append(os.path.join(ove_dir, "tests", "sim", app_subdir,
                                     "nuttx_test_coverage_defconfig"))

    _nuttx_cmake_build(
        ove_dir,
        nuttx_src=nuttx_src,
        apps_build=apps_build,
        build_dir=build_dir,
        board_config=board_config,
        defconfig_overlays=overlays,
        coverage=coverage,
        variant_dir=_nuttx_variant_config_dir(ove_dir, app_subdir),
        extra_env=extra_env,
    )

    logger.info(f"Running {label}")
    qemu_run = os.path.join(ove_dir, "boards", "qemu-mps2-an500",
                            "qemu-run.sh")
    return _run_test_binary(
        [qemu_run, nuttx_exe, "--headless", "--timeout", "45"], label)


# ── Zephyr QEMU shared driver ──────────────────────────────────────────
def _run_zephyr_qemu(ove_dir, output_dir, *, src_subdir, label,
                     extra_conf=None, board="mps2/an500", run_via_west=False):
    """Build and run a Zephyr QEMU ARM test variant.

    `board` selects the Zephyr board (default the Cortex-M7 mps2/an500). Boards
    the oveRTOS an500 qemu-run.sh can't host (e.g. mps2/an521/cpu0, the Cortex-M33
    USERSPACE target) set `run_via_west=True` to run through Zephyr's own QEMU
    integration (`west build -t run`) instead.

    `extra_conf` is applied as Zephyr's EXTRA_CONF_FILE so Kconfig overlays
    (e.g. overlay-coverage.conf) layer on top of the baseline prj.conf
    without duplicating the build graph. Coverage builds must start from a
    clean build dir: CMake caches Kconfig state and stale .gcno would
    mislead lcov.
    """
    import hashlib
    dl_dir = os.path.join(ove_dir, "dl")
    build = os.path.join(output_dir, "tests", label)
    west = os.path.join(ove_dir, ".venv", "bin", "west")

    manifest = load_manifest(ove_dir)
    default_rev = get_component(manifest, "rtos", "zephyr", "version")
    zephyr_url = get_component(manifest, "rtos", "zephyr", "url")
    dl_hash = hashlib.sha256(default_rev.encode()).hexdigest()[:8]

    if extra_conf:
        shutil.rmtree(build, ignore_errors=True)
    logger.info(f"Building {label}")
    os.makedirs(build, exist_ok=True)

    hash_dir = os.path.join(dl_dir, f"zephyr-workspace-{dl_hash}")
    if not os.path.isdir(os.path.join(hash_dir, "zephyr")):
        logger.debug("Zephyr workspace not found -- downloading...")
        run([west, "init", "-m",
              zephyr_url,
              "--mr", "main", hash_dir])
        run(["git", "-C", os.path.join(hash_dir, "zephyr"),
              "checkout", default_rev])
        run([west, "update"], cwd=hash_dir)

    link = os.path.join(build, "zephyr-workspace")
    atomic_symlink(hash_dir, link)

    env = _zephyr_sdk_build_env(ove_dir)
    env["ZEPHYR_BASE"] = os.path.join(link, "zephyr")
    build_cmd = [
        west, "build",
        "-b", board,
        "-d", build,
        os.path.join(ove_dir, "tests", "sim", src_subdir),
    ]
    if extra_conf:
        build_cmd.extend(["--", f"-DEXTRA_CONF_FILE={extra_conf}"])
    run(build_cmd, env=env)

    logger.info(f"Running {label}")
    if run_via_west:
        # mps2/an521 (Cortex-M33): the oveRTOS an500 qemu-run.sh can't host it,
        # and Zephyr's own `-t run` routes the UART to a host pty without
        # semihosting. Drive QEMU directly with -semihosting so the firmware's
        # semihosting I/O reaches stdout and SYS_EXIT shuts QEMU down for us.
        elf = os.path.join(build, "zephyr", "zephyr.elf")
        qemu_cmd = ["qemu-system-arm", "-cpu", "cortex-m33",
                    "-machine", "mps2-an521", "-m", "16", "-vga", "none",
                    "-nographic", "-semihosting", "-kernel", elf]
        try:
            return _run_test_binary(qemu_cmd, label, timeout=60)
        except subprocess.TimeoutExpired:
            logger.error("%s: QEMU run timed out", label)
            return TestResults(suite=label, passed=0, failed=1)
    qemu_run = os.path.join(ove_dir, "boards", "qemu-mps2-an500",
                            "qemu-run.sh")
    return _run_test_binary(
        [qemu_run, os.path.join(build, "zephyr", "zephyr.elf"),
         "--headless", "--timeout", "120"], label)


def test_qemu_nuttx(ove_dir, output_dir):
    """Build and run NuttX QEMU ARM tests (uses NuttX build system)."""
    return _run_nuttx_qemu(ove_dir, output_dir,
                           app_subdir="nuttx-qemu", label="qemu-nuttx")


def test_qemu_nuttx_zeroheap(ove_dir, output_dir):
    """Build and run NuttX QEMU ARM tests (zero-heap mode)."""
    return _run_nuttx_qemu(ove_dir, output_dir,
                           app_subdir="nuttx-qemu-zeroheap",
                           label="qemu-nuttx-zeroheap")


def test_qemu_nuttx_coverage(ove_dir, output_dir):
    """Build and run NuttX QEMU tests with --coverage; emit lcov tracefile.

    The app CMakeLists gates `--coverage` + `-fprofile-abs-path` on
    `-DOVE_COVERAGE=ON`, and main.c calls `__gcov_dump()` before
    `semihosting_exit`. libgcov's fopen/fwrite writes .gcda via
    semihosting to absolute host paths (-fprofile-abs-path).
    .gcno/.gcda land under <build>/CMakeFiles/.../*; lcov --capture
    walks ove_dir to collect them.
    """
    label = "qemu-nuttx-coverage"
    _clean_gcda(ove_dir)
    result = _run_nuttx_qemu(ove_dir, output_dir,
                             app_subdir="nuttx-qemu", label=label,
                             coverage=True)
    cov_dir = os.path.join(output_dir, "tests", "qemu_nuttx_coverage")
    os.makedirs(cov_dir, exist_ok=True)
    raw = os.path.join(cov_dir, "coverage.info")
    filtered = os.path.join(cov_dir, "coverage.filtered.info")
    tc_dir = _ensure_arm_toolchain(ove_dir)
    gcov = os.path.join(tc_dir, "bin", "arm-none-eabi-gcov")
    lcov = shutil.which("lcov")
    if not lcov:
        logger.error("lcov not installed; skipping capture")
        return result
    run([lcov, "--capture", "--directory", ove_dir,
         "--directory", os.path.join(output_dir, "tests", label),
         "--gcov-tool", gcov,
         "--output-file", raw, "--rc", "branch_coverage=1",
         "--ignore-errors", "mismatch,source,gcov,unused,inconsistent"])
    _lcov_filter_ove_sources(lcov, raw, filtered, ove_dir)
    logger.info("NuttX QEMU coverage: %s", filtered)
    return result


def test_qemu_zephyr(ove_dir, output_dir):
    """Build and run Zephyr QEMU ARM tests."""
    return _run_zephyr_qemu(ove_dir, output_dir,
                            src_subdir="zephyr-qemu", label="qemu-zephyr")


def test_qemu_zephyr_zeroheap(ove_dir, output_dir):
    """Build and run Zephyr QEMU ARM tests (zero-heap mode)."""
    return _run_zephyr_qemu(ove_dir, output_dir,
                            src_subdir="zephyr-qemu-zeroheap",
                            label="qemu-zephyr-zeroheap")


def test_qemu_freertos_linux_segv(ove_dir, output_dir):
    """Build the QEMU mps2-an500 FreeRTOS ARM_CM4_MPU Linux personality and run the
    NEGATIVE isolation test: /usr/bin/segv deliberately writes kernel SRAM, which the
    MPU must contain — segv is killed (exit 139), the shell survives, the kernel does
    not fault. tests/sim/freertos-linux/segv_drive.py boots the firmware, logs in, runs
    segv, and asserts all of that (exit 0 = pass).

    Manual/opt-in — needs the embedded Buildroot rootfs.cpio carrying /usr/bin/segv (the
    overtos board's post-build compiles it) plus QEMU and the slow uClinux boot, so it
    is deliberately NOT in any auto-run group."""
    ove = os.path.join(ove_dir, ".venv", "bin", "ove")
    run([ove, "defconfig-fragments", "qemu.freertos.linux_interop"], cwd=ove_dir)
    # Regenerate after changing Kconfig: this workspace is shared with the
    # hard-float suite, which leaves generated/ selecting the hard guest and the
    # FP self-test. Without this the soft-guest build silently reuses them.
    run([ove, "configure"], cwd=ove_dir)
    run([ove, "build"], cwd=ove_dir)
    drive = os.path.join(ove_dir, "tests", "sim", "freertos-linux", "segv_drive.py")
    logdir = os.path.join(output_dir, "tests", "qemu-freertos-linux-segv")
    os.makedirs(logdir, exist_ok=True)
    log = os.path.join(logdir, "segv.log")
    # segv_drive.py emits no CMocka output, so _run_test_binary maps its exit code
    # (0 = contained / pass, non-zero = isolation broke / fail).
    return _run_test_binary([sys.executable, drive, log],
                            "qemu-freertos-linux-segv", cwd=ove_dir)


def test_qemu_freertos_linux_hardfloat(ove_dir, output_dir):
    """Build and run a hard-float FDPIC guest on the Cortex-M7 FreeRTOS host.

    fpcheck verifies all s0-s31 registers and FPSCR across repeated deferred and
    blocking syscalls, while normal completion proves the host remains live.
    Manual/opt-in because it needs Buildroot's separately generated
    output-hardfloat rootfs and the uClinux QEMU execution environment.
    """
    import kconfiglib

    ove = os.path.join(ove_dir, ".venv", "bin", "ove")
    run([ove, "defconfig-fragments", "qemu.freertos.linux_interop"], cwd=ove_dir)

    # The fragment command intentionally defaults to the broadly compatible
    # soft guest. Select the independent hard guest ABI; the hidden rootfs
    # output symbol must follow that choice even after loading a saved .config.
    os.environ["srctree"] = ove_dir
    kconf = kconfiglib.Kconfig(os.path.join(ove_dir, "Config.in"))
    config_path = os.path.join(ove_dir, ".config")
    kconf.load_config(config_path)
    kconf.syms["OVE_LINUX_GUEST_FLOAT_ABI_HARD"].set_value(2)
    kconf.syms["OVE_LINUX_GUEST_FP_SELFTEST"].set_value(2)
    rootfs_output = kconf.syms["OVE_LINUX_ROOTFS_OUTPUT"].str_value
    if rootfs_output != "output-hardfloat":
        raise RuntimeError(f"hard-float guest selected {rootfs_output!r} rootfs")
    kconf.write_config(config_path)

    # Regenerate the CMake/header inputs after changing Kconfig. An existing
    # workspace may otherwise retain a previously generated soft-guest file.
    run([ove, "configure"], cwd=ove_dir)
    run([ove, "build"], cwd=ove_dir)
    drive = os.path.join(ove_dir, "tests", "sim", "freertos-linux",
                         "fpcheck_drive.py")
    logdir = os.path.join(output_dir, "tests", "qemu-freertos-linux-hardfloat")
    os.makedirs(logdir, exist_ok=True)
    log = os.path.join(logdir, "fpcheck.log")
    return _run_test_binary([sys.executable, drive, log],
                            "qemu-freertos-linux-hardfloat", cwd=ove_dir)


def test_linux_abi_switch(ove_dir, output_dir):
    """Switch the Linux guest ABI soft -> hard -> soft in one checkout, no clean.

    Both float ABIs live inside .config and appear in no path, so a soft and a
    hard build used to overwrite the same images/firmware.elf with nothing
    recording which was which. Asserts each leg lands in its own image
    directory, that image-id.json names the ABI and rootfs actually built, and
    that the two ABIs' images coexist.

    Builds only — no QEMU — but manual/opt-in: three full firmware builds, and
    the hard leg needs Buildroot's separately generated output-hardfloat rootfs.
    """
    drive = os.path.join(ove_dir, "tests", "sim", "freertos-linux",
                         "abi_switch_drive.py")
    logdir = os.path.join(output_dir, "tests", "linux-abi-switch")
    os.makedirs(logdir, exist_ok=True)
    log = os.path.join(logdir, "abi_switch.log")
    # The driver prints no CMocka output, so _run_test_binary maps its exit code.
    return _run_test_binary([sys.executable, drive, log],
                            "linux-abi-switch", cwd=ove_dir)


def test_qemu_freertos_linux_fbtest(ove_dir, output_dir):
    """Build the QEMU mps2-an500 FreeRTOS Linux personality and run the /dev/fb0
    framebuffer smoke: /usr/bin/fbtest reads the panel geometry via FBIOGET_*SCREENINFO,
    fills an RGB565 gradient, and verifies a pwrite()/pread() round-trip (LVGL's fbdev
    write path). tests/sim/freertos-linux/fbtest_drive.py boots, logs in, runs fbtest,
    and asserts geometry + PASS + a crw /dev/fb0 node + shell survival (exit 0 = pass).

    Manual/opt-in — needs the embedded Buildroot rootfs.cpio carrying /usr/bin/fbtest
    (the overtos board's post-build compiles it) plus QEMU and the slow uClinux boot."""
    ove = os.path.join(ove_dir, ".venv", "bin", "ove")
    run([ove, "defconfig-fragments", "qemu.freertos.linux_interop"], cwd=ove_dir)
    run([ove, "build"], cwd=ove_dir)
    drive = os.path.join(ove_dir, "tests", "sim", "freertos-linux", "fbtest_drive.py")
    logdir = os.path.join(output_dir, "tests", "qemu-freertos-linux-fbtest")
    os.makedirs(logdir, exist_ok=True)
    log = os.path.join(logdir, "fbtest.log")
    return _run_test_binary([sys.executable, drive, log],
                            "qemu-freertos-linux-fbtest", cwd=ove_dir)


def test_qemu_freertos_linux_lvbench(ove_dir, output_dir):
    """Build the QEMU mps2-an500 FreeRTOS Linux personality and run the LVGL
    benchmark: /usr/bin/lvbench (a stock LVGL fbdev program, lv_demo_benchmark)
    renders every scene to /dev/fb0 as an FDPIC Linux binary, then prints an
    FPS/CPU/render summary. tests/sim/freertos-linux/lvbench_drive.py boots, logs
    in, runs it, and asserts completion with a non-zero FPS + shell survival.

    Manual/opt-in — needs the embedded Buildroot rootfs.cpio carrying /usr/bin/lvbench
    (the overtos-lvbench package) plus QEMU and the slow uClinux boot + benchmark run."""
    ove = os.path.join(ove_dir, ".venv", "bin", "ove")
    run([ove, "defconfig-fragments", "qemu.freertos.linux_interop"], cwd=ove_dir)
    run([ove, "build"], cwd=ove_dir)
    drive = os.path.join(ove_dir, "tests", "sim", "freertos-linux", "lvbench_drive.py")
    logdir = os.path.join(output_dir, "tests", "qemu-freertos-linux-lvbench")
    os.makedirs(logdir, exist_ok=True)
    log = os.path.join(logdir, "lvbench.log")
    return _run_test_binary([sys.executable, drive, log],
                            "qemu-freertos-linux-lvbench", cwd=ove_dir)


def test_qemu_freertos_linux_evread(ove_dir, output_dir):
    """Build the QEMU mps2-an500 FreeRTOS Linux personality and run the evdev touch
    smoke: /usr/bin/evread blocking-reads /dev/input/event0 and prints the touch
    coordinates the testpad injector feeds — exercising the evdev class, the input
    feeder, and the blocking-read park/retry + coordinator-kick path.
    tests/sim/freertos-linux/evread_drive.py asserts the 16-byte event layout,
    several touch reports, and shell survival.

    Manual/opt-in — needs the embedded Buildroot rootfs.cpio carrying /usr/bin/evread."""
    ove = os.path.join(ove_dir, ".venv", "bin", "ove")
    run([ove, "defconfig-fragments", "qemu.freertos.linux_interop"], cwd=ove_dir)
    run([ove, "build"], cwd=ove_dir)
    drive = os.path.join(ove_dir, "tests", "sim", "freertos-linux", "evread_drive.py")
    logdir = os.path.join(output_dir, "tests", "qemu-freertos-linux-evread")
    os.makedirs(logdir, exist_ok=True)
    log = os.path.join(logdir, "evread.log")
    return _run_test_binary([sys.executable, drive, log],
                            "qemu-freertos-linux-evread", cwd=ove_dir)


def test_qemu_nuttx_linux_segv(ove_dir, output_dir):
    """Build the QEMU mps2-an500 NuttX Linux personality and run the NEGATIVE isolation test:
    /usr/bin/segv deliberately writes kernel SRAM, which the per-program MPU view must contain —
    the UNPRIVILEGED program is killed (exit 139), the shell survives, the kernel does not fault.
    tests/sim/nuttx-linux/segv_drive.py boots the firmware, logs in, runs segv, and asserts all of
    that (exit 0 = pass).

    Manual/opt-in — needs the embedded Buildroot rootfs.cpio carrying /usr/bin/segv plus QEMU and
    the slow uClinux boot, so it is deliberately NOT in any auto-run group."""
    ove = os.path.join(ove_dir, ".venv", "bin", "ove")
    run([ove, "defconfig-fragments", "qemu.nuttx.linux_interop"], cwd=ove_dir)
    run([ove, "build"], cwd=ove_dir)
    drive = os.path.join(ove_dir, "tests", "sim", "nuttx-linux", "segv_drive.py")
    logdir = os.path.join(output_dir, "tests", "qemu-nuttx-linux-segv")
    os.makedirs(logdir, exist_ok=True)
    log = os.path.join(logdir, "segv.log")
    return _run_test_binary([sys.executable, drive, log],
                            "qemu-nuttx-linux-segv", cwd=ove_dir)


def test_qemu_nuttx_linux_xregion(ove_dir, output_dir):
    """Build the QEMU mps2-an500 NuttX Linux personality and run the INTER-PROGRAM isolation test
    (Phase 2): /usr/bin/xregion deliberately writes a SIBLING program's pool region, which the
    privileged-only whole-pool base MPU region (unpriv-NO) must contain — the UNPRIVILEGED program
    is killed (exit 139), the shell survives, the kernel does not fault. Where segv writes kernel
    SRAM (denied by the ARM default map), xregion exercises the per-program base-region deny that
    segv never reaches. tests/sim/nuttx-linux/xregion_drive.py boots, logs in, runs xregion, and
    asserts all of that (exit 0 = pass).

    Manual/opt-in — needs the embedded Buildroot rootfs.cpio carrying /usr/bin/xregion plus QEMU and
    the slow uClinux boot, so it is deliberately NOT in any auto-run group."""
    ove = os.path.join(ove_dir, ".venv", "bin", "ove")
    run([ove, "defconfig-fragments", "qemu.nuttx.linux_interop"], cwd=ove_dir)
    run([ove, "build"], cwd=ove_dir)
    drive = os.path.join(ove_dir, "tests", "sim", "nuttx-linux", "xregion_drive.py")
    logdir = os.path.join(output_dir, "tests", "qemu-nuttx-linux-xregion")
    os.makedirs(logdir, exist_ok=True)
    log = os.path.join(logdir, "xregion.log")
    return _run_test_binary([sys.executable, drive, log],
                            "qemu-nuttx-linux-xregion", cwd=ove_dir)


def test_qemu_zephyr_linux_segv(ove_dir, output_dir):
    """Build the QEMU mps2-an521 Zephyr Linux personality and run the NEGATIVE isolation test:
    /usr/bin/segv deliberately writes kernel SRAM, outside its K_USER MPU domain. Zephyr's default
    fatal handler would HALT the whole system; the personality's k_sys_fatal_error_handler override
    contains it instead — the UNPRIVILEGED program is killed (exit 139), the shell survives, exactly
    like the FreeRTOS/NuttX MemManage handlers. tests/sim/zephyr-linux/segv_drive.py boots, logs in,
    runs segv, and asserts all of that (exit 0 = pass).

    Uses the FULL board name (qemu-mps2-an521.zephyr.linux_interop) — `qemu.zephyr` prefix-matches the
    an500 and would drop USERSPACE. Manual/opt-in — needs the embedded Buildroot rootfs.cpio carrying
    /usr/bin/segv plus QEMU and the slow uClinux boot, so it is deliberately NOT in any auto-run
    group."""
    ove = os.path.join(ove_dir, ".venv", "bin", "ove")
    run([ove, "defconfig-fragments", "qemu-mps2-an521.zephyr.linux_interop"], cwd=ove_dir)
    run([ove, "build"], cwd=ove_dir)
    drive = os.path.join(ove_dir, "tests", "sim", "zephyr-linux", "segv_drive.py")
    logdir = os.path.join(output_dir, "tests", "qemu-zephyr-linux-segv")
    os.makedirs(logdir, exist_ok=True)
    log = os.path.join(logdir, "segv.log")
    return _run_test_binary([sys.executable, drive, log],
                            "qemu-zephyr-linux-segv", cwd=ove_dir)


def _linux_kstress(ove_dir, output_dir, board_frag, engine):
    """Kernel-hardening regression: /usr/bin/kstress ptr (access_ok rejects a bad syscall pointer with
    -EFAULT, so the privileged handler never dereferences it) + udf (a UsageFault is contained, exit
    139, not a kernel panic) — the shell survives both. Proves the syscall-boundary + fault-type
    hardening. Manual/opt-in — needs the embedded rootfs carrying /usr/bin/kstress plus QEMU and the
    slow uClinux boot, so it is deliberately NOT in any auto-run group."""
    ove = os.path.join(ove_dir, ".venv", "bin", "ove")
    run([ove, "defconfig-fragments", board_frag], cwd=ove_dir)
    run([ove, "build"], cwd=ove_dir)
    drive = os.path.join(ove_dir, "tests", "sim", engine + "-linux", "kstress_drive.py")
    logdir = os.path.join(output_dir, "tests", "qemu-" + engine + "-linux-kstress")
    os.makedirs(logdir, exist_ok=True)
    log = os.path.join(logdir, "kstress.log")
    return _run_test_binary([sys.executable, drive, log],
                            "qemu-" + engine + "-linux-kstress", cwd=ove_dir)


def test_qemu_nuttx_linux_kstress(ove_dir, output_dir):
    return _linux_kstress(ove_dir, output_dir, "qemu.nuttx.linux_interop", "nuttx")


def test_qemu_zephyr_linux_kstress(ove_dir, output_dir):
    return _linux_kstress(ove_dir, output_dir, "qemu-mps2-an521.zephyr.linux_interop", "zephyr")


def test_qemu_freertos_linux_kstress(ove_dir, output_dir):
    return _linux_kstress(ove_dir, output_dir, "qemu.freertos.linux_interop", "freertos")


def test_qemu_zephyr_coverage(ove_dir, output_dir):
    """Build and run Zephyr QEMU tests with CONFIG_COVERAGE=y; emit lcov.

    Zephyr's CONFIG_COVERAGE_SEMIHOST routes gcov_coverage_semihost() (called
    automatically when main() returns, see kernel/init.c) through ARM
    semihosting — so QEMU writes .gcda files directly to the host build dir
    alongside each .gcno, without touching the console stream. We then run
    `lcov --capture` on the build dir and filter non-oveRTOS paths.
    """
    label = "qemu-zephyr-coverage"
    overlay = os.path.join(ove_dir, "tests", "sim", "zephyr-qemu",
                            "overlay-coverage.conf")
    result = _run_zephyr_qemu(ove_dir, output_dir,
                              src_subdir="zephyr-qemu", label=label,
                              extra_conf=overlay)
    build = os.path.join(output_dir, "tests", label)
    cov_dir = os.path.join(output_dir, "tests", "qemu_zephyr_coverage")
    os.makedirs(cov_dir, exist_ok=True)
    raw = os.path.join(cov_dir, "coverage.info")
    filtered = os.path.join(cov_dir, "coverage.filtered.info")

    # Zephyr compiles with its own arm-zephyr-eabi GCC (Zephyr SDK, typically
    # GCC 13), not our arm-none-eabi-gcc 15, so the gcda .gcno version tags
    # won't match if we capture with our toolchain. Derive gcov from the
    # compiler Zephyr actually used.
    gcov = None
    cache = os.path.join(build, "CMakeCache.txt")
    if os.path.isfile(cache):
        with open(cache) as f:
            for line in f:
                if line.startswith("CMAKE_C_COMPILER:"):
                    cc = line.split("=", 1)[1].strip()
                    gcov_candidate = cc[:-3] + "gcov" if cc.endswith("gcc") \
                        else cc + "-gcov"
                    if os.path.isfile(gcov_candidate):
                        gcov = gcov_candidate
                    break
    if not gcov:
        logger.error("Could not locate Zephyr SDK gcov; aborting capture")
        return result
    lcov = shutil.which("lcov")
    if not lcov:
        logger.error("lcov not installed; skipping capture")
        return result
    # `negative`: Zephyr's own coverage.c is instrumented while being the thing
    # that dumps gcda — the recursive counting occasionally produces a negative
    # delta for one of its own lines, which we drop along with non-oveRTOS
    # sources in the filter step below.
    run([lcov, "--capture", "--directory", build,
         "--gcov-tool", gcov,
         "--output-file", raw, "--rc", "branch_coverage=1",
         "--ignore-errors",
         "mismatch,source,gcov,unused,inconsistent,negative"])
    _lcov_filter_ove_sources(lcov, raw, filtered, ove_dir)
    logger.info("Zephyr QEMU coverage: %s", filtered)
    return result


def _compile_probe(label, cmd, *, env=None, cwd=None):
    """Run a compile-only command; return (ok, combined_output).

    Never raises — unlike `run()` — so all three bindings get probed even
    when one fails, and every failure is reported together.
    """
    logger.info("lvgl-compile: %s", label)
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, env=env, cwd=cwd)
    if p.returncode != 0:
        print(p.stdout, end="")
    return p.returncode == 0, (p.stdout or "")


def test_lvgl_bindings_compile(ove_dir, output_dir):
    """Compile-check the C++/Rust/Zig LVGL bindings against the REAL
    vendored LVGL (dl/lvgl) + the host lv_conf, not the test stub header.

    Why this exists: the binding wrappers are otherwise only ever compiled
    against the hand-written stub `tests/backends/stub/lvgl/lvgl.h` (during
    `make test-{rust,zig,cpp}`, whose `ove_config.h` sets CONFIG_OVE_LVGL).
    The stub declares "just enough for bindgen", so it can neither catch a
    wrapper that calls an `lv_*` function with the wrong signature nor
    detect stub drift from upstream LVGL.  Pointing each binding's compile
    at `dl/lvgl` makes a bad `lv_*` call fail here against ground truth.

    Compile-only (no run): it validates the binding *surface*, not runtime
    behaviour.  C++ is `-fsyntax-only`; Zig is `build-obj`; Rust is
    `cargo check` reusing the stub-build env from `_rust_test_env` with the
    LVGL include path swapped from the stub to `dl/lvgl`.
    """
    res = TestResults(suite="lvgl-compile")
    lvgl_dir = os.path.join(ove_dir, "dl", "lvgl")
    if not os.path.isfile(os.path.join(lvgl_dir, "lvgl.h")):
        logger.warning("lvgl-compile: dl/lvgl not present — skipping")
        res.skipped = 3
        return res

    host_lv_conf = os.path.join(ove_dir, "boards", "host", "posix")
    # tests/ove_config.h carries CONFIG_OVE_LVGL=1 + the host-stub backend,
    # so it is a complete host config for a compile-only check.
    gen_dir = os.path.join(ove_dir, "tests")

    def _inc(*parts):
        return "-I" + os.path.join(ove_dir, *parts)

    # The Zig binding backs Animation with a fixed byte buffer because
    # @cImport renders lv_anim_t opaque (inline union + bitfields).  Pull
    # its size out of the Zig source and static-assert it against real
    # LVGL below, so the buffer can never silently under-size.
    anim_buf = 256
    try:
        zsrc = Path(ove_dir, "bindings", "zig", "ove", "src", "lvgl.zig").read_text()
        m = re.search(r"pub const ANIM_STORAGE_SIZE\s*=\s*(\d+)", zsrc)
        if m:
            anim_buf = int(m.group(1))
    except OSError:
        pass

    # ── C++ : syntax-only compile of a TU that includes the binding ──
    probe_cpp = os.path.join(output_dir, "tests", "lvgl_compile_probe.cpp")
    os.makedirs(os.path.dirname(probe_cpp), exist_ok=True)
    with open(probe_cpp, "w") as f:
        f.write(
            "#include <ove/lvgl.hpp>\n"
            f"static_assert(sizeof(lv_anim_t) <= {anim_buf},\n"
            '    "ANIM_STORAGE_SIZE in bindings/zig/ove/src/lvgl.zig is '
            'smaller than sizeof(lv_anim_t) against real LVGL — enlarge it");\n')
    ok, _ = _compile_probe("cpp", [
        "g++", "-std=c++20", "-fsyntax-only", "-DCONFIG_OVE_LVGL=1",
        _inc("bindings", "cpp"), _inc("include"), "-I" + gen_dir,
        _inc("dl"), "-I" + host_lv_conf, _inc("backends", "posix", "include"),
        probe_cpp,
    ])
    res.passed += ok
    if not ok:
        res.failed += 1
        res.failed_names.append("cpp")

    # ── Zig : build-obj of the binding against real LVGL ──
    # Zig analyses function bodies lazily — `build-obj root.zig` skips any
    # wrapper method that isn't referenced, so a typo in an uncalled method
    # would slip through.  Compile a probe that force-references every decl
    # in the lvgl module (refAllDeclsRecursive), which instantiates the
    # comptime mixins and analyses each method body.
    zig = _find_zig(ove_dir)
    # std.testing.refAllDecls is a no-op outside test builds AND
    # non-recursive, so roll our own: ref every decl (forcing fn/const
    # analysis) and recurse into nested type decls so each widget's method
    # bodies — including the comptime-mixin re-exports — get analysed.
    zig_probe = os.path.join(output_dir, "tests", "lvgl_compile_probe.zig")
    with open(zig_probe, "w") as f:
        f.write(
            'const std = @import("std");\n'
            'const ove = @import("ove");\n'
            'fn refAll(comptime T: type) void {\n'
            '    inline for (comptime std.meta.declarations(T)) |decl| {\n'
            '        _ = &@field(T, decl.name);\n'
            '        const D = @field(T, decl.name);\n'
            '        if (@TypeOf(D) == type) switch (@typeInfo(D)) {\n'
            '            .@"struct", .@"enum", .@"union", .@"opaque" '
            '=> refAll(D),\n'
            '            else => {},\n'
            '        };\n'
            '    }\n'
            '}\n'
            'comptime {\n'
            '    @setEvalBranchQuota(1000000);\n'
            '    refAll(ove.lvgl);\n'
            '}\n')
    zig_obj = os.path.join(output_dir, "tests", "lvgl_compile_zig.o")
    ok, _ = _compile_probe("zig", [
        zig, "build-obj", "-target", "native-native-gnu", "-OReleaseSafe",
        "--dep", "ove",
        "-Mroot=" + zig_probe,
        "-Move=" + os.path.join(ove_dir, "bindings", "zig", "ove", "src",
                                "root.zig"),
        _inc("include"), _inc("backends", "posix", "include"),
        _inc("dl", "lvgl"), _inc("dl"), "-I" + host_lv_conf, "-I" + gen_dir,
        "-lc",  # add libc include paths so @cImport finds <stdio.h> etc.
        "-femit-bin=" + zig_obj,
    ])
    res.passed += ok
    if not ok:
        res.failed += 1
        res.failed_names.append("zig")

    # ── Rust : cargo check the binding crate against real LVGL ──
    # Reuse the stub-build env (builds rust_stub + generates ove_config.h),
    # then swap the LVGL include path from the stub to dl/lvgl.
    rust_target = os.path.join(output_dir, "tests", "lvgl_compile_rust")
    _rust_dir, env = _rust_test_env(ove_dir, output_dir, rust_target)
    env["LVGL_INCLUDE_PATH"] = lvgl_dir
    env["LVGL_PARENT_PATH"] = os.path.join(ove_dir, "dl")
    ok, _ = _compile_probe("rust", [
        "cargo", "check", "--manifest-path",
        os.path.join(ove_dir, "bindings", "rust", "ove", "Cargo.toml"),
        "--features", "std",
    ], env=env, cwd=ove_dir)
    res.passed += ok
    if not ok:
        res.failed += 1
        res.failed_names.append("rust")

    logger.info("lvgl-compile: %d/3 bindings compiled against real LVGL",
                res.passed)
    return res


# Test name -> function mapping
TEST_TARGETS = {
    "stub": test_stub,
    "lvgl-compile": test_lvgl_bindings_compile,
    "stub-sanitize": test_stub_sanitize,
    "stub-sanitize-zh": test_stub_sanitize_zh,
    "stub-tsan": test_stub_tsan,
    "stub-msan": test_stub_msan,
    "cpp": test_cpp,
    "cpp-sanitize": test_cpp_sanitize,
    "cpp-sanitize-zh": test_cpp_sanitize_zh,
    "cpp-tsan": test_cpp_tsan,
    "rust": test_rust,
    "rust-zeroheap": test_rust_zeroheap,
    "rust-coverage": test_rust_coverage,
    "zig": test_zig,
    "zig-zeroheap": test_zig_zeroheap,
    "zig-debug": test_zig_debug,
    "zig-coverage": test_zig_coverage,
    "nuttx": test_nuttx,
    "nuttx-coverage": test_nuttx_coverage,
    "zephyr": test_zephyr,
    "zephyr-coverage": test_zephyr_coverage,
    "qemu-freertos": test_qemu_freertos,
    "qemu-freertos-zeroheap": test_qemu_freertos_zeroheap,
    "qemu-freertos-coverage": test_qemu_freertos_coverage,
    "qemu-nuttx": test_qemu_nuttx,
    "qemu-nuttx-zeroheap": test_qemu_nuttx_zeroheap,
    "qemu-nuttx-coverage": test_qemu_nuttx_coverage,
    "qemu-zephyr": test_qemu_zephyr,
    "qemu-zephyr-zeroheap": test_qemu_zephyr_zeroheap,
    "qemu-freertos-linux-segv": test_qemu_freertos_linux_segv,
    "qemu-freertos-linux-hardfloat": test_qemu_freertos_linux_hardfloat,
    "linux-abi-switch": test_linux_abi_switch,
    "qemu-freertos-linux-fbtest": test_qemu_freertos_linux_fbtest,
    "qemu-freertos-linux-lvbench": test_qemu_freertos_linux_lvbench,
    "qemu-freertos-linux-evread": test_qemu_freertos_linux_evread,
    "qemu-nuttx-linux-segv": test_qemu_nuttx_linux_segv,
    "qemu-nuttx-linux-xregion": test_qemu_nuttx_linux_xregion,
    "qemu-zephyr-linux-segv": test_qemu_zephyr_linux_segv,
    "qemu-nuttx-linux-kstress": test_qemu_nuttx_linux_kstress,
    "qemu-zephyr-linux-kstress": test_qemu_zephyr_linux_kstress,
    "qemu-freertos-linux-kstress": test_qemu_freertos_linux_kstress,
    "qemu-zephyr-coverage": test_qemu_zephyr_coverage,
    "renode-stm32f746-freertos": test_renode_stm32f746_freertos,
    "renode-stm32f746-freertos-zeroheap": test_renode_stm32f746_freertos_zeroheap,
    "renode-stm32f746-zephyr": test_renode_stm32f746_zephyr,
    "renode-stm32f746-zephyr-zeroheap": test_renode_stm32f746_zephyr_zeroheap,
    "renode-stm32f746-nuttx": test_renode_stm32f746_nuttx,
    "renode-stm32f746-nuttx-zeroheap": test_renode_stm32f746_nuttx_zeroheap,
    # Hardware-in-the-loop targets — manual only.  Deliberately absent
    # from any group list below so `ove test all` and CI never run them.
    "hw-stm32f746-freertos": test_hw_stm32f746_freertos,
    "hw-stm32f746-freertos-zeroheap": test_hw_stm32f746_freertos_zeroheap,
    "hw-stm32f746-zephyr": test_hw_stm32f746_zephyr,
    "hw-stm32f746-zephyr-zeroheap": test_hw_stm32f746_zephyr_zeroheap,
    "hw-stm32f746-nuttx": test_hw_stm32f746_nuttx,
    "hw-stm32f746-nuttx-zeroheap": test_hw_stm32f746_nuttx_zeroheap,
}

# Grouped test sets
SIM_TESTS = ["stub", "cpp", "rust", "zig", "nuttx", "zephyr"]
QEMU_TESTS = ["qemu-freertos", "qemu-freertos-zeroheap", "qemu-nuttx",
               "qemu-nuttx-zeroheap",
               "qemu-zephyr", "qemu-zephyr-zeroheap"]
RENODE_TESTS = ["renode-stm32f746-freertos",
                "renode-stm32f746-freertos-zeroheap",
                "renode-stm32f746-zephyr",
                "renode-stm32f746-zephyr-zeroheap",
                "renode-stm32f746-nuttx",
                "renode-stm32f746-nuttx-zeroheap"]
# HW_TESTS is deliberately a separate group, NOT included in `all`.
# Invoke via `ove test hw` (or by individual name) — the user must
# have a board attached and OVE_HW_SERIAL_PORT set.
HW_TESTS = ["hw-stm32f746-freertos",
            "hw-stm32f746-freertos-zeroheap",
            "hw-stm32f746-zephyr",
            "hw-stm32f746-zephyr-zeroheap",
            "hw-stm32f746-nuttx",
            "hw-stm32f746-nuttx-zeroheap"]


def _save_terminal():
    """Save terminal settings; returns state or None if not a tty."""
    try:
        import termios
        fd = sys.stdin.fileno()
        return termios.tcgetattr(fd)
    except Exception:
        return None


def _restore_terminal(state):
    """Restore terminal settings saved by _save_terminal."""
    if state is None:
        return
    try:
        import termios
        fd = sys.stdin.fileno()
        termios.tcsetattr(fd, termios.TCSADRAIN, state)
    except Exception:
        pass


def cmd_test(args):
    """CLI entry point for 'ove test [name]'."""
    ove_dir = find_ove_dir()
    output_dir = os.path.join(ove_dir, "output")
    term_state = _save_terminal()

    names = args.names if args.names else SIM_TESTS

    # Expand group names
    expanded = []
    for n in names:
        if n == "all":
            # Intentionally excludes HW_TESTS — those need a board.
            expanded.extend(SIM_TESTS + QEMU_TESTS + RENODE_TESTS)
        elif n == "qemu":
            expanded.extend(QEMU_TESTS)
        elif n == "sim":
            expanded.extend(SIM_TESTS)
        elif n == "renode":
            expanded.extend(RENODE_TESTS)
        elif n == "hw":
            expanded.extend(HW_TESTS)
        else:
            expanded.append(n)

    results = []
    any_failed = False
    try:
        for name in expanded:
            func = TEST_TARGETS.get(name)
            if not func:
                logger.error(f"unknown test target '{name}'")
                print(f"Available: {', '.join(sorted(TEST_TARGETS.keys()))}")
                sys.exit(1)
            result = func(ove_dir, output_dir)
            if result:
                results.append(result)
                # A populated failed_names with failed==0 can occur when
                # _parse_cmocka captured a failure it couldn't fold into the
                # integer count (a cmocka GROUP SETUP/TEARDOWN error, which
                # cmocka reports as total_errors not total_failed) or a
                # driver appended a "<...>" marker. Treat that as a real
                # failure too — never let names-without-count slip past.
                if result.failed == 0 and result.failed_names:
                    result.failed = len(result.failed_names)
                if result.failed > 0:
                    any_failed = True
    finally:
        _restore_terminal(term_state)

    # Print summary table
    if results:
        print()
        print(f"{'Suite':<25} {'Passed':>8} {'Failed':>8} {'Skipped':>8}")
        print("-" * 51)
        total_p = total_f = total_s = 0
        for r in results:
            print(f"{r.suite:<25} {r.passed:>8} {r.failed:>8} {r.skipped:>8}")
            total_p += r.passed
            total_f += r.failed
            total_s += r.skipped
        print("-" * 51)
        print(f"{'TOTAL':<25} {total_p:>8} {total_f:>8} {total_s:>8}")

    if getattr(args, "json", False):
        import json
        json.dump({
            "suites": [
                {"suite": r.suite, "passed": r.passed, "failed": r.failed,
                 "skipped": r.skipped, "failed_names": r.failed_names}
                for r in results
            ],
            "total": {
                "passed": sum(r.passed for r in results),
                "failed": sum(r.failed for r in results),
                "skipped": sum(r.skipped for r in results),
            },
            "ok": not any_failed,
        }, sys.stdout, indent=2)
        print()

    if any_failed:
        # Print failed test names grouped by suite
        all_failures = []
        for r in results:
            if r.failed_names:
                all_failures.extend(
                    f"  {r.suite}: {name}" for name in r.failed_names)
            elif r.failed > 0 and not r.failed_names:
                all_failures.append(f"  {r.suite}: ({r.failed} failure(s))")
        if all_failures:
            print()
            print("Failed tests:")
            for line in all_failures:
                print(line)
        sys.exit(1)
