# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that owned test processes report status and leave no group behind."""

import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import suppress
from pathlib import Path
from unittest import mock

from ove import test as ove_test

CLI = str(Path(ove_test.__file__).resolve().parents[1])
# Leaves a stray sleep in the group and records its PID.
STRAY = "sleep 60 & echo $! > \"$0\""
# The child starts a grandchild in the same group, records its PID, then waits.
SPAWN = ("import subprocess, sys, time; "
         "p = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)']); "
         "open(sys.argv[1], 'w').write(f'{p.pid}\\n'); time.sleep(60)")


def alive(pid):
    """Return whether *pid* still runs 2 s later; a zombie awaiting its reaper does not."""
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        try:
            if Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] == "Z":
                return False
        except (FileNotFoundError, ProcessLookupError):
            return False
        time.sleep(0.01)
    return True


def recorded_pid(path):
    deadline = time.monotonic() + 10
    while not (path.exists() and path.read_text().endswith("\n")) and time.monotonic() < deadline:
        time.sleep(0.01)
    return int(path.read_text())


class OwnedProcessTest(unittest.TestCase):
    def tearDown(self):
        for pid in getattr(self, "strays", ()):
            with suppress(ProcessLookupError):
                os.kill(pid, signal.SIGKILL)

    def run_owned(self, code, **kwargs):
        return ove_test._run_owned_test_process([sys.executable, "-c", code],
                                                env=dict(os.environ), **kwargs)

    def test_exit_status_and_signal(self):
        self.assertEqual(self.run_owned("raise SystemExit(7)", timeout=10), 7)
        self.assertEqual(self.run_owned("import os; os.kill(os.getpid(), 9)", timeout=10), -9)

    def test_output_environment_and_directory(self):
        with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryFile("w+") as output:
            code = "import os, sys; print(os.getcwd()); sys.stderr.write(os.environ['PATH'])"
            self.assertEqual(self.run_owned(code, timeout=10, stdout=output, cwd=directory), 0)
            output.seek(0)
            self.assertEqual(output.read(), os.path.realpath(directory) + "\n" + os.environ["PATH"])

    def test_normal_exit_kills_strays(self):
        with tempfile.TemporaryDirectory() as directory:
            record = Path(directory) / "pid"
            self.assertEqual(ove_test._run_owned_test_process(["sh", "-c", STRAY, str(record)],
                                                              env=dict(os.environ), timeout=10), 0)
            self.strays = [recorded_pid(record)]
            self.assertFalse(alive(self.strays[0]))

    def test_sigterm_to_the_owner_kills_the_group(self):
        with tempfile.TemporaryDirectory() as directory:
            record = Path(directory) / "pid"
            code = ("import os, sys; from ove.test import _run_owned_test_process; "
                    "_run_owned_test_process(['sh', '-c', sys.argv[1] + '; wait', sys.argv[2]], "
                    "env=dict(os.environ), timeout=60)")
            owner = subprocess.Popen([sys.executable, "-c", code, STRAY, str(record)],
                                     env=dict(os.environ, PYTHONPATH=CLI))
            self.strays = [recorded_pid(record)]
            owner.terminate()
            owner.wait(timeout=10)
            self.assertFalse(alive(self.strays[0]))

    def test_timeout_kills_the_whole_group(self):
        with tempfile.TemporaryDirectory() as directory:
            record = Path(directory) / "pid"
            command = [sys.executable, "-c", SPAWN, str(record)]
            self.assertEqual(ove_test._run_owned_test_process(command, env=dict(os.environ),
                                                              timeout=5, cleanup_timeout=1), 124)
            self.assertFalse(alive(recorded_pid(record)))

    def test_interruption_kills_the_group_and_propagates(self):
        with tempfile.TemporaryDirectory() as directory:
            record = Path(directory) / "pid"
            real_wait = subprocess.Popen.wait

            def interrupted(process, timeout=None):
                if timeout == 30:
                    while not (record.exists() and record.read_text()):
                        time.sleep(0.01)
                    # The signal can land while wait() holds its internal lock.
                    process._waitpid_lock.acquire()
                    raise KeyboardInterrupt
                if timeout is None and process._waitpid_lock.locked():
                    raise AssertionError("untimed wait() would block on the held wait lock")
                return real_wait(process, timeout)

            command = [sys.executable, "-c", SPAWN, str(record)]
            with mock.patch.object(subprocess.Popen, "wait", interrupted), \
                    self.assertRaises(KeyboardInterrupt):
                ove_test._run_owned_test_process(command, env=dict(os.environ), timeout=30,
                                                 cleanup_timeout=1)
            self.assertFalse(alive(int(record.read_text())))


if __name__ == "__main__":
    unittest.main()
