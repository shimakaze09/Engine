#!/usr/bin/env python3
"""Audit tracked source-like files for concise file-level comments."""

from __future__ import annotations

import pathlib
import subprocess
import sys


COMMENT_PREFIXES: dict[str, tuple[str, ...]] = {
    ".c": ("//", "/*"),
    ".cc": ("//", "/*"),
    ".cpp": ("//", "/*"),
    ".cxx": ("//", "/*"),
    ".h": ("//", "/*"),
    ".hh": ("//", "/*"),
    ".hpp": ("//", "/*"),
    ".hxx": ("//", "/*"),
    ".inl": ("//", "/*"),
    ".frag": ("//", "/*"),
    ".vert": ("//", "/*"),
    ".geom": ("//", "/*"),
    ".comp": ("//", "/*"),
    ".glsl": ("//", "/*"),
    ".lua": ("--", "--[["),
    ".py": ("#", '"""', "'''"),
    ".sh": ("#",),
    ".ps1": ("#", "<#"),
    ".bat": ("::", "REM", "@REM"),
    ".cmake": ("#",),
    ".yml": ("#",),
    ".yaml": ("#",),
}


def comment_prefixes(path: pathlib.Path) -> tuple[str, ...] | None:
    """Returns the accepted comment prefixes for a tracked file."""
    if path.name == "CMakeLists.txt":
        return ("#",)
    return COMMENT_PREFIXES.get(path.suffix.lower())


def first_content_line(lines: list[str]) -> str:
    """Returns the first non-empty line after an optional shebang."""
    index = 0
    while index < len(lines) and not lines[index].strip():
        index += 1
    if index < len(lines) and lines[index].startswith("#!"):
        index += 1
    while index < len(lines) and not lines[index].strip():
        index += 1
    if index >= len(lines):
        return ""
    return lines[index].lstrip("\ufeff").lstrip()


def has_file_comment(path: pathlib.Path, prefixes: tuple[str, ...]) -> bool:
    """Returns whether the file starts with an accepted file-level comment."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        lines = path.read_text(encoding="utf-8-sig").splitlines()
    line = first_content_line(lines)
    return any(line.startswith(prefix) for prefix in prefixes)


def tracked_files(repo_root: pathlib.Path) -> list[pathlib.Path]:
    """Returns all tracked files relative to the repository root."""
    result = subprocess.run(
        ["git", "-C", str(repo_root), "ls-files"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr, end="")
        raise SystemExit(result.returncode)
    return [repo_root / line for line in result.stdout.splitlines() if line]


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    checked = 0
    missing: list[str] = []

    for path in tracked_files(repo_root):
        prefixes = comment_prefixes(path)
        if prefixes is None:
            continue
        checked += 1
        if not has_file_comment(path, prefixes):
            missing.append(path.relative_to(repo_root).as_posix())

    if missing:
        print("source comment audit failed:")
        for file_name in missing:
            print(f"  {file_name}: missing file-level comment")
        return 1

    print(f"source comment audit passed: {checked} files checked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
