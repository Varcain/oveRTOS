# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Download RTOS sources and dependencies based on .config.

Absorbs config/scripts/download.py into the ove CLI package.
"""

import hashlib
import logging
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

from .manifest import get_component, load_manifest, warn_if_dirty
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


def rev_hash(revision):
    """Return first 8 hex chars of SHA-256 of revision string."""
    return hashlib.sha256(revision.encode()).hexdigest()[:8]


def hashed_dir(dl_dir, base_name, revision, ws_dl_dir=None):
    """Return (hashed_path, link_path, global_link) for a versioned download.

    hashed_path:  dl/<base_name>-<hash>  (actual content)
    link_path:    <ws_dl_dir>/<base_name> or dl/<base_name> (symlink)
    global_link:  dl/<base_name> when ws_dl_dir is set (else None)
    """
    h = rev_hash(revision)
    link_dir = ws_dl_dir if ws_dl_dir else dl_dir
    global_link = (os.path.join(dl_dir, base_name)
                   if ws_dl_dir else None)
    return (os.path.join(dl_dir, f"{base_name}-{h}"),
            os.path.join(link_dir, base_name),
            global_link)


def update_symlink(link_path, target_path):
    """Create or update a symlink from link_path -> target_path (relative)."""
    rel = os.path.relpath(target_path, os.path.dirname(link_path))
    if os.path.islink(link_path):
        if os.readlink(link_path) == rel:
            return
        os.unlink(link_path)
    elif os.path.exists(link_path):
        backup = link_path + ".old"
        logger.debug(f"NOTE: moving legacy {link_path} -> {backup}")
        os.rename(link_path, backup)
    os.symlink(rel, link_path)


def git_clone(url, tag, dest, name, submodules=None):
    """Clone a git repository to dest with shallow depth."""
    if os.path.isdir(dest):
        logger.debug(f"{name}: already downloaded at {dest}")
        return True

    logger.debug(f"{name}: cloning {url} (tag: {tag})...")
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
        logger.debug(f"{name}: initializing submodules...")
        cmd = ["git", "-C", dest, "submodule", "update",
               "--init", "--depth", "1"] + submodules
        ret = subprocess.run(cmd, capture_output=True, text=True)
        if ret.returncode != 0:
            logger.error(f"submodule init failed for {name}")
            logger.error(f"{ret.stderr}")
            return False

    logger.debug(f"{name}: done")
    return True


def download_tarball(url, dest_dir, name):
    """Download a tarball to dest_dir."""
    os.makedirs(dest_dir, exist_ok=True)
    filename = url.rsplit("/", 1)[-1]
    filepath = os.path.join(dest_dir, filename)

    if os.path.isfile(filepath):
        logger.debug(f"{name}: tarball already downloaded at {filepath}")
        return True

    logger.debug(f"{name}: downloading {url}...")
    try:
        _retry(lambda: urllib.request.urlretrieve(url, filepath),
               f"download {name}")
    except Exception as e:
        logger.error(f"download failed for {name}: {e}")
        return False

    logger.debug(f"{name}: done")
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
    """Simple progress indicator for urlretrieve."""
    downloaded = block_count * block_size
    if total_size > 0:
        pct = min(100, downloaded * 100 // total_size)
        mb_done = downloaded // (1024 * 1024)
        mb_total = total_size // (1024 * 1024)
        print(f"\r  Toolchain: {mb_done}/{mb_total} MB ({pct}%)",
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
        logger.debug(f"Toolchain: already available at {toolchain_dir}")
        _write_toolchain_sentinel(extract_dir, toolchain_dir)
        return True

    if not os.path.isfile(tarball_path):
        logger.debug(f"Toolchain: downloading {filename}...")
        logger.debug("(this may take several minutes for ~500 MB)")
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

    logger.debug(f"Toolchain: extracting to {extract_dir}...")
    os.makedirs(extract_dir, exist_ok=True)
    ret = subprocess.run(
        ["tar", "xf", tarball_path, "-C", extract_dir],
        capture_output=True, text=True)
    if ret.returncode != 0:
        logger.error(f"extraction failed: {ret.stderr}")
        return False

    _write_toolchain_sentinel(extract_dir, toolchain_dir)
    logger.debug(f"Toolchain: ready at {toolchain_dir}")
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
            logger.debug(f"Zephyr: workspace already exists at {zephyr_dir}")
            update_symlink(link, zephyr_dir)
            return True

        west = _find_west(ove_dir or ".")

        # Run west init if not already initialized
        west_config = os.path.join(zephyr_dir, ".west")
        if not os.path.isdir(west_config):
            logger.debug(f"Zephyr: initializing west workspace (rev: {rev})...")
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
            logger.debug(f"Zephyr: workspace exists, running west update...")

        # Always run west update if marker is missing
        ret = subprocess.run(
            [west, "update"],
            cwd=zephyr_dir, capture_output=True, text=True)
        if ret.returncode != 0:
            logger.error(f"west update failed: {ret.stderr}")
            return False

        # Mark as complete
        with open(west_done_marker, "w") as f:
            f.write(rev + "\n")

        update_symlink(link, zephyr_dir)
        logger.debug("Zephyr: workspace ready")
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
    # and ove_rust.mk picks the actual target based on FPU config.
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

    logger.debug("Rust: cargo and rustc found")

    if get_bool(config, "CONFIG_OVE_RUST_TOOLCHAIN_SYSTEM"):
        rustup = shutil.which("rustup")
        if rustup:
            logger.debug(f"Rust: adding targets {targets}...")
            ret = subprocess.run(
                [rustup, "target", "add"] + targets,
                capture_output=True, text=True)
            if ret.returncode != 0:
                logger.error(f"rustup target add failed: {ret.stderr}")
                return False
            logger.debug(f"Rust: targets {targets} ready")
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
        logger.debug(f"Zig: already available at {zig_dir}")
        return True

    if not os.path.isfile(tarball_path):
        os.makedirs(dl_dir, exist_ok=True)
        logger.debug(f"Zig: downloading {filename}...")
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

    logger.debug(f"Zig: extracting to {toolchains_dir}...")
    os.makedirs(toolchains_dir, exist_ok=True)
    ret = subprocess.run(
        ["tar", "xf", tarball_path, "-C", toolchains_dir],
        capture_output=True, text=True)
    if ret.returncode != 0:
        logger.error(f"extraction failed: {ret.stderr}")
        return False

    logger.debug(f"Zig: ready at {zig_dir}")
    return True


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
        logger.debug(f"TFLM: cloning {tflm_url} ({tflm_rev[:12]})...")
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
        logger.debug(f"TFLM: already downloaded at {dest}")

    if os.path.isdir(dest):
        update_symlink(link, dest)
        if global_link:
            update_symlink(global_link, dest)
        # Download TFLM third-party dependencies (flatbuffers, gemmlowp, etc.)
        downloads_dir = os.path.join(
            dest, "tensorflow", "lite", "micro", "tools", "make", "downloads")
        if not os.path.isdir(os.path.join(downloads_dir, "flatbuffers")):
            logger.debug("TFLM: downloading third-party dependencies...")
            ret = subprocess.run(
                ["make", "-f",
                 "tensorflow/lite/micro/tools/make/Makefile",
                 "third_party_downloads"],
                cwd=dest, capture_output=True, text=True)
            if ret.returncode != 0:
                logger.error(f"TFLM third-party download failed: {ret.stderr}")
                ok = False
            else:
                logger.debug("TFLM: third-party dependencies ready")
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
    download_all(ws)
