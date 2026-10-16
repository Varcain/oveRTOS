# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""board.yaml: schema, declared engines, upstream boards, generated Kconfig."""

import glob
import json
import os
import tempfile
import unittest

import jsonschema
import kconfiglib
import yaml
from ove.board_yaml import nuttx_board_config, rtos_entry, zephyr_board
from ove.boardgen import board_kconfig, load_boards

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

    def test_board_identity_is_unique(self):
        boards = load_boards(TREE)
        for name, data in boards:
            # OVE_BOARD_NAME is the board directory the build uses.
            self.assertEqual(data["board"]["name"], name)
        symbols = [data["board"]["symbol"] for _, data in boards]
        self.assertEqual(len(symbols), len(set(symbols)))
        self.assertLessEqual(sum(data["board"].get("default", False) for _, data in boards), 1)


# The symbols the generated board Kconfig refers to, as Config.in defines them.
_KCONFIG_CONTEXT = """
choice
    prompt "RTOS"
config OVE_RTOS_FREERTOS
    bool "FreeRTOS"
config OVE_RTOS_ZEPHYR
    bool "Zephyr"
config OVE_RTOS_NUTTX
    bool "NuttX"
endchoice
config OVE_LINUX
    bool "Linux"
config OVE_SIM
    bool
config OVE_SIM_WEB
    bool "Web"
config OVE_DEBUG_BUILD
    bool "Debug"
"""


class BoardKconfigTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        path = os.path.join(self.temp.name, "Config.in")
        with open(path, "w") as f:
            f.write(_KCONFIG_CONTEXT + board_kconfig(load_boards(TREE)))
        self.kconf = kconfiglib.Kconfig(path, warn=False)

    def tearDown(self):
        self.temp.cleanup()

    def _sym(self, name):
        return self.kconf.syms[name].str_value

    def test_default_board(self):
        self.assertEqual(self._sym("OVE_BOARD_NAME"), "stm32f746g-discovery")

    def test_board_facts_come_from_yaml(self):
        self.kconf.syms["OVE_RTOS_ZEPHYR"].set_value(2)
        self.kconf.syms["OVE_BOARD_QEMU_MPS2_AN521"].set_value(2)
        self.assertEqual(self._sym("OVE_BOARD_NAME"), "qemu-mps2-an521")
        self.assertEqual(self._sym("OVE_MCU"), "CMSDK_CM33")
        self.assertEqual(self._sym("OVE_FLASH_SIZE"), str(4096 * 1024))
        self.assertEqual(self._sym("OVE_RAM_SIZE"), str(4096 * 1024))

    def test_board_needs_a_declared_engine(self):
        self.kconf.syms["OVE_RTOS_NUTTX"].set_value(2)
        self.assertEqual(self.kconf.syms["OVE_BOARD_QEMU_MPS2_AN521"].visibility, 0)

    def test_board_kconfig_attributes(self):
        self.kconf.syms["OVE_BOARD_QEMU_MPS2_AN500"].set_value(2)
        self.assertEqual(self._sym("OVE_SIM"), "y")
        self.kconf.syms["OVE_LINUX"].set_value(2)
        self.assertEqual(self._sym("OVE_SIM"), "n")


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
