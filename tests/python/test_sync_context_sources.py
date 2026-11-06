# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synchronous context detection needs its native source without async features."""

import os
from pathlib import Path
import unittest

from jinja2 import Environment, FileSystemLoader

ROOT = Path(__file__).resolve().parents[2]


class SyncContextSourcesTest(unittest.TestCase):
    def test_minimal_and_disabled_source_selection(self):
        path = Path(os.environ.get("OVE_SYNC_CONTEXT_TEMPLATE",
                                   ROOT / "config/templates/ove_config.cmake.j2"))
        template = Environment(loader=FileSystemLoader(path.parent)).get_template(path.name)
        for platform in ("freertos", "zephyr", "nuttx"):
            for sync, asynchronous in ((True, False), (True, True), (False, True), (False, False)):
                with self.subTest(platform=platform, sync=sync, asynchronous=asynchronous):
                    config = {
                        "CONFIG_OVE_RTOS_" + platform.upper(): True,
                        "CONFIG_OVE_SYNC": sync,
                        "CONFIG_OVE_ASYNC": asynchronous,
                        "CONFIG_OVE_TIME": False,
                        "CONFIG_OVE_CONSOLE": False,
                        "CONFIG_OVE_INFER": False,
                    }
                    text = template.render(config=config)
                    source = "backends/" + platform + "/" + platform + "_irq.c"
                    self.assertEqual(text.count(source), int(sync or asynchronous))
                    for other in {"freertos", "zephyr", "nuttx"} - {platform}:
                        self.assertNotIn("backends/" + other + "/" + other + "_irq.c", text)


if __name__ == "__main__":
    unittest.main()
