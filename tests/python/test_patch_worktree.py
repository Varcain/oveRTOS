# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Regression tests for content-addressed RTOS patch worktrees."""

import os
import subprocess
import tempfile
import unittest

from ove.build import (
    _prepare_patched_git_tree,
    _shared_patched_tree,
    discard_all_patch_worktrees,
    discard_unreferenced_source_trees,
)


def _run(*args, cwd):
    subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True)


class _GitSourceCase(unittest.TestCase):
    """A one-commit Git source and a patch directory."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = self.temp.name
        self.source = os.path.join(self.root, "source")
        self.worktree = os.path.join(self.root, "worktree")
        self.patches = os.path.join(self.root, "patches")
        os.makedirs(self.source)
        os.makedirs(self.patches)

        _run("git", "init", "-q", cwd=self.source)
        _run("git", "config", "user.name", "ove test", cwd=self.source)
        _run("git", "config", "user.email", "ove@example.invalid",
             cwd=self.source)
        self._write(os.path.join(self.source, "a.txt"), "alpha\n")
        self._write(os.path.join(self.source, "b.txt"), "base\n")
        _run("git", "add", "a.txt", "b.txt", cwd=self.source)
        _run("git", "commit", "-qm", "base", cwd=self.source)

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def _write(path, value):
        with open(path, "w") as f:
            f.write(value)

    def _patch_a(self, value):
        self._write(
            os.path.join(self.patches, "0001-value.patch"),
            "diff --git a/a.txt b/a.txt\n"
            "index 4a58007..0000000 100644\n"
            "--- a/a.txt\n"
            "+++ b/a.txt\n"
            "@@ -1 +1 @@\n"
            "-alpha\n"
            f"+{value}\n")


class PatchWorktreeTest(_GitSourceCase):
    def _prepare(self, layers=None):
        if layers is None:
            layers = [("test patch", self.patches)]
        return _prepare_patched_git_tree(
            self.source, self.worktree, layers)

    def _read_worktree(self, name):
        with open(os.path.join(self.worktree, name)) as f:
            return f.read()

    def test_modified_and_deleted_patch_refresh_from_base(self):
        self._patch_a("bravo")
        self._prepare()
        self.assertEqual(self._read_worktree("a.txt"), "bravo\n")

        # A content edit under the same filename must not hit the old stamp.
        self._patch_a("charlie")
        self._prepare()
        self.assertEqual(self._read_worktree("a.txt"), "charlie\n")

        # Removing the patch must restore the pristine source revision.
        os.unlink(os.path.join(self.patches, "0001-value.patch"))
        self._prepare()
        self.assertEqual(self._read_worktree("a.txt"), "alpha\n")
        self.assertEqual(
            subprocess.run(
                ["git", "status", "--short"], cwd=self.source,
                check=True, capture_output=True, text=True).stdout,
            "")

    def test_damaged_patch_owned_path_is_rebuilt(self):
        self._patch_a("bravo")
        self._prepare()
        self._write(os.path.join(self.worktree, "a.txt"), "damaged\n")

        self._prepare()
        self.assertEqual(self._read_worktree("a.txt"), "bravo\n")

    def test_equal_basenames_in_distinct_layers_are_not_collapsed(self):
        second = os.path.join(self.root, "second-patches")
        os.makedirs(second)
        self._patch_a("bravo")
        self._write(
            os.path.join(second, "0001-value.patch"),
            "diff --git a/b.txt b/b.txt\n"
            "index df967b9..0000000 100644\n"
            "--- a/b.txt\n"
            "+++ b/b.txt\n"
            "@@ -1 +1 @@\n"
            "-base\n"
            "+second\n")

        self._prepare([
            ("first layer", self.patches),
            ("second layer", second),
        ])
        self.assertEqual(self._read_worktree("a.txt"), "bravo\n")
        self.assertEqual(self._read_worktree("b.txt"), "second\n")

    def test_global_cleanup_removes_generated_worktree_registration(self):
        dl_dir = os.path.join(self.root, "dl")
        west_topdir = os.path.join(dl_dir, "zephyr-workspace-test")
        os.makedirs(west_topdir)
        os.symlink(self.source, os.path.join(west_topdir, "zephyr"))
        generated = os.path.join(west_topdir, ".ove-worktrees", "test")
        self._patch_a("bravo")
        _prepare_patched_git_tree(
            self.source, generated, [("test patch", self.patches)])

        discard_all_patch_worktrees(dl_dir)

        self.assertFalse(os.path.exists(generated))
        worktrees = subprocess.run(
            ["git", "worktree", "list", "--porcelain"], cwd=self.source,
            check=True, capture_output=True, text=True).stdout
        self.assertNotIn(generated, worktrees)



class SharedSourceTreeTest(_GitSourceCase):
    """Workspaces with the same patch set share one tree under dl/."""

    def setUp(self):
        super().setUp()
        self.dl_dir = os.path.join(self.root, "dl")
        os.makedirs(self.dl_dir)
        os.rename(self.source, os.path.join(self.dl_dir, "source"))
        self.source = os.path.join(self.dl_dir, "source")

    def _workspace(self, name):
        class Ws:
            build_dir = os.path.join(self.root, "output", "board", "rtos", name, "build")
        return Ws()

    def _shared(self, ws):
        return _shared_patched_tree(ws, self.source, [("test patch", self.patches)])

    @staticmethod
    def _records(ws):
        with open(os.path.join(ws.build_dir, ".ove-source-trees")) as f:
            return f.read().split()

    def test_same_patch_set_shares_one_tree(self):
        self._patch_a("bravo")
        first, second = self._workspace("a"), self._workspace("b")
        tree = self._shared(first)
        self.assertEqual(self._shared(second), tree)
        self.assertEqual(os.path.dirname(tree), os.path.join(self.dl_dir, ".ove-worktrees"))
        with open(os.path.join(tree, "a.txt")) as f:
            self.assertEqual(f.read(), "bravo\n")
        self.assertEqual(self._records(first), [tree])
        self.assertEqual(self._records(second), [tree])

    def test_patch_content_selects_another_tree(self):
        ws = self._workspace("a")
        self._patch_a("bravo")
        bravo = self._shared(ws)
        self._patch_a("charlie")
        charlie = self._shared(ws)
        self.assertNotEqual(bravo, charlie)
        with open(os.path.join(bravo, "a.txt")) as f:
            self.assertEqual(f.read(), "bravo\n")

    def test_clean_sources_keeps_recorded_trees_only(self):
        kept, dropped = self._workspace("kept"), self._workspace("dropped")
        self._patch_a("bravo")
        used = self._shared(kept)
        self._patch_a("charlie")
        unused = self._shared(dropped)
        os.unlink(os.path.join(dropped.build_dir, ".ove-source-trees"))

        self.assertEqual(discard_unreferenced_source_trees(self.root, self.dl_dir), 1)
        self.assertTrue(os.path.isdir(used))
        self.assertFalse(os.path.exists(unused))
        worktrees = subprocess.run(
            ["git", "worktree", "list", "--porcelain"], cwd=self.source,
            check=True, capture_output=True, text=True).stdout
        self.assertNotIn(unused, worktrees)

    def test_clean_all_removes_shared_trees(self):
        self._patch_a("bravo")
        tree = self._shared(self._workspace("a"))
        discard_all_patch_worktrees(self.dl_dir)
        self.assertFalse(os.path.exists(tree))


if __name__ == "__main__":
    unittest.main()
