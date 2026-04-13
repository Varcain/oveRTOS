# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Build orchestration — dispatches to cmake per RTOS."""

import logging
import os
import shlex
import shutil
import sys

from .utils import run, nproc, apply_defconfig_overlay
from .constants import NUTTX_BOARD_CONFIGS, ZEPHYR_BOARD_MAPPINGS
from .workspace import Workspace, get_bool, get_str

logger = logging.getLogger("ove")

_build_log = None


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


def _apply_patches(source_dir, patches_dir, stamp_path, label="patches",
                   log_file=None):
    """Apply .patch files from patches_dir to source_dir (idempotent).

    Uses stamp_path to track which patches have been applied.
    Returns the list of newly applied patch names.
    """
    if not os.path.isdir(patches_dir):
        return []
    patches = sorted(f for f in os.listdir(patches_dir)
                     if f.endswith(".patch"))
    if not patches:
        return []

    # Read already-applied patches from stamp
    already_applied = set()
    if os.path.isfile(stamp_path):
        with open(stamp_path) as f:
            already_applied = set(f.read().splitlines())

    to_apply = [p for p in patches if p not in already_applied]
    if not to_apply:
        return []

    for p in to_apply:
        patch_path = os.path.join(patches_dir, p)
        logger.debug(f"Applying {label}: {p}")
        run(["git", "apply", patch_path], cwd=source_dir,
            log_file=log_file)

    # Append to stamp
    with open(stamp_path, "a") as f:
        for p in to_apply:
            f.write(p + "\n")
    return to_apply


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


def build_freertos(ws):
    """Build FreeRTOS firmware via CMake."""
    cmake = _find_cmake()
    env = ws.toolchain_env()
    board_dir = os.path.join(ws.board_dir, "freertos")
    fw_build = os.path.join(ws.build_dir, "firmware")
    os.makedirs(fw_build, exist_ok=True)

    # Apply board patches, then app patches to FreeRTOS source tree
    freertos_src = os.path.join(ws.ws_dl_dir, "FreeRTOS-Kernel")
    if os.path.isdir(freertos_src):
        patches_stamp = os.path.join(freertos_src,
                                     ".ove_patches_applied")
        _apply_patches(freertos_src,
                       os.path.join(board_dir, "patches"),
                       patches_stamp, label="board patch",
                       log_file=_build_log)
        if ws.app_dir:
            _apply_patches(freertos_src,
                           os.path.join(ws.app_dir, "patches", "freertos"),
                           patches_stamp, label="app patch",
                           log_file=_build_log)

    toolchain_file = os.path.join(board_dir, "cmake", "arm-none-eabi.cmake")

    cmake_args = [
        cmake,
        "-DOVE_DIR=" + ws.ove_dir,
        "-DOVE_APP_DIR=" + ws.app_dir,
        "-DOVE_GEN_DIR=" + ws.gen_dir,
        "-DOVE_DL_DIR=" + ws.ws_dl_dir,
        "-DBOARD_DIR=" + board_dir,
    ]

    tc = ws.toolchain_dir
    if tc:
        cmake_args.append(f"-DOVE_TOOLCHAIN_DIR={tc}")

    if os.path.isfile(toolchain_file):
        cmake_args.append(f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}")

    cmake_args.append(board_dir)

    logger.info("Building FreeRTOS firmware")
    run(cmake_args, env=env, cwd=fw_build, log_file=_build_log)
    run([cmake, "--build", fw_build, f"-j{nproc()}"], env=env,
        log_file=_build_log)

    _copy_images(ws, fw_build)
    _create_run_or_flash_script(ws)


def build_zephyr(ws):
    """Build Zephyr firmware via west."""
    env = ws.toolchain_env()
    west = os.path.join(ws.venv_dir, "bin", "west")
    board_dir = os.path.join(ws.board_dir, "zephyr")
    fw_build = os.path.join(ws.build_dir, "firmware")
    os.makedirs(fw_build, exist_ok=True)

    zephyr_ws = os.path.join(ws.ws_dl_dir, "zephyr-workspace", "zephyr")
    if os.path.isdir(zephyr_ws):
        env["ZEPHYR_BASE"] = zephyr_ws

    # Apply board patches, then app patches to Zephyr source tree
    if os.path.isdir(zephyr_ws):
        patches_stamp = os.path.join(zephyr_ws, ".ove_patches_applied")
        _apply_patches(zephyr_ws,
                       os.path.join(ws.board_dir, "zephyr", "patches"),
                       patches_stamp, label="board patch",
                       log_file=_build_log)
        if ws.app_dir:
            _apply_patches(zephyr_ws,
                           os.path.join(ws.app_dir, "patches", "zephyr"),
                           patches_stamp, label="app patch",
                           log_file=_build_log)

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
    run(west_args, env=env, log_file=_build_log)

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


