# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Browser demos for the documentation site.

Builds every app for the QEMU board's 'web' variant, keeps those whose
firmware starts, and lays them out in the built site where the pages link
to them: <site>/<app>/ per demo, the emulator binary once in <site>/qemu/,
and the examples.html gallery.  The Pages workflow and 'make docs-serve'
both go through here, so a local preview holds what gets published.

The Linux personality runs in the browser too, one demo per engine at
<site>/linux-<rtos>/.  Its rootfs is built with Buildroot outside this tree:
the local Buildroot output when there is one, else the image manifest.yaml
pins (tools.linux-rootfs).  Without either, those demos are left out.
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

from .allconfigs import _ove_bin, _run_interruptible, _run_pipeline, build_matrix
from .build import LINUX_ROOTFS_ENV
from .download import download_linux_rootfs
from .manifest import load_manifest
from .workspace import Workspace

logger = logging.getLogger("ove")

# What a demo page needs out of the bundle 'ove build' wrote to serve/.
_DEMO_FILES = ("index.html", "app.js", "board.js", "style.css", "board.css",
               "manifest.json", "coi-serviceworker.js", "qemu-wasm.js",
               "firmware.elf")
# ...and out of a Linux personality bundle, whose page is a terminal.
_TERMINAL_FILES = ("index.html", "terminal.js", "coi-serviceworker.js",
                   "firmware.elf", "rootfs.cpio")
_EMULATOR_SCRIPT = "qemu-system-arm.js"
_EMULATOR_SHARED = ("qemu-system-arm.wasm", "SOURCE.txt")
# The demos run FreeRTOS: the LVGL examples do not start on the other
# kernels' qemu-mps2-an500 ports yet.
_RTOS = "freertos"
_LANGS = {"c": "C", "cpp": "C++", "rust": "Rust", "zig": "Zig"}
# The Linux personality demos: (rtos, board variant).  Zephyr's personality
# needs the Cortex-M33 board, whose variant is web-an521.
_PERSONALITY_APP = "linux_interop_minimal"
_PERSONALITY = (("freertos", "web"), ("nuttx", "web"), ("zephyr", "web-an521"))
_RTOS_NAMES = {"freertos": "FreeRTOS", "nuttx": "NuttX", "zephyr": "Zephyr"}


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
    """The rootfs for the personality demos, or None when there is none.

    Returns "" for the local Buildroot output, which the workspaces find on
    their own, else the path of the image manifest.yaml pins.
    """
    local = os.path.join(ws.ove_dir, "..", "buildroot", "output", "images",
                         "rootfs.cpio")
    if os.environ.get(LINUX_ROOTFS_ENV) or os.path.isfile(local):
        return ""
    return download_linux_rootfs(load_manifest(ws.ove_dir), ws.dl_dir)


def build_personality_demos(ws, rootfs):
    """Build the personality bundles; returns one result per engine."""
    env = {LINUX_ROOTFS_ENV: rootfs} if rootfs else None
    ove_bin = _ove_bin(ws)
    results = []
    for rtos, board in _PERSONALITY:
        spec = f"{board}.{rtos}.{_PERSONALITY_APP}"
        workspace = os.path.join(ws.ove_dir, "output", board, rtos,
                                 _PERSONALITY_APP)
        print(f"\n{'=' * 60}\nBuilding {spec}\n{'=' * 60}")
        setup = [ove_bin, "defconfig-fragments", spec, "--no-activate"]
        ok, _elapsed = _run_pipeline(ws.ove_dir, ove_bin, setup, workspace,
                                     env)
        results.append({"name": f"linux-{rtos}", "rtos": rtos, "ok": ok,
                        "serve": os.path.join(workspace, "serve")})
    return results


def remove_demos(site_dir, names):
    """Drop the demos an earlier run left in the site for these apps."""
    for name in names:
        dest = os.path.join(site_dir, name)
        if os.path.isfile(os.path.join(dest, "web.json")):
            shutil.rmtree(dest)


def assemble_demo(serve_dir, site_dir, name):
    """Copy one app's bundle to <site>/<name>/.

    The emulator's script has to sit below the page: its worker threads
    load it again, and the service worker only isolates the page's own
    directory.  The large binary has no such limit, so every demo points
    at the one copy in <site>/qemu/.
    """
    emulator = os.path.realpath(os.path.join(serve_dir, "qemu"))
    dest = os.path.join(site_dir, name)
    os.makedirs(os.path.join(dest, "qemu"), exist_ok=True)
    terminal = os.path.isfile(os.path.join(serve_dir, "terminal.js"))
    for filename in _TERMINAL_FILES if terminal else _DEMO_FILES:
        shutil.copy2(os.path.join(serve_dir, filename), dest)
    shutil.copy2(os.path.join(emulator, _EMULATOR_SCRIPT),
                 os.path.join(dest, "qemu"))

    shared = os.path.join(site_dir, "qemu")
    os.makedirs(shared, exist_ok=True)
    for filename in _EMULATOR_SHARED:
        shutil.copy2(os.path.join(emulator, filename), shared)

    with open(os.path.join(serve_dir, "web.json")) as f:
        config = json.load(f)
    config["wasm"] = "../qemu/qemu-system-arm.wasm"
    with open(os.path.join(dest, "web.json"), "w") as f:
        json.dump(config, f, indent=2)
        f.write("\n")


