# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Tests for laying the browser demos out in the documentation site."""

import contextlib
import hashlib
import io
import json
import os
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

from ove import docs_demos, download


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
                mock.patch.object(docs_demos, "personality_rootfs",
                                  return_value=None), \
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
                mock.patch.object(docs_demos, "personality_rootfs",
                                  return_value=None), \
                contextlib.redirect_stdout(io.StringIO()), \
                self.assertLogs("ove", level="ERROR"), \
                self.assertRaises(SystemExit) as raised:
            docs_demos._cmd_docs_demos(SimpleNamespace(site=None))
        self.assertEqual(raised.exception.code, 1)


    def _terminal_bundle(self, rtos):
        """A Linux personality workspace's bundle: a terminal page."""
        serve = os.path.join(self.root, "output", "web", rtos,
                             "linux_interop_minimal", "serve")
        for filename in docs_demos._TERMINAL_FILES:
            self._write(os.path.join(serve, filename), f"{rtos}:{filename}")
        self._write(os.path.join(serve, "web.json"), json.dumps(
            {"firmware": "firmware.elf", "emulator": "qemu/",
             "files": {"rootfs.cpio": "/rootfs.cpio"}, "args": []}))
        os.symlink(self.emulator, os.path.join(serve, "qemu"))
        return serve

    def test_personality_demos_join_the_gallery(self):
        built = [
            {"name": "linux-freertos", "rtos": "freertos", "ok": True,
             "serve": self._terminal_bundle("freertos")},
            {"name": "linux-nuttx", "rtos": "nuttx", "ok": True,
             "serve": self._terminal_bundle("nuttx")},
            {"name": "linux-zephyr", "rtos": "zephyr", "ok": False,
             "serve": os.path.join(self.root, "absent")},
        ]
        ws = SimpleNamespace(ove_dir=self.root)
        smoke = mock.Mock(side_effect=lambda serve: (
            "nuttx" not in serve, "smoke: no login prompt"))
        with mock.patch.object(docs_demos, "Workspace", return_value=ws), \
                mock.patch.object(docs_demos, "build_matrix",
                                  return_value=[]), \
                mock.patch.object(docs_demos, "personality_rootfs",
                                  return_value="/rootfs.cpio"), \
                mock.patch.object(docs_demos, "build_personality_demos",
                                  return_value=built) as build, \
                mock.patch.object(docs_demos, "_smoke_terminal", smoke), \
                contextlib.redirect_stdout(io.StringIO()):
            docs_demos._cmd_docs_demos(SimpleNamespace(site=None))

        build.assert_called_once_with(ws, "/rootfs.cpio")
        self.assertEqual(sorted(os.listdir(self.site)),
                         ["examples.html", "linux-freertos", "qemu"])
        self.assertEqual(
            sorted(os.listdir(os.path.join(self.site, "linux-freertos"))),
            sorted(docs_demos._TERMINAL_FILES + ("qemu", "web.json")))
        with open(os.path.join(self.site, "examples.html")) as f:
            gallery = f.read()
        self.assertIn('href="linux-freertos/"', gallery)
        self.assertIn("Linux personality on FreeRTOS", gallery)
        self.assertNotIn("linux-nuttx", gallery)

    def test_linux_rootfs_download_is_checked(self):
        image = os.path.join(self.root, "published.cpio")
        self._write(image, "070701rootfs")
        with open(image, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        dl_dir = os.path.join(self.root, "dl")

        def manifest(sha256):
            return {"tools": {"linux-rootfs": {
                "url": "file://" + image, "sha256": sha256}}}

        with contextlib.redirect_stdout(io.StringIO()):
            path = download.download_linux_rootfs(manifest(digest), dl_dir)
            self.assertEqual(path, os.path.join(
                dl_dir, f"linux-rootfs-{digest[:12]}.cpio"))
            with open(path) as f:
                self.assertEqual(f.read(), "070701rootfs")
            # A cached copy that still matches is used as it is.
            with mock.patch.object(download.urllib.request,
                                   "urlretrieve") as fetch:
                self.assertEqual(
                    download.download_linux_rootfs(manifest(digest), dl_dir),
                    path)
            fetch.assert_not_called()

            with self.assertLogs("ove", level="WARNING"):
                self.assertIsNone(download.download_linux_rootfs(
                    manifest("0" * 64), dl_dir))
        self.assertEqual(sorted(os.listdir(dl_dir)), [os.path.basename(path)])
        self.assertIsNone(download.download_linux_rootfs({}, dl_dir))
        self.assertIsNone(download.download_linux_rootfs(
            {"tools": {"linux-rootfs": {"sha256": digest}}}, dl_dir))


if __name__ == "__main__":
    unittest.main()