def _setup_nuttx_build_tree(ws, env, log_file=None):
    """Copy NuttX sources, apply patches, set up ext app, and run configure.sh.

    Returns (nuttx_build, apps_build) paths.
    """
    nuttx_build = os.path.join(ws.build_dir, "nuttx")
    apps_build = os.path.join(ws.build_dir, "nuttx-apps")

    # Copy sources to build tree (keeps dl/ pristine)
    if not os.path.isdir(nuttx_build):
        logger.debug("Copying NuttX sources to build tree...")
        shutil.copytree(os.path.join(ws.ws_dl_dir, "nuttx"), nuttx_build,
                        symlinks=True, dirs_exist_ok=True)
    if not os.path.isdir(apps_build):
        logger.debug("Copying NuttX apps to build tree...")
        shutil.copytree(os.path.join(ws.ws_dl_dir, "nuttx-apps"), apps_build,
                        symlinks=True, dirs_exist_ok=True)

    # Apply board patches, then app patches (app patches last)
    patches_stamp = os.path.join(nuttx_build, ".ove_patches_applied")
    _apply_patches(nuttx_build,
                   os.path.join(ws.board_dir, "nuttx", "patches"),
                   patches_stamp, label="board patch", log_file=log_file)
    if ws.app_dir:
        _apply_patches(nuttx_build,
                       os.path.join(ws.app_dir, "patches", "nuttx"),
                       patches_stamp, label="app patch", log_file=log_file)

    # Set up external app
    ext_dir = os.path.join(apps_build, "external")
    os.makedirs(ext_dir, exist_ok=True)
    app_dest = os.path.join(ext_dir, "ove_app")
    if os.path.exists(app_dest):
        shutil.rmtree(app_dest)
    shutil.copytree(os.path.join(ws.board_dir, "nuttx"), app_dest)

    with open(os.path.join(ext_dir, "Kconfig"), "w") as f:
        f.write(f'source "{os.path.abspath(app_dest)}/Kconfig"\n')
    with open(os.path.join(ext_dir, "Make.defs"), "w") as f:
        f.write('ifneq ($(CONFIG_EXTERNAL_OVE_APP),)\n')
        f.write('CONFIGURED_APPS += $(APPDIR)/external/ove_app\n')
        f.write('endif\n')

    # Write .ove_env so the app Makefile can resolve paths even when
    # NuttX's recursive make doesn't propagate command-line variables.
    with open(os.path.join(app_dest, ".ove_env"), "w") as f:
        f.write(f"OVE_DIR := {ws.ove_dir}\n")
        f.write(f"OVE_GEN_DIR := {ws.gen_dir}\n")
        f.write(f"OVE_APP_DIR := {ws.app_dir}\n")
        for mod in ("AUDIO", "FS", "SHELL", "NVS", "WATCHDOG"):
            key = f"CONFIG_OVE_{mod}"
            if get_bool(ws.config, key):
                f.write(f"{key} := y\n")

    # NuttX board config mapping
    nuttx_board_cfg = NUTTX_BOARD_CONFIGS.get(ws.board_name)
    if not nuttx_board_cfg:
        nuttx_board_cfg = _fallback_rtos_mapping(ws, "nuttx") or "mps2-an500:nsh"

    # Initial configure
    configured_flag = os.path.join(nuttx_build, ".ove_configured")
    nuttx_config = os.path.join(nuttx_build, ".config")
    base_snapshot = os.path.join(nuttx_build, ".config.ove_base")
    if not os.path.isfile(configured_flag):
        logger.debug(f"Configuring NuttX for {nuttx_board_cfg}...")
        run(["./tools/configure.sh", "-a", "../nuttx-apps", nuttx_board_cfg],
             env=env, cwd=nuttx_build, log_file=log_file)

        # Snapshot the pristine board defconfig BEFORE any oveRTOS
        # overlays are applied. _apply_nuttx_guard restores from this
        # snapshot on every build so that overlay-key removals (which
        # apply_defconfig_overlay can't detect by itself) actually take
        # effect — without it, a key that used to be in the overlay but
        # has since been removed would persist in .config indefinitely
        # and break NuttX's olddefconfig dependency check.
        if os.path.isfile(nuttx_config):
            shutil.copy2(nuttx_config, base_snapshot)

        # Apply oveRTOS overlay immediately so architecture-level settings
        # (FPU, stack sizes, etc.) take effect before any compilation.
        overlay = os.path.join(ws.gen_dir, "nuttx_defconfig")
        if os.path.isfile(overlay):
            apply_defconfig_overlay(nuttx_config, overlay)
            apps_abs = os.path.abspath(apps_build)
            run(["make", "olddefconfig", f"APPDIR={apps_abs}"],
                env=env, cwd=nuttx_build, log_file=log_file)

        with open(configured_flag, "w") as f:
            f.write("configured\n")

    return nuttx_build, apps_build


