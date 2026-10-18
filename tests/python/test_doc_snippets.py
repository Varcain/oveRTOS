# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Cookbook C blocks are found, and fragments are skipped."""

import unittest

from ove.lint_doc_snippets import _blocks

PAGE = """# Page

```c
void ove_main(void) { ove_run(); }
```

<!-- ove:fragment -->
```c
ove_queue_send(q, &v, 0);
```

```rust
fn main() {}
```
"""


class DocSnippetBlocksTest(unittest.TestCase):
    def test_c_blocks_without_fragment_mark(self):
        self.assertEqual(list(_blocks(PAGE)), [(3, "void ove_main(void) { ove_run(); }\n")])


if __name__ == "__main__":
    unittest.main()
