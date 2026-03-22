#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Download RTOS sources and dependencies based on .config."""

import argparse
import hashlib
import os
import sys
import subprocess
import shutil
import tarfile
import urllib.request

# Add parent for genconfig helpers
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from genconfig import parse_dotconfig, get_config_bool, get_config_str


def rev_hash(revision):
    """Return first 8 hex chars of SHA-256 of revision string."""
    return hashlib.sha256(revision.encode()).hexdigest()[:8]


def hashed_dir(dl_dir, base_name, revision, ws_dl_dir=None):
    """Return (hashed_path, link_path) for a versioned download directory.

    hashed_path: dl/<base_name>-<hash>  (actual content)
    link_path:   <ws_dl_dir>/<base_name> or dl/<base_name> (symlink for consumers)

    When ws_dl_dir is provided, the convenience symlink is placed inside the
    workspace build directory instead of the shared dl/ directory, keeping
    dl/ free of symlinks.
    """
    h = rev_hash(revision)
    link_dir = ws_dl_dir if ws_dl_dir else dl_dir
    return (os.path.join(dl_dir, f"{base_name}-{h}"),
            os.path.join(link_dir, base_name))


def update_symlink(link_path, target_path):
    """Create or update a symlink from link_path -> target_path (relative)."""
    rel = os.path.relpath(target_path, os.path.dirname(link_path))
    if os.path.islink(link_path):
        if os.readlink(link_path) == rel:
            return
        os.unlink(link_path)
    elif os.path.exists(link_path):
        # Plain directory from a pre-hash installation — move it aside
        backup = link_path + ".old"
        print(f"  NOTE: moving legacy {link_path} -> {backup}")
        os.rename(link_path, backup)
    os.symlink(rel, link_path)


def git_clone(url, tag, dest, name, submodules=None):
    """Clone a git repository to dest with shallow depth.

    If submodules is a list of paths, init those submodules after cloning.
    """
    if os.path.isdir(dest):
        print(f"  {name}: already downloaded at {dest}")
        return True

    print(f"  {name}: cloning {url} (tag: {tag})...")
    cmd = ["git", "clone", "--depth", "1", "--branch", tag, url, dest]
    ret = subprocess.run(cmd, capture_output=True, text=True)
    if ret.returncode != 0:
        print(f"  ERROR: git clone failed for {name}")
        print(f"  {ret.stderr}")
        return False

    if submodules:
        print(f"  {name}: initializing submodules...")
        cmd = ["git", "-C", dest, "submodule", "update",
               "--init", "--depth", "1"] + submodules
        ret = subprocess.run(cmd, capture_output=True, text=True)
        if ret.returncode != 0:
            print(f"  ERROR: submodule init failed for {name}")
            print(f"  {ret.stderr}")
            return False

    print(f"  {name}: done")
    return True


def download_tarball(url, dest_dir, name):
    """Download and extract a tarball."""
    os.makedirs(dest_dir, exist_ok=True)
    filename = url.rsplit("/", 1)[-1]
    filepath = os.path.join(dest_dir, filename)

    if os.path.isfile(filepath):
        print(f"  {name}: tarball already downloaded at {filepath}")
        return True

    print(f"  {name}: downloading {url}...")
    try:
        urllib.request.urlretrieve(url, filepath)
    except Exception as e:
        print(f"  ERROR: download failed for {name}: {e}")
        return False

    print(f"  {name}: done")
    return True


def extract_tarball(tarball_path, dest_dir, name):
    """Extract a tarball to dest_dir."""
    if not os.path.isfile(tarball_path):
        print(f"  ERROR: tarball not found: {tarball_path}")
        return False

    print(f"  {name}: extracting to {dest_dir}...")
    os.makedirs(dest_dir, exist_ok=True)
    try:
        with tarfile.open(tarball_path) as tf:
            tf.extractall(dest_dir)
    except Exception as e:
        print(f"  ERROR: extraction failed: {e}")
        return False
    return True


