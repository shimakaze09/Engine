#!/usr/bin/env python3
"""Audit first-party C++ for error-handling calls that abort the process.

With exceptions disabled (`_HAS_EXCEPTIONS=0`, `/EHs-c- /GR-`),
`std::expected::value()` and `std::optional::value()` do not throw on a
missing value — they call `std::terminate`. A single such call turns a
recoverable error into a process abort, which is exactly the outcome the
error-handling rule exists to prevent. The safe accessors are
`has_value()`, `operator*` and `error()`.

The tree carries zero such calls, so this gate starts at zero and has no
allowlist: a finding means a new one was introduced. Comments are
stripped before matching, so prose about the rule is not a finding.

Usage:
  python tools/check_error_handling.py            # report, exit 1 on findings
  python tools/check_error_handling.py --root DIR # audit an alternate tree
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"}

# Directories holding first-party engine code. Third-party sources are
# fetched into the build tree and never audited.
AUDITED_ROOTS = (
    "app",
    "audio",
    "content",
    "core",
    "editor",
    "math",
    "physics",
    "renderer",
    "runtime",
    "scripting",
    "tools",
)

VALUE_CALL_RE = re.compile(r"\.\s*value\s*\(\s*\)")


def strip_comments(lines: list[str]) -> list[str]:
    """Returns the lines with C++ comment text blanked out.

    Line and block comments become spaces so column-insensitive matching
    sees only code. String literals are not parsed: a '//' inside a
    string blanks the rest of that line, which can only hide a finding on
    that line, never invent one.
    """
    out: list[str] = []
    in_block = False
    for line in lines:
        result: list[str] = []
        index = 0
        while index < len(line):
            if in_block:
                end = line.find("*/", index)
                if end < 0:
                    break
                result.append(" " * (end + 2 - index))
                index = end + 2
                in_block = False
                continue
            block = line.find("/*", index)
            slash = line.find("//", index)
            if slash >= 0 and (block < 0 or slash < block):
                result.append(line[index:slash])
                break
            if block >= 0:
                result.append(line[index:block])
                index = block + 2
                in_block = True
                continue
            result.append(line[index:])
            break
        out.append("".join(result))
    return out


def audited_files(root: pathlib.Path) -> list[pathlib.Path]:
    """Returns every first-party C++ source under the audited roots."""
    files: list[pathlib.Path] = []
    for name in AUDITED_ROOTS:
        directory = root / name
        if not directory.is_dir():
            continue
        files.extend(
            path
            for path in directory.rglob("*")
            if path.is_file() and path.suffix.lower() in CPP_SUFFIXES
        )
    return sorted(files)


def main() -> int:
    """Runs the audit and reports findings; exit 1 when any exist."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        default=str(pathlib.Path(__file__).resolve().parents[1]),
        help="repository root to audit (defaults to this checkout)",
    )
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()

    findings: list[str] = []
    files = audited_files(root)
    for path in files:
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        rel = path.relative_to(root).as_posix()
        for number, line in enumerate(strip_comments(lines), 1):
            if VALUE_CALL_RE.search(line):
                findings.append(
                    f"  {rel}:{number}: .value() terminates with exceptions "
                    "disabled; use has_value()/operator*/error()"
                )

    if findings:
        print("error-handling audit failed:")
        for finding in findings:
            print(finding)
        return 1

    print(f"error-handling audit passed: {len(files)} file(s) checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
