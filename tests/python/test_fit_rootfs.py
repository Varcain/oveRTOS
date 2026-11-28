# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check which entries fit-rootfs.py keeps and when it rewrites its output."""

import contextlib
import importlib.util
import io
import os
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "boards/qemu-mps2/fit-rootfs.py"
spec = importlib.util.spec_from_file_location("fit_rootfs", SCRIPT)
fit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fit)


def newc(entries):
    """Return a newc archive holding regular files {name: data}."""
    out = b""
    for name, data in list(entries.items()) + [("TRAILER!!!", b"")]:
        fields = [0, 0o100755, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name) + 1, 0]
        out += b"070701" + b"".join(b"%08X" % f for f in fields) + name.encode() + b"\0"
        out += b"\0" * (-len(out) % 4) + data
        out += b"\0" * (-len(out) % 4)
    return out


class FitRootfsTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.src, self.dst = Path(tmp.name, "rootfs.cpio"), Path(tmp.name, "fit.cpio")
        self.src.write_bytes(newc({"usr/bin/lvmusic": b"m" * 64,
                                   "usr/lib/libsqlite3.so.0": b"l" * 64,
                                   "usr/lib/libsqlite3.so.3.53.2": b"r" * 64,
                                   "usr/bin/sqlite3": b"s" * 4096}))
        self.argv = ["fit-rootfs.py", str(self.src), "2048", str(self.dst)]

    def fit(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(fit.main(self.argv), 0)

    def test_drops_only_leaf_tools(self):
        self.fit()
        names = self.dst.read_bytes()
        self.assertIn(b"usr/bin/lvmusic\0", names)
        self.assertIn(b"usr/lib/libsqlite3.so.0\0", names)
        self.assertIn(b"usr/lib/libsqlite3.so.3.53.2\0", names)
        self.assertNotIn(b"usr/bin/sqlite3\0", names)

    def test_rewrites_output_older_than_script(self):
        self.dst.write_bytes(b"stale")
        os.utime(self.src, (100, 100))
        os.utime(self.dst, (200, 200))
        self.fit()
        self.assertEqual(self.dst.read_bytes(),
                         fit.filter_newc(self.src.read_bytes(), set(fit.OPTIONAL)))


if __name__ == "__main__":
    unittest.main()
