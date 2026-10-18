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
import shutil
import subprocess
import sys

from .utils import run, nproc, apply_defconfig_overlay, atomic_symlink
from .board_yaml import nuttx_board_config, rtos_entry, zephyr_board as yaml_zephyr_board
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
    if rtos_entry(ws.board_dir, rtos) is None:
        logger.error(
            f"Board '{ws.board_name}' does not support RTOS '{rtos}' "
            f"(no rtos.{rtos} in its board.yaml)")
        sys.exit(1)


_PATCH_STATE_VERSION = 1
_PATCH_STATE_FILE = ".ove_patch_state.json"

# A rootfs.cpio for a Linux personality web bundle in place of the Buildroot
# output the workspace configures.
LINUX_ROOTFS_ENV = "OVE_LINUX_ROOTFS"


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


_SOURCE_TREES_DIR = ".ove-worktrees"
_SOURCE_TREES_RECORD = ".ove-source-trees"


def _shared_patched_tree(ws, source_dir, layers, *, log_file=None):
    """Return the patched source tree shared by every workspace with this patch set.

    The tree is named after its content (source revision plus the ordered patch
    set), next to the download, so workspaces with the same patches use one
    checkout. Builds treat it as read-only. A lock serializes its preparation,
    and the workspace records it for `ove clean --sources`.
    """
    import fcntl

    source_dir = os.path.realpath(source_dir)
    manifest = _patch_manifest(source_dir, _collect_patch_series(layers))
    key = hashlib.sha256(
        json.dumps(manifest, sort_keys=True).encode()).hexdigest()[:16]
    root = os.path.join(os.path.dirname(source_dir), _SOURCE_TREES_DIR)
    tree = os.path.join(root, f"{os.path.basename(source_dir)}-{key}")
    os.makedirs(root, exist_ok=True)
    with open(tree + ".lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        _prepare_patched_git_tree(source_dir, tree, layers, log_file=log_file)
        _record_source_tree(ws, tree)
    return tree


def _record_source_tree(ws, tree):
    """Note in the workspace that its build uses a shared source tree."""
    record = os.path.join(ws.build_dir, _SOURCE_TREES_RECORD)
    trees = set()
    if os.path.isfile(record):
        with open(record) as f:
            trees = {line.strip() for line in f if line.strip()}
    if tree in trees:
        return
    trees.add(tree)
    os.makedirs(ws.build_dir, exist_ok=True)
    with open(record, "w") as f:
        f.writelines(f"{path}\n" for path in sorted(trees))


def _source_tree_roots(dl_dir):
    """Every directory that holds shared source trees, with its Git source."""
    import glob

    roots = {}
    for candidate in glob.glob(os.path.join(dl_dir, "zephyr-workspace-*")):
        topdir = os.path.realpath(candidate)
        roots[os.path.join(topdir, _SOURCE_TREES_DIR)] = topdir
    roots[os.path.join(os.path.realpath(dl_dir), _SOURCE_TREES_DIR)] = None
    return {root: topdir for root, topdir in roots.items() if os.path.isdir(root)}


def _discard_source_tree(path):
    """Remove one shared tree (or a legacy one) and its Git registration."""
    git_file = os.path.join(path, ".git")
    source = None
    if os.path.isfile(git_file):
        with open(git_file) as f:
            gitdir = f.read().strip().removeprefix("gitdir: ")
        # <source>/.git/worktrees/<name>
        source = os.path.dirname(os.path.dirname(os.path.dirname(gitdir)))
    shutil.rmtree(path, ignore_errors=True)
    if source and os.path.isdir(source):
        run(["git", "worktree", "prune"], cwd=source)


def discard_unreferenced_source_trees(ove_dir, dl_dir):
    """Remove shared source trees no workspace under output/ records."""
    import fcntl
    import glob

    referenced = set()
    pattern = os.path.join(ove_dir, "output", "**", _SOURCE_TREES_RECORD)
    for record in glob.glob(pattern, recursive=True):
        with open(record) as f:
            referenced.update(line.strip() for line in f if line.strip())
    removed = 0
    for root in _source_tree_roots(dl_dir):
        for name in sorted(os.listdir(root)):
            path = os.path.join(root, name)
            if not os.path.isdir(path) or path in referenced:
                continue
            # The lock file stays: a build may be waiting on it.
            with open(path + ".lock", "w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                _discard_source_tree(path)
            removed += 1
    return removed


def discard_all_patch_worktrees(dl_dir):
    """Remove every shared source tree retained outside output/."""
    for root in _source_tree_roots(dl_dir):
        for name in sorted(os.listdir(root)):
            path = os.path.join(root, name)
            if os.path.isdir(path):
                _discard_source_tree(path)
        shutil.rmtree(root)


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
    if not os.path.isdir(freertos_download):
        logger.error("FreeRTOS-Kernel not downloaded: run 'ove download' first")
        sys.exit(1)
    patch_layers = [("board patch", os.path.join(board_dir, "patches"))]
    if ws.config.get("CONFIG_OVE_LINUX"):
        patch_layers.append((
            "LXP FreeRTOS port patch",
            os.path.join(ws.ove_dir, "modules", "lxp", "ports",
                         "freertos", "patches")))
    if ws.app_dir:
        patch_layers.append((
            "app patch", os.path.join(ws.app_dir, "patches", "freertos")))
    freertos_src = _shared_patched_tree(
        ws, freertos_download, patch_layers, log_file=ws.build_log)

    toolchain_file = os.path.join(board_dir, "cmake", "arm-none-eabi.cmake")

    cmake_args = [
        cmake,
        "-DOVE_DIR=" + ws.ove_dir,
        "-DOVE_APP_DIR=" + ws.app_dir,
        "-DOVE_GEN_DIR=" + ws.gen_dir,
        "-DOVE_DL_DIR=" + ws.ws_dl_dir,
        "-DBOARD_DIR=" + board_dir,
        "-DFREERTOS_PATH=" + freertos_src,
    ]

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

        # The shared tree sits below the original west workspace so west still
        # discovers its manifest and modules.
        zephyr_ws = _shared_patched_tree(
            ws, zephyr_download, patch_layers, log_file=ws.build_log)
        env["ZEPHYR_BASE"] = zephyr_ws

    _prepare_zephyr_build_dir(fw_build, zephyr_ws)

    # Apply layered config (build guard — fresh by construction)
    from .rtos_menuconfig import ensure_rtos_config_applied
    conf_file = ensure_rtos_config_applied(ws, "zephyr")

    # Map oveRTOS board name to Zephyr board identifier
    zephyr_board = yaml_zephyr_board(ws.board_dir)
    if not zephyr_board:
        logger.error(f"no rtos.zephyr.board in {ws.board_name}/board.yaml")
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

    # Start from the board's patched upstream defconfig: the overlays below
    # only replace the keys they name, so lines from an earlier build (a
    # feature since switched off, a template line since removed) would stay.
    run(["git", "checkout", "--", defconfig], cwd=nuttx_src,
        log_file=ws.build_log)

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
    nuttx_board_cfg = nuttx_board_config(ws.board_dir)
    if not nuttx_board_cfg:
        logger.error(f"no rtos.nuttx in {ws.board_name}/board.yaml")
        sys.exit(1)

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
    _check_nuttx_config_landed(ws, os.path.join(cmake_build, ".config"))

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

    _create_run_or_flash_script(ws, rtos="nuttx")


def _check_nuttx_config_landed(ws, config_path):
    """Fail when NuttX's olddefconfig dropped or changed a requested line.

    kconfiglib drops an assignment to an unknown symbol, or to one whose
    dependencies are unmet, without a word, so a renamed upstream symbol or a
    misplaced board line would silently do nothing.
    """
    from .utils import config_assignments, unlanded_assignments

    layers = [os.path.join(ws.gen_dir, "merged_nuttx_defconfig")]
    if os.path.isfile(ws.rtos_config_path):
        layers.append(ws.rtos_config_path)
    layers = [path for path in layers if os.path.isfile(path)]
    if not layers or not os.path.isfile(config_path):
        return
    missed = unlanded_assignments(config_assignments(*layers), config_path)
    if not missed:
        return
    logger.error("NuttX did not take these requested config lines "
                 "(unknown symbol, unmet dependency, or overridden):")
    for key, want, got in missed:
        shown = "absent" if got is None else got
        logger.error(f"  {key}: requested {want}, got {shown}")
    sys.exit(1)


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




def _web_index_html(dashboard_dir):
    """Return the dashboard page adapted to run the firmware in the page.

    sim/dashboard/index.html is the page the local bridge serves.  Two
    marked spots differ in the browser bundle: a static host cannot send
    the cross-origin isolation headers, so coi-serviceworker.js takes the
    place of the script that removes stale service workers; and
    qemu-wasm.js is loaded ahead of app.js to provide the firmware link.
    """
    with open(os.path.join(dashboard_dir, "index.html")) as f:
        html = f.read()
    html, heads = re.subn(
        r"<!-- ove:web-head .*?<!-- ove:web-head-end -->",
        '<script src="coi-serviceworker.js"></script>', html, flags=re.S)
    html, links = re.subn(
        r"<!-- ove:web-link .*?-->",
        '<script src="qemu-wasm.js"></script>', html, flags=re.S)
    if heads != 1 or links != 1:
        raise RuntimeError("sim/dashboard/index.html: the web bundle "
                           "markers (ove:web-head, ove:web-link) are gone")
    return html


def _strip_firmware(ws, src, dest):
    """Copy the firmware ELF without symbols and debug info.

    The browser downloads this file; QEMU only needs the load segments.
    Falls back to a plain copy when no objcopy is found.
    """
    path = ws.toolchain_env().get("PATH")
    for name in ("arm-none-eabi-objcopy", "arm-zephyr-eabi-objcopy"):
        objcopy = shutil.which(name, path=path)
        if not objcopy:
            continue
        ret = subprocess.run([objcopy, "--strip-all", src, dest],
                             capture_output=True, text=True)
        if ret.returncode == 0:
            return
        logger.debug(f"{name} --strip-all failed: {ret.stderr.strip()}")
    shutil.copy2(src, dest)


# QEMU machine options of the boards a browser bundle can run, as their
# qemu-run.sh passes them.
_WEB_MACHINES = {
    "qemu-mps2-an500": ["-machine", "mps2-an500"],
    "qemu-mps2-an521": ["-cpu", "cortex-m33", "-machine", "mps2-an521"],
}


def _assemble_web_bundle(ws):
    """Package the firmware for a web browser into <workspace>/serve/.

    serve/ holds the page, the firmware, web.json naming what to run, and
    qemu/, a link to the WebAssembly QEMU under output/tools/.  It needs
    nothing else from the tree: copy it with the link dereferenced to put it
    on a static host.  serve/run.sh serves it locally.

    A Linux personality build gets a terminal on the guest console and its
    rootfs instead of the dashboard, which needs the sim framework.
    """
    from .download import build_qemu_wasm
    from .manifest import load_manifest

    firmware = os.path.join(ws.images_dir, "firmware.elf")
    if not os.path.isfile(firmware):
        logger.error(f"web bundle: {firmware} not found")
        sys.exit(1)
    machine = _WEB_MACHINES.get(ws.board_name)
    if machine is None:
        logger.error(f"web bundle: no QEMU machine known for {ws.board_name}")
        sys.exit(1)

    emulator = build_qemu_wasm(ws.ove_dir, ws.dl_dir,
                               os.path.join(ws.output_dir, "tools"),
                               manifest=load_manifest(ws.ove_dir))
    if emulator is None:
        logger.error("web bundle: the WebAssembly QEMU could not be built; "
                     "see 'ove ensure-toolchain qemu-wasm'")
        sys.exit(1)

    serve_dir = os.path.join(ws.workspace_dir, "serve")
    os.makedirs(serve_dir, exist_ok=True)

    dash_dir = os.path.join(ws.ove_dir, "sim", "dashboard")
    if get_bool(ws.config, "CONFIG_OVE_LINUX"):
        shutil.copy2(os.path.join(dash_dir, "terminal.html"),
                     os.path.join(serve_dir, "index.html"))
        pages = ("terminal.js", "coi-serviceworker.js")
        web = _personality_web_config(ws, machine, serve_dir)
    else:
        with open(os.path.join(serve_dir, "index.html"), "w") as f:
            f.write(_web_index_html(dash_dir))
        pages = ("app.js", "style.css", "coi-serviceworker.js", "qemu-wasm.js")
        web = {"firmware": "firmware.elf", "emulator": "qemu/",
               "machine": machine[-1]}
    for name in pages:
        shutil.copy2(os.path.join(dash_dir, name), serve_dir)

    _strip_firmware(ws, firmware, os.path.join(serve_dir, "firmware.elf"))
    atomic_symlink(emulator, os.path.join(serve_dir, "qemu"))
    with open(os.path.join(serve_dir, "web.json"), "w") as f:
        json.dump(web, f, indent=2)
        f.write("\n")

    _write_web_run_sh(os.path.join(serve_dir, "run.sh"))

    # The workspace-level run script follows 'ove run': the browser.
    run_script = os.path.join(ws.workspace_dir, "run")
    with open(run_script, "w") as f:
        f.write('#!/bin/bash\n')
        f.write('set -e\n')
        f.write('DIR="$(cd "$(dirname "$0")" && pwd)"\n')
        f.write('exec "$DIR/serve/run.sh" "$@"\n')
    os.chmod(run_script, 0o755)
    logger.info(f"Web bundle: {serve_dir}/")


def _personality_web_config(ws, machine, serve_dir):
    """Stage a Linux personality build's rootfs; return its web.json.

    The page runs QEMU with the arguments the board's qemu-run.sh uses for
    a personality build: the guest console on UART1 (stdio), the rootfs
    loaded raw into its window, and the image fit-rootfs.py picks for that
    window.  The rootfs is the configured Buildroot output's, or the file
    OVE_LINUX_ROOTFS names (the docs demos use a published image).
    """
    from .workspace import get_str

    rootfs = os.environ.get(LINUX_ROOTFS_ENV)
    if not rootfs:
        br = get_str(ws.config, "CONFIG_OVE_BUILDROOT") or "../buildroot"
        out = get_str(ws.config, "CONFIG_OVE_LINUX_ROOTFS_OUTPUT") or "output"
        if not os.path.isabs(br):
            br = os.path.join(ws.ove_dir, br)
        rootfs = os.path.normpath(
            os.path.join(br, out, "images", "rootfs.cpio"))
    base = get_str(ws.config, "CONFIG_OVE_LINUX_ROOTFS_BASE")
    size = get_str(ws.config, "CONFIG_OVE_LINUX_ROOTFS_SIZE")
    if not os.path.isfile(rootfs):
        logger.error(f"web bundle: {rootfs} not found (build Buildroot first)")
        sys.exit(1)
    if not base or not size:
        logger.error("web bundle: the rootfs window is not configured")
        sys.exit(1)
    base, size = int(base, 0), int(size, 0)

    fit = subprocess.run(
        [sys.executable, os.path.join(ws.ove_dir, "boards", "qemu-mps2",
                                      "fit-rootfs.py"),
         rootfs, str(size),
         os.path.join(ws.build_dir, "rootfs-fit.cpio")],
        capture_output=True, text=True)
    if fit.returncode != 0:
        logger.error(f"web bundle: {fit.stderr.strip()}")
        sys.exit(1)
    shutil.copy2(fit.stdout.strip(), os.path.join(serve_dir, "rootfs.cpio"))

    args = machine + [
        "-m", "16",
        # UART1 is the guest console; UART0, the engine's, is discarded.
        "-semihosting-config", "enable=on,chardev=c0", "-chardev", "null,id=c0",
        "-serial", "none", "-serial", "stdio", "-monitor", "none",
        "-display", "none",
        "-kernel", "/firmware.elf",
        "-device", f"loader,file=/rootfs.cpio,addr={base:#x},force-raw=on",
        # The default translation cache would take hundreds of megabytes.
        "-accel", "tcg,tb-size=32",
    ]
    return {"firmware": "firmware.elf", "emulator": "qemu/",
            "files": {"rootfs.cpio": "/rootfs.cpio"}, "args": args}


def _write_web_run_sh(path):
    """Generate serve/run.sh: a local server for the browser bundle.

    The emulator's threads need a cross-origin isolated page.  This server
    sends the headers for it; on a static host coi-serviceworker.js does.
    """
    content = '''#!/usr/bin/env bash
# Auto-generated — serves this browser bundle with COOP/COEP headers.
# Usage: ./run.sh [PORT]

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:-8080}"

echo "=== Serving the browser bundle at http://localhost:$PORT ==="
echo "  Files:  $DIR"
echo "  Press Ctrl+C to stop."

# Open browser (best-effort, non-blocking)
( sleep 1 && python3 -c "import webbrowser; webbrowser.open('http://localhost:$PORT')" ) 2>/dev/null &

exec python3 -c "
import functools, http.server, sys

class H(http.server.SimpleHTTPRequestHandler):
    extensions_map = dict(http.server.SimpleHTTPRequestHandler.extensions_map,
                          **{'.wasm': 'application/wasm'})
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()
    def log_message(self, *a):
        pass

handler = functools.partial(H, directory=sys.argv[2])
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), handler).serve_forever()
" "$PORT" "$DIR"
'''
    with open(path, "w") as f:
        f.write(content)
    os.chmod(path, 0o755)


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
    if get_bool(ws.config, "CONFIG_OVE_SIM_WEB"):
        _assemble_web_bundle(ws)

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
