#!/usr/bin/env python3
# Coverage gate for Engine tooling: compares a gcovr JSON summary's
# line_percent against a required minimum. Rejects non-finite values —
# NaN compares false against any threshold, so an unchecked NaN report
# used to pass the gate silently (audit M-27).

import argparse
import json
import math
import sys
from pathlib import Path


# Rejects the non-finite JSON constants (NaN/Infinity) Python's json
# module would otherwise accept.
def _reject_constant(token: str) -> float:
    raise ValueError(f"non-finite JSON constant '{token}' in coverage summary")


# Runs this executable or test program.
def main() -> int:
    parser = argparse.ArgumentParser(description="Check gcovr JSON summary coverage threshold")
    parser.add_argument("--summary", required=True)
    parser.add_argument("--min-line", type=float, required=True)
    args = parser.parse_args()

    if not math.isfinite(args.min_line) or args.min_line <= 0.0:
        print(f"FAIL: --min-line must be positive and finite, got {args.min_line}")
        return 1

    summary_path = Path(args.summary)
    try:
        with summary_path.open("r", encoding="utf-8") as f:
            data = json.load(f, parse_constant=_reject_constant)
    except (OSError, ValueError) as error:
        print(f"FAIL: cannot read coverage summary {summary_path}: {error}")
        return 1

    try:
        line_pct = float(data.get("line_percent", "missing"))
    except (TypeError, ValueError):
        print("FAIL: coverage summary has no numeric line_percent")
        return 1
    if not math.isfinite(line_pct):
        print(f"FAIL: line_percent is not finite: {line_pct}")
        return 1

    print(f"Line coverage: {line_pct:.2f}% (threshold {args.min_line:.2f}%)")
    if line_pct < args.min_line:
        print("FAIL: coverage threshold not met")
        return 1

    print("PASS: coverage threshold met")
    return 0


if __name__ == "__main__":
    sys.exit(main())
