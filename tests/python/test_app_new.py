# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Regression tests for generated external-application templates."""

import os
import tempfile
import unittest

import yaml

from ove.app_new import _stamp_tree
from ove.appgen import AppManifestError, _scan_app_dirs, _scan_apps_dir


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

    def test_external_app_identity_comes_from_manifest(self):
        root, context = self._context()
        with tempfile.TemporaryDirectory(prefix="unrelated-directory-") as output:
            template = os.path.join(root, "templates", "external-app", "c")
            _stamp_tree(template, output, context)

            [(name, path, data)] = _scan_app_dirs([output])

            self.assertEqual(name, "template_smoke")
            self.assertEqual(path, os.path.abspath(output))
            self.assertEqual(data["config_name"], "template_smoke")

    def test_in_tree_scan_is_depth_independent_and_stops_at_app(self):
        root, context = self._context()
        with tempfile.TemporaryDirectory() as output:
            app = os.path.join(output, "language", "profile", "group", "app")
            template = os.path.join(root, "templates", "external-app", "c")
            _stamp_tree(template, app, context)
            nested = os.path.join(app, "vendor", "nested")
            os.makedirs(nested)
            with open(os.path.join(nested, "app.yaml"), "w") as f:
                f.write("config_name: must_not_escape_parent\n")

            apps = []
            paths = {}
            _scan_apps_dir(output, apps, paths)

            self.assertEqual(list(paths), ["template_smoke"])
            self.assertEqual(paths["template_smoke"], app)
            self.assertEqual(apps[0]["config_name"], "TEMPLATE_SMOKE")

    def test_duplicate_manifest_identities_are_rejected(self):
        with tempfile.TemporaryDirectory() as output:
            for directory in ("first", "second"):
                app = os.path.join(output, directory)
                os.makedirs(app)
                with open(os.path.join(app, "app.yaml"), "w") as f:
                    f.write("config_name: duplicate\n")

            with self.assertRaisesRegex(
                    AppManifestError, "duplicate config_name 'duplicate'"):
                _scan_apps_dir(output, [], {})

    def test_invalid_manifest_identity_is_rejected_at_source(self):
        with tempfile.TemporaryDirectory() as output:
            with open(os.path.join(output, "app.yaml"), "w") as f:
                f.write("config_name: invalid-name\n")

            with self.assertRaisesRegex(AppManifestError,
                                        "invalid config_name"):
                _scan_app_dirs([output])

    def test_hello_apps_declare_only_used_optional_modules(self):
        for language in ("c", "cpp", "rust", "zig"):
            with self.subTest(language=language):
                output, _source = self._stamp_source(
                    language,
                    "src/lib.rs" if language == "rust" else
                    "src/main.zig" if language == "zig" else
                    "src/app.cpp" if language == "cpp" else "src/app.c")
                self.addCleanup(output.cleanup)
                with open(os.path.join(output.name, "app.yaml")) as f:
                    manifest = yaml.safe_load(f)
                self.assertEqual(manifest["defconfig"], [
                    "CONFIG_OVE_CONSOLE=y",
                    "CONFIG_OVE_LOG=y",
                ])


if __name__ == "__main__":
    unittest.main()
