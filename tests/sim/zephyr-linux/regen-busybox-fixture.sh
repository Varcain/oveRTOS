#!/usr/bin/env bash
#
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate tests/ontarget/loader_hello_image.h — the BusyBox 1.36 hush bFLT
# embedded as the Linux-personality init program for the qemu-zephyr-linux test.
#
# hush (not ash) is the shell: ash hard-refuses NOMMU; hush uses vfork()+exec(),
# which is exactly the personality's process model. Built static against the
# Buildroot uClibc-ng uClinux/bFLT toolchain, minimal config (no job control /
# line editing → no termios/signal-delivery dependency yet).
#
# Usage:  BUILDROOT=/path/to/buildroot ./regen-busybox-fixture.sh
set -euo pipefail

BB_VER="${BB_VER:-1.36.1}"
BUILDROOT="${BUILDROOT:-$HOME/projects/private/hIRoic/buildroot}"
OVE_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
TC="$BUILDROOT/output/host/bin/arm-buildroot-uclinux-uclibcgnueabi"
work="$(mktemp -d)"

[ -x "$TC-gcc" ] || { echo "toolchain not found: $TC-gcc (set BUILDROOT, run 'make toolchain' in buildroot)"; exit 1; }
export PATH="$BUILDROOT/output/host/bin:$PATH"
export CROSS_COMPILE="arm-buildroot-uclinux-uclibcgnueabi-"

cd "$work"
wget -q "https://busybox.net/downloads/busybox-$BB_VER.tar.bz2"
tar xf "busybox-$BB_VER.tar.bz2"
cd "busybox-$BB_VER"

make allnoconfig >/dev/null
cfg=.config
en() { sed -i "s/^# $1 is not set/$1=y/; t; s/^$1=.*/$1=y/" "$cfg"; grep -q "^$1=y" "$cfg" || echo "$1=y" >>"$cfg"; }
dis() { sed -i "s/^$1=y/# $1 is not set/" "$cfg"; }
en CONFIG_STATIC
en CONFIG_LFS         # uClibc-ng off_t is 64-bit; must match or the build BUG-asserts
en CONFIG_FEATURE_LFS
en CONFIG_HUSH        # the NOMMU-capable shell (vfork+exec)
en CONFIG_SH_IS_HUSH
en CONFIG_ECHO
dis CONFIG_ASH        # ash #errors out on NOMMU
dis CONFIG_SH_IS_ASH
dis CONFIG_HUSH_JOB   # no job control (needs signals/tty)
dis CONFIG_FEATURE_EDITING
yes "" | make oldconfig >/dev/null
make -j"$(nproc)" || true   # the final strip step fails on FLAT; busybox_unstripped is the bFLT

[ "$(head -c4 busybox_unstripped)" = "bFLT" ] || { echo "build did not produce a bFLT"; exit 1; }

# Zero the bFLT build-date word (offset 0x28) so the fixture is reproducible.
python3 -c "import sys;d=bytearray(open('busybox_unstripped','rb').read());d[0x28:0x2c]=b'\0\0\0\0';open('bb.bflt','wb').write(d)"

cmake -DIN="$work/busybox-$BB_VER/bb.bflt" \
      -DOUT="$OVE_DIR/tests/ontarget/loader_hello_image.h" \
      -DSYM=ove_test_hello_bflt \
      -P "$OVE_DIR/tests/cmake/embed_bin.cmake"

echo "regenerated tests/ontarget/loader_hello_image.h from BusyBox $BB_VER hush ($(wc -c <bb.bflt) bytes)"
rm -rf "$work"
