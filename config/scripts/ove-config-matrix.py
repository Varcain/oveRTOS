#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""
Build trace/profiler Kconfig combinations for one QEMU RTOS workspace.

The sim feature set (thread-state stats, trace stream, trace markers and
sampling profiler) is normally exercised with every option on.  This
driver rebuilds every legal choice of stats, trace and profiler (trace
markers stay off except in all-on) so link errors or missing no-op stubs
in a trimmed build show up in CI instead of on a user's desk.

Combinations:
  minimal         stats off, trace off, markers off, profiler off
  trace-nomarkers stats on, trace on, markers off, profiler off
  profiler-only   stats on, trace off, markers off, profiler on
  stats-only      stats on, trace off, markers off, profiler off
  trace-profiler  stats on, trace on, markers off, profiler on
  all-on          stats on, trace on, markers on, profiler on

The all-on baseline is built by the "all-on" combination above.

Every combination is re-resolved through Kconfig before it is built, so a
combination the Kconfig dependency graph rejects (config/Config.in.debug)
is reported instead of being built silently.

Usage:
  ove-config-matrix.py --rtos freertos
  ove-config-matrix.py --rtos zephyr --app example_c --ove /path/to/ove
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

_COMBOS = {
    "minimal": {
        "CONFIG_OVE_THREAD_STATE_STATS": False,
        "CONFIG_OVE_TRACE_STREAM": False,
        "CONFIG_OVE_TRACE_MARKERS": False,
        "CONFIG_OVE_PROFILER": False,
    },
    "trace-nomarkers": {
        "CONFIG_OVE_THREAD_STATE_STATS": True,
        "CONFIG_OVE_TRACE_STREAM": True,
        "CONFIG_OVE_TRACE_MARKERS": False,
        "CONFIG_OVE_PROFILER": False,
    },
    "profiler-only": {
        "CONFIG_OVE_THREAD_STATE_STATS": True,
        "CONFIG_OVE_TRACE_STREAM": False,
        "CONFIG_OVE_TRACE_MARKERS": False,
        "CONFIG_OVE_PROFILER": True,
    },
    "stats-only": {
        "CONFIG_OVE_THREAD_STATE_STATS": True,
        "CONFIG_OVE_TRACE_STREAM": False,
        "CONFIG_OVE_TRACE_MARKERS": False,
        "CONFIG_OVE_PROFILER": False,
    },
    "trace-profiler": {
        "CONFIG_OVE_THREAD_STATE_STATS": True,
        "CONFIG_OVE_TRACE_STREAM": True,
        "CONFIG_OVE_TRACE_MARKERS": False,
        "CONFIG_OVE_PROFILER": True,
    },
    "all-on": {
        "CONFIG_OVE_THREAD_STATE_STATS": True,
        "CONFIG_OVE_TRACE_STREAM": True,
        "CONFIG_OVE_TRACE_MARKERS": True,
        "CONFIG_OVE_PROFILER": True,
    },
}


def set_symbol(config: str, symbol: str, enabled: bool) -> str:
    """Set or unset one Kconfig symbol in a .config text blob."""
    line = f"{symbol}=y" if enabled else f"# {symbol} is not set"
    pattern = re.compile(
        rf"^(?:{re.escape(symbol)}=.*|# {re.escape(symbol)} is not set)$",
        re.MULTILINE,
    )
    if pattern.search(config):
        return pattern.sub(line, config, count=1)
    return config.rstrip("\n") + "\n" + line + "\n"


def check_symbol(config: str, symbol: str, enabled: bool) -> bool:
    if enabled:
        return re.search(rf"^{re.escape(symbol)}=y$", config, re.MULTILINE) is not None
    return re.search(rf"^{re.escape(symbol)}=y$", config, re.MULTILINE) is None


def load_kconfig():
    """Load the Kconfig tree the way config/ove-cli/ove/kconfig.py does.

    The dependency graph of config/Config.in* is the authority on which
    symbol values are legal, so the matrix has to ask Kconfig itself.
    """
    try:
        import kconfiglib
    except ImportError:
        sys.exit("Error: kconfiglib not installed. Run `make setup` first.")

    saved = os.environ.get("srctree")
    os.environ["srctree"] = str(REPO)
    try:
        kconf = kconfiglib.Kconfig(str(REPO / "Config.in"))
    finally:
        if saved is None:
            del os.environ["srctree"]
        else:
            os.environ["srctree"] = saved
    kconf.warn_assign_redun = False
    return kconf


