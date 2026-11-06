# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check child ownership before group effects and after the final reap."""

import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from ove import test as ove_test


class ChildLedger:
    """Refuse post-reap group effects before the real syscall."""

    def __init__(self, ready=None, after_spawn=None, before_signal=None):
        self.ready = ready
        self.after_spawn = after_spawn
        self.before_signal = before_signal
        self.children = []
        self.effects = []
        self.refused = []
        self.setup_failed = False
        self.real_popen = subprocess.Popen
        self.real_killpg = os.killpg
        self.real_waitid = os.waitid

    def observe(self, child, descriptor):
        kind, target = (os.P_PIDFD, descriptor) if descriptor is not None else (os.P_PID, child.pid)
        return self.real_waitid(kind, target, os.WEXITED | os.WNOHANG | os.WNOWAIT)

    def spawn(self, *args, **kwargs):
        child = self.real_popen(*args, **kwargs)
        self.children.append((child, None))
        try:
            descriptor = os.pidfd_open(child.pid)
        except OSError:
            self.setup_failed = True
            raise
        self.children[-1] = (child, descriptor)
        if self.ready is not None:
            deadline = time.monotonic() + 3
            while not self.ready.exists() and time.monotonic() < deadline:
                if self.observe(child, descriptor) is not None:
                    break
                time.sleep(0.01)
            self.setup_failed = not self.ready.exists()
        if self.after_spawn is not None:
            self.after_spawn()
        return child

    def signal_group(self, group, number):
        assert len(self.children) == 1 and group == self.children[0][0].pid
        child, descriptor = self.children[0]
        try:
            self.observe(child, descriptor)
        except ChildProcessError:
            self.refused.append(number)
            return
        self.effects.append(number)
        if self.before_signal is not None:
            self.before_signal(number)
        try:
            self.real_killpg(child.pid, number)
        except ProcessLookupError:
            pass

    def reaped(self):
        assert len(self.children) == 1
        try:
            self.observe(*self.children[0])
        except ChildProcessError:
            return True
        return False

    def __enter__(self):
        self.saved_handlers = {n: signal.getsignal(n) for n in
                               (signal.SIGINT, signal.SIGTERM, signal.SIGCHLD)}
        self.saved_mask = signal.pthread_sigmask(signal.SIG_BLOCK, set())
        self.patches = ExitStack()
        self.patches.enter_context(mock.patch.object(ove_test.subprocess, 'Popen', self.spawn))
        self.patches.enter_context(mock.patch.object(ove_test.os, 'killpg', self.signal_group))
        return self

    def __exit__(self, *error):
        self.patches.close()
        signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
        try:
            for child, descriptor in self.children:
                try:
                    try:
                        self.observe(child, descriptor)
                    except ChildProcessError:
                        continue
                    # Private cleanup uses only this still-owned child. A mutant
                    # may set Popen.returncode without actually reaping it.
                    try:
                        self.real_killpg(child.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    _, status = os.waitpid(child.pid, 0)
                    child.returncode = os.waitstatus_to_exitcode(status)
                finally:
                    if descriptor is not None:
                        os.close(descriptor)
        finally:
            try:
                for number, handler in self.saved_handlers.items():
                    signal.signal(number, handler)
            finally:
                signal.pthread_sigmask(signal.SIG_SETMASK, self.saved_mask)


class OwnedProcessTest(unittest.TestCase):
    def command(self, source):
        return [sys.executable, '-I', '-S', '-B', '-c', source]

    def assert_clean(self, ledger):
        self.assertFalse(ledger.setup_failed, 'child readiness is a setup requirement')
        self.assertEqual(ledger.refused, [], 'group effects require an unreaped leader')
        self.assertEqual(ledger.effects, [signal.SIGTERM, signal.SIGKILL])
        self.assertTrue(ledger.reaped(), 'the final wait must really reap the child')

    def test_exit_status_and_real_reap(self):
        for expected in (0, 7):
            with self.subTest(expected=expected), ChildLedger() as ledger:
                rc = ove_test._run_owned_test_process(
                    self.command('raise SystemExit(' + str(expected) + ')'),
                    env={}, timeout=3)
                self.assertEqual(rc, expected)
                self.assert_clean(ledger)

    def test_signal_status(self):
        with ChildLedger() as ledger:
            rc = ove_test._run_owned_test_process(
                self.command('import os,signal;os.kill(os.getpid(),signal.SIGTERM)'),
                env={}, timeout=3)
            self.assertEqual(rc, -signal.SIGTERM)
            self.assert_clean(ledger)

    def test_timeout_allows_cooperative_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            ready = Path(directory) / 'ready'
            stopped = Path(directory) / 'stopped'
            source = (
                'import signal,time\nfrom pathlib import Path\n'
                'def stop(number,frame):\n'
                f' Path({str(stopped)!r}).write_text("stopped")\n'
                ' raise SystemExit(0)\n'
                'signal.signal(signal.SIGTERM,stop)\n'
                f'Path({str(ready)!r}).write_text("ready")\n'
                'while True: time.sleep(0.01)\n')
            with ChildLedger(ready=ready) as ledger:
                rc = ove_test._run_owned_test_process(self.command(source), env={},
                                                     timeout=0.02, cleanup_timeout=1)
                self.assertEqual(rc, 124)
                self.assertEqual(stopped.read_text(), 'stopped')
                self.assertEqual(ledger.children[0][0].returncode, 0)
                self.assert_clean(ledger)

    def test_environment_directory_and_combined_output(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'output'
            source = ('import os,sys\nprint(os.environ["OWNED_TEST_MARKER"],flush=True)\n'
                      'print(os.getcwd(),flush=True)\nprint("stderr",file=sys.stderr,flush=True)')
            with ChildLedger() as ledger, log.open('wb') as stream:
                rc = ove_test._run_owned_test_process(
                    self.command(source), env={'OWNED_TEST_MARKER': 'selected'},
                    cwd=directory, stdout=stream, timeout=3)
                self.assertEqual(rc, 0)
                self.assert_clean(ledger)
            self.assertEqual(log.read_text(), 'selected\n' + directory + '\nstderr\n')

    def test_none_timeout_preserves_normal_exit(self):
        with ChildLedger() as ledger:
            self.assertEqual(ove_test._run_owned_test_process(
                self.command('pass'), env={}, timeout=None), 0)
            self.assert_clean(ledger)

    def test_unavailable_observation_refuses_before_spawn(self):
        with mock.patch.object(ove_test.os, 'WNOWAIT', create=False):
            # A missing attribute is the unsupported platform boundary.
            del ove_test.os.WNOWAIT
            with mock.patch.object(ove_test.subprocess, 'Popen',
                                   side_effect=RuntimeError('prohibited spawn')) as spawn:
                with self.assertRaises(RuntimeError):
                    ove_test._run_owned_test_process(['unused'], env={}, timeout=1)
                spawn.assert_not_called()

    def test_ignored_child_handler_refuses_before_spawn(self):
        real_getsignal = signal.getsignal
        with (mock.patch.object(ove_test.signal, 'getsignal',
                                side_effect=lambda n: signal.SIG_IGN if n == signal.SIGCHLD
                                else real_getsignal(n)),
              mock.patch.object(ove_test.subprocess, 'Popen',
                                side_effect=RuntimeError('prohibited spawn')) as spawn):
            with self.assertRaises(RuntimeError):
                ove_test._run_owned_test_process(['unused'], env={}, timeout=1)
            spawn.assert_not_called()

    def test_nonfinite_intervals_refuse_before_spawn(self):
        for name in ('timeout', 'cleanup_timeout'):
            for value in (float('nan'), float('inf'), -float('inf')):
                with self.subTest(name=name, value=value):
                    with mock.patch.object(ove_test.subprocess, 'Popen',
                                           side_effect=ValueError('prohibited spawn')) as spawn:
                        args = dict(timeout=1, cleanup_timeout=1)
                        args[name] = value
                        with self.assertRaises(ValueError):
                            ove_test._run_owned_test_process(['unused'], env={}, **args)
                        spawn.assert_not_called()

    def test_dumped_status_mapping_uses_an_initialized_record(self):
        real_waitid = os.waitid

        def observe(*args):
            status = real_waitid(*args)
            if status is not None:
                return SimpleNamespace(si_pid=status.si_pid, si_code=os.CLD_DUMPED,
                                       si_status=signal.SIGTERM)
            return status

        with ChildLedger() as ledger, mock.patch.object(ove_test.os, 'waitid', observe):
            rc = ove_test._run_owned_test_process(self.command('pass'), env={}, timeout=3)
            self.assertEqual(rc, -signal.SIGTERM)
            self.assertEqual(ledger.children[0][0].returncode, 0)
            self.assert_clean(ledger)

    def test_interrupt_after_spawn_is_replayed_after_reap(self):
        self.check_interrupt(after_spawn=True)

    def test_repeated_cleanup_interrupts_preserve_handlers_and_reap(self):
        self.check_interrupt(after_spawn=False)

    def test_main_thread_refusal_precedes_spawn(self):
        with (mock.patch.object(ove_test.threading, 'current_thread', return_value=object()),
              mock.patch.object(ove_test.subprocess, 'Popen',
                                side_effect=RuntimeError('prohibited spawn')) as spawn):
            with self.assertRaises(RuntimeError):
                ove_test._run_owned_test_process(['unused'], env={}, timeout=1)
            spawn.assert_not_called()

    def test_multiple_kernel_tasks_refuse_before_spawn(self):
        original_iterdir = Path.iterdir

        def entries(path):
            return iter([Path('first'), Path('second')]) if str(path) == '/proc/self/task' else original_iterdir(path)

        with (mock.patch.object(Path, 'iterdir', entries),
              mock.patch.object(ove_test.subprocess, 'Popen',
                                side_effect=RuntimeError('prohibited spawn')) as spawn):
            with self.assertRaises(RuntimeError):
                ove_test._run_owned_test_process(['unused'], env={}, timeout=1)
            spawn.assert_not_called()

    def test_default_term_replay_follows_reap_and_restoration(self):
        original = signal.getsignal(signal.SIGTERM)
        replayed = []
        ledger = None

        def dispatch():
            handler = signal.getsignal(signal.SIGTERM)
            if callable(handler):
                handler(signal.SIGTERM, None)
            else:
                replayed.append('unprotected default handler')

        def replay(number):
            replayed.append((number, ledger.reaped(), signal.getsignal(number)))

        try:
            signal.signal(signal.SIGTERM, signal.SIG_DFL)
            ledger = ChildLedger(after_spawn=dispatch)
            with ledger, mock.patch.object(ove_test.signal, 'raise_signal', replay):
                rc = ove_test._run_owned_test_process(self.command('pass'), env={}, timeout=3)
                self.assertEqual(rc, 128 + signal.SIGTERM)
                self.assertEqual(replayed, [(signal.SIGTERM, True, signal.SIG_DFL)])
                self.assert_clean(ledger)
        finally:
            signal.signal(signal.SIGTERM, original)

    def test_handler_restoration_blocks_both_interrupts(self):
        original = {n: signal.getsignal(n) for n in (signal.SIGINT, signal.SIGTERM)}
        real_set = signal.signal
        masks = []
        installed = set()

        def install(number, handler):
            if number in original:
                if handler == original[number] and number in installed:
                    masks.append(set(signal.pthread_sigmask(signal.SIG_BLOCK, set())))
                else:
                    installed.add(number)
            return real_set(number, handler)

        before_mask = set(signal.pthread_sigmask(signal.SIG_BLOCK, set()))
        with ChildLedger() as ledger, mock.patch.object(ove_test.signal, 'signal', install):
            self.assertEqual(ove_test._run_owned_test_process(
                self.command('pass'), env={}, timeout=3), 0)
            self.assert_clean(ledger)
            observed_mask = set(signal.pthread_sigmask(signal.SIG_BLOCK, set()))
        self.assertEqual(len(masks), 2)
        self.assertTrue(all({signal.SIGINT, signal.SIGTERM} <= mask for mask in masks))
        self.assertEqual(observed_mask, before_mask)

    def test_infrastructure_failure_restores_handlers_before_propagation(self):
        original = {n: signal.getsignal(n) for n in (signal.SIGINT, signal.SIGTERM)}
        real_waitid = os.waitid
        injected = []
        replayed = []
        ledger = ChildLedger()

        def observe(kind, target, flags):
            if ledger.children and target == ledger.children[0][0].pid and not injected:
                injected.append(True)
                signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)
                raise OSError('synthetic observation failure')
            return real_waitid(kind, target, flags)

        with (ledger, mock.patch.object(ove_test.os, 'waitid', observe),
              mock.patch.object(ove_test.signal, 'raise_signal', side_effect=replayed.append)):
            with self.assertRaisesRegex(OSError, 'synthetic observation failure'):
                ove_test._run_owned_test_process(self.command('pass'), env={}, timeout=3)
            self.assertEqual(injected, [True])
            self.assertEqual(replayed, [])
            self.assertEqual({n: signal.getsignal(n) for n in original}, original)
            self.assert_clean(ledger)

    def check_interrupt(self, after_spawn):
        original = {n: signal.getsignal(n) for n in (signal.SIGINT, signal.SIGTERM)}
        events = []
        ledger = None

        def restored_handler(number, frame):
            events.append((number, ledger.reaped()))
            self.assertEqual(signal.getsignal(signal.SIGINT), restored_handler)
            self.assertEqual(signal.getsignal(signal.SIGTERM), restored_handler)

        def dispatch():
            # Dispatch the installed Python handler at a precise boundary.
            # This is a synthetic call, not a claim of kernel delivery.
            signal.getsignal(signal.SIGINT)(signal.SIGINT, None)
            signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)

        try:
            for number in original:
                signal.signal(number, restored_handler)
            ledger = ChildLedger(after_spawn=dispatch if after_spawn else None,
                                 before_signal=(lambda number: dispatch())
                                 if not after_spawn else None)
            with ledger:
                rc = ove_test._run_owned_test_process(self.command('pass'), env={}, timeout=3)
                self.assertEqual(rc, 128 + signal.SIGINT)
                self.assertEqual(events, [(signal.SIGINT, True)])
                self.assert_clean(ledger)
        finally:
            for number, handler in original.items():
                signal.signal(number, handler)


if __name__ == '__main__':
    unittest.main()
