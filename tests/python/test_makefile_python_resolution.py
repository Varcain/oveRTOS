# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Test for the Makefile's Python interpreter resolution.

``?=`` cannot override a ``PYTHON`` from the environment, so the Makefile
resolves the interpreter by origin; otherwise ``OVE_PYTHON``, the escape
hatch the "Python >= 3.12 is required" error names, would do nothing when
``PYTHON`` is exported.  The precedence asserted here is

    command-line PYTHON=  >  OVE_PYTHON  >  environment PYTHON  >  auto-detect

and the auto-detection must take the first interpreter >= 3.12, never the
one of the venv it is about to replace.

The test drives real GNU make and inspects the *expanded* recipe, so it does
not depend on the name of the variable the Makefile uses internally, and it
never needs the interpreter it names to exist (the assertions are on the
interpreter identity string that make expands).

Two details matter for the harness:

* The recipe is expanded by asking for the venv stamp target itself.  The
  ``setup`` target delegates to a recursive ``$(MAKE)``, and that sub-make
  reads the Makefile from the *current directory* -- not from the file given
  with ``-f`` -- so only the stamp target really exercises the Makefile
  under test.
* Every invocation redirects ``VENV_DIR`` into a scratch directory and only
  ever runs ``make -n``, so the repository ``.venv`` is never touched.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Interpreter identities that do not exist on purpose: the assertions are on
# the string make expands, never on the interpreter actually running.
OLD_PY = "/nonexistent/python3.11"
GOOD_PY = "/nonexistent/python3.12"
CLI_PY = "/nonexistent/python3.from-cli"

# The recipe line that runs the ">= 3.12" version gate, and the interpreter
# that precedes it.
GATE_RE = re.compile(
    r"^\s*(?P<interp>.*?)\s+-c 'import sys; raise SystemExit", re.MULTILINE
)

PROBE_MK = """\
.PHONY: ove-python-probe
ove-python-probe:
\t@echo "PYTHON=[$(PYTHON)]"
\t@echo "OVE_PY=[$(OVE_PY)]"
\t@echo "OVE_PYTHON=[$(OVE_PYTHON)]"
"""


def _make_program():
    for name in ("make", "gmake"):
        path = shutil.which(name)
        if path:
            return path
    return None


MAKE = _make_program()


def makefile_path():
    """The Makefile under test.

    ``OVE_TEST_MAKEFILE`` points the test at another copy of the Makefile
    (mutation checks).
    """
    override = os.environ.get("OVE_TEST_MAKEFILE")
    if override:
        return Path(override).resolve()
    return REPO_ROOT / "Makefile"


