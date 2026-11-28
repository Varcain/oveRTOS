# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Tests for laying the browser demos out in the documentation site."""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ove import allconfigs, build, docs_demos, download, kconfig
from ove.workspace import WORKSPACE_DIR_ENV

_REPO = Path(__file__).resolve().parents[2]


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

    def _bundle(self, workspace, args=None):
        """A workspace as 'ove build' leaves it for a web variant."""
        serve = os.path.join(workspace, "serve")
        for filename in docs_demos._DEMO_FILES:
            self._write(os.path.join(serve, filename), filename)
        web = {"firmware": "firmware.elf", "emulator": "qemu/"}
        if args is not None:
            web.update(files={"rootfs.cpio": "/rootfs.cpio"}, args=args)
            self._write(os.path.join(serve, "rootfs.cpio"), "rootfs")
        self._write(os.path.join(serve, "web.json"), json.dumps(web))
        os.symlink(self.emulator, os.path.join(serve, "qemu"))
        self._write(os.path.join(workspace, "images", "firmware.elf"), "elf")
        return serve

    def test_web_bundles_include_local_page_assets(self):
        dashboard = _REPO / "sim" / "dashboard"
        terminal_html = (dashboard / "terminal.html").read_text()
        dashboard_html = build._web_index_html(dashboard)
        board_js = (dashboard / "board.js").read_text()
        references = re.compile(r'(?:src|href)="([^":]+)"|loadScript\("([^":]+)"\)')
        for source, pages in (
            (terminal_html, build._TERMINAL_PAGES),
            (dashboard_html, build._DASHBOARD_PAGES),
            (board_js, build._DASHBOARD_PAGES),
        ):
            for src, script in references.findall(source):
                self.assertIn(src or script, pages)

    def test_demo_shares_the_emulator_binary(self):
        serve = self._bundle(os.path.join(self.root, "ws"))
        docs_demos.assemble_demo(serve, self.site, "freertos/demo_a")

        dest = os.path.join(self.site, "freertos", "demo_a")
        self.assertEqual(sorted(os.listdir(dest)), sorted(
            docs_demos._DEMO_FILES + ("qemu", "web.json")))
        # Only the script sits below the page; the binary is shared.
        self.assertEqual(os.listdir(os.path.join(dest, "qemu")),
                         ["qemu-system-arm.js"])
        self.assertEqual(sorted(os.listdir(os.path.join(self.site, "qemu"))),
                         ["SOURCE.txt", "qemu-system-arm.wasm"])
        with open(os.path.join(dest, "web.json")) as f:
            config = json.load(f)
        self.assertEqual(config["wasm"], "../../qemu/qemu-system-arm.wasm")
        self.assertEqual(config["firmware"], "firmware.elf")

    def _run_command(self, specs, failing=()):
        """Run 'ove docs-demos' over *specs* with every build stage faked.

        Returns the specs configured and the environment each was built in.
        """
        self._write(os.path.join(self.root, "docs-site", "demos.yml"),
                    "".join(f"- {spec}\n" for spec in specs))
        for name, linux in (("example_c_heap", False), ("example_c_zh", False),
                            ("unlisted", False), ("linux_interop", True)):
            self._write(os.path.join(self.root, "apps", name, "app.yaml"),
                        f"config_name: {name}\ndefconfig:\n  - "
                        f"CONFIG_OVE_LINUX={'y' if linux else 'n'}\n")
        built, spec = {}, None

        def stage(cmd, env, _cwd):
            nonlocal spec
            if cmd[1] == "defconfig-fragments":
                spec = cmd[2]
            elif cmd[1] == "build":
                built[spec] = env.get(build.LINUX_ROOTFS_ENV)
                if spec in failing:
                    return 1
                linux = spec.endswith("linux_interop")
                self._bundle(env[WORKSPACE_DIR_ENV], [] if linux else None)
            return 0

        ws = SimpleNamespace(ove_dir=self.root)
        with mock.patch.object(docs_demos, "Workspace", return_value=ws), \
                mock.patch.object(allconfigs, "_run_command", stage), \
                mock.patch.object(docs_demos, "_smoke",
                                  return_value=(True, "")), \
                mock.patch.object(docs_demos, "_smoke_terminal",
                                  return_value=(True, "")), \
                mock.patch.object(docs_demos, "personality_rootfs",
                                  return_value="/rootfs.cpio"), \
                contextlib.redirect_stdout(io.StringIO()):
            docs_demos._cmd_docs_demos(SimpleNamespace(site=None))
        return built

    def test_command_builds_only_the_listed_demos(self):
        specs = ["web.freertos.example_c_heap", "web.nuttx.example_c_zh",
                 "web-an521.zephyr.linux_interop"]
        self.assertEqual(self._run_command(specs), {
            "web.freertos.example_c_heap": None,
            "web.nuttx.example_c_zh": None,
            "web-an521.zephyr.linux_interop": "/rootfs.cpio"})
        demos = os.path.join(self.site, "demos")
        for name in ("freertos/example_c_heap", "nuttx/example_c_zh",
                     "zephyr/linux_interop"):
            with open(os.path.join(demos, name, "web.json")) as f:
                self.assertEqual(json.load(f)["wasm"],
                                 "../../qemu/qemu-system-arm.wasm")
        self.assertEqual(sorted(os.listdir(demos)),
                         ["freertos", "nuttx", "qemu", "zephyr"])

    def test_failed_demo_gets_a_tbd_page(self):
        self._run_command(["web.freertos.example_c_heap",
                           "web.nuttx.example_c_heap"],
                          failing=("web.nuttx.example_c_heap",))
        dest = os.path.join(self.site, "demos", "nuttx", "example_c_heap")
        self.assertEqual(os.listdir(dest), ["index.html"])
        with open(os.path.join(dest, "index.html")) as f:
            self.assertIn("TBD", f.read())

    def test_command_fails_when_nothing_can_be_published(self):
        with self.assertLogs("ove", level="ERROR"), \
                self.assertRaises(SystemExit) as raised:
            self._run_command(["web.freertos.example_c_heap"],
                              failing=("web.freertos.example_c_heap",))
        self.assertEqual(raised.exception.code, 1)

    def test_listed_demos_name_real_configurations(self):
        apps = docs_demos._app_metadata(str(_REPO))
        for spec in docs_demos.load_demos(str(_REPO)):
            board, rtos, app = spec.split(".")
            self.assertIn(rtos, ("freertos", "nuttx", "zephyr"), spec)
            self.assertIn(app, apps, spec)
            self.assertIsNotNone(
                kconfig._find_board_variant(str(_REPO), board)[0], spec)

    @unittest.skipUnless(importlib.util.find_spec("mkdocs"), "needs mkdocs")
    def test_hook_links_listed_demos_and_marks_the_rest_tbd(self):
        spec = importlib.util.spec_from_file_location(
            "demo_links", _REPO / "docs-site" / "hooks" / "demo_links.py")
        hook = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(hook)
        hook.on_config(SimpleNamespace(config_file_path=str(
            _REPO / "docs-site" / "mkdocs.yml")))
        page = SimpleNamespace(url="examples/example/")
        out = hook.on_page_content(
            '<a href="demo:nuttx/example_c_zh" target="_blank">C</a> '
            '<a href="demo:zephyr/lvgl_gallery_cpp_heap">G</a>', page=page)
        self.assertEqual(out, '<a href="../../demos/nuttx/example_c_zh/" '
                         'target="_blank">C</a> '
                         '<span class="demo-tbd">G (TBD)</span>')

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
