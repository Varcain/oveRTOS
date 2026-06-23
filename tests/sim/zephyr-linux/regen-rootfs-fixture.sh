#!/usr/bin/env bash
#
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Rebuild the Buildroot rootfs.cpio for the oveRTOS Linux personality. The
# Buildroot config is self-contained in the Buildroot tree itself:
#   configs/overtos_defconfig + board/overtos/busybox.config
# (a uClinux/bFLT uClibc userspace + BusyBox, cpio rootfs, no kernel). Enable
# applets by editing board/overtos/busybox.config in the Buildroot tree, then
# run this. The header is NOT committed — the oveRTOS build embeds the cpio
# (cmake/OveLinuxFixtures.cmake); after this, run `ove configure && ove build`.
#
# Usage:  BUILDROOT=/path/to/buildroot ./regen-rootfs-fixture.sh
set -euo pipefail
BUILDROOT="${BUILDROOT:-$HOME/projects/private/hIRoic/buildroot}"
[ -x "$BUILDROOT/output/host/bin/arm-buildroot-uclinux-uclibcgnueabi-gcc" ] ||
	{ echo "toolchain missing; run 'make overtos_defconfig && make' in $BUILDROOT"; exit 1; }

cd "$BUILDROOT"
make overtos_defconfig >/dev/null  # configs/overtos_defconfig -> board/overtos/busybox.config
make busybox-dirclean >/dev/null   # force a busybox rebuild from the (possibly edited) config
make                               # builds busybox + assembles output/images/rootfs.cpio

[ "$(head -c6 output/images/rootfs.cpio)" = "070701" ] || { echo "no newc cpio produced"; exit 1; }
echo "produced $BUILDROOT/output/images/rootfs.cpio ($(stat -c%s output/images/rootfs.cpio) bytes)"
echo "now run 'ove configure && ove build' to re-embed it"
