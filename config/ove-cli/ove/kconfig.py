# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Kconfig operations: menuconfig, defconfig, savedefconfig."""

import os
import sys

from .appgen import generate_app_kconfig
from .workspace import find_ove_dir


def _find_external_app_for_defconfig(defconfig_path):
    """If defconfig_path lives under an external app, return that app dir."""
    ext_apps_env = os.environ.get("OVE_EXTERNAL_APPS", "")
    if not ext_apps_env:
        return None
    defconfig_abs = os.path.abspath(defconfig_path)
    for d in ext_apps_env.split(":"):
        d = d.strip()
        if not d:
            continue
        app_abs = os.path.abspath(d)
        if defconfig_abs.startswith(app_abs + os.sep):
            return app_abs
    return None


def cmd_menuconfig(args):
    """Run Kconfig menuconfig TUI."""
    ove_dir = find_ove_dir()
    os.chdir(ove_dir)

    config_path = os.path.join(ove_dir, ".config")
    os.environ.setdefault("KCONFIG_CONFIG", config_path)
    os.environ["srctree"] = ove_dir

    if os.path.isfile(config_path) and not os.path.islink(config_path):
        print("WARNING: .config is a regular file, not a workspace symlink.")
        print("  Run 'ove defconfig <name>' to use workspace separation.")
        print()

    try:
        import kconfiglib
    except ImportError:
        print("Error: kconfiglib not installed.")
        print("Install with: pip install kconfiglib")
        sys.exit(1)

    try:
        from menuconfig import menuconfig
    except ImportError:
        print("Error: menuconfig not installed.")
        print("Install with: pip install kconfiglib")
        sys.exit(1)

    generate_app_kconfig(ove_dir)
    kconf = kconfiglib.Kconfig("Config.in")
    menuconfig(kconf)


