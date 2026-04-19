# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""App-level asset pipeline.

- Image pipeline: PNG -> lv_image_dsc_t C arrays (+ aggregate .h / .rs)
- ``app_sources.cmake`` emission from ``app.yaml``
- Platform/board-specific source resolution
"""

import json
import os
import subprocess
import sys

try:
    from jinja2 import Environment, FileSystemLoader
except ImportError:
    Environment = None

try:
    import yaml
except ImportError:
    yaml = None

try:
    import jsonschema
except ImportError:
    jsonschema = None


# ── lv_conf.h discovery ──────────────────────────────────────────────────

def find_board_lv_conf(ws):
    """Locate the board's lv_conf.h for LV_COLOR_DEPTH discovery.

    Returns the absolute path or None if not found. Searches the same
    candidate locations that `config/cmake/ove_rust.cmake` uses.
    """
    candidates = []
    if ws.rtos == "posix":
        candidates.append(os.path.join(
            ws.ove_dir, "boards", "host", "posix", "lv_conf.h"))
        candidates.append(os.path.join(
            ws.ove_dir, "boards", "wasm", "posix", "lv_conf.h"))
    board_dir = getattr(ws, "board_dir", None) or (
        os.path.join(ws.ove_dir, "boards", ws.board_name or "")
        if ws.board_name else None)
    if board_dir:
        rtos = ws.rtos or "posix"
        for sub in (rtos, f"{rtos}/inc", "inc", "freertos/inc"):
            candidates.append(os.path.join(board_dir, sub, "lv_conf.h"))
        candidates.append(os.path.join(board_dir, "lv_conf.h"))

    for c in candidates:
        if c and os.path.isfile(c):
            return c
    return None


def _parse_lv_color_depth(lv_conf_path):
    """Parse `#define LV_COLOR_DEPTH <n>` from the given header."""
    if not lv_conf_path or not os.path.isfile(lv_conf_path):
        return None
    try:
        with open(lv_conf_path) as f:
            for line in f:
                line = line.strip()
                if line.startswith("#define") and "LV_COLOR_DEPTH" in line:
                    parts = line.split()
                    if len(parts) >= 3:
                        try:
                            return int(parts[2])
                        except ValueError:
                            pass
    except OSError:
        pass
    return None


def _pick_image_format(color_depth):
    """Map LVGL color depth to a converter format name."""
    if color_depth == 16:
        return "rgb565"
    if color_depth == 24:
        return "rgb888"
    # 32-bit default — use XRGB8888 for opaque images and rely on
    # runtime upcast for transparency if the user supplies a PNG with
    # alpha. (argb8888 would double RAM cost on everything.)
    return "xrgb8888"


def _image_symbol_name(path):
    """Derive a C symbol name from an image path."""
    base = os.path.basename(path)
    name = os.path.splitext(base)[0]
    safe = "".join(c if c.isalnum() else "_" for c in name)
    if safe and safe[0].isdigit():
        safe = "_" + safe
    return safe


def _process_app_images(ws, app, app_dir):
    """Convert every image listed in app.yaml to a C array.

    Returns a list of absolute paths to the generated .c files (to be
    emitted as APP_GENERATED_SOURCES in app_sources.cmake).
    """
    images = app.get("images") or []
    if not images:
        return []

    lv_conf_path = find_board_lv_conf(ws)
    color_depth = _parse_lv_color_depth(lv_conf_path) or 32
    fmt = _pick_image_format(color_depth)

    gen_images_dir = os.path.join(ws.gen_dir, "generated_images")
    os.makedirs(gen_images_dir, exist_ok=True)

    convert_script = os.path.join(
        ws.ove_dir, "scripts", "lvgl_img_conv.py")
    if not os.path.isfile(convert_script):
        print(f"Warning: image converter missing at {convert_script}")
        return []

    print("=== Generating image assets ===")
    print(f"  Format: {fmt} (LV_COLOR_DEPTH={color_depth})")

    generated_c = []
    h_symbols = []
    for rel_path in images:
        abs_path = rel_path if os.path.isabs(rel_path) else \
            os.path.join(app_dir, rel_path)
        if not os.path.isfile(abs_path):
            print(f"  Error: image not found: {abs_path}")
            sys.exit(1)
        sym = _image_symbol_name(rel_path)
        result = subprocess.run(
            [sys.executable, convert_script,
             "--input", abs_path,
             "--output-dir", gen_images_dir,
             "--format", fmt,
             "--name", sym],
            capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  Error converting {abs_path}:")
            print(result.stderr.strip())
            sys.exit(1)
        if result.stdout:
            print(result.stdout.rstrip())
        generated_c.append(os.path.join(gen_images_dir, f"{sym}.c"))
        h_symbols.append(sym)

    # Emit an aggregate header users can `#include "generated_images/lvgl_images.h"`.
    agg_h = os.path.join(gen_images_dir, "lvgl_images.h")
    with open(agg_h, "w") as f:
        f.write("/* Auto-generated by ove configure — do not edit. */\n")
        f.write("#ifndef OVE_LVGL_IMAGES_H\n")
        f.write("#define OVE_LVGL_IMAGES_H\n")
        for sym in h_symbols:
            f.write(f'#include "{sym}.h"\n')
        f.write("#endif\n")

    # Emit a Rust glue module with extern declarations so Rust apps can
    # pull the images in via `include!(...)` from their lib.rs. Outer
    # attributes only — `include!` into a module rejects inner attrs.
    #
    # The `unsafe extern "C"` block is unavoidable (Rust 2024 syntax for
    # foreign static declarations). Apps that `#![deny(unsafe_code)]`
    # should wrap the `include!` in `#[allow(unsafe_code)] mod images { ... }`.
    agg_rs = os.path.join(gen_images_dir, "lvgl_images.rs")
    with open(agg_rs, "w") as f:
        f.write("// Auto-generated by ove configure — do not edit.\n\n")
        f.write("use ove::lvgl::ImageDsc;\n\n")
        f.write("unsafe extern \"C\" {\n")
        for sym in h_symbols:
            f.write(f"    #[allow(non_upper_case_globals)]\n")
            f.write(f"    pub static {sym}: ImageDsc;\n")
        f.write("}\n")

    print(f"  Generated: {gen_images_dir}/ ({len(images)} images)")
    return generated_c


# ── app.yaml -> app_sources.cmake ────────────────────────────────────────

def generate_app_sources(ws):
    """Generate app_sources.cmake from app.yaml."""
    app_dir = ws.app_dir
    if not app_dir:
        return

    app_yaml_path = os.path.join(app_dir, "app.yaml")
    if not os.path.isfile(app_yaml_path):
        return

    if yaml is None:
        print("Warning: pyyaml not installed, skipping app source generation")
        return

    print("=== Generating app build files ===")
    with open(app_yaml_path) as f:
        app = yaml.safe_load(f)

    # Validate against schema
    schema_path = os.path.join(ws.ove_dir, "config", "schemas",
                               "app_schema.json")
    if jsonschema and os.path.isfile(schema_path):
        with open(schema_path) as f:
            schema = json.load(f)
        try:
            jsonschema.validate(instance=app, schema=schema)
        except jsonschema.ValidationError as e:
            print(f"App YAML validation error: {e.message}")
            sys.exit(1)

    # Resolve platform/board-specific sources
    platform_sources = app.pop("platform_sources", None)
    board_sources = app.pop("board_sources", None)
    if platform_sources or board_sources:
        rtos = ws.rtos or "posix"
        board = ws.board_name or ""
        # Board-specific sources take priority over platform-specific
        if board_sources and board in board_sources:
            extra = board_sources[board]
        elif platform_sources:
            if rtos in platform_sources:
                extra = platform_sources[rtos]
            elif "default" in platform_sources:
                extra = platform_sources["default"]
            else:
                extra = []
        else:
            extra = []
        app.setdefault("sources", [])
        app["sources"] = list(app["sources"]) + extra

    # Normalize defaults
    if app.get("lang") == "rust":
        app.setdefault("rust", {})
        app["rust"].setdefault("crate_dir", ".")
    elif app.get("lang") == "zig":
        app.setdefault("zig", {})
        app["zig"].setdefault("src_dir", "src")

    # Process images → generated lv_image_dsc_t C arrays
    generated_image_sources = _process_app_images(ws, app, app_dir)
    if generated_image_sources:
        app["generated_image_sources"] = generated_image_sources

    if Environment is None:
        print("Warning: jinja2 not installed, skipping app source generation")
        return

    templates_dir = os.path.join(ws.ove_dir, "config", "templates")
    env = Environment(
        loader=FileSystemLoader(templates_dir),
        keep_trailing_newline=True,
        trim_blocks=True,
        lstrip_blocks=True,
    )

    # Render app_sources.cmake
    cmake_template = env.get_template("app_sources.cmake.j2")
    cmake_content = cmake_template.render(app=app)
    cmake_path = os.path.join(ws.gen_dir, "app_sources.cmake")
    with open(cmake_path, "w") as f:
        f.write(cmake_content)
    print(f"  Generated: {cmake_path}")
