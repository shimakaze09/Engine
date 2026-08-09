#!/usr/bin/env python3
"""Audit tracked asset metadata for absolute developer-machine paths.

Cooked sidecars (.meta.json, .cookstamp) and other tracked asset JSON must
store repo-relative paths only (audit L-03): a Windows drive prefix or a
Unix absolute home/temp prefix means a developer machine's filesystem
layout leaked into shared metadata. Exits non-zero listing every offender.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

TRACKED_PATTERNS = ("assets/**/*.json", "assets/**/*.cookstamp")

ABSOLUTE_PATH_RE = re.compile(
    r"((?<![A-Za-z0-9])[A-Za-z]:[\\/])"
    r"|(\\\\[A-Za-z0-9_.$-]+\\)"
    r"|((?:^|[\"'=\s])/(?:home|Users|mnt|tmp|var|opt|root)/)"
)


def tracked_metadata_files() -> list[pathlib.Path]:
    """Returns tracked asset metadata files from git, newest layout aware."""
    result = subprocess.run(
        ["git", "ls-files", "-z", "--", *TRACKED_PATTERNS],
        cwd=REPO_ROOT, capture_output=True, text=True, check=True)
    names = [name for name in result.stdout.split("\0") if name]
    return [REPO_ROOT / name for name in names]


def scan_file(path: pathlib.Path) -> list[str]:
    """Returns 'line:match' style findings for one metadata file."""
    findings: list[str] = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        return [f"unreadable: {error}"]
    for line_number, line in enumerate(text.splitlines(), start=1):
        match = ABSOLUTE_PATH_RE.search(line)
        if match is not None:
            findings.append(f"line {line_number}: {line.strip()[:120]}")
    return findings


def main() -> int:
    """Scans every tracked metadata file and reports absolute paths."""
    files = tracked_metadata_files()
    failures = 0
    for path in files:
        for finding in scan_file(path):
            print(f"FAIL {path.relative_to(REPO_ROOT)}: {finding}")
            failures += 1
    if failures:
        print(f"\n{failures} absolute developer path(s) in tracked asset "
              "metadata; cook from the repository root so sidecars store "
              "repo-relative paths (audit L-03)")
        return 1
    print(f"PASS: {len(files)} tracked asset metadata files are free of "
          "absolute developer paths")
    return 0


if __name__ == "__main__":
    sys.exit(main())