@unittest.skipIf(MAKE is None, "GNU make is not available")
class MakefilePythonResolutionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.venv_dir = (Path(self.tmp.name) / "venv").resolve()
        self.venv_dir.mkdir(parents=True, exist_ok=True)
        self.stamp = self.venv_dir / ".stamp"
        self.probe = Path(self.tmp.name) / "probe.mk"
        self.probe.write_text(PROBE_MK)
        self.makefile = makefile_path()
        if not self.makefile.is_file():
            self.skipTest(f"no Makefile at {self.makefile}")

    # -- helpers ---------------------------------------------------------
    def _env(self, **overrides):
        """A controlled environment: no inherited PYTHON/OVE_PYTHON."""
        env = dict(os.environ)
        for name in ("PYTHON", "OVE_PYTHON", "MAKEFLAGS", "MFLAGS"):
            env.pop(name, None)
        env.update(overrides)
        return env

    def _make(self, args, env):
        return subprocess.run(
            [MAKE, "-f", str(self.makefile), *args],
            cwd=str(REPO_ROOT),
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )

    def _resolved(self, env, cmd=()):
        """The interpreter the expanded venv recipe really uses.

        The stamp target is requested directly so that the recipe is
        expanded by *this* make process, i.e. by the Makefile under test.
        """
        proc = self._make(
            ["-n", f"VENV_DIR={self.venv_dir}", str(self.stamp), *cmd], env
        )
        self.assertEqual(
            proc.returncode,
            0,
            f"make -n failed:\n{proc.stdout}\n{proc.stderr}",
        )
        match = GATE_RE.search(proc.stdout)
        self.assertIsNotNone(
            match,
            "no version-gate line in the expanded recipe:\n" + proc.stdout,
        )
        return match.group("interp")

    def _probe(self, env, cmd=()):
        proc = self._make(
            [
                "-f",
                str(self.probe),
                f"VENV_DIR={self.venv_dir}",
                "ove-python-probe",
                *cmd,
            ],
            env,
        )
        self.assertEqual(
            proc.returncode,
            0,
            f"make probe failed:\n{proc.stdout}\n{proc.stderr}",
        )
        values = {}
        for line in proc.stdout.splitlines():
            key, sep, value = line.partition("=")
            if sep and key in ("PYTHON", "OVE_PY", "OVE_PYTHON"):
                values[key] = value[1:-1] if value.startswith("[") else value
        return values

    # -- the precedence rules -------------------------------------------
    def test_env_python_does_not_shadow_ove_python(self):
        """OVE_PYTHON must win over an environment PYTHON."""
        env = self._env(PYTHON=OLD_PY)
        self.assertEqual(
            self._resolved(env, cmd=[f"OVE_PYTHON={GOOD_PY}"]), GOOD_PY
        )

    def test_env_python_does_not_shadow_env_ove_python(self):
        """OVE_PYTHON from the environment beats PYTHON from the environment."""
        env = self._env(PYTHON=OLD_PY, OVE_PYTHON=GOOD_PY)
        self.assertEqual(self._resolved(env), GOOD_PY)

    def test_command_line_python_wins(self):
        """An explicit PYTHON= on the command line wins over OVE_PYTHON."""
        env = self._env()
        self.assertEqual(
            self._resolved(
                env, cmd=[f"PYTHON={CLI_PY}", f"OVE_PYTHON={GOOD_PY}"]
            ),
            CLI_PY,
        )

    def test_command_line_python_wins_over_env_python(self):
        env = self._env(PYTHON=OLD_PY)
        self.assertEqual(
            self._resolved(env, cmd=[f"PYTHON={CLI_PY}"]), CLI_PY
        )

    def test_env_python_is_honoured(self):
        """Without OVE_PYTHON the environment PYTHON is still used."""
        env = self._env(PYTHON=OLD_PY)
        self.assertEqual(self._resolved(env), OLD_PY)

    def test_command_line_ove_python_is_used(self):
        env = self._env()
        self.assertEqual(
            self._resolved(env, cmd=[f"OVE_PYTHON={GOOD_PY}"]), GOOD_PY
        )

    def test_env_ove_python_is_used(self):
        env = self._env(OVE_PYTHON=GOOD_PY)
        self.assertEqual(self._resolved(env), GOOD_PY)

    # -- auto-detection -------------------------------------------------
    def _fake_bin(self, name, versions):
        """A directory of fake interpreters: each passes the >= 3.12 check
        (exit 0) when its version in *versions* is new enough."""
        bin_dir = Path(self.tmp.name) / name
        bin_dir.mkdir(parents=True, exist_ok=True)
        for prog, ok in versions.items():
            exe = bin_dir / prog
            exe.write_text(f"#!/bin/sh\nexit {0 if ok else 1}\n")
            exe.chmod(0o755)
        return bin_dir

    def _detect_env(self, *bin_dirs):
        """A PATH holding only *bin_dirs* and the tools the detection runs,
        so no real interpreter of this machine is found."""
        tools = Path(self.tmp.name) / "tools"
        tools.mkdir(exist_ok=True)
        for tool in ("tr", "grep", "paste"):
            real = shutil.which(tool)
            if real is None:
                self.skipTest(f"{tool} is not available")
            link = tools / tool
            if not link.exists():
                link.symlink_to(real)
        path = ":".join(str(d) for d in (*bin_dirs, tools))
        return self._env(PATH=path)

    def test_auto_detect_skips_the_venv_being_replaced(self):
        """`make setup` re-runs make with the venv's bin first on PATH; the
        venv's own interpreter must not be picked to rebuild it."""
        venv_bin = self._fake_bin("venv/bin", {"python3.14": True,
                                                "python3": True})
        system = self._fake_bin("system", {"python3.14": True,
                                           "python3": False})
        env = self._detect_env(venv_bin, system)
        self.assertEqual(self._resolved(env), str(system / "python3.14"))

    def test_auto_detect_takes_the_first_new_enough_interpreter(self):
        system = self._fake_bin("system", {"python3.14": False,
                                           "python3.13": True,
                                           "python3": True})
        env = self._detect_env(system)
        self.assertEqual(self._resolved(env), str(system / "python3.13"))

    def test_auto_detect_falls_back_to_python3(self):
        """Without an interpreter >= 3.12, python3 is used so the version
        gate can report it."""
        system = self._fake_bin("system", {"python3": False})
        env = self._detect_env(system)
        self.assertEqual(self._resolved(env), str(system / "python3"))

    # -- the resolved variable stays consistent --------------------------
    def test_python_variable_matches_the_used_interpreter(self):
        """$(PYTHON) and $(OVE_PY) must name the interpreter that is used.

        Re-assigning PYTHON in the Makefile must not shadow an explicit
        ``PYTHON=`` given on the command line.
        """
        scenarios = (
            ("cli-python", self._env(), [f"PYTHON={CLI_PY}"]),
            ("cli-ove-python", self._env(), [f"OVE_PYTHON={GOOD_PY}"]),
            ("env-python", self._env(PYTHON=OLD_PY), []),
            ("env-ove-python", self._env(OVE_PYTHON=GOOD_PY), []),
            ("env-both", self._env(PYTHON=OLD_PY, OVE_PYTHON=GOOD_PY), []),
            (
                "cli-both",
                self._env(),
                [f"PYTHON={CLI_PY}", f"OVE_PYTHON={GOOD_PY}"],
            ),
        )
        for name, env, cmd in scenarios:
            with self.subTest(scenario=name):
                values = self._probe(env, cmd=cmd)
                resolved = self._resolved(env, cmd=cmd)
                self.assertEqual(values["PYTHON"], resolved)
                if values["OVE_PY"]:
                    self.assertEqual(values["PYTHON"], values["OVE_PY"])


if __name__ == "__main__":
    unittest.main()
