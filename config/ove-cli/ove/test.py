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
from dataclasses import dataclass
from pathlib import Path

from .utils import run, nproc, apply_defconfig_overlay
from .manifest import load_manifest, get_component
from .workspace import Workspace, find_ove_dir

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

    Sums all ``[  PASSED  ] N test(s).`` / ``[  FAILED  ] N test(s).``
    lines and collects individual failed test names from
    ``[  FAILED  ] test_name`` lines.
    """
    passed = failed = 0
    failed_names = []
    for line in stdout.splitlines():
        m = re.match(r'\[\s+PASSED\s+\]\s+(\d+)\s+test', line)
        if m:
            passed += int(m.group(1))
            continue
        m = re.match(r'\[\s+FAILED\s+\]\s+(\d+)\s+test', line)
        if m:
            failed += int(m.group(1))
            continue
        # Individual failure: "[  FAILED  ] test_name"  (no digit after])
        m = re.match(r'\[\s+FAILED\s+\]\s+([A-Za-z_]\w*)', line)
        if m:
            failed_names.append(m.group(1))
    return TestResults(suite="", passed=passed, failed=failed,
                       failed_names=failed_names)


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
    """Download ARM toolchain if needed, return path to toolchain dir."""
    from .download import download_toolchain

    dl_dir = os.path.join(ove_dir, "dl")
    toolchains_dir = os.path.join(ove_dir, "output", "toolchains")
    os.makedirs(dl_dir, exist_ok=True)
    os.makedirs(toolchains_dir, exist_ok=True)

    # Check sentinel first
    sentinel = os.path.join(toolchains_dir, "path.txt")
    if os.path.isfile(sentinel):
        tc_dir = Path(sentinel).read_text().strip()
        if os.path.isfile(os.path.join(tc_dir, "bin", "arm-none-eabi-gcc")):
            return tc_dir

    manifest = load_manifest(ove_dir)
    ok = download_toolchain({}, dl_dir, toolchains_dir, manifest=manifest)
    if not ok:
        logger.error("ARM toolchain download failed")
        sys.exit(1)

    tc_dir = Path(sentinel).read_text().strip()
    return tc_dir


def test_stub(ove_dir, output_dir):
    """Build and run stub backend tests."""
    build = os.path.join(output_dir, "tests", "stub")
    logger.info("Building stub tests")
    _cmake_build(os.path.join(ove_dir, "tests"), build)
    logger.info("Running stub tests")
    return _run_test_binary([os.path.join(build, "ove_test_stub")], "stub")


def test_cpp(ove_dir, output_dir):
    """Build and run C++ binding tests."""
    build = os.path.join(output_dir, "tests", "cpp")
    logger.info("Building C++ tests")
    _cmake_build(os.path.join(ove_dir, "tests", "cpp"), build)
    logger.info("Running C++ tests")
    return _run_test_binary([os.path.join(build, "ove_test_cpp")], "cpp")


def _rust_test_env(ove_dir, output_dir, target_dir):
    """Return (rust_dir, env) set up to build tests/rust/ against the
    rust_stub CMake library. Both test_rust and test_rust_coverage use this."""
    stub_build = os.path.join(output_dir, "tests", "rust_stub")
    _cmake_build(
        os.path.join(ove_dir, "tests", "rust", "stub_cmake"),
        stub_build)
    rust_dir = os.path.join(ove_dir, "tests", "rust")
    env = dict(os.environ)
    env.update({
        "OVE_DIR": ove_dir,
        "OVE_GEN_DIR": os.path.join(ove_dir, "tests"),
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
             "--instr-profile=" + profdata, binary],
            stdout=f, check=True)

    # Filter out /home/*/.cargo/registry, target/, and system headers so the
    # merged report only shows oveRTOS sources. subprocess passes args
    # directly to lcov — no shell quoting required.
    filtered = os.path.join(cov_dir, "coverage.filtered.info")
    run(["lcov",
         "--remove", info,
         "/usr/*",
         "*/.cargo/registry/*",
         "*/target/*",
         "*/rustc/*",
         "*/tests/*",
         "--output-file", filtered,
         "--ignore-errors", "unused,empty,format,inconsistent"])
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


def _build_zig_test_binary(ove_dir, output_dir, *, debug=False):
    """Build tests/zig/main.zig into an executable; return (exe_path, cwd).

    Shared by test_zig and test_zig_coverage. When `debug=True` we pass
    `-ODebug` so kcov's DWARF-driven source attribution produces useful
    line-level data instead of aggressive-inlined ReleaseSafe noise.
    """
    stub_build = os.path.join(output_dir, "tests", "zig_stub")
    logger.info("Building Zig stub library")
    _cmake_build(
        os.path.join(ove_dir, "tests", "rust", "stub_cmake"),
        stub_build)

    zig = _find_zig(ove_dir)
    zig_test_dir = os.path.join(ove_dir, "tests", "zig")
    zig_bindings = os.path.join(ove_dir, "bindings", "zig", "ove",
                                "src", "root.zig")
    zig_output = os.path.join(output_dir, "tests",
                              "zig_coverage" if debug else "zig")
    os.makedirs(zig_output, exist_ok=True)
    zig_exe = os.path.join(zig_output, "ove_test_zig")

    include_args = [
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
    # Zig 0.14+ defaults to the self-hosted x86_64 backend for Debug builds,
    # whose DWARF output kcov can't parse (Zig issue #25368). Force LLVM
    # codegen for coverage builds so kcov gets usable line tables.
    if debug:
        cmd.append("-fllvm")
    run(cmd, cwd=zig_test_dir)
    return zig_exe, zig_test_dir


def test_zig(ove_dir, output_dir):
    """Build and run Zig binding tests."""
    zig_exe, _ = _build_zig_test_binary(ove_dir, output_dir)
    logger.info("Running Zig tests")
    return _run_test_binary([zig_exe], "zig")


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

    Zig 0.15 has no native source-based coverage; we wrap the debug-built
    test binary with kcov (locally built per manifest, see _ensure_kcov).
    kcov walks DWARF via ptrace and writes cobertura.xml + HTML; we
    convert to lcov so the result merges with the other backends.

    The debug build is forced through the LLVM backend via `-fllvm` in
    `_build_zig_test_binary` — Zig 0.14+ defaults to the self-hosted
    x86_64 codegen for Debug, and its DWARF output is unreadable to kcov
    (Zig issue #25368).
    """
    kcov = _ensure_kcov(ove_dir)
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
        run([lcov, "--remove", raw_lcov,
             "/usr/*", f"{ove_dir}/dl/*", f"{ove_dir}/output/*",
             f"{ove_dir}/tests/*",
             "--output-file", filtered,
             "--ignore-errors", "unused,empty,format,inconsistent"])
    else:
        shutil.copy(raw_lcov, filtered)

    if n_files == 0:
        logger.warning(
            "kcov attributed 0 Zig source files — check that the zig "
            "build-exe invocation includes -fllvm so the x86_64 self-hosted "
            "backend isn't used (its DWARF is unreadable to kcov). "
            "Emitted empty tracefile: %s", filtered)
    else:
        logger.info("Zig coverage: %s (%d files)", filtered, n_files)
    return result