def build_nuttx(ws):
    """Build NuttX firmware via Make (pre-CMake migration)."""
    env = ws.toolchain_env()

    logger.info("Building NuttX firmware")

    nuttx_build, apps_build = _setup_nuttx_build_tree(ws, env,
                                                       log_file=_build_log)

    # Apply layered config (build guard — fresh by construction)
    from .rtos_menuconfig import ensure_rtos_config_applied
    ensure_rtos_config_applied(ws, "nuttx", nuttx_build=nuttx_build,
                               apps_build=apps_build, env=env,
                               log_file=_build_log)

    apps_abs = os.path.abspath(apps_build)

    # LVGL paths — use external LVGL from workspace dl/ symlink
    lvgl_inc = os.path.join(ws.ws_dl_dir, "lvgl")
    lvgl_parent = ws.ws_dl_dir

    # Export all build paths as environment variables so they reach NuttX's
    # recursive sub-makes and Rust's cargo build. NuttX only passes APPDIR
    # on the make command line to app sub-makes, so command-line vars don't
    # propagate through $(MAKE) -C <app_dir>.
    env["OVE_DIR"] = ws.ove_dir
    env["OVE_GEN_DIR"] = ws.gen_dir
    env["OVE_APP_DIR"] = ws.app_dir
    env["LVGL_INCLUDE_PATH"] = lvgl_inc
    env["LVGL_PARENT_PATH"] = lvgl_parent

    # LV_CONF_PATH for Rust bindgen — board's lv_conf.h directory
    lv_conf_dir = os.path.join(ws.board_dir, "nuttx")
    if os.path.isfile(os.path.join(lv_conf_dir, "lv_conf.h")):
        env["LV_CONF_PATH"] = lv_conf_dir

    make_vars = [
        f"APPDIR={apps_abs}",
        f"OVE_DIR={ws.ove_dir}",
        f"OVE_GEN_DIR={ws.gen_dir}",
        f"LVGL_INCLUDE_PATH={lvgl_inc}",
        f"LVGL_PARENT_PATH={lvgl_parent}",
    ]

    # Two-pass context build: first pass generates NuttX headers and
    # .config; second pass retries Rust/Zig bindgen with final FPU flags.
    # (LVGL headers are already available from the external dl/lvgl tree.)
    run(["make", "-k", "context"] + make_vars,
         env=env, cwd=nuttx_build, check=False, log_file=_build_log)

    # Clean stale build artifacts between context passes so the second
    # pass recompiles everything with the final .config (FPU flags, etc.):
    # - Rust target dir: retry bindgen with correct compiler flags
    # - Apps context stamp + staging: recompile with correct compiler flags
    rust_target_dir = os.path.join(apps_build, "external", "ove_app",
                                   "rust_target")
    if os.path.isdir(rust_target_dir):
        shutil.rmtree(rust_target_dir)
    # Remove stale .gcno coverage files so the second pass doesn't mismatch
    for root, _dirs, files in os.walk(apps_build):
        for fname in files:
            if fname.endswith(".gcno"):
                os.unlink(os.path.join(root, fname))
    for stamp in [os.path.join(apps_build, ".context"),
                  os.path.join(apps_build, ".built"),
                  os.path.join(nuttx_build, ".context")]:
        if os.path.isfile(stamp):
            os.unlink(stamp)

    run(["make", "context"] + make_vars, env=env, cwd=nuttx_build,
        log_file=_build_log)

    # Remove stale Make.dep
    dep_file = os.path.join(apps_build, "external", "ove_app", "Make.dep")
    if os.path.isfile(dep_file):
        os.unlink(dep_file)

    # Generate LVGL source list for NuttX (external — no longer bundled
    # in nuttx-apps). Glob all .c files under dl/lvgl/src/ and write a
    # makefile fragment that ove_nuttx_common.mk includes.
    if ws.config.get("CONFIG_OVE_LVGL") in (True, "y"):
        import glob as _glob
        lvgl_dir = os.path.join(ws.ws_dl_dir, "lvgl")
        if os.path.isdir(lvgl_dir):
            lvgl_srcs = sorted(_glob.glob(
                os.path.join(lvgl_dir, "src", "**", "*.c"), recursive=True))
            # Exclude Helium/NEON SIMD blends — Cortex-M only has Thumb
            lvgl_srcs = [s for s in lvgl_srcs
                         if "/blend/helium/" not in s
                         and "/blend/neon/" not in s]
            lvgl_mk = os.path.join(ws.gen_dir, "ove_lvgl_sources.mk")
            lv_conf_dir = os.path.join(ws.board_dir, "nuttx")
            with open(lvgl_mk, "w") as f:
                f.write("# Auto-generated LVGL sources for NuttX\n")
                f.write(f"CFLAGS += -I{lvgl_dir}\n")
                f.write(f"CFLAGS += -I{ws.ws_dl_dir}\n")
                f.write(f"CFLAGS += -I{lv_conf_dir}\n")
                f.write("CFLAGS += -DLV_CONF_INCLUDE_SIMPLE\n")
                f.write("CSRCS +=")
                for s in lvgl_srcs:
                    f.write(f" \\\n    {s}")
                f.write("\n")

    # Generate TFLM source list for NuttX (compiled as CXXSRCS within
    # NuttX's own build system, which handles C++ include paths correctly).
    if ws.config.get("CONFIG_OVE_INFER") in (True, "y"):
        tflm_dir = os.path.join(ws.ws_dl_dir, "tflite-micro")
        if os.path.isdir(tflm_dir):
            import glob
            tflm_srcs = []
            for pattern in [
                "tensorflow/lite/micro/*.cc",
                "tensorflow/lite/micro/arena_allocator/*.cc",
                "tensorflow/lite/micro/memory_planner/*.cc",
                "tensorflow/lite/micro/tflite_bridge/*.cc",
                "tensorflow/lite/micro/kernels/*.cc",
                "tensorflow/lite/core/c/*.cc",
                "tensorflow/lite/core/api/*.cc",
                "tensorflow/lite/kernels/*.cc",
                "tensorflow/lite/kernels/internal/*.cc",
                "tensorflow/compiler/mlir/lite/core/api/*.cc",
                "tensorflow/compiler/mlir/lite/schema/*.cc",
                "tensorflow/lite/micro/tflite_bridge/*.cc",
                "signal/micro/kernels/*.cc",
                "signal/src/*.cc",
                "signal/src/kiss_fft_wrappers/*.cc",
            ]:
                tflm_srcs.extend(glob.glob(os.path.join(tflm_dir, pattern)))
            # Exclude test files and files with GCC 15 template issues
            tflm_srcs = [s for s in tflm_srcs
                         if not s.endswith("_test.cc")
                         and "test_helpers" not in s
                         and "micro_time.cc" not in s]
            # Add oveRTOS inference wrapper
            tflm_srcs.append(os.path.join(ws.ove_dir, "backends", "common",
                                          "ove_infer.cc"))
            tflm_srcs.append(os.path.join(ws.ove_dir, "backends", "common",
                                          "tflm", "ove_tflm_debug_log.cc"))
            tflm_srcs.append(os.path.join(ws.ove_dir, "backends", "common",
                                          "tflm", "ove_tflm_time.cc"))
            # Clean any stale TFLM .o files left over from previous builds.
            # NuttX Application.mk places .o files next to absolute-path
            # sources (dl/ for C apps, generated/tflm_links/ for C++ apps),
            # and these survive workspace reconfiguration — masking any
            # compile-flag changes because make sees them as up-to-date.
            _tflm_stale_dirs = [
                tflm_dir,
                os.path.join(ws.gen_dir, "tflm_links"),
            ]
            for _stale_dir in _tflm_stale_dirs:
                if not os.path.isdir(_stale_dir):
                    continue
                for _root, _, _files in os.walk(_stale_dir):
                    for _f in _files:
                        if _f.endswith(".o"):
                            try:
                                os.unlink(os.path.join(_root, _f))
                            except OSError:
                                pass

            # Write TFLM source list as a makefile fragment
            tflm_mk = os.path.join(ws.gen_dir, "ove_tflm_sources.mk")
            tflm_downloads = os.path.join(tflm_dir,
                "tensorflow/lite/micro/tools/make/downloads")
            with open(tflm_mk, "w") as f:
                f.write("# Auto-generated TFLM sources for NuttX\n")
                # Override CXXEXT to .cc (NuttX defaults to .cxx)
                # unless ove_app_lang.mk already set it to .cpp
                f.write("ifeq ($(APP_LANG),cpp)\n")
                f.write("# C++ app: keep CXXEXT=.cpp from ove_app_lang.mk\n")
                f.write("else\n")
                f.write("CXXEXT = .cc\n")
                f.write("endif\n\n")
                # Create .cpp symlinks for C++ apps (CXXEXT=.cpp)
                link_dir = os.path.join(ws.gen_dir, "tflm_links")
                cc_srcs = []
                cpp_srcs = []
                for s in tflm_srcs:
                    cc_srcs.append(s)
                    if s.endswith(".cc"):
                        rel = os.path.relpath(s, tflm_dir) if s.startswith(tflm_dir) \
                              else os.path.relpath(s, ws.ove_dir)
                        link_path = os.path.join(link_dir,
                                                 rel.replace(".cc", ".cpp"))
                        os.makedirs(os.path.dirname(link_path), exist_ok=True)
                        if os.path.exists(link_path):
                            os.unlink(link_path)
                        os.symlink(os.path.abspath(s), link_path)
                        cpp_srcs.append(link_path)
                    else:
                        cpp_srcs.append(s)
                # Use .cc sources when CXXEXT=.cc, .cpp symlinks otherwise
                f.write("ifeq ($(CXXEXT),.cc)\n")
                f.write("CXXSRCS +=")
                for s in cc_srcs:
                    f.write(f" \\\n    {s}")
                f.write("\n")
                f.write("else\n")
                f.write("CXXSRCS +=")
                for s in cpp_srcs:
                    f.write(f" \\\n    {s}")
                f.write("\n")
                f.write("endif\n\n")
                f.write(f"CXXFLAGS += -I{tflm_dir}\n")
                f.write(f"CXXFLAGS += -I{tflm_downloads}/flatbuffers/include\n")
                f.write(f"CXXFLAGS += -I{tflm_downloads}/gemmlowp\n")
                f.write(f"CXXFLAGS += -I{tflm_downloads}/ruy\n")
                f.write(f"CXXFLAGS += -I{tflm_dir}/signal/micro/kernels\n")
                f.write(f"CXXFLAGS += -I{tflm_dir}/signal/src\n")
                f.write(f"CXXFLAGS += -I{tflm_downloads}/kissfft\n")
                f.write(f"CXXFLAGS += -I{tflm_dir}/signal/src/kiss_fft_wrappers\n")
                # kissfft C sources
                kissfft_dir = os.path.join(tflm_downloads, "kissfft")
                f.write(f"CSRCS += {kissfft_dir}/kiss_fft.c\n")
                f.write(f"CSRCS += {kissfft_dir}/tools/kiss_fftr.c\n")
                f.write(f"CFLAGS += -I{kissfft_dir}\n")
                f.write("CXXFLAGS += -DTF_LITE_STATIC_MEMORY\n")
                f.write("CXXFLAGS += -DTF_LITE_USE_GLOBAL_MAX\n")
                f.write("CXXFLAGS += -DTF_LITE_USE_GLOBAL_MIN\n")
                f.write("CXXFLAGS += -fno-threadsafe-statics\n")
                f.write("CXXFLAGS += -fno-exceptions -fno-rtti\n")
                # GCC 15 auto-enables _GLIBCXX_ASSERTIONS for unoptimized
                # builds, which makes std::array::operator[] (used in
                # batch_matmul_common.cc) emit calls to std::__glibcxx_
                # assert_fail — a symbol only defined in libstdc++.a, which
                # NuttX does not link.  Disable the assertions so the link
                # succeeds.
                f.write("CXXFLAGS += -D_GLIBCXX_NO_ASSERTIONS\n")
                # Note: don't set -std= here. C apps get NuttX default,
                # C++ apps get -std=c++20 from ove_app_lang.mk.
                f.write("CXXFLAGS += -w -fpermissive\n")
                # GCC 15 rejects the int16 LUTPopulate call in
                # softmax_common.cc (lambda doesn't decay to function
                # pointer in template overload resolution).  Patch the
                # source to use an explicit function pointer cast.
                softmax_src = os.path.join(tflm_dir, "tensorflow", "lite",
                    "micro", "kernels", "softmax_common.cc")
                if os.path.isfile(softmax_src):
                    with open(softmax_src, "r") as sf:
                        src = sf.read()
                    old = ("[](float value) { return std::exp(value); }"
                           ", op_data->exp_lut")
                    new = ("static_cast<float(*)(float)>("
                           "[](float value) -> float { return std::exp(value); })"
                           ", op_data->exp_lut")
                    if old in src:
                        with open(softmax_src, "w") as sf:
                            sf.write(src.replace(old, new))
                # NuttX adds -nostdinc++ globally.  Re-add the toolchain's
                # C++ standard library headers via -idirafter (lower priority
                # than NuttX's cxx/ wrappers).
                f.write("_TFLM_CXX_SYSROOT := $(shell $(CXX) -print-sysroot)\n")
                f.write("_TFLM_CXX_VER := $(shell $(CXX) -dumpversion)\n")
                f.write("_TFLM_CXX_MACHINE := $(shell $(CXX) -dumpmachine)\n")
                f.write("CXXFLAGS += -idirafter "
                        "$(_TFLM_CXX_SYSROOT)/include/c++/$(_TFLM_CXX_VER)\n")
                f.write("CXXFLAGS += -idirafter "
                        "$(_TFLM_CXX_SYSROOT)/include/c++/"
                        "$(_TFLM_CXX_VER)/$(_TFLM_CXX_MACHINE)\n")
                # Provide ctype macros that the toolchain's ctype_base.h
                # expects (NuttX's ctype.h defines them, but #include_next
                # in the C++ headers can't find them through -idirafter).
                f.write("CXXFLAGS += -D_U=01 -D_L=02 -D_N=04 "
                        "-D_S=010 -D_P=020 -D_C=040 -D_X=0100 -D_B=0200\n")
                # CMSIS-NN if available
                cmsis_nn = os.path.join(ws.ove_dir, "dl", "CMSIS_5",
                                        "CMSIS", "NN")
                cmsis_core = os.path.join(ws.ove_dir, "dl", "CMSIS_5",
                                          "CMSIS", "Core", "Include")
                # Prefer the standalone CMSIS-DSP repo over the bundled
                # copy in CMSIS_5/CMSIS/DSP. The bundled headers are
                # older and don't define ARM_DSP_ATTRIBUTE, which the
                # newer standalone source files require — mixing the
                # two leaves boards that pull in CMSIS-DSP sources
                # (e.g. stm32f746g-discovery) failing with "expected
                # ';' before 'void'" because the macro is undefined.
                cmsis_dsp_standalone = os.path.join(
                    ws.ove_dir, "dl", "CMSIS-DSP", "Include")
                cmsis_dsp_bundled = os.path.join(
                    ws.ove_dir, "dl", "CMSIS_5", "CMSIS", "DSP",
                    "Include")
                if os.path.isdir(cmsis_dsp_standalone):
                    cmsis_dsp = cmsis_dsp_standalone
                else:
                    cmsis_dsp = cmsis_dsp_bundled
                if os.path.isdir(cmsis_nn):
                    nn_srcs = glob.glob(
                        os.path.join(cmsis_nn, "Source", "**", "*.c"),
                        recursive=True)
                    if nn_srcs:
                        for i, s in enumerate(nn_srcs):
                            if i == 0:
                                f.write(f"CSRCS += {s}")
                            else:
                                f.write(f" \\\n    {s}")
                        f.write("\n\n")
                    f.write(f"CFLAGS += -I{cmsis_nn}/Include\n")
                    f.write(f"CXXFLAGS += -I{cmsis_nn}/Include\n")
                    f.write("CXXFLAGS += -DCMSIS_NN\n")
                if os.path.isdir(cmsis_core):
                    f.write(f"CFLAGS += -I{cmsis_core}\n")
                    f.write(f"CXXFLAGS += -I{cmsis_core}\n")
                if os.path.isdir(cmsis_dsp):
                    f.write(f"CFLAGS += -I{cmsis_dsp}\n")
                    f.write(f"CXXFLAGS += -I{cmsis_dsp}\n")
            logger.info(f"Generated TFLM source list: {tflm_mk}")

    # Build
    make_vars.append(f"OVE_APP_DIR={ws.app_dir}")
    run(["make", f"-j{nproc()}"] + make_vars, env=env, cwd=nuttx_build,
        log_file=_build_log)

    # Copy images
    os.makedirs(ws.images_dir, exist_ok=True)
    for name in ("nuttx", "nuttx.bin"):
        src = os.path.join(nuttx_build, name)
        dst_name = "firmware.elf" if name == "nuttx" else "firmware.bin"
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(ws.images_dir, dst_name))

    _check_rtos_config_drift(ws, os.path.join(nuttx_build, ".config"))
    _create_run_or_flash_script(ws, rtos="nuttx")

    # Clean stray .o files NuttX places next to source files when CSRCS
    # contains absolute paths (NuttX Application.mk quirk). This includes
    # TFLM sources under dl/tflite-micro-*, whose .o files otherwise persist
    # across builds and mask flag changes (make sees them as up-to-date
    # because the source hasn't changed).
    ove_dl_dir = os.path.join(ws.ove_dir, "dl")
    search_dirs = [ws.ove_dir, ws.app_dir]
    for search_dir in search_dirs:
        for root, dirs, files in os.walk(search_dir):
            # Skip output/ and .venv/, but walk dl/ so TFLM .o files
            # placed next to their sources get cleaned.
            if any(skip in root for skip in ["/output/", "/.venv/"]):
                continue
            # Inside dl/, only clean .o files (not any other artifacts).
            for f in files:
                if f.endswith(".o"):
                    os.unlink(os.path.join(root, f))


