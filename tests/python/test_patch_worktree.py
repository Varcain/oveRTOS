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
    discard_all_patch_worktrees,
    discard_workspace_patch_worktrees,
)


def _run(*args, cwd):
    subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True)


class PatchWorktreeTest(unittest.TestCase):
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

    def test_workspace_cleanup_tolerates_missing_config(self):
        class MissingConfigWorkspace:
            @property
            def rtos(self):
                raise FileNotFoundError("no .config")

        discard_workspace_patch_worktrees(MissingConfigWorkspace())


if __name__ == "__main__":
    unittest.main()