def cmd_defconfig(args):
    """Load a defconfig and set up workspace."""
    ove_dir = find_ove_dir()
    name = args.name

    # Ensure it ends with _defconfig
    if not name.endswith("_defconfig"):
        name = name + "_defconfig"

    # Find the defconfig file — search in-tree first, then external apps
    search_dirs = [os.path.join(ove_dir, "defconfigs")]
    ext_apps_env = os.environ.get("OVE_EXTERNAL_APPS", "")
    if ext_apps_env:
        for d in ext_apps_env.split(":"):
            d = d.strip()
            if d:
                ext_defconfigs = os.path.join(os.path.abspath(d), "defconfigs")
                if os.path.isdir(ext_defconfigs):
                    search_dirs.append(ext_defconfigs)

    defconfig_path = None
    for defconfig_dir in search_dirs:
        for root, dirs, files in os.walk(defconfig_dir):
            if name in files:
                defconfig_path = os.path.join(root, name)
                break
        if defconfig_path:
            break

    if not defconfig_path:
        print(f"Error: defconfig '{name}' not found in defconfigs/")
        sys.exit(1)

    # Parse workspace location from defconfig name.
    # Filename format: <board>_<rtos>_<app>[_zeroheap]_defconfig
    # Extract board by finding the first known RTOS name in the stem.
    stem = name.replace("_defconfig", "")
    valid_rtos = ("freertos", "nuttx", "zephyr", "posix")
    ws_board = None
    ws_rtos = None
    for rtos in valid_rtos:
        idx = stem.find(f"_{rtos}_")
        if idx >= 0:
            ws_board = stem[:idx]
            ws_rtos = rtos
            break
    if not ws_board or not ws_rtos:
        print(f"Error: cannot parse board/RTOS from defconfig '{name}'")
        sys.exit(1)

    ws_app = stem[len(ws_board) + 1 + len(ws_rtos) + 1:]
    if not ws_app:
        print(f"Error: could not parse app name from defconfig '{name}'")
        sys.exit(1)

    output_dir = os.path.join(ove_dir, "output")

    # Determine if this defconfig comes from an external app.
    # If so, place the workspace under the external app's output/ dir.
    ext_app_dir = _find_external_app_for_defconfig(defconfig_path)
    if ext_app_dir:
        ws_output = os.path.join(ext_app_dir, "output")
        ws_dir = os.path.join(ws_output, ws_board, ws_rtos, ws_app)
        print(f"Loading defconfig: {defconfig_path}")
        print(f"  Workspace: {ws_dir}/")
    else:
        ws_dir = os.path.join(output_dir, ws_board, ws_rtos, ws_app)
        rel_defconfig = os.path.relpath(defconfig_path, ove_dir)
        print(f"Loading defconfig: {rel_defconfig}")
        print(f"  Workspace: output/{ws_board}/{ws_rtos}/{ws_app}/")

    os.makedirs(ws_dir, exist_ok=True)

    # Use kconfiglib to load and expand the defconfig
    try:
        import kconfiglib
    except ImportError:
        print("Error: kconfiglib not installed.")
        sys.exit(1)

    os.environ["srctree"] = ove_dir
    generate_app_kconfig(ove_dir)
    kconf = kconfiglib.Kconfig(os.path.join(ove_dir, "Config.in"))
    kconf.load_config(defconfig_path)
    ws_config = os.path.join(ws_dir, ".config")
    kconf.write_config(ws_config)
    print(f"Configuration written to {ws_config}")

    # Symlink root .config -> workspace .config
    config_link = os.path.join(ove_dir, ".config")
    if os.path.islink(config_link) or os.path.exists(config_link):
        os.unlink(config_link)
    os.symlink(ws_config, config_link)

    # Symlink output/current -> workspace
    os.makedirs(output_dir, exist_ok=True)
    current_link = os.path.join(output_dir, "current")
    if ext_app_dir:
        # External app: use absolute path since workspace is outside output/
        target = ws_dir
    else:
        target = os.path.join(ws_board, ws_rtos, ws_app)
    if os.path.islink(current_link):
        os.unlink(current_link)
    os.symlink(target, current_link)

    # Link toolchain if available
    tc_sentinel = os.path.join(output_dir, "toolchains", "path.txt")
    if os.path.isfile(tc_sentinel):
        with open(tc_sentinel) as f:
            tc_path = f.read().strip()
        tc_name = os.path.basename(tc_path)
        tc_link = os.path.join(ws_dir, "toolchain")
        rel = os.path.relpath(
            os.path.join(output_dir, "toolchains", tc_name), ws_dir)
        if os.path.islink(tc_link):
            os.unlink(tc_link)
        if os.path.isdir(os.path.join(output_dir, "toolchains", tc_name)):
            os.symlink(rel, tc_link)

    print(f"Active workspace: {ws_dir}/")


def _setup_workspace(ove_dir, ws_board, ws_rtos, ws_app, ext_app_dir=None):
    """Set up workspace directory and symlinks. Returns workspace dir path."""
    output_dir = os.path.join(ove_dir, "output")

    if ext_app_dir:
        ws_output = os.path.join(ext_app_dir, "output")
        ws_dir = os.path.join(ws_output, ws_board, ws_rtos, ws_app)
    else:
        ws_dir = os.path.join(output_dir, ws_board, ws_rtos, ws_app)

    os.makedirs(ws_dir, exist_ok=True)

    # Symlink root .config -> workspace .config
    config_link = os.path.join(ove_dir, ".config")
    ws_config = os.path.join(ws_dir, ".config")
    if os.path.islink(config_link) or os.path.exists(config_link):
        os.unlink(config_link)
    os.symlink(ws_config, config_link)

    # Symlink output/current -> workspace
    os.makedirs(output_dir, exist_ok=True)
    current_link = os.path.join(output_dir, "current")
    if ext_app_dir:
        target = ws_dir
    else:
        target = os.path.join(ws_board, ws_rtos, ws_app)
    if os.path.islink(current_link):
        os.unlink(current_link)
    os.symlink(target, current_link)

    # Link toolchain if available
    tc_sentinel = os.path.join(output_dir, "toolchains", "path.txt")
    if os.path.isfile(tc_sentinel):
        with open(tc_sentinel) as f:
            tc_path = f.read().strip()
        tc_name = os.path.basename(tc_path)
        tc_link = os.path.join(ws_dir, "toolchain")
        rel = os.path.relpath(
            os.path.join(output_dir, "toolchains", tc_name), ws_dir)
        if os.path.islink(tc_link):
            os.unlink(tc_link)
        if os.path.isdir(os.path.join(output_dir, "toolchains", tc_name)):
            os.symlink(rel, tc_link)

    return ws_dir


