# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate finite test-only variants; never edit SDK or product sources."""

import json
import sys
from pathlib import Path

root, out = map(Path, sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
wrapper = (root / "backends/common/ove_infer.cc").read_text()
arena = (root / "backends/common/tflm/ove_tflm_arena.cc").read_text()
variants = []


def variant(name, source, old, new, fixture, definition, cases):
    if source.count(old) != 1:
        raise SystemExit(f"{name}: mutation anchor drift")
    path = out / (name + ".cc")
    changed = source.replace(old, new)
    if not path.exists() or path.read_text() != changed:
        path.write_text(changed)
    variants.append(
        {
            "name": name,
            "source": str(path),
            "fixture": fixture,
            "definition": definition,
            "cases": cases,
        }
    )


variant(
    "overlap",
    arena,
    "if (alignment == 0 || bytes > available || alignment - 1 > available - bytes)",
    "if (false)",
    "allocator",
    "OVE_ARENA_SOURCE",
    ["overlap"],
)
variant(
    "exhaustion",
    arena,
    "exhausted_ = true;",
    "exhausted_ = false;",
    "allocator",
    "OVE_ARENA_SOURCE",
    [
        "persistent",
        "temp",
        "resizable",
        "resize",
        "reserve",
        "entries",
        "offline-entries",
    ],
)
variant(
    "plan",
    arena,
    "if (persistent > arena_.capacity() ||\n\t\t    Base::GetMaximumMemorySize() > arena_.capacity() - persistent)",
    "if (false)",
    "allocator",
    "OVE_ARENA_SOURCE",
    ["plan"],
)
variant(
    "gap",
    arena,
    "Base::GetMaximumMemorySize() > arena_.capacity() - persistent",
    "Base::GetMaximumMemorySize() > arena_.GetAvailableMemory(1)",
    "allocator",
    "OVE_ARENA_SOURCE",
    ["live-scratch"],
)
variant(
    "minimum",
    wrapper,
    "bytes - padding < ove_tflm_arena_minimum()",
    "false",
    "frontiers",
    "OVE_INFER_SOURCE",
    ["minimum"],
)
variant(
    "alignment",
    wrapper,
    "(bytes & (tflite::MicroArenaBufferAlignment() - 1))",
    "false",
    "frontiers",
    "OVE_INFER_SOURCE",
    ["alignment"],
)
variant(
    "schema",
    wrapper,
    "if (tfl_model->version() != TFLITE_SCHEMA_VERSION)",
    "if (false)",
    "frontiers",
    "OVE_INFER_SOURCE",
    ["schema"],
)
variant(
    "zero",
    wrapper,
    "std::memset(effective, 0, effective_size);",
    "(void)0;",
    "frontiers",
    "OVE_INFER_SOURCE",
    ["zero"],
)
variant(
    "range",
    wrapper,
    "cfg->arena_size > static_cast<size_t>(INT_MAX)",
    "false",
    "frontiers",
    "OVE_INFER_SOURCE",
    ["range"],
)
# A small numerical error safely changes only the query; normal teardown runs.
variant(
    "query",
    wrapper,
    "return interpreter->arena_used_bytes() + model_arena_padding(model->arena);",
    "return interpreter->arena_used_bytes() + model_arena_padding(model->arena) + 1;",
    "models",
    "OVE_INFER_SOURCE",
    ["query aligned static"],
)
variant(
    "mapping",
    wrapper,
    "return exhausted ? OVE_ERR_NO_MEMORY : OVE_ERR_ML_FAILED;",
    "return OVE_ERR_ML_FAILED;",
    "models",
    "OVE_INFER_SOURCE",
    ["candidate preprocessor static", "candidate classifier static"],
)
anchor = "if (!model_try_claim())\n\t\treturn OVE_ERR_BUSY;"
# Both constructor adapters reject tiny sizes before admission. They still stop
# before candidate writes or allocations, so the failure is a plain assertion.
if wrapper.count(anchor) != 2:
    raise SystemExit("busy: mutation anchor drift")
busy = wrapper.replace(
    anchor,
    "if (cfg->arena_size < ove_tflm_arena_minimum())\n\t\treturn OVE_ERR_NO_MEMORY;\n\t"
    + anchor,
)
busy_path = out / "busy.cc"
if not busy_path.exists() or busy_path.read_text() != busy:
    busy_path.write_text(busy)
variants.append(
    {
        "name": "busy",
        "source": str(busy_path),
        "fixture": "controlled",
        "definition": "",
        "cases": ["busy-tiny-static"],
        "heap_cases": ["busy-tiny-heap"],
    }
)
assertions = {
    "overlap": "persistent == nullptr && owned.exhausted()",
    "exhaustion": "allocator.exhausted()",
    "plan": "plan.GetOffsetForBuffer(0, &offset) != kTfLiteOk",
    "gap": "plan.GetOffsetForBuffer(0, &offset) == kTfLiteOk",
    "minimum": "model_arena_status(arena, n) == OVE_ERR_NO_MEMORY",
    "alignment": "model_config_status(&cfg) == OVE_ERR_INVALID_PARAM",
    "schema": "rc == (schema ? OVE_ERR_NOT_SUPPORTED : OVE_ERR_NO_MEMORY)",
    "zero": "schema || cleared",
    "range": "model_config_status(&cfg) == OVE_ERR_INVALID_PARAM",
    "query": "good && ove_model_arena_used(nullptr) == 0 && released()",
    "mapping": "rc == OVE_ERR_NO_MEMORY && !published && released()",
    "busy": "rc == OVE_ERR_BUSY && candidate == active",
}
for item in variants:
    item["assertions"] = {
        case: (
            "rc != kTfLiteOk && allocator.exhausted()"
            if item["name"] == "exhaustion" and case in ["entries", "offline-entries"]
            else assertions[item["name"]]
        )
        for case in item["cases"] + item.get("heap_cases", [])
    }
(out / "variants.json").write_text(json.dumps(variants, indent=2) + "\n")
cmake = []
for item in variants:
    name = "infer_arena_mut_" + item["name"]
    if item["fixture"] == "controlled":
        cmake += [
            f'add_executable({name} "${{OVE_DIR}}/tests/infer_admission/controlled.cc" "${{OVE_DIR}}/tests/infer_admission/controlled/arena.cc" "{item["source"]}")',
            f"target_include_directories({name} BEFORE PRIVATE ${{OVE_DIR}}/tests/infer_admission/controlled ${{CMAKE_BINARY_DIR}} ${{OVE_DIR}}/include ${{OVE_DIR}}/tests/host/posix/include ${{OVE_DIR}}/backends/common)",
            f"target_link_libraries({name} PRIVATE pthread)",
            f'target_link_options({name} PRIVATE -Wl,--wrap=malloc -Wl,--wrap=free "-Wl,-Map,${{CMAKE_BINARY_DIR}}/{name}.map")',
        ]
        continue
    cmake += [
        f'add_executable({name} "${{OVE_DIR}}/tests/infer_arena/{item["fixture"]}.cc")',
        f'target_compile_definitions({name} PRIVATE TF_LITE_STATIC_MEMORY {item["definition"]}="{item["source"]}")',
        f"target_compile_options({name} PRIVATE -fno-rtti -fno-exceptions -fno-threadsafe-statics)",
        f"target_include_directories({name} BEFORE PRIVATE ${{CMAKE_BINARY_DIR}} ${{OVE_DIR}}/tests ${{OVE_DIR}}/tests/host/posix/include ${{OVE_DIR}}/backends/common ${{OVE_DIR}}/backends/common/tflm ${{CMAKE_BINARY_DIR}}/arena-models)",
        f"target_link_libraries({name} PRIVATE ove_tflm m pthread)",
        f'target_link_options({name} PRIVATE "-Wl,-Map,${{CMAKE_BINARY_DIR}}/{name}.map")',
    ]
    if item["fixture"] == "models":
        cmake += [
            f"target_link_libraries({name} PRIVATE infer_arena_data)",
            f"add_dependencies({name} infer_arena_data)",
            f"target_link_options({name} PRIVATE -Wl,--wrap=malloc -Wl,--wrap=free)",
        ]
(out / "Targets.cmake").write_text("\n".join(cmake) + "\n")
