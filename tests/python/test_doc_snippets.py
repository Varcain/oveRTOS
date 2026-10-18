# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Doc C blocks are found, fragments skipped and body excerpts wrapped."""

import os
import tempfile
import unittest

from ove.lint_doc_snippets import _blocks, check_names

PAGE = """# Page

```c
void ove_main(void) { ove_run(); }
```

<!-- ove:fragment -->
```c
ove_queue_send(q, &v, 0);
```

<!-- ove:body -->
```c
ove_run();
```

```rust
fn main() {}
```
"""


class DocSnippetBlocksTest(unittest.TestCase):
    def test_blocks_and_markers(self):
        self.assertEqual(list(_blocks(PAGE)), [
            (3, "void ove_main(void) { ove_run(); }\n"),
            (13, "void ove_doc_excerpt(void)\n{\nove_run();\n}\n"),
        ])


class DocNamesTest(unittest.TestCase):
    def test_unknown_names_fail_and_prefixes_pass(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "include"))
            os.makedirs(os.path.join(root, "docs-site", "docs"))
            with open(os.path.join(root, "include", "a.h"), "w") as f:
                f.write("int ove_real(void);\n")
            with open(os.path.join(root, "docs-site", "docs", "p.md"), "w") as f:
                f.write("Use `ove_real()`, not `ove_fake()`; the `ove_mutex_` family and "
                        "`ove_X_init` are fine.\n\n```c\nove_in_code();\n```\n")
            status, detail = check_names(root)
        self.assertEqual(status, "FAIL")
        self.assertIn("ove_fake", detail)
        self.assertNotIn("ove_mutex_", detail)
        self.assertNotIn("ove_in_code", detail)


if __name__ == "__main__":
    unittest.main()
