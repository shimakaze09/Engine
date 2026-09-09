#!/usr/bin/env python3
"""Audit tracked C++ and CMake sources for comments that break the
commenting standard (docs/development/commenting-guidelines.md).

Complements check_source_comments.py (which enforces file-level comment
PRESENCE). Two tiers of finding:

Zero-tolerance (never allowlisted; C++ doc comments only):
  tautology            /// Handles <identifier>.
  template             machine-template stems that carry no information
  misplaced-access     /// directly above public:/private:/protected:
  misplaced-initlist   /// directly above a constructor init-list line

Standard classes (allowlisted per file while the sweep shrinks them):
  history-reference    issue/PR numbers or audit finding codes in a comment
                       outside the sanctioned TODO(#n)/FIXME(#n) markers and
                       regression-provenance comments (guideline 36, 37, 42,
                       57, 82); the tests/ tree is exempt because regression
                       provenance is encouraged there
  temporal-language    words that describe history or temporary perception
                       instead of the current design (guideline 72); tests/
                       exempt because a test legitimately narrates a sequence
  commented-out-code   statements or CMake commands committed as comments
                       (guideline 41)
  vague-todo           TODO/FIXME without a tracked issue (guideline 36, 37)
  developer-language   personal, emotional, or uncertain wording
                       (guideline 73, 74, 75)

The allowlist (tools/comment_quality_allowlist.txt) records, per file and
class, exactly how many findings the tree still carries. A file whose
count grows is red; a file whose count shrinks without the entry being
updated is also red (a stale entry is itself a finding), so every
cleanup deletes or lowers its own entries. The allowlist applies only to
this repository's checkout; --root fixtures run without one unless
--allowlist names a file.

Usage:
  python tools/check_comment_quality.py            # report, exit 1 if findings
  python tools/check_comment_quality.py --summary  # counts per file only
  python tools/check_comment_quality.py --limit 50 # cap detailed lines printed
  python tools/check_comment_quality.py --root DIR [--allowlist FILE]
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass

CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"}

ALLOWLIST_NAME = "comment_quality_allowlist.txt"

ZERO_TOLERANCE = ("tautology", "template", "misplaced-access", "misplaced-initlist")
STANDARD_CLASSES = (
    "history-reference",
    "temporal-language",
    "commented-out-code",
    "vague-todo",
    "developer-language",
)

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

# Sanctioned markers: a line carrying one may cite issues freely.
SANCTIONED_MARKER_RE = re.compile(r"\b(TODO|FIXME)\(#\d+\)")
REGRESSION_RE = re.compile(r"\bregression\b", re.IGNORECASE)

# History references: issue/PR numbers and audit finding codes.
ISSUE_REF_RE = re.compile(r"(?<![\w/&])#\d+\b|\bPR\s+#?\d+\b")
AUDIT_CODE_RE = re.compile(r"\b(audit|finding)s?\s+[A-Z]{1,2}-\d{1,3}\b")

# Temporal language: unambiguous history words and calendar dates.
# "previously" is deliberately absent: "a previously registered service"
# describes call ordering, not history.
TEMPORAL_RE = re.compile(
    r"\b(formerly|for now|used to be|going forward|in the future|"
    r"historically|originally|as of 20\d\d)\b|\b20\d\d-\d\d-\d\d\b|\blanded\b",
    re.IGNORECASE,
)

# Commented-out C++: control flow, jump statements, call statements, a
# stray closing brace, or a preprocessor include living inside a comment.
CPP_CODE_RES = [
    re.compile(r"^\s*//\s*(if|for|while|switch)\s*\(.*\)\s*\{?\s*$"),
    re.compile(r"^\s*//\s*(return|break|continue)\b[^;]*;\s*$"),
    re.compile(r"^\s*//\s*[A-Za-z_][\w:.]*(->\w+)*\(.*\)\s*;\s*$"),
    re.compile(r"^\s*//\s*\}\s*(else\s*\{)?\s*$"),
    re.compile(r"^\s*//\s*#include\s*[<\"]"),
]
# Commented-out CMake: a command invocation behind '#'.
CMAKE_CODE_RE = re.compile(
    r"^\s*#\s*(set|unset|target_\w+|add_\w+|include|find_package|option|if|"
    r"elseif|endif|foreach|endforeach|message|list|string|install|project|"
    r"cmake_minimum_required|FetchContent_\w+)\s*\([^)]"
)

VAGUE_TODO_RE = re.compile(r"\b(TODO|FIXME)\b(?!\(#\d+\))")

DEVELOPER_RE = re.compile(
    r"\bI think\b|\bI believe\b|\bI'm not sure\b|\bnot sure\b|\bprobably\b|"
    r"\bmaybe\b|\bstupid\b|\bhorrible\b|\bsucks\b|\bdumb\b|\bcrap\b",
    re.IGNORECASE,
)


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
    in_tests = rel.startswith("tests/")

    if not cmake:
        findings.extend(audit_zero_tolerance(lines, rel))

    comments = cmake_comment_lines(lines) if cmake else cpp_comment_lines(lines)
    for index, text, full in comments:
        line_no = index + 1
        marker = SANCTIONED_MARKER_RE.search(text) is not None
        if not in_tests and not marker and not REGRESSION_RE.search(text):
            if ISSUE_REF_RE.search(text) or AUDIT_CODE_RE.search(text):
                findings.append(
                    Finding(rel, line_no, "history-reference", text.strip())
                )
        if not in_tests and not marker and TEMPORAL_RE.search(text):
            findings.append(
                Finding(rel, line_no, "temporal-language", text.strip())
            )
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
        if DEVELOPER_RE.search(text):
            findings.append(
                Finding(rel, line_no, "developer-language", text.strip())
            )
    return findings


def audit_zero_tolerance(lines: list[str], rel: str) -> list[Finding]:
    """Returns the never-allowlisted doc-comment findings for a C++ file."""
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


def load_allowlist(path: pathlib.Path) -> dict[tuple[str, str], int]:
    """Parses '<path>\\t<class>\\t<count>' lines; '#' lines are comments."""
    allow: dict[tuple[str, str], int] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 3 or parts[1] not in STANDARD_CLASSES:
            raise SystemExit(f"malformed allowlist line in {path}: {raw!r}")
        allow[(parts[0], parts[1])] = int(parts[2])
    return allow


def apply_allowlist(
    findings: list[Finding], allow: dict[tuple[str, str], int]
) -> tuple[list[Finding], int, list[str]]:
    """Excuses allowlisted counts; reports excess and stale entries.

    Returns (remaining findings, excused count, stale-entry messages). Per
    (file, class) the first N findings are excused; a file with fewer
    findings than its entry is stale and must have the entry lowered.
    """
    counts: dict[tuple[str, str], int] = {}
    for f in findings:
        counts[(f.path, f.category)] = counts.get((f.path, f.category), 0) + 1

    stale: list[str] = []
    for key, allowed in sorted(allow.items()):
        actual = counts.get(key, 0)
        if actual < allowed:
            stale.append(
                f"stale allowlist entry: {key[0]}\t{key[1]}\t{allowed} "
                f"(the file now carries {actual}; lower or delete the entry)"
            )

    remaining: list[Finding] = []
    excused = 0
    seen: dict[tuple[str, str], int] = {}
    for f in findings:
        key = (f.path, f.category)
        if f.category in ZERO_TOLERANCE:
            remaining.append(f)
            continue
        used = seen.get(key, 0)
        if used < allow.get(key, 0):
            seen[key] = used + 1
            excused += 1
        else:
            remaining.append(f)
    return remaining, excused, stale


def main() -> int:
    """Runs the audit and reports findings; exit 1 when any exist."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", action="store_true")
    parser.add_argument("--limit", type=int, default=200)
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        help="audit this tree instead of the repository (no git, no allowlist)",
    )
    parser.add_argument(
        "--allowlist",
        type=pathlib.Path,
        help="allowlist file (default: tools/%s for the repository)" % ALLOWLIST_NAME,
    )
    args = parser.parse_args()

    repo_root = pathlib.Path(__file__).resolve().parent.parent
    root = args.root.resolve() if args.root else repo_root
    files = tracked_files(root) if root == repo_root else walked_files(root)

    allowlist_path = args.allowlist
    if allowlist_path is None and root == repo_root:
        allowlist_path = repo_root / "tools" / ALLOWLIST_NAME
    allow = (
        load_allowlist(allowlist_path)
        if allowlist_path is not None and allowlist_path.is_file()
        else {}
    )

    findings: list[Finding] = []
    for path in files:
        rel = path.relative_to(root).as_posix()
        findings.extend(audit_file(path, rel))

    remaining, excused, stale = apply_allowlist(findings, allow)

    printed = 0
    per_file: dict[str, int] = {}
    for f in remaining:
        per_file[f.path] = per_file.get(f.path, 0) + 1
        if not args.summary and printed < args.limit:
            print(f"{f.path}:{f.line}: [{f.category}] {f.text}")
            printed += 1
    for message in stale:
        print(message)

    if args.summary:
        for rel in sorted(per_file, key=per_file.get, reverse=True):
            print(f"{per_file[rel]:5d}  {rel}")

    total = len(remaining) + len(stale)
    print(f"\ncomment quality findings: {total} ({excused} allowlisted)")
    return 1 if total > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
