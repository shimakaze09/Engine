#!/usr/bin/env python3
"""Audit functional tests for wall-clock reads that could gate on timing.

CLAUDE.md's test-strictness rule: functional tests never assert wall-clock
timing or throughput; only the dedicated engine_bench_* executables hold
performance thresholds, gated against tests/benchmark/perf_baseline.json.
A functional test that compares an elapsed time against a budget fails on
a loaded runner, under a sanitizer, or in a virtual machine without any
behavior regression, and its threshold is tuned to the slowest host rather
than to the contract under test. This gate makes the rule mechanical in
the shape of the other audits: it reports findings and exits non-zero, and
CI holds it at zero.

The only way a test obtains an elapsed time is by reading a clock, so the
gate audits clock reads. Every read in a functional test source must be
classified by a marker comment, on the same line or within the two
preceding lines, naming the one purpose that is not a timing assertion:

  // wall-clock: harness-timeout
      A bounded wait for an external event (a socket, a worker thread)
      that ends the wait and fails on expiry; the elapsed value itself is
      never asserted against.

  // wall-clock: diagnostic
      A measurement that is only printed, never compared.

An unmarked read is a finding, and so is a marker with no read beside it,
so a marker cannot outlive the read it once classified. Sleeping
(`sleep_for`) is not a read and is not audited. Benchmark sources are
outside this gate; their thresholds are the perf gate's concern.

Usage:
  python tools/check_test_timing.py            # report, exit 1 on findings
  python tools/check_test_timing.py --root DIR # audit an alternate tree
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re

# Functional test roots under tests/; benchmark sources are excluded on
# purpose (their thresholds are gated by tools/ci/run_perf_gate.py).
FUNCTIONAL_TEST_DIRS = ("unit", "integration", "smoke")
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp"}

# Every clock read a test can reach: the standard chrono clocks (including
# the common `Clock` alias), the C clock, `time(nullptr)`, and SDL's tick
# counters.
CLOCK_READ_RE = re.compile(
    r"\b(?:steady_clock|high_resolution_clock|system_clock|Clock)::now\s*\("
    r"|\bstd::clock\s*\("
    r"|(?<![\w:])time\s*\(\s*(?:nullptr|NULL|0)\s*\)"
    r"|\bSDL_GetTicks(?:NS)?\s*\("
)
MARKER_RE = re.compile(r"//\s*wall-clock:\s*(harness-timeout|diagnostic)\b")
# A marker classifies a read on its own line or on the next two lines; a
# read looks back over the same window.
MARKER_WINDOW = 2


class Finding:
    """One unclassified read or orphaned marker, one report line."""

    def __init__(self, location: str, message: str) -> None:
        self.location = location
        self.message = message

    def __str__(self) -> str:
        return f"  {self.location}: {self.message}"


def functional_test_files(root: pathlib.Path) -> list[pathlib.Path]:
    """Every functional test source under tests/, benchmarks excluded."""
    found: list[pathlib.Path] = []
    for name in FUNCTIONAL_TEST_DIRS:
        directory = root / "tests" / name
        if not directory.is_dir():
            continue
        for current, subdirectories, files in os.walk(directory):
            subdirectories.sort()
            for file_name in sorted(files):
                path = pathlib.Path(current) / file_name
                if path.suffix.lower() in SOURCE_SUFFIXES:
                    found.append(path)
    return found


def read_text(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8-sig")


def strip_line_comment(line: str) -> str:
    """The code part of a line: a read mentioned inside a comment is prose."""
    return line.split("//", 1)[0]


def audit_file(path: pathlib.Path, relative: str) -> tuple[list[Finding], int]:
    """Findings for one file plus the count of classified reads."""
    lines = read_text(path).splitlines()
    read_lines = {
        number
        for number, line in enumerate(lines, 1)
        if CLOCK_READ_RE.search(strip_line_comment(line))
    }
    marker_lines = {
        number
        for number, line in enumerate(lines, 1)
        if MARKER_RE.search(line)
    }

    findings: list[Finding] = []
    classified = 0
    for number in sorted(read_lines):
        window = range(number - MARKER_WINDOW, number + 1)
        if any(candidate in marker_lines for candidate in window):
            classified += 1
        else:
            findings.append(
                Finding(
                    f"{relative}:{number}",
                    "wall-clock read in a functional test; mark it "
                    "`// wall-clock: harness-timeout` or "
                    "`// wall-clock: diagnostic`, or move the threshold to "
                    "an engine_bench_* test",
                )
            )
    for number in sorted(marker_lines):
        window = range(number, number + MARKER_WINDOW + 1)
        if not any(candidate in read_lines for candidate in window):
            findings.append(
                Finding(
                    f"{relative}:{number}",
                    "wall-clock marker with no clock read beside it; delete "
                    "the stale marker",
                )
            )
    return findings, classified


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
    classified = 0
    files = functional_test_files(root)
    for path in files:
        relative = path.relative_to(root).as_posix()
        file_findings, file_classified = audit_file(path, relative)
        findings.extend(file_findings)
        classified += file_classified

    if findings:
        print(f"test timing audit: {len(findings)} finding(s)")
        for finding in findings:
            print(finding)
        return 1

    print(
        f"test timing audit passed: {len(files)} functional test files, "
        f"{classified} classified clock read(s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
