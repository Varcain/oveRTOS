# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""The configuration layers `ove defconfig-fragments` merges: order and checks."""

import contextlib
import io
import os
import tempfile
import textwrap
import unittest
from types import SimpleNamespace
from unittest import mock

from ove import kconfig

# A minimal tree: the RTOS fragment and the board both set OVE_FOO.
TREE = {
    "Config.in": """
        source "output/kconfig/apps/Config.in"
        choice
            prompt "RTOS"
        config OVE_RTOS_FREERTOS
            bool "FreeRTOS"
        config OVE_RTOS_ZEPHYR
            bool "Zephyr"
        config OVE_RTOS_NUTTX
            bool "NuttX"
        endchoice
        source "output/kconfig/boards/Config.in"
        config OVE_FOO
            int "foo"
            default 0
        """,
    "config/fragments/global.defconfig": "",
    "config/fragments/rtos/freertos.defconfig": """
        CONFIG_OVE_RTOS_FREERTOS=y
        CONFIG_OVE_FOO=1
        """,
    "boards/demo-board/board.yaml": """
        board:
          name: demo-board
          mcu_family: arm
          mcu: M
          symbol: OVE_BOARD_DEMO
          title: Demo
        rtos:
          freertos: {}
        defconfig:
          - CONFIG_OVE_FOO=2
        """,
    "apps/c/demo/app.yaml": """
        lang: c
        config_name: demo
        description: "Demo"
        """,
}


class LayerOrderTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = self.temp.name
        for path, text in TREE.items():
            full = os.path.join(self.root, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w") as f:
                f.write(textwrap.dedent(text).lstrip())

    def tearDown(self):
        self.temp.cleanup()

    def _foo(self):
        seen = {}

        def capture(kconf, *args, **kwargs):
            seen["foo"] = kconf.syms["OVE_FOO"].str_value
            return self.root, os.path.join(self.root, ".config")

        args = SimpleNamespace(spec="demo-board.freertos.demo", no_activate=True)
        with mock.patch.object(kconfig, "find_ove_dir", return_value=self.root), \
                mock.patch.object(kconfig, "_write_workspace_config", side_effect=capture), \
                mock.patch.dict(os.environ, {"srctree": self.root}), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            kconfig.cmd_defconfig_fragments(args)
        return seen["foo"]

    def test_board_defconfig_overrides_the_rtos_fragment(self):
        self.assertEqual(self._foo(), "2")

    def test_unknown_symbol_in_an_app_line_fails(self):
        with open(os.path.join(self.root, "apps/c/demo/app.yaml"), "a") as f:
            f.write("defconfig:\n  - CONFIG_OVE_NO_SUCH=y\n")
        with self.assertRaises(SystemExit):
            self._foo()


if __name__ == "__main__":
    unittest.main()
