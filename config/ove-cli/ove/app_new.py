# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""`ove app new` — stamp a new external app from a template.

Stamps one of the templates under `templates/external-app/<lang>/` into a
fresh directory, substituting `{{NAME}}` / `{{CONFIG_NAME}}` /
`{{LIB_NAME}}` / `{{OVE_DIR}}` placeholders.  Optionally runs `git init`
in the new directory so it starts out as a self-contained repo.

Files ending in `.in` are template files: the suffix is stripped during
stamping, and the body is run through the placeholder substitution.
Files without the suffix are copied verbatim.
"""

import logging
import os
import re
import shutil
import subprocess
import sys

from .workspace import find_ove_dir

logger = logging.getLogger("ove")

LANGS = ("c", "cpp", "rust", "zig")

# Templates currently shipped under templates/external-app/.  The flag
# accepts every value the user might reasonably try; values without a
# corresponding directory error out at stamp time with a clear message.
TEMPLATES = ("hello", "lvgl", "net", "audio")

# Reserved names that would collide with built-in apps, make targets,
# or fragments.  Refusing them up-front beats discovering the conflict
# at build time.
_RESERVED = {
    "example", "benchmark", "example_net", "example_pm",
    "example_keyword_live", "lvgl_benchmark", "lvgl_gallery",
    "test", "clean", "all", "menuconfig", "build", "run", "flash",
}

_NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_-]*$")


def _validate_name(name: str) -> tuple[str, str, str]:
    """Validate and normalise the app name.

    Returns ``(display, config_name, lib_name)`` where:
      - ``display`` is the user-typed name verbatim (allows hyphens).
      - ``config_name`` is snake-case (hyphens → underscores), used as
        the ``config_name:`` field, the ``make <board>.<rtos>.<…>``
        token, and the file prefixes the build system emits.
      - ``lib_name`` is the same as ``config_name`` — Rust/Zig libraries
        cannot contain hyphens.
    """
    if not _NAME_RE.match(name):
        raise ValueError(
            f"invalid name {name!r}: must start with a letter and contain "
            "only letters, digits, underscores, or hyphens"
        )
    config_name = name.replace("-", "_").lower()
    if config_name in _RESERVED:
        raise ValueError(
            f"name {name!r} resolves to reserved identifier "
            f"{config_name!r} — pick another name"
        )
    return name, config_name, config_name


def _resolve_template_dir(ove_dir: str, lang: str, template: str) -> str:
    """Return the absolute path to the source template directory.

    For the initial cut, every language has only a `hello` template, and
    that template is the language directory itself
    (`templates/external-app/<lang>/`).  Future templates (`lvgl`,
    `net`, `audio`) would live under a per-template subdir but they're
    not implemented yet; we surface that explicitly rather than 404 at
    file-walk time.
    """
    if template != "hello":
        raise ValueError(
            f"template {template!r} is not implemented yet; only 'hello' "
            "ships in this version.  Open an issue if you'd like to see "
            "lvgl/net/audio templates."
        )
    root = os.path.join(ove_dir, "templates", "external-app", lang)
    if not os.path.isdir(root):
        raise FileNotFoundError(
            f"template directory not found: {root}.  Is OVE_DIR pointing "
            "at a real oveRTOS checkout?"
        )
    return root


def _is_empty_dir(path: str) -> bool:
    return not any(os.scandir(path))


def _substitute(text: str, ctx: dict) -> str:
    """Replace `{{TOKEN}}` placeholders in `text` with values from `ctx`.

    Unknown tokens are left in place so a stray `{{...}}` in a comment
    can't silently drop content.  Substitution is plain string replace
    — no escaping; tokens are not expected in code that produces
    `{{...}}` literals.
    """
    for token, value in ctx.items():
        text = text.replace("{{" + token + "}}", value)
    return text


def _stamp_file(src: str, dst: str, ctx: dict) -> None:
    """Copy `src` to `dst`, substituting placeholders if it's a `.in` file.

    The `.in` suffix is stripped from the destination path.  Non-`.in`
    files are copied byte-for-byte (preserves binary template assets if
    any are ever added).
    """
    is_template = src.endswith(".in")
    real_dst = dst[:-3] if is_template else dst   # strip ".in"

    os.makedirs(os.path.dirname(real_dst), exist_ok=True)

    if is_template:
        with open(src, "r", encoding="utf-8") as f:
            content = f.read()
        content = _substitute(content, ctx)
        with open(real_dst, "w", encoding="utf-8") as f:
            f.write(content)
        # Preserve executable bit if the template was marked executable.
        st = os.stat(src)
        if st.st_mode & 0o111:
            os.chmod(real_dst, st.st_mode)
    else:
        shutil.copy2(src, real_dst)


def _stamp_tree(src_root: str, dst_root: str, ctx: dict) -> list[str]:
    """Walk `src_root` and stamp every file under `dst_root`.

    Returns the list of materialised files (absolute paths) for the
    next-steps summary.
    """
    materialised = []
    for dirpath, _dirnames, filenames in os.walk(src_root):
        rel = os.path.relpath(dirpath, src_root)
        for fn in filenames:
            src = os.path.join(dirpath, fn)
            dst = (os.path.join(dst_root, fn)
                   if rel == "."
                   else os.path.join(dst_root, rel, fn))
            _stamp_file(src, dst, ctx)
            real = dst[:-3] if dst.endswith(".in") else dst
            materialised.append(real)
    return materialised


def _git_init(path: str) -> bool:
    """Run `git init` in `path`.  Returns True on success."""
    if not shutil.which("git"):
        logger.warning("git not found — skipping git init")
        return False
    try:
        subprocess.run(["git", "init", "--quiet", path],
                       check=True, capture_output=True)
        # Stage everything as the initial state but don't commit —
        # let the user inspect first.
        subprocess.run(["git", "-C", path, "add", "."],
                       check=True, capture_output=True)
        return True
    except subprocess.CalledProcessError as e:
        logger.warning("git init failed: %s",
                       e.stderr.decode(errors="replace").strip())
        return False


def cmd_app_new(args) -> None:
    """CLI entry point for `ove app new`."""
    try:
        display, config_name, lib_name = _validate_name(args.name)
    except ValueError as e:
        logger.error("%s", e)
        sys.exit(2)

    # Resolve OVE_DIR.  Order: --ove-dir, $OVE_DIR, the active checkout.
    ove_dir = args.ove_dir or os.environ.get("OVE_DIR") or find_ove_dir()
    ove_dir = os.path.abspath(ove_dir)

    try:
        template_dir = _resolve_template_dir(ove_dir, args.lang, args.template)
    except (ValueError, FileNotFoundError) as e:
        logger.error("%s", e)
        sys.exit(2)

    # Resolve destination directory.
    dst = os.path.abspath(args.dir or args.name)
    if os.path.exists(dst):
        if not os.path.isdir(dst):
            logger.error("destination %s exists and is not a directory", dst)
            sys.exit(2)
        if not _is_empty_dir(dst) and not args.force:
            logger.error(
                "destination %s is not empty; pass --force to overwrite",
                dst,
            )
            sys.exit(2)
    else:
        os.makedirs(dst)

    ctx = {
        "NAME": display,
        "CONFIG_NAME": config_name,
        "CONFIG_NAME_UPPER": config_name.upper(),
        "LIB_NAME": lib_name,
        "OVE_DIR": ove_dir,
    }

    logger.info("stamping %s template %s → %s",
                args.lang, args.template, dst)
    materialised = _stamp_tree(template_dir, dst, ctx)

    if not args.no_git:
        _git_init(dst)

    # Summary + next steps.
    print()
    print(f"  Created {len(materialised)} files under {dst}")
    print()
    print("  Next steps:")
    print(f"    cd {dst}")
    print(f"    make host.posix.{config_name}")
    print("    make")
    print("    make run")
    print()
    print(f"  See {dst}/README.md for the full reference.")
