#!/usr/bin/env python3
"""Reject large fixed-capacity arrays of non-scalar type that are
value-initialized with an empty braced list.

`std::array<T, N> m{};` and `std::array<T, N> m = std::array<T, N>();`
mean the same thing: value-initialization, zero then defaults, byte for
byte. MSVC's front end does not treat them the same. It lowers the braced
form element by element (its __builtin_array_init_helper), at a cost
linear in N, whenever T is not a scalar. For a default member initializer
of a non-template class that cost is paid in every translation unit that
includes the header, whether or not anything constructs the class. The
parenthesized form is lowered in constant time.

Measured with cl 19.44 and 19.51 on the CI image: one 65536-element
member costs 1.4 s of front end in each includer braced, 0.0 s
parenthesized; world.h alone cost 16 s before the change. Generated code
and construction time are the same in both spellings.

A finding is an array declared with an empty braced initializer whose
element type is not a scalar (fundamental, enum, pointer, or an alias of
one) and whose capacity is at least MIN_ELEMENTS or cannot be resolved
here (a template parameter, or a constant from elsewhere). Scalar arrays
are exempt: MSVC zero-fills those in constant time either way.

Usage:
  python tools/check_array_value_init.py            # report, exit 1 on findings
  python tools/check_array_value_init.py --fix      # rewrite findings in place
  python tools/check_array_value_init.py --root DIR # audit an alternate tree
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

# Below this many elements the braced form costs well under a millisecond
# per translation unit, so it is not worth a finding.
MIN_ELEMENTS = 256

MODULES = ("app", "audio", "content", "core", "editor", "math", "physics",
           "renderer", "runtime", "scripting", "tests", "tools")
SUFFIXES = (".h", ".hpp", ".cpp")

FUNDAMENTAL = re.compile(
    r"^(?:std::)?(?:u?int(?:8|16|32|64)_t|u?int_(?:fast|least)\d+_t|size_t|"
    r"ptrdiff_t|uintptr_t|intptr_t|byte|max_align_t)$|"
    r"^(?:signed |unsigned )?(?:char|short|int|long|long long|bool|float|"
    r"double|long double|wchar_t|char8_t|char16_t|char32_t)$|^unsigned$")

# std::array<ELEMENT, CAPACITY> NAME{};  (the declaration may wrap)
DECLARATION = re.compile(
    r"std::array<(?P<element>(?:[^<>;]|<(?:[^<>;]|<[^<>;]*>)*>)+?),"
    r"\s*(?P<capacity>[^;{}]+?)>\s*(?P<name>\w+)\s*\{\s*\}\s*;")
CONSTANT = re.compile(
    r"constexpr\s+(?:const\s+)?[\w:<>]+\s+(k\w+)\s*=\s*([^;{]+);")
ENUM = re.compile(r"\benum\s+(?:class\s+|struct\s+)?(\w+)")
ALIAS = re.compile(r"\busing\s+(\w+)\s*=\s*([^;]+);")


def source_files(root: pathlib.Path) -> list[pathlib.Path]:
    files = []
    for module in MODULES:
        base = root / module
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix in SUFFIXES and "_deps" not in path.parts:
                files.append(path)
    return sorted(files)


class Vocabulary:
    """Constants, enums and scalar aliases declared anywhere in the tree."""

    def __init__(self, files: list[pathlib.Path]) -> None:
        self.constants: dict[str, str] = {}
        self.enums: set[str] = set()
        aliases: dict[str, str] = {}
        for path in files:
            text = path.read_text(encoding="utf-8", errors="replace")
            for name, value in CONSTANT.findall(text):
                self.constants.setdefault(name, value.strip())
            self.enums.update(ENUM.findall(text))
            for name, target in ALIAS.findall(text):
                aliases.setdefault(name, target.strip())
        self.aliases = aliases

    def is_scalar(self, element: str, depth: int = 0) -> bool:
        element = re.sub(r"\bconst\b|\bvolatile\b", "", element).strip()
        if element.endswith("*") or FUNDAMENTAL.match(element):
            return True
        nested = re.match(r"^std::array<(.+),\s*[^,]+>$", element)
        if nested:
            return self.is_scalar(nested.group(1), depth)
        short = element.split("::")[-1]
        if short in self.enums:
            return True
        if depth < 4 and short in self.aliases:
            return self.is_scalar(self.aliases[short], depth + 1)
        return False

    def capacity(self, expression: str) -> int | None:
        """The capacity's value, or None when it cannot be resolved here."""
        expr = expression
        for _ in range(8):
            expr = re.sub(r"(?:\w+::)*\b(k\w+)\b",
                          lambda m: "(" + self.constants.get(m.group(1), m.group(0)) + ")",
                          expr)
            expr = expr.replace("ENGINE_MAX_ENTITIES", "65536")
        expr = re.sub(r"static_cast<[^>]+>", "", expr)
        expr = re.sub(r"(\d)[uU][lL]{0,2}\b", r"\1", expr)
        expr = re.sub(r"(?:std::)?size_t\b", "", expr)
        if re.search(r"[A-Za-z_]", expr):
            return None
        try:
            return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307
        except (SyntaxError, ZeroDivisionError, TypeError, NameError):
            return None


def findings(root: pathlib.Path) -> list[tuple[pathlib.Path, re.Match, int | None]]:
    files = source_files(root)
    vocabulary = Vocabulary(files)
    found = []
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in DECLARATION.finditer(text):
            element = match.group("element").strip()
            if vocabulary.is_scalar(element):
                continue
            count = vocabulary.capacity(match.group("capacity"))
            if count is not None and count < MIN_ELEMENTS:
                continue
            found.append((path, match, count))
    return found


def rewrite(match: re.Match) -> str:
    element = match.group("element").strip()
    capacity = match.group("capacity").strip()
    array_type = f"std::array<{element}, {capacity}>"
    head = match.group(0)[: match.start("name") - match.start(0)]
    return f"{head}{match.group('name')} = {array_type}();"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=None,
                        help="tree to audit (default: this repository)")
    parser.add_argument("--fix", action="store_true",
                        help="rewrite each finding to the parenthesized form")
    args = parser.parse_args()
    root = pathlib.Path(args.root) if args.root else \
        pathlib.Path(__file__).resolve().parent.parent

    found = findings(root)
    if args.fix:
        by_file: dict[pathlib.Path, list[re.Match]] = {}
        for path, match, _ in found:
            by_file.setdefault(path, []).append(match)
        for path, matches in by_file.items():
            text = path.read_text(encoding="utf-8")
            for match in sorted(matches, key=lambda m: m.start(), reverse=True):
                text = text[:match.start()] + rewrite(match) + text[match.end():]
            path.write_text(text, encoding="utf-8")
        print(f"rewrote {len(found)} declaration(s) in {len(by_file)} file(s)")
        return 0

    for path, match, count in found:
        line = path.read_text(encoding="utf-8", errors="replace")[:match.start()].count("\n") + 1
        size = "unresolved capacity" if count is None else f"{count} elements"
        print(f"{path.relative_to(root).as_posix()}:{line}: "
              f"{match.group('name')} ({size}) is value-initialized with an "
              f"empty braced list; write `= std::array<T, N>()`")
    if found:
        print(f"\n{len(found)} finding(s). MSVC expands the braced form element "
              "by element in every translation unit that sees it.")
        return 1
    print("array value-initialization audit passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
