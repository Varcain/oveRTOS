# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""board.yaml: schema, declared engines, upstream boards, generated Kconfig."""

import contextlib
import glob
import io
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


class UndefinedSymbolTest(unittest.TestCase):
    def test_only_unknown_symbols_are_reported(self):
        from ove.kconfig import _undefined_symbols

        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "Config.in")
            with open(path, "w") as f:
                f.write(_KCONFIG_CONTEXT + "config USES_GHOST\n    bool\n    depends on GHOST\n")
            kconf = kconfiglib.Kconfig(path, warn=False)
        lines = ["CONFIG_OVE_LINUX=y", "# CONFIG_OVE_SIM_WEB is not set",
                 "CONFIG_NO_SUCH=y", "# CONFIG_GHOST is not set", "# a comment"]
        # GHOST is referenced by a dependency but defined nowhere.
        self.assertEqual(_undefined_symbols(kconf, lines), ["NO_SUCH", "GHOST"])


class ConsoleTest(unittest.TestCase):
    def test_nuttx_boards_name_their_console_uart(self):
        for board in _boards():
            if rtos_entry(board, "nuttx") is None:
                continue
            with self.subTest(board=os.path.basename(board)):
                with open(os.path.join(board, "board.yaml")) as f:
                    console = yaml.safe_load(f)["serial"]["console"]
                self.assertIn("instance", console)

    def test_rx_buffer_must_be_a_power_of_two(self):
        from ove.configure.board_desc import _generate_board_desc_header

        board = {"board": {"name": "b", "mcu_family": "f", "mcu": "m"},
                 "serial": {"console": {"rx_buffer_size": 1000}}}
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
            _generate_board_desc_header(board)
        board["serial"]["console"]["rx_buffer_size"] = 1024
        self.assertIn("OVE_SERIAL_CONSOLE_RX_BUFFER_SIZE 1024",
                      _generate_board_desc_header(board))

    def test_nuttx_defconfig_takes_the_console_from_board_yaml(self):
        text = _render_nuttx_defconfig({"serial": {"console": {
            "instance": "USART1", "baud": 115200, "rx_buffer_size": 1024}}})
        self.assertIn("CONFIG_USART1_BAUD=115200\n", text)
        self.assertIn("CONFIG_USART1_RXBUFSIZE=1024\n", text)


def _render_nuttx_defconfig(board):
    from jinja2 import Environment, FileSystemLoader

    env = Environment(loader=FileSystemLoader(os.path.join(TREE, "config", "templates")),
                      trim_blocks=True, lstrip_blocks=True)
    env.globals.update(config={}, get_bool=lambda k, d=False: d,
                       get_int=lambda k, d=0: d, get_str=lambda k, d="": d, board=board)
    return env.get_template("nuttx_defconfig.j2").render()


DISPLAY = {"width": 480, "height": 272, "color_format": "RGB565"}


class MemoryTest(unittest.TestCase):
    def test_board_desc_places_flash_and_ram(self):
        from ove.configure.board_desc import _generate_board_desc_header

        header = _generate_board_desc_header({
            "board": {"name": "b", "mcu_family": "f", "mcu": "m"},
            "memory": {"flash_start": "0x08000000", "flash_kb": 1024,
                       "ram_start": "0x20000000", "ram_kb": 320}})
        for line in ("#define OVE_MEMORY_FLASH_START 0x08000000", "#define OVE_MEMORY_FLASH_KB 1024",
                     "#define OVE_MEMORY_RAM_START 0x20000000", "#define OVE_MEMORY_RAM_KB 320"):
            self.assertIn(line, header)


class DisplayTest(unittest.TestCase):
    def test_board_desc_names_the_pixel_format(self):
        from ove.configure.board_desc import _generate_board_desc_header

        header = _generate_board_desc_header({
            "board": {"name": "b", "mcu_family": "f", "mcu": "m"}, "display": DISPLAY})
        self.assertIn("#define OVE_DISPLAY_FORMAT_RGB565 1", header)
        self.assertIn("#define OVE_DISPLAY_BPP 2", header)

    def test_stm32f7_ltdc_framebuffer_comes_from_board_yaml(self):
        text = _render_nuttx_defconfig({"board": {"mcu_family": "stm32f7"}, "display": DISPLAY,
                                        "memory": {"sdram_start": "0xC0000000"}})
        self.assertIn("CONFIG_STM32F7_LTDC_FB_BASE=0xC0000000\n", text)
        self.assertIn("CONFIG_STM32F7_LTDC_FB_SIZE=261120\n", text)
        self.assertNotIn("LTDC_FB_SIZE", _render_nuttx_defconfig(
            {"board": {"mcu_family": "arm"}, "display": DISPLAY}))