def _clean_gcda(root):
    """Remove stale .gcda under `root`. Needed before each NuttX sim
    coverage run because NuttX's Application.mk scatters .gcda next to the
    .o files (see the KNOWN WART note in tests/sim/nuttx-qemu/nuttx_app/
    Makefile) — i.e. throughout the oveRTOS source tree. Leftover counters
    from a previous run would poison the merge."""
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


def _run_nuttx_sim(ove_dir, output_dir, *, build_subdir, label,
                    coverage=False):
    """Shared driver for test_nuttx and test_nuttx_coverage.

    When `coverage=True` the NuttX app is built with `OVE_COVERAGE=1`, stale
    `.gcda` counters under OVE_DIR are removed before the run, and the
    `__gcov_dump()` hook in main.c flushes counters on clean exit. The
    scattered `.gcda` files are collected with `lcov --directory` in the
    caller.
    """
    import hashlib
    dl_dir = os.path.join(ove_dir, "dl")
    build_base = os.path.join(output_dir, "tests", build_subdir)

    manifest = load_manifest(ove_dir)
    default_tag = get_component(manifest, "rtos", "nuttx", "kernel", "version")
    nuttx_url = get_component(manifest, "rtos", "nuttx", "kernel", "url")
    apps_url = get_component(manifest, "rtos", "nuttx", "apps", "url")
    tag_hash = hashlib.sha256(default_tag.encode()).hexdigest()[:8]
    nuttx_build = os.path.join(build_base, "nuttx")
    apps_build = os.path.join(build_base, "nuttx-apps")

    logger.info("Building %s", label)
    os.makedirs(build_base, exist_ok=True)

    # Fetch NuttX
    nuttx_hash = os.path.join(dl_dir, f"nuttx-{tag_hash}")
    if not os.path.isdir(nuttx_hash):
        logger.debug(f"Cloning NuttX {default_tag}...")
        run(["git", "clone", "--depth", "1", "-b", default_tag,
              nuttx_url, nuttx_hash])
    link = os.path.join(dl_dir, "nuttx")
    if os.path.islink(link):
        os.unlink(link)
    os.symlink(nuttx_hash, link)

    # Fetch apps
    apps_hash = os.path.join(dl_dir, f"nuttx-apps-{tag_hash}")
    if not os.path.isdir(apps_hash):
        logger.debug(f"Cloning NuttX apps {default_tag}...")
        run(["git", "clone", "--depth", "1", "-b", default_tag,
              apps_url, apps_hash])
    link = os.path.join(dl_dir, "nuttx-apps")
    if os.path.islink(link):
        os.unlink(link)
    os.symlink(apps_hash, link)

    # Fetch CMocka
    cmocka_dl = os.path.join(dl_dir, "cmocka")
    if not os.path.isdir(cmocka_dl):
        logger.debug("Cloning CMocka...")
        run(["git", "clone", "--depth", "1", "-b", "cmocka-1.1.7",
              "https://gitlab.com/cmocka/cmocka.git", cmocka_dl])

    # Copy to build tree
    if not os.path.isdir(nuttx_build):
        logger.debug("Copying NuttX to build tree...")
        shutil.copytree(os.path.join(dl_dir, "nuttx"), nuttx_build,
                        symlinks=True)
    if not os.path.isdir(apps_build):
        logger.debug("Copying NuttX apps to build tree...")
        shutil.copytree(os.path.join(dl_dir, "nuttx-apps"), apps_build,
                        symlinks=True)

    # Register test app (reuse nuttx-qemu app)
    ext_dir = os.path.join(apps_build, "external")
    os.makedirs(ext_dir, exist_ok=True)
    test_dest = os.path.join(ext_dir, "ove_test")
    if os.path.exists(test_dest):
        shutil.rmtree(test_dest)
    shutil.copytree(
        os.path.join(ove_dir, "tests", "sim", "nuttx-qemu", "nuttx_app"),
        test_dest)
    with open(os.path.join(ext_dir, "Kconfig"), "w") as f:
        f.write(f'source "$APPSDIR/external/ove_test/Kconfig"\n')
    # NuttX sim tests still use Make (configure.sh + make) because
    # the NuttX sim architecture has limited CMake support upstream.
    with open(os.path.join(ext_dir, "Make.defs"), "w") as f:
        f.write('ifneq ($(CONFIG_EXTERNAL_OVE_TEST),)\n')
        f.write('CONFIGURED_APPS += $(APPDIR)/external/ove_test\n')
        f.write('endif\n')
    with open(os.path.join(ext_dir, "CMakeLists.txt"), "w") as f:
        f.write('add_subdirectory(ove_test)\n')

    # Configure for sim board
    # NuttX's configure.sh -> sethost.sh calls kconfig-tweak from PATH
    nuttx_env = _venv_env(ove_dir)
    flag = os.path.join(nuttx_build, ".ove_test_configured")
    if not os.path.isfile(flag):
        logger.debug("Configuring NuttX for sim:nsh...")
        run(["./tools/configure.sh", "-a", "../nuttx-apps",
              "sim:nsh"], cwd=nuttx_build, env=nuttx_env)
        Path(flag).write_text("configured\n")

    # Apply test defconfig overlay
    overlay = os.path.join(ove_dir, "tests", "sim", "nuttx",
                           "nuttx_sim_defconfig")
    nuttx_config = os.path.join(nuttx_build, ".config")
    apply_defconfig_overlay(nuttx_config, overlay)

    apps_abs = os.path.abspath(apps_build)
    nuttx_env["APPDIR"] = apps_abs
    run(["make", "olddefconfig"], cwd=nuttx_build, env=nuttx_env)

    # Build
    nuttx_env["OVE_DIR"] = ove_dir
    if coverage:
        nuttx_env["OVE_COVERAGE"] = "1"
        _clean_gcda(ove_dir)
    run(["make", f"-j{nproc()}"], cwd=nuttx_build, env=nuttx_env)

    logger.info("Running %s", label)
    # NuttX sim doesn't exit when init task returns; use timeout + output parsing
    try:
        result = subprocess.run(
            [os.path.join(nuttx_build, "nuttx")],
            timeout=60, capture_output=True, text=True)
        stdout = result.stdout
    except subprocess.TimeoutExpired as e:
        stdout = e.stdout.decode() if e.stdout else ""
    print(stdout, end="")

    parsed = _parse_cmocka(stdout)
    parsed.suite = label
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

    `.gcda` files scatter through the oveRTOS source tree (NuttX
    Application.mk wart), so lcov scans OVE_DIR for counters and writes
    the filtered tracefile into the coverage build dir.
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
         "--output-file", info,
         "--ignore-errors", "mismatch,gcov,source,empty,inconsistent"])
    run([lcov, "--remove", info,
         "/usr/*", f"{ove_dir}/dl/*", f"{ove_dir}/output/*",
         f"{ove_dir}/tests/*",
         "--output-file", filtered,
         "--ignore-errors", "unused,empty,format,inconsistent"])
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
    if os.path.islink(link):
        os.unlink(link)
    os.symlink(hash_dir, link)

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
         "--output-file", info,
         "--ignore-errors", "mismatch,gcov,source,empty,inconsistent"])
    run([lcov, "--remove", info,
         "/usr/*", "*/zephyr-workspace/*", "*/_deps/*", "*/tests/*",
         "--output-file", filtered,
         "--ignore-errors", "unused,empty,format,inconsistent"])
    logger.info("Zephyr coverage: %s", filtered)
    return result


