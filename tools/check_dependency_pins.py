#!/usr/bin/env python3
"""Audit third-party dependency pins: FetchContent tags and Actions refs.

CLAUDE.md requires every FetchContent dependency to be SHA-pinned, and a
CI workflow step that runs under a mutable tag can change without a
repository commit. Both are supply-chain boundaries: what a cold build
downloads and what a privileged runner executes must be fixed by a commit
in this repository, never by a tag a third party can move. This gate
makes that rule mechanical in the shape of the other audits: it reports
findings and exits non-zero, and CI holds it at zero.

Two checks, one root cause each:

  1. FetchContent declarations. Every `FetchContent_Declare(...)` that
     fetches from git must carry a `GIT_TAG` that is a full 40-hex commit
     SHA; a tag or branch name, a variable, or a missing `GIT_TAG` is a
     finding. A declaration that downloads a `URL` must carry `URL_HASH`.
     A declaration naming neither (a SOURCE_DIR override, say) downloads
     nothing and is not audited.

  2. GitHub Actions references. Every `uses:` in `.github/workflows/`
     that names a remote action must pin it at a full 40-hex commit SHA.
     Local (`./...`) and `docker://` references are outside this check.

Actions still referenced by a mutable tag today are listed in
KNOWN_UNPINNED_ACTIONS with the issue that tracks them. The entry is the
exact reference text, so pinning one action removes exactly one line. An
entry that no longer matches anything is itself a finding, which keeps
the list shrinking to empty. Adding an entry is not mechanically
prevented; that half stays [REVIEW].

Usage:
  python tools/check_dependency_pins.py            # report, exit 1 on findings
  python tools/check_dependency_pins.py --root DIR # audit an alternate tree
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
from collections.abc import Iterator

SHA_RE = re.compile(r"^[0-9a-f]{40}$")
DECLARE_RE = re.compile(r"FetchContent_Declare\s*\(", re.IGNORECASE)
# `uses:` as a mapping key or as a list item, with an optional quoted
# value; a leading `#` never matches, so commented-out steps are ignored.
USES_RE = re.compile(r"^\s*(?:-\s+)?uses:\s*[\"']?([^\s\"'#]+)")
CMAKE_SUFFIXES = {".cmake"}
CMAKE_NAMES = {"CMakeLists.txt"}
WORKFLOW_SUFFIXES = {".yml", ".yaml"}
# Build trees and fetched dependencies carry their own CMake files, which
# are not this repository's declarations.
SKIPPED_DIR_NAMES = {".git", "_deps"}
SKIPPED_DIR_PREFIXES = ("build",)

# Remote actions this repository still references by a mutable tag. Each
# entry is the exact `uses:` text; pinning it to the tag's commit SHA
# deletes the entry in the same commit, and a stale entry fails the gate.
KNOWN_UNPINNED_ACTIONS: dict[str, str] = {
    "actions/cache@v5": "tracked: issue #352, pin to the tag's commit SHA",
    "actions/checkout@v6": "tracked: issue #352, pin to the tag's commit SHA",
    "actions/download-artifact@v7": (
        "tracked: issue #352, pin to the tag's commit SHA"
    ),
    "actions/upload-artifact@v7": (
        "tracked: issue #352, pin to the tag's commit SHA"
    ),
    "ilammy/msvc-dev-cmd@v1": "tracked: issue #352, pin to the tag's commit SHA",
}


class Finding:
    """One unpinned dependency, rendered as a single report line."""

    def __init__(self, location: str, message: str) -> None:
        self.location = location
        self.message = message

    def __str__(self) -> str:
        return f"  {self.location}: {self.message}"


def skipped_directory(name: str) -> bool:
    """True for directories whose CMake files are not ours to audit."""
    return name in SKIPPED_DIR_NAMES or name.startswith(SKIPPED_DIR_PREFIXES)


def cmake_files(root: pathlib.Path) -> list[pathlib.Path]:
    """Every CMake listfile and module under the root, build trees excluded."""
    found: list[pathlib.Path] = []
    for directory, subdirectories, files in os.walk(root):
        subdirectories[:] = sorted(
            name for name in subdirectories if not skipped_directory(name)
        )
        for name in sorted(files):
            path = pathlib.Path(directory) / name
            if name in CMAKE_NAMES or path.suffix.lower() in CMAKE_SUFFIXES:
                found.append(path)
    return found


def workflow_files(root: pathlib.Path) -> list[pathlib.Path]:
    """Every workflow definition GitHub would execute for this repository."""
    workflows = root / ".github" / "workflows"
    if not workflows.is_dir():
        return []
    return sorted(
        path
        for path in workflows.iterdir()
        if path.is_file() and path.suffix.lower() in WORKFLOW_SUFFIXES
    )


def read_text(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8-sig")


def strip_cmake_comments(text: str) -> str:
    """Drops `#` comments so a commented-out GIT_TAG cannot count as a pin.

    A `#` inside a quoted argument is literal; nothing else in CMake's
    argument grammar (bracket comments excepted) hides one.
    """
    stripped: list[str] = []
    for line in text.splitlines():
        kept: list[str] = []
        quoted = False
        for char in line:
            if char == '"':
                quoted = not quoted
            elif char == "#" and not quoted:
                break
            kept.append(char)
        stripped.append("".join(kept))
    return "\n".join(stripped)


def declarations(text: str) -> Iterator[tuple[int, list[str]]]:
    """Yields (line, argument tokens) for each FetchContent_Declare call."""
    for match in DECLARE_RE.finditer(text):
        depth = 1
        index = match.end()
        while index < len(text) and depth > 0:
            if text[index] == "(":
                depth += 1
            elif text[index] == ")":
                depth -= 1
            index += 1
        body = text[match.end() : index - 1]
        line = text.count("\n", 0, match.start()) + 1
        yield line, body.split()


def value_after(tokens: list[str], keyword: str) -> str | None:
    """The argument following a keyword, unquoted; None when absent."""
    for position, token in enumerate(tokens):
        if token == keyword and position + 1 < len(tokens):
            return tokens[position + 1].strip('"')
    return None


def check_fetchcontent(root: pathlib.Path) -> tuple[list[Finding], int]:
    """Check 1: every downloading declaration is pinned by content."""
    findings: list[Finding] = []
    audited = 0
    for path in cmake_files(root):
        text = strip_cmake_comments(read_text(path))
        relative = path.relative_to(root).as_posix()
        for line, tokens in declarations(text):
            name = tokens[0] if tokens else "<unnamed>"
            location = f"{relative}:{line}"
            if "GIT_REPOSITORY" in tokens or "GIT_TAG" in tokens:
                audited += 1
                tag = value_after(tokens, "GIT_TAG")
                if tag is None:
                    findings.append(
                        Finding(location, f"{name}: no GIT_TAG; pin a commit SHA")
                    )
                elif not SHA_RE.match(tag):
                    findings.append(
                        Finding(
                            location,
                            f"{name}: GIT_TAG {tag} is not a 40-hex commit SHA",
                        )
                    )
            elif "URL" in tokens:
                audited += 1
                if value_after(tokens, "URL_HASH") is None:
                    findings.append(
                        Finding(location, f"{name}: URL download has no URL_HASH")
                    )
    return findings, audited


def check_workflows(
    root: pathlib.Path, used: set[str], allowlisted: bool
) -> tuple[list[Finding], int]:
    """Check 2: every remote action reference is pinned to a commit SHA."""
    findings: list[Finding] = []
    audited = 0
    for path in workflow_files(root):
        relative = path.relative_to(root).as_posix()
        for number, line in enumerate(read_text(path).splitlines(), 1):
            match = USES_RE.match(line)
            if match is None:
                continue
            reference = match.group(1)
            if reference.startswith("./") or reference.startswith("docker://"):
                continue
            audited += 1
            location = f"{relative}:{number}"
            action, separator, version = reference.rpartition("@")
            if not separator or not action:
                findings.append(
                    Finding(location, f"{reference}: no version reference")
                )
                continue
            if SHA_RE.match(version):
                continue
            if allowlisted and reference in KNOWN_UNPINNED_ACTIONS:
                used.add(reference)
                continue
            findings.append(
                Finding(
                    location,
                    f"{reference}: mutable tag; pin to the tag's commit SHA "
                    f"({action}@<40-hex>, tag as a trailing comment)",
                )
            )
    return findings, audited


def check_stale_allowlist(used: set[str]) -> list[Finding]:
    """Flags allowlist entries that no longer match anything."""
    return [
        Finding(
            "tools/check_dependency_pins.py",
            f"stale allowlist entry: no workflow uses {reference} ({reason}); "
            "delete the entry",
        )
        for reference, reason in sorted(KNOWN_UNPINNED_ACTIONS.items())
        if reference not in used
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        default=str(pathlib.Path(__file__).resolve().parents[1]),
        help="repository root to audit (defaults to this checkout)",
    )
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()

    # The allowlist enumerates references in THIS repository, so it
    # applies only when auditing this checkout; an alternate root (the
    # gate's own self-tests) is audited with nothing excused.
    allowlisted = root == pathlib.Path(__file__).resolve().parents[1]

    used: set[str] = set()
    findings, declarations_checked = check_fetchcontent(root)
    workflow_findings, references_checked = check_workflows(
        root, used, allowlisted
    )
    findings += workflow_findings
    if allowlisted:
        findings += check_stale_allowlist(used)

    if findings:
        print("dependency pin audit failed:")
        for finding in findings:
            print(str(finding))
        return 1

    print(
        "dependency pin audit passed: "
        f"{declarations_checked} FetchContent declarations and "
        f"{references_checked} action references checked, "
        f"{len(used)} mutable action tags still allowlisted"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
