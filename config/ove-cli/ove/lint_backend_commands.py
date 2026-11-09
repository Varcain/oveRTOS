# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Translate supported firmware and native compile commands for Clang analysis."""

import os
import re
import shlex
import subprocess


# These affect code generation or diagnostic presentation, not C types/headers.
_GCC_ONLY = {
    "-fno-defer-pop", "-fno-printf-return-value", "-fno-reorder-functions",
    "-fno-allow-store-data-races", "-mfix-cmse-cve-2021-35465",
    "-fprofile-abs-path",
}
_GCC_ONLY_PREFIXES = ("--param=", "-fdiagnostics-color=")
_GCC_PRIVATE_INCLUDE = re.compile(r"/lib/gcc/[^/]+/[^/]+/include(?:-fixed)?/?$")
_INCLUDE_OPTIONS = ("-isystem", "-idirafter", "-iquote", "-I")


def source_path(entry):
    """Resolve a database source against its command's working directory."""
    if not isinstance(entry, dict):
        raise ValueError("compile command entry must be an object")
    directory, source = entry.get("directory"), entry.get("file")
    if not isinstance(directory, str) or not os.path.isabs(directory):
        raise ValueError("compile command needs an absolute directory")
    if not isinstance(source, str) or not source:
        raise ValueError("compile command needs a source file")
    return os.path.abspath(os.path.join(directory, source))


def _clang_headers(args, directory):
    out = []
    replaced = False
    index = 0
    while index < len(args):
        arg = args[index]
        option = next((p for p in _INCLUDE_OPTIONS if arg.startswith(p)), None)
        if option:
            split = arg == option
            if split and index + 1 == len(args):
                raise ValueError("missing include path in compile command")
            path = args[index + 1] if split else arg[len(option):]
            if _GCC_PRIVATE_INCLUDE.search(os.path.abspath(os.path.join(directory, path))):
                replaced = True
                index += 2 if split else 1
                continue
        out.append(arg)
        index += 1
    # Only reopen resource headers when replacing explicit GCC resource headers.
    # All user include paths retain their original spelling and search priority.
    if replaced:
        out = ["-nostdlibinc" if a == "-nostdinc" else a for a in out]
    return out


def _target_family(target):
    if re.fullmatch(r"(?:arm|thumb)(?:v[0-9][a-z0-9]*)?-(?:none|zephyr)-eabi", target):
        return "arm"
    if re.fullmatch(r"x86_64-(?:[a-z0-9_]+-)*linux-gnu", target):
        return "native"
    raise ValueError("unsupported compiler target: " + target)


class BackendCommands:
    """Cache bounded native driver queries for one backend lint invocation."""

    def __init__(self):
        self.targets = {}

    def _driver_target(self, driver, directory):
        name = os.path.basename(driver)
        match = re.fullmatch(r"(.+)-(?:gcc|g\+\+)(?:-[0-9.]+)?", name)
        if match:
            return match[1]
        if not re.fullmatch(r"(?:cc|c\+\+|gcc|g\+\+|clang|clang\+\+)(?:-[0-9.]+)?", name):
            raise ValueError("unsupported compiler command: " + driver)
        key = (driver, directory)
        if key not in self.targets:
            try:
                process = subprocess.run([driver, "-dumpmachine"], cwd=directory,
                                         capture_output=True, text=True, timeout=10, check=True)
            except (OSError, subprocess.SubprocessError) as error:
                raise ValueError("cannot identify compiler target: " + driver) from error
            target = process.stdout.strip()
            _target_family(target)
            self.targets[key] = target
        return self.targets[key]

    def normalize(self, entry):
        source = source_path(entry)
        directory = entry["directory"]
        if not os.path.isfile(source):
            raise ValueError("missing backend source: " + source)
        if "arguments" in entry:
            args = entry["arguments"]
            if not isinstance(args, list) or not args or not all(isinstance(a, str) and a for a in args):
                raise ValueError("invalid compile command arguments")
            args = list(args)
        else:
            command = entry.get("command")
            if not isinstance(command, str) or not command:
                raise ValueError("missing compile command")
            try:
                args = shlex.split(command)
            except ValueError as error:
                raise ValueError("malformed compile command: " + str(error)) from error
            if not args:
                raise ValueError("empty compile command")
        if any(a.startswith("@") for a in args):
            raise ValueError("unsupported response file in compile command")
        driver, *options = args
        explicit = None
        remaining = []
        index = 0
        while index < len(options):
            arg = options[index]
            if arg in ("-target", "--target"):
                if index + 1 == len(options):
                    raise ValueError("missing explicit compiler target")
                explicit = options[index + 1]
                index += 2
            elif arg.startswith(("--target=", "-target=")):
                explicit = arg.split("=", 1)[1]
                index += 1
            else:
                remaining.append(arg)
                index += 1
        is_clang = re.fullmatch(r"clang(?:\+\+)?(?:-[0-9.]+)?", os.path.basename(driver))
        if explicit is not None:
            _target_family(explicit)
        inferred = None if explicit and is_clang else self._driver_target(driver, directory)
        if explicit and inferred and explicit != inferred:
            raise ValueError("explicit target disagrees with compiler command")
        target = explicit or inferred
        family = _target_family(target)
        widths = [a for a in remaining if a in ("-m32", "-m64", "-mx32")]
        if widths and (family != "native" or widths[-1] != "-m64"):
            raise ValueError("unsupported width for compiler target: " + widths[-1])
        out = [driver, "--target=" + target]
        single = False
        for arg in _clang_headers(remaining, directory):
            if arg in _GCC_ONLY or arg.startswith(_GCC_ONLY_PREFIXES):
                continue
            if arg.startswith("-mfp16-format="):
                if family != "arm" or arg != "-mfp16-format=ieee":
                    raise ValueError("unsupported half format: " + arg)
                continue
            if arg in ("-fsingle-precision-constant", "-fno-single-precision-constant"):
                single = arg == "-fsingle-precision-constant"
                continue
            out.append("-fhosted" if arg == "-fno-freestanding" else arg)
        if single:
            # Clang18 preserves literal types through this frontend option.
            # Double rounding can differ from GCC; see the lint documentation.
            out.extend(["-Xclang", "-cl-single-precision-constant"])
        if family == "arm" and not any(a.startswith("-mfloat-abi=") for a in remaining):
            out.append("-mfloat-abi=soft")
        result = dict(entry, file=source, arguments=out)
        result.pop("command", None)
        return result
