# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""App Kconfig generation — produces Config.in files from app.yaml descriptors."""

import os
import sys

try:
    import yaml
except ImportError:
    yaml = None

try:
    from jinja2 import Environment, FileSystemLoader
except ImportError:
    Environment = None


def generate_app_kconfig(ove_dir):
    """Scan apps/*/app.yaml and generate Kconfig files to output/kconfig/.

    Must be called before kconfiglib parses Config.in, since the root
    Config.in sources output/kconfig/apps/Config.in.
    """
    if yaml is None:
        print("Warning: pyyaml not installed, skipping app Kconfig generation")
        return

    apps_dir = os.path.join(ove_dir, "apps")
    if not os.path.isdir(apps_dir):
        return

    # Scan all app.yaml files
    apps = []
    for entry in sorted(os.listdir(apps_dir)):
        app_yaml_path = os.path.join(apps_dir, entry, "app.yaml")
        if os.path.isfile(app_yaml_path):
            with open(app_yaml_path) as f:
                data = yaml.safe_load(f)
            data["name"] = entry
            # Derive Kconfig symbol name: example_c -> EXAMPLE_C
            data["config_name"] = entry.upper()
            apps.append(data)

    if not apps:
        return

    # Ensure output directory
    kconfig_dir = os.path.join(ove_dir, "output", "kconfig", "apps")
    os.makedirs(kconfig_dir, exist_ok=True)

    # Generate per-app Config.in fragments
    for app in apps:
        app_kconfig_dir = os.path.join(kconfig_dir, app["name"])
        os.makedirs(app_kconfig_dir, exist_ok=True)
        app_config_in = os.path.join(app_kconfig_dir, "Config.in")

        lines = []
        lines.append(f'menu "{app["description"]}"')
        lines.append("")
        if app.get("kconfig"):
            lines.append(app["kconfig"].rstrip())
            lines.append("")
        lines.append("endmenu")
        lines.append("")

        with open(app_config_in, "w") as f:
            f.write("\n".join(lines))

    # Generate parent apps/Config.in
    _generate_parent_config_in(kconfig_dir, apps)


def _generate_parent_config_in(kconfig_dir, apps):
    """Generate the parent Config.in that lists all app choices."""
    lines = []
    lines.append("# Auto-generated from app.yaml files — do not edit.")
    lines.append("")
    lines.append('menu "Application"')
    lines.append("")

    # App selection choice
    lines.append("choice")
    lines.append('    prompt "Application to build"')
    # Default to the first app
    lines.append(f"    default OVE_APP_{apps[0]['config_name']}")
    lines.append("")

    for app in apps:
        lines.append(f"config OVE_APP_{app['config_name']}")
        lines.append(f'    bool "{app["name"]} — {app["description"]}"')
        lines.append("")

    lines.append("endchoice")
    lines.append("")

    # OVE_APP_NAME derived config
    lines.append("config OVE_APP_NAME")
    lines.append("    string")
    for app in apps:
        lines.append(
            f'    default "{app["name"]}" if OVE_APP_{app["config_name"]}'
        )
    lines.append("")

    # App language choice (static — same as original)
    lines.append("choice")
    lines.append('    prompt "Application language"')
    lines.append("    default OVE_APP_LANG_C")
    lines.append("")
    lines.append("config OVE_APP_LANG_C")
    lines.append('    bool "C"')
    lines.append("")
    lines.append("config OVE_APP_LANG_CXX")
    lines.append('    bool "C++"')
    lines.append("")
    lines.append("config OVE_APP_LANG_RUST")
    lines.append('    bool "Rust"')
    lines.append("")
    lines.append("config OVE_APP_LANG_ZIG")
    lines.append('    bool "Zig"')
    lines.append("")
    lines.append("endchoice")
    lines.append("")

    # OVE_APP_LANG derived config
    lines.append("config OVE_APP_LANG")
    lines.append("    string")
    lines.append('    default "c" if OVE_APP_LANG_C')
    lines.append('    default "cpp" if OVE_APP_LANG_CXX')
    lines.append('    default "rust" if OVE_APP_LANG_RUST')
    lines.append('    default "zig" if OVE_APP_LANG_ZIG')
    lines.append("")

    # Conditional per-app Kconfig includes
    for app in apps:
        lines.append(f"if OVE_APP_{app['config_name']}")
        lines.append(
            f'source "output/kconfig/apps/{app["name"]}/Config.in"'
        )
        lines.append("endif")
        lines.append("")

    lines.append("endmenu")
    lines.append("")

    config_in_path = os.path.join(kconfig_dir, "Config.in")
    with open(config_in_path, "w") as f:
        f.write("\n".join(lines))