def _find_board_dir(ove_dir, board_short):
    """Find board directory matching a short name.

    Matches against directory basename or the 'name' field in board.yaml.
    Short names like 'qemu', 'stm32f746', 'host', 'wasm' match via prefix.
    """
    boards_dir = os.path.join(ove_dir, "boards")
    if not os.path.isdir(boards_dir):
        return None

    for entry in sorted(os.listdir(boards_dir)):
        board_path = os.path.join(boards_dir, entry)
        if not os.path.isdir(board_path):
            continue
        # Exact match on directory name
        if entry == board_short:
            return board_path
        # Prefix match (e.g., 'qemu' matches 'qemu-mps2-an500')
        if entry.startswith(board_short + "-") or entry.startswith(board_short):
            return board_path
    return None


def _find_board_kconfig_symbol(ove_dir, board_dir_name):
    """Find the CONFIG_OVE_BOARD_* symbol for a board directory name."""
    hw_config = os.path.join(ove_dir, "config", "Config.in.hardware")
    if not os.path.isfile(hw_config):
        return None

    # Build mapping from board directory names to Kconfig symbols
    # by reading the 'default "board-name" if OVE_BOARD_*' lines
    import re
    with open(hw_config) as f:
        content = f.read()

    for m in re.finditer(
            r'config (OVE_BOARD_\w+)\s.*?default\s+"([^"]+)"',
            content, re.DOTALL):
        if m.group(1) == "OVE_BOARD_NAME":
            continue
        # Find the board name associated with this symbol from
        # the OVE_BOARD_NAME defaults
    # Simpler: match board dir name against OVE_BOARD_NAME defaults
    symbol_map = {}
    for m in re.finditer(
            r'default\s+"([^"]+)"\s+if\s+(OVE_BOARD_\w+)',
            content):
        symbol_map[m.group(1)] = m.group(2)

    return symbol_map.get(board_dir_name)


def _load_yaml(path):
    """Load a YAML file."""
    try:
        import yaml
    except ImportError:
        # Fallback: simple parser for the fields we need
        return _load_yaml_simple(path)
    with open(path) as f:
        return yaml.safe_load(f)


def _load_yaml_simple(path):
    """Minimal YAML parser for board.yaml/app.yaml (flat lists only)."""
    # Use the workspace module's parser if available
    result = {}
    current_key = None
    current_list = None
    current_dict = None
    current_dict_key = None

    with open(path) as f:
        for line in f:
            stripped = line.rstrip()
            if not stripped or stripped.startswith('#'):
                continue
            indent = len(line) - len(line.lstrip())

            if indent == 0 and ':' in stripped:
                if current_key and current_list is not None:
                    result[current_key] = current_list
                elif current_key and current_dict is not None:
                    result[current_key] = current_dict
                key, _, val = stripped.partition(':')
                current_key = key.strip()
                val = val.strip()
                if val:
                    result[current_key] = val
                    current_key = None
                    current_list = None
                    current_dict = None
                else:
                    current_list = None
                    current_dict = None
            elif stripped.lstrip().startswith('- '):
                val = stripped.lstrip()[2:].strip()
                if val.startswith('"') and val.endswith('"'):
                    val = val[1:-1]
                if current_list is None:
                    current_list = []
                if current_dict_key is not None:
                    if current_dict is None:
                        current_dict = {}
                    if current_dict_key not in current_dict:
                        current_dict[current_dict_key] = []
                    current_dict[current_dict_key].append(val)
                else:
                    current_list.append(val)
            elif indent > 0 and ':' in stripped and not stripped.lstrip().startswith('- '):
                key, _, val = stripped.partition(':')
                key = key.strip()
                val = val.strip()
                if not val:
                    # Sub-dict key (e.g., rtos_defconfig.freertos:)
                    current_dict_key = key
                    if current_dict is None:
                        current_dict = {}

    if current_key and current_list is not None:
        result[current_key] = current_list
    elif current_key and current_dict is not None:
        result[current_key] = current_dict

    return result


