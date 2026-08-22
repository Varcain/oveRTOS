# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Regression tests for generated external-application templates."""

import os
import tempfile
import unittest

from ove.app_new import _stamp_tree


class ExternalAppTemplateTest(unittest.TestCase):
    def _context(self):
        root = os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))))
        return root, {
            "NAME": "template-smoke",
            "CONFIG_NAME": "template_smoke",
            "CONFIG_NAME_UPPER": "TEMPLATE_SMOKE",
            "LIB_NAME": "template_smoke",
            "OVE_DIR": root,
        }

    def _stamp_source(self, language, source):
        root, context = self._context()
        output = tempfile.TemporaryDirectory()
        template = os.path.join(root, "templates", "external-app", language)
        _stamp_tree(template, output.name, context)
        with open(os.path.join(output.name, source)) as f:
            generated = f.read()
        return output, generated

    def test_c_family_hello_apps_use_current_logging_api(self):

        for language, source in (("c", "src/app.c"),
                                 ("cpp", "src/app.cpp")):
            with self.subTest(language=language):
                output, generated = self._stamp_source(language, source)
                self.addCleanup(output.cleanup)
                self.assertIn("OVE_LOG_INF", generated)
                self.assertNotIn("OVE_LOG_MODULE_REGISTER", generated)
                self.assertNotIn("{{", generated)

    def test_language_templates_use_current_sleep_api(self):
        expected = {
            "c": ("src/app.c", "ove_thread_sleep_ms(1000)"),
            "cpp": ("src/app.cpp", "ove::this_thread::sleep_ms(1000)"),
            "rust": ("src/lib.rs", "ove::Thread::sleep_ms(1000)"),
        }
        for language, (source, call) in expected.items():
            with self.subTest(language=language):
                output, generated = self._stamp_source(language, source)
                self.addCleanup(output.cleanup)
                self.assertIn(call, generated)
                self.assertNotIn("{{", generated)


if __name__ == "__main__":
    unittest.main()
