#!/usr/bin/env python3
# Self-tests for the tooling quality gates (audit M-27): the coverage
# gate must reject NaN/missing/non-numeric reports and thresholds, the
# perf gate's evaluate() must reject non-finite or non-positive
# measurements and baselines, the asset metadata path audit must flag
# absolute developer paths while passing repo-relative ones (audit L-03),
# and the Lua binding generator must reject
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


def run_captured(script_args):
    """Runs a gate and returns its exit code with its combined output."""
    proc = subprocess.run([sys.executable] + script_args,
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


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


def test_metadata_path_check():
    spec = importlib.util.spec_from_file_location(
        "check_asset_metadata_paths",
        TOOLS / "ci" / "check_asset_metadata_paths.py")
    meta = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(meta)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        windows_abs = tmp / "windows.meta.json"
        windows_abs.write_text(
            '{"source":"D:\\\\dev\\\\Engine\\\\assets\\\\a.gltf"}',
            encoding="utf-8")
        unix_abs = tmp / "unix.meta.json"
        unix_abs.write_text('{"source":"/home/dev/Engine/assets/a.gltf"}',
                            encoding="utf-8")
        relative = tmp / "relative.meta.json"
        relative.write_text('{"source":"assets/props/a.gltf",'
                            '"output":"assets/props/a.mesh"}',
                            encoding="utf-8")
        scheme = tmp / "scheme.meta.json"
        scheme.write_text('{"source":"asset://props/a.gltf"}',
                          encoding="utf-8")

        check(meta.scan_file(windows_abs),
              "metadata: Windows drive path is flagged")
        check(meta.scan_file(unix_abs),
              "metadata: Unix home path is flagged")
        check(not meta.scan_file(relative),
              "metadata: repo-relative paths pass")
        check(not meta.scan_file(scheme),
              "metadata: URI scheme is not a drive letter")

    check(meta.main() == 0,
          "metadata: tracked asset metadata is free of absolute paths")


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


def write_source(root, relative, includes):
    """Plants a commented source file naming the given quoted includes."""
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    body = "// Synthetic fixture for the module dependency gate.\n"
    body += "".join(f'#include "{name}"\n' for name in includes)
    path.write_text(body, encoding="utf-8")
    return path


def test_module_dependency_gate():
    """The declared-graph gate (issue #311) must reject every edge the
    dependency rule forbids, accept the legal downward ones, and hold its
    allowlist to exactly today's tracked violations."""
    script = str(TOOLS / "check_module_deps.py")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        # A legal tree: downward edges only, including one that skips a
        # tier (editor -> renderer), plus own-module and third-party
        # includes the gate must not care about.
        clean = tmp / "clean"
        write_source(clean, "core/src/logging.cpp", ["engine/core/logging.h"])
        write_source(clean, "math/src/mat4.h", ["engine/core/entity.h"])
        write_source(clean, "content/src/store.cpp", ["engine/core/vfs.h"])
        write_source(clean, "renderer/src/flush.cpp",
                     ["engine/core/cvar.h", "engine/math/mat4.h",
                      "engine/content/asset_id.h", "pch.h"])
        write_source(clean, "renderer/src/pch.h", [])
        write_source(clean, "runtime/src/world.cpp",
                     ["engine/physics/collider.h", "engine/scripting/vm.h"])
        write_source(clean, "editor/src/panels.cpp",
                     ["engine/runtime/world.h", "engine/renderer/device.h"])
        check(run([script, "--root", str(clean)]) == 0,
              "module deps: a strictly downward tree passes")

        # Upward: the issue #309 class, a subsystem reaching into runtime.
        upward = tmp / "upward"
        write_source(upward, "scripting/src/bindings.cpp",
                     ["engine/runtime/world.h"])
        check(run([script, "--root", str(upward)]) != 0,
              "module deps: an upward subsystem -> runtime edge fails")

        # Sideways: the issue #310 class, two mid-tier siblings meeting
        # outside runtime.
        sideways = tmp / "sideways"
        write_source(sideways, "scripting/src/spawn.cpp",
                     ["engine/physics/primitive_hulls.h"])
        check(run([script, "--root", str(sideways)]) != 0,
              "module deps: a sideways subsystem -> subsystem edge fails")

        # The bottom tier has a direction too: math -> core, never back.
        reversed_bottom = tmp / "reversed_bottom"
        write_source(reversed_bottom, "core/src/logging.cpp",
                     ["engine/math/vec3.h"])
        check(run([script, "--root", str(reversed_bottom)]) != 0,
              "module deps: core -> math reverses the bottom tier and fails")

        # content is the generic asset layer and depends only on core.
        impure_content = tmp / "impure_content"
        write_source(impure_content, "content/src/store.cpp",
                     ["engine/math/vec3.h"])
        check(run([script, "--root", str(impure_content)]) != 0,
              "module deps: content -> math breaks content purity and fails")

        # A private header is not a public surface, even downward.
        private_header = tmp / "private_header"
        write_source(private_header, "runtime/src/component_registry.h", [])
        write_source(private_header, "editor/src/inspector.cpp",
                     ["component_registry.h"])
        check(run([script, "--root", str(private_header)]) != 0,
              "module deps: including another module's private header fails")

        # A relative path can climb out of the module while still
        # resolving next to the including file; the owner is decided by
        # where it lands, not by the include spelling.
        climbing = tmp / "climbing"
        write_source(climbing, "runtime/src/component_registry.h", [])
        write_source(climbing, "editor/src/inspector.cpp",
                     ["../../runtime/src/component_registry.h"])
        check(run([script, "--root", str(climbing)]) != 0,
              "module deps: a '..' path out of the module is still a crossing")

        # A same-named private header in the including module resolves
        # locally and is not a crossing.
        shadowed = tmp / "shadowed"
        write_source(shadowed, "runtime/src/pch.h", [])
        write_source(shadowed, "editor/src/pch.h", [])
        write_source(shadowed, "editor/src/panels.cpp", ["pch.h"])
        check(run([script, "--root", str(shadowed)]) == 0,
              "module deps: a module's own private header is not a crossing")

        # An angle-bracket first-party include names a module just as a
        # quoted one does, and must not bypass the direction rule.
        angled = tmp / "angled"
        write_source(angled, "scripting/src/vm.cpp",
                     ["engine/runtime/world.h"])
        (angled / "scripting" / "src" / "vm.cpp").write_text(
            "// Synthetic fixture for the module dependency gate.\n"
            "#include <engine/runtime/world.h>\n", encoding="utf-8")
        check(run([script, "--root", str(angled)]) != 0,
              "module deps: an angle-bracket first-party include is audited")

        # A hand-wired foreign include dir grants headers without
        # declaring the dependency. Each spelling below reaches the same
        # directory, so each must be judged the same way.
        def grant_case(name, lines, leaf="CMakeLists.txt"):
            case = tmp / name
            write_source(case, "editor/src/panels.cpp", [])
            target = case / "editor" / leaf
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(
                "# Synthetic fixture for the module dependency gate.\n"
                "engine_add_module_library(engine_editor\n"
                "    PRIVATE_INCLUDE_DIRS\n"
                + "".join(f"    {line}\n" for line in lines)
                + ")\n", encoding="utf-8")
            return case

        check(run([script, "--root", str(grant_case(
            "grant", ["${CMAKE_SOURCE_DIR}/runtime/include"]))]) != 0,
              "module deps: a hand-wired foreign include dir fails")
        check(run([script, "--root", str(grant_case(
            "grant_project", ["${PROJECT_SOURCE_DIR}/runtime/include"]))]) != 0,
              "module deps: the PROJECT_SOURCE_DIR spelling fails too")
        check(run([script, "--root", str(grant_case(
            "grant_relative",
            ["${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include"]))]) != 0,
              "module deps: the relative spelling fails too")
        # Nested, because a grant in a subdirectory is the same grant —
        # three lived in tools/asset_packer/CMakeLists.txt unseen.
        check(run([script, "--root", str(grant_case(
            "grant_nested", ["${CMAKE_SOURCE_DIR}/runtime/include"],
            leaf="sub/CMakeLists.txt"))]) != 0,
              "module deps: a grant in a nested CMakeLists fails")

        # Two more spellings of the same directory. CMake treats these as
        # identical to the flagged `../runtime/include`, so a verdict that
        # differs between them would be judging spelling, not location.
        check(run([script, "--root", str(grant_case(
            "grant_dot_relative", ["./../runtime/include"]))]) != 0,
              "module deps: a './..' spelling fails too")
        check(run([script, "--root", str(grant_case(
            "grant_through_subdir",
            ["sub/../../runtime/include"]))]) != 0,
              "module deps: a path through a subdirectory fails too")

        # CMake is whitespace-insensitive, so a line's verdict must not
        # depend on token order: an own-module directory listed first must
        # not excuse a foreign one after it.
        check(run([script, "--root", str(grant_case("grant_own_first", [
            "${CMAKE_SOURCE_DIR}/editor/include "
            "${CMAKE_SOURCE_DIR}/runtime/include",
        ]))]) != 0,
              "module deps: an own-module dir first does not excuse the rest "
              "of the line")

        # And every foreign grant on a line is reported, not just the
        # first — an under-reporting finding list invites the same
        # hand-enumeration error the allowlist already made once.
        code, output = run_captured([script, "--root", str(grant_case(
            "grant_two_foreign", [
                "${CMAKE_SOURCE_DIR}/runtime/include "
                "${CMAKE_SOURCE_DIR}/core/include",
            ]))])
        check(code != 0 and "runtime/include" in output
              and "core/include" in output,
              "module deps: both grants on one line are reported")

        # The other direction: a module's own include dir, in every
        # spelling, and a third-party variable, are not foreign grants.
        check(run([script, "--root", str(grant_case("grant_own", [
            "${CMAKE_CURRENT_SOURCE_DIR}/include",
            "${CMAKE_SOURCE_DIR}/editor/include",
            "${ENGINE_STB_INCLUDE_DIR}",
        ]))]) == 0,
              "module deps: a module's own and third-party dirs are not "
              "grants")
        # Same, all on one line, since the per-token filter is what makes
        # that safe now.
        check(run([script, "--root", str(grant_case("grant_own_same_line", [
            "${CMAKE_CURRENT_SOURCE_DIR}/include ./include ../editor/include",
        ]))]) == 0,
              "module deps: own-module dirs sharing a line are not grants")

    # The real tree: green today, and the allowlist is load-bearing.
    spec = importlib.util.spec_from_file_location(
        "check_module_deps", TOOLS / "check_module_deps.py")
    deps = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(deps)

    argv = sys.argv
    sys.argv = ["check_module_deps.py"]
    try:
        check(deps.main() == 0,
              "module deps: this checkout passes with its tracked allowlist")

        # Dropping an entry must turn the site it excused red, which is
        # what makes the gate red on the base revision of each #309/#310
        # fix rather than merely documenting the debt.
        excused = ("scripting/src/scripting.cpp",
                   "engine/runtime/scripting_bridge.h")
        reason = deps.KNOWN_VIOLATIONS.pop(excused)
        check(deps.main() != 0,
              "module deps: an unexcused tracked violation fails the gate")
        deps.KNOWN_VIOLATIONS[excused] = reason

        # And an entry that excuses nothing must fail too, so the list
        # can only shrink as the migration lands.
        stale = ("scripting/src/nothing_here.cpp", "engine/runtime/world.h")
        deps.KNOWN_VIOLATIONS[stale] = "stale fixture"
        check(deps.main() != 0,
              "module deps: a stale allowlist entry fails the gate")
        del deps.KNOWN_VIOLATIONS[stale]

        check(deps.main() == 0,
              "module deps: the allowlist is restored and the gate is green")
    finally:
        sys.argv = argv


def main():
    test_coverage_gate()
    test_perf_gate_evaluate()
    test_metadata_path_check()
    test_binding_generator()
    test_module_dependency_gate()
    if failures:
        print(f"\nFAILED ({len(failures)} failure(s))")
        return 1
    print("\nAll tool gate self-tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
