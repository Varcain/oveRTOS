# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Tests for laying the browser demos out in the documentation site."""

import contextlib
import io
import json
import os
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

from ove import docs_demos


class DocsDemosTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = self.temp.name
        self.site = os.path.join(self.root, "docs-site", "site")
        self.emulator = os.path.join(self.root, "output", "tools", "qemu")
        os.makedirs(self.emulator)
        for name in ("qemu-system-arm.js", "qemu-system-arm.wasm",
                     "SOURCE.txt"):
            self._write(os.path.join(self.emulator, name), name)

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def _write(path, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)

    def _bundle(self, name):
        """A workspace as 'ove build' leaves it for the web variant."""
        workspace = os.path.join(self.root, "output", "web", "freertos", name)
        serve = os.path.join(workspace, "serve")
        for filename in docs_demos._DEMO_FILES:
            self._write(os.path.join(serve, filename), f"{name}:{filename}")
        self._write(os.path.join(serve, "web.json"), json.dumps(
            {"firmware": "firmware.elf", "emulator": "qemu/",
             "machine": "mps2-an500"}))
        os.symlink(self.emulator, os.path.join(serve, "qemu"))
        self._write(os.path.join(workspace, "images", "firmware.elf"), name)
        return serve

    def _app(self, name, lang, description):
        self._write(os.path.join(self.root, "apps", lang, name, "app.yaml"),
                    f"lang: {lang}\nconfig_name: {name}\n"
                    f'description: "{description}"\n')

    def test_demo_shares_the_emulator_binary(self):
        docs_demos.assemble_demo(self._bundle("demo_a"), self.site, "demo_a")

        dest = os.path.join(self.site, "demo_a")
        self.assertEqual(sorted(os.listdir(dest)), sorted(
            docs_demos._DEMO_FILES + ("qemu", "web.json")))
        # Only the script sits below the page; the binary is site-wide.
        self.assertEqual(os.listdir(os.path.join(dest, "qemu")),
                         ["qemu-system-arm.js"])
        self.assertEqual(sorted(os.listdir(os.path.join(self.site, "qemu"))),
                         ["SOURCE.txt", "qemu-system-arm.wasm"])
        with open(os.path.join(dest, "web.json")) as f:
            config = json.load(f)
        self.assertEqual(config["wasm"], "../qemu/qemu-system-arm.wasm")
        self.assertEqual(config["firmware"], "firmware.elf")

    def test_remove_demos_leaves_pages_alone(self):
        docs_demos.assemble_demo(self._bundle("demo_a"), self.site, "demo_a")
        self._write(os.path.join(self.site, "examples", "index.html"), "page")

        docs_demos.remove_demos(self.site, ["demo_a", "examples", "absent"])

        self.assertFalse(os.path.exists(os.path.join(self.site, "demo_a")))
        self.assertTrue(os.path.isfile(
            os.path.join(self.site, "examples", "index.html")))

    def test_command_publishes_only_what_builds_and_starts(self):
        bundles = {name: self._bundle(name)
                   for name in ("built", "faults", "unbuilt", "stale")}
        self._app("built", "cpp", "A <demo>")
        # A demo from an earlier run, for an app the variant now skips.
        docs_demos.assemble_demo(bundles["stale"], self.site, "stale")

        results = [
            {"app": "built", "ok": True, "skipped": False},
            {"app": "faults", "ok": True, "skipped": False},
            {"app": "unbuilt", "ok": False, "skipped": False},
            {"app": "stale", "ok": True, "skipped": True},
        ]
        ws = SimpleNamespace(ove_dir=self.root)
        smoke = mock.Mock(side_effect=lambda _ove, elf: (
            "faults" not in elf, "smoke: the app reported a failure"))
        with mock.patch.object(docs_demos, "Workspace", return_value=ws), \
                mock.patch.object(docs_demos, "build_matrix",
                                  return_value=results), \
                mock.patch.object(docs_demos, "_smoke", smoke), \
                contextlib.redirect_stdout(io.StringIO()):
            docs_demos._cmd_docs_demos(SimpleNamespace(site=None))

        self.assertEqual(
            sorted(os.listdir(self.site)), ["built", "examples.html", "qemu"])
        with open(os.path.join(self.site, "examples.html")) as f:
            gallery = f.read()
        self.assertIn('href="built/"', gallery)
        self.assertIn("A &lt;demo&gt;", gallery)
        self.assertIn("C++ &middot;", gallery)
        self.assertNotIn("faults", gallery)

    def test_command_fails_when_nothing_can_be_published(self):
        ws = SimpleNamespace(ove_dir=self.root)
        results = [{"app": "unbuilt", "ok": False, "skipped": False}]
        with mock.patch.object(docs_demos, "Workspace", return_value=ws), \
                mock.patch.object(docs_demos, "build_matrix",
                                  return_value=results), \
                contextlib.redirect_stdout(io.StringIO()), \
                self.assertLogs("ove", level="ERROR"), \
                self.assertRaises(SystemExit) as raised:
            docs_demos._cmd_docs_demos(SimpleNamespace(site=None))
        self.assertEqual(raised.exception.code, 1)


if __name__ == "__main__":
    unittest.main()
