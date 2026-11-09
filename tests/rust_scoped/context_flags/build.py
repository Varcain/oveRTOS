# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build metadata models of the actual providers; never run a model here."""

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

from profiles import profiles, validate_profiles

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def getters(profile, case):
    """Literal direct-getter observations, separate from the flags table."""
    if profile["config_fallback"]:
        return [0] * 7
    if "__restored_valid" in case:
        case = "ordinary"
    provider = profile["provider"]
    if provider == "freertos":
        if "unprivileged" in case:
            return [0] * 7
        if "isr" in case:
            return [1, 0, 0, 0, 0, 0, 0]
        if "prestart" in case:
            return [1, 1, 0, 0, 0, 0, 0]
        return [1, 1, 1, 1, 0, 0, 0]
    if provider == "zephyr":
        if case == "user":
            return [1, 0, 0, 0, 0, 0, 0]
        if case == "pre_kernel":
            return [1, 1, 0, 0, 0, 0, 0]
        if case == "isr":
            return [1, 1, 1, 0, 0, 0, 0]
        native = "CONFIG_BOARD_NATIVE_SIM" in profile["defines"]
        if case == "unprivileged":
            return [1, 1, 1, 0, int(native), 0, 0]
        return [1, 1, 1, 1, int(native), 0, 0]
    if provider == "nuttx":
        native = "CONFIG_ARCH_SIM" in profile["defines"]
        if case == "unprivileged":
            return [0] * 7
        if case in ("powerup", "boot", "tasklists", "memory", "hardware", "panic"):
            return [0, 0, int(native), 0, 0, 0, 0]
        if case == "isr":
            return [1, 0, int(native), 0, 0, 0, 0]
        return [1, 1, int(native), 0, 0, 0, 0]
    return [0] * 7


def expected(profile, canary=False):
    text = "failed: explicit early failure canary\n" if canary else ""
    for row in profile["cases"]:
        counts = ",".join(map(str, getters(profile, row["case"])))
        text += (
            f"{row['case']}: flags={row['expected_flags']} "
            f"legacy={row['expected_resource_blocking']} readonly=1 effects=0 "
            f"early=0 getters={counts}\n"
        )
    return text + (
        f"context-model: profile={profile['profile']} failures={int(canary)} "
        f"queries={len(profile['cases'])}\n"
    )


def build(output, sanitize=False, selected_profiles=None):
    if sys.flags.optimize:
        raise ValueError("context checks require Python without optimization")
    validate_profiles()
    selection = profiles if selected_profiles is None else selected_profiles
    if not selection or len({row["profile"] for row in selection}) != len(selection):
        raise ValueError("empty or duplicate context selection")
    output.mkdir(parents=True, exist_ok=True)
    original = (ROOT / "backends/common/ove_sync_context.h").read_text()
    replacements = []
    transformed = original
    for register in ("control", "primask", "basepri", "faultmask"):
        old = f'__asm__ volatile("mrs %0, {register}" : "=r"({register}) : : "memory");'
        new = f"{register} = model_{register}();"
        if transformed.count(old) != 1:
            raise ValueError(f"architectural read changed: {register}")
        transformed = transformed.replace(old, new)
        replacements.append({"original": old, "model": new})
    reverse = transformed
    for row in replacements:
        reverse = reverse.replace(row["model"], row["original"])
    assert reverse == original
    (output / "ove_sync_context.h").write_text(transformed)
    digest = lambda text: hashlib.sha256(text.encode()).hexdigest()
    (output / "helper-provenance.json").write_text(
        json.dumps(
            {
                "original_sha256": digest(original),
                "derived_sha256": digest(transformed),
                "reversed_sha256": digest(reverse),
                "replacements": replacements,
            },
            indent=2,
        )
        + "\n"
    )
    images = []
    for profile in selection:
        folder = output / profile["profile"]
        folder.mkdir(exist_ok=True)
        (folder / "ove_config.h").write_text(
            "#pragma once\n"
            + "".join(
                f"#define {key} {value}\n" for key, value in profile["defines"].items()
            )
        )
        cases = "".join(
            f"observe({json.dumps(row['case'])}, {row['expected_flags']}, "
            f"{row['expected_resource_blocking']}, "
            f"(const unsigned[7]){{{','.join(map(str, getters(profile, row['case'])))}}});\n"
            for row in profile["cases"]
        )
        (folder / "cases.inc").write_text(cases)
        provider = profile["provider"]
        source = {"posix": ROOT / "tests/host/posix/posix_irq.c"}.get(
            provider, ROOT / f"backends/{provider}/{provider}_irq.c"
        )
        command = [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-g",
            "-O1",
            "-fno-omit-frame-pointer",
            "-Wno-unused-variable",
            "-Wno-unused-function",
            "-include",
            str(folder / "ove_config.h"),
            "-include",
            str(HERE / "model.h"),
            "-I" + str(folder),
            "-I" + str(output),
            "-I" + str(HERE / "sdk"),
            "-I" + str(HERE),
            "-I" + str(ROOT / "include"),
            "-I" + str(ROOT / "backends/freertos/include"),
            "-DMODEL_PROFILE=" + json.dumps(profile["profile"]),
        ]
        if provider != "header_only":
            command += [
                "-DMODEL_" + provider.upper() + "=1",
                "-DMODEL_PROVIDER_SOURCE=" + json.dumps(str(source)),
            ]
        if sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        command += [
            "-MMD",
            "-MF",
            str(folder / "model.d"),
            str(HERE / "model.c"),
            "-o",
            str(folder / "model"),
        ]
        completed = subprocess.run(command, timeout=60, check=False)
        if completed.returncode:
            raise RuntimeError(f"context model build failed: {profile['profile']}")
        binary = folder / "model"
        images.append(
            {
                "profile": profile["profile"],
                "binary": str(binary),
                "command": command,
                "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "normal_text": expected(profile),
                "canary_text": expected(profile, True),
            }
        )
    (output / "images.json").write_text(json.dumps(images, indent=2) + "\n")
    return images


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    build(args.output.resolve(), args.sanitize)
