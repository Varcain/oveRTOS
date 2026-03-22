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
from .constants import NUTTX_DEFAULT_TAG, ZEPHYR_DEFAULT_REV, ARM_TOOLCHAIN_URL
from .workspace import Workspace, find_ove_dir

logger = logging.getLogger("ove")


@dataclass
class TestResults:
    suite: str
    passed: int = 0
    failed: int = 0
    skipped: int = 0


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

    # Use workspace config if available, otherwise use constant default
    config = {}
    config_path = os.path.join(ove_dir, ".config")
    if os.path.isfile(config_path):
        with open(config_path) as f:
            for line in f:
                if line.startswith("CONFIG_OVE_TOOLCHAIN_URL="):
                    config["CONFIG_OVE_TOOLCHAIN_URL"] = (
                        line.split("=", 1)[1].strip().strip('"'))
    if "CONFIG_OVE_TOOLCHAIN_URL" not in config:
        config["CONFIG_OVE_TOOLCHAIN_URL"] = ARM_TOOLCHAIN_URL

    ok = download_toolchain(config, dl_dir, toolchains_dir)
    if not ok:
        logger.error("ARM toolchain download failed")
        sys.exit(1)

    tc_dir = Path(sentinel).read_text().strip()
    return tc_dir


def test_stub(ove_dir, output_dir):
    """Build and run stub backend tests."""
    build = os.path.join(output_dir, "test", "stub")
    logger.info("Building stub tests")
    _cmake_build(os.path.join(ove_dir, "tests"), build)
    logger.info("Running stub tests")
    run([os.path.join(build, "ove_test_stub")])
    return TestResults(suite="stub", passed=1, failed=0)


def test_cpp(ove_dir, output_dir):
    """Build and run C++ binding tests."""
    build = os.path.join(output_dir, "test", "cpp")
    logger.info("Building C++ tests")
    _cmake_build(os.path.join(ove_dir, "tests", "cpp"), build)
    logger.info("Running C++ tests")
    run([os.path.join(build, "ove_test_cpp")])
    return TestResults(suite="cpp", passed=1, failed=0)


def test_rust(ove_dir, output_dir):
    """Build and run Rust tests."""
    stub_build = os.path.join(output_dir, "test", "rust_stub")
    logger.info("Building Rust stub library")
    _cmake_build(
        os.path.join(ove_dir, "tests", "rust", "stub_cmake"),
        stub_build)

    logger.info("Building Rust tests")
    rust_dir = os.path.join(ove_dir, "tests", "rust")
    env = dict(os.environ)
    env.update({
        "OVE_DIR": ove_dir,
        "OVE_GEN_DIR": os.path.join(ove_dir, "tests"),
        "RUST_IS_NATIVE": "1",
        "STUB_LIB_DIR": stub_build,
        "OVE_STORAGE_SIZES": os.path.join(stub_build,
                                               "ove_storage_sizes.env"),
        "LV_CONF_PATH": os.path.join(ove_dir, "boards", "host-pc",
                                      "posix"),
        "LVGL_INCLUDE_PATH": os.path.join(ove_dir, "tests", "backends",
                                           "stub", "lvgl"),
        "LVGL_PARENT_PATH": os.path.join(ove_dir, "tests", "backends",
                                          "stub"),
    })
    run(["cargo", "build", "--release"], env=env, cwd=rust_dir)
    logger.info("Running Rust tests")
    run([os.path.join(rust_dir, "target", "release", "ove-tests")])
    return TestResults(suite="rust", passed=1, failed=0)


def _find_zig(ove_dir):
    """Find the zig binary — toolchain dir first, download if needed, then PATH."""
    import glob
    pattern = os.path.join(ove_dir, "output", "toolchains", "zig-*", "zig")
    matches = glob.glob(pattern)
    if matches:
        return matches[0]

    # Download the toolchain into output/toolchains like the example build does
    from .download import download_zig_toolchain
    dl_dir = os.path.join(ove_dir, "dl")
    toolchains_dir = os.path.join(ove_dir, "output", "toolchains")
    os.makedirs(dl_dir, exist_ok=True)
    os.makedirs(toolchains_dir, exist_ok=True)
    if download_zig_toolchain({}, dl_dir, toolchains_dir):
        matches = glob.glob(pattern)
        if matches:
            return matches[0]

    zig = shutil.which("zig")
    if zig:
        return zig
    logger.error("zig compiler not found")
    sys.exit(1)


