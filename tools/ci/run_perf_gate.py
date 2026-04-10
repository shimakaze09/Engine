#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def find_executable(build_dir: Path, name: str) -> Path:
    candidates = []
    exts = ["", ".exe"]
    for ext in exts:
        for p in build_dir.rglob(name + ext):
            if p.is_file() and os.access(p, os.X_OK):
                candidates.append(p)
    if not candidates:
        raise FileNotFoundError(f"Executable '{name}' not found under {build_dir}")
    candidates.sort(key=lambda p: len(str(p)))
    return candidates[0]


def run_benchmark(executable: Path, metric_key: str) -> float:
    with tempfile.NamedTemporaryFile(delete=False, suffix=".json") as tmp:
        tmp_path = Path(tmp.name)

    try:
        cmd = [str(executable), "--json-out", str(tmp_path)]
        print(f"Running: {' '.join(cmd)}")
        proc = subprocess.run(cmd, check=False, capture_output=True, text=True)
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        if proc.returncode != 0:
            raise RuntimeError(f"Benchmark failed ({executable.name}) with code {proc.returncode}")

        with tmp_path.open("r", encoding="utf-8") as f:
            data = json.load(f)

        if metric_key not in data:
            raise KeyError(f"Metric '{metric_key}' missing in benchmark output {tmp_path}")

        return float(data[metric_key])
    finally:
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description="Run perf benchmarks and compare to baseline")
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--threshold", type=float, default=0.10,
                        help="Allowed regression ratio; default 0.10 for 10%%")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    baseline_path = Path(args.baseline).resolve()

    with baseline_path.open("r", encoding="utf-8") as f:
        baseline = json.load(f)

    measured = {
        "ecs_iterate_ms": run_benchmark(find_executable(build_dir, "engine_bench_ecs_perf"), "ecs_iterate_ms"),
        "physics_step_ms": run_benchmark(find_executable(build_dir, "engine_bench_physics_perf"), "physics_step_ms"),
    }

    print("\nPerformance gate summary:")
    failed = False
    for key, current in measured.items():
        base = float(baseline[key])
        allowed = base * (1.0 + args.threshold)
        ratio = (current / base) if base > 0.0 else 0.0
        print(f"- {key}: baseline={base:.4f} current={current:.4f} ratio={ratio:.3f} allowed_max={allowed:.4f}")
        if current > allowed:
            failed = True

    if failed:
        print("\nFAIL: performance regression exceeded threshold")
        return 1

    print("\nPASS: performance gate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
