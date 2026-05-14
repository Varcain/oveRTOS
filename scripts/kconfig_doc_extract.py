#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Extract Kconfig options into a MkDocs Markdown reference table."""

import re
from pathlib import Path

CONFIG_DIRS = [Path("config"), Path("apps"), Path("boards")]
OUT_FILE = Path("docs-site/docs/build-system/kconfig.md")


def parse_kconfig(path: Path) -> list[dict]:
    """Parse a Kconfig file and extract config entries."""
    entries = []
    lines = path.read_text().splitlines()
    i = 0
    while i < len(lines):
        m = re.match(r"^config\s+(\w+)", lines[i])
        if m:
            entry = {"name": m.group(1), "type": "", "default": "",
                      "help": "", "source": str(path)}
            i += 1
            help_mode = False
            help_indent = None
            while i < len(lines):
                line = lines[i]
                stripped = line.strip()
                if help_mode:
                    if not stripped:
                        entry["help"] += "\n"
                    elif help_indent is None:
                        help_indent = len(line) - len(line.lstrip())
                        entry["help"] += stripped
                    elif len(line) - len(line.lstrip()) >= help_indent:
                        entry["help"] += " " + stripped
                    else:
                        break
                elif stripped.startswith(("bool", "int", "string", "hex", "tristate")):
                    parts = stripped.split(None, 1)
                    entry["type"] = parts[0]
                elif stripped.startswith("default"):
                    entry["default"] = stripped.removeprefix("default").strip()
                elif stripped.startswith("help"):
                    help_mode = True
                elif stripped.startswith(("config ", "menu ", "endmenu", "source", "if ", "endif")):
                    break
                i += 1
            entries.append(entry)
        else:
            i += 1
    return entries


def main():
    OUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    all_entries = []

    for d in CONFIG_DIRS:
        if not d.exists():
            continue
        for f in sorted(d.rglob("Config.in*")):
            all_entries.extend(parse_kconfig(f))

    lines = [
        "# Kconfig Options Reference\n",
        "Auto-generated from `config/Config.in.*` files.\n",
        "| Option | Type | Default | Description |",
        "|--------|------|---------|-------------|",
    ]
    for e in all_entries:
        desc = e["help"].strip().replace("|", "\\|")[:120]
        lines.append(
            f"| `CONFIG_{e['name']}` | {e['type']} | `{e['default']}` | {desc} |"
        )

    OUT_FILE.write_text("\n".join(lines) + "\n")
    print(f"Generated {len(all_entries)} Kconfig entries -> {OUT_FILE}")


if __name__ == "__main__":
    main()
