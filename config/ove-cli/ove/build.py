# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Build orchestration — dispatches to cmake per RTOS."""

import hashlib
import json
import logging
import os
import re
import shlex
import shutil
import subprocess
import sys

from .utils import run, nproc, apply_defconfig_overlay, atomic_symlink
from .constants import NUTTX_BOARD_CONFIGS, ZEPHYR_BOARD_MAPPINGS
from .workspace import Workspace, get_bool

logger = logging.getLogger("ove")


def _preflight_check(ws):
    """Validate workspace prerequisites before building."""
    rtos = ws.rtos
    # Verify RTOS sources exist in dl/
    rtos_dir_map = {
        "freertos": ["FreeRTOS-Kernel", "STM32CubeF7"],
        "nuttx": ["nuttx"],
        "zephyr": ["zephyr-workspace"],
        "posix": [],
    }
    candidates = rtos_dir_map.get(rtos, [])
    if candidates:
        found = any(
            os.path.isdir(os.path.join(ws.ws_dl_dir, d))
            for d in candidates
        )
        if not found:
            logger.error(
                f"RTOS sources not found in {ws.ws_dl_dir} "
                f"(checked: {', '.join(candidates)}). "
                f"Run 'ove download' first.")
            sys.exit(1)

    # Verify toolchain is available when cross-compiling
    if ws.toolchain_dir:
        gcc = os.path.join(ws.toolchain_dir, "bin", "arm-none-eabi-gcc")
        if not os.path.isfile(gcc):
            logger.error(
                f"ARM toolchain not found at {gcc}. "
                f"Run 'ove download' to fetch the toolchain.")
            sys.exit(1)

    # Warn rather than fail: the guest ABI may be mid-change deliberately, and
    # 'ove flash' refuses the resulting image anyway.
    from .image_id import rootfs_abi_conflict
    conflict = rootfs_abi_conflict(ws)
    if conflict:
        logger.warning(conflict)

    # Validate board directory exists
    if ws.board_dir is None:
        logger.error(
            "No board configured. Set CONFIG_OVE_BOARD_NAME in .config.")
        sys.exit(1)
    if not os.path.isdir(ws.board_dir):
        logger.error(
            f"Board directory does not exist: {ws.board_dir}")
        sys.exit(1)

    # Validate board+RTOS combo
    board_rtos_dir = os.path.join(ws.board_dir, rtos)
    if not os.path.isdir(board_rtos_dir):
        logger.error(
            f"Board '{ws.board_name}' does not support RTOS '{rtos}' "
            f"(missing {board_rtos_dir})")
        sys.exit(1)


_PATCH_STATE_VERSION = 1
_PATCH_STATE_FILE = ".ove_patch_state.json"


def _collect_patch_series(layers):
    """Return the complete ordered patch series for ``(label, dir)`` layers."""
    series = []
    for label, patches_dir in layers:
        if not patches_dir or not os.path.isdir(patches_dir):
            continue
        for name in sorted(os.listdir(patches_dir)):
            if name.endswith(".patch"):
                series.append((label, os.path.join(patches_dir, name)))
    return series


def _git_capture(repo, args, *, binary=False):
    """Run a read-only git query and return its output."""
    result = subprocess.run(
        ["git", *args], cwd=repo, capture_output=True,
        text=not binary)
    if result.returncode != 0:
        stderr = result.stderr
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
        raise RuntimeError(
            f"git {' '.join(args)} failed in {repo}: {stderr.strip()}")
    return result.stdout