def test_zig(ove_dir, output_dir):
    """Build and run Zig binding tests."""
    stub_build = os.path.join(output_dir, "test", "zig_stub")
    logger.info("Building Zig stub library")
    _cmake_build(
        os.path.join(ove_dir, "tests", "rust", "stub_cmake"),
        stub_build)

    zig = _find_zig(ove_dir)
    zig_test_dir = os.path.join(ove_dir, "tests", "zig")
    zig_bindings = os.path.join(ove_dir, "bindings", "zig", "ove",
                                "src", "root.zig")
    zig_output = os.path.join(output_dir, "test", "zig")
    os.makedirs(zig_output, exist_ok=True)
    zig_exe = os.path.join(zig_output, "ove_test_zig")

    # Include paths for @cImport
    include_args = [
        "-I" + os.path.join(ove_dir, "include"),
        "-I" + os.path.join(ove_dir, "tests"),
        "-I" + os.path.join(ove_dir, "backends", "posix", "include"),
        "-I" + os.path.join(ove_dir, "tests", "backends", "stub", "lvgl"),
        "-I" + os.path.join(ove_dir, "tests", "backends", "stub"),
        "-I" + os.path.join(ove_dir, "boards", "host-pc", "posix"),
    ]

    logger.info("Building Zig tests")
    cmd = [
        zig, "build-exe",
        "-target", "native-native-gnu",
        "-OReleaseSafe",
        "--dep", "ove",
        "-Mroot=" + os.path.join(zig_test_dir, "main.zig"),
        "-Move=" + zig_bindings,
    ] + include_args + [
        "-L" + stub_build, "-love_stub",
        "-lpthread", "-lrt",
        "-lc",
        "-femit-bin=" + zig_exe,
    ]
    run(cmd, cwd=zig_test_dir)

    logger.info("Running Zig tests")
    run([zig_exe])
    return TestResults(suite="zig", passed=1, failed=0)


def test_nuttx(ove_dir, output_dir):
    """Build and run NuttX sim tests (uses NuttX sim board)."""
    import hashlib
    dl_dir = os.path.join(ove_dir, "dl")
    build_base = os.path.join(output_dir, "test", "nuttx")

    default_tag = NUTTX_DEFAULT_TAG
    tag_hash = hashlib.sha256(default_tag.encode()).hexdigest()[:8]
    nuttx_build = os.path.join(build_base, "nuttx")
    apps_build = os.path.join(build_base, "nuttx-apps")

    logger.info("Building NuttX sim tests")
    os.makedirs(build_base, exist_ok=True)

    # Fetch NuttX
    nuttx_hash = os.path.join(dl_dir, f"nuttx-{tag_hash}")
    if not os.path.isdir(nuttx_hash):
        logger.debug(f"Cloning NuttX {default_tag}...")
        run(["git", "clone", "--depth", "1", "-b", default_tag,
              "https://github.com/apache/nuttx.git", nuttx_hash])
    link = os.path.join(dl_dir, "nuttx")
    if os.path.islink(link):
        os.unlink(link)
    os.symlink(nuttx_hash, link)

    # Fetch apps
    apps_hash = os.path.join(dl_dir, f"nuttx-apps-{tag_hash}")
    if not os.path.isdir(apps_hash):
        logger.debug(f"Cloning NuttX apps {default_tag}...")
        run(["git", "clone", "--depth", "1", "-b", default_tag,
              "https://github.com/apache/nuttx-apps.git", apps_hash])
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
    with open(os.path.join(ext_dir, "Make.defs"), "w") as f:
        f.write('ifneq ($(CONFIG_EXTERNAL_OVE_TEST),)\n')
        f.write('CONFIGURED_APPS += $(APPDIR)/external/ove_test\n')
        f.write('endif\n')

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
    run(["make", f"-j{nproc()}"], cwd=nuttx_build, env=nuttx_env)

    logger.info("Running NuttX sim tests")
    # NuttX sim doesn't exit when init task returns; use timeout + output parsing
    try:
        result = subprocess.run(
            [os.path.join(nuttx_build, "nuttx")],
            timeout=60, capture_output=True, text=True)
        stdout = result.stdout
    except subprocess.TimeoutExpired as e:
        stdout = e.stdout.decode() if e.stdout else ""
    print(stdout, end="")

    # Parse CMocka output: "[  PASSED  ] N test(s)." and "[  FAILED  ] N test(s)."
    passed_match = re.search(r'\[\s+PASSED\s+\]\s+(\d+)\s+test', stdout)
    failed_match = re.search(r'\[\s+FAILED\s+\]\s+(\d+)\s+test', stdout)
    passed = int(passed_match.group(1)) if passed_match else 0
    failed = int(failed_match.group(1)) if failed_match else 0

    if failed > 0 or passed == 0:
        logger.error("NuttX sim tests had failures")
        return TestResults(suite="nuttx-sim", passed=passed, failed=max(failed, 1))
    return TestResults(suite="nuttx-sim", passed=passed, failed=0)


