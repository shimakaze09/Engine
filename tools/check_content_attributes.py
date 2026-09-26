#!/usr/bin/env python3
"""Audit that content-addressed files are pinned to their committed bytes.

The cook stamps record a hash of each authored source and of each cooked
output, and the runtime stale check re-hashes the working tree; a checkout
that rewrites line endings (Git for Windows defaults to core.autocrlf)
changes the hash of every glTF and reports every bundled prop stale. The
repository's .gitattributes marks those suffixes `-text` (or `binary`),
and this gate holds every tracked file with such a suffix to that mark, so
a new content-hashed format cannot be added without its attribute and an
attribute cannot be dropped without the gate saying so.

One check: for every path `git ls-files` reports whose name ends with a
suffix in HASHED_SUFFIXES, `git check-attr text` must answer `unset`. A
file the attribute file does not cover answers `unspecified`, one marked
text answers `set` or an end-of-line value; each is a finding. A tree
without git, or a root that is not a work tree, is one finding, because a
gate that cannot ask git has verified nothing.

Usage:
  python tools/check_content_attributes.py            # report, exit 1 on findings
  python tools/check_content_attributes.py --root DIR # audit an alternate work tree
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess

# Suffixes whose bytes a stamp or checksum records. Kept beside the
# .gitattributes patterns: adding a format touches both.
HASHED_SUFFIXES = (
    ".gltf",
    ".glb",
    ".bin",
    ".png",
    ".jpg",
    ".jpeg",
    ".tga",
    ".dds",
    ".ktx2",
    ".hdr",
    ".wav",
    ".ogg",
    ".mp3",
    ".ttf",
    ".mesh",
    ".hull",
    ".anim",
    ".skel",
    ".cookstamp",
    ".checksum",
    ".cookmeta",
)


class Finding:
    """One file whose bytes a checkout may rewrite, as a report line."""

    def __init__(self, location: str, message: str) -> None:
        self.location = location
        self.message = message

    def __str__(self) -> str:
        return f"  {self.location}: {self.message}"


def is_hashed(path: str) -> bool:
    """True when the path carries a content-hashed suffix."""
    lowered = path.lower()
    return any(lowered.endswith(suffix) for suffix in HASHED_SUFFIXES)


def git_output(root: pathlib.Path, arguments: list[str],
               stdin: bytes = b"") -> bytes | None:
    """Runs git in the root; None when git is missing or the call fails."""
    try:
        completed = subprocess.run(
            ["git", "-C", str(root)] + arguments, input=stdin,
            capture_output=True, check=False)
    except OSError:
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout


def tracked_hashed_files(root: pathlib.Path) -> list[str] | None:
    """Tracked paths with a hashed suffix; None when git cannot list them."""
    listing = git_output(root, ["ls-files", "-z"])
    if listing is None:
        return None
    return [path for path in listing.decode("utf-8").split("\0")
            if path and is_hashed(path)]


def text_attributes(root: pathlib.Path,
                    paths: list[str]) -> dict[str, str] | None:
    """The `text` attribute value git reports for each path."""
    if not paths:
        return {}
    stdin = "".join(path + "\0" for path in paths).encode("utf-8")
    answer = git_output(root, ["check-attr", "-z", "text", "--stdin"], stdin)
    if answer is None:
        return None
    fields = answer.decode("utf-8").split("\0")
    values: dict[str, str] = {}
    # check-attr -z prints path, attribute, value triples.
    for index in range(0, len(fields) - 2, 3):
        values[fields[index]] = fields[index + 2]
    return values


def audit(root: pathlib.Path) -> list[Finding]:
    """Every tracked hashed file whose text attribute is not unset."""
    paths = tracked_hashed_files(root)
    if paths is None:
        return [Finding(str(root), "git cannot list the tracked files "
                        "(git missing, or not a work tree)")]
    values = text_attributes(root, paths)
    if values is None:
        return [Finding(str(root), "git check-attr failed")]
    findings: list[Finding] = []
    for path in paths:
        value = values.get(path, "unspecified")
        if value != "unset":
            findings.append(Finding(
                path, f"text attribute is '{value}'; a content-hashed file "
                      "must be '-text' (or 'binary') in .gitattributes"))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", default=".",
                        help="work tree to audit (default: current directory)")
    arguments = parser.parse_args()
    root = pathlib.Path(arguments.root).resolve()
    findings = audit(root)
    if findings:
        print(f"check_content_attributes: {len(findings)} finding(s)")
        for finding in findings:
            print(finding)
        return 1
    print("check_content_attributes: every content-hashed file is pinned")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