def resolve_config(config_path: Path, kconf) -> str:
    """Re-resolve the edited .config through Kconfig and return it.

    `ove configure` only renders the Jinja templates from the already
    parsed .config -- it never re-runs Kconfig
    (config/ove-cli/ove/configure/__init__.py).  The file this script
    writes is therefore *not* the effective configuration: the dependency
    graph has never been applied to it.

    Loading and writing the file back through kconfiglib -- the same
    idiom as config/ove-cli/ove/kconfig.py -- applies that graph, so the
    returned text holds the effective values (dependencies honoured).
    """
    kconf.load_config(str(config_path))
    kconf.write_config(str(config_path))
    return config_path.read_text()


def kconfig_reason(kconf, symbol: str) -> str:
    """Explain why Kconfig did not keep *symbol* at the requested value."""
    import kconfiglib

    sym = kconf.syms.get(symbol[len("CONFIG_"):])
    if sym is None:
        return "not a Kconfig symbol"
    missing = sorted(
        f"CONFIG_{dep.name}"
        for dep in kconfiglib.expr_items(sym.direct_dep)
        if not dep.tri_value
    )
    if missing:
        return (f"depends on {kconfiglib.expr_str(sym.direct_dep)}; "
                f"not set: {', '.join(missing)}")
    selected = sorted(
        f"CONFIG_{dep.name}"
        for dep in kconfiglib.expr_items(sym.rev_dep)
        if dep.tri_value
    )
    if selected:
        return f"selected by {', '.join(selected)}"
    return f"kept at {sym.str_value} by Kconfig"


def elf_exists(build_dir: Path) -> bool:
    if not build_dir.is_dir():
        return False
    if any(build_dir.rglob("*.elf")):
        return True
    return any(
        p.is_file() and p.name in ("firmware", "nuttx") for p in build_dir.rglob("*")
    )


def run(argv: list[str], what: str) -> None:
    print(f"+ {' '.join(argv)}", flush=True)
    proc = subprocess.run(argv, check=False, cwd=REPO)
    if proc.returncode != 0:
        sys.exit(f"{what} failed with exit code {proc.returncode}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--rtos", required=True, choices=("freertos", "nuttx", "zephyr")
    )
    parser.add_argument("--app", default="example_c")
    parser.add_argument(
        "--ove",
        default=str(REPO / ".venv" / "bin" / "ove"),
        help="ove executable to use (default: repo .venv)",
    )
    args = parser.parse_args()

    ove = Path(args.ove)
    if not ove.is_file():
        sys.exit(f"{ove} not found; run `make setup` first")

    workspace = REPO / "output" / "qemu" / args.rtos / args.app
    spec = f"qemu.{args.rtos}.{args.app}"

    run([str(ove), "defconfig-fragments", spec], "defconfig-fragments")
    config_path = workspace / ".config"
    if not config_path.is_file():
        sys.exit(f"{config_path} was not generated")
    base = config_path.read_text()

    # `ove download` needs the workspace config, which the defconfig above
    # just created.  Fetch the RTOS sources before the first build -- the
    # same order the repo's own matrix pipeline uses
    # (config/ove-cli/ove/allconfigs.py: download -> configure -> build).
    run([str(ove), "download"], "download")

    kconf = load_kconfig()

    for name, overrides in _COMBOS.items():
        print(f"\n=== {args.rtos}/{args.app}: {name} ===", flush=True)
        config = base
        for symbol, enabled in overrides.items():
            config = set_symbol(config, symbol, enabled)
        config_path.write_text(config)

        # `ove configure` renders the templates from the .config as it is
        # and never re-runs Kconfig, so the dependency graph has to be
        # applied here: an illegal combination is reported instead of
        # being built.
        effective = resolve_config(config_path, kconf)
        for symbol, enabled in overrides.items():
            if not check_symbol(effective, symbol, enabled):
                expected = "y" if enabled else "not set"
                sys.exit(f"{name}: Kconfig did not keep {symbol}={expected}: "
                         f"{kconfig_reason(kconf, symbol)}")

        run([str(ove), "configure"], "configure")

        shutil.rmtree(workspace / "build", ignore_errors=True)
        run([str(ove), "build"], "build")
        if not elf_exists(workspace / "build"):
            sys.exit(f"{name}: build produced no ELF under {workspace}/build")
        print(f"=== {name}: OK ===", flush=True)

    # Leave the workspace configured like a normal defconfig run.
    config_path.write_text(base)
    run([str(ove), "configure"], "final configure")
    return 0


if __name__ == "__main__":
    sys.exit(main())