def test_zephyr(ove_dir, output_dir):
    """Build and run Zephyr native_sim tests."""
    import hashlib
    build = os.path.join(output_dir, "test", "zephyr")
    dl_dir = os.path.join(ove_dir, "dl")
    west = os.path.join(ove_dir, ".venv", "bin", "west")

    default_rev = ZEPHYR_DEFAULT_REV
    dl_hash = hashlib.sha256(default_rev.encode()).hexdigest()[:8]

    logger.info("Building Zephyr native_sim tests")
    os.makedirs(build, exist_ok=True)

    hash_dir = os.path.join(dl_dir, f"zephyr-workspace-{dl_hash}")
    if not os.path.isdir(os.path.join(hash_dir, "zephyr")):
        logger.debug("Zephyr workspace not found -- downloading...")
        run([west, "init", "-m",
              "https://github.com/zephyrproject-rtos/zephyr.git",
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
    run([
        west, "build",
        "-b", "native_sim/native/64",
        "-d", build,
        os.path.join(ove_dir, "tests", "sim", "zephyr"),
    ], env=env)

    logger.info("Running Zephyr native_sim tests")
    run([os.path.join(build, "zephyr", "zephyr.exe")])
    return TestResults(suite="zephyr-native-sim", passed=1, failed=0)


def test_qemu_freertos(ove_dir, output_dir):
    """Build and run FreeRTOS QEMU ARM tests."""
    tc_dir = _ensure_arm_toolchain(ove_dir)
    build = os.path.join(output_dir, "test", "qemu-freertos")
    logger.info("Building FreeRTOS QEMU ARM tests")
    _cmake_build(os.path.join(ove_dir, "tests", "sim", "freertos-qemu"),
                 build,
                 extra_args=[f"-DOVE_TOOLCHAIN_DIR={tc_dir}"])
    logger.info("Running FreeRTOS QEMU ARM tests")
    qemu_run = os.path.join(ove_dir, "boards", "qemu-mps2-an500",
                            "qemu-run.sh")
    run([qemu_run, os.path.join(build, "ove_test_freertos_qemu"),
          "--headless", "--timeout", "45"])
    return TestResults(suite="qemu-freertos", passed=1, failed=0)


def test_qemu_freertos_zeroheap(ove_dir, output_dir):
    """Build and run FreeRTOS QEMU ARM tests (zero-heap mode)."""
    tc_dir = _ensure_arm_toolchain(ove_dir)
    build = os.path.join(output_dir, "test", "qemu-freertos-zeroheap")
    logger.info("Building FreeRTOS QEMU ARM tests (zero-heap)")
    _cmake_build(os.path.join(ove_dir, "tests", "sim", "freertos-qemu-zeroheap"),
                 build,
                 extra_args=[f"-DOVE_TOOLCHAIN_DIR={tc_dir}"])
    logger.info("Running FreeRTOS QEMU ARM tests (zero-heap)")
    qemu_run = os.path.join(ove_dir, "boards", "qemu-mps2-an500",
                            "qemu-run.sh")
    run([qemu_run, os.path.join(build, "ove_test_freertos_qemu_zeroheap"),
          "--headless", "--timeout", "45"])
    return TestResults(suite="qemu-freertos-zeroheap", passed=1, failed=0)


def test_qemu_nuttx(ove_dir, output_dir):
    """Build and run NuttX QEMU ARM tests (uses NuttX build system)."""
    tc_dir = _ensure_arm_toolchain(ove_dir)
    import hashlib
    dl_dir = os.path.join(ove_dir, "dl")
    build_base = os.path.join(output_dir, "test", "qemu-nuttx")

    default_tag = NUTTX_DEFAULT_TAG
    tag_hash = hashlib.sha256(default_tag.encode()).hexdigest()[:8]
    nuttx_build = os.path.join(build_base, "nuttx")
    apps_build = os.path.join(build_base, "nuttx-apps")

    logger.info("Building NuttX QEMU ARM tests")
    os.makedirs(build_base, exist_ok=True)

    # Fetch NuttX
    nuttx_hash = os.path.join(dl_dir, f"nuttx-{tag_hash}")
    if not os.path.isdir(nuttx_hash):
        logger.debug(f"Cloning NuttX {default_tag}...")
        run(["git", "clone", "--depth", "1", "-b", default_tag,
              "https://github.com/apache/nuttx.git", nuttx_hash])
    link = os.path.join(dl_dir, "nuttx")
    if os.path.islink(link):
        os.unlink(link)
    os.symlink(nuttx_hash, link)

    # Fetch apps
    apps_hash = os.path.join(dl_dir, f"nuttx-apps-{tag_hash}")
    if not os.path.isdir(apps_hash):
        logger.debug(f"Cloning NuttX apps {default_tag}...")
        run(["git", "clone", "--depth", "1", "-b", default_tag,
              "https://github.com/apache/nuttx-apps.git", apps_hash])
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
        os.path.join(ove_dir, "tests", "sim", "nuttx-qemu", "nuttx_app"),
        test_dest)
    with open(os.path.join(ext_dir, "Kconfig"), "w") as f:
        f.write(f'source "$APPSDIR/external/ove_test/Kconfig"\n')
    with open(os.path.join(ext_dir, "Make.defs"), "w") as f:
        f.write('ifneq ($(CONFIG_EXTERNAL_OVE_TEST),)\n')
        f.write('CONFIGURED_APPS += $(APPDIR)/external/ove_test\n')
        f.write('endif\n')

    # Configure
    # NuttX's configure.sh -> sethost.sh calls kconfig-tweak from PATH
    nuttx_env = _venv_env(ove_dir)
    # Prepend ARM toolchain to PATH so NuttX finds arm-none-eabi-gcc
    tc_bin = os.path.join(tc_dir, "bin")
    nuttx_env["PATH"] = tc_bin + os.pathsep + nuttx_env["PATH"]
    flag = os.path.join(nuttx_build, ".ove_test_configured")
    if not os.path.isfile(flag):
        logger.debug("Configuring NuttX for mps2-an500:nsh...")
        run(["./tools/configure.sh", "-a", "../nuttx-apps",
              "mps2-an500:nsh"], cwd=nuttx_build, env=nuttx_env)
        Path(flag).write_text("configured\n")

    # Apply test defconfig overlay
    overlay = os.path.join(ove_dir, "tests", "sim", "nuttx-qemu",
                           "nuttx_test_defconfig")
    nuttx_config = os.path.join(nuttx_build, ".config")
    apply_defconfig_overlay(nuttx_config, overlay)

    apps_abs = os.path.abspath(apps_build)
    nuttx_env["APPDIR"] = apps_abs
    run(["make", "olddefconfig"], cwd=nuttx_build, env=nuttx_env)

    # Build
    nuttx_env["OVE_DIR"] = ove_dir
    run(["make", f"-j{nproc()}"], cwd=nuttx_build, env=nuttx_env)

    logger.info("Running NuttX QEMU ARM tests")
    qemu_run = os.path.join(ove_dir, "boards", "qemu-mps2-an500",
                            "qemu-run.sh")
    run([qemu_run, os.path.join(nuttx_build, "nuttx"), "--headless",
          "--timeout", "45"])
    return TestResults(suite="qemu-nuttx", passed=1, failed=0)


def test_qemu_zephyr(ove_dir, output_dir):
    """Build and run Zephyr QEMU ARM tests."""
    import hashlib
    dl_dir = os.path.join(ove_dir, "dl")
    build = os.path.join(output_dir, "test", "qemu-zephyr")
    west = os.path.join(ove_dir, ".venv", "bin", "west")

    default_rev = ZEPHYR_DEFAULT_REV
    dl_hash = hashlib.sha256(default_rev.encode()).hexdigest()[:8]

    logger.info("Building Zephyr QEMU ARM tests")
    os.makedirs(build, exist_ok=True)

    hash_dir = os.path.join(dl_dir, f"zephyr-workspace-{dl_hash}")
    if not os.path.isdir(os.path.join(hash_dir, "zephyr")):
        logger.debug("Zephyr workspace not found -- downloading...")
        run([west, "init", "-m",
              "https://github.com/zephyrproject-rtos/zephyr.git",
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
        os.path.join(ove_dir, "tests", "sim", "zephyr-qemu"),
    ], env=env)

    logger.info("Running Zephyr QEMU ARM tests")
    qemu_run = os.path.join(ove_dir, "boards", "qemu-mps2-an500",
                            "qemu-run.sh")
    run([qemu_run, os.path.join(build, "zephyr", "zephyr.elf"),
          "--headless", "--timeout", "120"])
    return TestResults(suite="qemu-zephyr", passed=1, failed=0)


def test_qemu_nuttx_zeroheap(ove_dir, output_dir):
    """Build and run NuttX QEMU ARM tests (zero-heap mode)."""
    tc_dir = _ensure_arm_toolchain(ove_dir)
    import hashlib
    dl_dir = os.path.join(ove_dir, "dl")
    build_base = os.path.join(output_dir, "test", "qemu-nuttx-zeroheap")

    default_tag = NUTTX_DEFAULT_TAG
    tag_hash = hashlib.sha256(default_tag.encode()).hexdigest()[:8]
    nuttx_build = os.path.join(build_base, "nuttx")
    apps_build = os.path.join(build_base, "nuttx-apps")

    logger.info("Building NuttX QEMU ARM tests (zero-heap)")
    os.makedirs(build_base, exist_ok=True)

    # Fetch NuttX
    nuttx_hash = os.path.join(dl_dir, f"nuttx-{tag_hash}")
    if not os.path.isdir(nuttx_hash):
        logger.debug(f"Cloning NuttX {default_tag}...")
        run(["git", "clone", "--depth", "1", "-b", default_tag,
              "https://github.com/apache/nuttx.git", nuttx_hash])
    link = os.path.join(dl_dir, "nuttx")
    if os.path.islink(link):
        os.unlink(link)
    os.symlink(nuttx_hash, link)

    # Fetch apps
    apps_hash = os.path.join(dl_dir, f"nuttx-apps-{tag_hash}")
    if not os.path.isdir(apps_hash):
        logger.debug(f"Cloning NuttX apps {default_tag}...")
        run(["git", "clone", "--depth", "1", "-b", default_tag,
              "https://github.com/apache/nuttx-apps.git", apps_hash])
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

    # Register test app (use zeroheap app directory)
    ext_dir = os.path.join(apps_build, "external")
    os.makedirs(ext_dir, exist_ok=True)
    test_dest = os.path.join(ext_dir, "ove_test")
    if os.path.exists(test_dest):
        shutil.rmtree(test_dest)
    shutil.copytree(
        os.path.join(ove_dir, "tests", "sim", "nuttx-qemu-zeroheap",
                     "nuttx_app"),
        test_dest)
    with open(os.path.join(ext_dir, "Kconfig"), "w") as f:
        f.write(f'source "$APPSDIR/external/ove_test/Kconfig"\n')
    with open(os.path.join(ext_dir, "Make.defs"), "w") as f:
        f.write('ifneq ($(CONFIG_EXTERNAL_OVE_TEST),)\n')
        f.write('CONFIGURED_APPS += $(APPDIR)/external/ove_test\n')
        f.write('endif\n')

    # Configure
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
    overlay = os.path.join(ove_dir, "tests", "sim", "nuttx-qemu-zeroheap",
                           "nuttx_test_defconfig")
    nuttx_config = os.path.join(nuttx_build, ".config")
    apply_defconfig_overlay(nuttx_config, overlay)

    apps_abs = os.path.abspath(apps_build)
    nuttx_env["APPDIR"] = apps_abs
    run(["make", "olddefconfig"], cwd=nuttx_build, env=nuttx_env)

    # Build
    nuttx_env["OVE_DIR"] = ove_dir
    run(["make", f"-j{nproc()}"], cwd=nuttx_build, env=nuttx_env)

    logger.info("Running NuttX QEMU ARM tests (zero-heap)")
    qemu_run = os.path.join(ove_dir, "boards", "qemu-mps2-an500",
                            "qemu-run.sh")
    run([qemu_run, os.path.join(nuttx_build, "nuttx"), "--headless",
          "--timeout", "45"])
    return TestResults(suite="qemu-nuttx-zeroheap", passed=1, failed=0)


def test_qemu_zephyr_zeroheap(ove_dir, output_dir):
    """Build and run Zephyr QEMU ARM tests (zero-heap mode)."""
    import hashlib
    dl_dir = os.path.join(ove_dir, "dl")
    build = os.path.join(output_dir, "test", "qemu-zephyr-zeroheap")
    west = os.path.join(ove_dir, ".venv", "bin", "west")

    default_rev = ZEPHYR_DEFAULT_REV
    dl_hash = hashlib.sha256(default_rev.encode()).hexdigest()[:8]

    logger.info("Building Zephyr QEMU ARM tests (zero-heap)")
    os.makedirs(build, exist_ok=True)

    hash_dir = os.path.join(dl_dir, f"zephyr-workspace-{dl_hash}")
    if not os.path.isdir(os.path.join(hash_dir, "zephyr")):
        logger.debug("Zephyr workspace not found -- downloading...")
        run([west, "init", "-m",
              "https://github.com/zephyrproject-rtos/zephyr.git",
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
        os.path.join(ove_dir, "tests", "sim", "zephyr-qemu-zeroheap"),
    ], env=env)

    logger.info("Running Zephyr QEMU ARM tests (zero-heap)")
    qemu_run = os.path.join(ove_dir, "boards", "qemu-mps2-an500",
                            "qemu-run.sh")
    run([qemu_run, os.path.join(build, "zephyr", "zephyr.elf"),
          "--headless", "--timeout", "120"])
    return TestResults(suite="qemu-zephyr-zeroheap", passed=1, failed=0)


# Test name -> function mapping
TEST_TARGETS = {
    "stub": test_stub,
    "cpp": test_cpp,
    "rust": test_rust,
    "zig": test_zig,
    "nuttx": test_nuttx,
    "zephyr": test_zephyr,
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


def cmd_test(args):
    """CLI entry point for 'ove test [name]'."""
    ove_dir = find_ove_dir()
    output_dir = os.path.join(ove_dir, "output")

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

    if any_failed:
        sys.exit(1)