def download_toolchain(config, dl_dir, toolchains_dir):
    """Download and extract prebuilt ARM GNU toolchain.

    Tarball is cached in dl_dir, extracted to toolchains_dir (output/toolchains/).
    """
    url = get_config_str(config, "CONFIG_OVE_TOOLCHAIN_URL")
    if not url:
        print("  ERROR: OVE_TOOLCHAIN_URL not set")
        return False

    filename = url.rsplit("/", 1)[-1]
    tarball_path = os.path.join(dl_dir, filename)

    # Derive extraction directory name (strip archive extension)
    dir_name = filename
    for ext in (".tar.xz", ".tar.gz", ".tar.bz2"):
        if dir_name.endswith(ext):
            dir_name = dir_name[:-len(ext)]
            break
    extract_dir = toolchains_dir
    toolchain_dir = os.path.join(extract_dir, dir_name)

    if os.path.isdir(toolchain_dir) and os.path.isfile(
            os.path.join(toolchain_dir, "bin", "arm-none-eabi-gcc")):
        print(f"  Toolchain: already available at {toolchain_dir}")
        _write_toolchain_sentinel(extract_dir, toolchain_dir)
        return True

    # Download
    if not os.path.isfile(tarball_path):
        print(f"  Toolchain: downloading {filename}...")
        print(f"  (this may take several minutes for ~500 MB)")
        try:
            urllib.request.urlretrieve(url, tarball_path, _download_progress)
            print()  # newline after progress
        except Exception as e:
            print(f"\n  ERROR: download failed: {e}")
            if os.path.isfile(tarball_path):
                os.unlink(tarball_path)  # clean up partial
            return False

    # Extract using tar command (faster than Python tarfile for .xz)
    print(f"  Toolchain: extracting to {extract_dir}...")
    os.makedirs(extract_dir, exist_ok=True)
    ret = subprocess.run(
        ["tar", "xf", tarball_path, "-C", extract_dir],
        capture_output=True, text=True)
    if ret.returncode != 0:
        print(f"  ERROR: extraction failed: {ret.stderr}")
        return False

    _write_toolchain_sentinel(extract_dir, toolchain_dir)
    print(f"  Toolchain: ready at {toolchain_dir}")
    return True


def _download_progress(block_count, block_size, total_size):
    """Simple progress indicator for urlretrieve."""
    downloaded = block_count * block_size
    if total_size > 0:
        pct = min(100, downloaded * 100 // total_size)
        mb_done = downloaded // (1024 * 1024)
        mb_total = total_size // (1024 * 1024)
        print(f"\r  Toolchain: {mb_done}/{mb_total} MB ({pct}%)",
              end="", flush=True)


def _write_toolchain_sentinel(extract_dir, toolchain_dir):
    """Write sentinel file so the Makefile can find the toolchain path."""
    sentinel = os.path.join(extract_dir, "path.txt")
    with open(sentinel, "w") as f:
        f.write(os.path.abspath(toolchain_dir))


def symlink_local(src, dest, name):
    """Create a symlink from src to dest."""
    src = os.path.abspath(src)
    if not os.path.exists(src):
        print(f"  ERROR: local path does not exist: {src}")
        return False

    if os.path.exists(dest):
        if os.path.islink(dest):
            os.unlink(dest)
        else:
            print(f"  {name}: destination already exists: {dest}")
            return True

    os.makedirs(os.path.dirname(dest), exist_ok=True)
    os.symlink(src, dest)
    print(f"  {name}: linked {src} -> {dest}")
    return True


def download_freertos(config, dl_dir, build_dir, ws_dl_dir=None):
    """Download FreeRTOS (STM32CubeF7) and LVGL sources."""
    ok = True

    is_qemu = get_config_bool(config, "CONFIG_OVE_BOARD_QEMU_MPS2_AN500")

    if get_config_bool(config, "CONFIG_FREERTOS_SOURCE_GIT"):
        if is_qemu:
            # QEMU boards use standalone FreeRTOS-Kernel (no STM32CubeF7)
            freertos_tag = "V11.1.0"
            dest, link = hashed_dir(dl_dir, "FreeRTOS-Kernel",
                                    freertos_tag, ws_dl_dir)
            ok = git_clone(
                "https://github.com/FreeRTOS/FreeRTOS-Kernel.git",
                freertos_tag, dest, "FreeRTOS-Kernel") and ok
            if os.path.isdir(dest):
                update_symlink(link, dest)
        else:
            url = get_config_str(config, "CONFIG_FREERTOS_GIT_URL",
                                 "https://github.com/STMicroelectronics/STM32CubeF7.git")
            tag = get_config_str(config, "CONFIG_FREERTOS_GIT_TAG", "v1.17.2")
            stm32cube_submodules = [
                "Drivers/STM32F7xx_HAL_Driver",
                "Drivers/CMSIS/Device/ST/STM32F7xx",
                "Drivers/BSP/STM32746G-Discovery",
                "Drivers/BSP/Components/Common",
                "Drivers/BSP/Components/wm8994",
                "Drivers/BSP/Components/ft5336",
                "Drivers/BSP/Components/stmpe811",
                "Drivers/BSP/Components/rk043fn48h",
            ]
            dest, link = hashed_dir(dl_dir, "STM32CubeF7", tag, ws_dl_dir)
            ok = git_clone(url, tag, dest, "STM32CubeF7",
                           submodules=stm32cube_submodules) and ok
            if os.path.isdir(dest):
                update_symlink(link, dest)

        lvgl_url = get_config_str(config, "CONFIG_FREERTOS_LVGL_GIT_URL",
                                  "https://github.com/lvgl/lvgl.git")
        lvgl_tag = get_config_str(config, "CONFIG_FREERTOS_LVGL_GIT_TAG",
                                  "v8.3.0")
        dest, link = hashed_dir(dl_dir, "lvgl", lvgl_tag, ws_dl_dir)
        ok = git_clone(lvgl_url, lvgl_tag, dest, "LVGL") and ok
        if os.path.isdir(dest):
            update_symlink(link, dest)

    elif get_config_bool(config, "CONFIG_FREERTOS_SOURCE_TARBALL"):
        url = get_config_str(config, "CONFIG_FREERTOS_TARBALL_URL")
        if url:
            ok = download_tarball(url, dl_dir, "STM32CubeF7") and ok
        else:
            print("  ERROR: FREERTOS_TARBALL_URL not set")
            ok = False

    elif get_config_bool(config, "CONFIG_FREERTOS_SOURCE_LOCAL"):
        path = get_config_str(config, "CONFIG_FREERTOS_LOCAL_PATH")
        if path:
            ok = symlink_local(path,
                               os.path.join(build_dir, "STM32CubeF7"),
                               "STM32CubeF7") and ok
        else:
            print("  ERROR: FREERTOS_LOCAL_PATH not set")
            ok = False

    return ok


def _find_west():
    """Find the west tool in PATH or in the ove venv."""
    west = shutil.which("west")
    if west:
        return west
    ove_dir = os.environ.get("OVE_DIR",
        os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__)))))
    venv_west = os.path.join(ove_dir, ".venv", "bin", "west")
    if os.path.isfile(venv_west):
        return venv_west
    return "west"  # fall back, will error if not found


