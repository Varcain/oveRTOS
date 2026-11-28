# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Browser demos for the documentation site.

Builds the configurations docs-site/demos.yml lists, keeps those whose
firmware starts, and lays them out in the built site where the pages link
to them: <site>/demos/<rtos>/<app>/ per demo, the emulator binary once in
<site>/demos/qemu/.  A configuration that fails to build or start gets a
TBD page in its place.  The Pages workflow and 'make docs-serve' both go
through here, so a local preview holds what gets published.

The Linux personality demos need a rootfs built with Buildroot outside this
tree: the local Buildroot output when there is one, else the image
manifest.yaml pins (tools.linux-rootfs).  Without either, they are TBD.
"""

import html
import json
import logging
import os
import select
import shutil
import subprocess
import sys
import time

from .allconfigs import _ove_bin, _run_interruptible, _run_pipeline
from .build import LINUX_ROOTFS_ENV, _DASHBOARD_PAGES, _TERMINAL_PAGES
from .download import download_linux_rootfs
from .manifest import load_manifest
from .workspace import Workspace

logger = logging.getLogger("ove")

# What a demo page needs out of the bundle 'ove build' wrote to serve/.
_DEMO_FILES = ("index.html", *_DASHBOARD_PAGES, "manifest.json", "firmware.elf")
_TERMINAL_FILES = ("index.html", *_TERMINAL_PAGES, "firmware.elf")
_EMULATOR_SCRIPT = "qemu-system-arm.js"
_EMULATOR_SHARED = ("qemu-system-arm.wasm", "SOURCE.txt")
_TBD_PAGE = """<!DOCTYPE html>
<meta charset="UTF-8">
<title>Browser demo TBD</title>
<p>The browser demo of <code>{name}</code> is TBD: this configuration does
not build or start in the browser yet.</p>
"""


def _app_metadata(ove_dir):
    """Map each app's config_name to its app.yaml contents."""
    import yaml
    apps = {}
    for root, _dirs, files in os.walk(os.path.join(ove_dir, "apps")):
        if "app.yaml" not in files:
            continue
        with open(os.path.join(root, "app.yaml")) as f:
            data = yaml.safe_load(f) or {}
        if data.get("config_name"):
            apps[data["config_name"]] = data
    return apps


def _smoke(ove_dir, firmware):
    """Boot the firmware under the native QEMU; returns (started, output)."""
    script = os.path.join(ove_dir, "config", "scripts", "qemu-smoke.sh")
    proc = subprocess.run([script, firmware], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)
    return proc.returncode == 0, proc.stdout.strip()


