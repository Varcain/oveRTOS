#!/usr/bin/env bash
#
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Rebuild the Buildroot rootfs.cpio for the oveRTOS Linux personality. The Buildroot config is
# self-contained in the Buildroot tree: configs/<defconfig> + board/overtos/busybox.config
# (BusyBox cpio rootfs, no kernel). DEFAULT = the dynamic-FDPIC userspace (output-fdpic /
# overtos_fdpic_defconfig): every process shares ONE in-place copy of busybox.so/ld.so/libc.so
# text and Zephyr keeps W^X on. For the legacy bFLT userspace:
#   OUT=output DEFCONFIG=overtos_defconfig ./regen-rootfs-fixture.sh
# Enable applets by editing board/overtos/busybox.config in the Buildroot tree, then run this. The
# header is NOT committed — the oveRTOS build embeds the cpio (cmake/OveLinuxFixtures.cmake,
# OVE_LINUX_ROOTFS_OUTPUT); after this, run `ove configure && ove build`.
#
# Usage:  BUILDROOT=/path/to/buildroot ./regen-rootfs-fixture.sh
set -euo pipefail
BUILDROOT="${BUILDROOT:-$HOME/projects/private/hIRoic/buildroot}"
OUT="${OUT:-output-fdpic}"
DEFCONFIG="${DEFCONFIG:-overtos_fdpic_defconfig}"

cd "$BUILDROOT"
make O="$OUT" "$DEFCONFIG" >/dev/null      # configs/$DEFCONFIG -> board/overtos/busybox.config
make O="$OUT" busybox-dirclean >/dev/null  # force a busybox rebuild from the (possibly edited) config
make O="$OUT"                              # builds the toolchain (first run) + busybox + the cpio

CPIO="$BUILDROOT/$OUT/images/rootfs.cpio"
[ "$(head -c6 "$CPIO")" = "070701" ] || { echo "no newc cpio produced"; exit 1; }
echo "produced $CPIO ($(stat -c%s "$CPIO") bytes)"
echo "now run 'ove configure && ove build' to re-embed it"
