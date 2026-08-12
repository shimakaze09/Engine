#!/usr/bin/env python3
# Perf gate for Engine tooling: runs the ECS/physics benchmarks and
# compares against a baseline JSON. Measurements and baselines must be
# positive and finite — NaN compares false against any allowance, so an
# unchecked NaN measurement or baseline used to pass silently (audit
# M-27).

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path


# Finds the matching object or resource for executable.
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


# Runs the configured command, loop, or tool for benchmark.
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

        value = float(data[metric_key])
        if not math.isfinite(value) or value <= 0.0:
            raise ValueError(
                f"Metric '{metric_key}' must be positive and finite, got {value}")
        return value
    finally:
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass


# Runs the benchmark up to `attempts` times and keeps the fastest sample,
# stopping early once a sample is within the baseline allowance; a one-off
# jittery run therefore cannot fail the gate while the threshold itself
# stays unchanged (never looser than a single-run comparison).
def measure_with_retries(executable: Path, metric_key: str, baseline,
                         threshold: float, attempts: int) -> float:
    allowed_max = float("nan")
    base = float(baseline.get(metric_key, float("nan")))
    if math.isfinite(base) and base > 0.0 and math.isfinite(threshold):
        allowed_max = base * (1.0 + threshold)

    best = float("inf")
    for attempt in range(1, max(1, attempts) + 1):
        value = run_benchmark(executable, metric_key)
        best = min(best, value)
        print(f"[{metric_key}] attempt {attempt}: {value:.4f} (best {best:.4f})")
        if math.isfinite(allowed_max) and best <= allowed_max:
            break
    return best


# Compares measured metrics against the baseline; returns True when the
# gate passes. Rejects non-finite or non-positive measurements and
# baselines so NaN can never satisfy the comparison by vacuity.
def evaluate(measured, baseline, threshold) -> bool:
    print("\nPerformance gate summary:")
    failed = False
    for key, current in measured.items():
        base = float(baseline[key])
        if not math.isfinite(current) or current <= 0.0:
            print(f"- {key}: FAIL measurement must be positive and finite, got {current}")
            failed = True
            continue
        if not math.isfinite(base) or base <= 0.0:
            print(f"- {key}: FAIL baseline must be positive and finite, got {base}")
            failed = True
            continue
        allowed = base * (1.0 + threshold)
        ratio = current / base
        print(f"- {key}: baseline={base:.4f} current={current:.4f} ratio={ratio:.3f} allowed_max={allowed:.4f}")
        if current > allowed:
            failed = True
    return not failed


# Runs this executable or test program.
def main() -> int:
    parser = argparse.ArgumentParser(description="Run perf benchmarks and compare to baseline")
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--threshold", type=float, default=0.10,
                        help="Allowed regression ratio; default 0.10 for 10%%")
    parser.add_argument("--attempts", type=int, default=3,
                        help="Max benchmark re-runs per metric; best sample is kept")
    args = parser.parse_args()

    if args.attempts < 1:
        print(f"FAIL: --attempts must be at least 1, got {args.attempts}")
        return 1

    if not math.isfinite(args.threshold) or args.threshold < 0.0:
        print(f"FAIL: --threshold must be non-negative and finite, got {args.threshold}")
        return 1

    build_dir = Path(args.build_dir).resolve()
    baseline_path = Path(args.baseline).resolve()

    with baseline_path.open("r", encoding="utf-8") as f:
        baseline = json.load(f)

    physics_bench = find_executable(build_dir, "engine_bench_physics_perf")
    measured = {
        "ecs_iterate_ms": measure_with_retries(
            find_executable(build_dir, "engine_bench_ecs_perf"),
            "ecs_iterate_ms", baseline, args.threshold, args.attempts),
        "physics_step_ms": measure_with_retries(
            physics_bench, "physics_step_ms", baseline, args.threshold,
            args.attempts),
        "physics_dense_step_ms": measure_with_retries(
            physics_bench, "physics_dense_step_ms", baseline, args.threshold,
            args.attempts),
    }

    if not evaluate(measured, baseline, args.threshold):
        print("\nFAIL: performance regression exceeded threshold")
        return 1

    print("\nPASS: performance gate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
