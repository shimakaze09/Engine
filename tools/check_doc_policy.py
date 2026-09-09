#!/usr/bin/env python3
"""Audit contributor-facing documentation that mirrors a binding rule.

CLAUDE.md is the canonical contributor contract, but the README's quick
rules are what a newcomer reads first. When a quick rule restates a
safety rule more loosely than the contract, contributors follow the loose
version: the README once told them to keep engine API functions
`noexcept` outright, while the contract permits it only when every
invoked operation is proven non-throwing, and the difference is a
`std::terminate` path under the no-exception build. This gate holds each
mirrored rule to the language that carries its condition, so a rewrite
cannot quietly drop it. It reports findings and exits non-zero, in the
shape of the other audits.

Each POLICY names a document, a pattern that locates the mirrored bullet
inside it, and the phrases that bullet must keep. A bullet is a markdown
list item plus its indented continuation lines, joined with single
spaces, so a phrase may wrap across lines. A document that is missing,
a bullet the pattern cannot find, a pattern matching more than one
bullet, and a missing phrase are each a finding.

Usage:
  python tools/check_doc_policy.py            # report, exit 1 on findings
  python tools/check_doc_policy.py --root DIR # audit an alternate tree
"""

from __future__ import annotations

import argparse
import pathlib
import re

BULLET_RE = re.compile(r"^\s*[-*]\s+")
CONTINUATION_RE = re.compile(r"^\s+\S")


class Policy:
    """One mirrored rule: where it lives and what it must keep saying."""

    def __init__(
        self, document: str, anchor: str, phrases: tuple[str, ...], rule: str
    ) -> None:
        self.document = document
        self.anchor = re.compile(anchor)
        self.phrases = phrases
        self.rule = rule


POLICIES: tuple[Policy, ...] = (
    Policy(
        document="README.md",
        anchor=r"`noexcept`",
        phrases=(
            "only when every operation it invokes is proven non-throwing",
            "a recoverable `noexcept` path must not call allocation, "
            "filesystem, or thread-creation operations that can terminate "
            "under the no-exception build",
            "`CLAUDE.md`",
        ),
        rule="the conditional noexcept rule (CLAUDE.md, Hard rules)",
    ),
)


class Finding:
    """One policy violation, rendered as a single report line."""

    def __init__(self, location: str, message: str) -> None:
        self.location = location
        self.message = message

    def __str__(self) -> str:
        return f"  {self.location}: {self.message}"


def bullets(text: str) -> list[tuple[int, str]]:
    """Every markdown list item with its continuation lines joined.

    Returns (line number of the item's first line, joined text). A
    continuation line is any indented non-blank line directly following
    the item; a blank line or an unindented line ends it.
    """
    found: list[tuple[int, str]] = []
    current: list[str] = []
    start = 0
    for number, line in enumerate(text.splitlines(), 1):
        if BULLET_RE.match(line):
            if current:
                found.append((start, " ".join(current)))
            current = [BULLET_RE.sub("", line, count=1).strip()]
            start = number
        elif current and CONTINUATION_RE.match(line):
            current.append(line.strip())
        else:
            if current:
                found.append((start, " ".join(current)))
            current = []
    if current:
        found.append((start, " ".join(current)))
    return found


def check_policy(root: pathlib.Path, policy: Policy) -> list[Finding]:
    """Findings for one mirrored rule in one document."""
    path = root / policy.document
    if not path.is_file():
        return [Finding(policy.document, f"missing; it mirrors {policy.rule}")]

    text = path.read_text(encoding="utf-8")
    matches = [
        (line, body) for line, body in bullets(text) if policy.anchor.search(body)
    ]
    if not matches:
        return [
            Finding(
                policy.document,
                f"no bullet matches /{policy.anchor.pattern}/; the document "
                f"must mirror {policy.rule}",
            )
        ]
    if len(matches) > 1:
        lines = ", ".join(str(line) for line, _ in matches)
        return [
            Finding(
                policy.document,
                f"/{policy.anchor.pattern}/ matches {len(matches)} bullets "
                f"(lines {lines}); the mirror of {policy.rule} must be one "
                "bullet",
            )
        ]

    line, body = matches[0]
    location = f"{policy.document}:{line}"
    return [
        Finding(
            location,
            f'missing "{phrase}"; the bullet must keep the language of '
            f"{policy.rule}",
        )
        for phrase in policy.phrases
        if phrase not in body
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

    findings: list[Finding] = []
    for policy in POLICIES:
        findings += check_policy(root, policy)

    if findings:
        print("documentation policy audit failed:")
        for finding in findings:
            print(str(finding))
        return 1

    print(
        "documentation policy audit passed: "
        f"{len(POLICIES)} mirrored rule(s) checked"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
