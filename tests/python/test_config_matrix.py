# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Regression tests for the trace/profiler Kconfig combination matrix.

The matrix driver (config/scripts/ove-config-matrix.py) claims to build the
*legal* trace/profiler Kconfig combinations.  Two properties are pinned
down here:

  * the driver must really validate the Kconfig -- a combination the
    dependency graph rejects has to abort the run with a diagnostic that
    names the offending symbol, and it must never reach the build step;
  * the combination list must cover every legal (STATS, TRACE, PROFILER)
    tuple the graph allows, and every tuple it does build must be legal.

The driver is a standalone script, so it is imported from its path.  The
"must abort" case is driven end to end in a sandbox repository root with a
stub `ove`, so no firmware is built, no network is touched and the real
workspace is left alone.
"""

import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MATRIX = os.environ.get(
    "OVE_CONFIG_MATRIX",
    os.path.join(ROOT, "config", "scripts", "ove-config-matrix.py"),
)
CONFIG_IN_DEBUG = os.path.join(ROOT, "config", "Config.in.debug")

STATS = "CONFIG_OVE_THREAD_STATE_STATS"
TRACE = "CONFIG_OVE_TRACE_STREAM"
MARKERS = "CONFIG_OVE_TRACE_MARKERS"
PROFILER = "CONFIG_OVE_PROFILER"
CONTROLLED = (STATS, TRACE, MARKERS, PROFILER)

# A base configuration that is valid on its own: the sim framework is on,
# so every controlled symbol may be enabled.
BASE_CONFIG = """\
CONFIG_OVE_APP_EXAMPLE_C_HEAP=y
CONFIG_OVE_APP_LANG_C=y
CONFIG_OVE_SIM=y
CONFIG_OVE_LOG_LEVEL_INF=y
CONFIG_OVE_LOG_LEVEL=2
CONFIG_OVE_THREAD_STATE_STATS=y
CONFIG_OVE_TRACE_STREAM=y
CONFIG_OVE_TRACE_MARKERS=y
CONFIG_OVE_PROFILER=y
"""

try:
    import kconfiglib  # noqa: F401

    HAVE_KCONFIGLIB = True
except ImportError:
    HAVE_KCONFIGLIB = False


def load_matrix(path, name="ove_config_matrix"):
    """Import the matrix driver from its path (the file name has dashes)."""
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# --- sandbox repository root ------------------------------------------------

_STUB_OVE = '''#!{python}
"""Stand-in for the `ove` CLI: records what the matrix driver asked for."""
import json
import os
import shutil
import sys

cmd = sys.argv[1] if len(sys.argv) > 1 else ""
ws = os.environ["OVE_TEST_WS"]
base = os.environ["OVE_TEST_BASE"]
log = os.environ["OVE_TEST_LOG"]


def rec(event, **kw):
    kw.setdefault("srctree", os.environ.get("srctree"))
    with open(log, "a") as fh:
        fh.write(json.dumps(dict(event=event, **kw)) + "\\n")


if cmd == "defconfig-fragments":
    os.makedirs(ws, exist_ok=True)
    shutil.copyfile(base, os.path.join(ws, ".config"))
    rec("defconfig-fragments")
elif cmd == "download":
    rec("download")
elif cmd == "configure":
    rec("configure")
elif cmd == "build":
    cfg = os.path.join(ws, ".config")
    text = open(cfg).read() if os.path.isfile(cfg) else ""
    rec("build", config=text)
    fw = os.path.join(ws, "build", "firmware")
    os.makedirs(fw, exist_ok=True)
    with open(os.path.join(fw, "firmware.elf"), "w") as fh:
        fh.write("ELF\\n")
else:
    rec("other", cmd=cmd)
'''


def make_sandbox(tmpdir):
    """Build a sandbox repository root that looks like the real one.

    The matrix driver derives its repository root from its own path, so the
    driver is *copied* (never symlinked) into the sandbox: the sandboxed run
    then operates on the sandbox workspace and never on the real one.
    """
    repo = os.path.join(tmpdir, "repo")
    os.makedirs(os.path.join(repo, "config", "scripts"))
    for entry in sorted(os.listdir(os.path.join(ROOT, "config"))):
        if entry == "scripts":
            continue
        os.symlink(os.path.join(ROOT, "config", entry),
                   os.path.join(repo, "config", entry))
    shutil.copy2(MATRIX, os.path.join(repo, "config", "scripts",
                                      "ove-config-matrix.py"))
    os.symlink(os.path.join(ROOT, "Config.in"), os.path.join(repo, "Config.in"))
    # The app/board Kconfig fragments are generated build output; the sandbox
    # gets minimal ones so the tree parses without running `ove`.
    kdir = os.path.join(repo, "output", "kconfig")
    os.makedirs(os.path.join(kdir, "apps"))
    os.makedirs(os.path.join(kdir, "boards"))
    with open(os.path.join(kdir, "apps", "Config.in"), "w") as fh:
        fh.write('menu "Applications"\n\n'
                 "config OVE_APP_EXAMPLE_C_HEAP\n"
                 '    bool "example_c heap app"\n\n'
                 "endmenu\n")
    with open(os.path.join(kdir, "boards", "Config.in"), "w") as fh:
        fh.write('menu "Boards"\n\nendmenu\n')
    return repo


def parse_depends(path):
    """Map every symbol in a Kconfig file to the symbols it depends on."""
    deps = {}
    current = None
    in_help = False
    with open(path) as fh:
        for raw in fh:
            line = raw.split("#")[0].strip()
            if not line:
                continue
            match = re.match(r"^config\s+([A-Za-z0-9_]+)$", line)
            if match:
                current = "CONFIG_" + match.group(1)
                deps.setdefault(current, [])
                in_help = False
                continue
            if line in ("endmenu", "endchoice"):
                current = None
                in_help = False
                continue
            if line == "help":
                in_help = True
                continue
            if in_help or current is None:
                continue
            if line.startswith("depends on"):
                expr = line[len("depends on"):]
                for ident in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", expr):
                    if ident in ("and", "or", "not"):
                        continue
                    dep = "CONFIG_" + ident
                    if dep not in deps[current]:
                        deps[current].append(dep)
    return deps


def legal_tuples(deps):
    """Every legal (STATS, TRACE, MARKERS, PROFILER) tuple of the graph.

    Dependencies outside the four controlled symbols (CONFIG_OVE_SIM for
    the profiler) are assumed satisfied: the matrix runs on sim workspaces,
    where the sim framework is on.
    """
    legal = set()
    for stats in (False, True):
        for trace in (False, True):
            for markers in (False, True):
                for profiler in (False, True):
                    assign = {STATS: stats, TRACE: trace, MARKERS: markers,
                              PROFILER: profiler}
                    ok = True
                    for symbol, on in assign.items():
                        if not on:
                            continue
                        for dep in deps.get(symbol, []):
                            value = assign.get(dep, True)
                            if not value:
                                ok = False
                    if ok:
                        legal.add((stats, trace, markers, profiler))
    return legal


def combo_tuple(overrides):
    """The (STATS, TRACE, MARKERS, PROFILER) tuple of one matrix entry."""
    return tuple(bool(overrides.get(symbol, False)) for symbol in CONTROLLED)


@unittest.skipUnless(HAVE_KCONFIGLIB, "kconfiglib is not installed")
class MatrixKconfigTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.repo = make_sandbox(self.temp.name)
        self.ws = os.path.join(self.repo, "output", "qemu", "freertos",
                               "example_c")
        os.makedirs(self.ws)
        self.base = os.path.join(self.temp.name, "base.config")
        with open(self.base, "w") as fh:
            fh.write(BASE_CONFIG)
        self.log = os.path.join(self.temp.name, "stub.log")
        self.stub = os.path.join(self.temp.name, "ove_stub.py")
        with open(self.stub, "w") as fh:
            fh.write(_STUB_OVE.format(python=sys.executable))
        os.chmod(self.stub, 0o755)
        self._srctree = os.environ.get("srctree")

    def tearDown(self):
        if self._srctree is None:
            os.environ.pop("srctree", None)
        else:
            os.environ["srctree"] = self._srctree
        self.temp.cleanup()

    def _events(self):
        if not os.path.isfile(self.log):
            return []
        with open(self.log) as fh:
            return [json.loads(line) for line in fh if line.strip()]

    def _run_matrix(self, base_text):
        """Run the sandboxed matrix driver; return the completed process."""
        with open(self.base, "w") as fh:
            fh.write(base_text)
        env = dict(os.environ)
        env.update({
            "OVE_TEST_WS": self.ws,
            "OVE_TEST_BASE": self.base,
            "OVE_TEST_LOG": self.log,
        })
        env.pop("srctree", None)
        script = os.path.join(self.repo, "config", "scripts",
                              "ove-config-matrix.py")
        return subprocess.run(
            [sys.executable, script, "--rtos", "freertos", "--app",
             "example_c", "--ove", self.stub],
            cwd=self.repo, env=env, capture_output=True, text=True,
            timeout=600, check=False)

    def test_illegal_combination_is_detected_and_never_built(self):
        # The sim framework is off, so the "profiler-only" combination
        # (CONFIG_OVE_PROFILER=y) violates config/Config.in.debug: the
        # profiler depends on OVE_SIM.  The driver must abort with a
        # diagnostic that names the symbol instead of building it.
        illegal = BASE_CONFIG.replace("CONFIG_OVE_SIM=y",
                                      "# CONFIG_OVE_SIM is not set")
        proc = self._run_matrix(illegal)

        output = proc.stdout + proc.stderr
        self.assertNotIn("kconfiglib not installed", output,
                         "the driver aborted because kconfiglib is missing")
        self.assertNotEqual(proc.returncode, 0,
                            "the matrix accepted an illegal combination:\n"
                            + output)
        self.assertIn(PROFILER, output,
                      "the diagnostic does not name the offending symbol:\n"
                      + output)

        builds = [e for e in self._events() if e.get("event") == "build"]
        for build in builds:
            self.assertNotIn(PROFILER + "=y", build.get("config", ""),
                             "an illegal combination reached the build step")

    def test_sources_are_fetched_before_the_first_build(self):
        # `ove build` needs the RTOS sources, so the driver has to fetch
        # them before the first combination is built.
        proc = self._run_matrix(BASE_CONFIG)
        names = [e.get("event") for e in self._events()]
        self.assertIn("download", names,
                      "the RTOS sources are never fetched:\n" + proc.stdout)
        self.assertIn("build", names, "the matrix built nothing:\n"
                      + proc.stdout)
        self.assertLess(names.index("download"), names.index("build"),
                        "the RTOS sources are fetched after the first build")

    def test_ove_does_not_inherit_the_kconfig_srctree(self):
        # The driver loads the oveRTOS Kconfig in-process; the srctree it
        # sets for that must not leak into the `ove` subprocesses, or
        # NuttX's olddefconfig looks for <repo>/Kconfig and fails.
        proc = self._run_matrix(BASE_CONFIG)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        leaked = [e for e in self._events() if e.get("srctree")]
        self.assertEqual(leaked, [], "srctree leaked into ove: %r"
                         % sorted({e["event"] for e in leaked}))

    def test_kconfig_check_can_fail(self):
        # Unit-level check of the same property: the driver must be able to
        # tell that Kconfig did not keep the requested value.
        mod = load_matrix(os.path.join(self.repo, "config", "scripts",
                                        "ove-config-matrix.py"))
        self.assertEqual(os.path.realpath(mod.REPO), os.path.realpath(self.repo))
        kconf = mod.load_kconfig()

        cfg_path = pathlib.Path(self.temp.name) / "illegal.config"
        with open(cfg_path, "w") as fh:
            fh.write(BASE_CONFIG.replace("CONFIG_OVE_SIM=y",
                                         "# CONFIG_OVE_SIM is not set"))
        effective = mod.resolve_config(cfg_path, kconf)

        self.assertFalse(mod.check_symbol(effective, PROFILER, True),
                         "the Kconfig check accepted an illegal value")
        reason = mod.kconfig_reason(kconf, PROFILER)
        self.assertIn("CONFIG_OVE_SIM", reason,
                      "the diagnostic does not name the unmet dependency: "
                      + reason)


class MatrixCombosTest(unittest.TestCase):
    """The combination list must cover exactly the legal combinations."""

    def setUp(self):
        self.mod = load_matrix(MATRIX)

    def test_every_combination_is_legal(self):
        deps = parse_depends(CONFIG_IN_DEBUG)
        legal = legal_tuples(deps)
        self.assertIn("CONFIG_OVE_SIM", deps.get(PROFILER, []),
                      "the profiler no longer depends on the sim framework")
        for name, overrides in self.mod._COMBOS.items():
            self.assertIn(combo_tuple(overrides), legal,
                          f"combination {name} violates the Kconfig graph")

    def test_every_legal_combination_is_covered(self):
        deps = parse_depends(CONFIG_IN_DEBUG)
        legal = legal_tuples(deps)
        legal3 = {(s, t, p) for (s, t, _m, p) in legal}
        covered3 = {(o.get(STATS, False), o.get(TRACE, False),
                     o.get(PROFILER, False))
                    for o in self.mod._COMBOS.values()}
        self.assertEqual(covered3, legal3,
                         "the matrix does not cover exactly the legal "
                         "(STATS, TRACE, PROFILER) combinations")

    def test_combinations_are_distinct(self):
        tuples = [combo_tuple(o) for o in self.mod._COMBOS.values()]
        self.assertEqual(len(tuples), len(set(tuples)),
                         "the matrix builds the same combination twice")


if __name__ == "__main__":
    unittest.main()
