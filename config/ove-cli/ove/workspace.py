# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Workspace resolution, .config parsing, and directory management."""

import contextlib
import json
import os


WORKSPACE_DIR_ENV = "OVE_WORKSPACE_DIR"
APP_PATH_FILE = ".ove-app-path"


def write_app_path(workspace_dir, app_dir):
    """Record the configured external app relative to its workspace."""
    path = os.path.join(workspace_dir, APP_PATH_FILE)
    temporary = path + ".tmp"
    relative = os.path.relpath(os.path.realpath(app_dir), workspace_dir)
    with open(temporary, "w") as f:
        f.write(relative + "\n")
    os.replace(temporary, path)


def find_ove_dir():
    """Find the oveRTOS root directory.

    Walks up from the current working directory looking for Config.in.
    Falls back to the directory three levels above this file.
    """
    # Try from cwd upwards
    d = os.getcwd()
    for _ in range(10):
        if os.path.isfile(os.path.join(d, "Config.in")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent

    # Fallback: relative to this package
    # config/ove-cli/ove/workspace.py -> <ove_root>
    return os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))


def parse_dotconfig(path):
    """Parse a Kconfig .config file into a dict.

    Values: bool (y/n), int, hex->int, or string (quotes stripped).
    """
    config = {}
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
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


def get_bool(config, key, default=False):
    """Get a boolean config value."""
    val = config.get(key, default)
    if isinstance(val, bool):
        return val
    if isinstance(val, str):
        return val.lower() in ("y", "yes", "true", "1")
    return bool(val)


def get_int(config, key, default=0):
    """Get an integer config value."""
    val = config.get(key, default)
    if isinstance(val, int):
        return val
    try:
        return int(val)
    except (ValueError, TypeError):
        return default


def get_str(config, key, default=""):
    """Get a string config value."""
    return str(config.get(key, default))


