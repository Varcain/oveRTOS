# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""The version-pin lint rule passes on the tree and catches each drift."""

import os
import re
import shutil
import tempfile
import unittest

from ove.lint_version import _HEADER, _MANIFESTS, _STUB, check

TREE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


class LintVersionTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.fresh()

    def tearDown(self):
        shutil.rmtree(self.root)

    def fresh(self):
        """The header and manifests, as in the tree, under self.root."""
        for rel in [_HEADER, _STUB] + [m for m, _ in _MANIFESTS]:
            dst = os.path.join(self.root, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy(os.path.join(TREE, rel), dst)

    def edit(self, rel, pattern, repl):
        path = os.path.join(self.root, rel)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        new, n = re.subn(pattern, repl, text, count=1, flags=re.MULTILINE)
        self.assertEqual(n, 1, f"{pattern} not found in {rel}")
        with open(path, "w", encoding="utf-8") as f:
            f.write(new)

    def failing_files(self):
        return [rel for rel, _ in check(self.root)]

    def test_tree_agrees(self):
        self.assertEqual(check(TREE), [])
        self.assertEqual(check(self.root), [])

    def test_each_manifest_drift_is_caught(self):
        for rel, section in _MANIFESTS:
            with self.subTest(rel=rel):
                self.fresh()
                if section is None:
                    self.edit(rel, r'^(\s*\.version\s*=\s*)"[^"]*"', r'\g<1>"9.9.9"')
                else:
                    self.edit(rel, r'^(version\s*=\s*)"[^"]*"', r'\g<1>"9.9.9"')
                self.assertEqual(self.failing_files(), [rel])

    def test_ove_macros_requirement(self):
        rel = "bindings/rust/ove/Cargo.toml"
        self.edit(rel, r'(ove-macros\s*=\s*\{[^}]*version\s*=\s*)"[^"]*"', r'\g<1>"0.0"')
        self.assertEqual(self.failing_files(), [rel])

    def test_header_string_must_match_numbers(self):
        self.edit(_HEADER, r'(#define\s+OVE_VERSION_STRING\s+)"[^"]*"', r'\g<1>"9.9.9"')
        self.assertEqual(self.failing_files(), [_HEADER])

    def test_header_bump_flags_every_manifest(self):
        self.edit(_HEADER, r"(#define\s+OVE_VERSION_PATCH\s+)\d+", r"\g<1>7")
        failing = self.failing_files()
        for rel, _ in _MANIFESTS:
            self.assertIn(rel, failing)
        self.assertIn(_HEADER, failing)  # the string still says .0
        self.assertIn(_STUB, failing)

    def test_stub_drift_is_caught(self):
        for pattern, repl in (
            (r"(pub const OVE_VERSION_MINOR: u32 = )\d+", r"\g<1>9"),
            (r'(pub const OVE_VERSION_STRING: &\[u8; \d+\] = b)"[^"]*"', r'\g<1>"9.9.9\\0"'),
            (r"(pub const OVE_VERSION: u32 = )\d+", r"\g<1>1"),
        ):
            with self.subTest(pattern=pattern):
                self.fresh()
                self.edit(_STUB, pattern, repl)
                self.assertEqual(self.failing_files(), [_STUB])


if __name__ == "__main__":
    unittest.main()