def _smoke_terminal(serve_dir, seconds=120):
    """Boot a personality bundle as its page would, under the native QEMU.

    Runs the command line web.json gives the browser, with the bundle's
    files in place of the emulator's own filesystem, and waits for the
    guest's login prompt.  Returns (started, output).
    """
    with open(os.path.join(serve_dir, "web.json")) as f:
        config = json.load(f)
    paths = {"/firmware.elf": os.path.join(serve_dir, config["firmware"])}
    for name, dest in (config.get("files") or {}).items():
        paths[dest] = os.path.join(serve_dir, name)
    args = []
    for arg in config["args"]:
        for dest, path in paths.items():
            arg = arg.replace(f"file={dest},", f"file={path},")
            if arg == dest:
                arg = path
        args.append(arg)
    proc = subprocess.Popen(["qemu-system-arm", *args],
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    output = b""
    deadline = time.monotonic() + seconds
    try:
        while b"login:" not in output and time.monotonic() < deadline:
            ready, _w, _x = select.select([proc.stdout], [], [], 1.0)
            if ready:
                chunk = os.read(proc.stdout.fileno(), 4096)
                if not chunk:
                    break
                output += chunk
    finally:
        proc.kill()
        proc.wait()
    if b"login:" in output:
        return True, ""
    text = output.decode("utf-8", errors="replace")
    return False, "smoke: no login prompt\n" + "\n".join(
        text.strip().splitlines()[-5:])


def personality_rootfs(ws):
    """The rootfs for the Linux personality demos, or None when there is none.

    Returns "" for the local Buildroot output, which the workspaces find on
    their own, else the path of the image manifest.yaml pins.
    """
    local = os.path.join(ws.ove_dir, "..", "buildroot", "output", "images",
                         "rootfs.cpio")
    if os.environ.get(LINUX_ROOTFS_ENV) or os.path.isfile(local):
        return ""
    return download_linux_rootfs(load_manifest(ws.ove_dir), ws.dl_dir)


def load_demos(ove_dir):
    """The <board>.<rtos>.<app> specs docs-site/demos.yml lists."""
    import yaml
    with open(os.path.join(ove_dir, "docs-site", "demos.yml")) as f:
        return yaml.safe_load(f)


def write_tbd(demos_dir, name):
    """Put a TBD page where demo *name* would be, so its links still work."""
    dest = os.path.join(demos_dir, name)
    os.makedirs(dest, exist_ok=True)
    with open(os.path.join(dest, "index.html"), "w") as f:
        f.write(_TBD_PAGE.format(name=html.escape(name)))


def assemble_demo(serve_dir, demos_dir, name):
    """Copy one bundle to <demos_dir>/<name>/.

    The emulator's script has to sit below the page: its worker threads
    load it again, and the service worker only isolates the page's own
    directory.  The large binary has no such limit, so every demo points
    at the one copy in <demos_dir>/qemu/.
    """
    emulator = os.path.realpath(os.path.join(serve_dir, "qemu"))
    dest = os.path.join(demos_dir, name)
    os.makedirs(os.path.join(dest, "qemu"), exist_ok=True)
    with open(os.path.join(serve_dir, "web.json")) as f:
        config = json.load(f)
    terminal = os.path.isfile(os.path.join(serve_dir, "terminal.js"))
    pages = _TERMINAL_FILES if terminal else _DEMO_FILES
    for filename in (*pages, *(config.get("files") or {})):
        path = os.path.join(dest, filename)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        shutil.copy2(os.path.join(serve_dir, filename), path)
    shutil.copy2(os.path.join(emulator, _EMULATOR_SCRIPT),
                 os.path.join(dest, "qemu"))

    shared = os.path.join(demos_dir, "qemu")
    os.makedirs(shared, exist_ok=True)
    for filename in _EMULATOR_SHARED:
        shutil.copy2(os.path.join(emulator, filename), shared)

    config["wasm"] = os.path.relpath(
        os.path.join(shared, "qemu-system-arm.wasm"), dest)
    with open(os.path.join(dest, "web.json"), "w") as f:
        json.dump(config, f, indent=2)
        f.write("\n")


def _cmd_docs_demos(args):
    ws = Workspace()
    site_dir = os.path.abspath(
        args.site or os.path.join(ws.ove_dir, "docs-site", "site"))
    demos_dir = os.path.join(site_dir, "demos")
    if os.path.isdir(demos_dir):
        shutil.rmtree(demos_dir)

    apps = _app_metadata(ws.ove_dir)
    rootfs = personality_rootfs(ws)
    ove_bin = _ove_bin(ws)
    deployed, unbuilt, broken, no_rootfs = [], [], [], []
    for spec in load_demos(ws.ove_dir):
        board, rtos, app = spec.split(".")
        name = f"{rtos}/{app}"
        workspace = os.path.join(ws.ove_dir, "output", board, rtos, app)
        serve_dir = os.path.join(workspace, "serve")
        linux = "CONFIG_OVE_LINUX=y" in (apps.get(app, {}).get("defconfig")
                                         or [])
        if linux and rootfs is None:
            no_rootfs.append(name)
            write_tbd(demos_dir, name)
            continue
        print(f"\n{'=' * 60}\nBuilding {spec}\n{'=' * 60}")
        setup = [ove_bin, "defconfig-fragments", spec, "--no-activate"]
        env = {LINUX_ROOTFS_ENV: rootfs} if linux and rootfs else None
        ok, _elapsed = _run_pipeline(ws.ove_dir, ove_bin, setup, workspace,
                                     env)
        if not ok or not os.path.isfile(os.path.join(serve_dir, "web.json")):
            unbuilt.append(name)
            write_tbd(demos_dir, name)
            continue
        # Only publish a demo whose firmware comes up under the native
        # QEMU: a page for an app that faults at start-up helps no one.
        if linux:
            started, output = _smoke_terminal(serve_dir)
        else:
            started, output = _smoke(
                ws.ove_dir, os.path.join(workspace, "images", "firmware.elf"))
        if not started:
            print(f"{name}: {output}")
            broken.append(name)
            write_tbd(demos_dir, name)
            continue
        assemble_demo(serve_dir, demos_dir, name)
        deployed.append(name)

    print(f"\n{'=' * 60}")
    print(f"Browser demos in {demos_dir}")
    print(f"Deployed ({len(deployed)}): {' '.join(deployed) or 'none'}")
    print(f"TBD, build failed: {' '.join(unbuilt) or 'none'}")
    print(f"TBD, failed the smoke run: {' '.join(broken) or 'none'}")
    if no_rootfs:
        print("TBD, no Buildroot rootfs and manifest.yaml pins none "
              f"(tools.linux-rootfs): {' '.join(no_rootfs)}")
    if not deployed:
        logger.error("no browser demo could be assembled")
        sys.exit(1)


def cmd_docs_demos(args):
    """CLI entry point for 'ove docs-demos'."""
    _run_interruptible("docs-demos", _cmd_docs_demos, args)


def add_subcommands(sub):
    p = sub.add_parser(
        "docs-demos",
        help="Build the browser demos and add them to the built docs site")
    p.add_argument("--site",
                   help="Site directory (default: docs-site/site)")
    return p
