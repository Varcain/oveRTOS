# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""MkDocs hook: resolve `demo:<rtos>/<app>` links to the browser demos.

The demos are not MkDocs pages.  `ove docs-demos` builds the ones
docs-site/demos.yml lists into the built site afterwards, at
demos/<rtos>/<app>/.  A page links to one as

    [Browser demo](demo:freertos/example_c_heap)

and this hook turns that into a URL relative to the page, so the link
follows the site wherever it is served from.  A link to a demo the list
leaves out becomes plain "(TBD)" text instead.
"""

import os
import re

import yaml
from mkdocs.utils import get_relative_url

_DEMO_LINK = re.compile(r'<a href="demo:([A-Za-z0-9_/-]+)"([^>]*)>(.*?)</a>',
                        re.S)
_demos = set()


def on_config(config, **_kwargs):
    path = os.path.join(os.path.dirname(config.config_file_path), "demos.yml")
    with open(path) as f:
        _demos.clear()
        for spec in yaml.safe_load(f):
            _board, rtos, app = spec.split(".")
            _demos.add(f"{rtos}/{app}")


def _link(match, page):
    name, attrs, text = match.groups()
    if name not in _demos:
        return f'<span class="demo-tbd">{text} (TBD)</span>'
    href = get_relative_url(f"demos/{name}/", page.url)
    return f'<a href="{href}"{attrs}>{text}</a>'


def on_page_content(html, page, **_kwargs):
    return _DEMO_LINK.sub(lambda m: _link(m, page), html)