def _patch_manifest(source_dir, series):
    """Describe the immutable source revision and complete ordered patch set."""
    base = _git_capture(source_dir, ["rev-parse", "HEAD"]).strip()
    patches = []
    for label, path in series:
        with open(path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        patches.append({
            "label": label,
            "name": os.path.basename(path),
            "sha256": digest,
        })
    return {
        "version": _PATCH_STATE_VERSION,
        "base": base,
        "patches": patches,
    }


def _patched_paths(worktree):
    """Return every tracked path changed by the applied patch series."""
    output = _git_capture(
        worktree, ["diff", "--name-only", "-z", "HEAD"], binary=True)
    return sorted(p.decode("utf-8", errors="surrogateescape")
                  for p in output.split(b"\0") if p)


def _patch_fingerprint(worktree, paths):
    """Hash the effective diff for patch-owned paths only.

    NuttX legitimately edits its board defconfig after source preparation. By
    restricting validation to paths actually owned by patches, those generated
    edits do not invalidate the source while interrupted or manually damaged
    patch application still does.
    """
    if not paths:
        return hashlib.sha256(b"").hexdigest()
    diff = _git_capture(
        worktree, ["diff", "--binary", "HEAD", "--", *paths], binary=True)
    return hashlib.sha256(diff).hexdigest()


def _load_patch_state(worktree):
    try:
        with open(os.path.join(worktree, _PATCH_STATE_FILE)) as f:
            return json.load(f)
    except (OSError, ValueError, TypeError):
        return None


def _patch_tree_matches(worktree, manifest):
    """Return true only for a complete, undamaged prepared worktree."""
    state = _load_patch_state(worktree)
    if not state:
        return False
    expected = {key: state.get(key) for key in manifest}
    if expected != manifest:
        return False
    paths = state.get("paths")
    fingerprint = state.get("fingerprint")
    if not isinstance(paths, list) or not isinstance(fingerprint, str):
        return False
    try:
        if _git_capture(worktree, ["rev-parse", "HEAD"]).strip() \
                != manifest["base"]:
            return False
        return _patch_fingerprint(worktree, paths) == fingerprint
    except (OSError, RuntimeError):
        return False


def _discard_patch_worktree(source_dir, worktree):
    """Remove a generated worktree and its now-stale Git registration."""
    if os.path.islink(worktree) or os.path.isfile(worktree):
        os.unlink(worktree)
    elif os.path.isdir(worktree):
        shutil.rmtree(worktree)
    # A deleted worktree becomes prunable immediately. This never touches the
    # source checkout's files or revision.
    run(["git", "worktree", "prune"], cwd=source_dir)


def _prepare_patched_git_tree(source_dir, worktree, layers, *,
                              log_file=None):
    """Materialize and verify a build-owned RTOS source worktree.

    The download cache is an immutable Git source. The destination is recreated
    from its exact HEAD whenever a patch is added, removed, renamed, reordered,
    or edited in place. State is committed only after every patch applies, so an
    interrupted preparation is also rebuilt on the next invocation.
    """
    source_dir = os.path.realpath(source_dir)
    worktree = os.path.abspath(worktree)
    series = _collect_patch_series(layers)
    manifest = _patch_manifest(source_dir, series)

    if os.path.isdir(worktree) and _patch_tree_matches(worktree, manifest):
        logger.debug("Patched RTOS source is current: %s", worktree)
        return worktree

    logger.info("Refreshing patched RTOS source: %s", worktree)
    os.makedirs(os.path.dirname(worktree), exist_ok=True)
    _discard_patch_worktree(source_dir, worktree)

    state_tmp = os.path.join(
        worktree, f"{_PATCH_STATE_FILE}.tmp.{os.getpid()}")
    try:
        run(["git", "worktree", "add", "--detach", "--force",
             worktree, manifest["base"]], cwd=source_dir,
            log_file=log_file)
        for label, patch_path in series:
            logger.debug("Applying %s: %s", label,
                         os.path.basename(patch_path))
            # Updating the worktree index makes new files part of the verified
            # diff as well; a plain git apply would leave them untracked.
            run(["git", "apply", "--index", patch_path], cwd=worktree,
                log_file=log_file)

        paths = _patched_paths(worktree)
        state = dict(manifest)
        state["paths"] = paths
        state["fingerprint"] = _patch_fingerprint(worktree, paths)
        with open(state_tmp, "w") as f:
            json.dump(state, f, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(state_tmp, os.path.join(worktree, _PATCH_STATE_FILE))
    except BaseException:
        if os.path.exists(state_tmp):
            os.unlink(state_tmp)
        _discard_patch_worktree(source_dir, worktree)
        raise

    return worktree


def _zephyr_patch_worktree(ws, zephyr_download):
    """Return this build workspace's stable location inside the west tree."""
    workspace_key = hashlib.sha256(
        os.path.realpath(ws.workspace_dir).encode()).hexdigest()[:16]
    return os.path.join(
        os.path.dirname(zephyr_download), ".ove-worktrees", workspace_key)


def discard_workspace_patch_worktrees(ws):
    """Remove generated patch worktrees owned by the active workspace."""
    try:
        rtos = ws.rtos
    except FileNotFoundError:
        # Preserve `ove clean` as a recovery command when configuration was
        # never generated or has already been removed.
        return
    if rtos != "zephyr":
        return
    source = os.path.join(ws.ws_dl_dir, "zephyr-workspace", "zephyr")
    if not os.path.isdir(source):
        return
    _discard_patch_worktree(
        os.path.realpath(source), _zephyr_patch_worktree(ws, source))


def discard_all_patch_worktrees(dl_dir):
    """Remove generated Zephyr worktrees retained outside output/."""
    import glob

    seen = set()
    for candidate in glob.glob(os.path.join(dl_dir, "zephyr-workspace-*")):
        topdir = os.path.realpath(candidate)
        if topdir in seen:
            continue
        seen.add(topdir)
        source = os.path.join(topdir, "zephyr")
        generated = os.path.join(topdir, ".ove-worktrees")
        if not os.path.isdir(source) or not os.path.isdir(generated):
            continue
        shutil.rmtree(generated)
        run(["git", "worktree", "prune"], cwd=source)


def _fallback_rtos_mapping(ws, rtos):
    """Fall back to board.yaml _meta for RTOS board mapping."""
    import yaml
    yaml_path = os.path.join(ws.board_dir, "board.yaml")
    if not os.path.isfile(yaml_path):
        return None
    try:
        with open(yaml_path) as f:
            data = yaml.safe_load(f) or {}
    except Exception:
        return None
    meta = data.get("_meta", {})
    if rtos == "nuttx":
        board = meta.get("nuttx_board")
        config = meta.get("nuttx_config", "nsh")
        return f"{board}:{config}" if board else None
    elif rtos == "zephyr":
        return meta.get("zephyr_board")
    return None


def _find_cmake():
    """Find cmake executable."""
    cmake = shutil.which("cmake")
    if not cmake:
        logger.error("cmake not found. Install with: sudo apt install cmake")
        sys.exit(1)
    return cmake


def _prepare_freertos_build_dir(fw_build, float_abi):
    """Discard CMake state compiled for a different ARM float ABI.

    CMAKE_<LANG>_FLAGS_INIT is consumed only when a build tree is first
    configured.  Updating OVE_ARM_FLOAT_ABI in an existing cache therefore
    does not update the cached compiler flags and can silently mix hard-float
    and softfp objects.  The object files are ABI-specific, so a clean CMake
    tree is required when the selected calling convention changes.
    """
    cache = os.path.join(fw_build, "CMakeCache.txt")
    cached_abis = set()
    if os.path.isfile(cache):
        with open(cache) as f:
            for line in f:
                if line.startswith(("CMAKE_C_FLAGS:",
                                    "CMAKE_CXX_FLAGS:",
                                    "CMAKE_ASM_FLAGS:")):
                    cached_abis.update(re.findall(
                        r"(?:^|\s)-mfloat-abi=(hard|softfp)(?=\s|$)", line))

    if cached_abis and cached_abis != {float_abi}:
        logger.info(
            "ARM float ABI changed (%s -> %s); cleaning the FreeRTOS "
            "CMake build tree",
            ", ".join(sorted(cached_abis)), float_abi)
        shutil.rmtree(fw_build)

    os.makedirs(fw_build, exist_ok=True)


def _prepare_zephyr_build_dir(fw_build, zephyr_base):
    """Discard CMake state bound to a different Zephyr source worktree."""
    cache = os.path.join(fw_build, "CMakeCache.txt")
    cached_base = None
    if os.path.isfile(cache):
        with open(cache) as f:
            for line in f:
                if line.startswith("ZEPHYR_BASE:"):
                    cached_base = line.rstrip("\n").split("=", 1)[-1]
                    break

    if cached_base and os.path.realpath(cached_base) \
            != os.path.realpath(zephyr_base):
        logger.info(
            "Zephyr source worktree changed; cleaning the CMake build tree")
        shutil.rmtree(fw_build)

    os.makedirs(fw_build, exist_ok=True)


def build_freertos(ws):
    """Build FreeRTOS firmware via CMake."""
    cmake = _find_cmake()
    env = ws.toolchain_env()
    board_dir = os.path.join(ws.board_dir, "freertos")
    fw_build = os.path.join(ws.build_dir, "firmware")
    _prepare_freertos_build_dir(fw_build, ws.arm_float_abi)

    # Keep the shared download immutable. Board, reusable LXP-port, and app
    # patches are materialized into this build workspace's verified worktree.
    # Kernel coupling required by a port belongs with that port rather than
    # whichever application happens to use it.
    freertos_download = os.path.join(ws.ws_dl_dir, "FreeRTOS-Kernel")
    freertos_src = None
    if os.path.isdir(freertos_download):
        patch_layers = [("board patch", os.path.join(board_dir, "patches"))]
        if ws.config.get("CONFIG_OVE_LINUX"):
            patch_layers.append((
                "LXP FreeRTOS port patch",
                os.path.join(ws.ove_dir, "modules", "lxp", "ports",
                             "freertos", "patches")))
        if ws.app_dir:
            patch_layers.append((
                "app patch", os.path.join(ws.app_dir, "patches", "freertos")))
        freertos_src = _prepare_patched_git_tree(
            freertos_download,
            os.path.join(ws.build_dir, "rtos-source", "FreeRTOS-Kernel"),
            patch_layers, log_file=ws.build_log)

    toolchain_file = os.path.join(board_dir, "cmake", "arm-none-eabi.cmake")

    cmake_args = [
        cmake,
        "-DOVE_DIR=" + ws.ove_dir,
        "-DOVE_APP_DIR=" + ws.app_dir,
        "-DOVE_GEN_DIR=" + ws.gen_dir,
        "-DOVE_DL_DIR=" + ws.ws_dl_dir,
        "-DBOARD_DIR=" + board_dir,
    ]

    if freertos_src:
        cmake_args.append("-DFREERTOS_PATH=" + freertos_src)
    else:
        # Non-Linux STM32 profiles intentionally use the FreeRTOS copy bundled
        # by STM32CubeF7.  Clear a value cached by an earlier standalone-kernel
        # profile so OveFreeRTOS.cmake can select that board-native fallback.
        cmake_args.append("-UFREERTOS_PATH")

    tc = ws.toolchain_dir
    if tc:
        cmake_args.append(f"-DOVE_TOOLCHAIN_DIR={tc}")

    # The Cortex-M toolchain runs before project(), so this cannot be learned
    # later from generated ove_config.cmake. Keep compiler probes, Picolibc,
    # and the final image on the Kconfig-selected calling convention.
    cmake_args.append(f"-DOVE_ARM_FLOAT_ABI={ws.arm_float_abi}")

    if os.path.isfile(toolchain_file):
        cmake_args.append(f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}")

    cmake_args.append(board_dir)

    logger.info("Building FreeRTOS firmware")
    run(cmake_args, env=env, cwd=fw_build, log_file=ws.build_log)
    run([cmake, "--build", fw_build, f"-j{nproc()}"], env=env,
        log_file=ws.build_log)

    _copy_images(ws, fw_build)
    _create_run_or_flash_script(ws)


def build_zephyr(ws):
    """Build Zephyr firmware via west."""
    env = ws.toolchain_env()
    from .download import download_zephyr_sdk, zephyr_sdk_env
    from .manifest import load_manifest

    manifest = load_manifest(ws.ove_dir)
    sdk_dir = download_zephyr_sdk(ws.dl_dir, ws.toolchains_dir,
                                  manifest=manifest)
    if sdk_dir is None:
        logger.error("Zephyr SDK unavailable; run "
                     "'ove ensure-toolchain zephyr-sdk'")
        sys.exit(1)
    env = zephyr_sdk_env(env, sdk_dir)

    west = os.path.join(ws.venv_dir, "bin", "west")
    board_dir = os.path.join(ws.board_dir, "zephyr")
    fw_build = os.path.join(ws.build_dir, "firmware")

    zephyr_download = os.path.join(
        ws.ws_dl_dir, "zephyr-workspace", "zephyr")
    zephyr_ws = zephyr_download
    if os.path.isdir(zephyr_download):
        patch_layers = [
            ("global patch",
             os.path.join(ws.ove_dir, "config", "patches", "zephyr")),
            ("board patch",
             os.path.join(ws.board_dir, "zephyr", "patches")),
        ]
        if ws.app_dir:
            patch_layers.append((
                "app patch", os.path.join(ws.app_dir, "patches", "zephyr")))

        # Keep ZEPHYR_BASE below the original west workspace so west still
        # discovers its manifest and modules while using our isolated checkout.
        zephyr_ws = _prepare_patched_git_tree(
            zephyr_download,
            _zephyr_patch_worktree(ws, zephyr_download),
            patch_layers, log_file=ws.build_log)
        env["ZEPHYR_BASE"] = zephyr_ws

    _prepare_zephyr_build_dir(fw_build, zephyr_ws)

    # Apply layered config (build guard — fresh by construction)
    from .rtos_menuconfig import ensure_rtos_config_applied
    conf_file = ensure_rtos_config_applied(ws, "zephyr")

    # Map oveRTOS board name to Zephyr board identifier
    zephyr_board = ZEPHYR_BOARD_MAPPINGS.get(ws.board_name)
    if not zephyr_board:
        zephyr_board = _fallback_rtos_mapping(ws, "zephyr")
    if not zephyr_board:
        logger.error(f"no Zephyr board mapping for '{ws.board_name}'")
        sys.exit(1)

    logger.info("Building Zephyr firmware")
    west_args = [
        west, "build",
        "-b", zephyr_board,
        "-d", fw_build,
        board_dir,
        "--",
        f"-DOVE_DIR={ws.ove_dir}",
        f"-DOVE_APP_DIR={ws.app_dir}",
        f"-DOVE_GEN_DIR={ws.gen_dir}",
    ]
    if conf_file:
        west_args.append(f"-DEXTRA_CONF_FILE={conf_file}")
    tc = ws.toolchain_dir
    if tc:
        west_args.append(f"-DOVE_TOOLCHAIN_DIR={tc}")
    run(west_args, env=env, log_file=ws.build_log)

    # Copy images
    os.makedirs(ws.images_dir, exist_ok=True)
    for src, dst in [
        ("zephyr/zephyr.elf", "firmware.elf"),
        ("zephyr/zephyr.bin", "firmware.bin"),
    ]:
        src_path = os.path.join(fw_build, src)
        if os.path.isfile(src_path):
            shutil.copy2(src_path, os.path.join(ws.images_dir, dst))

    _check_rtos_config_drift(ws, os.path.join(fw_build, "zephyr", ".config"))
    _create_run_or_flash_script(ws, rtos="zephyr")


def _stage_nuttx_tree(src, dest, label):
    """Copy src → dest once, gated by a stamp file.

    copytree is not atomic: an interrupted copy leaves a partial tree
    whose presence fools a plain isdir() check on the next run. The
    stamp file is only written after copytree returns, so any earlier
    abort forces a full re-copy.
    """
    stamp = os.path.join(dest, ".ove_stage_complete")
    if os.path.exists(stamp):
        return
    if os.path.isdir(dest):
        logger.debug("Removing partial %s tree at %s", label, dest)
        shutil.rmtree(dest)
    logger.debug("Copying %s to build tree...", label)
    shutil.copytree(src, dest, symlinks=True, dirs_exist_ok=True)
    with open(stamp, "w") as f:
        f.write("staged\n")


def _setup_nuttx_build_tree(ws, env, log_file=None):
    """Prepare patched NuttX sources and set up the external CMake app.

    Returns (nuttx_src, apps_build) paths.  The nuttx_src directory is
    used as the CMake source directory (-S); a separate binary directory
    is created by the caller.
    """
    nuttx_src = os.path.join(ws.build_dir, "nuttx")
    apps_build = os.path.join(ws.build_dir, "nuttx-apps")

    # Materialize the kernel from immutable downloaded Git state. The build may
    # subsequently edit its private defconfig; patch validation is deliberately
    # limited to patch-owned paths so those generated edits remain valid.
    nuttx_download = os.path.join(ws.ws_dl_dir, "nuttx")
    patch_layers = [
        ("board patch", os.path.join(ws.board_dir, "nuttx", "patches")),
    ]
    if ws.app_dir:
        patch_layers.append((
            "app patch", os.path.join(ws.app_dir, "patches", "nuttx")))
    _prepare_patched_git_tree(
        nuttx_download, nuttx_src, patch_layers, log_file=log_file)

    # NuttX apps are not patched, but still need a private writable staging tree.
    # A stamp gates re-copy: a bare isdir() check can accept a partial copy left
    # by interruption and later fail with a misleading missing-file error.
    _stage_nuttx_tree(os.path.join(ws.ws_dl_dir, "nuttx-apps"), apps_build,
                      label="NuttX apps")

    # Set up external app (CMake)
    ext_dir = os.path.join(apps_build, "external")
    os.makedirs(ext_dir, exist_ok=True)
    app_dest = os.path.join(ext_dir, "ove_app")
    if os.path.exists(app_dest):
        shutil.rmtree(app_dest)
    shutil.copytree(os.path.join(ws.board_dir, "nuttx"), app_dest)

    # Kconfig for the external app (same as before — Kconfig is
    # build-system agnostic and works with both Make and CMake)
    with open(os.path.join(ext_dir, "Kconfig"), "w") as f:
        f.write(f'source "{os.path.abspath(app_dest)}/Kconfig"\n')

    # CMakeLists.txt for the external directory — NuttX's CMake build
    # discovers external apps via add_subdirectory(external)
    with open(os.path.join(ext_dir, "CMakeLists.txt"), "w") as f:
        f.write("add_subdirectory(ove_app)\n")

    # Write .ove_env in key=value format (read by OveNuttX.cmake)
    with open(os.path.join(app_dest, ".ove_env"), "w") as f:
        f.write(f"OVE_DIR={ws.ove_dir}\n")
        f.write(f"OVE_GEN_DIR={ws.gen_dir}\n")
        f.write(f"OVE_APP_DIR={ws.app_dir}\n")
        f.write(f"OVE_DL_DIR={ws.ws_dl_dir}\n")

    return nuttx_src, apps_build


def _find_nuttx_defconfig(nuttx_src, board_cfg):
    """Find the defconfig file for a NuttX board:config pair.

    board_cfg is e.g. "mps2-an500:nsh".  Returns the absolute path to the
    defconfig file, or None if not found.
    """
    import glob as _glob
    sep = ":" if ":" in board_cfg else "/"
    board, config = board_cfg.split(sep, 1)
    matches = _glob.glob(
        os.path.join(nuttx_src, "boards", "*", "*", board,
                     "configs", config, "defconfig"))
    return matches[0] if matches else None


def _apply_nuttx_defconfig_overlays(ws, nuttx_src, board_cfg):
    """Merge oveRTOS config overlays into the NuttX board defconfig.

    This modifies the defconfig in-place within the copied source tree so
    that NuttX's CMake build picks up the merged config during its
    olddefconfig expansion.
    """
    from .rtos_menuconfig import merge_rtos_config_layers

    defconfig = _find_nuttx_defconfig(nuttx_src, board_cfg)
    if not defconfig:
        logger.warning(f"NuttX defconfig not found for {board_cfg}")
        return

    # Layers 1-3 (oveRTOS template + board overlay + app overlay)
    merged = merge_rtos_config_layers(ws, "nuttx")
    if merged:
        apply_defconfig_overlay(defconfig, merged)

    # Layer 4: user customizations (rtos.config)
    if os.path.isfile(ws.rtos_config_path):
        apply_defconfig_overlay(defconfig, ws.rtos_config_path)


def build_nuttx(ws):
    """Build NuttX firmware via CMake."""
    cmake = _find_cmake()
    env = ws.toolchain_env()

    logger.info("Building NuttX firmware")

    nuttx_src, apps_build = _setup_nuttx_build_tree(ws, env,
                                                     log_file=ws.build_log)
    cmake_build = os.path.join(ws.build_dir, "nuttx-cmake")
    os.makedirs(cmake_build, exist_ok=True)

    # NuttX board config mapping (e.g. "mps2-an500:nsh")
    nuttx_board_cfg = NUTTX_BOARD_CONFIGS.get(ws.board_name)
    if not nuttx_board_cfg:
        nuttx_board_cfg = _fallback_rtos_mapping(ws, "nuttx") or "mps2-an500:nsh"

    # Apply oveRTOS defconfig overlays to the board's defconfig in the
    # copied source tree.  NuttX CMake reads this defconfig directly and
    # runs olddefconfig to expand it into .config.
    _apply_nuttx_defconfig_overlays(ws, nuttx_src, nuttx_board_cfg)

    # Check if defconfig changed since last cmake configure — if so,
    # force re-initialisation by removing the cached .config.
    defconfig_path = _find_nuttx_defconfig(nuttx_src, nuttx_board_cfg)
    if defconfig_path:
        from .utils import hash_file as _hash_file
        current_hash = _hash_file(defconfig_path)
        hash_file = os.path.join(cmake_build, ".ove_defconfig_hash")
        force_reconfig = True
        if os.path.isfile(hash_file):
            with open(hash_file) as f:
                if f.read().strip() == current_hash:
                    force_reconfig = False
        if force_reconfig:
            config_path = os.path.join(cmake_build, ".config")
            if os.path.isfile(config_path):
                os.unlink(config_path)
            with open(hash_file, "w") as f:
                f.write(current_hash + "\n")

    # Export build paths as environment variables for CMake and cargo/zig
    env["OVE_DIR"] = ws.ove_dir
    env["OVE_GEN_DIR"] = ws.gen_dir
    env["OVE_APP_DIR"] = ws.app_dir
    env["OVE_DL_DIR"] = ws.ws_dl_dir

    # LVGL paths for Rust bindgen
    lvgl_inc = os.path.join(ws.ws_dl_dir, "lvgl")
    env["LVGL_INCLUDE_PATH"] = lvgl_inc
    env["LVGL_PARENT_PATH"] = ws.ws_dl_dir
    lv_conf_dir = os.path.join(ws.board_dir, "nuttx")
    if os.path.isfile(os.path.join(lv_conf_dir, "lv_conf.h")):
        env["LV_CONF_PATH"] = lv_conf_dir

    # CMake configure
    apps_abs = os.path.abspath(apps_build)
    cmake_args = [
        cmake,
        f"-S{os.path.abspath(nuttx_src)}",
        f"-B{os.path.abspath(cmake_build)}",
        f"-DBOARD_CONFIG={nuttx_board_cfg}",
        f"-DNUTTX_APPS_DIR={apps_abs}",
    ]

    logger.debug(f"CMake configure: {' '.join(cmake_args)}")
    run(cmake_args, env=env, cwd=nuttx_src, log_file=ws.build_log)

    # Build
    run([cmake, "--build", os.path.abspath(cmake_build), f"-j{nproc()}"],
        env=env, log_file=ws.build_log)

    # Copy images
    os.makedirs(ws.images_dir, exist_ok=True)
    for name in ("nuttx", "nuttx.bin"):
        src = os.path.join(cmake_build, name)
        dst_name = "firmware.elf" if name == "nuttx" else "firmware.bin"
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(ws.images_dir, dst_name))

    _check_rtos_config_drift(ws, os.path.join(cmake_build, ".config"))
    _create_run_or_flash_script(ws, rtos="nuttx")


def build_posix(ws):
    """Build POSIX native executable (or WASM) via CMake."""
    is_wasm = get_bool(ws.config, "CONFIG_OVE_BOARD_WASM")

    cmake = _find_cmake()
    env = ws.toolchain_env()
    board_dir = os.path.join(ws.board_dir, "posix")
    fw_build = os.path.join(ws.build_dir, "firmware")
    os.makedirs(fw_build, exist_ok=True)

    if is_wasm:
        logger.info("Building WASM/Emscripten target")

        # Find emcmake: first try downloaded emsdk, then PATH.
        # Resolve through the per-workspace symlink to the canonical
        # `dl/emsdk-<hash>/` realpath. Emscripten's sanity check stamps
        # the absolute emsdk path into the cache; passing the symlinked
        # `<workspace>/dl/emsdk` makes every app look like a different
        # SDK install ("(Emscripten: config changed, clearing cache)"),
        # and the in-flight zig @cImport then races the cache wipe and
        # fails with FileNotFound on the sysroot headers.
        emsdk_link = os.path.join(ws.ws_dl_dir, "emsdk")
        if os.path.exists(emsdk_link):
            emsdk_dir = os.path.realpath(emsdk_link)
        else:
            emsdk_dir = os.path.realpath(os.path.join(ws.dl_dir, "emsdk"))
        em_bin = os.path.join(emsdk_dir, "upstream", "emscripten")
        emcmake = os.path.join(em_bin, "emcmake")
        emmake = os.path.join(em_bin, "emmake")

        if not os.path.isfile(emcmake):
            # Fallback to PATH
            emcmake = shutil.which("emcmake")
            emmake = shutil.which("emmake")

        if not emcmake or not os.path.isfile(emcmake):
            logger.error("Emscripten SDK not found. Run 'ove download' first.")
            sys.exit(1)

        # Add emsdk paths to env so emcmake can find node, etc.
        emsdk_node = os.path.join(emsdk_dir, "node")
        node_dirs = [os.path.join(emsdk_node, d, "bin")
                     for d in os.listdir(emsdk_node)
                     if os.path.isdir(os.path.join(emsdk_node, d, "bin"))] \
                    if os.path.isdir(emsdk_node) else []
        extra_path = os.pathsep.join([em_bin, emsdk_dir] + node_dirs)
        env["PATH"] = extra_path + os.pathsep + env.get("PATH", "")
        env["EMSDK"] = emsdk_dir
        env["EM_CONFIG"] = os.path.join(emsdk_dir, ".emscripten")
        # Pin the emcc cache to the canonical SDK install rather than
        # the workspace-local symlink, so concurrent allconfigs builds
        # share one cache and don't race on per-app cache rebuilds.
        env["EM_CACHE"] = os.path.join(emsdk_dir, "upstream", "emscripten", "cache")

        if not emmake or not os.path.isfile(emmake):
            emmake = os.path.join(em_bin, "emmake")

        run([
            emcmake, cmake,
            f"-DOVE_DIR={ws.ove_dir}",
            f"-DOVE_APP_DIR={ws.app_dir}",
            f"-DOVE_GEN_DIR={ws.gen_dir}",
            f"-DOVE_DL_DIR={ws.ws_dl_dir}",
            board_dir,
        ], env=env, cwd=fw_build, log_file=ws.build_log)
        run([emmake, "make", f"-j{nproc()}"], env=env, cwd=fw_build,
            log_file=ws.build_log)
    else:
        logger.info("Building POSIX native executable")
        run([
            cmake,
            f"-DOVE_DIR={ws.ove_dir}",
            f"-DOVE_APP_DIR={ws.app_dir}",
            f"-DOVE_GEN_DIR={ws.gen_dir}",
            f"-DOVE_DL_DIR={ws.ws_dl_dir}",
            board_dir,
        ], env=env, cwd=fw_build, log_file=ws.build_log)
        run([cmake, "--build", fw_build, f"-j{nproc()}"], env=env,
            log_file=ws.build_log)

    os.makedirs(ws.images_dir, exist_ok=True)
    if is_wasm:
        for name in ("ove_wasm.html", "ove_wasm.js", "ove_wasm.wasm",
                      "ove_wasm.worker.js"):
            src = os.path.join(fw_build, name)
            if os.path.isfile(src):
                shutil.copy2(src, ws.images_dir)

        # Assemble serve directory with all assets needed to run
        _assemble_wasm_serve(ws, fw_build)
    else:
        src = os.path.join(fw_build, "ove_posix")
        if os.path.isfile(src):
            shutil.copy2(src, ws.images_dir)

        # Create run script — mirrors 'ove run' for POSIX: launch the
        # dashboard bridge in the background, then run the binary.
        run_script = os.path.join(ws.workspace_dir, "run")
        ove_dir_q = shlex.quote(ws.ove_dir)
        with open(run_script, "w") as f:
            f.write('#!/bin/bash\n')
            f.write('DIR="$(cd "$(dirname "$0")" && pwd)"\n')
            f.write(f'OVE_DIR={ove_dir_q}\n')
            f.write('export OVE_DIR\n')
            f.write('\n')
            f.write('HEADLESS=0\n')
            f.write('ARGS=()\n')
            f.write('for arg in "$@"; do\n')
            f.write('    case "$arg" in\n')
            f.write('        --headless) HEADLESS=1 ;;\n')
            f.write('        *) ARGS+=("$arg") ;;\n')
            f.write('    esac\n')
            f.write('done\n')
            f.write('\n')
            f.write('BRIDGE_PID=""\n')
            f.write('cleanup() {\n')
            f.write('    if [ -n "$BRIDGE_PID" ]; then\n')
            f.write('        kill "$BRIDGE_PID" 2>/dev/null || true\n')
            f.write('        wait "$BRIDGE_PID" 2>/dev/null || true\n')
            f.write('    fi\n')
            f.write('}\n')
            f.write('trap cleanup EXIT\n')
            f.write('\n')
            f.write('if [ "$HEADLESS" != "1" ]; then\n')
            f.write('    PYTHON="$OVE_DIR/.venv/bin/python3"\n')
            f.write('    [ -x "$PYTHON" ] || PYTHON=python3\n')
            f.write('    "$PYTHON" "$OVE_DIR/config/scripts/ove-dashboard-bridge.py" \\\n')
            f.write('        --port 8080 --dashboard "$OVE_DIR/sim/dashboard" &\n')
            f.write('    BRIDGE_PID=$!\n')
            f.write('fi\n')
            f.write('\n')
            f.write('"$DIR/images/ove_posix" "${ARGS[@]}"\n')
        os.chmod(run_script, 0o755)


def _assemble_wasm_serve(ws, fw_build):
    """Assemble the serve/ directory with build artifacts + dashboard assets."""
    serve_dir = os.path.join(ws.workspace_dir, "serve")
    os.makedirs(serve_dir, exist_ok=True)

    # Copy build artifacts
    shutil.copy2(os.path.join(fw_build, "ove_wasm.html"),
                 os.path.join(serve_dir, "index.html"))
    for name in ("ove_wasm.js", "ove_wasm.wasm", "ove_wasm.wasm.map",
                 "ove_wasm.worker.js"):
        src = os.path.join(fw_build, name)
        if os.path.isfile(src):
            shutil.copy2(src, serve_dir)

    # Copy dashboard assets
    dash_dir = os.path.join(ws.ove_dir, "sim", "dashboard")
    for f in ("app.js", "style.css", "coi-serviceworker.js"):
        src = os.path.join(dash_dir, f)
        if os.path.isfile(src):
            shutil.copy2(src, serve_dir)

    # Generate run.sh inside serve/
    _write_wasm_run_sh(os.path.join(serve_dir, "run.sh"), ws.ove_dir)

    # Generate workspace-level run script (matches POSIX convention)
    run_script = os.path.join(ws.workspace_dir, "run")
    with open(run_script, "w") as f:
        f.write('#!/bin/bash\n')
        f.write('set -e\n')
        f.write('DIR="$(cd "$(dirname "$0")" && pwd)"\n')
        f.write('exec "$DIR/serve/run.sh" "$@"\n')
    os.chmod(run_script, 0o755)


def _write_wasm_run_sh(path, ove_dir):
    """Generate a self-contained run.sh that serves the WASM build.

    Serves the serve/ directory (dashboard + wasm) with a fallback to
    ove_dir — the repo root — so that DWARF-referenced source files
    (paths like /apps/c/example/src/app.c) resolve in Chrome DevTools.
    """
    content = '''#!/usr/bin/env bash
# Auto-generated — serves this WASM build with COOP/COEP headers.
# Usage: ./run.sh [PORT]

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="{repo}"
PORT="${{1:-8080}}"

echo "=== Serving WASM at http://localhost:$PORT ==="
echo "  Files:  $DIR"
echo "  Repo:   $REPO (source-map fallback)"
echo "  Press Ctrl+C to stop."

# Open browser (best-effort, non-blocking)
( sleep 1 && python3 -c "import webbrowser; webbrowser.open('http://localhost:$PORT')" ) 2>/dev/null &

python3 -c "
import http.server, functools, sys, os, posixpath, urllib.parse

SERVE_DIR = sys.argv[2]
REPO_DIR  = sys.argv[3]

class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()
    def log_message(self, *a):
        pass
    def translate_path(self, path):
        # Strip query/fragment, normalize.
        p = urllib.parse.urlparse(path).path
        p = posixpath.normpath(urllib.parse.unquote(p)).lstrip('/')
        primary = os.path.join(SERVE_DIR, p)
        if os.path.exists(primary):
            return primary
        # Fallback to the repo root so DWARF-referenced .c/.h files
        # load in Chrome DevTools.
        fallback = os.path.join(REPO_DIR, p)
        if os.path.exists(fallback):
            return fallback
        return primary

s = http.server.HTTPServer(('127.0.0.1', int(sys.argv[1])), H)
try:
    s.serve_forever()
except KeyboardInterrupt:
    print('\\\\nStopped.')
" "$PORT" "$DIR" "$REPO"
'''.format(repo=ove_dir)
    with open(path, "w") as f:
        f.write(content)
    os.chmod(path, 0o755)


def _write_build_id_header(ws):
    """Generate the build ID header the firmware prints at boot.

    Written at build time rather than configure time: the ID pins source
    revisions, so a value baked in at configure would go stale on the next
    commit — precisely the misidentified image this exists to prevent.

    The content is derived only from configuration and revisions, never a
    timestamp: builds of the same source must produce the same ID, and a
    no-op rebuild must leave this header byte-identical so it does not force a
    recompile of everything that includes it.
    """
    from . import image_id

    ident = image_id.build_id(ws).replace("\\", "\\\\").replace('"', '\\"')
    sources = image_id.source_ids(ws)
    ove_rev = sources["overtos"]
    lxp_rev = sources["lxp"]
    content = ("/* Generated by 'ove build' — do not edit. */\n"
               "#ifndef OVE_BUILD_ID_H\n"
               "#define OVE_BUILD_ID_H\n"
               f'#define OVE_BUILD_ID "{ident}"\n'
               f'#define OVE_BUILD_OVERTOS_REV "{ove_rev}"\n'
               f'#define OVE_BUILD_LXP_REV "{lxp_rev}"\n'
               "#endif\n")
    path = os.path.join(ws.gen_dir, "ove_build_id.h")
    if os.path.isfile(path):
        with open(path) as f:
            if f.read() == content:
                return
    os.makedirs(ws.gen_dir, exist_ok=True)
    with open(path, "w") as f:
        f.write(content)


def _copy_images(ws, fw_build):
    """Copy firmware artifacts to images/ directory."""
    os.makedirs(ws.images_dir, exist_ok=True)
    for name in ("firmware.elf", "firmware.bin", "firmware.hex"):
        src = os.path.join(fw_build, name)
        if os.path.isfile(src):
            shutil.copy2(src, ws.images_dir)


def _create_run_or_flash_script(ws, rtos=None):
    """Create convenience run or flash script in workspace."""
    rtos = rtos or ws.rtos
    qemu_script = os.path.join(ws.board_dir, "qemu-run.sh")
    # Resolve against the active images dir: an ABI-variant build does not live
    # directly under images/, so this path is not always "images/firmware.elf".
    rel_elf = os.path.join(os.path.relpath(ws.images_dir, ws.workspace_dir),
                           "firmware.elf")

    if os.path.isfile(qemu_script):
        # QEMU board: create run script
        flash_script = os.path.join(ws.workspace_dir, "flash")
        if os.path.isfile(flash_script):
            os.unlink(flash_script)
        run_script = os.path.join(ws.workspace_dir, "run")
        with open(run_script, "w") as f:
            f.write('#!/bin/bash\n')
            f.write('set -e\n')
            f.write('DIR="$(cd "$(dirname "$0")" && pwd)"\n')
            f.write(f'exec {qemu_script} "$DIR/{rel_elf}" "$@"\n')
        os.chmod(run_script, 0o755)
    else:
        # Real hardware: create flash script
        run_script = os.path.join(ws.workspace_dir, "run")
        if os.path.isfile(run_script):
            os.unlink(run_script)
        flash_sh = os.path.join(ws.board_dir, rtos, "flash.sh")
        flash_script = os.path.join(ws.workspace_dir, "flash")
        with open(flash_script, "w") as f:
            f.write('#!/bin/bash\n')
            f.write('set -e\n')
            f.write('DIR="$(cd "$(dirname "$0")" && pwd)"\n')
            f.write(f'exec {flash_sh} "$DIR/{rel_elf}"\n')
        os.chmod(flash_script, 0o755)




def _check_generated_current(ws):
    """Refuse to build when generated/ came from a different .config.

    ove configure renders ove_config.h / ove_config.cmake from .config and
    nothing re-runs it automatically. The stale header is still valid C, so the
    firmware compiles cleanly against the *previous* configuration and the
    mismatch only surfaces as inexplicable runtime behaviour — a soft-guest
    build running the hard-float self-test, say.
    """
    from .configure import CONFIG_SENTINEL
    from .utils import hash_file

    sentinel = os.path.join(ws.workspace_dir, CONFIG_SENTINEL)
    if not os.path.isfile(sentinel):
        # Predates this check, or generated/ was never produced here; staleness
        # is unprovable either way, so say so rather than fail.
        logger.warning(
            "generated/ has no record of the .config it came from — run "
            "'ove configure' if this workspace was configured by an older "
            "checkout")
        return
    with open(sentinel) as f:
        recorded = f.read().strip()
    if hash_file(ws.config_path) != recorded:
        logger.error(
            ".config has changed since the last 'ove configure'. generated/ "
            "still holds the previous configuration and this build would "
            "silently use it. Run 'ove configure' first.")
        sys.exit(1)


def _check_rtos_config_drift(ws, config_path):
    """Warn if RTOS .config changed during build (post-build drift detection)."""
    sentinel = os.path.join(ws.workspace_dir, ".rtos_config_applied_sha256")
    if not os.path.isfile(sentinel) or not os.path.isfile(config_path):
        return
    from .utils import hash_file
    current_hash = hash_file(config_path)
    with open(sentinel) as f:
        expected_hash = f.read().strip()
    if current_hash != expected_hash:
        logger.warning("RTOS config was modified during build "
              "(possible CMake reconfigure or Kconfig dependency resolution).")
        logger.debug("Run 'ove rtos-menuconfig' to inspect.")


def build(ws):
    """Auto-detect RTOS and build."""
    from .manifest import warn_if_dirty

    ws.require_config()
    ws.ensure_dirs()
    warn_if_dirty(ws.ove_dir)
    rtos = ws.rtos
    if not rtos:
        logger.error("no RTOS selected in .config")
        sys.exit(1)

    _preflight_check(ws)
    _check_generated_current(ws)

    dispatch = {
        "freertos": build_freertos,
        "zephyr": build_zephyr,
        "nuttx": build_nuttx,
        "posix": build_posix,
    }

    builder = dispatch.get(rtos)
    if not builder:
        logger.error(f"unknown RTOS '{rtos}'")
        sys.exit(1)

    if get_bool(ws.config, "CONFIG_OVE_ZERO_HEAP"):
        logger.debug("[zero-heap] Fully static build — no dynamic allocation.")

    _write_build_id_header(ws)

    log_path = os.path.join(ws.workspace_dir, "build.log")
    with ws.open_build_log(log_path):
        builder(ws)
    _link_compile_commands(ws)

    # Record what was just built, after the artifacts exist so their hashes are
    # the ones 'ove flash' will check.
    from . import image_id
    ident = image_id.write(ws)

    logger.info("Build complete")
    logger.info(f"Firmware: {ws.images_dir}/")
    logger.info(f"Build ID: {ident['build_id']}")


def _link_compile_commands(ws):
    """Symlink output/compile_commands.json -> active workspace's copy.

    Lets clangd find the active build's compile DB without per-workspace
    configuration. Preferred candidate is the firmware build dir; falls
    back to the cmake-build dir used by NuttX.
    """
    if ws.is_isolated:
        return

    candidates = [
        os.path.join(ws.build_dir, "firmware", "compile_commands.json"),
        os.path.join(ws.build_dir, "nuttx-cmake", "compile_commands.json"),
    ]
    src = next((c for c in candidates if os.path.isfile(c)), None)
    if not src:
        return
    link = os.path.join(ws.output_dir, "compile_commands.json")
    rel = os.path.relpath(src, os.path.dirname(link))
    # If a regular file occupies the slot (legacy state), refuse to
    # clobber — atomic_symlink would replace it.
    if os.path.exists(link) and not os.path.islink(link):
        return
    try:
        atomic_symlink(rel, link)
    except OSError as e:
        logger.debug(f"compile_commands.json symlink skipped: {e}")


def cmd_build(args):
    """CLI entry point for 'ove build'."""
    import json
    import time

    ws = Workspace()
    if getattr(args, "dry_run", False):
        ws.require_config()
        print(f"[dry-run] would build {ws.rtos} firmware for "
              f"board={ws.board_name} app={os.path.basename(ws.app_dir or '')}")
        print(f"[dry-run] workspace:     {ws.workspace_dir}")
        print(f"[dry-run] build dir:     {ws.build_dir}")
        print(f"[dry-run] images dir:    {ws.images_dir}")
        if ws.toolchain_dir:
            print(f"[dry-run] toolchain dir: {ws.toolchain_dir}")
        return
    start = time.time()
    ok = True
    try:
        build(ws)
    except SystemExit as e:
        ok = (e.code == 0)
        raise
    finally:
        if getattr(args, "json", False):
            elapsed = round(time.time() - start, 2)
            firmware = os.path.join(ws.images_dir, "firmware.elf")
            payload = {
                "ok": ok,
                "rtos": ws.rtos,
                "board": ws.board_name,
                "workspace": ws.workspace_dir,
                "elapsed_s": elapsed,
                "firmware": firmware if os.path.isfile(firmware) else None,
                "firmware_bytes": (os.path.getsize(firmware)
                                   if os.path.isfile(firmware) else None),
            }
            json.dump(payload, sys.stdout, indent=2)
            print()
