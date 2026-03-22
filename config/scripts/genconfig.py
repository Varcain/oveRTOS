#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Generate configuration files from .config using Jinja2 templates."""

import os
import sys
import argparse

try:
    from jinja2 import Environment, FileSystemLoader
except ImportError:
    print("Error: jinja2 not installed. Install with: pip install jinja2")
    sys.exit(1)


def parse_dotconfig(path):
    """Parse a Kconfig .config file into a dict."""
    config = {}
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                # Check for "# CONFIG_FOO is not set"
                if line.startswith("# CONFIG_") and line.endswith(" is not set"):
                    key = line.split()[1]
                    config[key] = False
                continue
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip()
            if value == "y":
                config[key] = True
            elif value == "n":
                config[key] = False
            elif value.startswith('"') and value.endswith('"'):
                config[key] = value[1:-1]
            elif value.startswith("0x") or value.startswith("0X"):
                config[key] = int(value, 16)
            else:
                try:
                    config[key] = int(value)
                except ValueError:
                    config[key] = value
    return config


def get_config_bool(config, key, default=False):
    """Get a boolean config value."""
    val = config.get(key, default)
    if isinstance(val, bool):
        return val
    if isinstance(val, str):
        return val.lower() in ("y", "yes", "true", "1")
    return bool(val)


def get_config_int(config, key, default=0):
    """Get an integer config value."""
    val = config.get(key, default)
    if isinstance(val, int):
        return val
    try:
        return int(val)
    except (ValueError, TypeError):
        return default


def get_config_str(config, key, default=""):
    """Get a string config value."""
    return str(config.get(key, default))


def main():
    parser = argparse.ArgumentParser(description="Generate oveRTOS config files")
    parser.add_argument("--config", default=".config",
                        help="Path to .config file")
    parser.add_argument("--output-dir", default="output/generated",
                        help="Output directory for generated files")
    parser.add_argument("--templates-dir", default="config/templates",
                        help="Templates directory")
    args = parser.parse_args()

    ove_dir = os.environ.get("OVE_DIR",
        os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__)))))

    config_path = os.path.join(ove_dir, args.config)
    output_dir = os.path.join(ove_dir, args.output_dir)
    templates_dir = os.path.join(ove_dir, args.templates_dir)

    if not os.path.isfile(config_path):
        print(f"Error: config file not found: {config_path}")
        sys.exit(1)

    config = parse_dotconfig(config_path)
    os.makedirs(output_dir, exist_ok=True)

    env = Environment(
        loader=FileSystemLoader(templates_dir),
        keep_trailing_newline=True,
        trim_blocks=True,
        lstrip_blocks=True,
    )

    # Helper functions for templates
    env.globals["get_bool"] = lambda k, d=False: get_config_bool(config, k, d)
    env.globals["get_int"] = lambda k, d=0: get_config_int(config, k, d)
    env.globals["get_str"] = lambda k, d="": get_config_str(config, k, d)
    env.globals["config"] = config

    # Always generate ove_config.h
    render_template(env, "ove_config.h.j2",
                    os.path.join(output_dir, "ove_config.h"), config)
    print(f"  Generated: {output_dir}/ove_config.h")

    # Always generate ove_config.cmake
    render_template(env, "ove_config.cmake.j2",
                    os.path.join(output_dir, "ove_config.cmake"), config)
    print(f"  Generated: {output_dir}/ove_config.cmake")

    # RTOS-specific config
    if get_config_bool(config, "CONFIG_OVE_RTOS_FREERTOS"):
        render_template(env, "FreeRTOSConfig.h.j2",
                        os.path.join(output_dir, "FreeRTOSConfig.h"), config)
        print(f"  Generated: {output_dir}/FreeRTOSConfig.h")

    elif get_config_bool(config, "CONFIG_OVE_RTOS_ZEPHYR"):
        render_template(env, "prj.conf.j2",
                        os.path.join(output_dir, "prj.conf"), config)
        print(f"  Generated: {output_dir}/prj.conf")

    elif get_config_bool(config, "CONFIG_OVE_RTOS_NUTTX"):
        render_template(env, "nuttx_defconfig.j2",
                        os.path.join(output_dir, "nuttx_defconfig"), config)
        print(f"  Generated: {output_dir}/nuttx_defconfig")
        render_template(env, "ove_sources.mk.j2",
                        os.path.join(output_dir, "ove_sources.mk"), config)
        print(f"  Generated: {output_dir}/ove_sources.mk")

    elif get_config_bool(config, "CONFIG_OVE_RTOS_POSIX"):
        pass  # ove_config.h and .cmake are already generated above

    print("Config generation complete.")


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


if __name__ == "__main__":
    main()
