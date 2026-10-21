# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Regression tests for the NuttX patch layers the `ove test` paths use.

`ove test`'s NuttX build paths (_run_nuttx_sim, _run_nuttx_qemu,
_build_renode_stm32f746_nuttx) used to compile backends/nuttx/nuttx_thread.c
against the *pristine* dl/nuttx-<hash> clone.  The backend's scheduler-note
driver assigns `.ready`, a member that only the config/patches/nuttx layer
adds, so those paths could not compile the backend once
CONFIG_OVE_THREAD_STATE_STATS was enabled.

The checks here are static (the sources are parsed and the functions under
test are exec'd with stubs) so the test is fast, hermetic and needs neither
a NuttX checkout nor a toolchain.
"""

import ast
import os
import re
import shutil
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
# The repo root; overridable so the same test can be pointed at a copy of
# the tree (used to prove that it fails on the pre-fix sources).
ROOT = os.environ.get("OVE_PATCH_LAYERS_ROOT") or os.path.dirname(
    os.path.dirname(_HERE))

TEST_PY = os.path.join("config", "ove-cli", "ove", "test.py")
BUILD_PY = os.path.join("config", "ove-cli", "ove", "build.py")
BACKEND_C = os.path.join("backends", "nuttx", "nuttx_thread.c")
TEMPLATE = os.path.join("config", "templates", "nuttx_defconfig.j2")
GLOBAL_PATCH_DIR = os.path.join("config", "patches", "nuttx")

# The `ove test` NuttX build paths that compile backends/nuttx/nuttx_thread.c.
NUTTX_TEST_PATHS = ("_run_nuttx_sim", "_run_nuttx_qemu",
                    "_build_renode_stm32f746_nuttx")

# The macro the patched NuttX header defines and the backend keys its
# patch-only note op off.
READY_MACRO = "OVE_NUTTX_HAVE_SCHED_NOTE_READY"


def _read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as f:
        return f.read()


def _functions(source):
    """Map function name -> ast node for the module-level functions."""
    tree = ast.parse(source)
    return {node.name: node for node in tree.body
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))}


def _segment(source, node):
    """The source text of one function."""
    return ast.get_source_segment(source, node)


def _exec_function(source, node, namespace):
    """Compile one function into `namespace` and return the namespace."""
    exec(compile(_segment(source, node), "<ove-cli>", "exec"), namespace)
    return namespace


def _exec_constant(source, name, namespace):
    """Compile one module-level constant into `namespace`."""
    for node in ast.parse(source).body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == name
                for t in node.targets):
            exec(compile(ast.get_source_segment(source, node), "<ove-cli>",
                         "exec"), namespace)
            return namespace
    raise KeyError(name)


def _called_name(node):
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
        return node.func.id
    return None


def _bound_names(assign):
    """The names an assignment binds (tuple assignment included)."""
    if isinstance(assign, ast.AnnAssign) and assign.value is not None:
        targets = [assign.target]
    elif isinstance(assign, ast.Assign):
        targets = assign.targets
    else:
        return []
    names = []
    for target in targets:
        if isinstance(target, ast.Name):
            names.append(target.id)
        elif isinstance(target, (ast.Tuple, ast.List)):
            names.extend(el.id for el in target.elts
                         if isinstance(el, ast.Name))
    return names


def _conditions_at(lines):
    """The preprocessor conditions each line is compiled under.

    A crude but sufficient #if/#ifdef/#else/#endif walker: it yields the
    conditions that hold for every line, so a line inside the `#else` branch
    of `#ifdef X` is not reported as being inside X.
    """
    result = []
    stack = []  # [condition, currently taken]

    def active():
        return [cond for cond, taken in stack if taken]

    for line in lines:
        stripped = line.strip()
        match = re.match(r"#\s*if(n?)def\s+(.*)$", stripped)
        if match:
            stack.append([match.group(2).strip(), match.group(1) != "n"])
            result.append(active())
            continue
        match = re.match(r"#\s*if\s+(.*)$", stripped)
        if match:
            stack.append([match.group(1).strip(), True])
            result.append(active())
            continue
        if re.match(r"#\s*el(se|if)\b", stripped):
            if stack:
                stack[-1][1] = not stack[-1][1]
            result.append(active())
            continue
        if re.match(r"#\s*endif\b", stripped):
            if stack:
                stack.pop()
            result.append(active())
            continue
        result.append(active())
    return result


class NuttXPatchLayerTest(unittest.TestCase):
    """The `ove test` NuttX paths must build against the patched kernel."""

    def setUp(self):
        self.test_src = _read(TEST_PY)
        self.build_src = _read(BUILD_PY)
        self.test_funcs = _functions(self.test_src)
        self.build_funcs = _functions(self.build_src)

    # -- the test paths resolve their kernel through the patched helper ----

    def test_test_paths_exist(self):
        for name in NUTTX_TEST_PATHS:
            self.assertIn(name, self.test_funcs)

    def test_test_paths_use_the_patched_kernel_helper(self):
        """Every NuttX test path must hand the *patched* tree to the build.

        The kernel source the path passes as `nuttx_src=` has to come from
        the patched-tree helper; the pristine dl/ clone must not reach the
        build directly.
        """
        for name in NUTTX_TEST_PATHS:
            node = self.test_funcs[name]
            with self.subTest(path=name):
                patched_names = set()
                pristine_used = False
                helper_calls = []
                for child in ast.walk(node):
                    called = _called_name(child)
                    if called == "_nuttx_patched_kernel":
                        helper_calls.append(child)
                    elif called == "_nuttx_fetch_sources":
                        pristine_used = True
                    if isinstance(child, ast.Assign) and \
                            _called_name(child.value) == \
                            "_nuttx_patched_kernel":
                        patched_names.update(_bound_names(child))
                self.assertTrue(
                    patched_names,
                    "%s does not resolve its kernel through "
                    "_nuttx_patched_kernel()" % name)
                self.assertFalse(
                    pristine_used,
                    "%s still uses the pristine dl/nuttx clone" % name)
                for call in helper_calls:
                    self.assertTrue(
                        any(kw.arg == "record_dir" for kw in call.keywords),
                        "%s does not record the shared tree for "
                        "`ove clean --sources`" % name)

                builds = [c for c in ast.walk(node)
                          if _called_name(c) == "_nuttx_cmake_build"]
                self.assertTrue(builds, "%s does not build NuttX" % name)
                for build in builds:
                    for kw in build.keywords:
                        if kw.arg != "nuttx_src":
                            continue
                        self.assertIsInstance(
                            kw.value, ast.Name,
                            "%s: nuttx_src must be a name" % name)
                        self.assertIn(
                            kw.value.id, patched_names,
                            "%s: nuttx_src does not come from "
                            "_nuttx_patched_kernel()" % name)

    def test_test_paths_pass_the_state_stats_overlay(self):
        """The overlay a path derives must reach its NuttX build.

        Without it the kernel keeps the scheduler instrumentation off while
        the backend is built with CONFIG_OVE_THREAD_STATE_STATS.
        """
        for name in NUTTX_TEST_PATHS:
            node = self.test_funcs[name]
            with self.subTest(path=name):
                stats_names = set()
                for child in ast.walk(node):
                    if isinstance(child, ast.Assign) and \
                            _called_name(child.value) == \
                            "_nuttx_state_stats_overlay":
                        stats_names.update(_bound_names(child))
                self.assertTrue(stats_names,
                                "%s derives no state-stats overlay" % name)
                # The lists the overlay is appended to.
                carriers = set()
                for child in ast.walk(node):
                    if isinstance(child, ast.Call) and \
                            isinstance(child.func, ast.Attribute) and \
                            child.func.attr == "append" and \
                            isinstance(child.func.value, ast.Name) and \
                            len(child.args) == 1 and \
                            isinstance(child.args[0], ast.Name) and \
                            child.args[0].id in stats_names:
                        carriers.add(child.func.value.id)
                builds = [c for c in ast.walk(node)
                          if _called_name(c) == "_nuttx_cmake_build"]
                passed = [kw.value.id for b in builds for kw in b.keywords
                          if kw.arg == "defconfig_overlays" and
                          isinstance(kw.value, ast.Name)]
                self.assertTrue(
                    passed and all(p in carriers for p in passed),
                    "%s does not pass its state-stats overlay to "
                    "_nuttx_cmake_build()" % name)

    def test_cmake_build_drops_a_cache_of_another_tree(self):
        """A build dir configured for another kernel tree is reconfigured.

        The tree is content-addressed, so the move off the pristine clone
        and every patch edit change it; CMake refuses to reuse a build dir
        whose cache names another source tree.
        """
        node = self.test_funcs["_nuttx_cmake_build"]
        drops = [c for c in ast.walk(node)
                 if _called_name(c) == "_drop_stale_cmake_cache"]
        self.assertTrue(drops, "_nuttx_cmake_build() does not drop a stale "
                               "CMake cache")
        self.assertEqual(
            [a.id for a in drops[0].args if isinstance(a, ast.Name)],
            ["build_dir", "nuttx_src"])
        runs = [c for c in ast.walk(node) if _called_name(c) == "run"]
        self.assertLess(drops[0].lineno, min(c.lineno for c in runs),
                        "the stale cache must go before the first cmake run")

        class _Logger:
            def info(self, *args):
                pass

        namespace = {"os": os, "shutil": shutil, "logger": _Logger()}
        _exec_function(self.build_src,
                       self.build_funcs["_drop_stale_cmake_cache"], namespace)
        drop = namespace["_drop_stale_cmake_cache"]
        temp = tempfile.mkdtemp()
        try:
            build = os.path.join(temp, "build")
            tree = os.path.join(temp, "patched")
            os.makedirs(build)
            os.makedirs(tree)
            cache = os.path.join(build, "CMakeCache.txt")
            with open(cache, "w") as f:
                f.write("CMAKE_HOME_DIRECTORY:INTERNAL=%s\n" % tree)
            drop(build, tree)
            self.assertTrue(os.path.isfile(cache),
                            "a cache of the same tree was dropped")
            drop(build, os.path.join(temp, "pristine"))
            self.assertFalse(os.path.exists(build),
                             "a cache of another tree was kept")
        finally:
            shutil.rmtree(temp, ignore_errors=True)

    # -- the helper itself ------------------------------------------------

    def _patched_helper(self):
        """The real _nuttx_patched_kernel, exec'd with recording stubs."""
        self.assertTrue("_nuttx_patched_kernel" in self.test_funcs,
                        "the test CLI has no _nuttx_patched_kernel()")
        namespace = {"os": os, "re": re}
        _exec_function(self.test_src,
                       self.test_funcs["_nuttx_patched_kernel"], namespace)

        # The real layer helper from build.py.
        self.assertTrue("nuttx_patch_layers" in self.build_funcs,
                        "the build CLI has no nuttx_patch_layers()")
        build_ns = {"os": os}
        _exec_function(self.build_src,
                       self.build_funcs["nuttx_patch_layers"], build_ns)
        namespace["nuttx_patch_layers"] = build_ns["nuttx_patch_layers"]

        seen = {}

        def fake_shared(ws, source_dir, layers, *, log_file=None,
                        record_dir=None):
            seen["ws"] = ws
            seen["source_dir"] = source_dir
            seen["layers"] = layers
            seen["record_dir"] = record_dir
            return os.path.join("/shared", "nuttx-patched")

        namespace["_nuttx_fetch_sources"] = lambda ove_dir: (
            "/dl/nuttx-24f210ef", "/dl/nuttx-apps-24f210ef", "/dl/cmocka")
        namespace["_shared_patched_tree"] = fake_shared
        return namespace, seen

    def test_patched_helper_applies_the_global_layer(self):
        """The helper must build the patched worktree, not the clone."""
        namespace, seen = self._patched_helper()
        tree, apps = namespace["_nuttx_patched_kernel"](
            "/repo", record_dir="/repo/output/tests/nuttx")
        self.assertEqual(tree, os.path.join("/shared", "nuttx-patched"))
        self.assertEqual(apps, "/dl/nuttx-apps-24f210ef")
        # The pristine clone is the *source* of the patched worktree.
        self.assertEqual(seen["source_dir"], "/dl/nuttx-24f210ef")
        # ... and the global patch layer must be part of the layer set.
        layer_dirs = [os.path.normpath(d) for _, d in seen["layers"]]
        self.assertIn(
            os.path.normpath(os.path.join("/", "repo", GLOBAL_PATCH_DIR)),
            layer_dirs)
        # No workspace: the test paths run outside one, but the tree must
        # still be recorded for `ove clean --sources`.
        self.assertIsNone(seen["ws"])
        self.assertEqual(seen["record_dir"], "/repo/output/tests/nuttx")

    def test_build_path_layers_are_unchanged(self):
        """`ove build` must keep applying the global *and* board layers."""
        self.assertTrue("nuttx_patch_layers" in self.build_funcs,
                        "the build CLI has no nuttx_patch_layers()")
        namespace = {"os": os}
        _exec_function(self.build_src,
                       self.build_funcs["nuttx_patch_layers"], namespace)
        temp = tempfile.mkdtemp()
        try:
            board_dir = os.path.join(temp, "boards", "an500")
            app_dir = os.path.join(temp, "apps", "thing")
            board = os.path.join(board_dir, "nuttx", "patches")
            app = os.path.join(app_dir, "patches", "nuttx")
            os.makedirs(board)
            os.makedirs(app)
            layers = namespace["nuttx_patch_layers"](
                os.path.join(temp, "config-root"), board_dir, app_dir)
            dirs = [os.path.normpath(d) for _, d in layers]
            self.assertIn(os.path.normpath(
                os.path.join(temp, "config-root", GLOBAL_PATCH_DIR)), dirs)
            self.assertIn(os.path.normpath(board), dirs)
            self.assertIn(os.path.normpath(app), dirs)
            # The order matters: global, then board, then app.
            self.assertLess(
                dirs.index(os.path.normpath(
                    os.path.join(temp, "config-root", GLOBAL_PATCH_DIR))),
                dirs.index(os.path.normpath(board)))
            self.assertLess(dirs.index(os.path.normpath(board)),
                            dirs.index(os.path.normpath(app)))
        finally:
            shutil.rmtree(temp, ignore_errors=True)

    def test_test_app_links_the_state_stats_clock(self):
        """The test app must build the backend's state-stats time source.

        nuttx_thread.c calls ove_state_stats_now_us() under
        CONFIG_OVE_THREAD_STATE_STATS; without nuttx_state_stats.c the
        NuttX test image does not link once the feature is on.
        """
        app = _read("tests", "sim", "_nuttx_app.cmake")
        self.assertIn("${_nuttx_be}/nuttx_thread.c", app)
        self.assertIn("${_nuttx_be}/nuttx_state_stats.c", app)

    # -- the backend's patch-only members --------------------------------

    def test_backend_patch_only_members_are_guarded(self):
        """`.ready` and obs_note_ready() must be behind the patch macro."""
        source = _read(BACKEND_C)
        lines = source.splitlines()
        conditions = _conditions_at(lines)

        def find(pattern):
            for index, line in enumerate(lines):
                if re.search(pattern, line):
                    return index
            return -1

        ready_index = find(r"^\s*\.ready\s*=\s*obs_note_ready\s*,")
        self.assertNotEqual(ready_index, -1,
                            "the backend no longer assigns the ready op")
        func_index = find(r"static void obs_note_ready\s*\(")
        self.assertNotEqual(func_index, -1,
                            "the backend no longer defines obs_note_ready()")

        for index, what in ((ready_index, ".ready = obs_note_ready"),
                             (func_index, "obs_note_ready()")):
            with self.subTest(member=what):
                self.assertIn(READY_MACRO, conditions[index],
                              "%s is not guarded by %s" % (what, READY_MACRO))
                self.assertIn("CONFIG_OVE_THREAD_STATE_STATS", conditions[index],
                              "%s is not inside the state-stats section"
                              % what)

    def test_backend_includes_the_patched_header_first(self):
        """The macro is only visible if note_driver.h is included first."""
        source = _read(BACKEND_C)
        lines = source.splitlines()
        conditions = _conditions_at(lines)
        include_index = -1
        for index, line in enumerate(lines):
            if "nuttx/note/note_driver.h" in line and \
                    line.strip().startswith("#include"):
                include_index = index
                break
        self.assertNotEqual(include_index, -1,
                            "the backend no longer includes note_driver.h")
        self.assertIn("CONFIG_OVE_THREAD_STATE_STATS", conditions[include_index])
        ready_index = next(
            i for i, line in enumerate(lines)
            if re.search(r"^\s*\.ready\s*=\s*obs_note_ready\s*,", line))
        self.assertLess(include_index, ready_index,
                        "note_driver.h must be included before the guard")

    def test_patch_defines_the_guard_macro(self):
        """The patched NuttX header must define the macro."""
        patch_dir = os.path.join(ROOT, GLOBAL_PATCH_DIR)
        self.assertTrue(os.path.isdir(patch_dir))
        found = False
        for name in sorted(os.listdir(patch_dir)):
            if not name.endswith(".patch"):
                continue
            with open(os.path.join(patch_dir, name), encoding="utf-8") as f:
                text = f.read()
            if ("#define %s" % READY_MACRO) in text:
                found = True
                self.assertIn("include/nuttx/note/note_driver.h", text,
                              "the macro must live in note_driver.h")
        self.assertTrue(found,
                        "no NuttX patch defines %s" % READY_MACRO)

    # -- the state-stats defconfig overlay --------------------------------

    def _overlay(self):
        self.assertTrue("_nuttx_state_stats_overlay" in self.test_funcs,
                        "the test CLI has no _nuttx_state_stats_overlay()")
        namespace = {"os": os, "re": re}
        _exec_constant(self.test_src, "_STATE_STATS_DEFCONFIG", namespace)
        _exec_function(self.test_src,
                       self.test_funcs["_nuttx_state_stats_overlay"],
                       namespace)
        return namespace["_nuttx_state_stats_overlay"]

    def test_state_stats_overlay_is_inert_without_the_feature(self):
        """Today's variants do not define the feature -> no overlay."""
        overlay = self._overlay()
        temp = tempfile.mkdtemp()
        try:
            variant = os.path.join(temp, "variant")
            os.makedirs(variant)
            with open(os.path.join(variant, "ove_config.h"), "w") as f:
                f.write("#define CONFIG_OVE_RTOS_NUTTX 1\n")
            self.assertIsNone(overlay(variant, os.path.join(temp, "out")))
            # A missing ove_config.h must not raise either.
            empty = os.path.join(temp, "empty")
            os.makedirs(empty)
            self.assertIsNone(overlay(empty, os.path.join(temp, "out")))
        finally:
            shutil.rmtree(temp, ignore_errors=True)

    def _inherited_variant(self, temp, shared_defines):
        """The heap variants' shape: the variant includes a shared header."""
        shared = os.path.join(temp, "shared")
        variant = os.path.join(temp, "variant")
        os.makedirs(shared)
        os.makedirs(variant)
        with open(os.path.join(shared, "ove_config.h"), "w") as f:
            f.write("#define CONFIG_OVE_RTOS_NUTTX 1\n" + shared_defines)
        with open(os.path.join(variant, "ove_config.h"), "w") as f:
            f.write('#include "../shared/ove_config.h"\n'
                    "#define CONFIG_OVE_PROTECTED 1\n")
        return variant

    def test_state_stats_overlay_follows_the_include_chain(self):
        """An inherited define must produce the overlay as well.

        The heap variants inherit the feature: the variant's own
        tests/sim/nuttx-qemu/ove_config.h #includes the shared
        tests/sim/nuttx/ove_config.h.  The backend's translation unit sees
        the inherited define (the variant directory comes first in its
        include path), so the kernel has to get the scheduler
        instrumentation too — a detection that reads only the variant's
        own header would leave CONFIG_SCHED_INSTRUMENTATION_SWITCH off and
        the NuttX build would not compile the backend.
        """
        overlay = self._overlay()
        temp = tempfile.mkdtemp()
        try:
            variant = self._inherited_variant(
                temp, "#define CONFIG_OVE_THREAD_STATE_STATS 1\n")
            path = overlay(variant, os.path.join(temp, "out"))
            self.assertIsNotNone(
                path, "the inherited define was not detected")
            with open(path, encoding="utf-8") as f:
                emitted = set(re.findall(r"^(CONFIG_\w+)=", f.read(),
                                         re.MULTILINE))
            self.assertIn("CONFIG_SCHED_INSTRUMENTATION_SWITCH", emitted)
            self.assertIn("CONFIG_DRIVERS_NOTE", emitted)
        finally:
            shutil.rmtree(temp, ignore_errors=True)

    def test_state_stats_overlay_ignores_an_inherited_chain_without_it(self):
        """The chain is followed, but it does not conjure the feature."""
        overlay = self._overlay()
        temp = tempfile.mkdtemp()
        try:
            variant = self._inherited_variant(temp, "")
            self.assertIsNone(overlay(variant, os.path.join(temp, "out")))
        finally:
            shutil.rmtree(temp, ignore_errors=True)

    def test_state_stats_overlay_respects_a_conditional_define(self):
        """The define is resolved the way the compiler resolves it."""
        overlay = self._overlay()
        temp = tempfile.mkdtemp()
        try:
            variant = os.path.join(temp, "variant")
            os.makedirs(variant)
            with open(os.path.join(variant, "ove_config.h"), "w") as f:
                f.write("#if 0\n#define CONFIG_OVE_THREAD_STATE_STATS 1\n"
                        "#endif\n")
            self.assertIsNone(overlay(variant, os.path.join(temp, "out")))
            with open(os.path.join(variant, "ove_config.h"), "w") as f:
                f.write("#if 1\n#define CONFIG_OVE_THREAD_STATE_STATS 1\n"
                        "#endif\n")
            self.assertIsNotNone(
                overlay(variant, os.path.join(temp, "out")))
        finally:
            shutil.rmtree(temp, ignore_errors=True)

    @unittest.skipUnless(any(shutil.which(p) for p in
                             ("cc", "gcc", "cpp", "clang")),
                         "no C preprocessor")
    def test_state_stats_overlay_sees_through_an_inactive_undef(self):
        """An #undef in a dead branch does not switch the feature off."""
        overlay = self._overlay()
        temp = tempfile.mkdtemp()
        try:
            variant = os.path.join(temp, "variant")
            os.makedirs(variant)
            with open(os.path.join(variant, "ove_config.h"), "w") as f:
                f.write("#define CONFIG_OVE_THREAD_STATE_STATS 1\n"
                        "#if 0\n#undef CONFIG_OVE_THREAD_STATE_STATS\n"
                        "#endif\n")
            self.assertIsNotNone(
                overlay(variant, os.path.join(temp, "out")))
        finally:
            shutil.rmtree(temp, ignore_errors=True)

    def test_state_stats_overlay_matches_the_defconfig_template(self):
        """The overlay must emit the symbols `ove build` emits."""
        overlay = self._overlay()
        temp = tempfile.mkdtemp()
        try:
            variant = os.path.join(temp, "variant")
            os.makedirs(variant)
            with open(os.path.join(variant, "ove_config.h"), "w") as f:
                f.write("#define CONFIG_OVE_THREAD_STATE_STATS 1\n")
            path = overlay(variant, os.path.join(temp, "out"))
            self.assertIsNotNone(path)
            emitted = set()
            with open(path, encoding="utf-8") as f:
                for line in f:
                    match = re.match(r"^(CONFIG_\w+)=", line)
                    if match:
                        emitted.add(match.group(1))

            template = _read(TEMPLATE)
            block = re.search(
                r"\{%\s*if config\.get\(\"CONFIG_OVE_THREAD_STATE_STATS\"\)"
                r"\s*== True\s*%\}(.*?)\{%\s*endif\s*%\}", template,
                re.DOTALL)
            self.assertIsNotNone(block, "the defconfig template lost the "
                                       "state-stats block")
            expected = set(re.findall(r"^(CONFIG_\w+)=", block.group(1),
                                      re.MULTILINE))
            self.assertTrue(expected)
            self.assertEqual(emitted, expected)
        finally:
            shutil.rmtree(temp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