def download_zephyr(config, dl_dir, build_dir, ws_dl_dir=None):
    """Download Zephyr sources via west or local path."""
    if get_config_bool(config, "CONFIG_ZEPHYR_SOURCE_WEST"):
        url = get_config_str(config, "CONFIG_ZEPHYR_WEST_MANIFEST_URL",
                             "https://github.com/zephyrproject-rtos/zephyr.git")
        rev = get_config_str(config, "CONFIG_ZEPHYR_WEST_MANIFEST_REV",
                             "caa8079a5362cd0437ec4d74c888077857df1a9c")

        zephyr_dir, link = hashed_dir(dl_dir, "zephyr-workspace", rev, ws_dl_dir)
        if os.path.isdir(zephyr_dir):
            print("  Zephyr: workspace already exists at", zephyr_dir)
            update_symlink(link, zephyr_dir)
            return True

        print(f"  Zephyr: initializing west workspace (rev: {rev})...")
        os.makedirs(zephyr_dir, exist_ok=True)
        west = _find_west()

        # west init --mr requires a branch/tag; for bare commit hashes,
        # init with 'main' then checkout the specific commit.
        init_branch = rev if '/' not in rev and len(rev) < 40 else "main"
        ret = subprocess.run(
            [west, "init", "-m", url, "--mr", init_branch, zephyr_dir],
            capture_output=True, text=True)
        if ret.returncode != 0:
            print(f"  ERROR: west init failed\n  {ret.stderr}")
            return False

        # If rev is a commit hash, checkout it now before west update
        if init_branch != rev:
            zephyr_repo = os.path.join(zephyr_dir, "zephyr")
            ret = subprocess.run(
                ["git", "checkout", rev],
                cwd=zephyr_repo, capture_output=True, text=True)
            if ret.returncode != 0:
                print(f"  ERROR: git checkout {rev} failed\n  {ret.stderr}")
                return False

        ret = subprocess.run(
            [west, "update"],
            cwd=zephyr_dir, capture_output=True, text=True)
        if ret.returncode != 0:
            print(f"  ERROR: west update failed\n  {ret.stderr}")
            return False

        update_symlink(link, zephyr_dir)
        print("  Zephyr: workspace ready")
        return True

    elif get_config_bool(config, "CONFIG_ZEPHYR_SOURCE_LOCAL"):
        path = get_config_str(config, "CONFIG_ZEPHYR_LOCAL_PATH")
        if path:
            return symlink_local(path,
                                 os.path.join(build_dir, "zephyr"),
                                 "Zephyr")
        print("  ERROR: ZEPHYR_LOCAL_PATH not set")
        return False

    return False


