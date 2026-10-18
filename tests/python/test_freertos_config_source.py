# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""FreeRTOS boards build against the FreeRTOSConfig.h generated from .config.

A board's own FreeRTOSConfig.h comes before the generated one on the include
path, so every CONFIG_FREERTOS_* option would silently stop applying there.
"""

import glob
import os
import unittest

OVE_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class FreeRTOSConfigSourceTest(unittest.TestCase):
    def test_no_board_ships_its_own_freertos_config(self):
        found = glob.glob(os.path.join(OVE_DIR, "boards", "**", "FreeRTOSConfig.h"), recursive=True)
        self.assertEqual([os.path.relpath(path, OVE_DIR) for path in found], [])


if __name__ == "__main__":
    unittest.main()
