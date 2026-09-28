#!/usr/bin/env python3
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Select the rootfs image a QEMU MPS2 Linux-personality run loads.

The full Buildroot image is used whenever it fits the board's rootfs window.
Otherwise a copy without the optional applications listed below is written to
the output path and used instead. Neither boot nor the QEMU test suites use
them. Entries are filtered in the newc stream itself, so every retained
header, mode, owner and device node is copied byte for byte.

Usage: fit-rootfs.py <rootfs.cpio> <window-bytes> <output.cpio>
Prints the path of the image to load; exits non-zero if neither fits.
"""

import os
import sys

# Optional applications dropped from images that exceed the window.
OPTIONAL = (
    "usr/bin/sqlite3",
    "usr/lib/libsqlite3.so",
    "usr/lib/libsqlite3.so.0",
    "usr/lib/libsqlite3.so.3.53.2",
    "usr/bin/lvmusic",
    "usr/bin/iconv",
    "sbin/fsck.fat",
)

HEADER = 110
TRAILER = b"TRAILER!!!"


def _align4(n):
    return (n + 3) & ~3


def filter_newc(data, drop):
    """Return the newc archive without entries whose names are in drop."""
    out = bytearray()
    pos = 0
    while True:
        hdr = data[pos:pos + HEADER]
        if len(hdr) != HEADER or hdr[:6] != b"070701":
            raise ValueError("not a newc cpio archive at offset %d" % pos)
        filesize = int(hdr[54:62], 16)
        namesize = int(hdr[94:102], 16)
        name_end = pos + HEADER + namesize
        name = data[pos + HEADER:name_end - 1]
        data_start = _align4(name_end)
        end = _align4(data_start + filesize)
        if name.decode("latin-1").lstrip("./") not in drop:
            out += data[pos:end]
        pos = end
        if name == TRAILER:
            return bytes(out)


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    src, limit, dst = argv[1], int(argv[2], 0), argv[3]
    if os.path.getsize(src) <= limit:
        print(src)
        return 0
    if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
        with open(src, "rb") as f:
            fitted = filter_newc(f.read(), set(OPTIONAL))
        with open(dst + ".tmp", "wb") as f:
            f.write(fitted)
        os.replace(dst + ".tmp", dst)
    size = os.path.getsize(dst)
    if size > limit:
        sys.stderr.write("[qemu-run] ERROR: rootfs is %d bytes even without optional "
                         "applications; configured window is %d\n" % (size, limit))
        return 1
    print(dst)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
