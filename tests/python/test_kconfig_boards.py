# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Regression tests for board short-name resolution in fragment specs."""

import os
import tempfile
import unittest

from ove.kconfig import _find_board_dir


class FindBoardDirTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = self.temp.name
        # A shared support directory that sorts before the boards it serves.
        self._mkdir("qemu-mps2", "lxp_console.c")
        self._mkdir("qemu-mps2-an500", "board.yaml")
        self._mkdir("qemu-mps2-an521", "board.yaml")
        self._mkdir("stm32f746g-discovery", "board.yaml")

    def tearDown(self):
        self.temp.cleanup()

    def _mkdir(self, name, filename):
        path = os.path.join(self.root, "boards", name)
        os.makedirs(path)
        open(os.path.join(path, filename), "w").close()

    def _resolve(self, short):
        path = _find_board_dir(self.root, short)
        return os.path.basename(path) if path else None

    def test_short_name_skips_directory_without_board_yaml(self):
        self.assertEqual(self._resolve("qemu"), "qemu-mps2-an500")
        self.assertEqual(self._resolve("qemu-mps2"), "qemu-mps2-an500")

    def test_exact_and_prefix_names_resolve_to_boards(self):
        self.assertEqual(self._resolve("qemu-mps2-an521"), "qemu-mps2-an521")
        self.assertEqual(self._resolve("stm32f746"), "stm32f746g-discovery")
        self.assertIsNone(self._resolve("wasm"))


if __name__ == "__main__":
    unittest.main()
