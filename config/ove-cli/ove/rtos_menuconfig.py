# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""RTOS native menuconfig — Buildroot-style nested config with build guard."""

import hashlib
import os
import shutil
import sys

from .configure import configure_all
from .constants import NUTTX_BOARD_CONFIGS, ZEPHYR_BOARD_MAPPINGS
from .utils import apply_defconfig_overlay, diff_configs, run
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
    """Merge NuttX defconfig layers 1-3 into gen_dir/merged_nuttx_defconfig."""
    base = os.path.join(ws.gen_dir, "nuttx_defconfig")
    merged = os.path.join(ws.gen_dir, "merged_nuttx_defconfig")

    if not os.path.isfile(base):
        print("error: generated/nuttx_defconfig not found. "
              "Run 'ove configure' first.")
        sys.exit(1)

    shutil.copy2(base, merged)

    # Layer 2: board overlay
    if ws.board_dir:
        board_overlay = os.path.join(ws.board_dir, "nuttx",
                                     "ove_board_defconfig")
        if os.path.isfile(board_overlay):
            apply_defconfig_overlay(merged, board_overlay)

    # Layer 3: app overlay
    if ws.app_dir:
        app_overlay = os.path.join(ws.app_dir, "nuttx_defconfig")
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


def _hash_file(path):
    """SHA256 hash of a file's contents."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def ensure_rtos_config_applied(ws, rtos, nuttx_build=None, apps_build=None,
                               board_dir=None, env=None, log_file=None):
    """Build guard: unconditionally re-merge all 4 layers and apply.

    Called at the top of build_nuttx() and build_zephyr() to guarantee
    the RTOS build tree has a fresh, correct config.

    For Zephyr, returns the absolute path to the merged prj.conf so the
    caller can pass it via -DCONF_FILE to west build.
    """
    if rtos == "nuttx":
        _apply_nuttx_guard(ws, nuttx_build, apps_build, env, log_file)
        return None
    elif rtos == "zephyr":
        return _apply_zephyr_guard(ws)


def _apply_nuttx_guard(ws, nuttx_build, apps_build, env, log_file):
    """Apply all 4 layers to NuttX build tree .config.

    Precondition: _setup_nuttx_build_tree() must have been called first.
    Uses single-pass olddefconfig after all overlays.
    """
    nuttx_config = os.path.join(nuttx_build, ".config")
    apps_abs = os.path.abspath(apps_build)

    if not os.path.isfile(nuttx_config):
        print("error: NuttX .config not found in build tree. "
              "Build tree may not be set up correctly.")
        sys.exit(1)

    # Layers 1-3
    merged = merge_rtos_config_layers(ws, "nuttx")
    if merged:
        apply_defconfig_overlay(nuttx_config, merged)

    # Layer 4: user customizations
    if os.path.isfile(ws.rtos_config_path):
        apply_defconfig_overlay(nuttx_config, ws.rtos_config_path)

    # Expand with olddefconfig
    run(["make", "olddefconfig", f"APPDIR={apps_abs}"],
        env=env, cwd=nuttx_build, log_file=log_file)

    # Store hash for post-build drift detection
    sentinel = os.path.join(ws.workspace_dir, ".rtos_config_applied_sha256")
    with open(sentinel, "w") as f:
        f.write(_hash_file(nuttx_config) + "\n")


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
        f.write(_hash_file(merged) + "\n")

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
    """NuttX native menuconfig flow."""
    nuttx_build, apps_build = ensure_rtos_build_tree(ws, "nuttx", env)
    nuttx_config = os.path.join(nuttx_build, ".config")
    apps_abs = os.path.abspath(apps_build)

    # Layers 1-3: merge and apply as reference baseline
    merged = merge_rtos_config_layers(ws, "nuttx")
    if merged and os.path.isfile(nuttx_config):
        apply_defconfig_overlay(nuttx_config, merged)
    run(["make", "olddefconfig", f"APPDIR={apps_abs}"],
        env=env, cwd=nuttx_build)

    # Snapshot reference (layers 1-3 only, no user customizations)
    ref_tmp = os.path.join(ws.gen_dir, ".nuttx_reference_config")
    shutil.copy2(nuttx_config, ref_tmp)

    # Layer 4: overlay existing rtos.config so menuconfig shows current state
    if os.path.isfile(ws.rtos_config_path):
        apply_defconfig_overlay(nuttx_config, ws.rtos_config_path)
        run(["make", "olddefconfig", f"APPDIR={apps_abs}"],
            env=env, cwd=nuttx_build)

    # Snapshot pre-menuconfig for no-save detection
    with open(nuttx_config) as f:
        pre_content = f.read()

    # Launch native menuconfig (interactive)
    print("=== Launching NuttX native menuconfig ===")
    print("  Save and exit to apply changes. Exit without saving to cancel.")
    result = run(["make", "menuconfig", f"APPDIR={apps_abs}"],
                 env=env, cwd=nuttx_build, check=False)

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
