# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import errno
import http.client
import json
import os
import signal
import socket
import subprocess
import threading
import unittest
from contextlib import ExitStack, closing, suppress
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest import mock

from ove import build, download


class WebRunTest(unittest.TestCase):
    def test_linux_bundle_uses_the_dashboard_setting(self):
        with TemporaryDirectory() as temp:
            root = Path(temp)
            serve = root / "serve"
            serve.mkdir()
            (serve / "terminal.js").touch()
            (root / "firmware.elf").touch()
            ws = SimpleNamespace(
                ove_dir=Path(__file__).resolve().parents[2], images_dir=root,
                workspace_dir=root, dl_dir=root, output_dir=root,
                board_name="qemu-mps2-an500", config={
                    "CONFIG_OVE_LINUX": "y", "CONFIG_OVE_SIM": "y",
                    "CONFIG_OVE_SIM_DASHBOARD": "y"})
            web = {"firmware": "firmware.elf", "emulator": "qemu/",
                   "args": ["-serial", "stdio"], "files": {"rootfs.cpio": "/rootfs.cpio"}}
            with mock.patch.object(download, "build_qemu_wasm", return_value=root / "qemu"), \
                    mock.patch.object(build, "_strip_firmware"), \
                    mock.patch.object(build, "_personality_web_config", return_value=web), \
                    mock.patch.object(build, "_write_web_manifest",
                                      side_effect=lambda _, path: Path(path).write_text("{}")):
                build._assemble_web_bundle(ws)
                self.assertIn('src="qemu-wasm.js"', (serve / "index.html").read_text(encoding="utf-8"))
                self.assertEqual((serve / "manifest.json").read_text(), "{}")
                self.assertFalse((serve / "terminal.js").exists())
                self.assertEqual(json.loads((serve / "web.json").read_text()), web)

                ws.config["CONFIG_OVE_SIM_DASHBOARD"] = "n"
                build._assemble_web_bundle(ws)
                self.assertTrue((serve / "terminal.js").is_file())
                self.assertFalse((serve / "manifest.json").exists())
                self.assertFalse((serve / "app.js").exists())

    def _hold_ports(self, stack):
        for start in range(20000, 30000, 4):
            with ExitStack() as attempt:
                try:
                    for port in range(start, start + 4):
                        sock = attempt.enter_context(socket.socket())
                        sock.bind(("127.0.0.1", port))
                        sock.listen()
                except PermissionError as exc:
                    self.skipTest(f"cannot listen on loopback: {exc}")
                except OSError as exc:
                    if exc.errno != errno.EADDRINUSE:
                        raise
                    continue
                sock.close()  # Leave the fourth port free for the launcher.
                stack.enter_context(attempt.pop_all())
                return start
        self.skipTest("no free run of four loopback ports")

    def test_serves_on_the_next_free_port(self):
        with TemporaryDirectory() as temp, ExitStack() as stack:
            start = self._hold_ports(stack)
            bundle = Path(temp)
            run_sh = bundle / "run.sh"
            build._write_web_run_sh(run_sh)
            (bundle / "index.html").write_text("page", encoding="utf-8")
            browser = bundle / "browser"
            browser.write_text('#!/bin/sh\nprintf \'%s\\n\' "$1"\n', encoding="utf-8")
            browser.chmod(0o755)
            proc = subprocess.Popen(
                ["bash", run_sh, str(start)], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, start_new_session=True,
                env=dict(os.environ, BROWSER=str(browser)),
            )
            stack.enter_context(proc.stdout)

            def kill_group():
                with suppress(ProcessLookupError):
                    os.killpg(proc.pid, signal.SIGKILL)

            guard = threading.Timer(10, kill_group)
            guard.start()
            try:
                port = start + 3
                url = f"http://127.0.0.1:{port}"
                self.assertEqual(proc.stdout.readline().strip(),
                                 f"=== Serving the browser bundle at {url} ===")
                with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=5)) as conn:
                    conn.request("GET", "/")
                    with conn.getresponse() as reply:
                        self.assertEqual(reply.status, 200)
                        self.assertEqual(reply.getheader("Cross-Origin-Opener-Policy"), "same-origin")
                        self.assertEqual(reply.getheader("Cross-Origin-Embedder-Policy"), "require-corp")
                self.assertTrue(proc.stdout.readline().startswith("  Files:  "))
                self.assertEqual(proc.stdout.readline().strip(), "Press Ctrl+C to stop.")
                self.assertEqual(proc.stdout.readline().strip(), url)
            finally:
                guard.cancel()
                guard.join()
                kill_group()
                proc.wait(timeout=5)
