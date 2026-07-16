#!/usr/bin/env python3
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Guest ABI switching regression: soft -> hard -> soft in one checkout, without
# cleaning between builds.
#
# The float ABIs live inside .config and appear in no path, so before image
# identity a soft and a hard build overwrote the same images/firmware.elf and
# nothing recorded which was which. This asserts, for each leg:
#
#   - the image lands in its own images/<host>-guest-<guest>/ directory
#   - image-id.json names the ABI and rootfs it was actually built from
#   - the recorded artifact hash matches the ELF on disk
#   - switching back does not resurrect the previous leg's image
#
# and finally that both ABIs' images coexist rather than clobbering each other.
#
# Switching goes through kconfiglib, never a textual edit: OVE_LINUX_ROOTFS_OUTPUT
# is a hidden symbol derived from the ABI choice, and only kconfiglib re-derives
# it. `ove configure` after each flip is deliberate — it is what regenerates
# ove_config.h, and skipping it is exactly the bug this guards.
#
# Manual/opt-in: three full firmware builds, and the hard leg needs Buildroot's
# separately generated output-hardfloat rootfs.
#
# Usage: abi_switch_drive.py [logfile]
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.getcwd(), "config", "ove-cli"))
from ove import image_id                       # noqa: E402
from ove.workspace import Workspace, get_str   # noqa: E402

log_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/abi_switch_drive.log"
OVE_DIR = os.getcwd()
OVE = os.path.join(OVE_DIR, ".venv", "bin", "ove")

# (guest ABI, Buildroot output subdir the hidden symbol must derive to)
LEGS = [("soft", "output"), ("hard", "output-hardfloat"), ("soft", "output")]

lines = []


def say(msg):
    print(msg)
    lines.append(msg)


def run(*args):
    r = subprocess.run([OVE, *args], cwd=OVE_DIR,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True)
    if r.returncode != 0:
        say(f"  FAIL: 'ove {' '.join(args)}' exited {r.returncode}")
        say((r.stdout or "")[-2000:])
    return r.returncode == 0


def select_guest_abi(abi):
    """Flip the guest ABI choice through kconfiglib so hidden symbols re-derive."""
    import kconfiglib

    os.environ["srctree"] = OVE_DIR
    kconf = kconfiglib.Kconfig(os.path.join(OVE_DIR, "Config.in"))
    cfg = os.path.join(OVE_DIR, ".config")
    kconf.load_config(cfg)
    sym = ("OVE_LINUX_GUEST_FLOAT_ABI_HARD" if abi == "hard"
           else "OVE_LINUX_GUEST_FLOAT_ABI_SOFT")
    kconf.syms[sym].set_value(2)
    # The self-test replaces init; irrelevant here and not valid for a soft guest.
    kconf.syms["OVE_LINUX_GUEST_FP_SELFTEST"].set_value(0)
    kconf.write_config(cfg)
    return kconf.syms["OVE_LINUX_ROOTFS_OUTPUT"].str_value


def check_leg(abi, want_rootfs):
    """Build one leg and verify the image it produced describes itself."""
    ok = True
    derived = select_guest_abi(abi)
    if derived != want_rootfs:
        say(f"  FAIL: {abi} guest derived rootfs {derived!r}, want {want_rootfs!r}")
        return False, None

    if not run("configure") or not run("build"):
        return False, None

    ws = Workspace()
    if ws.linux_guest_float_abi != abi:
        say(f"  FAIL: workspace reports guest {ws.linux_guest_float_abi!r}, want {abi!r}")
        ok = False

    elf = os.path.join(ws.images_dir, "firmware.elf")
    if not os.path.isfile(elf):
        say(f"  FAIL: no firmware.elf in {ws.images_dir}")
        return False, ws.images_dir

    ident = image_id.load(ws.images_dir)
    if ident is None:
        say(f"  FAIL: no image-id.json beside {elf}")
        return False, ws.images_dir

    got = ident["config"]["guest_float_abi"]
    if got != abi:
        say(f"  FAIL: image-id.json says guest {got!r}, want {abi!r}")
        ok = False
    got_rootfs = (ident.get("rootfs") or {}).get("output")
    if got_rootfs != want_rootfs:
        say(f"  FAIL: image-id.json rootfs {got_rootfs!r}, want {want_rootfs!r}")
        ok = False
    if get_str(ws.config, "CONFIG_OVE_LINUX_ROOTFS_OUTPUT") != want_rootfs:
        say("  FAIL: .config rootfs output did not follow the ABI choice")
        ok = False

    # The identity must describe the bytes actually on disk, not a past build.
    stale = image_id.mismatches(ws)
    if stale:
        say(f"  FAIL: image does not match its own workspace: {stale}")
        ok = False

    say(f"  {abi:4s} guest -> {os.path.basename(ws.images_dir)}/  "
        f"rootfs={want_rootfs}  build_id={ident['build_id']}")
    return ok, ws.images_dir


def main():
    say("=== guest ABI switch: soft -> hard -> soft (no clean between) ===")
    if not run("defconfig-fragments", "qemu.freertos.linux_interop"):
        return 1

    ok = True
    seen = {}
    for abi, want_rootfs in LEGS:
        good, images_dir = check_leg(abi, want_rootfs)
        ok = ok and good
        if images_dir:
            seen[abi] = images_dir

    # Coexistence: the point of the per-ABI directories. Both legs' images must
    # still be on disk, each still described by its own identity.
    for abi, images_dir in seen.items():
        elf = os.path.join(images_dir, "firmware.elf")
        ident = image_id.load(images_dir)
        if not os.path.isfile(elf) or ident is None:
            say(f"  FAIL: {abi} image did not survive the other ABI's build")
            ok = False
        elif ident["config"]["guest_float_abi"] != abi:
            say(f"  FAIL: {abi} image dir now claims "
                f"{ident['config']['guest_float_abi']!r}")
            ok = False
    if len(seen) == 2 and seen["soft"] != seen["hard"]:
        say(f"  coexist OK: {os.path.basename(seen['soft'])} and "
            f"{os.path.basename(seen['hard'])}")
    else:
        say("  FAIL: soft and hard did not land in distinct image directories")
        ok = False

    say("RESULT: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


rc = main()
with open(log_path, "w") as f:
    f.write("\n".join(lines) + "\n")
sys.exit(rc)
