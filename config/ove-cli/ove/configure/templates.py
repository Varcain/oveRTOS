# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Jinja2-based rendering of the ove / RTOS configuration files.

Produces: ``ove_config.h``, ``ove_config.cmake``, ``FreeRTOSConfig.h``,
``prj.conf``, ``nuttx_defconfig`` and (when inference is enabled) the
generated model C arrays under ``gen_dir/generated_models/``.
"""

import glob
import json
import os
import subprocess
import sys

from ..workspace import get_bool, get_str

try:
    from jinja2 import Environment, FileSystemLoader
except ImportError:
    Environment = None


def _gcc_sysroot_cached(cc, cxx, cache_path):
    """Cache `gcc -print-*` output keyed on compiler path + mtime.

    The four subprocess calls aren't slow individually but they're waste
    on an otherwise-untouched configure run, and cross compilers on
    network filesystems add latency.
    """
    cc_mtime = os.path.getmtime(cc)
    cxx_mtime = os.path.getmtime(cxx)
    key = {"cc": cc, "cxx": cxx, "cc_mtime": cc_mtime, "cxx_mtime": cxx_mtime}
    if os.path.isfile(cache_path):
        try:
            with open(cache_path) as f:
                cached = json.load(f)
            if all(cached.get(k) == v for k, v in key.items()):
                return (cached["builtin"], cached["sysroot"],
                        cached["ver"], cached["machine"])
        except (OSError, json.JSONDecodeError, KeyError):
            pass

    builtin = subprocess.check_output(
        [cc, "-print-file-name=include"], text=True).strip()
    sysroot = os.path.realpath(subprocess.check_output(
        [cxx, "-print-sysroot"], text=True).strip())
    ver = subprocess.check_output(
        [cxx, "-dumpversion"], text=True).strip()
    machine = subprocess.check_output(
        [cxx, "-dumpmachine"], text=True).strip()

    os.makedirs(os.path.dirname(cache_path), exist_ok=True)
    with open(cache_path, "w") as f:
        json.dump({**key, "builtin": builtin, "sysroot": sysroot,
                   "ver": ver, "machine": machine}, f)
    return builtin, sysroot, ver, machine


def render_template(env, template_name, output_path, config):
    """Render a Jinja2 template to a file."""
    try:
        template = env.get_template(template_name)
    except Exception as e:
        print(f"Error loading template {template_name}: {e}")
        sys.exit(1)

    content = template.render(config=config)
    with open(output_path, "w") as f:
        f.write(content)


def generate_configs(ws):
    """Generate configuration files from .config using Jinja2 templates."""
    if Environment is None:
        print("Error: jinja2 not installed. Install with: pip install jinja2")
        sys.exit(1)

    config = ws.config
    output_dir = ws.gen_dir
    templates_dir = os.path.join(ws.ove_dir, "config", "templates")
    os.makedirs(output_dir, exist_ok=True)

    env = Environment(
        loader=FileSystemLoader(templates_dir),
        keep_trailing_newline=True,
        trim_blocks=True,
        lstrip_blocks=True,
    )

    # Helper functions available in templates
    env.globals["get_bool"] = lambda k, d=False: get_bool(config, k, d)
    from ..workspace import get_int
    env.globals["get_int"] = lambda k, d=0: get_int(config, k, d)
    env.globals["get_str"] = lambda k, d="": get_str(config, k, d)
    env.globals["config"] = config

    # NuttX + C++/inference: discover GCC C++ sysroot paths so cmake
    # doesn't have to introspect the compiler on every invocation.
    is_nuttx = get_bool(config, "CONFIG_OVE_RTOS_NUTTX")
    needs_cxx = (get_bool(config, "CONFIG_OVE_APP_LANG_CXX")
                 or get_bool(config, "CONFIG_OVE_INFER"))
    if is_nuttx and needs_cxx and ws.toolchain_dir:
        cross = get_str(config, "CONFIG_OVE_CROSS_COMPILE", "arm-none-eabi-")
        cc = os.path.join(ws.toolchain_dir, "bin", cross + "gcc")
        cxx = os.path.join(ws.toolchain_dir, "bin", cross + "g++")
        if os.path.isfile(cc) and os.path.isfile(cxx):
            cache_path = os.path.join(ws.gen_dir, ".gcc_sysroot_cache.json")
            builtin, sysroot, ver, machine = _gcc_sysroot_cached(
                cc, cxx, cache_path)
            cxx_base = os.path.join(sysroot, "include", "c++", ver)
            cxx_mach = os.path.join(cxx_base, machine)
            cxx_dirs = [cxx_base, cxx_mach]
            for d in glob.glob(os.path.join(cxx_mach, "thumb", "*", "*")):
                if os.path.isdir(d) and os.path.isfile(
                        os.path.join(d, "bits", "c++config.h")):
                    cxx_dirs.append(d)
            config["_GCC_BUILTIN_INC"] = builtin
            config["_GCC_CXX_DIRS"] = cxx_dirs
            nuttx_src = os.path.join(ws.ws_dl_dir, "nuttx")
            config["_NUTTX_INC"] = os.path.join(nuttx_src, "include")
            libm = glob.glob(os.path.join(
                nuttx_src, "libs", "libm", "*", "include"))
            config["_NUTTX_LIBM_INC"] = libm[0] if libm else ""

    # Resolve lv_conf.h directory once so cmake doesn't repeat the search.
    if get_bool(config, "CONFIG_OVE_LVGL"):
        from .assets import find_board_lv_conf
        lv_conf = find_board_lv_conf(ws)
        if lv_conf:
            config["_LV_CONF_DIR"] = os.path.dirname(lv_conf)

    # Always generate core config files
    render_template(env, "ove_config.h.j2",
                    os.path.join(output_dir, "ove_config.h"), config)
    print(f"  Generated: {output_dir}/ove_config.h")

    render_template(env, "ove_config.cmake.j2",
                    os.path.join(output_dir, "ove_config.cmake"), config)
    print(f"  Generated: {output_dir}/ove_config.cmake")

    # RTOS-specific config
    if get_bool(config, "CONFIG_OVE_RTOS_FREERTOS"):
        render_template(env, "FreeRTOSConfig.h.j2",
                        os.path.join(output_dir, "FreeRTOSConfig.h"), config)
        print(f"  Generated: {output_dir}/FreeRTOSConfig.h")

    elif get_bool(config, "CONFIG_OVE_RTOS_ZEPHYR"):
        render_template(env, "prj.conf.j2",
                        os.path.join(output_dir, "prj.conf"), config)
        print(f"  Generated: {output_dir}/prj.conf")

    elif get_bool(config, "CONFIG_OVE_RTOS_NUTTX"):
        render_template(env, "nuttx_defconfig.j2",
                        os.path.join(output_dir, "nuttx_defconfig"), config)
        print(f"  Generated: {output_dir}/nuttx_defconfig")
        # ove_sources.mk is no longer needed — NuttX uses CMake with
        # ove_config.cmake (OVE_BACKEND_SOURCES) like the other RTOSes.

    # Generate model C arrays from .tflite files (pre-generated so they
    # are available at CMake configure time for all RTOSes).
    if get_bool(config, "CONFIG_OVE_INFER"):
        model_dir = os.path.join(ws.ove_dir, "models")
        gen_models_dir = os.path.join(output_dir, "generated_models")
        convert_script = os.path.join(model_dir, "convert.py")
        if os.path.isdir(model_dir) and os.path.isfile(convert_script):
            tflite_files = glob.glob(os.path.join(model_dir, "**/*.tflite"),
                                     recursive=True)
            if tflite_files:
                os.makedirs(gen_models_dir, exist_ok=True)
                result = subprocess.run(
                    [sys.executable, convert_script,
                     "--model-dir", model_dir,
                     "--output-dir", gen_models_dir],
                    capture_output=True, text=True)
                if result.returncode == 0:
                    print(f"  Generated: {gen_models_dir}/ "
                          f"({len(tflite_files)} models)")
                else:
                    print(f"  Warning: model conversion failed: "
                          f"{result.stderr.strip()}")

    print("Config generation complete.")
