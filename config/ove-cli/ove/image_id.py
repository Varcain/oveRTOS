# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Firmware image identity: what exactly is this ELF, and does it match?

Distinct from :mod:`manifest`, which pins *external component versions* from
manifest.yaml.  This module describes a *built image*: the configuration it was
produced from, the source revisions it was built at, and the hashes of the
resulting artifacts.  It answers "is the thing I am about to flash the thing I
think I built?".

The identity is written as image-id.json beside the ELF and mirrors into a short
build ID compiled into the firmware, so an image that is already running on a
target can be matched against the ELF on disk over the console.

The build ID is deliberately free of timestamps and build counters: two builds
of the same configuration at the same revisions must produce the same ID, or it
cannot be used to tell images apart.
"""

import json
import logging
import os
import subprocess

from .utils import hash_file

logger = logging.getLogger("ove")

ID_FILE = "image-id.json"
SCHEMA = 1

# Artifacts hashed into the identity when present beside the ELF.
_ARTIFACTS = ("firmware.elf", "firmware.bin", "firmware.hex")


def _git(repo, *args):
    """Run git in *repo*, returning stripped stdout or None."""
    if not os.path.isdir(repo):
        return None
    try:
        out = subprocess.run(("git", "-C", repo) + args,
                             capture_output=True, text=True, check=True)
    except (subprocess.CalledProcessError, OSError):
        return None
    return out.stdout.strip()


def _revision(repo):
    """Return {'commit': ..., 'dirty': ...} for *repo*, or None if not a repo."""
    commit = _git(repo, "rev-parse", "HEAD")
    if not commit:
        return None
    # --porcelain is empty exactly when the worktree matches HEAD.
    status = _git(repo, "status", "--porcelain", "--untracked-files=no")
    return {"commit": commit, "dirty": bool(status)}


def _short(rev):
    """Abbreviate a revision dict for the human-facing build ID."""
    if not rev:
        return "nogit"
    return rev["commit"][:7] + ("-dirty" if rev["dirty"] else "")


def _lxp_dir(ove_dir):
    return os.path.join(ove_dir, "modules", "lxp")


def build_id(ws):
    """Short identifier embedded in the firmware and printed at boot."""
    ove = _short(_revision(ws.ove_dir))
    lxp = _short(_revision(_lxp_dir(ws.ove_dir)))
    parts = [ws.board_name, ws.rtos, ws.app_name, f"ove-{ove}", f"lxp-{lxp}"]
    variant = ws.image_variant
    if variant:
        parts.append(variant)
    return " ".join(p for p in parts if p)


def _rootfs(ws):
    """Identify the Buildroot rootfs.cpio backing a Linux personality build.

    Reports the *configured* output subdir rather than the one the guest ABI
    implies: this records what the build actually embedded, so that a
    contradiction between the two stays visible instead of being normalised
    away here.  See :func:`rootfs_abi_conflict`.
    """
    if ws.linux_guest_float_abi is None:
        return None
    from .workspace import get_str
    br = get_str(ws.config, "CONFIG_OVE_BUILDROOT")
    out = get_str(ws.config, "CONFIG_OVE_LINUX_ROOTFS_OUTPUT")
    if not br or not out:
        return None
    # Relative Buildroot paths resolve against the oveRTOS root, matching
    # cmake/OveLinuxFixtures.cmake.
    if not os.path.isabs(br):
        br = os.path.join(ws.ove_dir, br)
    path = os.path.normpath(os.path.join(br, out, "images", "rootfs.cpio"))
    info = {"path": path, "output": out}
    if os.path.isfile(path):
        info["sha256"] = hash_file(path)
        info["bytes"] = os.path.getsize(path)
    else:
        info["sha256"] = None
    return info


def rootfs_abi_conflict(ws):
    """Return a message if the embedded rootfs contradicts the guest ABI.

    OVE_LINUX_ROOTFS_OUTPUT is a hidden Kconfig symbol *derived* from the guest
    ABI choice.  Every CLI writer of .config goes through kconfiglib, which
    re-derives it, but ove configure renders its templates from a flat .config
    parse and evaluates no dependencies.  So a .config that was edited by hand
    or by an external tool can select one ABI while the stale derived line
    still names the other rootfs, and the build silently embeds a userspace of
    the wrong ABI.  Returns None when consistent.
    """
    guest = ws.linux_guest_float_abi
    if guest is None:
        return None
    from .workspace import get_str
    # An explicit override names an out-of-tree O= dir whose ABI the user
    # asserts; only the derived default is checkable.
    if get_str(ws.config, "CONFIG_OVE_LINUX_ROOTFS_OUTPUT_OVERRIDE"):
        return None
    actual = get_str(ws.config, "CONFIG_OVE_LINUX_ROOTFS_OUTPUT")
    expected = "output-hardfloat" if guest == "hard" else "output"
    if actual == expected:
        return None
    return (f"{guest}-float guest ABI selected but rootfs output is "
            f"{actual!r} (expected {expected!r}) — the firmware would embed a "
            f"userspace of the wrong ABI; re-run 'ove configure' after setting "
            f"the guest ABI through menuconfig/kconfiglib so the derived "
            f"CONFIG_OVE_LINUX_ROOTFS_OUTPUT follows the choice")


def compute(ws, images_dir=None):
    """Build the identity dict describing the images in *images_dir*."""
    images_dir = images_dir or ws.images_dir
    artifacts = {}
    for name in _ARTIFACTS:
        path = os.path.join(images_dir, name)
        if os.path.isfile(path):
            artifacts[name] = {"sha256": hash_file(path),
                               "bytes": os.path.getsize(path)}
    return {
        "schema": SCHEMA,
        "build_id": build_id(ws),
        "config": {
            "board": ws.board_name,
            "rtos": ws.rtos,
            "app": ws.app_name,
            "host_float_abi": ws.arm_float_abi,
            "guest_float_abi": ws.linux_guest_float_abi,
            "image_variant": ws.image_variant,
        },
        "source": {
            "overtos": _revision(ws.ove_dir),
            "lxp": _revision(_lxp_dir(ws.ove_dir)),
        },
        "rootfs": _rootfs(ws),
        "artifacts": artifacts,
    }


def write(ws, images_dir=None):
    """Write image-id.json beside the built images. Returns the identity."""
    images_dir = images_dir or ws.images_dir
    ident = compute(ws, images_dir)
    path = os.path.join(images_dir, ID_FILE)
    with open(path, "w") as f:
        json.dump(ident, f, indent=2, sort_keys=True)
        f.write("\n")
    return ident


def load(images_dir):
    """Read image-id.json from *images_dir*, or None if absent/unreadable."""
    path = os.path.join(images_dir, ID_FILE)
    if not os.path.isfile(path):
        return None
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError) as exc:
        logger.warning(f"{ID_FILE} is unreadable: {exc}")
        return None


def mismatches(ws, images_dir=None):
    """Reasons the images in *images_dir* do not match the active workspace.

    Returns a list of human-readable strings; empty means the image on disk is
    the one this configuration builds, unmodified since it was built.
    """
    images_dir = images_dir or ws.images_dir
    stored = load(images_dir)
    if stored is None:
        return [f"no {ID_FILE} beside the image — it predates image identity "
                f"or was copied in by hand; rebuild with 'ove build'"]

    reasons = []
    if stored.get("schema") != SCHEMA:
        reasons.append(f"{ID_FILE} schema {stored.get('schema')} != {SCHEMA} "
                       f"(rebuild to refresh)")
        return reasons

    # Configuration drift: the .config symlink is mutable shared state, so an
    # image built for another board/ABI can otherwise be flashed unnoticed.
    want = compute(ws, images_dir)["config"]
    got = stored.get("config", {})
    for key, expected in want.items():
        actual = got.get(key)
        if actual != expected:
            reasons.append(f"{key}: image has {actual!r}, "
                           f"workspace wants {expected!r}")

    # Content drift: the recorded hash is what the build produced, so a
    # difference means the file changed underneath the identity.
    for name, meta in stored.get("artifacts", {}).items():
        path = os.path.join(images_dir, name)
        if not os.path.isfile(path):
            reasons.append(f"{name} recorded but missing from {images_dir}")
            continue
        if hash_file(path) != meta.get("sha256"):
            reasons.append(f"{name} has changed since it was built "
                           f"(sha256 differs from {ID_FILE})")

    conflict = rootfs_abi_conflict(ws)
    if conflict:
        reasons.append(conflict)
    return reasons


def describe(ident):
    """One-line human summary of an identity dict."""
    if not ident:
        return "unknown image (no identity recorded)"
    cfg = ident.get("config", {})
    src = ident.get("source", {})
    bits = [ident.get("build_id", "?")]
    guest = cfg.get("guest_float_abi")
    if guest:
        bits.append(f"guest float ABI: {guest}")
    for name in ("overtos", "lxp"):
        rev = src.get(name)
        if rev and rev.get("dirty"):
            bits.append(f"{name} worktree dirty at build time")
    return " | ".join(bits)
