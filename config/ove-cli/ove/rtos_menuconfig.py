# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""RTOS native menuconfig — Buildroot-style nested config with build guard."""

import os
import shutil
import sys

from .configure import configure_all
from .constants import NUTTX_BOARD_CONFIGS, ZEPHYR_BOARD_MAPPINGS
from .utils import apply_defconfig_overlay, diff_configs, hash_file, run
from .workspace import Workspace


def merge_rtos_config_layers(ws, rtos):
    """Merge config layers 1-3 into a single file.

    Layer stack (lowest to highest priority):
      1. oveRTOS template base  (generated/nuttx_defconfig or generated/prj.conf)
      2. Board overlay          (NuttX only — Zephyr auto-detects boards/*.conf)
      3. App overlay            (apps/<app>/nuttx_defconfig or apps/<app>/prj.conf)

    Layer 4 (rtos.config) is NOT included here. Callers apply it separately
    so the reference snapshot (layers 1-3 only) can be taken first.

    Returns the path to the merged file in ws.gen_dir.
    """
    os.makedirs(ws.gen_dir, exist_ok=True)

    if rtos == "nuttx":
        return _merge_nuttx_layers(ws)
    elif rtos == "zephyr":
        return _merge_zephyr_layers(ws)
    else:
        return None


def _merge_nuttx_layers(ws):
    """Merge NuttX defconfig layers 1-3 into gen_dir/merged_nuttx_defconfig.

    Board overlay layer:
      ove_board_defconfig            — always applied
      ove_board_defconfig.<feature>  — applied only when CONFIG_OVE_<FEATURE>
                                       is set in the oveRTOS workspace config
                                       (e.g. ove_board_defconfig.net loads
                                       when CONFIG_OVE_NET=y).  Lets a board
                                       contribute hardware-specific NuttX
                                       symbols (e.g. STM32F7_ETHMAC) only
                                       when the relevant oveRTOS feature is
                                       enabled — without those guards the
                                       extra symbols would `select` other
                                       NuttX options (NETDEVICES) and break
                                       olddefconfig for apps that don't
                                       enable the feature.
    """
    from .workspace import get_bool

    base = os.path.join(ws.gen_dir, "nuttx_defconfig")
    merged = os.path.join(ws.gen_dir, "merged_nuttx_defconfig")

    if not os.path.isfile(base):
        print("error: generated/nuttx_defconfig not found. "
              "Run 'ove configure' first.")
        sys.exit(1)

    shutil.copy2(base, merged)

    # Layer 2: board overlay
    if ws.board_dir:
        board_nuttx_dir = os.path.join(ws.board_dir, "nuttx")
        board_overlay = os.path.join(board_nuttx_dir, "ove_board_defconfig")
        if os.path.isfile(board_overlay):
            apply_defconfig_overlay(merged, board_overlay)

        # Layer 2b: feature-conditional board overlays.  Loaded when
        # CONFIG_OVE_<FEATURE> is true in the oveRTOS workspace config.
        if os.path.isdir(board_nuttx_dir):
            for fname in sorted(os.listdir(board_nuttx_dir)):
                if not fname.startswith("ove_board_defconfig."):
                    continue
                feature = fname[len("ove_board_defconfig."):]
                if not feature:
                    continue
                kconfig_key = f"CONFIG_OVE_{feature.upper()}"
                if get_bool(ws.config, kconfig_key):
                    apply_defconfig_overlay(merged,
                        os.path.join(board_nuttx_dir, fname))

    # Layer 3: app overlay (board-specific takes priority over generic)
    if ws.app_dir:
        board_specific = os.path.join(ws.app_dir, "nuttx",
                                      ws.board_name + "_defconfig")
        generic = os.path.join(ws.app_dir, "nuttx_defconfig")
        app_overlay = board_specific if os.path.isfile(board_specific) \
            else generic
        if os.path.isfile(app_overlay):
            apply_defconfig_overlay(merged, app_overlay)

    return merged


