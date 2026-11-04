# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Finite ARM qualification of the shipped, trusted keyword models."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import struct
import subprocess
from types import SimpleNamespace
from unittest.mock import patch

SEEDS = (0, 17)
RECIPE = "pcm[i,frame,seed] = 100 + ((i + frame + seed) % 31); reset per seed"
MODEL_NAMES = ("audio_preprocessor_int8", "micro_speech_quantized")
CAPACITY = 32768


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class ModelBytes:
    """Read only the metadata needed to qualify these trusted build assets."""

    def __init__(self, data):
        self.data = data
        require(data[4:8] == b"TFL3", "missing TFLite identifier")
        self.root = self.num(0)
        require(self.value(self.root, 0) == 3, "unexpected model schema")

    def num(self, offset, fmt="I"):
        size = struct.calcsize("<" + fmt)
        require(0 <= offset <= len(self.data) - size, "truncated metadata")
        return struct.unpack_from("<" + fmt, self.data, offset)[0]

    def field(self, table, index):
        vtable = table - self.num(table, "i")
        length = self.num(vtable, "H")
        require(length >= 4 and length % 2 == 0, "invalid vtable")
        delta = self.num(vtable + 4 + 2 * index, "H") if 4 + 2 * index < length else 0
        return table + delta if delta else None

    def value(self, table, index, fmt="I", default=0):
        offset = self.field(table, index)
        return default if offset is None else self.num(offset, fmt)

    def target(self, offset):
        return offset + self.num(offset)

    def vector(self, table, index, fmt=None):
        offset = self.field(table, index)
        if offset is None:
            return []
        start = self.target(offset)
        count = self.num(start)
        size = 4 if fmt is None else struct.calcsize("<" + fmt)
        require(count <= (len(self.data) - start - 4) // size, "truncated vector")
        if fmt is None:
            return [self.target(start + 4 + 4 * i) for i in range(count)]
        return [self.num(start + 4 + size * i, fmt) for i in range(count)]

    def blob(self, table, index):
        return bytes(self.vector(table, index, "B"))


def integer_map(raw):
    """Decode the sorted integer FlexBuffers map used by SignalRfft."""
    def number(offset, width, signed=False):
        require(width in (1, 2, 4, 8) and 0 <= offset <= len(raw) - width,
                "invalid FlexBuffers range")
        return int.from_bytes(raw[offset:offset + width], "little", signed=signed)

    require(len(raw) >= 3 and raw[-2] >> 2 == 9, "expected FlexBuffers map")
    parent_width = raw[-1]
    width = 1 << (raw[-2] & 3)
    root = len(raw) - 2 - parent_width
    start = root - number(root, parent_width)
    count = number(start - width, width)
    key_parent = start - 3 * width
    keys = key_parent - number(key_parent, width)
    key_width = number(start - 2 * width, width)
    require(number(keys - key_width, key_width) == count, "map key count")
    require(count <= len(raw), "map length")
    result = {}
    for i in range(count):
        key_slot = keys + i * key_width
        key_start = key_slot - number(key_slot, key_width)
        require(0 <= key_start < len(raw), "map key range")
        key_end = raw.find(b"\0", key_start)
        require(key_end >= 0, "unterminated map key")
        key = raw[key_start:key_end].decode("ascii")
        kind = number(start + count * width + i, 1) >> 2
        require(kind in (1, 2) and key not in result, "expected distinct integer options")
        result[key] = number(start + i * width, width, kind == 1)
    require(list(result) == sorted(result), "unsorted map keys")
    return result


def radix_235(length):
    if type(length) is not int or length <= 0:
        return False
    for factor in (2, 3, 5):
        while length % factor == 0:
            length //= factor
    return length == 1


def rfft_metadata(data, expected_count):
    model = ModelBytes(data)
    codes = [model.blob(code, 1).decode("ascii")
             for code in model.vector(model.root, 1)]
    found = []
    for graph in model.vector(model.root, 2):
        tensors = model.vector(graph, 0)
        for op in model.vector(graph, 3):
            code = model.value(op, 0)
            require(code < len(codes), "operator-code index")
            if codes[code] != "SignalRfft":
                continue
            options = integer_map(model.blob(op, 5))
            require(list(options) == ["T", "fft_length"] and options["T"] == 7,
                    "unexpected RFFT options/type")
            length = options["fft_length"]
            require(radix_235(length) and length % 2 == 0, "unsupported RFFT length")
            inputs, outputs = model.vector(op, 1, "i"), model.vector(op, 2, "i")
            require(len(inputs) == len(outputs) == 1, "RFFT tensor arity")
            require(all(0 <= i < len(tensors) for i in inputs + outputs), "RFFT tensor index")
            source, dest = tensors[inputs[0]], tensors[outputs[0]]
            source_dims, dest_dims = model.vector(source, 0, "i"), model.vector(dest, 0, "i")
            require(model.value(source, 1, "b") == model.value(dest, 1, "b") == 7,
                    "RFFT tensor type")
            require(source_dims and len(source_dims) == len(dest_dims) and
                    source_dims[:-1] == dest_dims[:-1] and
                    0 < source_dims[-1] <= length and dest_dims[-1] == length + 2,
                    "RFFT dimensions")
            found.append(length)
    require(len(found) == expected_count, "RFFT operator count")
    return found


def headroom(capacity, used, padding):
    require(all(type(v) is int and 0 <= v <= 0x7fffffff for v in (capacity, used, padding)),
            "invalid arena measurement")
    require(0 < used <= capacity and capacity - used >= 16 and padding < 16,
            "arena requires at least 16 bytes headroom")


def parse_output(text, *, zero_heap):
    """Require the entire finite protocol, including its exact record order."""
    lines = text.replace("\r\n", "\n").splitlines()
    position = 0

    def take(prefix, count):
        nonlocal position
        require(position < len(lines), "truncated qualifier output")
        fields = lines[position].split()
        position += 1
        require(len(fields) == count and fields[0] == prefix,
                f"unexpected record at line {position}")
        return fields[1:]

    results = {}
    for mode in (("init",) if zero_heap else ("init", "create")):
        require(take("IT_BEGIN", 2) == [mode], "session mode/order")
        results[mode] = {}
        for seed in SEEDS:
            frames = []
            for frame in range(49):
                actual_seed, actual_frame, raw = take("IT_FRAME", 4)
                require((actual_seed, actual_frame) == (str(seed), str(frame)), "frame order")
                require(re.fullmatch("[0-9a-f]{80}", raw), "feature byte count")
                frames.append(raw)
            arenas = {}

            def arena_record(name):
                actual_seed, actual_name, cap, used, padding = take("IT_ARENA", 6)
                require((actual_seed, actual_name) == (str(seed), name), "arena order")
                cap, used, padding = int(cap), int(used), int(padding)
                require(cap == CAPACITY, "configured arena changed")
                headroom(cap, used, padding)
                if mode == "init":
                    require(padding == 0, "static arena alignment")
                arenas[name] = {"capacity": cap, "used": used, "padding": padding}

            arena_record("preprocessor")
            actual_seed, scores = take("IT_SCORES", 3)
            require(actual_seed == str(seed) and re.fullmatch("[0-9a-f]{8}", scores),
                    "score order/byte count")
            arena_record("classifier")
            results[mode][str(seed)] = {"features": frames, "scores": scores, "arenas": arenas}
        take("IT_END", 1)
    require(take("IT_RESULT", 2) == ["0"], "qualification failed")
    require(position == len(lines), "extra output after completion")
    return results


def compare_goldens(results, outputs):
    require(set(outputs) == {str(s) for s in SEEDS}, "golden seeds")
    for mode, cases in results.items():
        for seed in map(str, SEEDS):
            expected, actual = outputs[seed], cases[seed]
            require(len(expected["features"]) == 49, "golden frame count")
            for frame in range(49):
                want, got = expected["features"][frame], actual["features"][frame]
                require(re.fullmatch("[0-9a-f]{80}", want), "golden feature byte count")
                for index in range(40):
                    require(want[2 * index:2 * index + 2] == got[2 * index:2 * index + 2],
                            f"{mode} seed={seed} frame={frame} feature={index}")
            want, got = expected["scores"], actual["scores"]
            require(re.fullmatch("[0-9a-f]{8}", want), "golden score byte count")
            for index in range(4):
                require(want[2 * index:2 * index + 2] == got[2 * index:2 * index + 2],
                        f"{mode} seed={seed} score={index}")


def selected_provider(rows, provider):
    paths = [Path(row["file"]).as_posix() for row in rows]
    cmsis = [p for p in paths if "/micro/kernels/cmsis_nn/" in p]
    require(provider in ("reference", "cmsis"), "unknown provider")
    require(bool(cmsis) == (provider == "cmsis"), "compiled provider disagrees with requested provider")
    for name in ("depthwise_conv.cc", "fully_connected.cc", "softmax.cc"):
        required = "/micro/kernels/" + ("cmsis_nn/" if provider == "cmsis" else "") + name
        require(any(p.endswith(required) for p in paths), "missing used provider source: " + name)
    core = [r for r in rows if r["file"].endswith("/micro_allocator.cc")]
    require(len(core) == 1, "missing unique micro allocator")
    command = shlex.split(core[0]["command"])
    require("-mcpu=cortex-m7" in command and "-DTF_LITE_STATIC_MEMORY" in command,
            "qualification requires the actual Cortex-M7 static-memory build")
    return command[0]


def model_manifest(root):
    paths = {name: root / "models" / "keyword" / (name + ".tflite") for name in MODEL_NAMES}
    for index, name in enumerate(MODEL_NAMES):
        rfft_metadata(paths[name].read_bytes(), 1 if index == 0 else 0)
    return {name: sha(path) for name, path in paths.items()}


def app_budgets(root):
    budgets = {}
    for lang in ("c", "cpp", "rust", "zig"):
        for mode in ("heap", "zeroheap"):
            app = root / "apps" / lang / mode / "example_keyword_live"
            source = {"c": "src/app.c", "cpp": "src/app.cpp", "rust": "src/lib.rs",
                      "zig": "src/main.zig"}[lang]
            path = app / source
            pattern = r"(?:ARENA_SIZE|arena_size)(?:\s*:\s*(?:usize|u32))?\s*(?:=|\s)\s*(\d+)\s*;?"
            values = re.findall(pattern, path.read_text())
            require(len(values) == 1 and int(values[0]) == CAPACITY,
                    "review changed app arena: " + str(path))
            budgets[str(path.relative_to(root))] = {"capacity": int(values[0]), "sha256": sha(path)}
    return budgets


def shipped_providers(root):
    enabled = []
    for base in (root / "apps", root / "boards", root / "config/fragments", root / ".github"):
        for path in base.rglob("*"):
            if not path.is_file():
                continue
            text = path.read_bytes().decode("utf-8", errors="replace")
            if re.search(r"CONFIG_OVE_INFER_CMSIS_NN\s*=\s*(?:y|1|ON)\b", text):
                enabled.append(str(path.relative_to(root)))
    require(not enabled, "shipped CMSIS configuration requires extending the mandatory lane: " +
            ", ".join(enabled))
    return ["reference"]


def git_output(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True, timeout=30).strip()


def sdk_check(root, pin):
    sdk = root / "dl" / "tflite-micro"
    require(git_output(sdk, "rev-parse", "HEAD") == pin and
            not git_output(sdk, "status", "--porcelain"), "TFLM must be clean at the manifest pin")


def sdk_sources(root, rows, provider):
    sdk = (root / "dl" / "tflite-micro").resolve()
    selected = {}
    for row in rows:
        path = Path(row["file"]).resolve()
        candidate = (path.is_relative_to(sdk) or "/tensorflow/lite/" in str(path) or
                     "/tflite-micro/" in row["file"])
        if candidate:
            require(path.is_relative_to(sdk), "SDK source is outside the clean pinned tree")
            selected[str(path.relative_to(sdk))] = sha(path)
    if provider == "reference":
        require(len(selected) == 213, "reference SDK source selection changed")
    return selected


def model_arrays(root, elf):
    """Bind linked ARM32 symbols and their bytes to the original model assets."""
    data = elf.read_bytes()
    require(data[:6] == b"\x7fELF\x01\x01" and struct.unpack_from("<H", data, 18)[0] == 40,
            "qualification requires a little-endian ARM32 ELF")
    section_offset = struct.unpack_from("<I", data, 32)[0]
    section_size, count = struct.unpack_from("<HH", data, 46)
    require(section_size == 40, "unexpected ELF section format")
    sections = [struct.unpack_from("<10I", data, section_offset + i * section_size)
                for i in range(count)]
    found = {}
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        require(section[9] == 16, "unexpected ELF symbol format")
        for offset in range(section[4], section[4] + section[5], 16):
            name_offset, address, size, _, _, index = struct.unpack_from("<IIIBBH", data, offset)
            end = names.find(b"\0", name_offset)
            name = names[name_offset:end].decode("ascii")
            for model in MODEL_NAMES:
                if name != "g_" + model + "_model_data":
                    continue
                require(model not in found and 0 < index < len(sections), "model symbol uniqueness")
                original = (root / "models/keyword" / (model + ".tflite")).read_bytes()
                owner = sections[index]
                file_offset = owner[4] + address - owner[3]
                require(owner[1] != 8 and address % 16 == 0 and size == len(original) and
                        owner[3] <= address and address + size <= owner[3] + owner[5] and
                        data[file_offset:file_offset + size] == original, "linked model bytes/alignment")
                found[model] = {"address": address, "bytes": size,
                                "sha256": hashlib.sha256(original).hexdigest()}
    require(set(found) == set(MODEL_NAMES), "missing linked original model arrays")
    return found


def input_lease(root, workspace, rows):
    paths = set()
    for row in rows:
        paths.add(Path(row["file"]).resolve())
        paths.add(Path(shlex.split(row["command"])[0]).resolve())
    for dep in (workspace / "build").rglob("*.d"):
        raw = dep.read_text().replace("\\\n", " ")
        for name in shlex.split(raw.split(":", 1)[1]):
            path = Path(name)
            if not path.is_absolute():
                path = workspace / "build" / "firmware" / name
            if path.is_file():
                paths.add(path.resolve())
    for base in (workspace / "generated", workspace / "build"):
        paths.update(p.resolve() for p in base.rglob("*") if p.is_file())
    for path in (root / "tests" / "infer_target").rglob("*"):
        if path.is_file() and path.suffix in (".c", ".h", ".txt", ".py", ".yaml"):
            paths.add(path)
    paths.update((root / "config/ove-cli/ove").rglob("*.py"))
    paths.add(Path(__file__).resolve())
    paths.add(root / "models" / "convert.py")
    return {str(p): sha(p) for p in sorted(paths)}


def check_lease(lease):
    for name, expected in lease.items():
        require(Path(name).is_file() and sha(name) == expected, "qualification input changed: " + name)


def cmsis_dsp_evidence(compiler, elf):
    objdump = compiler.removesuffix("g++") + "objdump"
    text = subprocess.check_output([objdump, "-d", str(elf)], text=True, timeout=30)
    bodies = {}
    for name in ("arm_fully_connected_s8", "arm_nn_vec_mat_mult_t_s8"):
        match = re.search(r"<" + name + r">:\n(.*?)(?:\n\n|\Z)", text, re.S)
        require(match, "missing linked CMSIS function " + name)
        bodies[name] = match.group(1)
    require("arm_nn_vec_mat_mult_t_s8" in bodies["arm_fully_connected_s8"] and
            re.search(r"\bsmlad\b", bodies["arm_nn_vec_mat_mult_t_s8"]),
            "original classifier must reach the target CMSIS DSP path")
    return {name: hashlib.sha256(body.encode()).hexdigest() for name, body in bodies.items()}


def qemu_diagnostics(text, qemu):
    """Accept only the pinned board's harmless disconnected-NIC diagnostic."""
    lines = text.splitlines()
    expected = Path(qemu).name + ": warning: nic lan9118.0 has no peer"
    require(not lines or lines == [expected], "unexpected QEMU diagnostics")


def run_qemu(qemu, elf, workspace):
    from .test import _run_owned_test_process

    log = workspace / "qualification.log"
    diagnostics = workspace / "qualification-qemu.log"
    log.unlink(missing_ok=True)
    command = [qemu, "-M", "mps2-an500", "-nographic", "-monitor", "none", "-nic", "none",
               "-chardev", "file,id=qualification,path=" + str(log).replace(",", ",,"),
               "-semihosting-config", "enable=on,target=native,chardev=qualification",
               "-kernel", str(elf)]

    def interrupt(_signal, _frame):
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, interrupt)
    try:
        with diagnostics.open("w") as output:
            rc = _run_owned_test_process(command, env=dict(os.environ), timeout=45,
                                         stdout=output, cwd=str(workspace))
    finally:
        signal.signal(signal.SIGTERM, previous)
    require(rc == 0, "QEMU qualification failed; see " + str(diagnostics))
    qemu_diagnostics(diagnostics.read_text(), qemu)
    require(log.is_file(), "QEMU produced no guest output")
    return log.read_text()


