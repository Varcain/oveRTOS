# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify the shipped keyword models on QEMU Cortex-M7 against fixed goldens."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
from types import SimpleNamespace

SEEDS = (0, 17)
RECIPE = "pcm[i,frame,seed] = 100 + ((i + frame + seed) % 31); reset per seed"
MODEL_NAMES = ("audio_preprocessor_int8", "micro_speech_quantized")
CAPACITY = 32768


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def expected_records(outputs, zero_heap):
    """Yield (mode, record) for the complete guest protocol in its exact order."""
    for mode in ("init",) if zero_heap else ("init", "create"):
        yield mode, f"IT_BEGIN {mode}"
        for seed in SEEDS:
            golden = outputs[str(seed)]
            require(len(golden["features"]) == 49, "golden frame count")
            for frame, raw in enumerate(golden["features"]):
                yield mode, f"IT_FRAME {seed} {frame} {raw}"
            yield mode, f"IT_ARENA {seed} preprocessor"
            yield mode, f"IT_SCORES {seed} {golden['scores']}"
            yield mode, f"IT_ARENA {seed} classifier"
        yield mode, "IT_END"
    yield mode, "IT_RESULT 0"


def check_output(text, outputs, *, zero_heap):
    """Compare every guest record with the goldens; return the arena records."""
    lines = text.replace("\r\n", "\n").splitlines()
    expected = list(expected_records(outputs, zero_heap))
    require(len(lines) == len(expected), f"expected {len(expected)} records, got {len(lines)}")
    arenas = []
    for number, (line, (mode, want)) in enumerate(zip(lines, expected), 1):
        if want.startswith("IT_ARENA"):
            # The guest already requires 16 bytes of headroom; padding is front alignment.
            match = re.fullmatch(re.escape(want) + f" {CAPACITY} ([0-9]+) ([0-9]+)", line)
            require(match and int(match[2]) < (1 if mode == "init" else 16), f"line {number}: {line}")
            arenas.append(f"{mode} {line}")
        else:
            require(line == want, f"line {number}: want {want!r} got {line!r}")
    return arenas


def capture(text):
    """Take candidate goldens from the first session; check_output then verifies all of it."""
    records = [line.split() for line in text.splitlines()]
    return {str(seed): {"features": [r[3] for r in records if r[:2] == ["IT_FRAME", str(seed)]][:49],
                        "scores": next((r[2] for r in records if r[:2] == ["IT_SCORES", str(seed)]), "")}
            for seed in SEEDS}


def git_output(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True, timeout=30).strip()


def check_image(root, build, provider):
    """Use the toolchain's nm to check the linked models and kernel provider."""
    nm = re.search(r"^CMAKE_NM:FILEPATH=(.*)$", (build / "CMakeCache.txt").read_text(), re.M).group(1)
    elf = Path((build / "qualifier-elf.txt").read_text().strip())
    symbols = {f[3]: (int(f[0], 16), int(f[1], 16)) for f in map(str.split, subprocess.check_output(
        [nm, "-S", str(elf)], text=True, timeout=30).splitlines()) if len(f) == 4}
    for name in MODEL_NAMES:
        address, size = symbols.get("g_" + name + "_model_data", (1, 0))
        model = root / "models" / "keyword" / (name + ".tflite")
        require(address % 16 == 0 and size == model.stat().st_size, "linked model array " + name)
    # CMSIS-NN alone defines this kernel; the reference build must not link it.
    require(("arm_nn_vec_mat_mult_t_s8" in symbols) == (provider == "cmsis"), "linked NN provider")
    return nm.removesuffix("nm") + "g++", elf


def run_qemu(qemu, elf, workspace):
    from .test import _run_owned_test_process

    log, diagnostics = workspace / "qualification.log", workspace / "qualification-qemu.log"
    log.unlink(missing_ok=True)
    command = [qemu, "-M", "mps2-an500", "-nographic", "-monitor", "none", "-nic", "none",
               "-chardev", "file,id=qualification,path=" + str(log).replace(",", ",,"),
               "-semihosting-config", "enable=on,target=native,chardev=qualification",
               "-kernel", str(elf)]
    with diagnostics.open("w") as output:
        rc = _run_owned_test_process(command, env=dict(os.environ), timeout=45,
                                     stdout=output, cwd=str(workspace))
    require(rc == 0 and log.is_file(), "QEMU qualification failed; see " + str(diagnostics))
    # The pinned board's disconnected NIC warning is the only accepted diagnostic.
    expected = Path(qemu).name + ": warning: nic lan9118.0 has no peer"
    require(diagnostics.read_text().splitlines() in ([], [expected]), "unexpected QEMU diagnostics")
    return log.read_text()