def _find_app_yaml(ove_dir, app_name):
    """Find the app.yaml for a given app config_name."""
    import json
    app_paths_file = os.path.join(ove_dir, "output", "kconfig", "app_paths.json")
    if os.path.isfile(app_paths_file):
        with open(app_paths_file) as f:
            paths = json.load(f)
        if app_name in paths:
            return os.path.join(paths[app_name], "app.yaml")

    # Fallback: scan apps/ directories
    apps_dir = os.path.join(ove_dir, "apps")
    for lang in ("c", "cpp", "rust", "zig"):
        for entry in os.listdir(os.path.join(apps_dir, lang)):
            yaml_path = os.path.join(apps_dir, lang, entry, "app.yaml")
            if os.path.isfile(yaml_path):
                data = _load_yaml(yaml_path)
                if data.get("config_name") == app_name:
                    return yaml_path
    return None


def _write_temp_defconfig(lines, path):
    """Write config lines to a temporary defconfig file."""
    with open(path, "w") as f:
        for line in lines:
            f.write(line + "\n")


def cmd_defconfig_fragments(args):
    """Load configuration by merging fragments and yaml-defined configs."""
    ove_dir = find_ove_dir()

    # Parse board.rtos.app from spec
    spec = args.spec
    parts = spec.split(".")
    if len(parts) != 3:
        print(f"Error: expected board.rtos.app, got '{spec}'")
        sys.exit(1)

    board, rtos, app = parts

    valid_rtos = ("freertos", "nuttx", "zephyr", "posix")
    if rtos not in valid_rtos:
        print(f"Error: unknown RTOS '{rtos}'. Valid: {', '.join(valid_rtos)}")
        sys.exit(1)

    zeroheap = getattr(args, 'zeroheap', False)
    frag_dir = os.path.join(ove_dir, "config", "fragments")

    # ── Resolve board ──────────────────────────────────────────────
    board_dir = _find_board_dir(ove_dir, board)
    if not board_dir:
        print(f"Error: board '{board}' not found in boards/")
        sys.exit(1)

    board_dir_name = os.path.basename(board_dir)
    board_yaml_path = os.path.join(board_dir, "board.yaml")
    if not os.path.isfile(board_yaml_path):
        print(f"Error: {board_dir_name}/board.yaml not found")
        sys.exit(1)

    board_yaml = _load_yaml(board_yaml_path)
    board_symbol = _find_board_kconfig_symbol(ove_dir, board_dir_name)
    if not board_symbol:
        print(f"Error: no Kconfig symbol found for board '{board_dir_name}'")
        sys.exit(1)

    # ── Resolve app ────────────────────────────────────────────────
    os.environ["srctree"] = ove_dir
    generate_app_kconfig(ove_dir)

    app_yaml_path = _find_app_yaml(ove_dir, app)
    if not app_yaml_path:
        print(f"Error: app '{app}' not found (no app.yaml with "
              f"config_name: {app})")
        sys.exit(1)

    app_yaml = _load_yaml(app_yaml_path)
    app_config_name = app_yaml.get("config_name", app)
    app_lang = app_yaml.get("lang", "c")

    # Map lang to Kconfig symbol
    lang_symbol_map = {
        "c": "OVE_APP_LANG_C",
        "cpp": "OVE_APP_LANG_CXX",
        "rust": "OVE_APP_LANG_RUST",
        "zig": "OVE_APP_LANG_ZIG",
    }
    lang_symbol = lang_symbol_map.get(app_lang)
    app_symbol = f"OVE_APP_{app_config_name.upper()}"

    # ── Build merged defconfig lines ───────────────────────────────
    # Layer 1: global fragment
    # Layer 2: board (from board.yaml)
    # Layer 3: rtos fragment
    # Layer 4: board rtos-specific (from board.yaml rtos_defconfig)
    # Layer 5: app (from app.yaml)
    # Layer 6: variant

    import tempfile
    try:
        import kconfiglib
    except ImportError:
        print("Error: kconfiglib not installed.")
        sys.exit(1)

    kconf = kconfiglib.Kconfig(os.path.join(ove_dir, "Config.in"))

    tmp_dir = tempfile.mkdtemp(prefix="ove_frag_")

    def _load_fragment(label, path):
        if os.path.isfile(path):
            rel = os.path.relpath(path, ove_dir)
            print(f"  [{label}] {rel}")
            kconf.load_config(path, replace=False)

    def _load_lines(label, lines):
        if not lines:
            return
        tmp = os.path.join(tmp_dir, f"{label}.defconfig")
        _write_temp_defconfig(lines, tmp)
        print(f"  [{label}] {len(lines)} configs from yaml")
        kconf.load_config(tmp, replace=False)

    # 1. Global
    global_path = os.path.join(frag_dir, "global.defconfig")
    if os.path.isfile(global_path):
        print("  [global] config/fragments/global.defconfig")
        kconf.load_config(global_path, replace=True)

    # 2. Board selection + board.yaml defconfig
    board_lines = [f"CONFIG_{board_symbol}=y"]
    board_defconfig = board_yaml.get("defconfig", [])
    if isinstance(board_defconfig, list):
        board_lines.extend(board_defconfig)
    _load_lines("board", board_lines)

    # 3. RTOS fragment
    rtos_path = os.path.join(frag_dir, "rtos", f"{rtos}.defconfig")
    _load_fragment("rtos", rtos_path)

    # 4. Board rtos-specific from board.yaml
    rtos_defconfig = board_yaml.get("rtos_defconfig", {})
    if isinstance(rtos_defconfig, dict):
        rtos_lines = rtos_defconfig.get(rtos, [])
        if rtos_lines:
            _load_lines("board+rtos", rtos_lines)

    # 5. App selection + app.yaml defconfig
    app_lines = [f"CONFIG_{app_symbol}=y"]
    if lang_symbol:
        app_lines.append(f"CONFIG_{lang_symbol}=y")
    app_defconfig = app_yaml.get("defconfig", [])
    if isinstance(app_defconfig, list):
        app_lines.extend(app_defconfig)
    _load_lines("app", app_lines)

    # 6. App rtos-specific from app.yaml (mirrors board rtos_defconfig)
    app_rtos_defconfig = app_yaml.get("rtos_defconfig", {})
    if isinstance(app_rtos_defconfig, dict):
        app_rtos_lines = app_rtos_defconfig.get(rtos, [])
        if app_rtos_lines:
            _load_lines("app+rtos", app_rtos_lines)

    # 7. Zeroheap variant
    if zeroheap:
        variant_path = os.path.join(frag_dir, "variant", "zeroheap.defconfig")
        _load_fragment("variant", variant_path)

    # Clean up temp files
    import shutil
    shutil.rmtree(tmp_dir, ignore_errors=True)

    # Determine workspace app name
    ws_app = app
    if zeroheap:
        ws_app += "_zeroheap"

    ws_dir = _setup_workspace(ove_dir, board, rtos, ws_app)
    ws_config = os.path.join(ws_dir, ".config")
    kconf.write_config(ws_config)

    print(f"Configuration written to {ws_config}")
    print(f"Active workspace: output/{board}/{rtos}/{ws_app}/")


def cmd_savedefconfig(args):
    """Save current config as minimal defconfig."""
    ove_dir = find_ove_dir()
    config_path = os.path.join(ove_dir, ".config")

    if not os.path.isfile(config_path):
        print("Error: .config not found. Run menuconfig or load a "
              "defconfig first.")
        sys.exit(1)

    try:
        import kconfiglib
    except ImportError:
        print("Error: kconfiglib not installed.")
        sys.exit(1)

    os.environ["srctree"] = ove_dir
    generate_app_kconfig(ove_dir)
    kconf = kconfiglib.Kconfig(os.path.join(ove_dir, "Config.in"))
    kconf.load_config(config_path)
    kconf.write_min_config(os.path.join(ove_dir, "defconfig"))
    print("Minimal config saved to defconfig")