def download_nuttx(config, dl_dir, build_dir, ws_dl_dir=None):
    """Download NuttX kernel, apps, and CMSIS dependencies."""
    ok = True

    if get_config_bool(config, "CONFIG_NUTTX_SOURCE_GIT"):
        nuttx_url = get_config_str(config, "CONFIG_NUTTX_GIT_URL",
                                   "https://github.com/apache/nuttx.git")
        nuttx_tag = get_config_str(config, "CONFIG_NUTTX_GIT_TAG",
                                   "nuttx-12.12.0")
        dest, link = hashed_dir(dl_dir, "nuttx", nuttx_tag, ws_dl_dir)
        ok = git_clone(nuttx_url, nuttx_tag, dest, "NuttX") and ok
        if os.path.isdir(dest):
            update_symlink(link, dest)

        apps_url = get_config_str(config, "CONFIG_NUTTX_APPS_GIT_URL",
                                  "https://github.com/apache/nuttx-apps.git")
        apps_tag = get_config_str(config, "CONFIG_NUTTX_APPS_GIT_TAG",
                                  "nuttx-12.12.0")
        dest, link = hashed_dir(dl_dir, "nuttx-apps", apps_tag, ws_dl_dir)
        ok = git_clone(apps_url, apps_tag, dest, "NuttX apps") and ok
        if os.path.isdir(dest):
            update_symlink(link, dest)

        cmsis5_url = get_config_str(config, "CONFIG_NUTTX_CMSIS5_GIT_URL",
                                    "https://github.com/ARM-software/CMSIS_5.git")
        ok = git_clone(cmsis5_url, "develop",
                       os.path.join(dl_dir, "CMSIS_5"), "CMSIS-5") and ok

        cmsis_dsp_url = get_config_str(config, "CONFIG_NUTTX_CMSIS_DSP_GIT_URL",
                                       "https://github.com/ARM-software/CMSIS-DSP.git")
        ok = git_clone(cmsis_dsp_url, "main",
                       os.path.join(dl_dir, "CMSIS-DSP"), "CMSIS-DSP") and ok

    elif get_config_bool(config, "CONFIG_NUTTX_SOURCE_LOCAL"):
        path = get_config_str(config, "CONFIG_NUTTX_LOCAL_PATH")
        if path:
            ok = symlink_local(path,
                               os.path.join(build_dir, "nuttx"),
                               "NuttX") and ok
        else:
            print("  ERROR: NUTTX_LOCAL_PATH not set")
            ok = False

    return ok


def download_posix(config, dl_dir, build_dir, ws_dl_dir=None):
    """Download LVGL sources for POSIX/SDL2 backend."""
    ok = True
    lvgl_url = get_config_str(config, "CONFIG_POSIX_LVGL_GIT_URL",
                              "https://github.com/lvgl/lvgl.git")
    lvgl_tag = get_config_str(config, "CONFIG_POSIX_LVGL_GIT_TAG", "v9.2.0")
    dest, link = hashed_dir(dl_dir, "lvgl", lvgl_tag, ws_dl_dir)
    ok = git_clone(lvgl_url, lvgl_tag, dest, "LVGL") and ok
    if os.path.isdir(dest):
        update_symlink(link, dest)
    return ok