# ── FreeRTOS QEMU shared driver ────────────────────────────────────────
def _run_freertos_qemu(ove_dir, output_dir, *, src_subdir, binary, label):
    """Build and run a FreeRTOS QEMU ARM test variant."""
    tc_dir = _ensure_arm_toolchain(ove_dir)
    build = os.path.join(output_dir, "tests", label)
    logger.info(f"Building {label}")
    _cmake_build(os.path.join(ove_dir, "tests", "sim", src_subdir),
                 build,
                 extra_args=[f"-DOVE_TOOLCHAIN_DIR={tc_dir}"])
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


# ── NuttX QEMU shared driver ───────────────────────────────────────────
def _run_nuttx_qemu(ove_dir, output_dir, *, app_subdir, label):
    """Build and run a NuttX QEMU ARM test variant.

    `app_subdir` is the tests/sim/<dir>/ holding nuttx_app/ and
    nuttx_test_defconfig (e.g. "nuttx-qemu" or "nuttx-qemu-zeroheap").
    """
    tc_dir = _ensure_arm_toolchain(ove_dir)
    import hashlib
    dl_dir = os.path.join(ove_dir, "dl")
    build_base = os.path.join(output_dir, "tests", label)

    manifest = load_manifest(ove_dir)
    default_tag = get_component(manifest, "rtos", "nuttx", "kernel", "version")
    nuttx_url = get_component(manifest, "rtos", "nuttx", "kernel", "url")
    apps_url = get_component(manifest, "rtos", "nuttx", "apps", "url")
    tag_hash = hashlib.sha256(default_tag.encode()).hexdigest()[:8]
    nuttx_build = os.path.join(build_base, "nuttx")
    apps_build = os.path.join(build_base, "nuttx-apps")

    logger.info(f"Building {label}")
    os.makedirs(build_base, exist_ok=True)

    # Fetch NuttX
    nuttx_hash = os.path.join(dl_dir, f"nuttx-{tag_hash}")
    if not os.path.isdir(nuttx_hash):
        logger.debug(f"Cloning NuttX {default_tag}...")
        run(["git", "clone", "--depth", "1", "-b", default_tag,
              nuttx_url, nuttx_hash])
    link = os.path.join(dl_dir, "nuttx")
    if os.path.islink(link):
        os.unlink(link)
    os.symlink(nuttx_hash, link)

    # Fetch apps
    apps_hash = os.path.join(dl_dir, f"nuttx-apps-{tag_hash}")
    if not os.path.isdir(apps_hash):
        logger.debug(f"Cloning NuttX apps {default_tag}...")
        run(["git", "clone", "--depth", "1", "-b", default_tag,
              apps_url, apps_hash])
    link = os.path.join(dl_dir, "nuttx-apps")
    if os.path.islink(link):
        os.unlink(link)
    os.symlink(apps_hash, link)

    # Fetch CMocka
    cmocka_dl = os.path.join(dl_dir, "cmocka")
    if not os.path.isdir(cmocka_dl):
        logger.debug("Cloning CMocka...")
        run(["git", "clone", "--depth", "1", "-b", "cmocka-1.1.7",
              "https://gitlab.com/cmocka/cmocka.git", cmocka_dl])

    # Copy to build tree
    if not os.path.isdir(nuttx_build):
        logger.debug("Copying NuttX to build tree...")
        shutil.copytree(os.path.join(dl_dir, "nuttx"), nuttx_build,
                        symlinks=True)
    if not os.path.isdir(apps_build):
        logger.debug("Copying NuttX apps to build tree...")
        shutil.copytree(os.path.join(dl_dir, "nuttx-apps"), apps_build,
                        symlinks=True)

    # Register test app
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
    # NuttX sim tests still use Make (configure.sh + make) because
    # the NuttX sim architecture has limited CMake support upstream.
    with open(os.path.join(ext_dir, "Make.defs"), "w") as f:
        f.write('ifneq ($(CONFIG_EXTERNAL_OVE_TEST),)\n')
        f.write('CONFIGURED_APPS += $(APPDIR)/external/ove_test\n')
        f.write('endif\n')
    with open(os.path.join(ext_dir, "CMakeLists.txt"), "w") as f:
        f.write('add_subdirectory(ove_test)\n')

    # Configure: configure.sh -> sethost.sh calls kconfig-tweak from PATH
    nuttx_env = _venv_env(ove_dir)
    tc_bin = os.path.join(tc_dir, "bin")
    nuttx_env["PATH"] = tc_bin + os.pathsep + nuttx_env["PATH"]
    flag = os.path.join(nuttx_build, ".ove_test_configured")
    if not os.path.isfile(flag):
        logger.debug("Configuring NuttX for mps2-an500:nsh...")
        run(["./tools/configure.sh", "-a", "../nuttx-apps",
              "mps2-an500:nsh"], cwd=nuttx_build, env=nuttx_env)
        Path(flag).write_text("configured\n")

    # Apply test defconfig overlay
    overlay = os.path.join(ove_dir, "tests", "sim", app_subdir,
                           "nuttx_test_defconfig")
    nuttx_config = os.path.join(nuttx_build, ".config")
    apply_defconfig_overlay(nuttx_config, overlay)

    apps_abs = os.path.abspath(apps_build)
    nuttx_env["APPDIR"] = apps_abs
    run(["make", "olddefconfig"], cwd=nuttx_build, env=nuttx_env)

    nuttx_env["OVE_DIR"] = ove_dir
    run(["make", f"-j{nproc()}"], cwd=nuttx_build, env=nuttx_env)

    logger.info(f"Running {label}")
    qemu_run = os.path.join(ove_dir, "boards", "qemu-mps2-an500",
                            "qemu-run.sh")
    return _run_test_binary(
        [qemu_run, os.path.join(nuttx_build, "nuttx"), "--headless",
         "--timeout", "45"], label)