def _apply_nuttx_defconfig_overlay(ws, nuttx_build, apps_build, env):
    """Apply oveRTOS-generated defconfig overlay to NuttX .config."""
    overlay = os.path.join(ws.gen_dir, "nuttx_defconfig")
    nuttx_config = os.path.join(nuttx_build, ".config")

    if not os.path.isfile(overlay):
        return

    # Apply the overlay using shared utility
    apply_defconfig_overlay(nuttx_config, overlay)

    # Run olddefconfig
    apps_abs = os.path.abspath(apps_build)
    run(["make", "olddefconfig", f"APPDIR={apps_abs}"],
         env=env, cwd=nuttx_build, log_file=_build_log)


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

        # Find emcmake: first try downloaded emsdk, then PATH
        emsdk_dir = os.path.join(ws.ws_dl_dir, "emsdk")
        if not os.path.isdir(emsdk_dir):
            emsdk_dir = os.path.join(ws.dl_dir, "emsdk")
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

        if not emmake or not os.path.isfile(emmake):
            emmake = os.path.join(em_bin, "emmake")

        run([
            emcmake, cmake,
            f"-DOVE_DIR={ws.ove_dir}",
            f"-DOVE_APP_DIR={ws.app_dir}",
            f"-DOVE_GEN_DIR={ws.gen_dir}",
            f"-DOVE_DL_DIR={ws.ws_dl_dir}",
            board_dir,
        ], env=env, cwd=fw_build, log_file=_build_log)
        run([emmake, "make", f"-j{nproc()}"], env=env, cwd=fw_build,
            log_file=_build_log)
    else:
        logger.info("Building POSIX native executable")
        run([
            cmake,
            f"-DOVE_DIR={ws.ove_dir}",
            f"-DOVE_APP_DIR={ws.app_dir}",
            f"-DOVE_GEN_DIR={ws.gen_dir}",
            f"-DOVE_DL_DIR={ws.ws_dl_dir}",
            board_dir,
        ], env=env, cwd=fw_build, log_file=_build_log)
        run([cmake, "--build", fw_build, f"-j{nproc()}"], env=env,
            log_file=_build_log)

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
    for name in ("ove_wasm.js", "ove_wasm.wasm", "ove_wasm.worker.js"):
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
    _write_wasm_run_sh(os.path.join(serve_dir, "run.sh"))

    # Generate workspace-level run script (matches POSIX convention)
    run_script = os.path.join(ws.workspace_dir, "run")
    with open(run_script, "w") as f:
        f.write('#!/bin/bash\n')
        f.write('set -e\n')
        f.write('DIR="$(cd "$(dirname "$0")" && pwd)"\n')
        f.write('exec "$DIR/serve/run.sh" "$@"\n')
    os.chmod(run_script, 0o755)


