# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check public Canvas types without executing native layer operations."""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("deps", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
library = max(args.deps.glob("libove-*.rlib"), key=lambda p: p.stat().st_mtime_ns)
cases = []
for method in ["init_layer", "finish_layer"]:
    body = f"canvas.{method}(core::ptr::null_mut());"
    for safe in [True, False]:
        name = method + ("_safe" if safe else "_unsafe")
        source = "pub fn operation(canvas: ove::lvgl::Canvas) {" + (body if safe else "unsafe {" + body + "}") + "}\n"
        cases.append((name, source, not safe))
cases += [("copied_initializer", """
pub fn copied(parent: ove::lvgl::Obj) -> ove::Result<ove::lvgl::Canvas> {
    let source = [0u8; 64];
    ove::lvgl::Canvas::create(parent).set_buffer(
        ove::lvgl::CanvasBuffer::new(&source, 4, 4, ove::lvgl::ColorFormat::XRGB8888)?
    )
}
""", True), ("copy_view", """
fn copy<T: Copy>() {}
pub fn check() { copy::<ove::lvgl::Canvas>(); }
""", True)]
rows = []
for name, source, expected in cases:
    path = args.output / (name + ".rs")
    path.write_text(source)
    command = ["rustc", "--edition", "2024", "--crate-type", "lib", "--emit", "metadata",
               "--crate-name", name, "-L", "dependency=" + str(args.deps),
               "--extern", "ove=" + str(library), str(path), "-o", str(args.output / (name + ".rmeta"))]
    result = subprocess.run(command, capture_output=True, text=True, env=dict(os.environ), timeout=30, check=False)
    (args.output / (name + ".log")).write_text(result.stdout + result.stderr)
    passed = result.returncode == 0 if expected else result.returncode == 1 and "E0133" in result.stderr
    rows.append({"case": name, "must_compile": expected, "rc": result.returncode, "passed": passed})
    if not passed:
        raise SystemExit(f"Canvas type case failed: {name}")
(args.output / "results.json").write_text(json.dumps({"library": str(library),
    "sha256": hashlib.sha256(library.read_bytes()).hexdigest(), "cases": rows}, indent=2) + "\n")
print(f"Canvas type checks: {len(rows)} passed")
