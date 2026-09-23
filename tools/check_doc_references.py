#!/usr/bin/env python3
"""Audit the project's documents for references to things that no longer
exist.

A document that names a file, a test or another document is making a
claim a reader will act on. When the code moves on and the document does
not, the claim goes quietly false: a contributor follows a path to a file
that was deleted, runs a test that was renamed, or trusts a link to a page
that is gone, and builds on the wrong picture. Nothing about the code
change itself says the document was left behind, so this gate does.

Every Markdown document under the audited set is checked for:

- relative Markdown links, `[text](target)`, whose target (without its
  #fragment) does not exist beside the document;
- repository paths written in backticks -- `renderer/src/x.cpp`,
  `tools/check_y.py:120` -- that do not exist from the repository root
  (a trailing :line or :line-line is allowed and ignored; paths holding a
  wildcard or a <placeholder> are templates, not claims, and are skipped);
- test names -- engine_unit_*, engine_integration_*, engine_bench_* --
  that no CMake file registers, either whole or as the prefix a ctest -R
  pattern selects by.

It cannot tell whether prose still describes what the code does; that is
the author's job in the same push as the change. It catches the part a
machine can: a reference to something that is not there.

Usage:
  python tools/check_doc_references.py            # report, exit 1 on findings
  python tools/check_doc_references.py --root DIR # audit an alternate tree
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

# The documents a contributor or agent reads to decide what to do.
AUDITED_GLOBS = (
    "README.md",
    "CLAUDE.md",
    "docs/**/*.md",
    ".claude/skills/**/*.md",
)

# A backticked token that names a repository file or directory: at least
# one slash, path characters only, optionally a :line or :line-line suffix.
PATH_TOKEN = re.compile(
    r"^(?P<path>(?:\./)?[A-Za-z0-9_.\-]+(?:/[A-Za-z0-9_.\-]+)+/?)"
    r"(?::\d+(?:-\d+)?)?$")
BACKTICKED = re.compile(r"`([^`\n]+)`")
LINK = re.compile(r"\[[^\]\n]*\]\(([^)\s]+)\)")
TEST_NAME = re.compile(r"\bengine_(?:unit|integration|bench)_[a-z0-9_]+\b")

# Top-level names a backticked path may start with. A token like
# `a/b` in prose that is not rooted here is a phrase, not a path claim.
ROOT_PREFIXES = (
    "app/", "assets/", "audio/", "cmake/", "content/", "core/", "docs/",
    "editor/", "math/", "physics/", "renderer/", "runtime/", "scripting/",
    "tests/", "tools/", ".github/", ".claude/",
)


def registered_tests(root: pathlib.Path) -> set[str]:
    """Every engine_* test name any CMake file under the tree mentions."""
    names: set[str] = set()
    for cmake in list(root.rglob("CMakeLists.txt")) + list(
            root.rglob("*.cmake")):
        parts = cmake.relative_to(root).parts
        if parts and (parts[0].startswith("build") or parts[0] == ".git"
                      or "_deps" in parts):
            continue
        try:
            text = cmake.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        names.update(TEST_NAME.findall(text))
    return names


def audited_documents(root: pathlib.Path) -> list[pathlib.Path]:
    found: set[pathlib.Path] = set()
    for pattern in AUDITED_GLOBS:
        found.update(p for p in root.glob(pattern) if p.is_file())
    return sorted(found)


def strip_fenced_blocks(text: str) -> list[tuple[int, str]]:
    """Lines outside ``` fences, with their 1-based numbers. Code blocks
    show commands and output, not references."""
    lines: list[tuple[int, str]] = []
    fenced = False
    for number, line in enumerate(text.splitlines(), start=1):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if not fenced:
            lines.append((number, line))
    return lines


def audit(root: pathlib.Path) -> list[str]:
    findings: list[str] = []
    tests = registered_tests(root)
    for doc in audited_documents(root):
        rel = doc.relative_to(root).as_posix()
        text = doc.read_text(encoding="utf-8", errors="replace")
        for number, line in strip_fenced_blocks(text):
            for target in LINK.findall(line):
                if re.match(r"^[a-z][a-z0-9+.-]*:", target) or \
                        target.startswith("#"):
                    continue
                path = target.split("#", 1)[0]
                if path and not (doc.parent / path).exists():
                    findings.append(
                        f"{rel}:{number}: link target does not exist: "
                        f"{target}")
            for token in BACKTICKED.findall(line):
                token = token.strip()
                match = PATH_TOKEN.match(token)
                if match is None:
                    continue
                path = match.group("path")
                if path.startswith("./"):
                    path = path[2:]
                if not path.startswith(ROOT_PREFIXES):
                    continue
                if not (root / path).exists():
                    findings.append(
                        f"{rel}:{number}: path does not exist: {token}")
            for name in TEST_NAME.findall(line):
                # A prefix of registered names is a ctest -R pattern.
                if name not in tests and not any(
                        test.startswith(name) for test in tests):
                    findings.append(
                        f"{rel}:{number}: no CMake file registers the "
                        f"test {name}")
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=None,
                        help="tree to audit (default: this repository)")
    args = parser.parse_args()
    root = pathlib.Path(args.root) if args.root else \
        pathlib.Path(__file__).resolve().parent.parent

    findings = audit(root)
    for finding in findings:
        print(finding)
    if findings:
        print(f"\n{len(findings)} stale reference(s) in the documents. "
              "Update the document in the same change that moved the code.")
        return 1
    print("document reference audit passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
