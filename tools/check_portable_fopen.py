#!/usr/bin/env python3
"""Audit first-party C++ for fopen calls the Windows lanes cannot build.

The MSVC C runtime marks `fopen` deprecated in favour of `fopen_s`, and the
engine builds with warnings as errors, so a bare `std::fopen` compiles on
Linux and macOS and fails on every Windows lane. The tree's idiom is to
open through `fopen_s` under `_WIN32` and keep `fopen` for the other
branch:

    #ifdef _WIN32
      if (fopen_s(&file, path, "wb") != 0) { file = nullptr; }
    #else
      file = std::fopen(path, "wb");
    #endif

A contributor on Linux gets no signal when they forget it — the break
appears only on a Windows build they may never run — so this gate gives
that signal everywhere: an `fopen` call is a finding unless it sits in a
preprocessor branch that Windows does not compile — the `#else` of a
`_WIN32` or `_MSC_VER` conditional, the body of a negated one, or a branch
that requires another platform (`__linux__`, `__APPLE__`, ...). Comments
and string literals are stripped before matching.

`tools/` is not audited: the asset packer defines
`_CRT_SECURE_NO_WARNINGS` for its own target, so the deprecation does not
reach it.

The tree carries zero such calls, so the gate starts at zero and has no
allowlist.

Usage:
  python tools/check_portable_fopen.py            # report, exit 1 on findings
  python tools/check_portable_fopen.py --root DIR # audit an alternate tree
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"}

# First-party trees. Third-party sources are fetched into the build tree
# and never audited. Tests are included: they build on the Windows lanes
# under the same flags as the engine.
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
    "tests",
)

# Macros that select the Windows branch, and macros whose presence means
# the branch is for some other platform.
WINDOWS_MACRO = re.compile(r"\b(_WIN32|_WIN64|_MSC_VER)\b")
OTHER_PLATFORM_MACRO = re.compile(
    r"\b(__linux__|__APPLE__|__unix__|__ANDROID__|__EMSCRIPTEN__)\b")

# `fopen(` not preceded by an identifier character, so fopen_s, _wfopen and
# my_fopen do not match while fopen and std::fopen do.
FOPEN_CALL = re.compile(r"(?<![A-Za-z0-9_])fopen\s*\(")
CONDITIONAL = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments and string/char literals, keeping line breaks so
    reported line numbers stay true."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif ch == "/" and nxt == "*":
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
        elif ch in ('"', "'"):
            quote = ch
            out.append(" ")
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    i += 1
                if i < n and text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def windows_skips(frames: list[dict]) -> bool:
    """True when some enclosing conditional is in a branch a Windows build
    does not compile."""
    return any(frame["non_windows"] for frame in frames)


def classify_branch(directive: str, condition: str) -> tuple[bool, bool]:
    """(names_windows, non_windows) for one `#if`-family branch: whether its
    condition selects Windows, and whether Windows skips it."""
    mentions_windows = WINDOWS_MACRO.search(condition) is not None
    if mentions_windows:
        negated = (directive == "ifndef" or re.search(
            r"!\s*(defined\s*\(?\s*)?(_WIN32|_WIN64|_MSC_VER)", condition)
            is not None)
        return (not negated, negated)
    if OTHER_PLATFORM_MACRO.search(condition) and "!" not in condition:
        return (False, True)
    return (False, False)


def audit_file(path: pathlib.Path) -> list[tuple[int, str]]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    findings = []
    frames: list[dict] = []
    original = text.splitlines()
    for number, line in enumerate(strip_comments_and_strings(text).splitlines(), 1):
        match = CONDITIONAL.match(line)
        if match:
            directive, condition = match.group(1), match.group(2)
            if directive in ("if", "ifdef", "ifndef"):
                names_windows, non_windows = classify_branch(directive,
                                                             condition)
                frames.append({"windows_seen": names_windows,
                               "non_windows": non_windows})
            elif directive == "elif" and frames:
                names_windows, non_windows = classify_branch(directive,
                                                             condition)
                frame = frames[-1]
                # Once an earlier branch took Windows, no later one can.
                frame["non_windows"] = non_windows or frame["windows_seen"]
                frame["windows_seen"] = frame["windows_seen"] or names_windows
            elif directive == "else" and frames:
                frame = frames[-1]
                frame["non_windows"] = frame["windows_seen"]
            elif directive == "endif" and frames:
                frames.pop()
            continue
        if FOPEN_CALL.search(line) and not windows_skips(frames):
            shown = original[number - 1].strip() if number <= len(original) else ""
            findings.append((number, shown))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=None,
                        help="tree to audit (default: this checkout)")
    args = parser.parse_args()
    root = (pathlib.Path(args.root) if args.root
            else pathlib.Path(__file__).resolve().parent.parent)

    total = 0
    for top in AUDITED_ROOTS:
        base = root / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in CPP_SUFFIXES or not path.is_file():
                continue
            for number, shown in audit_file(path):
                rel = path.relative_to(root).as_posix()
                print(f"{rel}:{number}: fopen is deprecated under the MSVC CRT "
                      f"and fails the Windows build; open through fopen_s "
                      f"under _WIN32: {shown}")
                total += 1

    if total:
        print(f"\n{total} finding(s). See the docstring of "
              f"tools/check_portable_fopen.py for the idiom.")
        return 1
    print("check_portable_fopen: no findings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