def _merge_zephyr_layers(ws):
    """Merge Zephyr prj.conf layers 1+3 into gen_dir/merged_prj.conf.

    Layer 2 (board overlay) is skipped — Zephyr auto-detects
    boards/<boardname>.conf via its CMake build system.
    """
    base = os.path.join(ws.gen_dir, "prj.conf")
    merged = os.path.join(ws.gen_dir, "merged_prj.conf")

    if not os.path.isfile(base):
        print("error: generated/prj.conf not found. "
              "Run 'ove configure' first.")
        sys.exit(1)

    shutil.copy2(base, merged)

    # Layer 3: app overlay (concatenate — later values override)
    if ws.app_dir:
        app_overlay = os.path.join(ws.app_dir, "prj.conf")
        if os.path.isfile(app_overlay):
            with open(merged, "a") as out, open(app_overlay) as inp:
                out.write("\n# --- App overlay ---\n")
                out.write(inp.read())

    return merged


def ensure_rtos_config_applied(ws, rtos, nuttx_build=None, apps_build=None,
                               board_dir=None, env=None, log_file=None):
    """Build guard: unconditionally re-merge all 4 layers and apply.

    Called at the top of build_zephyr() to guarantee the RTOS build tree
    has a fresh, correct config.

    For NuttX CMake builds, defconfig overlays are applied by
    build_nuttx() directly via _apply_nuttx_defconfig_overlays() before
    cmake configure.  This function only records the drift sentinel.

    For Zephyr, returns the absolute path to the merged prj.conf so the
    caller can pass it via -DCONF_FILE to west build.
    """
    if rtos == "nuttx":
        _apply_nuttx_guard(ws, nuttx_build, apps_build, env, log_file)
        return None
    elif rtos == "zephyr":
        return _apply_zephyr_guard(ws)


def _apply_nuttx_guard(ws, nuttx_build, apps_build, env, log_file):
    """Apply all 4 config layers for the NuttX CMake build.

    Precondition: _setup_nuttx_build_tree() must have been called first.

    With CMake, defconfig overlays are merged into the board's defconfig
    in the source tree.  NuttX's CMake build reads the defconfig and runs
    olddefconfig during cmake configure.

    This function is kept as a no-op for the CMake flow since
    build_nuttx() applies overlays via _apply_nuttx_defconfig_overlays()
    before cmake configure.  It only records the hash sentinel for
    post-build drift detection.
    """
    cmake_build = os.path.join(ws.build_dir, "nuttx-cmake")
    nuttx_config = os.path.join(cmake_build, ".config")

    if os.path.isfile(nuttx_config):
        sentinel = os.path.join(ws.workspace_dir,
                                ".rtos_config_applied_sha256")
        with open(sentinel, "w") as f:
            f.write(hash_file(nuttx_config) + "\n")


def _apply_zephyr_guard(ws):
    """Merge all layers into gen_dir/merged_prj.conf (never touches board dir).

    Returns the absolute path to the merged prj.conf so the caller can
    pass it to west build via -DCONF_FILE.
    """
    merged = merge_rtos_config_layers(ws, "zephyr")
    if not merged:
        return None

    # Layer 4: user customizations (append to merged)
    if os.path.isfile(ws.rtos_config_path):
        with open(merged, "a") as out, open(ws.rtos_config_path) as inp:
            out.write("\n# --- User customizations ---\n")
            out.write(inp.read())

    # Store hash for drift detection
    sentinel = os.path.join(ws.workspace_dir, ".rtos_config_applied_sha256")
    with open(sentinel, "w") as f:
        f.write(hash_file(merged) + "\n")

    return os.path.abspath(merged)


def ensure_rtos_build_tree(ws, rtos, env):
    """Ensure RTOS build tree exists with a valid .config.

    For NuttX: delegates to build.py._setup_nuttx_build_tree().
    For Zephyr: verifies west workspace exists.
    """
    if rtos == "nuttx":
        nuttx_src = os.path.join(ws.ws_dl_dir, "nuttx")
        if not os.path.isdir(nuttx_src):
            print("error: NuttX sources not found. "
                  "Run 'ove download' first.")
            sys.exit(1)
        from .build import _setup_nuttx_build_tree
        return _setup_nuttx_build_tree(ws, env)

    elif rtos == "zephyr":
        zephyr_base = os.path.join(ws.ws_dl_dir, "zephyr-workspace", "zephyr")
        if not os.path.isdir(zephyr_base):
            print("error: Zephyr sources not found. "
                  "Run 'ove download' first.")
            sys.exit(1)
        return None


