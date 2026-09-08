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
     finding. A declaration that downloads a `URL` must carry a literal
     `URL_HASH` (`<ALGO>=<hex digest>` with the digest length the
     algorithm produces); a variable, a generator expression, or a
     malformed digest is a finding, since any of them lets the effective
     hash change outside a repository commit. A declaration naming
     neither (a SOURCE_DIR override, say) downloads nothing and is not
     audited.

  2. GitHub Actions references. Every `uses:` in `.github/workflows/`
     and in every composite action manifest under `.github/actions/`
     that names a remote action must pin it at a full 40-hex commit SHA.
     Local (`./...`) and `docker://` references are outside this check.

Actions still referenced by a mutable tag are listed in
KNOWN_UNPINNED_ACTIONS with the issue that tracks them. The entry is the
exact reference text, so pinning one action removes exactly one line. An
entry that no longer matches anything is itself a finding, which keeps
the list at empty once it gets there. Adding an entry is not
mechanically prevented; that half stays [REVIEW].

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
# A composite action's manifest is the one file name GitHub reads for it.
ACTION_MANIFEST_NAMES = {"action.yml", "action.yaml"}
# Hex digest length per algorithm CMake's URL_HASH accepts; a literal
# `ALGO=<digest>` is the only form the gate credits.
URL_HASH_DIGEST_LENGTHS = {
    "MD5": 32,
    "SHA1": 40,
    "SHA224": 56,
    "SHA256": 64,
    "SHA384": 96,
    "SHA512": 128,
    "SHA3_224": 56,
    "SHA3_256": 64,
    "SHA3_384": 96,
    "SHA3_512": 128,
}
URL_HASH_RE = re.compile(r"^([A-Za-z0-9_]+)=([0-9a-fA-F]+)$")
# Build trees and fetched dependencies carry their own CMake files, which
# are not this repository's declarations.
SKIPPED_DIR_NAMES = {".git", "_deps"}
SKIPPED_DIR_PREFIXES = ("build",)

# Remote actions this repository still references by a mutable tag. Each
# entry is the exact `uses:` text; pinning it to the tag's commit SHA
# deletes the entry in the same commit, and a stale entry fails the gate.
# Empty: every reference is pinned, so any new mutable reference fails
# unless it is deliberately added here with the issue that tracks it.
KNOWN_UNPINNED_ACTIONS: dict[str, str] = {}


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
    """Every file GitHub resolves `uses:` references from for this repository.

    Workflows under `.github/workflows/` and the manifests of composite
    actions anywhere under `.github/actions/`: a step a workflow reaches
    through a local composite action runs the remote actions that
    manifest names, with the same privileges, so they are audited alike.
    """
    found: list[pathlib.Path] = []
    workflows = root / ".github" / "workflows"
    if workflows.is_dir():
        found += [
            path
            for path in workflows.iterdir()
            if path.is_file() and path.suffix.lower() in WORKFLOW_SUFFIXES
        ]
    actions = root / ".github" / "actions"
    if actions.is_dir():
        for directory, subdirectories, files in os.walk(actions):
            subdirectories.sort()
            found += [
                pathlib.Path(directory) / name
                for name in sorted(files)
                if name.lower() in ACTION_MANIFEST_NAMES
            ]
    return sorted(found)


def url_hash_finding(value: str | None) -> str | None:
    """Why a URL_HASH argument does not pin the download; None when it does."""
    if value is None:
        return "URL download has no URL_HASH"
    match = URL_HASH_RE.match(value)
    if match is None:
        return (
            f"URL_HASH {value} is not a literal <ALGO>=<hex digest>; a "
            "variable or generator expression can change outside a commit"
        )
    algorithm, digest = match.group(1).upper(), match.group(2)
    expected = URL_HASH_DIGEST_LENGTHS.get(algorithm)
    if expected is None:
        return f"URL_HASH algorithm {algorithm} is not one CMake accepts"
    if len(digest) != expected:
        return (
            f"URL_HASH {algorithm} digest has {len(digest)} hex characters; "
            f"{algorithm} produces {expected}"
        )
    return None


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
                problem = url_hash_finding(value_after(tokens, "URL_HASH"))
                if problem is not None:
                    findings.append(Finding(location, f"{name}: {problem}"))
    return findings, audited


def check_workflows(
    root: pathlib.Path, used: set[str], allowlisted: bool
) -> tuple[list[Finding], int]:
    """Check 2: every remote action reference is pinned to a commit SHA.

    Covers workflows and composite action manifests alike (workflow_files).
    """
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
