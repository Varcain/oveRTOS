# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Download RTOS sources and dependencies based on .config.

Absorbs config/scripts/download.py into the ove CLI package.
"""

import logging
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

from .manifest import get_component, load_manifest, warn_if_dirty
from .utils import hashed_dir, rev_hash, update_symlink
from .workspace import Workspace, get_bool, get_str

logger = logging.getLogger("ove")


def _retry(fn, description, attempts=3, backoff=2):
    """Retry fn() with exponential backoff. Returns fn()'s result."""
    for i in range(attempts):
        try:
            return fn()
        except Exception as e:
            if i == attempts - 1:
                raise
            delay = backoff * (2 ** i)
            logger.warning(f"{description}: {e}, retrying in {delay}s ({i+1}/{attempts})")
            time.sleep(delay)


def git_clone(url, tag, dest, name, submodules=None):
    """Clone a git repository to dest with shallow depth."""
    if os.path.isdir(dest):
        logger.info(f"{name}: up to date")
        return True

    logger.info(f"{name}: cloning (tag: {tag})...")
    cmd = ["git", "clone", "--depth", "1", "--branch", tag, url, dest]

    def _do_clone():
        ret = subprocess.run(cmd, capture_output=True, text=True)
        if ret.returncode != 0:
            raise RuntimeError(ret.stderr.strip())
        return ret

    try:
        _retry(_do_clone, f"git clone {name}")
    except Exception as e:
        logger.error(f"git clone failed for {name}: {e}")
        return False

    if submodules:
        logger.info(f"{name}: initializing submodules...")
        cmd = ["git", "-C", dest, "submodule", "update",
               "--init", "--depth", "1"] + submodules
        ret = subprocess.run(cmd, capture_output=True, text=True)
        if ret.returncode != 0:
            logger.error(f"submodule init failed for {name}")
            logger.error(f"{ret.stderr}")
            return False

    logger.info(f"{name}: done")
    return True


def download_tarball(url, dest_dir, name):
    """Download a tarball to dest_dir."""
    os.makedirs(dest_dir, exist_ok=True)
    filename = url.rsplit("/", 1)[-1]
    filepath = os.path.join(dest_dir, filename)

    if os.path.isfile(filepath):
        logger.info(f"{name}: tarball up to date")
        return True

    logger.info(f"{name}: downloading tarball...")
    try:
        _retry(lambda: urllib.request.urlretrieve(url, filepath),
               f"download {name}")
    except Exception as e:
        logger.error(f"download failed for {name}: {e}")
        return False

    logger.info(f"{name}: done")
    return True


def extract_tarball(tarball_path, dest_dir, name):
    """Extract a tarball to dest_dir."""
    if not os.path.isfile(tarball_path):
        logger.error(f"tarball not found: {tarball_path}")
        return False

    logger.debug(f"{name}: extracting to {dest_dir}...")
    os.makedirs(dest_dir, exist_ok=True)
    try:
        with tarfile.open(tarball_path) as tf:
            kwargs = {"filter": "data"} if sys.version_info >= (3, 12) else {}
            tf.extractall(dest_dir, **kwargs)
    except Exception as e:
        logger.error(f"extraction failed: {e}")
        return False
    return True