def ensure_rust_target(config, dl_dir):
    """Ensure Rust toolchain and target are available for cross-compilation."""
    target = get_config_str(config, "CONFIG_OVE_RUST_TARGET",
                            "thumbv7em-none-eabihf")

    if get_config_bool(config, "CONFIG_OVE_RUST_TOOLCHAIN_CUSTOM"):
        custom_path = get_config_str(config,
                                     "CONFIG_OVE_RUST_TOOLCHAIN_CUSTOM_PATH")
        cargo = os.path.join(custom_path, "cargo") if custom_path else "cargo"
        rustc = os.path.join(custom_path, "rustc") if custom_path else "rustc"
    else:
        cargo = "cargo"
        rustc = "rustc"

    # Validate cargo is available
    if not shutil.which(cargo):
        print(f"  ERROR: cargo not found: {cargo}")
        print("  Install Rust via https://rustup.rs/ or set a custom path.")
        return False

    # Validate rustc is available
    if not shutil.which(rustc):
        print(f"  ERROR: rustc not found: {rustc}")
        return False

    print(f"  Rust: cargo and rustc found")

    # Add target if using system rustup
    if get_config_bool(config, "CONFIG_OVE_RUST_TOOLCHAIN_SYSTEM"):
        rustup = shutil.which("rustup")
        if rustup:
            print(f"  Rust: adding target {target}...")
            ret = subprocess.run(
                [rustup, "target", "add", target],
                capture_output=True, text=True)
            if ret.returncode != 0:
                print(f"  ERROR: rustup target add failed: {ret.stderr}")
                return False
            print(f"  Rust: target {target} ready")
        else:
            print("  WARNING: rustup not found, cannot add target automatically")
            print(f"  Ensure target '{target}' is installed.")

    return True


def main():
    parser = argparse.ArgumentParser(description="Download RTOS sources")
    parser.add_argument("--config", default=".config",
                        help="Path to .config file")
    parser.add_argument("--build-dir", default=None,
                        help="Build directory (workspace-aware)")
    parser.add_argument("--workspace-dir", default=None,
                        help="Workspace directory — convenience symlinks "
                             "are placed in <workspace-dir>/dl/ instead of "
                             "the shared dl/ directory")
    parser.add_argument("--toolchains-dir", default=None,
                        help="Toolchain extraction directory")
    args = parser.parse_args()

    ove_dir = os.environ.get("OVE_DIR",
        os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__)))))

    config_path = os.path.join(ove_dir, args.config)
    dl_dir = os.path.join(ove_dir, "dl")
    build_dir = args.build_dir or os.path.join(ove_dir, "output", "build")
    toolchains_dir = args.toolchains_dir or os.path.join(ove_dir, "output", "toolchains")

    # Workspace-local dl/ directory for convenience symlinks.
    # When set, symlinks (e.g. lvgl -> lvgl-0400d37f) are created here
    # instead of in the shared dl/ directory, keeping dl/ pristine.
    ws_dl_dir = None
    if args.workspace_dir:
        ws_dl_dir = os.path.join(args.workspace_dir, "dl")
        os.makedirs(ws_dl_dir, exist_ok=True)

    if not os.path.isfile(config_path):
        print("Error: .config not found. Run 'make menuconfig' or "
              "'make <name>_defconfig' first.")
        sys.exit(1)

    config = parse_dotconfig(config_path)
    os.makedirs(dl_dir, exist_ok=True)
    os.makedirs(build_dir, exist_ok=True)

    print("Downloading sources...")
    ok = True

    # Download toolchain if configured
    if get_config_bool(config, "CONFIG_OVE_TOOLCHAIN_DOWNLOAD"):
        ok = download_toolchain(config, dl_dir, toolchains_dir) and ok

    if get_config_bool(config, "CONFIG_OVE_RTOS_FREERTOS"):
        ok = download_freertos(config, dl_dir, build_dir, ws_dl_dir) and ok
    elif get_config_bool(config, "CONFIG_OVE_RTOS_ZEPHYR"):
        ok = download_zephyr(config, dl_dir, build_dir, ws_dl_dir) and ok
    elif get_config_bool(config, "CONFIG_OVE_RTOS_NUTTX"):
        ok = download_nuttx(config, dl_dir, build_dir, ws_dl_dir) and ok
    elif get_config_bool(config, "CONFIG_OVE_RTOS_POSIX"):
        ok = download_posix(config, dl_dir, build_dir, ws_dl_dir) and ok

    # Ensure Rust target if Rust language is selected
    if get_config_bool(config, "CONFIG_OVE_APP_LANG_RUST"):
        ok = ensure_rust_target(config, dl_dir) and ok

    if not ok:
        print("\nSome downloads failed.")
        sys.exit(1)

    print("\nAll downloads complete.")


if __name__ == "__main__":
    main()