def write_gallery(site_dir, names, apps, personality=()):
    """Write <site>/examples.html, one card per deployed demo.

    *personality* lists the deployed Linux personality demos by engine.
    """
    cards = []
    for name in sorted(names):
        app = apps.get(name, {})
        title = html.escape(str(app.get("description", name)))
        lang = _LANGS.get(app.get("lang", "c"), "C")
        cards.append(f'    <a class="card" href="{name}/"><h3>{title}</h3>'
                     f"<p>{lang} &middot; <code>{name}</code></p></a>")
    linux = []
    for rtos in personality:
        linux.append(f'    <a class="card" href="linux-{rtos}/"><h3>Linux '
                     f"personality on {_RTOS_NAMES[rtos]}</h3><p>BusyBox shell"
                     f" &middot; <code>{_PERSONALITY_APP}</code></p></a>")
    linux_section = ""
    if linux:
        linux_section = f"""
  <h2>Linux personality</h2>
  <p>A BusyBox userspace on the Linux personality, one demo per kernel.
  Log in as root, password root.</p>
  <div class="grid">
{chr(10).join(linux)}
  </div>"""
    page = f"""<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>oveRTOS Browser Examples</title>
  <style>
    body {{ font-family: sans-serif; max-width: 900px; margin: 40px auto; padding: 0 20px; }}
    h1, h2 {{ color: #333; }}
    .grid {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 16px; }}
    .card {{ border: 1px solid #ddd; border-radius: 8px; padding: 16px; text-decoration: none; color: #333; }}
    .card:hover {{ border-color: #4ecca3; background: #f8f8f8; }}
    .card h3 {{ margin: 0 0 8px 0; font-size: 16px; }}
    .card p {{ margin: 0; font-size: 14px; color: #666; }}
  </style>
</head>
<body>
  <h1>oveRTOS Browser Examples</h1>
  <p>Each demo is the FreeRTOS firmware of that example for the QEMU
  MPS2-AN500, running in QEMU compiled to WebAssembly.</p>
  <div class="grid">
{chr(10).join(cards)}
  </div>{linux_section}
</body>
</html>
"""
    with open(os.path.join(site_dir, "examples.html"), "w") as f:
        f.write(page)


def _cmd_docs_demos(args):
    ws = Workspace()
    site_dir = os.path.abspath(
        args.site or os.path.join(ws.ove_dir, "docs-site", "site"))

    # An app that fails to build only drops its own demo.
    results = build_matrix(ws, "web", _RTOS)

    rootfs = personality_rootfs(ws)
    personality = []
    if rootfs is not None:
        personality = build_personality_demos(ws, rootfs)

    os.makedirs(site_dir, exist_ok=True)
    remove_demos(site_dir, [r["app"] for r in results]
                 + [f"linux-{rtos}" for rtos, _board in _PERSONALITY])

    deployed, unbuilt, broken = [], [], []
    for result in results:
        name = result["app"]
        if result["skipped"]:
            continue
        workspace = os.path.join(ws.ove_dir, "output", "web", _RTOS, name)
        serve_dir = os.path.join(workspace, "serve")
        if not result["ok"] or not os.path.isfile(
                os.path.join(serve_dir, "firmware.elf")):
            unbuilt.append(name)
            continue
        # Only publish a demo whose firmware comes up under the native
        # QEMU: a page for an app that faults at start-up helps no one.
        started, output = _smoke(
            ws.ove_dir, os.path.join(workspace, "images", "firmware.elf"))
        if not started:
            print(f"{name}: {output}")
            broken.append(name)
            continue
        assemble_demo(serve_dir, site_dir, name)
        deployed.append(name)

    linux = []
    for result in personality:
        name = result["name"]
        if not result["ok"] or not os.path.isfile(
                os.path.join(result["serve"], "web.json")):
            unbuilt.append(name)
            continue
        started, output = _smoke_terminal(result["serve"])
        if not started:
            print(f"{name}: {output}")
            broken.append(name)
            continue
        assemble_demo(result["serve"], site_dir, name)
        deployed.append(name)
        linux.append(result["rtos"])

    write_gallery(site_dir, [n for n in deployed if not n.startswith("linux-")],
                  _app_metadata(ws.ove_dir), linux)

    print(f"\n{'=' * 60}")
    print(f"Browser demos in {site_dir}")
    print(f"Deployed ({len(deployed)}): {' '.join(deployed) or 'none'}")
    print(f"Not deployed, build failed: {' '.join(unbuilt) or 'none'}")
    print(f"Not deployed, failed the smoke run: {' '.join(broken) or 'none'}")
    if rootfs is None:
        print("Linux personality demos left out: no Buildroot rootfs, and "
              "manifest.yaml pins none (tools.linux-rootfs)")
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
