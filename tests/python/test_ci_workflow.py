# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Regression test for the CI jobs that build the RTOS example app.

The QEMU trace jobs (``make test-qemu-<rtos>-trace``) and the Kconfig
matrix job (``make test-kconfig-matrix``) build the example application
through ``ove build``, which refuses to run when the RTOS sources are
missing from the workspace ``dl/`` ("RTOS sources not found ... Run 'ove
download' first").  A CI job that never fetches those sources therefore
fails before it builds anything.

This test pins down the properties such a job must have:

  * a source-fetch step exists and runs BEFORE the build/test step;
  * the fetch step creates the workspace config first (``ove download``
    calls ``ws.require_config()``, and a fresh checkout has no
    ``.config``);
  * the fetch step configures the SAME workspace the build consumes
    (``output/qemu/<rtos>/example_c``) -- otherwise the sources land in
    another workspace's ``dl/`` and the fix silently does nothing;
  * the Kconfig-matrix script downloads before it builds, and does so
    once (not once per Kconfig combination);
  * the trace test keeps its own source-fetch guard, and that guard
    accepts exactly the source directories ``ove build`` requires;
  * the cached ``dl/`` paths name directories ``ove download`` really
    creates; otherwise ``actions/cache`` reports a "Path Validation
    Error" and saves nothing.

The test is self-contained: it only parses files, needs no network and
no third-party module (PyYAML is used when available, otherwise a
strict built-in parser for the workflow's YAML subset is used).

The files under test can be redirected with the environment variables
``OVE_CI_WORKFLOW``, ``OVE_CI_MATRIX_SCRIPT``, ``OVE_CI_CLI_TEST``,
``OVE_CI_BUILD_MODULE`` and ``OVE_CI_DOWNLOAD_MODULE`` -- that is how
the pre-fix tree is checked without touching the working copy.
"""

import ast
import fnmatch
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

WORKFLOW = os.environ.get(
    "OVE_CI_WORKFLOW", os.path.join(ROOT, ".github", "workflows", "ove-tests.yml"))
MATRIX_SCRIPT = os.environ.get(
    "OVE_CI_MATRIX_SCRIPT", os.path.join(ROOT, "config", "scripts",
                                         "ove-config-matrix.py"))
CLI_TEST = os.environ.get(
    "OVE_CI_CLI_TEST", os.path.join(ROOT, "config", "ove-cli", "ove", "test.py"))
BUILD_MODULE = os.environ.get(
    "OVE_CI_BUILD_MODULE", os.path.join(ROOT, "config", "ove-cli", "ove",
                                        "build.py"))
DOWNLOAD_MODULE = os.environ.get(
    "OVE_CI_DOWNLOAD_MODULE", os.path.join(ROOT, "config", "ove-cli", "ove",
                                            "download.py"))

TRACE_RTOS = ("freertos", "nuttx", "zephyr")

# The workspace the trace tests and the Kconfig matrix build in.  The CI
# fetch step has to configure exactly this workspace, otherwise the RTOS
# sources land in a different workspace's dl/ and the build still fails.
WORKSPACE_TEMPLATE = "output/qemu/<rtos>/example_c"

# `make download`, `ove download`, `.venv/bin/ove download`, ...
DOWNLOAD_CMD_RE = re.compile(
    r"(?:^|[\s`/&|;(])(?:make\b[^\n]*\bdownload\b|ove\b[^\n]*?\bdownload\b)",
    re.IGNORECASE)
# `make qemu.<rtos>.example_c` / `ove defconfig-fragments qemu.<rtos>.example_c`
# (the spec may contain a GitHub expression such as ${{ matrix.rtos }})
_SPEC = r"(?:[A-Za-z0-9_.\-]+|\$\{\{.*?\}\})+"
CONFIG_CMD_RE = re.compile(
    r"(?:make\s+(?P<make_spec>" + _SPEC + r")"
    r"|ove\s+defconfig-fragments\s+(?P<ove_spec>" + _SPEC + r"))")


# --------------------------------------------------------------------------
# A strict parser for the YAML subset the workflow uses
# --------------------------------------------------------------------------

class YamlSubsetError(Exception):
    """Raised when the workflow uses YAML the built-in parser cannot read."""


def _indent_of(line):
    return len(line) - len(line.lstrip(" "))


def _is_skippable(line):
    stripped = line.strip()
    return not stripped or stripped.startswith("#")


def _parse_node(lines, index, indent):
    """Parse the mapping or sequence that starts at ``lines[index]``.

    Returns ``(value, next_index)``.  Raises :class:`YamlSubsetError` for
    anything this subset parser does not understand -- a silent mis-parse
    would turn this regression test into a rubber stamp.
    """
    while index < len(lines) and _is_skippable(lines[index]):
        index += 1
    if index >= len(lines):
        return None, index
    if _indent_of(lines[index]) != indent:
        raise YamlSubsetError(
            f"unexpected indentation at line {index + 1}: {lines[index]!r}")
    if lines[index].strip().startswith("- "):
        return _parse_sequence(lines, index, indent)
    return _parse_mapping(lines, index, indent)


def _parse_sequence(lines, index, indent):
    items = []
    while index < len(lines):
        line = lines[index]
        if _is_skippable(line):
            index += 1
            continue
        if _indent_of(line) != indent or not line.strip().startswith("- "):
            break
        item_indent = indent + 2
        body = [" " * item_indent + line.strip()[2:].strip()]
        index += 1
        while index < len(lines):
            inner = lines[index]
            if _is_skippable(inner):
                body.append(inner)
                index += 1
                continue
            inner_indent = _indent_of(inner)
            if inner_indent < item_indent:
                break
            if inner_indent == item_indent and inner.strip().startswith("- "):
                break
            body.append(inner)
            index += 1
        value, _ = _parse_node(body, 0, item_indent)
        items.append(value)
    return items, index


def _parse_mapping(lines, index, indent):
    mapping = {}
    while index < len(lines):
        line = lines[index]
        if _is_skippable(line):
            index += 1
            continue
        if _indent_of(line) != indent:
            break
        stripped = line.strip()
        if stripped.startswith("- "):
            break
        match = re.match(r"^([^:\s][^:]*):(.*)$", stripped)
        if not match:
            raise YamlSubsetError(
                f"cannot parse line {index + 1}: {line!r}")
        key = match.group(1).strip()
        rest = match.group(2).strip()
        index += 1
        if rest in ("|", "|-", "|+", ">", ">-", ">+"):
            text, index = _parse_block_scalar(lines, index, indent)
            mapping[key] = text
        elif rest.startswith("[") and rest.endswith("]"):
            mapping[key] = [_scalar(item.strip())
                            for item in rest[1:-1].split(",") if item.strip()]
        elif rest == "":
            lookahead = index
            while lookahead < len(lines) and _is_skippable(lines[lookahead]):
                lookahead += 1
            if lookahead >= len(lines) or _indent_of(lines[lookahead]) <= indent:
                mapping[key] = None
                continue
            value, index = _parse_node(lines, index, _indent_of(lines[lookahead]))
            mapping[key] = value
        else:
            mapping[key] = _scalar(rest)
    return mapping, index


# _parse_block_scalar() reports where the block ended through this
# module-level cell: the recursive parser stays readable, and a block
# scalar is always consumed by the caller that opened it.
_block_scalar_end = 0


def _parse_block_scalar(lines, index, indent):
    """Read a ``|``/``>`` block scalar.

    Returns ``(text, next_index)``.
    """
    raw = []
    while index < len(lines):
        line = lines[index]
        if line.strip() == "":
            raw.append("")
            index += 1
            continue
        if _indent_of(line) <= indent:
            break
        raw.append(line)
        index += 1
    while raw and not raw[-1].strip():
        raw.pop()
    if not raw:
        return "", index
    block_indent = min(_indent_of(line) for line in raw if line.strip())
    text = "\n".join(line[block_indent:] if line.strip() else ""
                     for line in raw)
    # A YAML block scalar keeps the final newline.
    return text + "\n", index


def _scalar(text):
    if len(text) >= 2 and text[0] == text[-1] and text[0] in ("'", '"'):
        return text[1:-1]
    return text


def _parse_workflow_subset(text):
    """Parse the workflow with the built-in subset parser."""
    lines = text.splitlines()
    root, _ = _parse_node(lines, 0, 0)
    if not isinstance(root, dict) or "jobs" not in root:
        raise YamlSubsetError("no 'jobs' mapping in the workflow")
    return root


def _normalize(value):
    """Normalize a parsed YAML value for comparison.

    PyYAML resolves YAML 1.1 booleans (``true``/``false``) and plain
    integers to Python types; the built-in parser keeps them as text.
    Both are mapped to the same spelling so the two parsers can be
    compared.
    """
    if isinstance(value, dict):
        return {str(key): _normalize(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_normalize(item) for item in value]
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    return value


def _parse_workflow(text):
    """Parse the workflow, preferring PyYAML when it is installed.

    When both parsers are available they must agree on the ``jobs``
    subtree -- that keeps the built-in fallback honest.
    """
    try:
        import yaml
    except ImportError:
        return _parse_workflow_subset(text), "builtin"
    parsed = yaml.safe_load(text)
    builtin = _parse_workflow_subset(text)
    if _normalize(builtin.get("jobs")) != _normalize(parsed.get("jobs")):
        raise AssertionError(
            "the built-in workflow parser and PyYAML disagree on the "
            "'jobs' subtree; the built-in parser must be fixed before it "
            "can be trusted")
    return parsed, "pyyaml"


def _strip_comments(text):
    """Drop comment-only lines so a comment cannot fake a command."""
    return "\n".join(line for line in text.splitlines()
                     if not line.strip().startswith("#"))


# --------------------------------------------------------------------------
# AST helpers
# --------------------------------------------------------------------------

def _script_run_calls(path):
    """Return the ordered ``run([...])`` calls of a script.

    Each entry is ``(lineno, [literal words])``.
    """
    with open(path, "r", encoding="utf-8") as handle:
        tree = ast.parse(handle.read())
    calls = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        name = getattr(node.func, "id", None) or getattr(node.func, "attr", None)
        if name != "run" or not node.args:
            continue
        argv = node.args[0]
        words = []
        if isinstance(argv, (ast.List, ast.Tuple)):
            for element in argv.elts:
                if isinstance(element, ast.Constant) and \
                        isinstance(element.value, str):
                    words.append(element.value)
        elif isinstance(argv, ast.Name):
            continue
        else:
            continue
        calls.append((node.lineno, words))
    calls.sort(key=lambda item: item[0])
    return calls


def _hashed_dir_targets(path):
    """Return the base names ``hashed_dir()`` can create in ``dl/``."""
    with open(path, "r", encoding="utf-8") as handle:
        tree = ast.parse(handle.read())
    names = set()
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        func = getattr(node.func, "id", None) or getattr(node.func, "attr", None)
        if func != "hashed_dir":
            continue
        if len(node.args) < 2:
            continue
        second = node.args[1]
        if isinstance(second, ast.Constant) and isinstance(second.value, str):
            names.add(second.value)
    return names


def _dict_literal(path, variable):
    """Return a module-level dict literal assigned to ``variable``."""
    with open(path, "r", encoding="utf-8") as handle:
        tree = ast.parse(handle.read())
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        for target in node.targets:
            if isinstance(target, ast.Name) and target.id == variable:
                value = node.value
                if isinstance(value, ast.Dict):
                    return ast.literal_eval(value)
    return None


def _local_dict_literal(path, function, variable):
    """Return a dict literal assigned to ``variable`` inside ``function``."""
    with open(path, "r", encoding="utf-8") as handle:
        tree = ast.parse(handle.read())
    for node in ast.walk(tree):
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        if node.name != function:
            continue
        for inner in ast.walk(node):
            if not isinstance(inner, ast.Assign):
                continue
            for target in inner.targets:
                if isinstance(target, ast.Name) and target.id == variable:
                    value = inner.value
                    if isinstance(value, ast.Dict):
                        return ast.literal_eval(value)
    return None


# --------------------------------------------------------------------------
# The test
# --------------------------------------------------------------------------

class CiWorkflowSourceFetchTest(unittest.TestCase):
    """The CI jobs that build the example app must fetch the RTOS sources."""

    @classmethod
    def setUpClass(cls):
        with open(WORKFLOW, "r", encoding="utf-8") as handle:
            cls.workflow_text = handle.read()
        cls.workflow, cls.parser = _parse_workflow(cls.workflow_text)
        cls.jobs = cls.workflow.get("jobs") or {}
        with open(MATRIX_SCRIPT, "r", encoding="utf-8") as handle:
            cls.matrix_text = handle.read()
        with open(CLI_TEST, "r", encoding="utf-8") as handle:
            cls.cli_text = handle.read()
        cls.download_targets = _hashed_dir_targets(DOWNLOAD_MODULE)
        cls.preflight_map = _local_dict_literal(
            BUILD_MODULE, "_preflight_check", "rtos_dir_map") or {}

    # -- helpers ---------------------------------------------------------

    def _steps(self, job):
        steps = job.get("steps") or []
        for step in steps:
            if not isinstance(step, dict):
                # An assertion about the file's structure, not a type error
                # in an API, so AssertionError is the right exception here.
                raise AssertionError(  # noqa: TRY004
                    f"unexpected step entry: {step!r}")
        return steps

    def _run_text(self, step):
        run = step.get("run")
        if run is None:
            return ""
        return run if isinstance(run, str) else str(run)

    def _job_with_step(self, pattern):
        """Return ``(job_name, job, step_index)`` of the first match."""
        regex = re.compile(pattern)
        for name, job in self.jobs.items():
            for index, step in enumerate(self._steps(job)):
                if regex.search(_strip_comments(self._run_text(step))):
                    return name, job, index
        return None, None, None

    def _fetch_step(self, job, before_index):
        """Return the last source-fetch step before ``before_index``."""
        found = None
        for index, step in enumerate(self._steps(job)):
            if index >= before_index:
                break
            if DOWNLOAD_CMD_RE.search(_strip_comments(self._run_text(step))):
                found = (index, step)
        return found

    def _config_spec(self, run_text):
        """Return the workspace spec the step configures, if any."""
        for match in CONFIG_CMD_RE.finditer(_strip_comments(run_text)):
            spec = match.group("make_spec") or match.group("ove_spec")
            if spec:
                return spec
        return None

    def _cached_globs(self, job):
        """Return ``{rtos: [glob, ...]}`` for the job's actions/cache paths."""
        job_rtos = self._job_rtos(job)
        groups = {}
        for step in self._steps(job):
            uses = step.get("uses") or ""
            if "actions/cache" not in uses:
                continue
            with_block = step.get("with") or {}
            paths = with_block.get("path")
            if paths is None:
                continue
            if isinstance(paths, str):
                paths = [paths]
            rtos = self._step_rtos(job, step) or job_rtos
            globs = []
            for path in paths:
                for line in str(path).splitlines():
                    line = line.strip()
                    if line:
                        globs.append(line)
            groups.setdefault(rtos, []).extend(globs)
        return groups

    def _step_rtos(self, job, step):
        """The RTOS a step applies to (matrix jobs select it with `if`)."""
        condition = step.get("if")
        if condition:
            match = re.search(r"matrix\.rtos\s*==\s*['\"](\w+)['\"]",
                              str(condition))
            if match:
                return match.group(1)
        matrix = (job.get("strategy") or {}).get("matrix") or {}
        rtos = matrix.get("rtos")
        if isinstance(rtos, list) and len(rtos) == 1:
            return rtos[0]
        return None

    def _job_rtos(self, job):
        """The RTOS of a non-matrix job (derived from its build step)."""
        for step in self._steps(job):
            match = re.search(r"make\s+test-qemu-(\w+)-trace",
                              _strip_comments(self._run_text(step)))
            if match:
                return match.group(1)
        return None

    # -- the individual properties ---------------------------------------

    def test_qemu_trace_jobs_fetch_sources_before_building(self):
        """Every trace job downloads the sources before the build step."""
        for rtos in TRACE_RTOS:
            with self.subTest(rtos=rtos):
                name, job, build_index = self._job_with_step(
                    rf"make\s+test-qemu-{rtos}-trace\b")
                self.assertIsNotNone(
                    name, f"no CI job runs 'make test-qemu-{rtos}-trace'")
                self.assertIsNotNone(
                    self._fetch_step(job, build_index),
                    f"job '{name}' builds the example app without fetching "
                    f"the RTOS sources first -- the build aborts with "
                    f"'RTOS sources not found'")

    def test_fetch_step_creates_the_config_before_the_download(self):
        """`ove download` needs a .config; a fresh checkout has none."""
        for rtos in TRACE_RTOS:
            with self.subTest(rtos=rtos):
                name, job, build_index = self._job_with_step(
                    rf"make\s+test-qemu-{rtos}-trace\b")
                fetch = self._fetch_step(job, build_index)
                self.assertIsNotNone(
                    fetch,
                    f"job '{name}' builds the example app without fetching "
                    f"the RTOS sources first -- the build aborts with "
                    f"'RTOS sources not found'")
                _index, step = fetch
                run = _strip_comments(self._run_text(step))
                config_at = run.find(f"qemu.{rtos}.example_c")
                download_at = DOWNLOAD_CMD_RE.search(run)
                self.assertIsNotNone(
                    download_at,
                    f"job '{name}': the fetch step runs no download command")
                self.assertGreaterEqual(
                    config_at, 0,
                    f"job '{name}': the fetch step does not configure "
                    f"qemu.{rtos}.example_c, so 'ove download' has no "
                    f".config to work with")
                self.assertLess(
                    config_at, download_at.start(),
                    f"job '{name}': the download runs before the workspace "
                    f"is configured")

    def test_fetch_step_configures_the_workspace_the_build_uses(self):
        """The download must land in the workspace the build consumes."""
        for rtos in TRACE_RTOS:
            with self.subTest(rtos=rtos):
                name, job, build_index = self._job_with_step(
                    rf"make\s+test-qemu-{rtos}-trace\b")
                fetch = self._fetch_step(job, build_index)
                self.assertIsNotNone(
                    fetch,
                    f"job '{name}' builds the example app without fetching "
                    f"the RTOS sources first -- the build aborts with "
                    f"'RTOS sources not found'")
                _index, step = fetch
                spec = self._config_spec(self._run_text(step))
                self.assertEqual(
                    spec, f"qemu.{rtos}.example_c",
                    f"job '{name}': the fetch step configures {spec!r}; the "
                    f"trace test builds in "
                    f"{WORKSPACE_TEMPLATE.replace('<rtos>', rtos)}")
        # The trace test must really build in that workspace.
        self.assertRegex(
            self.cli_text,
            r'"qemu",\s*rtos,\s*"example_c",\s*"build"',
            "the trace test no longer looks for the firmware in "
            f"{WORKSPACE_TEMPLATE.replace('<rtos>', '<rtos>')}/build")
        self.assertRegex(
            self.cli_text,
            r'spec=f"qemu\.\{rtos\}\.example_c"',
            "the trace test no longer configures qemu.<rtos>.example_c")

    def test_matrix_job_fetches_sources_before_building(self):
        """The Kconfig-matrix job downloads before the matrix build."""
        name, job, build_index = self._job_with_step(
            r"make\s+test-kconfig-matrix\b")
        self.assertIsNotNone(name, "no CI job runs 'make test-kconfig-matrix'")
        self.assertIsNotNone(
            self._fetch_step(job, build_index),
            f"job '{name}' runs the Kconfig matrix without fetching the "
            f"RTOS sources first")
        fetch = self._fetch_step(job, build_index)
        self.assertIsNotNone(
            fetch,
            f"job '{name}' runs the Kconfig matrix without fetching the "
            f"RTOS sources first")
        _index, step = fetch
        run = _strip_comments(self._run_text(step))
        spec = self._config_spec(self._run_text(step))
        self.assertIsNotNone(
            spec, f"job '{name}': the fetch step configures no workspace")
        self.assertRegex(
            spec, r"^qemu\.\$\{\{\s*matrix\.rtos\s*\}\}\.example_c$",
            f"job '{name}': the fetch step configures {spec!r}; the matrix "
            f"script builds in {WORKSPACE_TEMPLATE.replace('<rtos>', '<rtos>')}")
        download_at = DOWNLOAD_CMD_RE.search(run)
        self.assertIsNotNone(download_at,
                             f"job '{name}': the fetch step runs no download")
        self.assertLess(run.find(spec), download_at.start(),
                        f"job '{name}': the download runs before the "
                        f"workspace is configured")

    def test_matrix_script_downloads_once_before_building(self):
        """ove-config-matrix.py downloads once, after the config, before the build."""
        calls = _script_run_calls(MATRIX_SCRIPT)
        kinds = [(lineno, words) for lineno, words in calls]
        downloads = [item for item in kinds if "download" in item[1]]
        builds = [item for item in kinds if "build" in item[1]]
        defconfigs = [item for item in kinds if "defconfig-fragments" in item[1]]
        self.assertTrue(defconfigs,
                        "ove-config-matrix.py no longer configures a workspace")
        self.assertEqual(
            len(downloads), 1,
            "ove-config-matrix.py must download exactly once (not once per "
            f"Kconfig combination), got {len(downloads)} downloads")
        self.assertTrue(builds, "ove-config-matrix.py no longer builds")
        self.assertLess(
            defconfigs[0][0], downloads[0][0],
            "'ove download' needs the .config that defconfig-fragments "
            "writes")
        self.assertLess(
            downloads[0][0], builds[0][0],
            "ove-config-matrix.py downloads only after its first build")

    def test_matrix_script_builds_the_workspace_ci_configures(self):
        """The matrix workspace is the one the CI fetch step configures."""
        self.assertRegex(
            self.matrix_text,
            r'spec\s*=\s*f"qemu\.\{args\.rtos\}\.\{args\.app\}"',
            "the matrix script no longer configures qemu.<rtos>.<app>")
        self.assertRegex(
            self.matrix_text,
            r'workspace\s*=\s*REPO\s*/\s*"output"\s*/\s*"qemu"\s*/\s*'
            r'args\.rtos\s*/\s*args\.app',
            "the matrix script no longer builds in output/qemu/<rtos>/<app>")

    def test_trace_test_fetches_sources_before_building(self):
        """The trace test keeps its own source-fetch guard."""
        tree = ast.parse(self.cli_text)
        function = None
        for node in ast.walk(tree):
            if isinstance(node, ast.FunctionDef) and \
                    node.name == "_run_sim_invariants":
                function = node
                break
        self.assertIsNotNone(function,
                             "config/ove-cli/ove/test.py has no "
                             "_run_sim_invariants()")
        ensure_line = None
        build_line = None
        for node in ast.walk(function):
            if not isinstance(node, ast.Call):
                continue
            name = getattr(node.func, "id", None) or \
                getattr(node.func, "attr", None)
            if name == "_ensure_rtos_sources" and ensure_line is None:
                ensure_line = node.lineno
            elif name == "cmd_build" and build_line is None:
                build_line = node.lineno
        self.assertIsNotNone(
            ensure_line,
            "_run_sim_invariants() does not fetch the RTOS sources -- a "
            "workspace without them aborts the build")
        self.assertIsNotNone(build_line,
                             "_run_sim_invariants() does not build")
        self.assertLess(ensure_line, build_line,
                        "the RTOS sources must be fetched before the build")

    def test_source_guard_accepts_what_the_build_requires(self):
        """The helper must look for the directories `ove build` requires."""
        expected = self.preflight_map
        self.assertTrue(expected,
                        "could not read the preflight source map from "
                        f"{BUILD_MODULE}")
        actual = _dict_literal(CLI_TEST, "_RTOS_SOURCE_DIRS")
        self.assertIsNotNone(
            actual,
            "config/ove-cli/ove/test.py has no _RTOS_SOURCE_DIRS map")
        self.assertEqual(
            {key: sorted(value) for key, value in actual.items()},
            {key: sorted(value) for key, value in expected.items()},
            "the source guard and the build's preflight check disagree "
            "about which directories hold the RTOS sources")

    def test_cached_source_paths_are_real_download_targets(self):
        """actions/cache aborts when a cached path does not exist."""
        self.assertTrue(self.download_targets,
                        f"could not read the download targets from "
                        f"{DOWNLOAD_MODULE}")
        for job_name, job in self.jobs.items():
            groups = self._cached_globs(job)
            if not groups:
                continue
            for rtos, globs in groups.items():
                for glob in globs:
                    if not glob.startswith("dl/"):
                        continue
                    pattern = re.sub(r"\$\{\{.*?\}\}", "*", glob)
                    matched = [name for name in self.download_targets
                               if fnmatch.fnmatch(f"dl/{name}-0123abcd",
                                                  pattern)]
                    self.assertTrue(
                        matched,
                        f"job '{job_name}': the cache path {glob!r} does "
                        f"not name a directory 'ove download' creates -- "
                        f"actions/cache reports a 'Path Validation Error' "
                        f"and saves nothing")
                if rtos and rtos in self.preflight_map:
                    candidates = self.preflight_map[rtos]
                    covered = False
                    for glob in globs:
                        if not glob.startswith("dl/"):
                            continue
                        pattern = re.sub(r"\$\{\{.*?\}\}", "*", glob)
                        for name in candidates:
                            if fnmatch.fnmatch(f"dl/{name}-0123abcd",
                                               pattern):
                                covered = True
                    self.assertTrue(
                        covered,
                        f"job '{job_name}': none of the cached dl/ paths "
                        f"covers the {rtos} sources the build needs "
                        f"({', '.join(candidates)})")


if __name__ == "__main__":
    unittest.main()
