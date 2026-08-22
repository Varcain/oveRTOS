# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""App Kconfig generation — produces Config.in files from app.yaml descriptors."""

import json
import os

try:
    import yaml
except ImportError:
    yaml = None

try:
    from jinja2 import Environment
except ImportError:
    Environment = None


def _scan_app_dirs(dirs):
    """Scan a list of directories for app.yaml files.

    Each directory should contain an app.yaml directly (not subdirs).
    Returns list of (name, path, data) tuples.
    """
    results = []
    for d in dirs:
        d = os.path.abspath(d)
        app_yaml_path = os.path.join(d, "app.yaml")
        if os.path.isfile(app_yaml_path):
            with open(app_yaml_path) as f:
                data = yaml.safe_load(f)
            name = data.get("config_name", os.path.basename(d))
            results.append((name, d, data))
    return results


def _scan_apps_dir(apps_dir, apps, app_paths):
    """Scan a directory tree for app.yaml files in three layouts:
       - flat:        <root>/<app>/app.yaml
       - two-level:   <root>/<lang>/<app>/app.yaml
       - three-level: <root>/<lang>/<heapmode>/<app>/app.yaml
                      where <heapmode> is `heap` or `zeroheap`
    Mutates `apps` and `app_paths` in place; later scans don't shadow
    earlier ones (first wins on config_name collision)."""
    if not os.path.isdir(apps_dir):
        return
    for subdir in sorted(os.listdir(apps_dir)):
        subdir_path = os.path.join(apps_dir, subdir)
        if not os.path.isdir(subdir_path):
            continue
        # flat: <root>/<app>/app.yaml
        flat_yaml = os.path.join(subdir_path, "app.yaml")
        if os.path.isfile(flat_yaml):
            with open(flat_yaml) as f:
                data = yaml.safe_load(f)
            cname = data.get("config_name", subdir)
            data["name"] = cname
            data["config_name"] = cname.upper()
            if cname not in app_paths:
                apps.append(data)
                app_paths[cname] = subdir_path
            continue
        # two-level / three-level: <root>/<lang>/<...>
        for entry in sorted(os.listdir(subdir_path)):
            entry_path = os.path.join(subdir_path, entry)
            if not os.path.isdir(entry_path):
                continue
            # two-level: <root>/<lang>/<app>/app.yaml
            app_yaml_path = os.path.join(entry_path, "app.yaml")
            if os.path.isfile(app_yaml_path):
                with open(app_yaml_path) as f:
                    data = yaml.safe_load(f)
                cname = data.get("config_name", entry)
                data["name"] = cname
                data["config_name"] = cname.upper()
                if cname not in app_paths:
                    apps.append(data)
                    app_paths[cname] = entry_path
                continue
            # three-level: <root>/<lang>/<heapmode>/<app>/app.yaml
            for app_entry in sorted(os.listdir(entry_path)):
                app_path = os.path.join(entry_path, app_entry)
                inner_yaml = os.path.join(app_path, "app.yaml")
                if not os.path.isfile(inner_yaml):
                    continue
                with open(inner_yaml) as f:
                    data = yaml.safe_load(f)
                cname = data.get("config_name", app_entry)
                data["name"] = cname
                data["config_name"] = cname.upper()
                if cname not in app_paths:
                    apps.append(data)
                    app_paths[cname] = app_path


def generate_app_kconfig(ove_dir):
    """Scan in-tree app.yaml files and external apps, generate Kconfig.

    Must be called before kconfiglib parses Config.in, since the root
    Config.in sources output/kconfig/apps/Config.in.

    Scans (in order, first-wins on name collisions):
      - apps/<lang>/<app>/app.yaml or apps/<app>/app.yaml
      - tests/benchmarks/<lang>/app.yaml (the cross-binding benchmark
        suite — historically lived under apps/<lang>/benchmark, moved
        out to make tests/benchmarks the canonical home for measurement
        apps)
      - $OVE_EXTERNAL_APPS — colon-separated dirs containing app.yaml
    """
    if yaml is None:
        print("Warning: pyyaml not installed, skipping app Kconfig generation")
        return

    apps = []
    app_paths = {}  # config_name -> absolute path mapping

    # In-tree apps (apps/<lang>/<app> or apps/<app>).
    _scan_apps_dir(os.path.join(ove_dir, "apps"), apps, app_paths)

    # Benchmark test suite under tests/benchmarks/<lang>/ — same flat
    # vs. two-level discovery rules; <lang> = c/cpp/rust/zig.
    _scan_apps_dir(os.path.join(ove_dir, "tests", "benchmarks"),
                   apps, app_paths)

    # Scan external apps from OVE_EXTERNAL_APPS env var
    ext_apps_env = os.environ.get("OVE_EXTERNAL_APPS", "")
    if ext_apps_env:
        ext_dirs = [d.strip() for d in ext_apps_env.split(":")
                    if d.strip()]
        for name, path, data in _scan_app_dirs(ext_dirs):
            if name in app_paths:
                print(f"Warning: external app '{name}' shadows "
                      f"in-tree app, skipping")
                continue
            data["name"] = name
            data["config_name"] = name.upper()
            apps.append(data)
            app_paths[name] = path

    if not apps:
        return

    # Write app path mapping so workspace can resolve external apps
    kconfig_base = os.path.join(ove_dir, "output", "kconfig")
    os.makedirs(kconfig_base, exist_ok=True)
    with open(os.path.join(kconfig_base, "app_paths.json"), "w") as f:
        json.dump(app_paths, f, indent=2)

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
