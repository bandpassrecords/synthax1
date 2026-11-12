#!/usr/bin/env python3
"""Set the project version a release tag is checked against, then verify it.

Used by the release skill (see SKILL.md next to this file).

`.github/workflows/build.yml` fails a tag build unless, for tag vX.Y.Z, the first
`VERSION X.Y.Z` in CMakeLists.txt (the project() call) is X.Y.Z. The plugin and
its About text read that version, so a stale one would ship the wrong number.

Usage:
    python .claude/skills/release/prepare_release.py 0.2.0
    python .claude/skills/release/prepare_release.py 0.2.0 --check
    python .claude/skills/release/prepare_release.py 0.2.0 --dry-run

Exit status is non-zero whenever the tag would still fail CI.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

CMAKELISTS = "CMakeLists.txt"
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")
# The same pattern the workflow's sed uses: the first line that is only `VERSION X.Y.Z`.
PROJECT_VERSION_RE = re.compile(r"(?m)^([ \t]*VERSION[ \t]+)(\d+\.\d+\.\d+)")


def vkey(v: str) -> tuple[int, int, int]:
    a, b, c = (int(x) for x in v.split("."))
    return a, b, c


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("version", help="X.Y.Z, without the v")
    ap.add_argument("--check", action="store_true", help="only run the version gate; change nothing")
    ap.add_argument("--dry-run", action="store_true", help="show what would change; write nothing")
    ap.add_argument("--root", default=".", help="repository root")
    args = ap.parse_args()

    version = args.version.lstrip("v")
    if not VERSION_RE.match(version):
        sys.exit(f"error: version must be X.Y.Z (got {args.version})")

    path = os.path.join(args.root, CMAKELISTS)
    with open(path, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    match = PROJECT_VERSION_RE.search(text)
    if not match:
        sys.exit(f"error: no `VERSION X.Y.Z` line found in {CMAKELISTS}")
    current = match.group(2)

    if not args.check and current != version:
        if vkey(version) < vkey(current):
            sys.exit(f"error: {version} is older than the project version {current}")
        print(("[dry-run] " if args.dry_run else "") + f"{CMAKELISTS}: VERSION {current} -> {version}")
        if not args.dry_run:
            text = text[: match.start(2)] + version + text[match.end(2):]
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(text)
            current = version
    elif current == version:
        print(f"{CMAKELISTS} already has VERSION {version}")

    if args.dry_run:
        return 0
    if current != version:
        print(f"\nv{version} would FAIL build.yml: {CMAKELISTS} has VERSION {current}")
        return 1
    print(f"\nv{version} passes the build.yml version gate.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