def _download_progress(block_count, block_size, total_size):
    """Simple progress indicator for urlretrieve.  Callers own the
    "Downloading X" log line that precedes this; we just print the
    incrementing counter to the same stderr/stdout line. """
    downloaded = block_count * block_size
    if total_size > 0:
        pct = min(100, downloaded * 100 // total_size)
        mb_done = downloaded // (1024 * 1024)
        mb_total = total_size // (1024 * 1024)
        print(f"\r  {mb_done}/{mb_total} MB ({pct}%)",
              end="", flush=True)


def _write_toolchain_sentinel(extract_dir, toolchain_dir):
    """Write sentinel file so the build system can find the toolchain path."""
    sentinel = os.path.join(extract_dir, "path.txt")
    with open(sentinel, "w") as f:
        f.write(os.path.abspath(toolchain_dir))


def download_toolchain(config, dl_dir, toolchains_dir, manifest=None):
    """Download and extract prebuilt ARM GNU toolchain."""
    url = get_component(manifest, "toolchains", "arm-gnu", "url")
    if not url:
        logger.error("arm-gnu toolchain URL not set in manifest.yaml")
        return False

    filename = url.rsplit("/", 1)[-1]
    tarball_path = os.path.join(dl_dir, filename)

    dir_name = filename
    for ext in (".tar.xz", ".tar.gz", ".tar.bz2"):
        if dir_name.endswith(ext):
            dir_name = dir_name[:-len(ext)]
            break
    extract_dir = toolchains_dir
    toolchain_dir = os.path.join(extract_dir, dir_name)

    if os.path.isdir(toolchain_dir) and os.path.isfile(
            os.path.join(toolchain_dir, "bin", "arm-none-eabi-gcc")):
        logger.info("Toolchain: up to date")
        _write_toolchain_sentinel(extract_dir, toolchain_dir)
        return True

    if not os.path.isfile(tarball_path):
        logger.info(f"Toolchain: downloading {filename}...")
        logger.info("(this may take several minutes for ~500 MB)")
        try:
            _retry(
                lambda: urllib.request.urlretrieve(url, tarball_path,
                                                   _download_progress),
                "download toolchain")
            print()
        except Exception as e:
            print()
            logger.error(f"download failed: {e}")
            if os.path.isfile(tarball_path):
                os.unlink(tarball_path)
            return False

    logger.info("Toolchain: extracting...")
    os.makedirs(extract_dir, exist_ok=True)
    ret = subprocess.run(
        ["tar", "xf", tarball_path, "-C", extract_dir],
        capture_output=True, text=True)
    if ret.returncode != 0:
        logger.error(f"extraction failed: {ret.stderr}")
        return False

    _write_toolchain_sentinel(extract_dir, toolchain_dir)
    logger.info("Toolchain: ready")
    return True


def symlink_local(src, dest, name):
    """Create a symlink from src to dest."""
    src = os.path.abspath(src)
    if not os.path.exists(src):
        logger.error(f"local path does not exist: {src}")
        return False

    if os.path.exists(dest):
        if os.path.islink(dest):
            os.unlink(dest)
        else:
            logger.debug(f"{name}: destination already exists: {dest}")
            return True

    os.makedirs(os.path.dirname(dest), exist_ok=True)
    os.symlink(src, dest)
    logger.debug(f"{name}: linked {src} -> {dest}")
    return True


def download_freertos(config, dl_dir, build_dir, ws_dl_dir=None,
                      manifest=None):
    """Download FreeRTOS (STM32CubeF7 or standalone kernel) and LVGL."""
    ok = True
    is_qemu = get_bool(config, "CONFIG_OVE_BOARD_QEMU_MPS2_AN500")

    if get_bool(config, "CONFIG_FREERTOS_SOURCE_GIT"):
        if is_qemu:
            freertos_tag = get_component(
                manifest, "rtos", "freertos", "kernel-qemu", "version")
            freertos_url = get_component(
                manifest, "rtos", "freertos", "kernel-qemu", "url")
            dest, link, global_link = hashed_dir(dl_dir, "FreeRTOS-Kernel",
                                    freertos_tag, ws_dl_dir)
            ok = git_clone(freertos_url,
                freertos_tag, dest, "FreeRTOS-Kernel") and ok
            if os.path.isdir(dest):
                update_symlink(link, dest)
        else:
            url = get_component(
                manifest, "rtos", "freertos", "stm32cubef7", "url")
            tag = get_component(
                manifest, "rtos", "freertos", "stm32cubef7", "version")
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
            dest, link, global_link = hashed_dir(dl_dir, "STM32CubeF7", tag, ws_dl_dir)
            ok = git_clone(url, tag, dest, "STM32CubeF7",
                           submodules=stm32cube_submodules) and ok
            if os.path.isdir(dest):
                update_symlink(link, dest)

        lvgl_url = get_component(manifest, "libraries", "lvgl", "url")
        lvgl_tag = get_component(manifest, "libraries", "lvgl", "version")
        dest, link, global_link = hashed_dir(dl_dir, "lvgl", lvgl_tag, ws_dl_dir)
        ok = git_clone(lvgl_url, lvgl_tag, dest, "LVGL") and ok
        if os.path.isdir(dest):
            update_symlink(link, dest)
            if global_link:
                update_symlink(global_link, dest)

    elif get_bool(config, "CONFIG_FREERTOS_SOURCE_TARBALL"):
        url = get_str(config, "CONFIG_FREERTOS_TARBALL_URL")
        if url:
            ok = download_tarball(url, dl_dir, "STM32CubeF7") and ok
        else:
            logger.error("FREERTOS_TARBALL_URL not set")
            ok = False

    elif get_bool(config, "CONFIG_FREERTOS_SOURCE_LOCAL"):
        path = get_str(config, "CONFIG_FREERTOS_LOCAL_PATH")
        if path:
            ok = symlink_local(path,
                               os.path.join(build_dir, "STM32CubeF7"),
                               "STM32CubeF7") and ok
        else:
            logger.error("FREERTOS_LOCAL_PATH not set")
            ok = False

    return ok


def _find_west(ove_dir):
    """Find the west tool."""
    west = shutil.which("west")
    if west:
        return west
    venv_west = os.path.join(ove_dir, ".venv", "bin", "west")
    if os.path.isfile(venv_west):
        return venv_west
    return "west"


def _ensure_lvgl(manifest, dl_dir, ws_dl_dir, zephyr_dir):
    """Clone LVGL and wire up the workspace symlinks.

    Called on both the fast (cache-hit) and slow paths. CI caches
    dl/zephyr-workspace-<hash>/ but not dl/lvgl-<tag>/, so after a
    cache restore the workspace contains a symlink at
    modules/lib/gui/lvgl pointing to a non-existent target. That
    leaves the LVGL module unregistered and LV_* Kconfig symbols
    undefined, so we must re-clone and re-link every time.
    """
    lvgl_url = get_component(manifest, "libraries", "lvgl", "url")
    lvgl_tag = get_component(manifest, "libraries", "lvgl", "version")
    lvgl_dest, _, _ = hashed_dir(dl_dir, "lvgl", lvgl_tag, ws_dl_dir)
    if not git_clone(lvgl_url, lvgl_tag, lvgl_dest, "LVGL"):
        return False

    # Replace bundled LVGL (from west update) with a symlink so all
    # backends compile the same LVGL version. Zephyr's module glue
    # under zephyr/modules/lvgl stays intact.
    zephyr_lvgl = os.path.join(zephyr_dir, "modules", "lib", "gui", "lvgl")
    os.makedirs(os.path.dirname(zephyr_lvgl), exist_ok=True)
    if os.path.isdir(zephyr_lvgl) and not os.path.islink(zephyr_lvgl):
        shutil.rmtree(zephyr_lvgl)
    update_symlink(zephyr_lvgl, lvgl_dest)

    if ws_dl_dir:
        update_symlink(os.path.join(ws_dl_dir, "lvgl"), lvgl_dest)
    update_symlink(os.path.join(dl_dir, "lvgl"), lvgl_dest)
    return True


def download_zephyr(config, dl_dir, build_dir, ws_dl_dir=None,
                    ove_dir=None, manifest=None):
    """Download Zephyr sources via west or local path."""
    if get_bool(config, "CONFIG_ZEPHYR_SOURCE_WEST"):
        url = get_component(manifest, "rtos", "zephyr", "url")
        rev = get_component(manifest, "rtos", "zephyr", "version")

        zephyr_dir, link, global_link = hashed_dir(dl_dir, "zephyr-workspace", rev,
                                      ws_dl_dir)
        west_done_marker = os.path.join(zephyr_dir, ".west_update_done")
        if os.path.isfile(west_done_marker):
            logger.info("Zephyr: workspace up to date")
            update_symlink(link, zephyr_dir)
            return _ensure_lvgl(manifest, dl_dir, ws_dl_dir, zephyr_dir)

        west = _find_west(ove_dir or ".")

        # Run west init if not already initialized
        west_config = os.path.join(zephyr_dir, ".west")
        if not os.path.isdir(west_config):
            logger.info(f"Zephyr: initializing west workspace (rev: {rev})...")
            os.makedirs(zephyr_dir, exist_ok=True)

            init_branch = rev if '/' not in rev and len(rev) < 40 else "main"
            ret = subprocess.run(
                [west, "init", "-m", url, "--mr", init_branch, zephyr_dir],
                capture_output=True, text=True)
            if ret.returncode != 0:
                logger.error(f"west init failed: {ret.stderr}")
                return False

            if init_branch != rev:
                zephyr_repo = os.path.join(zephyr_dir, "zephyr")
                ret = subprocess.run(
                    ["git", "checkout", rev],
                    cwd=zephyr_repo, capture_output=True, text=True)
                if ret.returncode != 0:
                    logger.error(f"git checkout {rev} failed: {ret.stderr}")
                    return False
        else:
            logger.info("Zephyr: running west update...")

        # Always run west update if marker is missing
        ret = subprocess.run(
            [west, "update"],
            cwd=zephyr_dir, capture_output=True, text=True)
        if ret.returncode != 0:
            logger.error(f"west update failed: {ret.stderr}")
            return False

        if not _ensure_lvgl(manifest, dl_dir, ws_dl_dir, zephyr_dir):
            return False

        # Mark as complete
        with open(west_done_marker, "w") as f:
            f.write(rev + "\n")

        update_symlink(link, zephyr_dir)
        logger.info("Zephyr: workspace ready")
        return True

    elif get_bool(config, "CONFIG_ZEPHYR_SOURCE_LOCAL"):
        path = get_str(config, "CONFIG_ZEPHYR_LOCAL_PATH")
        if path:
            return symlink_local(path,
                                 os.path.join(build_dir, "zephyr"), "Zephyr")
        logger.error("ZEPHYR_LOCAL_PATH not set")
        return False

    return False


def download_nuttx(config, dl_dir, build_dir, ws_dl_dir=None, manifest=None):
    """Download NuttX kernel, apps, and CMSIS dependencies."""
    ok = True

    if get_bool(config, "CONFIG_NUTTX_SOURCE_GIT"):
        nuttx_url = get_component(manifest, "rtos", "nuttx", "kernel", "url")
        nuttx_tag = get_component(
            manifest, "rtos", "nuttx", "kernel", "version")
        dest, link, global_link = hashed_dir(dl_dir, "nuttx", nuttx_tag, ws_dl_dir)
        ok = git_clone(nuttx_url, nuttx_tag, dest, "NuttX") and ok
        if os.path.isdir(dest):
            update_symlink(link, dest)
            if global_link:
                update_symlink(global_link, dest)

        apps_url = get_component(manifest, "rtos", "nuttx", "apps", "url")
        apps_tag = get_component(
            manifest, "rtos", "nuttx", "apps", "version")
        dest, link, global_link = hashed_dir(dl_dir, "nuttx-apps", apps_tag, ws_dl_dir)
        ok = git_clone(apps_url, apps_tag, dest, "NuttX apps") and ok
        if os.path.isdir(dest):
            update_symlink(link, dest)
            if global_link:
                update_symlink(global_link, dest)

        cmsis5_url = get_component(manifest, "libraries", "cmsis5", "url")
        cmsis5_tag = get_component(
            manifest, "libraries", "cmsis5", "version")
        dest, link, global_link = hashed_dir(dl_dir, "CMSIS_5", cmsis5_tag, ws_dl_dir)
        ok = git_clone(cmsis5_url, cmsis5_tag, dest, "CMSIS-5") and ok
        if os.path.isdir(dest):
            update_symlink(link, dest)
            if global_link:
                update_symlink(global_link, dest)

        cmsis_dsp_url = get_component(
            manifest, "libraries", "cmsis-dsp", "url")
        cmsis_dsp_tag = get_component(
            manifest, "libraries", "cmsis-dsp", "version")
        dest, link, global_link = hashed_dir(dl_dir, "CMSIS-DSP", cmsis_dsp_tag, ws_dl_dir)
        ok = git_clone(cmsis_dsp_url, cmsis_dsp_tag, dest, "CMSIS-DSP") and ok
        if os.path.isdir(dest):
            update_symlink(link, dest)
            if global_link:
                update_symlink(global_link, dest)

        # External LVGL — NuttX no longer uses bundled nuttx-apps LVGL.
        lvgl_url = get_component(manifest, "libraries", "lvgl", "url")
        lvgl_tag = get_component(manifest, "libraries", "lvgl", "version")
        dest, link, global_link = hashed_dir(dl_dir, "lvgl", lvgl_tag, ws_dl_dir)
        ok = git_clone(lvgl_url, lvgl_tag, dest, "LVGL") and ok
        if os.path.isdir(dest):
            update_symlink(link, dest)
            if global_link:
                update_symlink(global_link, dest)

    elif get_bool(config, "CONFIG_NUTTX_SOURCE_LOCAL"):
        path = get_str(config, "CONFIG_NUTTX_LOCAL_PATH")
        if path:
            ok = symlink_local(path,
                               os.path.join(build_dir, "nuttx"), "NuttX") and ok
        else:
            logger.error("NUTTX_LOCAL_PATH not set")
            ok = False

    return ok


def download_posix(config, dl_dir, build_dir, ws_dl_dir=None, manifest=None):
    """Download LVGL sources for POSIX backend."""
    ok = True
    lvgl_url = get_component(manifest, "libraries", "lvgl", "url")
    lvgl_tag = get_component(manifest, "libraries", "lvgl", "version")
    dest, link, global_link = hashed_dir(dl_dir, "lvgl", lvgl_tag, ws_dl_dir)
    ok = git_clone(lvgl_url, lvgl_tag, dest, "LVGL") and ok
    if os.path.isdir(dest):
        update_symlink(link, dest)
        if global_link:
            update_symlink(global_link, dest)
    return ok


def ensure_rust_target(config, dl_dir):
    """Ensure Rust toolchain and target are available."""
    target = get_str(config, "CONFIG_OVE_RUST_TARGET",
                     "thumbv7em-none-eabihf")

    # Install both soft-float and hard-float variants — at download time
    # CONFIG_ARCH_FPU isn't known yet (NuttX config is generated later),
    # and ove_rust.cmake picks the actual target based on FPU config.
    targets = [target]
    if target == "thumbv7em-none-eabihf":
        targets.append("thumbv7em-none-eabi")
    elif target == "thumbv7em-none-eabi":
        targets.append("thumbv7em-none-eabihf")

    if get_bool(config, "CONFIG_OVE_RUST_TOOLCHAIN_CUSTOM"):
        custom_path = get_str(config,
                              "CONFIG_OVE_RUST_TOOLCHAIN_CUSTOM_PATH")
        cargo = os.path.join(custom_path, "cargo") if custom_path else "cargo"
        rustc = os.path.join(custom_path, "rustc") if custom_path else "rustc"
    else:
        cargo = "cargo"
        rustc = "rustc"

    if not shutil.which(cargo):
        logger.error(f"cargo not found: {cargo}")
        logger.error("Install Rust via https://rustup.rs/ or set a custom path.")
        return False

    if not shutil.which(rustc):
        logger.error(f"rustc not found: {rustc}")
        return False

    logger.info("Rust: cargo and rustc found")

    if get_bool(config, "CONFIG_OVE_RUST_TOOLCHAIN_SYSTEM"):
        rustup = shutil.which("rustup")
        if rustup:
            logger.info(f"Rust: adding targets {targets}...")
            ret = subprocess.run(
                [rustup, "target", "add"] + targets,
                capture_output=True, text=True)
            if ret.returncode != 0:
                logger.error(f"rustup target add failed: {ret.stderr}")
                return False
            logger.info("Rust: targets ready")
        else:
            logger.warning("rustup not found, cannot add target "
                  "automatically")
            logger.debug(f"Ensure targets {targets} are installed.")

    return True


def download_zig_toolchain(config, dl_dir, toolchains_dir, manifest=None):
    """Download and extract Zig toolchain."""
    import platform

    version = get_component(manifest, "toolchains", "zig", "version")
    arch = platform.machine()
    if arch == "x86_64":
        zig_arch = "x86_64"
    elif arch == "aarch64":
        zig_arch = "aarch64"
    else:
        logger.error(f"unsupported architecture for Zig: {arch}")
        return False

    dirname = f"zig-{zig_arch}-linux-{version}"
    filename = f"{dirname}.tar.xz"
    url = f"https://ziglang.org/download/{version}/{filename}"
    tarball_path = os.path.join(dl_dir, filename)
    zig_dir = os.path.join(toolchains_dir, dirname)

    if os.path.isdir(zig_dir) and os.path.isfile(
            os.path.join(zig_dir, "zig")):
        logger.info("Zig: up to date")
        return True

    if not os.path.isfile(tarball_path):
        os.makedirs(dl_dir, exist_ok=True)
        logger.info(f"Zig: downloading {filename}...")
        try:
            _retry(
                lambda: urllib.request.urlretrieve(url, tarball_path,
                                                   _download_progress),
                "download Zig")
            print()
        except Exception as e:
            print()
            logger.error(f"download failed: {e}")
            if os.path.isfile(tarball_path):
                os.unlink(tarball_path)
            return False

    logger.info("Zig: extracting...")
    os.makedirs(toolchains_dir, exist_ok=True)
    ret = subprocess.run(
        ["tar", "xf", tarball_path, "-C", toolchains_dir],
        capture_output=True, text=True)
    if ret.returncode != 0:
        logger.error(f"extraction failed: {ret.stderr}")
        return False

    logger.info("Zig: ready")
    return True


def download_renode(dl_dir, tools_dir, manifest=None):
    """Download and extract a portable Renode build from the manifest.

    Layout:
        <dl_dir>/renode-<version>.linux-portable.tar.gz        — cached tarball
        <tools_dir>/renode/renode_<version>_portable/           — extracted
        <tools_dir>/renode/renode_<version>_portable/renode     — launcher

    Returns the absolute path to the `renode` launcher on success, or
    None if the manifest is missing the tools.renode entry, the URL
    lookup fails, or the download itself fails.  Callers (currently
    `test.py::_ensure_renode` and `ove ensure-toolchain renode`) treat
    None as "skip Renode-dependent work" rather than a hard error —
    Renode is a nice-to-have for STM32 tests, not a required build dep.
    """
    version = get_component(manifest, "tools", "renode", "version")
    url = get_component(manifest, "tools", "renode", "url")
    if not version or not url:
        logger.warning("Renode: not in manifest (tools.renode) — skipping")
        return None

    renode_root = os.path.join(tools_dir, "renode")
    extract_dir = os.path.join(renode_root, f"renode_{version}_portable")
    launcher = os.path.join(extract_dir, "renode")
    if os.path.isfile(launcher):
        logger.info(f"Renode {version}: up to date")
        return launcher

    os.makedirs(dl_dir, exist_ok=True)
    os.makedirs(renode_root, exist_ok=True)

    filename = url.rsplit("/", 1)[-1]
    tarball = os.path.join(dl_dir, filename)

    if not os.path.isfile(tarball):
        logger.info(f"Renode {version}: downloading from {url}")
        try:
            _retry(
                lambda: urllib.request.urlretrieve(url, tarball,
                                                   _download_progress),
                "download renode")
            print()
        except Exception as e:  # noqa: BLE001
            print()
            logger.warning(f"Renode download failed: {e}")
            if os.path.isfile(tarball):
                os.unlink(tarball)
            return None

    logger.info(f"Renode {version}: extracting...")
    ret = subprocess.run(["tar", "xzf", tarball, "-C", renode_root],
                         capture_output=True, text=True)
    if ret.returncode != 0:
        logger.warning(f"Renode extraction failed: {ret.stderr.strip()}")
        return None

    if not os.path.isfile(launcher):
        logger.warning(
            f"Renode: extraction completed but launcher not found at {launcher}")
        return None

    logger.info(f"Renode {version}: ready at {launcher}")
    return launcher


def download_tflm(config, dl_dir, ws_dl_dir=None, manifest=None):
    """Download TensorFlow Lite Micro sources for ML inference."""
    tflm_url = get_component(manifest, "libraries", "tflm", "url")
    tflm_rev = get_component(manifest, "libraries", "tflm", "version")
    if not tflm_url or not tflm_rev:
        logger.error("TFLM URL or version not set in manifest.yaml")
        return False

    dest, link, global_link = hashed_dir(dl_dir, "tflite-micro",
                                         tflm_rev, ws_dl_dir)
    # TFLM uses a commit hash, not a tag — can't shallow clone.
    # Do a full clone then checkout the specific commit.
    ok = True
    if not os.path.isdir(dest):
        logger.info(f"TFLM: cloning ({tflm_rev[:12]})...")
        ret = subprocess.run(
            ["git", "clone", tflm_url, dest],
            capture_output=True, text=True)
        if ret.returncode != 0:
            logger.error(f"TFLM clone failed: {ret.stderr}")
            return False
        ret = subprocess.run(
            ["git", "checkout", tflm_rev],
            cwd=dest, capture_output=True, text=True)
        if ret.returncode != 0:
            logger.error(f"TFLM checkout {tflm_rev[:12]} failed: {ret.stderr}")
            return False
        logger.debug(f"TFLM: checked out {tflm_rev[:12]}")
    else:
        logger.info("TFLM: up to date")

    if os.path.isdir(dest):
        update_symlink(link, dest)
        if global_link:
            update_symlink(global_link, dest)
        # Download TFLM third-party dependencies (flatbuffers, gemmlowp, etc.)
        downloads_dir = os.path.join(
            dest, "tensorflow", "lite", "micro", "tools", "make", "downloads")
        if not os.path.isdir(os.path.join(downloads_dir, "flatbuffers")):
            logger.info("TFLM: downloading third-party dependencies...")
            ret = subprocess.run(
                ["make", "-f",
                 "tensorflow/lite/micro/tools/make/Makefile",
                 "third_party_downloads"],
                cwd=dest, capture_output=True, text=True)
            if ret.returncode != 0:
                logger.error(f"TFLM third-party download failed: {ret.stderr}")
                ok = False
            else:
                logger.info("TFLM: third-party dependencies ready")
    return ok


def download_emscripten(config, dl_dir, ws_dl_dir=None, manifest=None):
    """Download and activate the Emscripten SDK."""
    em_ver = get_component(manifest, "toolchains", "emscripten", "version")
    em_url = get_component(manifest, "toolchains", "emscripten", "url")
    if not em_ver or not em_url:
        logger.error("Emscripten version/url missing from manifest.yaml")
        return False

    dest, link, glink = hashed_dir(dl_dir, "emsdk", em_ver, ws_dl_dir)

    # Check if already installed
    emcc_path = os.path.join(dest, "upstream", "emscripten", "emcc")
    if os.path.isfile(emcc_path):
        logger.debug(f"Emscripten {em_ver}: already installed at {dest}")
        update_symlink(link, dest)
        if glink:
            update_symlink(glink, dest)
        return True

    # Clone emsdk
    if not git_clone(em_url, "main", dest, "emsdk"):
        return False

    # Install and activate the requested version
    logger.info(f"Emscripten: installing version {em_ver}...")
    emsdk = os.path.join(dest, "emsdk")
    ret = subprocess.run([emsdk, "install", em_ver],
                         capture_output=True, text=True, cwd=dest)
    if ret.returncode != 0:
        logger.error(f"emsdk install failed: {ret.stderr}")
        return False

    ret = subprocess.run([emsdk, "activate", em_ver],
                         capture_output=True, text=True, cwd=dest)
    if ret.returncode != 0:
        logger.error(f"emsdk activate failed: {ret.stderr}")
        return False

    update_symlink(link, dest)
    if glink:
        update_symlink(glink, dest)

    logger.info(f"Emscripten {em_ver}: installed")
    return True


def download_all(ws):
    """Download all sources based on current .config."""
    ws.require_config()
    manifest = load_manifest(ws.ove_dir)
    warn_if_dirty(ws.ove_dir)
    config = ws.config
    os.makedirs(ws.dl_dir, exist_ok=True)
    os.makedirs(ws.ws_dl_dir, exist_ok=True)
    os.makedirs(ws.build_dir, exist_ok=True)

    logger.info("Downloading sources...")
    ok = True

    if get_bool(config, "CONFIG_OVE_TOOLCHAIN_DOWNLOAD"):
        ok = download_toolchain(config, ws.dl_dir, ws.toolchains_dir,
                                manifest=manifest) and ok
        # Create workspace toolchain symlink
        if os.path.isfile(os.path.join(ws.toolchains_dir, "path.txt")) \
                and os.path.islink(ws.config_path):
            with open(os.path.join(ws.toolchains_dir, "path.txt")) as f:
                tc_path = f.read().strip()
            tc_name = os.path.basename(tc_path)
            tc_link = os.path.join(ws.workspace_dir, "toolchain")
            rel = os.path.relpath(
                os.path.join(ws.toolchains_dir, tc_name),
                ws.workspace_dir)
            if os.path.islink(tc_link):
                os.unlink(tc_link)
            os.symlink(rel, tc_link)

    if get_bool(config, "CONFIG_OVE_RTOS_FREERTOS"):
        ok = download_freertos(config, ws.dl_dir, ws.build_dir,
                               ws.ws_dl_dir, manifest=manifest) and ok
    elif get_bool(config, "CONFIG_OVE_RTOS_ZEPHYR"):
        ok = download_zephyr(config, ws.dl_dir, ws.build_dir,
                             ws.ws_dl_dir, ws.ove_dir,
                             manifest=manifest) and ok
    elif get_bool(config, "CONFIG_OVE_RTOS_NUTTX"):
        ok = download_nuttx(config, ws.dl_dir, ws.build_dir,
                            ws.ws_dl_dir, manifest=manifest) and ok
    elif get_bool(config, "CONFIG_OVE_RTOS_POSIX"):
        ok = download_posix(config, ws.dl_dir, ws.build_dir,
                            ws.ws_dl_dir, manifest=manifest) and ok

    if get_bool(config, "CONFIG_OVE_INFER"):
        ok = download_tflm(config, ws.dl_dir, ws.ws_dl_dir,
                           manifest=manifest) and ok

    # lwIP for FreeRTOS networking
    if (get_bool(config, "CONFIG_OVE_NET") and
            get_bool(config, "CONFIG_OVE_RTOS_FREERTOS")):
        lwip_url = get_component(manifest, "libraries", "lwip", "url")
        lwip_ver = get_component(manifest, "libraries", "lwip", "version")
        if lwip_url and lwip_ver:
            dest, link, glink = hashed_dir(ws.dl_dir, "lwip",
                                           lwip_ver, ws.ws_dl_dir)
            ok = git_clone(lwip_url, lwip_ver, dest, "lwIP") and ok
            if os.path.isdir(dest):
                update_symlink(link, dest)
                if glink:
                    update_symlink(glink, dest)

    # mbedTLS for TLS support
    if get_bool(config, "CONFIG_OVE_NET_TLS"):
        mbed_url = get_component(manifest, "libraries", "mbedtls", "url")
        mbed_ver = get_component(manifest, "libraries", "mbedtls", "version")
        if mbed_url and mbed_ver:
            dest, link, glink = hashed_dir(ws.dl_dir, "mbedtls",
                                           mbed_ver, ws.ws_dl_dir)
            ok = git_clone(mbed_url, mbed_ver, dest, "mbedTLS") and ok
            if os.path.isdir(dest):
                update_symlink(link, dest)
                if glink:
                    update_symlink(glink, dest)

    if get_bool(config, "CONFIG_OVE_BOARD_WASM"):
        ok = download_emscripten(config, ws.dl_dir, ws.ws_dl_dir,
                                 manifest=manifest) and ok

    if get_bool(config, "CONFIG_OVE_APP_LANG_RUST"):
        ok = ensure_rust_target(config, ws.dl_dir) and ok

    if get_bool(config, "CONFIG_OVE_APP_LANG_ZIG"):
        ok = download_zig_toolchain(config, ws.dl_dir,
                                    ws.toolchains_dir,
                                    manifest=manifest) and ok

    if not ok:
        logger.error("Some downloads failed.")
        sys.exit(1)

    logger.info("All downloads complete.")


def cmd_download(args):
    """CLI entry point for 'ove download'."""
    ws = Workspace()
    if getattr(args, "dry_run", False):
        ws.require_config()
        wanted = []
        if get_bool(ws.config, "CONFIG_OVE_RTOS_FREERTOS"):
            wanted += ["FreeRTOS-Kernel", "STM32CubeF7", "lvgl"]
        elif get_bool(ws.config, "CONFIG_OVE_RTOS_ZEPHYR"):
            wanted += ["zephyr-workspace", "lvgl"]
        elif get_bool(ws.config, "CONFIG_OVE_RTOS_NUTTX"):
            wanted += ["nuttx", "nuttx-apps", "lvgl",
                       "CMSIS_5", "CMSIS-DSP"]
        elif get_bool(ws.config, "CONFIG_OVE_RTOS_POSIX"):
            wanted += ["lvgl"]
        if get_bool(ws.config, "CONFIG_OVE_INFER"):
            wanted.append("tflite-micro")
        if get_bool(ws.config, "CONFIG_OVE_NET"):
            wanted += ["lwip", "mbedtls"]
        print(f"[dry-run] would download into {ws.ws_dl_dir}")
        for name in wanted:
            print(f"[dry-run]   {name}")
        return
    download_all(ws)


def cmd_ensure_toolchain(args):
    """CLI entry point for 'ove ensure-toolchain <name>'.

    Workspace-independent — used by `make docs` to fetch a host Zig
    before .config exists, and by `make test-renode-*` to grab the
    Renode emulator on demand.  Supported names: `zig`, `renode`.
    """
    ws = Workspace()
    manifest = load_manifest(ws.ove_dir)
    warn_if_dirty(ws.ove_dir)
    if args.name == "zig":
        os.makedirs(ws.toolchains_dir, exist_ok=True)
        if not download_zig_toolchain({}, ws.dl_dir, ws.toolchains_dir,
                                      manifest=manifest):
            sys.exit(1)
    elif args.name == "renode":
        tools_dir = os.path.join(ws.ove_dir, "output", "tools")
        os.makedirs(tools_dir, exist_ok=True)
        if download_renode(ws.dl_dir, tools_dir, manifest=manifest) is None:
            sys.exit(1)
    else:
        logger.error(f"unknown toolchain: {args.name} "
                     "(supported: zig, renode)")
        sys.exit(2)