def build_profile(root, zero_heap, nn, core):
    from .build import build_freertos
    from .configure import configure_all
    from .download import download_all
    from .kconfig import cmd_defconfig_fragments
    from .workspace import Workspace

    app = "infer_target_zh" if zero_heap else "infer_target_heap"
    # Keep opt-in caches physically separate from the reference selection.
    suffix = "-cmsis" if nn else ""
    cmd_defconfig_fragments(SimpleNamespace(spec=f"qemu-mps2-an500.freertos.{app}", no_activate=True))
    normal = root / "output" / "qemu-mps2-an500" / "freertos" / app
    workspace = normal.with_name(app + suffix)
    workspace.mkdir(parents=True, exist_ok=True)
    if suffix:
        (workspace / ".config").write_bytes((normal / ".config").read_bytes())
    with patch.dict(os.environ, {"OVE_WORKSPACE_DIR": str(workspace)}):
        ws = Workspace(str(root))
        ws.ensure_dirs()
        download_all(ws)  # Raises SystemExit on a failed dependency.
        configure_all(ws)
        extra = ["-DOVE_QUALIFY_CMSIS_NN=" + (str(nn) if nn else ""),
                 "-DOVE_QUALIFY_CMSIS_CORE=" + (str(core) if core else ""),
                 "-DOVE_NO_CCACHE=ON"]
        build_freertos(ws, cmake_source=str(root / "tests" / "infer_target"), extra_cmake_args=extra)
    return workspace


