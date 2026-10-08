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
"""

import html
import json
import logging
import os
import shutil
import subprocess
import sys

from .allconfigs import _run_interruptible, build_matrix
from .workspace import Workspace

logger = logging.getLogger("ove")

# What a demo page needs out of the bundle 'ove build' wrote to serve/.
_DEMO_FILES = ("index.html", "app.js", "style.css", "coi-serviceworker.js",
               "qemu-wasm.js", "firmware.elf")
_EMULATOR_SCRIPT = "qemu-system-arm.js"
_EMULATOR_SHARED = ("qemu-system-arm.wasm", "SOURCE.txt")
# The demos run FreeRTOS: the LVGL examples do not start on the other
# kernels' qemu-mps2-an500 ports yet.
_RTOS = "freertos"
_LANGS = {"c": "C", "cpp": "C++", "rust": "Rust", "zig": "Zig"}


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
    for filename in _DEMO_FILES:
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


def write_gallery(site_dir, names, apps):
    """Write <site>/examples.html, one card per deployed demo."""
    cards = []
    for name in sorted(names):
        app = apps.get(name, {})
        title = html.escape(str(app.get("description", name)))
        lang = _LANGS.get(app.get("lang", "c"), "C")
        cards.append(f'    <a class="card" href="{name}/"><h3>{title}</h3>'
                     f"<p>{lang} &middot; <code>{name}</code></p></a>")
    page = f"""<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>oveRTOS Browser Examples</title>
  <style>
    body {{ font-family: sans-serif; max-width: 900px; margin: 40px auto; padding: 0 20px; }}
    h1 {{ color: #333; }}
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
  </div>
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

    os.makedirs(site_dir, exist_ok=True)
    remove_demos(site_dir, [r["app"] for r in results])

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

    write_gallery(site_dir, deployed, _app_metadata(ws.ove_dir))

    print(f"\n{'=' * 60}")
    print(f"Browser demos in {site_dir}")
    print(f"Deployed ({len(deployed)}): {' '.join(deployed) or 'none'}")
    print(f"Not deployed, build failed: {' '.join(unbuilt) or 'none'}")
    print(f"Not deployed, failed the smoke run: {' '.join(broken) or 'none'}")
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
