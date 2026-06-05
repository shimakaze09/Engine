#!/usr/bin/env python3
"""Run clang-tidy over engine compile commands only."""

from __future__ import annotations

import json
import pathlib
import re
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print(
            "usage: run_clang_tidy_analysis.py <compile_commands.json> "
            "<clang-tidy> <build-dir>",
            file=sys.stderr,
        )
        return 2

    compile_commands = pathlib.Path(sys.argv[1])
    clang_tidy = sys.argv[2]
    build_dir = pathlib.Path(sys.argv[3])

    if not compile_commands.is_file():
        print(f"missing compile database: {compile_commands}", file=sys.stderr)
        return 2

    database = json.loads(compile_commands.read_text(encoding="utf-8"))
    filtered_database = []
    files: list[str] = []
    for entry in database:
        file_name = str(entry.get("file", ""))
        normalized = file_name.replace("\\", "/")
        if not file_name:
            continue
        if "/build/" in normalized:
            continue
        if normalized.endswith("cmake_pch.cxx"):
            continue
        if "/_deps/" in normalized or "/tests/" in normalized:
            continue
        if "/tools/" in normalized:
            continue
        if not normalized.endswith((".cpp", ".cc", ".cxx")):
            continue
        sanitized_entry = dict(entry)
        command = str(sanitized_entry.get("command", ""))
        command = re.sub(r"\s/[YF][upI][^\s\"]*", "", command)
        command = re.sub(r"\s/Fp[^\s\"]*", "", command)
        sanitized_entry["command"] = command
        filtered_database.append(sanitized_entry)
        files.append(file_name)

    if not files:
        print("no engine source files found for clang-tidy", file=sys.stderr)
        return 2

    analysis_dir = build_dir / "analysis_compile_commands"
    analysis_dir.mkdir(parents=True, exist_ok=True)
    (analysis_dir / "compile_commands.json").write_text(
        json.dumps(filtered_database, indent=2), encoding="utf-8"
    )

    print(f"clang-tidy analysis: checking {len(files)} engine source files")
    command = [
        clang_tidy,
        "-p",
        str(analysis_dir),
        "--warnings-as-errors=*",
        *files,
    ]
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
