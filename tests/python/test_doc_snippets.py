# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Doc C blocks are found, fragments skipped and body excerpts wrapped."""

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


if __name__ == "__main__":
    unittest.main()