def _write_wasm_run_sh(path):
    """Generate a self-contained run.sh that serves the WASM build."""
    content = '''#!/usr/bin/env bash
# Auto-generated — serves this WASM build with COOP/COEP headers.
# Usage: ./run.sh [PORT]

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:-8080}"

echo "=== Serving WASM at http://localhost:$PORT ==="
echo "  Files: $DIR"
echo "  Press Ctrl+C to stop."

# Open browser (best-effort, non-blocking)
( sleep 1 && python3 -c "import webbrowser; webbrowser.open('http://localhost:$PORT')" ) 2>/dev/null &

python3 -c "
import http.server, functools, sys

class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()
    def log_message(self, *a):
        pass

s = http.server.HTTPServer(('127.0.0.1', int(sys.argv[1])),
    functools.partial(H, directory=sys.argv[2]))
try:
    s.serve_forever()
except KeyboardInterrupt:
    print('\\\\nStopped.')
" "$PORT" "$DIR"
'''
    with open(path, "w") as f:
        f.write(content)
    os.chmod(path, 0o755)


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
            f.write(f'exec {qemu_script} "$DIR/images/firmware.elf" "$@"\n')
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
            f.write(f'exec {flash_sh} "$DIR/images/firmware.elf"\n')
        os.chmod(flash_script, 0o755)