class Workspace:
    """Manages the oveRTOS workspace: paths, config, RTOS detection."""

    def __init__(self, ove_dir=None):
        self.ove_dir = ove_dir or find_ove_dir()
        self.config_path = os.path.join(self.ove_dir, ".config")
        self.output_dir = os.path.join(self.ove_dir, "output")
        self.dl_dir = os.path.join(self.ove_dir, "dl")
        self.venv_dir = os.path.join(self.ove_dir, ".venv")

        self._config = None
        self.build_log = None
        self.is_isolated = False
        self._resolve_workspace()

    def _resolve_workspace(self):
        """Resolve an explicit workspace or the repository's active one."""
        explicit = os.environ.get(WORKSPACE_DIR_ENV)
        if explicit:
            self.workspace_dir = os.path.realpath(
                os.path.abspath(os.path.expanduser(explicit)))
            self.config_path = os.path.join(self.workspace_dir, ".config")
            self.is_isolated = True
        elif os.path.islink(self.config_path):
            real = os.path.realpath(self.config_path)
            self.workspace_dir = os.path.dirname(real)
        else:
            if os.path.isfile(self.config_path):
                import logging
                logging.getLogger("ove").warning(
                    ".config is a regular file, not a symlink — "
                    "workspace may be misconfigured")
            self.workspace_dir = self.output_dir

        self.build_dir = os.path.join(self.workspace_dir, "build")
        self.gen_dir = os.path.join(self.workspace_dir, "generated")
        self.images_base_dir = os.path.join(self.workspace_dir, "images")
        self.ws_dl_dir = os.path.join(self.workspace_dir, "dl")
        self.toolchains_dir = os.path.join(self.output_dir, "toolchains")

    @property
    def config(self):
        """Lazy-loaded parsed .config dict."""
        if self._config is None:
            if not os.path.isfile(self.config_path):
                raise FileNotFoundError(
                    ".config not found. Run 'make menuconfig' or "
                    "'make <name>_defconfig' first.")
            self._config = parse_dotconfig(self.config_path)
        return self._config

    def require_config(self):
        """Ensure .config exists, raise if not."""
        _ = self.config
        return self

    @property
    def rtos(self):
        """Detect RTOS from .config."""
        for name in ("FREERTOS", "ZEPHYR", "NUTTX", "POSIX"):
            if get_bool(self.config, f"CONFIG_OVE_RTOS_{name}"):
                return name.lower()
        return None

    @property
    def board_name(self):
        return get_str(self.config, "CONFIG_OVE_BOARD_NAME")

    @property
    def board_dir(self):
        name = self.board_name
        if name:
            return os.path.join(self.ove_dir, "boards", name)
        return None

    @property
    def app_name(self):
        return get_str(self.config, "CONFIG_OVE_APP_NAME")

    @property
    def is_external_app(self):
        """Whether this workspace was configured from an external app."""
        return os.path.isfile(os.path.join(self.workspace_dir, APP_PATH_FILE))

    @property
    def app_dir(self):
        name = self.app_name
        if not name:
            return None
        configured_path = os.path.join(self.workspace_dir, APP_PATH_FILE)
        if os.path.isfile(configured_path):
            with open(configured_path) as f:
                path = f.read().strip()
            if path:
                if not os.path.isabs(path):
                    path = os.path.join(self.workspace_dir, path)
                return os.path.realpath(path)
        # Check app_paths.json first (handles both flat and two-level layouts)
        paths_file = os.path.join(self.ove_dir, "output", "kconfig",
                                  "app_paths.json")
        if os.path.isfile(paths_file):
            with open(paths_file) as f:
                app_paths = json.load(f)
            if name in app_paths:
                return app_paths[name]
        # Fallback: flat in-tree layout (backward compat)
        in_tree = os.path.join(self.ove_dir, "apps", name)
        if os.path.isdir(in_tree):
            return in_tree
        return None

    @property
    def app_lang(self):
        return get_str(self.config, "CONFIG_OVE_APP_LANG", "c")

    @property
    def arm_float_abi(self):
        """FreeRTOS ARM calling convention selected by Kconfig.

        Old workspaces predate the choice and therefore intentionally retain
        the historical hard-float default until their configuration is
        regenerated.
        """
        if get_bool(self.config, "CONFIG_OVE_ARM_FLOAT_ABI_SOFTFP"):
            return "softfp"
        return "hard"

    @property
    def linux_guest_float_abi(self):
        """Guest ABI, or None where no such choice exists.

        The Kconfig choice is gated on OVE_LINUX && OVE_RTOS_FREERTOS, so every
        other configuration has no guest ABI dimension at all.
        """
        if not get_bool(self.config, "CONFIG_OVE_LINUX"):
            return None
        if self.rtos != "freertos":
            return None
        if get_bool(self.config, "CONFIG_OVE_LINUX_GUEST_FLOAT_ABI_HARD"):
            return "hard"
        return "soft"

    @property
    def image_variant(self):
        """Subdirectory discriminating images that share one workspace.

        The workspace path already separates board, RTOS and app, so images can
        only overwrite one another when they differ solely by a float ABI —
        which lives inside .config and is therefore invisible to the path. Only
        the Linux personality carries both ABI dimensions; every other
        configuration has nothing to disambiguate and keeps the flat layout.
        """
        guest = self.linux_guest_float_abi
        if guest is None:
            return None
        return f"{self.arm_float_abi}-guest-{guest}"

    @property
    def images_dir(self):
        """Image output directory for the active configuration."""
        variant = self.image_variant
        if variant:
            return os.path.join(self.images_base_dir, variant)
        return self.images_base_dir

    @property
    def rtos_config_path(self):
        """Path to user's RTOS-native config customizations."""
        return os.path.join(self.workspace_dir, "rtos.config")

    @property
    def toolchain_dir(self):
        """Resolve the ARM toolchain directory."""
        if get_bool(self.config, "CONFIG_OVE_TOOLCHAIN_DOWNLOAD"):
            tc = os.path.join(self.workspace_dir, "toolchain")
            if os.path.exists(tc):
                return os.path.realpath(tc)
            sentinel = os.path.join(self.toolchains_dir, "path.txt")
            if os.path.isfile(sentinel):
                with open(sentinel) as f:
                    return f.read().strip()
        elif get_bool(self.config, "CONFIG_OVE_TOOLCHAIN_CUSTOM"):
            return get_str(self.config,
                           "CONFIG_OVE_TOOLCHAIN_CUSTOM_PATH")
        return None

    def toolchain_env(self):
        """Return a dict of environment overrides for toolchain PATH."""
        env = dict(os.environ)
        tc = self.toolchain_dir
        if tc:
            env["PATH"] = os.path.join(tc, "bin") + ":" + env.get("PATH", "")
        venv_bin = os.path.join(self.venv_dir, "bin")
        if os.path.isdir(venv_bin):
            env["PATH"] = venv_bin + ":" + env.get("PATH", "")
        return env

    def ensure_dirs(self):
        """Create workspace directories."""
        os.makedirs(self.build_dir, exist_ok=True)
        os.makedirs(self.gen_dir, exist_ok=True)
        os.makedirs(self.images_dir, exist_ok=True)
        os.makedirs(self.ws_dl_dir, exist_ok=True)

    @contextlib.contextmanager
    def open_build_log(self, path):
        """Context manager that exposes an open log file as ws.build_log.

        Inner build helpers read ws.build_log to tee subprocess output.
        Always closes the file on exit, even if the build raises.
        """
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        f = open(path, "w")
        self.build_log = f
        try:
            yield f
        finally:
            self.build_log = None
            f.close()

    def validate(self):
        """Verify all resolved workspace paths exist."""
        import logging
        logger = logging.getLogger("ove")
        ok = True
        for name, path in [
            ("workspace", self.workspace_dir),
            ("build", self.build_dir),
            ("generated", self.gen_dir),
        ]:
            if not os.path.isdir(path):
                logger.warning(f"Workspace path does not exist: {name} ({path})")
                ok = False
        return ok