def _run_nuttx_menuconfig(ws, env):
    """NuttX native menuconfig flow (CMake-based)."""
    import shutil as _shutil

    nuttx_src, apps_build = ensure_rtos_build_tree(ws, "nuttx", env)
    cmake_build = os.path.join(ws.build_dir, "nuttx-cmake")
    os.makedirs(cmake_build, exist_ok=True)

    # Resolve NuttX board config
    from .build import _find_nuttx_defconfig, _find_cmake
    nuttx_board_cfg = NUTTX_BOARD_CONFIGS.get(ws.board_name)
    if not nuttx_board_cfg:
        from .build import _fallback_rtos_mapping
        nuttx_board_cfg = _fallback_rtos_mapping(ws, "nuttx") or "mps2-an500:nsh"

    # Layers 1-3: merge into board defconfig
    merged = merge_rtos_config_layers(ws, "nuttx")
    defconfig = _find_nuttx_defconfig(nuttx_src, nuttx_board_cfg)
    if merged and defconfig:
        apply_defconfig_overlay(defconfig, merged)

    # Export env for CMake
    env["OVE_DIR"] = ws.ove_dir
    env["OVE_GEN_DIR"] = ws.gen_dir
    env["OVE_APP_DIR"] = ws.app_dir
    env["OVE_DL_DIR"] = ws.ws_dl_dir

    # Ensure cmake is configured (creates .config from merged defconfig)
    cmake = _find_cmake()
    apps_abs = os.path.abspath(apps_build)
    # Remove cached .config to force re-init from modified defconfig
    config_path = os.path.join(cmake_build, ".config")
    if os.path.isfile(config_path):
        os.unlink(config_path)
    run([cmake,
         f"-S{os.path.abspath(nuttx_src)}",
         f"-B{os.path.abspath(cmake_build)}",
         f"-DBOARD_CONFIG={nuttx_board_cfg}",
         f"-DNUTTX_APPS_DIR={apps_abs}"],
        env=env, cwd=nuttx_src)

    nuttx_config = os.path.join(cmake_build, ".config")

    # Snapshot reference (layers 1-3 only, no user customizations)
    ref_tmp = os.path.join(ws.gen_dir, ".nuttx_reference_config")
    _shutil.copy2(nuttx_config, ref_tmp)

    # Layer 4: overlay existing rtos.config so menuconfig shows current state
    if os.path.isfile(ws.rtos_config_path):
        apply_defconfig_overlay(nuttx_config, ws.rtos_config_path)

    # Snapshot pre-menuconfig for no-save detection
    with open(nuttx_config) as f:
        pre_content = f.read()

    # Launch native menuconfig via cmake build target
    print("=== Launching NuttX native menuconfig ===")
    print("  Save and exit to apply changes. Exit without saving to cancel.")
    result = run([cmake, "--build", os.path.abspath(cmake_build),
                  "-t", "menuconfig"],
                 env=env, check=False)

    if result.returncode != 0:
        print(f"error: NuttX menuconfig exited with code {result.returncode}")
        sys.exit(result.returncode)

    # Check if user saved
    with open(nuttx_config) as f:
        post_content = f.read()

    if pre_content == post_content:
        print("No changes made.")
        return

    # Compute delta: reference (layers 1-3) vs post-menuconfig
    delta = diff_configs(ref_tmp, nuttx_config)
    if delta:
        with open(ws.rtos_config_path, "w") as f:
            f.write(delta)
        n = sum(1 for line in delta.splitlines()
                if line.startswith("CONFIG_") or line.startswith("# CONFIG_"))
        print(f"{n} options changed, saved to {ws.rtos_config_path}")
    else:
        if os.path.isfile(ws.rtos_config_path):
            os.unlink(ws.rtos_config_path)
        print("All changes reverted to base config. rtos.config removed.")