# ── Zephyr QEMU shared driver ──────────────────────────────────────────
def _run_zephyr_qemu(ove_dir, output_dir, *, src_subdir, label):
    """Build and run a Zephyr QEMU ARM test variant."""
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
        run([west, "init", "-m",
              zephyr_url,
              "--mr", "main", hash_dir])
        run(["git", "-C", os.path.join(hash_dir, "zephyr"),
              "checkout", default_rev])
        run([west, "update"], cwd=hash_dir)

    link = os.path.join(build, "zephyr-workspace")
    if os.path.islink(link):
        os.unlink(link)
    os.symlink(hash_dir, link)

    env = dict(os.environ)
    env["ZEPHYR_BASE"] = os.path.join(link, "zephyr")
    run([
        west, "build",
        "-b", "mps2/an500",
        "-d", build,
        os.path.join(ove_dir, "tests", "sim", src_subdir),
    ], env=env)

    logger.info(f"Running {label}")
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


def test_qemu_zephyr(ove_dir, output_dir):
    """Build and run Zephyr QEMU ARM tests."""
    return _run_zephyr_qemu(ove_dir, output_dir,
                            src_subdir="zephyr-qemu", label="qemu-zephyr")


def test_qemu_zephyr_zeroheap(ove_dir, output_dir):
    """Build and run Zephyr QEMU ARM tests (zero-heap mode)."""
    return _run_zephyr_qemu(ove_dir, output_dir,
                            src_subdir="zephyr-qemu-zeroheap",
                            label="qemu-zephyr-zeroheap")


