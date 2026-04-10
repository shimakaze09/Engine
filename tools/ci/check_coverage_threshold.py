#!/usr/bin/env python3
import argparse
import json
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Check gcovr JSON summary coverage threshold")
    parser.add_argument("--summary", required=True)
    parser.add_argument("--min-line", type=float, required=True)
    args = parser.parse_args()

    summary_path = Path(args.summary)
    with summary_path.open("r", encoding="utf-8") as f:
        data = json.load(f)

    line_pct = float(data.get("line_percent", 0.0))
    print(f"Line coverage: {line_pct:.2f}% (threshold {args.min_line:.2f}%)")
    if line_pct < args.min_line:
        print("FAIL: coverage threshold not met")
        return 1

    print("PASS: coverage threshold met")
    return 0


if __name__ == "__main__":
    sys.exit(main())
