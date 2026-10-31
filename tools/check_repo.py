#!/usr/bin/env python3
"""Refuse to publish game assets, generated code, or huge files.

    python -I tools/check_repo.py

This scans the git index (not the working tree), so it also catches files that
were added and then deleted from disk without `git rm`. It is intentionally
conservative: the public mirror is source-only.

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 5 * 1024 * 1024

FORBIDDEN_DIRS = (
    "work/",
    "gen/",
    "gen_iop/",
    "gen.new/",
    "gen.old/",
    "build/",
    "dist/",
    "release/",
)

FORBIDDEN_SUFFIXES = (
    ".iso", ".chd", ".dol", ".elf", ".ps2", ".raw", ".irx", ".cue", ".nkit",
    ".rvz", ".wbfs", ".wia", ".ciso", ".o", ".a", ".map",
)

FORBIDDEN_NAMES = (
    "SLUS_210.50",
    "Mcd001.ps2",
    "burnout3.iso",
)


def tracked_files() -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=ROOT,
        capture_output=True,
        check=True,
    )
    return [p.decode("utf-8", "surrogateescape") for p in result.stdout.split(b"\0") if p]


def reason(path: str) -> str | None:
    if any(path.startswith(d) for d in FORBIDDEN_DIRS):
        return "generated/build directory"
    if path.startswith("build-"):
        return "generated/build directory"
    name = Path(path).name
    suffix = Path(path).suffix.lower()
    if suffix in FORBIDDEN_SUFFIXES:
        return f"forbidden file type {suffix}"
    if name in FORBIDDEN_NAMES:
        return "game asset or memory-card image"
    full = ROOT / path
    if full.is_file() and full.stat().st_size > MAX_BYTES:
        return f"file is larger than {MAX_BYTES // (1024 * 1024)} MiB"
    return None


def main() -> None:
    try:
        files = tracked_files()
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"[check] cannot list git files: {exc}", file=sys.stderr)
        raise SystemExit(2)

    bad: list[tuple[str, str]] = []
    for path in files:
        why = reason(path)
        if why:
            bad.append((path, why))

    if bad:
        print("[check] refusing to publish: game assets or generated files are tracked:\n")
        for path, why in bad:
            print(f"  {path}  ({why})")
        print(
            "\nRemove them from the index with `git rm --cached <path>` (and add them "
            "to .gitignore if needed), then run this check again.\n"
            "Do not rewrite already-published history unless you intend to force-push."
        )
        raise SystemExit(1)

    print(f"[check] OK: {len(files)} tracked file(s), no game assets or generated code.")


if __name__ == "__main__":
    main()
