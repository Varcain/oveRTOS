#!/usr/bin/env bash
#
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate tests/ontarget/loader_rootfs_image.h — a minimal Buildroot rootfs.cpio
# (newc) embedded as the boot image for the qemu-zephyr-linux test. The rootfs is
# BusyBox 1.38 (interactive hush + FEATURE_SH_STANDALONE applets, no job control)
# plus the Buildroot skeleton (/bin, /etc, /dev, applet symlinks). The personality
# parses the CPIO into its rootfs and execs /bin/busybox as the "sh" init process.
#
# Prereqs: the Buildroot tree built its uClinux/bFLT toolchain ([[reference]]) — it
# is configured for BR2_BINFMT_FLAT + the uClibc toolchain + BR2_TARGET_ROOTFS_CPIO.
#
# Usage:  BUILDROOT=/path/to/buildroot ./regen-rootfs-fixture.sh
set -euo pipefail
BUILDROOT="${BUILDROOT:-$HOME/projects/private/hIRoic/buildroot}"
OVE_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
[ -x "$BUILDROOT/output/host/bin/arm-buildroot-uclinux-uclibcgnueabi-gcc" ] ||
	{ echo "toolchain missing; run 'make toolchain' in $BUILDROOT"; exit 1; }

# Use our minimal busybox config (allnoconfig + hush/standalone/editing + a few applets).
cp "$OVE_DIR/tests/sim/zephyr-linux/ove-busybox-minimal.config" "$BUILDROOT/ove-busybox-minimal.config"
cd "$BUILDROOT"
sed -i 's#^BR2_PACKAGE_BUSYBOX_CONFIG=.*#BR2_PACKAGE_BUSYBOX_CONFIG="ove-busybox-minimal.config"#' .config
sed -i 's#^BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES=.*#BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES=""#' .config
yes "" | make olddefconfig >/dev/null
make busybox-dirclean >/dev/null
make            # builds busybox + assembles output/images/rootfs.cpio

[ "$(head -c6 output/images/rootfs.cpio)" = "070701" ] || { echo "no newc cpio produced"; exit 1; }
cmake -DIN="$BUILDROOT/output/images/rootfs.cpio" \
      -DOUT="$OVE_DIR/tests/ontarget/loader_rootfs_image.h" \
      -DSYM=ove_test_rootfs_cpio \
      -P "$OVE_DIR/tests/cmake/embed_bin.cmake"
echo "regenerated tests/ontarget/loader_rootfs_image.h ($(stat -c%s output/images/rootfs.cpio) bytes)"