def build_profile(root, app, workspace, nn, core):
    from .build import build_freertos
    from .configure import configure_all
    from .download import download_all
    from .kconfig import cmd_defconfig_fragments
    from .workspace import Workspace

    cmd_defconfig_fragments(SimpleNamespace(spec=f"qemu-mps2-an500.freertos.{app}", no_activate=True))
    workspace.mkdir(parents=True, exist_ok=True)
    if nn:  # Keep opt-in CMSIS caches physically separate from the reference build.
        (workspace / ".config").write_bytes((workspace.with_name(app) / ".config").read_bytes())
    os.environ["OVE_WORKSPACE_DIR"] = str(workspace)
    ws = Workspace(str(root))
    ws.ensure_dirs()
    download_all(ws)  # Raises SystemExit on a failed dependency.
    configure_all(ws)
    extra = ["-DOVE_QUALIFY_CMSIS_NN=" + (str(nn) if nn else ""),
             "-DOVE_QUALIFY_CMSIS_CORE=" + (str(core) if core else ""), "-DOVE_NO_CCACHE=ON"]
    build_freertos(ws, cmake_source=str(root / "tests" / "infer_target"), extra_cmake_args=extra)


def main():
    from .manifest import load_manifest, get_component
    from .workspace import find_ove_dir

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--regenerate-goldens", action="store_true")
    parser.add_argument("--cmsis-nn-dir", type=Path)
    parser.add_argument("--cmsis-core-include", type=Path)
    args = parser.parse_args()
    nn, core = (path and path.resolve() for path in (args.cmsis_nn_dir, args.cmsis_core_include))
    provider = "cmsis" if nn else "reference"
    require(bool(nn) == bool(core), "supply both CMSIS paths")
    require(not args.regenerate_goldens or provider == "reference", "only reference can produce goldens")
    # The ove test runner stops this process with SIGTERM; unwind so QEMU is killed too.
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    root = Path(find_ove_dir()).resolve()
    pin = get_component(load_manifest(str(root)), "libraries", "tflm", "version")
    golden_path = root / "tests" / "infer_target" / "goldens.json"
    golden = json.loads(golden_path.read_text())
    models = {name: sha(root / "models" / "keyword" / (name + ".tflite")) for name in MODEL_NAMES}
    require(golden["models"] == models and golden["tflm"] == pin and golden["recipe"] == RECIPE and
            golden["seeds"] == list(SEEDS), "model/input/SDK drift requires requalification")
    qemu = shutil.which(os.environ.get("OVE_QEMU_ARM", "qemu-system-arm"))
    require(qemu, "qemu-system-arm is required")
    captured = None
    for zero_heap in (False, True):
        app = "infer_target_zh" if zero_heap else "infer_target_heap"
        workspace = root / "output" / "qemu-mps2-an500" / "freertos" / (app + ("-cmsis" if nn else ""))
        build_profile(root, app, workspace, nn, core)
        sdk = root / "dl" / "tflite-micro"
        require(git_output(sdk, "rev-parse", "HEAD") == pin and not git_output(sdk, "status", "--porcelain"),
                "TFLM must be clean at the manifest pin")
        compiler, elf = check_image(root, workspace / "build" / "firmware", provider)
        text = run_qemu(qemu, elf, workspace)
        if args.regenerate_goldens:
            captured = captured or capture(text)
        else:
            require(golden.get("generation", {}).get("provider") == "reference",
                    "reference-generated goldens are required")
        for arena in check_output(text, captured or golden["outputs"], zero_heap=zero_heap):
            print(f"{app}/{provider}/{arena}")
    if args.regenerate_goldens:
        version = subprocess.check_output([compiler, "-dumpfullversion"], text=True, timeout=10).strip()
        golden.update(outputs=captured, generation={
            "provider": provider, "tflm": pin, "compiler": Path(compiler).name,
            "compiler_sha256": sha(compiler), "compiler_version": version})
        golden_path.write_text(json.dumps(golden, indent=2) + "\n")


if __name__ == "__main__":
    main()
