#!/usr/bin/env python3
# Self-tests for the tooling quality gates (audit M-27): the coverage
# gate must reject NaN/missing/non-numeric reports and thresholds, the
# perf gate's evaluate() must reject non-finite or non-positive
# measurements and baselines, and the Lua binding generator must reject
# duplicate Lua names and invalid or reserved parameter identifiers
# instead of emitting uncompilable or injected C++. Run from ctest as
# engine_integration_tool_gates.

import importlib.util
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")


def run(script_args):
    proc = subprocess.run([sys.executable] + script_args,
                          capture_output=True, text=True)
    return proc.returncode


def test_coverage_gate():
    cov = str(TOOLS / "ci" / "check_coverage_threshold.py")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        good = tmp / "good.json"
        good.write_text('{"line_percent": 82.5}', encoding="utf-8")
        nan_report = tmp / "nan.json"
        nan_report.write_text('{"line_percent": NaN}', encoding="utf-8")
        text_report = tmp / "text.json"
        text_report.write_text('{"line_percent": "high"}', encoding="utf-8")

        check(run([cov, "--summary", str(good), "--min-line", "50"]) == 0,
              "coverage: good report above threshold passes")
        check(run([cov, "--summary", str(good), "--min-line", "90"]) != 0,
              "coverage: report below threshold fails")
        check(run([cov, "--summary", str(nan_report), "--min-line", "50"]) != 0,
              "coverage: NaN report fails")
        check(run([cov, "--summary", str(text_report), "--min-line", "50"]) != 0,
              "coverage: non-numeric report fails")
        check(run([cov, "--summary", str(tmp / "missing.json"),
                   "--min-line", "50"]) != 0,
              "coverage: missing report fails")
        check(run([cov, "--summary", str(good), "--min-line", "nan"]) != 0,
              "coverage: NaN threshold fails")


def test_perf_gate_evaluate():
    spec = importlib.util.spec_from_file_location(
        "run_perf_gate", TOOLS / "ci" / "run_perf_gate.py")
    perf = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(perf)

    nan = float("nan")
    check(perf.evaluate({"m": 1.0}, {"m": 1.0}, 0.10),
          "perf: matching measurement passes")
    check(not perf.evaluate({"m": 1.2}, {"m": 1.0}, 0.10),
          "perf: regression beyond threshold fails")
    check(not perf.evaluate({"m": nan}, {"m": 1.0}, 0.10),
          "perf: NaN measurement fails")
    check(not perf.evaluate({"m": 1.0}, {"m": nan}, 0.10),
          "perf: NaN baseline fails")
    check(not perf.evaluate({"m": 1.0}, {"m": 0.0}, 0.10),
          "perf: zero baseline fails")
    check(not perf.evaluate({"m": 1.0}, {"m": float("inf")}, 0.10),
          "perf: infinite baseline fails")


def test_binding_generator():
    gen = str(TOOLS / "binding_generator" / "generate_bindings.py")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        good = tmp / "good.h"
        good.write_text("// LUA_BIND: ping(count: int) -> int\n"
                        "int ping(int count);\n", encoding="utf-8")
        out = tmp / "out.cpp"
        check((run([gen, str(good), "-o", str(out)]) == 0) and out.exists(),
              "bindgen: valid header generates output")

        dup = tmp / "dup.h"
        dup.write_text("// LUA_BIND: ping() -> void\nvoid ping();\n"
                       "// LUA_BIND: ping() -> void\nvoid ping_again();\n",
                       encoding="utf-8")
        check(run([gen, str(dup), "-o", str(tmp / "dup.cpp")]) != 0,
              "bindgen: duplicate lua name fails")

        bad = tmp / "bad.h"
        bad.write_text("// LUA_BIND: f(x = 0; y: int) -> void\n"
                       "void f(int x);\n", encoding="utf-8")
        check(run([gen, str(bad), "-o", str(tmp / "bad.cpp")]) != 0,
              "bindgen: non-identifier parameter name fails")

        reserved = tmp / "reserved.h"
        reserved.write_text("// LUA_BIND: g(L: int) -> void\n"
                            "void g(int value);\n", encoding="utf-8")
        check(run([gen, str(reserved), "-o", str(tmp / "reserved.cpp")]) != 0,
              "bindgen: reserved parameter name fails")


def main():
    test_coverage_gate()
    test_perf_gate_evaluate()
    test_binding_generator()
    if failures:
        print(f"\nFAILED ({len(failures)} failure(s))")
        return 1
    print("\nAll tool gate self-tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
