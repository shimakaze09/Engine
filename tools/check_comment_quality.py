#!/usr/bin/env python3
"""Audit tracked C++ and CMake sources for objectively bad comments.

Complements check_source_comments.py, which enforces file-level comment
PRESENCE. This gate checks only what a machine can judge without reading
for meaning, so it has no allowlist and every finding is a defect:

  tautology            /// Handles <identifier>.
  template             machine-template stems that carry no information
  misplaced-access     /// directly above public:/private:/protected:
  misplaced-initlist   /// directly above a constructor init-list line
  commented-out-code    statements or CMake commands committed as comments
  vague-todo           TODO/FIXME without a tracked issue

Comment *prose* quality — whether a comment narrates history, reads as a
temporary note, or is worded loosely — is authoring-time guidance in the
`comment` skill, not a merge gate. Policing it mechanically required a
shared per-file allowlist that every change had to edit, which serialized
all concurrent work on one file and made the gate a scheduling
bottleneck rather than a quality signal (docs/decisions/0006, 0009).

Usage:
  python tools/check_comment_quality.py            # report, exit 1 if findings
  python tools/check_comment_quality.py --summary   # counts per file only
  python tools/check_comment_quality.py --limit 50  # cap detailed lines
  python tools/check_comment_quality.py --root DIR  # audit a fixture tree
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass

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

# Commented-out C++: control flow, jump statements, call statements, or a
# preprocessor include living inside a comment. A lone closing brace is
# deliberately absent: it carries almost no evidence that code was commented
# out, and it false-positives on every documented JSON or data-format example.
CPP_CODE_RES = [
    re.compile(r"^\s*//\s*(if|for|while|switch)\s*\(.*\)\s*\{?\s*$"),
    re.compile(r"^\s*//\s*(return|break|continue)\b[^;]*;\s*$"),
    re.compile(r"^\s*//\s*[A-Za-z_][\w:.]*(->\w+)*\(.*\)\s*;\s*$"),
    re.compile(r"^\s*//\s*#include\s*[<\"]"),
]
# Commented-out CMake: a command invocation behind '#'.
CMAKE_CODE_RE = re.compile(
    r"^\s*#\s*(set|unset|target_\w+|add_\w+|include|find_package|option|if|"
    r"elseif|endif|foreach|endforeach|message|list|string|install|project|"
    r"cmake_minimum_required|FetchContent_\w+)\s*\([^)]"
)

# A tracked marker cites its issue; a bare TODO/FIXME is untracked work.
VAGUE_TODO_RE = re.compile(r"\b(TODO|FIXME)\b(?!\(#\d+\))")


@dataclass(frozen=True)
class Finding:
    """One audit hit: file-relative path, 1-based line, class, text."""

    path: str
    line: int
    category: str
    text: str


def is_cmake(path: pathlib.Path) -> bool:
    """Returns whether the path is a CMake source."""
    return path.name == "CMakeLists.txt" or path.suffix.lower() == ".cmake"


def is_audited(path: pathlib.Path) -> bool:
    """Returns whether the path is a C++ or CMake source the audit covers."""
    return path.suffix.lower() in CPP_SUFFIXES or is_cmake(path)


def tracked_files(repo_root: pathlib.Path) -> list[pathlib.Path]:
    """Returns tracked audited paths from git."""
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
        if is_audited(path) and path.is_file():
            files.append(path)
    return files


def walked_files(root: pathlib.Path) -> list[pathlib.Path]:
    """Returns audited paths under a fixture tree (no git required)."""
    return sorted(p for p in root.rglob("*") if p.is_file() and is_audited(p))


def cpp_comment_lines(lines: list[str]) -> list[tuple[int, str, str]]:
    """Yields (index, comment_text, full_line) for every C++ comment line.

    Line comments contribute their '//' tail; block comments contribute
    each line's content between the delimiters. String literals are not
    parsed, so a '//' inside a string is treated as a comment — the audit
    prefers a rare false positive over missing a real comment.
    """
    out: list[tuple[int, str, str]] = []
    in_block = False
    for index, line in enumerate(lines):
        if in_block:
            end = line.find("*/")
            text = line if end < 0 else line[:end]
            out.append((index, text, line))
            if end >= 0:
                in_block = False
            continue
        block = line.find("/*")
        slash = line.find("//")
        if slash >= 0 and (block < 0 or slash < block):
            out.append((index, line[slash:], line))
            continue
        if block >= 0:
            end = line.find("*/", block + 2)
            if end < 0:
                in_block = True
                out.append((index, line[block:], line))
            else:
                out.append((index, line[block : end + 2], line))
    return out


def cmake_comment_lines(lines: list[str]) -> list[tuple[int, str, str]]:
    """Yields (index, comment_text, full_line) for every CMake comment."""
    out: list[tuple[int, str, str]] = []
    for index, line in enumerate(lines):
        hash_pos = line.find("#")
        if hash_pos >= 0:
            out.append((index, line[hash_pos:], line))
    return out


def audit_file(path: pathlib.Path, rel: str) -> list[Finding]:
    """Returns every finding for one file."""
    findings: list[Finding] = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return findings

    cmake = is_cmake(path)
    if not cmake:
        findings.extend(audit_doc_comments(lines, rel))

    comments = cmake_comment_lines(lines) if cmake else cpp_comment_lines(lines)
    for index, text, full in comments:
        line_no = index + 1
        if cmake:
            code = CMAKE_CODE_RE.match(full) is not None
        else:
            code = any(r.match(full) for r in CPP_CODE_RES)
        if code:
            findings.append(
                Finding(rel, line_no, "commented-out-code", full.strip())
            )
        if VAGUE_TODO_RE.search(text):
            findings.append(Finding(rel, line_no, "vague-todo", text.strip()))
    return findings


def audit_doc_comments(lines: list[str], rel: str) -> list[Finding]:
    """Returns the filler and misplacement findings for a C++ file."""
    findings: list[Finding] = []
    for index, line in enumerate(lines):
        stripped = line.rstrip()
        if HANDLES_RE.match(stripped):
            findings.append(Finding(rel, index + 1, "tautology", stripped.strip()))
            continue
        for template in TEMPLATE_RES:
            if template.match(stripped):
                findings.append(
                    Finding(rel, index + 1, "template", stripped.strip())
                )
                break
        else:
            if DOC_LINE_RE.match(stripped) and index + 1 < len(lines):
                next_line = lines[index + 1]
                if ACCESS_SPECIFIER_RE.match(next_line):
                    findings.append(
                        Finding(rel, index + 1, "misplaced-access", stripped.strip())
                    )
                elif INIT_LIST_RE.match(next_line):
                    findings.append(
                        Finding(
                            rel, index + 1, "misplaced-initlist", stripped.strip()
                        )
                    )
    return findings


def main() -> int:
    """Runs the audit and reports findings; exit 1 when any exist."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", action="store_true")
    parser.add_argument("--limit", type=int, default=200)
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        help="audit this tree instead of the repository (no git required)",
    )
    args = parser.parse_args()

    repo_root = pathlib.Path(__file__).resolve().parent.parent
    root = args.root.resolve() if args.root else repo_root
    files = tracked_files(root) if root == repo_root else walked_files(root)

    findings: list[Finding] = []
    for path in files:
        rel = path.relative_to(root).as_posix()
        findings.extend(audit_file(path, rel))

    printed = 0
    per_file: dict[str, int] = {}
    for f in findings:
        per_file[f.path] = per_file.get(f.path, 0) + 1
        if not args.summary and printed < args.limit:
            print(f"{f.path}:{f.line}: [{f.category}] {f.text}")
            printed += 1

    if args.summary:
        for rel in sorted(per_file, key=per_file.get, reverse=True):
            print(f"{per_file[rel]:5d}  {rel}")

    print(f"\ncomment quality findings: {len(findings)}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
