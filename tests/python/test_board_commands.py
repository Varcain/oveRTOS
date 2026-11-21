# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import contextlib
import io
import re
import runpy
import unittest
from pathlib import Path
from types import SimpleNamespace as NS
from unittest.mock import ANY, patch

from ove import cli, completion, workspace


class BoardCommandsTest(unittest.TestCase):
    def test_completion_only_offers_current_board_commands(self):
        for shell in ("bash", "zsh", "fish"):
            with self.subTest(shell=shell):
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    completion._emit(shell)
                commands = re.search(r"^.*\blist\b.*\bimport\b.*$",
                                     output.getvalue(), re.MULTILINE)
                self.assertIsNotNone(commands)
                self.assertNotRegex(commands.group(), r"\b(?:register|sync)\b")

    def test_import_forwards_options(self):
        argv = ["ove", "board", "import", "--rtos", "nuttx", "--name", "demo",
                "--config", "netnsh", "--non-interactive", "--overwrite"]
        with patch("sys.argv", argv), \
                patch("ove.board._find_nuttx_base", return_value=None), \
                patch("ove.board._board_tool") as run:
            cli.main()
        run.assert_called_once_with(
            ANY, ["import", "nuttx", "--board", "demo", "--config", "netnsh",
                  "--non-interactive", "--overwrite"])

    def test_zephyr_import_delegates_source_selection(self):
        argv = ["ove", "board", "import", "--rtos", "zephyr", "--name", "demo"]
        with patch("sys.argv", argv), \
                patch("ove.board._find_nuttx_base") as find_nuttx, \
                patch("ove.board._board_tool") as run:
            cli.main()
        find_nuttx.assert_not_called()
        run.assert_called_once_with(ANY, ["import", "zephyr", "--board", "demo"])

    def test_board_tool_uses_workspace_config_parser(self):
        root = Path(__file__).resolve().parents[2]
        tool = runpy.run_path(str(root / "config/scripts/board_tool.py"))
        self.assertIs(tool["parse_dotconfig"], workspace.parse_dotconfig)

    def test_zephyr_edt_maps_chosen_hardware(self):
        def node(path, compat=(), regs=(), aliases=(), on_buses=(), binding_path=None, **props):
            return NS(path=path, name=path.rsplit("/", 1)[-1], compats=compat,
                      regs=[NS(addr=addr, size=size) for addr, size in regs],
                      props={key: NS(val=value) for key, value in props.items()},
                      status="okay", aliases=aliases, on_buses=on_buses, binding_path=binding_path)

        soc = node("/soc", ("st,stm32f746", "st,stm32f7", "simple-bus"))
        gpio = NS(controller=NS(labels=["gpioi"], status="okay"),
                  data={"pin": 1, "flags": 1})
        led = node("/leds/green", aliases=("led0",), gpios=[gpio])
        button = node("/keys/user", aliases=("sw0",), gpios=[gpio])
        sdram = node("/sdram@c0000000", regs=[(0xc0000000, 8 << 20)])
        sdmmc = node("/soc/mmc@40012c00", ("st,stm32-sdmmc",))
        haptics = node("/i2c/haptics@5a", ("cirrus,cs40l5x",), on_buses=("i2c",),
                       binding_path="/dts/bindings/haptics/cirrus,cs40l5x-i2c.yaml")
        codec = node("/i2c/audio@10", ("dlg,da7212",), regs=[(0x10, None)], on_buses=("i2c",),
                     binding_path="/dts/bindings/audio/dlg,da7212.yaml")
        edt = NS(
            nodes=[soc, led, button, sdram, sdmmc, haptics, codec],
            compat2okay={},
            chosen_nodes={
                "zephyr,sram": node("/ram", regs=[(0x20010000, 256 << 10)]),
                "zephyr,dtcm": node("/dtcm", regs=[(0x20000000, 64 << 10)]),
                "zephyr,flash": node("/flash", regs=[(0x08000000, 1 << 20)]),
                "zephyr,console": node("/uart", **{"current-speed": 115200}),
                "zephyr,display": node("/display", width=480, height=272),
            },
            label2node={"rcc": node("/rcc", **{"clock-frequency": 216000000}),
                        "clk_hse": node("/hse", **{"clock-frequency": 25000000})},
        )
        root = Path(__file__).resolve().parents[2]
        tool = runpy.run_path(str(root / "config/scripts/board_tool.py"))
        extract = tool["_extract_board_yaml_from_edt"]
        data, warnings = extract(edt, "stm32f746g_disco")
        self.assertEqual(data, {
            "board": {"name": "stm32f746g-disco", "mcu_family": "stm32f7",
                      "mcu": "STM32F746xx"},
            "memory": {"ram_kb": 320, "ram_start": "0x20000000", "flash_kb": 1024,
                       "flash_start": "0x08000000", "sdram_kb": 8192,
                       "sdram_start": "0xC0000000"},
            "serial": {"console": {"baud": 115200}},
            "leds": [{"port": 8, "pin": 1, "active_low": True}],
            "clock": {"sysclk_mhz": 216, "hse_freq_mhz": 25},
            "display": {"width": 480, "height": 272},
            "sdcard": {},
            "audio": {"codec": {"type": "DA7212", "i2c_addr": "0x10"}},
        })
        self.assertTrue(any("/keys/user" in warning for warning in warnings))

        edt.chosen_nodes["zephyr,sram"] = edt.chosen_nodes["zephyr,dtcm"]
        data, _ = extract(edt, "stm32f746g_disco")
        self.assertEqual(data["memory"]["ram_kb"], 64)
