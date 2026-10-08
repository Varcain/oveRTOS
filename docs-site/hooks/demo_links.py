# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""MkDocs hook: resolve `demo:<app>` links to the browser demos.

The demos are not MkDocs pages.  `ove docs-demos` copies them into the
built site afterwards, one directory per app at the site root, next to
the pages.  A page links to one as

    [Browser demo](demo:example_c_heap)

and this hook turns that into a URL relative to the page.  The link then
follows the site wherever it is served from: the project path on GitHub
Pages, the root of a local `make docs-serve`.
"""

import re

from mkdocs.utils import get_relative_url

_DEMO_HREF = re.compile(r'href="demo:([A-Za-z0-9_]+)"')


def on_page_content(html, page, **_kwargs):
    return _DEMO_HREF.sub(
        lambda m: f'href="{get_relative_url(m.group(1) + "/", page.url)}"',
        html)