def main():
    from .manifest import load_manifest, get_component
    from .workspace import find_ove_dir

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--regenerate-goldens", action="store_true")
    parser.add_argument("--cmsis-nn-dir", type=Path)
    parser.add_argument("--cmsis-core-include", type=Path)
    args = parser.parse_args()
    if args.cmsis_nn_dir:
        args.cmsis_nn_dir = args.cmsis_nn_dir.resolve()
    if args.cmsis_core_include:
        args.cmsis_core_include = args.cmsis_core_include.resolve()
    require(not (args.build_only and args.skip_build), "incompatible build modes")
    require(bool(args.cmsis_nn_dir) == bool(args.cmsis_core_include), "supply both CMSIS paths")
    provider = "cmsis" if args.cmsis_nn_dir else "reference"
    require(not args.regenerate_goldens or provider == "reference", "only reference can produce goldens")
    root = Path(find_ove_dir()).resolve()
    manifest = load_manifest(str(root))
    pin = get_component(manifest, "libraries", "tflm", "version")
    golden_path = root / "tests" / "infer_target" / "goldens.json"
    golden = json.loads(golden_path.read_text())
    models = model_manifest(root)
    require(golden["models"] == models and golden["tflm"] == pin and
            golden["recipe"] == RECIPE and golden["seeds"] == list(SEEDS),
            "model/input/SDK drift requires explicit fixture review and requalification")
    budgets = app_budgets(root)
    shipped_providers(root)
    if not args.build_only and not args.regenerate_goldens:
        require(golden.get("generation", {}).get("provider") == "reference",
                "reference-generated goldens are required")
    captured = None
    for zero_heap in (False, True):
        app = "infer_target_zh" if zero_heap else "infer_target_heap"
        suffix = "-cmsis" if provider == "cmsis" else ""
        workspace = root / "output" / "qemu-mps2-an500" / "freertos" / (app + suffix)
        if not args.skip_build:
            workspace = build_profile(root, zero_heap, args.cmsis_nn_dir, args.cmsis_core_include)
        build = workspace / "build" / "firmware"
        sdk_check(root, pin)
        rows = json.loads((build / "compile_commands.json").read_text())
        compiler = selected_provider(rows, provider)
        elf = Path((build / "qualifier-elf.txt").read_text().strip())
        sources = sdk_sources(root, rows, provider)
        arrays = model_arrays(root, elf)
        qemu = shutil.which(os.environ.get("OVE_QEMU_ARM", "qemu-system-arm"))
        require(qemu, "qemu-system-arm is required")
        record_path = workspace / "qualification-artifacts.json"
        if not args.skip_build:
            record = {"provider": provider, "elf": str(elf), "elf_sha256": sha(elf),
                      "inputs": input_lease(root, workspace, rows), "budgets": budgets,
                      "compiler": compiler, "models": models, "tflm": pin,
                      "sdk_sources": sources, "model_arrays": arrays,
                      "qemu": qemu, "qemu_sha256": sha(qemu)}
            if provider == "cmsis":
                record["dsp"] = cmsis_dsp_evidence(compiler, elf)
                record["nn_head"] = git_output(args.cmsis_nn_dir, "rev-parse", "HEAD")
                record["nn_status"] = git_output(args.cmsis_nn_dir, "status", "--porcelain")
                require(not record["nn_status"], "supplied CMSIS-NN tree must be clean")
            record_path.write_text(json.dumps(record, indent=2) + "\n")
        record = json.loads(record_path.read_text())
        require(record["provider"] == provider and record["models"] == models and
                record["tflm"] == pin and record["budgets"] == budgets and
                record["sdk_sources"] == sources and record["model_arrays"] == arrays and
                record["qemu"] == qemu and record["qemu_sha256"] == sha(qemu) and
                record["elf"] == str(elf) and record["elf_sha256"] == sha(elf), "artifact provenance changed")
        check_lease(record["inputs"])
        if provider == "cmsis":
            require(record["dsp"] == cmsis_dsp_evidence(compiler, elf) and
                    record["nn_head"] == git_output(args.cmsis_nn_dir, "rev-parse", "HEAD") and
                    not git_output(args.cmsis_nn_dir, "status", "--porcelain"),
                    "CMSIS source/DSP provenance changed")
        if args.build_only:
            continue
        log = workspace / "qualification.log"
        results = parse_output(run_qemu(qemu, elf, workspace), zero_heap=zero_heap)
        check_lease(record["inputs"])
        sdk_check(root, pin)
        if args.regenerate_goldens:
            outputs = {s: {k: results["init"][s][k] for k in ("features", "scores")} for s in map(str, SEEDS)}
            if captured is None:
                captured = outputs
            require(outputs == captured, "heap-mode reference outputs differ")
            compare_goldens(results, captured)
        else:
            compare_goldens(results, golden["outputs"])
        (workspace / "qualification-result.json").write_text(json.dumps({
            "provider": provider, "models": models, "tflm": pin, "elf_sha256": sha(elf),
            "log_sha256": sha(log), "qemu_log_sha256": sha(workspace / "qualification-qemu.log"),
            "results": results}, indent=2) + "\n")
        for mode, cases in results.items():
            for seed, case in cases.items():
                for name, measured in case["arenas"].items():
                    print(f"{app}/{provider}/{mode}/seed{seed}/{name}: {measured}")
    if args.regenerate_goldens and not args.build_only:
        golden.update(outputs=captured, generation={"provider": provider, "tflm": pin,
            "compiler": Path(compiler).name, "compiler_sha256": sha(compiler),
            "compiler_version": subprocess.check_output([compiler, "-dumpfullversion"],
                                                        text=True, timeout=10).strip()})
        golden_path.write_text(json.dumps(golden, indent=2) + "\n")


if __name__ == "__main__":
    main()
