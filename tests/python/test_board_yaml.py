# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""board.yaml: schema, declared engines, and the upstream board lookup."""

import glob
import json
import os
import tempfile
import unittest

import jsonschema
import yaml
from ove.board_yaml import nuttx_board_config, rtos_entry, zephyr_board

TREE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RTOSES = ("freertos", "nuttx", "zephyr")


def _boards():
    return sorted(os.path.dirname(p) for p in
                  glob.glob(os.path.join(TREE, "boards", "*", "board.yaml")))


class TreeBoardsTest(unittest.TestCase):
    def test_every_board_matches_the_schema(self):
        with open(os.path.join(TREE, "config", "schemas", "board_schema.json")) as f:
            schema = json.load(f)
        for board in _boards():
            with self.subTest(board=os.path.basename(board)):
                with open(os.path.join(board, "board.yaml")) as f:
                    data = yaml.safe_load(f)
                jsonschema.validate({k: v for k, v in data.items() if k != "_meta"}, schema)

    def test_declared_engines_match_board_directories(self):
        for board in _boards():
            for rtos in RTOSES:
                with self.subTest(board=os.path.basename(board), rtos=rtos):
                    self.assertEqual(rtos_entry(board, rtos) is not None,
                                     os.path.isdir(os.path.join(board, rtos)))


class LookupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.board = self.temp.name

    def tearDown(self):
        self.temp.cleanup()

    def _write(self, text):
        with open(os.path.join(self.board, "board.yaml"), "w") as f:
            f.write(text)

    def test_upstream_boards(self):
        self._write("rtos:\n  freertos: {}\n"
                    "  nuttx: {board: stm32f746g-disco, config: nsh}\n"
                    "  zephyr: {board: mps2/an521/cpu0}\n")
        self.assertEqual(rtos_entry(self.board, "freertos"), {})
        self.assertEqual(nuttx_board_config(self.board), "stm32f746g-disco:nsh")
        self.assertEqual(zephyr_board(self.board), "mps2/an521/cpu0")

    def test_undeclared_engine_has_no_upstream_board(self):
        self._write("rtos:\n  zephyr: {board: mps2/an521/cpu0}\n")
        self.assertIsNone(rtos_entry(self.board, "nuttx"))
        self.assertIsNone(nuttx_board_config(self.board))
        self.assertIsNone(rtos_entry(self.board, "freertos"))

    def test_missing_board_yaml(self):
        self.assertIsNone(zephyr_board(self.board))


if __name__ == "__main__":
    unittest.main()