# Test name -> function mapping
TEST_TARGETS = {
    "stub": test_stub,
    "cpp": test_cpp,
    "rust": test_rust,
    "rust-coverage": test_rust_coverage,
    "zig": test_zig,
    "zig-coverage": test_zig_coverage,
    "nuttx": test_nuttx,
    "nuttx-coverage": test_nuttx_coverage,
    "zephyr": test_zephyr,
    "zephyr-coverage": test_zephyr_coverage,
    "qemu-freertos": test_qemu_freertos,
    "qemu-freertos-zeroheap": test_qemu_freertos_zeroheap,
    "qemu-nuttx": test_qemu_nuttx,
    "qemu-nuttx-zeroheap": test_qemu_nuttx_zeroheap,
    "qemu-zephyr": test_qemu_zephyr,
    "qemu-zephyr-zeroheap": test_qemu_zephyr_zeroheap,
}

# Grouped test sets
SIM_TESTS = ["stub", "cpp", "rust", "zig", "nuttx", "zephyr"]
QEMU_TESTS = ["qemu-freertos", "qemu-freertos-zeroheap", "qemu-nuttx",
               "qemu-nuttx-zeroheap", "qemu-zephyr", "qemu-zephyr-zeroheap"]


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
            expanded.extend(SIM_TESTS + QEMU_TESTS)
        elif n == "qemu":
            expanded.extend(QEMU_TESTS)
        elif n == "sim":
            expanded.extend(SIM_TESTS)
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