def _run_zephyr_menuconfig(ws, env):
    """Zephyr native menuconfig flow."""
    ensure_rtos_build_tree(ws, "zephyr", env)

    board_dir = os.path.join(ws.board_dir, "zephyr")
    fw_build = os.path.join(ws.build_dir, "firmware")
    os.makedirs(fw_build, exist_ok=True)

    west = os.path.join(ws.venv_dir, "bin", "west")
    zephyr_ws = os.path.join(ws.ws_dl_dir, "zephyr-workspace", "zephyr")
    if os.path.isdir(zephyr_ws):
        env["ZEPHYR_BASE"] = zephyr_ws

    zephyr_board = ZEPHYR_BOARD_MAPPINGS.get(ws.board_name)
    if not zephyr_board:
        print(f"error: no Zephyr board mapping for '{ws.board_name}'")
        sys.exit(1)

    # Layers 1-3: merge into gen_dir/merged_prj.conf (NO user layer yet)
    merged = os.path.abspath(merge_rtos_config_layers(ws, "zephyr"))

    # cmake-only build with base config -> generates reference .config
    print("  Preparing Zephyr build tree (cmake-only)...")
    run([west, "build", "--cmake-only",
         "-b", zephyr_board, "-d", fw_build, board_dir,
         "--", f"-DOVE_DIR={ws.ove_dir}",
         f"-DOVE_APP_DIR={ws.app_dir}",
         f"-DOVE_GEN_DIR={ws.gen_dir}",
         f"-DEXTRA_CONF_FILE={merged}"],
        env=env)

    # Snapshot reference (base only)
    zephyr_config = os.path.join(fw_build, "zephyr", ".config")
    ref_tmp = os.path.join(ws.gen_dir, ".zephyr_reference_config")
    shutil.copy2(zephyr_config, ref_tmp)

    # Layer 4: if rtos.config exists, append to merged and re-run cmake-only
    if os.path.isfile(ws.rtos_config_path):
        with open(merged, "a") as out, open(ws.rtos_config_path) as inp:
            out.write("\n# --- User customizations ---\n")
            out.write(inp.read())
        run([west, "build", "--cmake-only",
             "-b", zephyr_board, "-d", fw_build, board_dir,
             "--", f"-DOVE_DIR={ws.ove_dir}",
             f"-DOVE_APP_DIR={ws.app_dir}",
             f"-DOVE_GEN_DIR={ws.gen_dir}",
             f"-DEXTRA_CONF_FILE={merged}"],
            env=env)

    # Snapshot pre-menuconfig for no-save detection
    with open(zephyr_config) as f:
        pre_content = f.read()

    # Launch native menuconfig
    print("=== Launching Zephyr native menuconfig ===")
    print("  Save and exit to apply changes. Exit without saving to cancel.")
    result = run([west, "build", "-t", "menuconfig", "-d", fw_build],
                 env=env, check=False)

    if result.returncode != 0:
        print(f"error: Zephyr menuconfig exited with code {result.returncode}")
        sys.exit(result.returncode)

    # Check if user saved
    with open(zephyr_config) as f:
        post_content = f.read()

    if pre_content == post_content:
        print("No changes made.")
        return

    # Compute delta: reference (layers 1-3) vs post-menuconfig
    delta = diff_configs(ref_tmp, zephyr_config)
    if delta:
        with open(ws.rtos_config_path, "w") as f:
            f.write(delta)
        n = sum(1 for line in delta.splitlines()
                if line.startswith("CONFIG_") or line.startswith("# CONFIG_"))
        print(f"{n} options changed, saved to {ws.rtos_config_path}")
    else:
        if os.path.isfile(ws.rtos_config_path):
            os.unlink(ws.rtos_config_path)
        print("All changes reverted to base config. rtos.config removed.")


def cmd_rtos_menuconfig(args):
    """CLI entry point for 'ove rtos-menuconfig'."""
    # Check explicit RTOS argument before requiring config
    explicit_rtos = getattr(args, "rtos", None)

    if explicit_rtos in ("freertos", "posix"):
        print(f"Note: {explicit_rtos} does not have native menuconfig.")
        print("Use 'make menuconfig' to configure via oveRTOS.")
        sys.exit(0)

    if explicit_rtos and explicit_rtos not in ("nuttx", "zephyr"):
        print(f"error: unknown RTOS '{explicit_rtos}'. Supported: nuttx, zephyr")
        sys.exit(1)

    ws = Workspace()
    ws.require_config()
    ws.ensure_dirs()

    rtos = explicit_rtos or ws.rtos
    if not rtos:
        print("error: no RTOS selected in .config")
        sys.exit(1)

    if rtos in ("freertos", "posix"):
        print(f"Note: {rtos} does not have native menuconfig.")
        print("Use 'make menuconfig' to configure via oveRTOS.")
        sys.exit(0)

    if rtos not in ("nuttx", "zephyr"):
        print(f"error: unknown RTOS '{rtos}'. Supported: nuttx, zephyr")
        sys.exit(1)

    # Regenerate templates (fresh by construction)
    print("=== Regenerating config templates ===")
    configure_all(ws)

    env = ws.toolchain_env()

    if rtos == "nuttx":
        _run_nuttx_menuconfig(ws, env)
    elif rtos == "zephyr":
        _run_zephyr_menuconfig(ws, env)
