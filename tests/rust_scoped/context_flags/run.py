# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and check the complete context metadata suite with exact transcripts."""

import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path

from profiles import PROFILE_NAMES

from build import build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    images = build(args.output.resolve(), args.sanitize)
    explicit_canary = os.environ.get("OVE_RUST_SCOPED_CANARY") == "1"
    if tuple(image["profile"] for image in images) != PROFILE_NAMES:
        raise ValueError("incomplete context image set")
    failures = 0
    observed = 0
    for image in images:
        for canary in (True,) if explicit_canary else (False, True):
            binary = Path(image["binary"])
            assert hashlib.sha256(binary.read_bytes()).hexdigest() == image["sha256"]
            command = [str(binary)] + (["canary"] if canary else [])
            completed = subprocess.run(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=15,
                check=False,
            )
            text = image["canary_text" if canary else "normal_text"].encode()
            valid = (
                not explicit_canary
                and completed.returncode == int(canary)
                and completed.stdout == text
            )
            suffix = "-canary" if canary else ""
            binary.with_name("result" + suffix + ".log").write_bytes(completed.stdout)
            binary.with_name("result" + suffix + ".status.json").write_text(
                json.dumps(
                    {
                        "command": command,
                        "status": completed.returncode,
                        "log_sha256": hashlib.sha256(completed.stdout).hexdigest(),
                    },
                    indent=2,
                )
                + "\n"
            )
            if not valid:
                failures += 1
                print(completed.stdout.decode(errors="backslashreplace"), end="")
            observed += 1
            print(
                f"{image['profile']}{suffix} {'PASS' if valid else 'FAIL'}", flush=True
            )
    if observed != (26 if explicit_canary else 52):
        raise ValueError("incomplete context observations")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
