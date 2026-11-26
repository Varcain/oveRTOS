# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import errno
import http.client
import os
import signal
import socket
import subprocess
import threading
import unittest
from contextlib import ExitStack, closing, suppress
from pathlib import Path
from tempfile import TemporaryDirectory

from ove import build


class WebRunTest(unittest.TestCase):
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
