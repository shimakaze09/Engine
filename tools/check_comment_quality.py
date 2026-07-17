#!/usr/bin/env python3
"""Audit tracked C++ sources for low-value or misplaced doc comments.

Complements check_source_comments.py (which enforces comment PRESENCE).
This script flags comment JUNK so the codebase's purpose comments stay
trustworthy:

  1. Tautology fillers:      /// Handles <identifier>.
  2. Known-wrong templates:  /// Handles void. / /// Handles operator=.
  3. Generic boilerplate:    "Adds a value or component to the target system"
                             on functions that are not component adders, etc.
  4. Misplaced doc comments: /// lines directly above access specifiers
                             (public:/private:/protected:) or inside
                             constructor member-init lists.

Usage:
  python tools/check_comment_quality.py            # report, exit 1 if findings
  python tools/check_comment_quality.py --summary  # counts per file only
  python tools/check_comment_quality.py --limit 50 # cap detailed lines printed
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"}

# /// Handles <words>.  — tautology template; never a real explanation.
HANDLES_RE = re.compile(r"^\s*///\s*Handles\s+[\w :+<>,=~\-\[\]().]*\.?\s*$")

# Other known machine-template stems that carry no information beyond the name.
TEMPLATE_RES = [
    re.compile(r"^\s*///\s*Stores\s+.*\s+data used by the engine\.\s*$"),
    re.compile(r"^\s*///\s*Owns the\s+.*\s+behavior and state\.\s*$"),
    re.compile(r"^\s*///\s*Returns the requested value(\s+for\s+.*)?\.\s*$"),
    re.compile(
        r"^\s*///\s*Adds a value or component to the target system"
        r"(\s+for\s+.*)?\.\s*$"
    ),
    re.compile(
        r"^\s*///\s*Removes a value or component from the target system"
        r"(\s+for\s+.*)?\.\s*$"
    ),
    re.compile(r"^\s*///\s*Compares values for equality\.\s*$"),
]

ACCESS_SPECIFIER_RE = re.compile(r"^\s*(public|private|protected)\s*:\s*$")
DOC_LINE_RE = re.compile(r"^\s*///")
# Continuation of a constructor init list or expression: doc comment directly
# above a line starting with ':' or inside parentheses is misplaced.
INIT_LIST_RE = re.compile(r"^\s*:\s*\w+\(")


def tracked_cpp_files(repo_root: pathlib.Path) -> list[pathlib.Path]:
    """Returns tracked C++ source/header paths under the engine modules."""
    result = subprocess.run(
        ["git", "ls-files"],
        cwd=repo_root,
        capture_output=True,
        text=True,
        check=True,
    )
    files: list[pathlib.Path] = []
    for line in result.stdout.splitlines():
        path = repo_root / line.strip()
        if path.suffix.lower() in CPP_SUFFIXES and path.is_file():
            files.append(path)
    return files


def audit_file(path: pathlib.Path) -> list[tuple[int, str, str]]:
    """Returns (line, category, text) findings for one file."""
    findings: list[tuple[int, str, str]] = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return findings

    for index, line in enumerate(lines):
        stripped = line.rstrip()
        if HANDLES_RE.match(stripped):
            findings.append((index + 1, "tautology", stripped.strip()))
            continue
        for template in TEMPLATE_RES:
            if template.match(stripped):
                findings.append((index + 1, "template", stripped.strip()))
                break
        else:
            if DOC_LINE_RE.match(stripped) and index + 1 < len(lines):
                next_line = lines[index + 1]
                if ACCESS_SPECIFIER_RE.match(next_line):
                    findings.append(
                        (index + 1, "misplaced-access", stripped.strip())
                    )
                elif INIT_LIST_RE.match(next_line):
                    findings.append(
                        (index + 1, "misplaced-initlist", stripped.strip())
                    )
    return findings


def main() -> int:
    """Runs the audit and reports findings; exit 1 when any exist."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", action="store_true")
    parser.add_argument("--limit", type=int, default=200)
    args = parser.parse_args()

    repo_root = pathlib.Path(__file__).resolve().parent.parent
    total = 0
    printed = 0
    per_file: dict[str, int] = {}

    for path in tracked_cpp_files(repo_root):
        findings = audit_file(path)
        if not findings:
            continue
        rel = path.relative_to(repo_root).as_posix()
        per_file[rel] = len(findings)
        total += len(findings)
        if not args.summary:
            for line_no, category, text in findings:
                if printed < args.limit:
                    print(f"{rel}:{line_no}: [{category}] {text}")
                    printed += 1

    if args.summary:
        for rel in sorted(per_file, key=per_file.get, reverse=True):
            print(f"{per_file[rel]:5d}  {rel}")

    print(f"\ncomment quality findings: {total}")
    return 1 if total > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