def _check_rtos_config_drift(ws, config_path):
    """Warn if RTOS .config changed during build (post-build drift detection)."""
    sentinel = os.path.join(ws.workspace_dir, ".rtos_config_applied_sha256")
    if not os.path.isfile(sentinel) or not os.path.isfile(config_path):
        return
    import hashlib
    h = hashlib.sha256()
    with open(config_path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    current_hash = h.hexdigest()
    with open(sentinel) as f:
        expected_hash = f.read().strip()
    if current_hash != expected_hash:
        logger.warning("RTOS config was modified during build "
              "(possible CMake reconfigure or Kconfig dependency resolution).")
        logger.debug("Run 'make nuttx-menuconfig' or 'make zephyr-menuconfig' to inspect.")


def build(ws):
    """Auto-detect RTOS and build."""
    from .manifest import warn_if_dirty

    global _build_log
    ws.require_config()
    ws.ensure_dirs()
    warn_if_dirty(ws.ove_dir)
    rtos = ws.rtos
    if not rtos:
        logger.error("no RTOS selected in .config")
        sys.exit(1)

    _preflight_check(ws)

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

    log_path = os.path.join(ws.workspace_dir, "build.log")
    _build_log = open(log_path, "w")
    try:
        builder(ws)
    finally:
        _build_log.close()
        _build_log = None
    logger.info("Build complete")
    logger.info(f"Firmware: {ws.images_dir}/")


def cmd_build(args):
    """CLI entry point for 'ove build'."""
    ws = Workspace()
    build(ws)
