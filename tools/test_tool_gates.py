#!/usr/bin/env python3
# Self-tests for the tooling quality gates (audit M-27): the coverage
# gate must reject NaN/missing/non-numeric reports and thresholds, the
# perf gate's evaluate() must reject non-finite or non-positive
# measurements and baselines, the asset metadata path audit must flag
# absolute developer paths while passing repo-relative ones (audit L-03),
# the Lua binding generator must reject
# duplicate Lua names and invalid or reserved parameter identifiers
# instead of emitting uncompilable or injected C++, the test timing
# audit must hold functional tests to classified clock reads only, and
# the documentation policy audit must hold the README's mirror of the
# conditional noexcept rule to its conditional wording. Run from ctest as
# engine_integration_tool_gates.

import importlib.util
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent

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
        windows_abs = tmp / "windows.cookmeta"
        windows_abs.write_text(
            '{"source":"D:\\\\dev\\\\Engine\\\\assets\\\\a.gltf"}',
            encoding="utf-8")
        unix_abs = tmp / "unix.cookmeta"
        unix_abs.write_text('{"source":"/home/dev/Engine/assets/a.gltf"}',
                            encoding="utf-8")
        relative = tmp / "relative.cookmeta"
        relative.write_text('{"source":"assets/props/a.gltf",'
                            '"output":"assets/props/a.mesh"}',
                            encoding="utf-8")
        scheme = tmp / "scheme.cookmeta"
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
        # SDL where it belongs: the platform layer, and the one editor TU
        # that drives the ImGui SDL3 backend.
        write_source(clean, "core/src/platform.cpp", ["SDL3/SDL.h"])
        write_source(clean, "editor/src/editor.cpp",
                     ["backends/imgui_impl_sdl3.h", "SDL3/SDL.h"])
        check(run([script, "--root", str(clean)]) == 0,
              "module deps: a strictly downward tree passes")

        # SDL anywhere else is the platform layer leaking (issue #312). The
        # tracked users are excused only for this checkout, so an alternate
        # root sees the pipeline's pump as the violation it is.
        sdl_runtime = tmp / "sdl_runtime"
        write_source(sdl_runtime, "runtime/src/engine_pipeline.cpp",
                     ["SDL3/SDL.h"])
        check(run([script, "--root", str(sdl_runtime)]) != 0,
              "module deps: SDL included outside the platform layer fails")

        # The ImGui SDL3 backend header declares SDL types, so including it
        # from another editor TU is the same leak by a side door -- the one
        # seven panels used with no call into it.
        sdl_backend = tmp / "sdl_backend"
        write_source(sdl_backend, "editor/src/editor_panels_main.cpp",
                     ["backends/imgui_impl_sdl3.h"])
        check(run([script, "--root", str(sdl_backend)]) != 0,
              "module deps: the ImGui SDL3 backend outside editor.cpp fails")

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

        # Public-header dependency visibility (check 4). A module whose
        # public headers include another module's headers must declare
        # that dep PUBLIC; PRIVATE leaves consumers compiling through
        # somebody else's transitive usage requirements, which is how
        # engine_physics reached core through engine_math.
        def visibility_case(name, deps_lines, header_includes=None,
                            source_includes=None, body=None):
            case = tmp / name
            write_source(case, "physics/include/engine/physics/context.h",
                         header_includes
                         if header_includes is not None
                         else ["engine/core/entity.h"])
            if source_includes is not None:
                write_source(case, "physics/src/collider.cpp",
                             source_includes)
            lists_file = case / "physics" / "CMakeLists.txt"
            lists_file.parent.mkdir(parents=True, exist_ok=True)
            lists_file.write_text(
                "# Synthetic fixture for the module dependency gate.\n"
                + (body if body is not None else
                   "engine_add_module_library(engine_physics\n"
                   "    SOURCES\n"
                   "    src/collider.cpp\n"
                   "    PUBLIC_INCLUDE_DIRS\n"
                   "    ${CMAKE_CURRENT_SOURCE_DIR}/include\n"
                   + "".join(f"    {line}\n" for line in deps_lines)
                   + ")\n"),
                encoding="utf-8")
            return case

        check(run([script, "--root", str(visibility_case(
            "visibility_private", ["PRIVATE_DEPS", "engine_core"]))]) != 0,
              "module deps: a public header's dep declared PRIVATE fails")
        check(run([script, "--root", str(visibility_case(
            "visibility_absent", []))]) != 0,
              "module deps: a public header's undeclared dep fails")
        check(run([script, "--root", str(visibility_case(
            "visibility_public", ["PUBLIC_DEPS", "engine_core"]))]) == 0,
              "module deps: a public header's dep declared PUBLIC passes")
        # PUBLIC_DEPS is a section, so the entries after it all count —
        # a per-line reading would see only the keyword's own line.
        check(run([script, "--root", str(visibility_case(
            "visibility_public_section",
            ["PUBLIC_DEPS", "engine_math", "engine_core",
             "PRIVATE_DEPS", "engine_lua"]))]) == 0,
              "module deps: every entry in the PUBLIC_DEPS section counts")
        # PRIVATE is right when only the module's own sources include it.
        check(run([script, "--root", str(visibility_case(
            "visibility_private_source_only",
            ["PRIVATE_DEPS", "engine_core"],
            header_includes=[],
            source_includes=["engine/core/entity.h"]))]) == 0,
              "module deps: a dep only src/ includes may stay PRIVATE")
        # The raw spelling of the same property.
        check(run([script, "--root", str(visibility_case(
            "visibility_raw_link", [], body=(
                "engine_add_module_library(engine_physics\n"
                "    SOURCES src/collider.cpp\n"
                ")\n"
                "target_link_libraries(engine_physics PUBLIC engine_core)\n"
            )))]) == 0,
              "module deps: target_link_libraries PUBLIC declares it too")
        check(run([script, "--root", str(visibility_case(
            "visibility_raw_link_private", [], body=(
                "engine_add_module_library(engine_physics\n"
                "    SOURCES src/collider.cpp\n"
                ")\n"
                "target_link_libraries(engine_physics PRIVATE engine_core)\n"
            )))]) != 0,
              "module deps: target_link_libraries PRIVATE does not")
        # INTERFACE propagates usage requirements to consumers exactly as
        # PUBLIC does, and on a header-only target it is the ONLY spelling
        # CMake accepts — flagging it would name a remedy CMake rejects.
        header_only = tmp / "visibility_interface"
        write_source(header_only, "math/include/engine/math/vec.h",
                     ["engine/core/entity.h"])
        lists_file = header_only / "math" / "CMakeLists.txt"
        lists_file.parent.mkdir(parents=True, exist_ok=True)
        lists_file.write_text(
            "# Synthetic fixture for the module dependency gate.\n"
            "add_library(engine_math INTERFACE)\n"
            "target_link_libraries(engine_math INTERFACE engine_core)\n",
            encoding="utf-8")
        check(run([script, "--root", str(header_only)]) == 0,
              "module deps: an INTERFACE target's dep is declared publicly")
        # A commented-out declaration declares nothing, and prose naming
        # a target must not be read as a declaration either.
        check(run([script, "--root", str(visibility_case(
            "visibility_commented", [], body=(
                "engine_add_module_library(engine_physics\n"
                "    SOURCES src/collider.cpp\n"
                "    # PUBLIC_DEPS engine_core\n"
                ")\n"
            )))]) != 0,
              "module deps: a commented-out PUBLIC_DEPS declares nothing")
        # Another target's deps in the same file say nothing about the
        # module's own library.
        check(run([script, "--root", str(visibility_case(
            "visibility_other_target", [], body=(
                "engine_add_module_library(engine_physics\n"
                "    SOURCES src/collider.cpp\n"
                ")\n"
                "engine_add_module_library(engine_physics_tool\n"
                "    SOURCES src/tool.cpp\n"
                "    PUBLIC_DEPS engine_core\n"
                ")\n"
            )))]) != 0,
              "module deps: another target's PUBLIC_DEPS does not count")
        # An edge the graph forbids outright belongs to check 1, which
        # says to delete it — not to check 4, which would say to declare
        # it. One finding, not two contradictory ones.
        forbidden = tmp / "visibility_forbidden"
        write_source(forbidden, "scripting/include/engine/scripting/vm.h",
                     ["engine/runtime/world.h"])
        code, output = run_captured([script, "--root", str(forbidden)])
        check(code != 0 and "PUBLIC dep" not in output,
              "module deps: a forbidden edge is reported as direction only")

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
        excused = ("editor/src/editor_component_registry.h",
                   "component_registry.h")
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

    # ENGINE_HELPER_KEYWORDS mirrors the library helpers' signatures, and
    # drift fails OPEN: tokens_in_section treats an unknown keyword as a
    # section value, so a keyword added to a helper and written after
    # PUBLIC_DEPS would silently join the declared-PUBLIC set and hide a
    # real under-declaration. Assert the mirror against the source.
    helpers = (TOOLS.parent / "cmake" / "EngineHelpers.cmake").read_text(
        encoding="utf-8")
    declared_keywords = set()
    for helper in ("engine_add_module_library", "engine_add_header_library"):
        body = helpers.split(f"function({helper} target)", 1)[1]
        body = body.split("endfunction()", 1)[0]
        for kind in ("oneValueArgs", "multiValueArgs"):
            match = re.search(rf"set\({kind}([^)]*)\)", body)
            if match is not None:
                declared_keywords |= set(match.group(1).split())
    check(declared_keywords == set(deps.ENGINE_HELPER_KEYWORDS),
          "module deps: ENGINE_HELPER_KEYWORDS matches the helper signatures "
          f"(helpers declare {sorted(declared_keywords)}, gate knows "
          f"{sorted(deps.ENGINE_HELPER_KEYWORDS)})")


def write_pin_fixture(root, cmake_body=None, workflow_body=None):
    """Plants a minimal tree with one CMake listfile and one workflow."""
    root.mkdir(parents=True, exist_ok=True)
    if cmake_body is not None:
        (root / "CMakeLists.txt").write_text(
            "# Synthetic fixture for the dependency pin gate.\n" + cmake_body,
            encoding="utf-8")
    if workflow_body is not None:
        workflows = root / ".github" / "workflows"
        workflows.mkdir(parents=True, exist_ok=True)
        (workflows / "ci.yml").write_text(
            "# Synthetic fixture for the dependency pin gate.\n"
            + workflow_body, encoding="utf-8")
    return root


PIN_SHA = "0123456789abcdef0123456789abcdef01234567"


def test_dependency_pin_gate():
    """The pin gate (issue #352) must accept only content-addressed
    FetchContent declarations and SHA-pinned action references, ignore
    what it should not audit, and hold its allowlist to exactly today's
    mutable action tags."""
    script = str(TOOLS / "check_dependency_pins.py")

    def declare(tag_line):
        return ("include(FetchContent)\n"
                "FetchContent_Declare(\n"
                "    dep\n"
                "    GIT_REPOSITORY https://example.invalid/dep.git\n"
                f"{tag_line}"
                ")\n")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        check(run([script, "--root", str(write_pin_fixture(
            tmp / "clean", declare(f"    GIT_TAG {PIN_SHA} # v1.2\n"),
            f"    steps:\n      - uses: actions/checkout@{PIN_SHA} # v6\n"
        ))]) == 0,
              "pins: a SHA-pinned tree passes")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "tag", declare("    GIT_TAG v1.2\n")))]) != 0,
              "pins: a GIT_TAG naming a tag fails")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "variable", declare("    GIT_TAG ${DEP_TAG}\n")))]) != 0,
              "pins: a GIT_TAG naming a variable fails")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "missing", declare("")))]) != 0,
              "pins: a git declaration with no GIT_TAG fails")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "commented", declare(f"    # GIT_TAG {PIN_SHA}\n")))]) != 0,
              "pins: a commented-out GIT_TAG is not a pin")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "quoted", declare(f'    GIT_TAG "{PIN_SHA}"\n')))]) == 0,
              "pins: a quoted SHA is still a SHA")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "url_unhashed",
            "FetchContent_Declare(dep URL https://example.invalid/d.zip)\n"
        ))]) != 0,
              "pins: a URL download without URL_HASH fails")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "url_hashed",
            "FetchContent_Declare(dep URL https://example.invalid/d.zip\n"
            f"    URL_HASH SHA256={PIN_SHA}{PIN_SHA[:24]})\n"))]) == 0,
              "pins: a URL download with a literal SHA256 URL_HASH passes")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "url_sha1",
            "FetchContent_Declare(dep URL https://example.invalid/d.zip\n"
            f"    URL_HASH SHA1={PIN_SHA})\n"))]) == 0,
              "pins: a literal SHA1 URL_HASH of SHA1's length passes")
        # A hash the build can re-key from outside the commit is no pin:
        # a variable, a generator expression, a digest of the wrong
        # length, or an algorithm CMake does not know.
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "url_variable",
            "FetchContent_Declare(dep URL https://example.invalid/d.zip\n"
            "    URL_HASH ${DEP_HASH})\n"))]) != 0,
              "pins: a URL_HASH naming a variable fails")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "url_genex",
            "FetchContent_Declare(dep URL https://example.invalid/d.zip\n"
            "    URL_HASH SHA256=$<TARGET_PROPERTY:dep,HASH>)\n"))]) != 0,
              "pins: a URL_HASH carrying a generator expression fails")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "url_short",
            "FetchContent_Declare(dep URL https://example.invalid/d.zip\n"
            f"    URL_HASH SHA256={PIN_SHA})\n"))]) != 0,
              "pins: a SHA256 URL_HASH with a SHA1-length digest fails")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "url_algorithm",
            "FetchContent_Declare(dep URL https://example.invalid/d.zip\n"
            f"    URL_HASH CRC32={PIN_SHA[:8]})\n"))]) != 0,
              "pins: a URL_HASH algorithm CMake does not accept fails")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "source_dir",
            "FetchContent_Declare(dep SOURCE_DIR ${CMAKE_SOURCE_DIR}/x)\n"
        ))]) == 0,
              "pins: a declaration that downloads nothing is not audited")

        # A build tree's fetched CMake files are not this repository's
        # declarations, whatever they pin.
        build_tree = write_pin_fixture(tmp / "build_tree",
                                       declare(f"    GIT_TAG {PIN_SHA}\n"))
        write_pin_fixture(build_tree / "build" / "_deps" / "x-src",
                          declare("    GIT_TAG main\n"))
        check(run([script, "--root", str(build_tree)]) == 0,
              "pins: fetched dependencies under build/ are skipped")

        # Actions: the mutable tag is the finding; a local action and a
        # commented-out step are not references the runner resolves.
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "action_tag", workflow_body=(
                "    steps:\n      - uses: actions/checkout@v6\n")))]) != 0,
              "pins: an action referenced by tag fails outside the allowlist")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "action_none", workflow_body=(
                "    steps:\n      - uses: actions/checkout\n")))]) != 0,
              "pins: an action with no version reference fails")
        check(run([script, "--root", str(write_pin_fixture(
            tmp / "action_ok", workflow_body=(
                "    steps:\n"
                "      - uses: ./.github/actions/local\n"
                "      # - uses: actions/checkout@v6\n"
                f"      - uses: 'actions/checkout@{PIN_SHA}'\n"
                "        with:\n          fetch-depth: 0\n")))]) == 0,
              "pins: local, commented-out, and SHA-pinned actions pass")

        # Composite actions: a workflow that reaches a remote action
        # through a local composite manifest runs it with the same
        # privileges, so the manifest is audited like a workflow, however
        # deep under .github/actions/ it sits.
        def write_composite(root, reference):
            write_pin_fixture(root, workflow_body=(
                "    steps:\n      - uses: ./.github/actions/nested/setup\n"))
            action = root / ".github" / "actions" / "nested" / "setup"
            action.mkdir(parents=True, exist_ok=True)
            (action / "action.yml").write_text(
                "name: setup\nruns:\n  using: composite\n  steps:\n"
                f"    - uses: {reference}\n", encoding="utf-8")
            return root

        check(run([script, "--root", str(write_composite(
            tmp / "composite_tag", "actions/checkout@v6"))]) != 0,
              "pins: a mutable tag inside a composite action fails")
        check(run([script, "--root", str(write_composite(
            tmp / "composite_sha", f"actions/checkout@{PIN_SHA} # v6"))]) == 0,
              "pins: a SHA-pinned composite action step passes")

    # The real tree: green today, and the allowlist is load-bearing in
    # both directions: it is empty (every reference is pinned), so a
    # mutable reference fails unless an entry excuses it, and an entry
    # that excuses nothing fails.
    spec = importlib.util.spec_from_file_location(
        "check_dependency_pins", TOOLS / "check_dependency_pins.py")
    pins = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pins)

    argv = sys.argv
    sys.argv = ["check_dependency_pins.py"]
    try:
        check(pins.main() == 0,
              "pins: this checkout passes the gate")
        check(not pins.KNOWN_UNPINNED_ACTIONS,
              "pins: this checkout has no allowlisted mutable action tags")

        stale = "example/nothing@v0"
        pins.KNOWN_UNPINNED_ACTIONS[stale] = "stale fixture"
        check(pins.main() != 0,
              "pins: a stale allowlist entry fails the gate")
        del pins.KNOWN_UNPINNED_ACTIONS[stale]

        check(pins.main() == 0,
              "pins: the allowlist is restored and the gate is green")
    finally:
        sys.argv = argv


def write_attribute_fixture(root, attributes, tracked):
    """A throwaway git work tree with the given .gitattributes text and
    the given tracked files staged (no commit or identity needed)."""
    root.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "-C", str(root), "init", "-q"], check=True,
                   capture_output=True)
    if attributes is not None:
        (root / ".gitattributes").write_text(attributes, encoding="utf-8")
    for relative in tracked:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"x\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True,
                   capture_output=True)
    return root


def write_identity_fixture(root, files, untracked=None):
    """A throwaway git work tree carrying a copy of the real asset type
    table (the gate reads its suffixes from there) plus the given staged
    files, each a {relative path: contents} pair. `untracked` files are
    written after staging, so they are present on disk and not tracked —
    the shape a .gitignore rule produces."""
    root.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "-C", str(root), "init", "-q"], check=True,
                   capture_output=True)
    table = REPO / "content" / "include" / "engine" / "content" / \
        "asset_type_table.h"
    destination = root / "content" / "include" / "engine" / "content"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "asset_type_table.h").write_text(
        table.read_text(encoding="utf-8"), encoding="utf-8")
    for relative, contents in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True,
                   capture_output=True)
    for relative, contents in (untracked or {}).items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
    return root


def sidecar_text(guid):
    """A minimal valid sidecar document claiming `guid`."""
    return '{"schemaVersion": 1, "guid": "%s"}\n' % guid


def cook_stamp_text(guid, outputs):
    """A cook stamp claiming `outputs`, each path relative to the stamp,
    in the ASSET plus OUTPUT shape the packer writes."""
    lines = ["SCHEMA 5", "TOOL_VERSION 4", "SOURCE_HASH 0123456789abcdef",
             "SOURCE_GUID %s" % guid]
    for index, relative in enumerate(outputs):
        lines.append("OUTPUT %016x %s" % (index + 1, relative))
        lines.append("ASSET %016x %s" % (index + 1, relative))
    return "\n".join(lines) + "\n"


def test_asset_identity_gate():
    """The identity gate must fail an identity-bearing asset with no
    committed sidecar, two sidecars claiming one GUID (naming every
    colliding path and picking no winner), two tracked paths that differ
    only by case, and a tracked cooked output no tracked cook stamp claims
    (issue #631); and pass on this checkout."""
    script = str(TOOLS / "check_asset_identity.py")
    one = "11111111-1111-4111-8111-111111111111"
    two = "22222222-2222-4222-8222-222222222222"

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        check(run([script, "--root", str(write_identity_fixture(
            tmp / "clean", {
                "assets/props/coin.gltf": "x\n",
                "assets/props/coin.gltf.meta": sidecar_text(one),
                "assets/scripts/hop.lua": "x\n",
                "assets/scripts/hop.lua.meta": sidecar_text(two),
                # A cooked output owns no identity of its own, so it needs
                # no sidecar and must not be reported as missing one — but
                # it does need the stamp that says whose output it is.
                "assets/props/coin.mesh": "x\n",
                "assets/props/coin.mesh.cookstamp":
                    cook_stamp_text(one, ["coin.mesh"]),
            }))]) == 0,
              "identity: every source with a committed sidecar passes")

        # Issue #631: the stamp is on the machine that cooked it and in no
        # clone, so the output's identity is unnameable everywhere else.
        orphan = write_identity_fixture(
            tmp / "orphan", {
                "assets/props/coin.gltf": "x\n",
                "assets/props/coin.gltf.meta": sidecar_text(one),
                "assets/props/coin.mesh": "x\n",
            },
            untracked={
                "assets/props/coin.mesh.cookstamp":
                    cook_stamp_text(one, ["coin.mesh"]),
            })
        completed = subprocess.run(
            [sys.executable, script, "--root", str(orphan)],
            capture_output=True, text=True)
        check(completed.returncode != 0,
              "identity: a cooked output whose stamp is untracked fails")
        check("coin.mesh.cookstamp" in completed.stdout,
              "identity: the orphan finding names the untracked stamp")

        check(run([script, "--root", str(write_identity_fixture(
            tmp / "nostamp", {
                "assets/props/coin.gltf": "x\n",
                "assets/props/coin.gltf.meta": sidecar_text(one),
                "assets/props/coin.mesh": "x\n",
            }))]) != 0,
              "identity: a cooked output with no stamp at all fails")

        # A stamp's claims resolve against the stamp's own directory, so a
        # like-named claim one directory over must not cover this output.
        check(run([script, "--root", str(write_identity_fixture(
            tmp / "elsewhere", {
                "assets/props/coin.gltf": "x\n",
                "assets/props/coin.gltf.meta": sidecar_text(one),
                "assets/props/coin.mesh": "x\n",
                "assets/other/coin.mesh.cookstamp":
                    cook_stamp_text(one, ["coin.mesh"]),
            }))]) != 0,
              "identity: a stamp in another directory claims nothing here")

        # A stamp claims its siblings too, so one stamp covers the whole
        # kit a single source cooked into.
        check(run([script, "--root", str(write_identity_fixture(
            tmp / "siblings", {
                "assets/hero.gltf": "x\n",
                "assets/hero.gltf.meta": sidecar_text(one),
                "assets/hero.mesh": "x\n",
                "assets/hero.skel": "x\n",
                "assets/hero.idle.anim": "x\n",
                "assets/hero.mesh.cookstamp": cook_stamp_text(
                    one, ["hero.mesh", "hero.skel", "hero.idle.anim"]),
            }))]) == 0,
              "identity: one stamp claiming its whole cooked kit passes")

        check(run([script, "--root", str(write_identity_fixture(
            tmp / "missing", {
                "assets/props/coin.gltf": "x\n",
            }))]) != 0,
              "identity: a source with no sidecar fails")

        duplicate = write_identity_fixture(tmp / "duplicate", {
            "assets/props/coin.gltf": "x\n",
            "assets/props/coin.gltf.meta": sidecar_text(one),
            "assets/props/gem.gltf": "x\n",
            "assets/props/gem.gltf.meta": sidecar_text(one),
        })
        completed = subprocess.run(
            [sys.executable, script, "--root", str(duplicate)],
            capture_output=True, text=True)
        check(completed.returncode != 0,
              "identity: two sidecars claiming one GUID fail")
        check(("coin.gltf.meta" in completed.stdout) and
              ("gem.gltf.meta" in completed.stdout),
              "identity: a duplicate names every colliding path")

        collision = write_identity_fixture(tmp / "case", {
            "assets/props/coin.gltf": "x\n",
            "assets/props/coin.gltf.meta": sidecar_text(one),
            "assets/props/Coin.gltf": "x\n",
            "assets/props/Coin.gltf.meta": sidecar_text(two),
        })
        # A case-insensitive filesystem cannot hold the pair, so the case
        # is only meaningful where it can; skipping beats a false pass.
        if (collision / "assets" / "props" / "Coin.gltf").exists() and \
                (collision / "assets" / "props" / "coin.gltf").read_text(
                    encoding="utf-8") == "x\n":
            tracked = subprocess.run(
                ["git", "-C", str(collision), "ls-files"],
                capture_output=True, text=True, check=True).stdout
            if ("assets/props/Coin.gltf" in tracked) and \
                    ("assets/props/coin.gltf" in tracked):
                check(run([script, "--root", str(collision)]) != 0,
                      "identity: paths differing only by case fail")

        check(run([script]) == 0, "identity: this checkout passes")


def write_variant_fixture(root, models, rows, variants):
    """A tree with just the three files the shader-variant gate reads: the
    ShadingModel enum, the engine's variant table, and the cook manifest."""
    header = root / "renderer" / "include" / "engine" / "renderer"
    header.mkdir(parents=True, exist_ok=True)
    enumerators = ", ".join("%s = %dU" % (name, index)
                            for index, name in enumerate(models))
    (header / "material.h").write_text(
        "enum class ShadingModel : std::uint8_t { %s };\n" % enumerators,
        encoding="utf-8")

    source = root / "renderer" / "src"
    source.mkdir(parents=True, exist_ok=True)
    table = ",\n".join(
        '        {ShadingModel::%s, "%s", "%s"}' % (model, define,
                                                    model.lower())
        for model, define in rows)
    (source / "command_buffer_init_core.cpp").write_text(
        "    const ModelVariant kModelVariants[] = {\n%s};\n" % table,
        encoding="utf-8")

    manifest = root / "assets" / "shaders" / "bgfx"
    manifest.mkdir(parents=True, exist_ok=True)
    (manifest / "shaders.manifest").write_text(
        json.dumps({"shaders": [
            {"source": "pbr.fs.sc", "type": "fragment",
             "output": "pbr.frag", "variants": variants}]}),
        encoding="utf-8")
    return root


def test_shader_variant_gate():
    """The shader-variant gate must fail when the engine can request a
    define set the manifest does not cook, and when a shading model has no
    row in the variant table at all -- both of which fall back to a stage's
    default binary in silence (#635/#615). It must pass on this checkout."""
    script = str(TOOLS / "check_shader_variants.py")
    every = [[], ["PBR_FULL"],
             ["ENGINE_SHADING_TOON"], ["ENGINE_SHADING_TOON", "PBR_FULL"],
             ["ENGINE_SHADING_UNLIT"], ["ENGINE_SHADING_UNLIT", "PBR_FULL"]]
    rows = [("Toon", "ENGINE_SHADING_TOON"),
            ("Unlit", "ENGINE_SHADING_UNLIT")]

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        check(run([script, "--root", str(write_variant_fixture(
            tmp / "clean", ["Pbr", "Toon", "Unlit"], rows, every))]) == 0,
              "variants: every requestable set cooked passes")

        # PBR_FULL is chosen at runtime by the sampler budget, so cooking
        # only one form of a model's set still leaves a silent fallback.
        check(run([script, "--root", str(write_variant_fixture(
            tmp / "half", ["Pbr", "Toon", "Unlit"], rows,
            [v for v in every if v != ["ENGINE_SHADING_TOON", "PBR_FULL"]]
            ))]) != 0,
              "variants: a model cooked without its PBR_FULL form fails")

        missing = write_variant_fixture(
            tmp / "missing", ["Pbr", "Toon", "Unlit"], rows,
            [v for v in every if "ENGINE_SHADING_UNLIT" not in v])
        completed = subprocess.run(
            [sys.executable, script, "--root", str(missing)],
            capture_output=True, text=True)
        check(completed.returncode != 0,
              "variants: an uncooked model define fails")
        check("ENGINE_SHADING_UNLIT" in completed.stdout,
              "variants: the finding names the uncooked define")

        # A model in the enum with no variant-table row loads no program of
        # its own, which is the same silent fallback one layer earlier.
        untabled = write_variant_fixture(
            tmp / "untabled", ["Pbr", "Toon", "Unlit", "Sketch"], rows,
            every)
        completed = subprocess.run(
            [sys.executable, script, "--root", str(untabled)],
            capture_output=True, text=True)
        check(completed.returncode != 0,
              "variants: a model with no variant-table row fails")
        check("Sketch" in completed.stdout,
              "variants: the finding names the untabled model")

        # The default model is what everything else falls back to, so it
        # needs no define and no row.
        check(run([script, "--root", str(write_variant_fixture(
            tmp / "default_only", ["Pbr"], rows, every))]) == 0,
              "variants: the default model needs no define of its own")

    check(run([script]) == 0, "variants: this checkout passes")


def test_content_attributes_gate():
    """The attributes gate (issue #590) must hold every tracked
    content-hashed file to `text` unset, accept both `-text` and the
    `binary` macro, ignore untracked and unhashed files, fail when git
    cannot answer, and pass on this checkout."""
    script = str(TOOLS / "check_content_attributes.py")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        check(run([script, "--root", str(write_attribute_fixture(
            tmp / "clean", "*.gltf -text\n*.mesh binary\n*.cookmeta -text\n",
            ["props/coin.gltf", "props/coin.mesh",
             "props/coin.mesh.cookmeta", "scene.json"]))]) == 0,
              "attributes: -text and binary marks on every hashed file pass")

        check(run([script, "--root", str(write_attribute_fixture(
            tmp / "missing", "*.mesh binary\n",
            ["props/coin.gltf", "props/coin.mesh"]))]) != 0,
              "attributes: a hashed suffix with no attribute fails")

        check(run([script, "--root", str(write_attribute_fixture(
            tmp / "no_file", None, ["props/coin.gltf"]))]) != 0,
              "attributes: a tree without .gitattributes fails")

        check(run([script, "--root", str(write_attribute_fixture(
            tmp / "marked_text", "*.gltf text\n", ["props/coin.gltf"]))]) != 0,
              "attributes: a hashed file marked text fails")

        check(run([script, "--root", str(write_attribute_fixture(
            tmp / "sidecar", "*.gltf -text\n*.json -text\n*.cookmeta text\n",
            ["props/coin.gltf", "props/coin.mesh.cookmeta"]))]) != 0,
              "attributes: a later text mark on a sidecar suffix fails")

        untracked = write_attribute_fixture(
            tmp / "untracked", "*.gltf -text\n", ["props/coin.gltf"])
        (untracked / "stray.mesh").write_bytes(b"x\n")
        check(run([script, "--root", str(untracked)]) == 0,
              "attributes: an untracked hashed file is not audited")

        plain = tmp / "plain"
        plain.mkdir()
        check(run([script, "--root", str(plain)]) != 0,
              "attributes: a directory that is not a work tree fails")

    check(run([script, "--root", str(TOOLS.parent)]) == 0,
          "attributes: this checkout passes the gate")


def write_timing_fixture(root, relative, body):
    """Plants one test source at `relative` under a synthetic tree root."""
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("// Synthetic fixture for the test timing gate.\n" + body,
                    encoding="utf-8")
    return root


def test_test_timing_gate():
    """The timing gate (issue #354) must reject an unclassified clock read
    in a functional test, accept a read classified by a marker on its own
    line or within the two lines above it, reject a marker with no read
    beside it, ignore benchmark sources and sleeps, and pass this
    checkout."""
    script = str(TOOLS / "check_test_timing.py")
    read = "  const auto t = std::chrono::steady_clock::now();\n"

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        check(run([script, "--root", str(write_timing_fixture(
            tmp / "unmarked", "tests/unit/a_test.cpp", read))]) != 0,
              "timing: an unclassified clock read in a unit test fails")
        check(run([script, "--root", str(write_timing_fixture(
            tmp / "alias", "tests/integration/a_test.cpp",
            "using Clock = std::chrono::high_resolution_clock;\n"
            "  const auto t = Clock::now();\n"))]) != 0,
              "timing: a read through a clock alias fails")
        check(run([script, "--root", str(write_timing_fixture(
            tmp / "same_line", "tests/unit/a_test.cpp",
            "  const auto t = std::chrono::steady_clock::now();"
            " // wall-clock: diagnostic\n"))]) == 0,
              "timing: a same-line marker classifies the read")
        check(run([script, "--root", str(write_timing_fixture(
            tmp / "above", "tests/unit/a_test.cpp",
            "  // wall-clock: harness-timeout\n"
            "  auto deadline =\n"
            "      std::chrono::steady_clock::now() + timeout;\n"))]) == 0,
              "timing: a marker two lines above classifies the read")
        check(run([script, "--root", str(write_timing_fixture(
            tmp / "too_far", "tests/unit/a_test.cpp",
            "  // wall-clock: harness-timeout\n"
            "  int a = 0;\n"
            "  int b = 0;\n"
            + read))]) != 0,
              "timing: a marker outside the two-line window classifies "
              "nothing (both the read and the marker are findings)")
        check(run([script, "--root", str(write_timing_fixture(
            tmp / "stale", "tests/unit/a_test.cpp",
            "  // wall-clock: diagnostic\n  int a = 0;\n"))]) != 0,
              "timing: a marker with no read beside it fails")
        check(run([script, "--root", str(write_timing_fixture(
            tmp / "benchmark", "tests/benchmark/a_test.cpp", read))]) == 0,
              "timing: benchmark sources are outside the gate")
        check(run([script, "--root", str(write_timing_fixture(
            tmp / "sleep", "tests/unit/a_test.cpp",
            "  std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
            "  // steady_clock::now() mentioned in prose is not a read\n"
        ))]) == 0,
              "timing: sleeping and prose mentions are not reads")
        check(run([script, "--root", str(tmp / "empty")]) == 0,
              "timing: a tree with no functional tests passes")

    check(run([script]) == 0,
          "timing: this checkout passes the gate")


def write_comment_fixture(root, rel, body):
    """Plants one source file under a fixture tree and returns the root."""
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")
    return root


def test_comment_quality_gate():
    """The comment gate must flag every objective class — filler, doc
    comments attached to the wrong line, commented-out code, untracked
    TODOs, issue numbers outside a tracked marker — accept prose and
    tracked markers, keep a documented data format out of the
    commented-out-code class, and pass this checkout with no allowlist."""
    script = str(TOOLS / "check_comment_quality.py")
    header = "// Purpose comment.\n"

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        def case(name, rel, body):
            return str(write_comment_fixture(tmp / name, rel, header + body))

        check(run([script, "--root", case(
            "clean", "core/src/a.cpp",
            "// The registry exists for the process lifetime.\nint a;\n")]) == 0,
              "comments: a standard-conforming comment passes")
        check(run([script, "--root", case(
            "todo_marker", "core/src/a.cpp",
            "// TODO(#184): Remove the compatibility path.\nint a;\n")]) == 0,
              "comments: TODO(#n) may cite its issue")
        check(run([script, "--root", case(
            "fixme_marker", "core/src/a.cpp",
            "// FIXME(#231): Overflows past 32767 cells.\nint a;\n")]) == 0,
              "comments: FIXME(#n) may cite its issue")
        check(run([script, "--root", case(
            "vague", "core/src/a.cpp", "// TODO: fix this.\nint a;\n")]) != 0,
              "comments: a TODO without an issue is a finding")
        check(run([script, "--root", case(
            "code", "core/src/a.cpp",
            "// oldRenderer.Draw(mesh);\nint a;\n")]) != 0,
              "comments: a commented-out call statement is a finding")
        check(run([script, "--root", case(
            "block_code", "core/src/a.cpp",
            "/* legacy notes */\n// if (x) {\nint a;\n")]) != 0,
              "comments: commented-out control flow is a finding")
        check(run([script, "--root", case(
            "prose_call", "core/src/a.cpp",
            "// Calls reset() once per frame so the pool never grows.\n"
            "int a;\n")]) == 0,
              "comments: prose mentioning a call is not code")
        # A documented JSON or data format closes its braces in prose, so a
        # lone closing brace is not evidence that code was commented out.
        check(run([script, "--root", case(
            "schema", "core/src/a.h",
            "// Schema:\n//   {\n//     \"version\": 1\n//   }\nint a;\n")]) == 0,
              "comments: a documented data format is not commented-out code")
        check(run([script, "--root", case(
            "filler", "core/src/a.h",
            "/// Handles frobnication.\nvoid frobnicate();\n")]) != 0,
              "comments: a tautology template is a finding")
        check(run([script, "--root", case(
            "misplaced", "core/src/a.h",
            "class A {\n /// Owns nothing.\n public:\n  int x;\n};\n")]) != 0,
              "comments: a doc comment above an access specifier is a finding")
        check(run([script, "--root", str(write_comment_fixture(
            tmp / "cmake_code", "CMakeLists.txt",
            "# Build options.\n# set(ENGINE_OLD ON)\nproject(x)\n"))]) != 0,
              "comments: a commented-out CMake command is a finding")
        # Comment prose — dates, loose wording, history — is authoring-time
        # guidance in the `comment` skill, not a gate. A shared per-file
        # allowlist for it serialized every concurrent change on one file
        # (docs/decisions/0006, 0009).
        check(run([script, "--root", case(
            "prose", "core/src/a.cpp",
            "// Rebuilt as arrays; formerly a map, 2026-08-22.\n"
            "int a;\n")]) == 0,
              "comments: comment prose is not mechanically policed")
        # An issue number is the one prose token a machine can judge: outside
        # TODO(#n)/FIXME(#n) it points a reader at the tracker instead of
        # describing the code, and the rule needs no allowlist.
        check(run([script, "--root", case(
            "issue_ref", "core/src/a.cpp",
            "// Rebuilt for the #301 arrays.\nint a;\n")]) != 0,
              "comments: an issue number outside a tracked marker is a finding")
        check(run([script, "--root", case(
            "issue_ref_doc", "core/include/engine/core/a.h",
            "/// Bounded since #86 L-01.\nvoid f();\n")]) != 0,
              "comments: an issue number in a doc comment is a finding")
        check(run([script, "--root", str(write_comment_fixture(
            tmp / "issue_ref_cmake", "CMakeLists.txt",
            "# Pinned per #310.\nproject(x)\n"))]) != 0,
              "comments: an issue number in a CMake comment is a finding")
        check(run([script, "--root", case(
            "issue_ref_test", "tests/unit/a_test.cpp",
            "// Regression for #301: the map must stay bounded.\nint a;\n")]) == 0,
              "comments: a test may cite the issue it reproduces")
        check(run([script, "--root", case(
            "small_number", "core/src/a.cpp",
            "// Slot #3 is the fallback.\nint a;\n")]) == 0,
              "comments: a one-digit ordinal is not an issue reference")
        check(run([script, "--root", str(tmp / "empty")]) == 0,
              "comments: an empty tree passes")

    check(run([script]) == 0,
          "comments: this checkout passes the gate with no allowlist")


def test_error_handling_gate():
    """The error-handling gate must reject `.value()` in first-party C++,
    ignore it inside a comment, ignore trees outside the audited module
    roots, and pass this checkout."""
    script = str(TOOLS / "check_error_handling.py")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        def case(name, rel, body):
            return str(write_comment_fixture(tmp / name, rel, body))

        check(run([script, "--root", case(
            "call", "core/src/a.cpp",
            "// Purpose.\nint f() { return e.value(); }\n")]) != 0,
              "error handling: a .value() call is a finding")
        check(run([script, "--root", case(
            "spaced", "core/src/a.cpp",
            "// Purpose.\nint f() { return e . value ( ); }\n")]) != 0,
              "error handling: whitespace does not hide the call")
        check(run([script, "--root", case(
            "header", "renderer/include/engine/renderer/a.h",
            "// Purpose.\ninline int f() { return e.value(); }\n")]) != 0,
              "error handling: public headers are audited too")
        check(run([script, "--root", case(
            "comment", "core/src/a.cpp",
            "// Never call .value() here.\nint f() { return *e; }\n")]) == 0,
              "error handling: .value() named in a comment is not a finding")
        check(run([script, "--root", case(
            "block", "core/src/a.cpp",
            "/* .value() aborts */\nint f() { return *e; }\n")]) == 0,
              "error handling: a block comment is stripped too")
        check(run([script, "--root", case(
            "safe", "core/src/a.cpp",
            "// Purpose.\nint f() { return e.has_value() ? *e : 0; }\n")]) == 0,
              "error handling: the safe accessors pass")
        check(run([script, "--root", case(
            "outside", "tests/unit/a_test.cpp",
            "// Purpose.\nint f() { return e.value(); }\n")]) == 0,
              "error handling: trees outside the audited roots are ignored")
        check(run([script, "--root", str(tmp / "empty")]) == 0,
              "error handling: an empty tree passes")

    check(run([script]) == 0,
          "error handling: this checkout passes the gate")


def test_portable_fopen_gate():
    """The portability gate must reject a bare call to any deprecated CRT
    function that a Windows lane would compile, accept one in a branch
    Windows skips, ignore comments, strings and look-alike names, and pass
    this checkout."""
    script = str(TOOLS / "check_portable_fopen.py")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        def case(name, rel, body):
            return str(write_comment_fixture(tmp / name, rel, body))

        check(run([script, "--root", case(
            "bare", "tests/unit/a_test.cpp",
            "// Purpose.\nvoid f() { std::FILE *g = std::fopen(p, \"wb\"); }\n")]) != 0,
              "portable fopen: a bare std::fopen in a test is a finding")
        check(run([script, "--root", case(
            "engine", "core/src/a.cpp",
            "// Purpose.\nvoid f() { FILE *g = fopen(p, \"rb\"); }\n")]) != 0,
              "portable fopen: engine code is audited, unqualified too")
        check(run([script, "--root", case(
            "winbranch", "core/src/a.cpp",
            "// Purpose.\n#ifdef _WIN32\nFILE *g = fopen(p, m);\n#endif\n")]) != 0,
              "portable fopen: the Windows branch itself is still a finding")
        check(run([script, "--root", case(
            "idiom", "tests/unit/a_test.cpp",
            "// Purpose.\n#ifdef _WIN32\nfopen_s(&g, p, m);\n#else\n"
            "g = std::fopen(p, m);\n#endif\n")]) == 0,
              "portable fopen: the fopen_s/#else idiom passes")
        check(run([script, "--root", case(
            "msc", "tests/benchmark/a_test.cpp",
            "// Purpose.\n#if defined(_MSC_VER)\nfopen_s(&g, p, m);\n#else\n"
            "g = std::fopen(p, m);\n#endif\n")]) == 0,
              "portable fopen: _MSC_VER selects the Windows branch as well")
        check(run([script, "--root", case(
            "negated", "core/src/a.cpp",
            "// Purpose.\n#ifndef _WIN32\ng = std::fopen(p, m);\n#endif\n")]) == 0,
              "portable fopen: the body of a negated conditional passes")
        check(run([script, "--root", case(
            "afterelse", "core/src/a.cpp",
            "// Purpose.\n#ifndef _WIN32\nint x;\n#else\ng = std::fopen(p, m);\n#endif\n")]) != 0,
              "portable fopen: the #else of a negated conditional is Windows")
        check(run([script, "--root", case(
            "linux", "core/src/a.cpp",
            "// Purpose.\n#if defined(_WIN32)\nint x;\n#elif defined(__linux__)\n"
            "g = std::fopen(p, m);\n#endif\n")]) == 0,
              "portable fopen: a branch that requires another platform passes")
        check(run([script, "--root", case(
            "afterendif", "core/src/a.cpp",
            "// Purpose.\n#ifdef _WIN32\nint x;\n#else\nint y;\n#endif\n"
            "FILE *g = std::fopen(p, m);\n")]) != 0,
              "portable fopen: a call after the #endif is unguarded again")
        check(run([script, "--root", case(
            "lookalike", "core/src/a.cpp",
            "// Never call fopen( here.\nvoid f() { fopen_s(&g, p, m); my_fopen(p); "
            "log(\"fopen(\"); }\n")]) == 0,
              "portable fopen: comments, strings and look-alike names pass")
        check(run([script, "--root", case(
            "tools", "tools/asset_packer/a.cpp",
            "// Purpose.\nvoid f() { FILE *g = std::fopen(p, m); }\n")]) == 0,
              "portable fopen: tools/ is outside the audited roots")
        # The rest of the deprecated family the MSVC CRT rejects, which
        # cost a Windows lane once (a test's std::sscanf).
        check(run([script, "--root", case(
            "sscanf", "tests/unit/a_test.cpp",
            "// Purpose.\nvoid f() { std::sscanf(t, \"%d\", &v); }\n")]) != 0,
              "portable fopen: a bare std::sscanf is a finding")
        check(run([script, "--root", case(
            "sprintf", "core/src/a.cpp",
            "// Purpose.\nvoid f() { sprintf(buffer, \"%d\", v); }\n")]) != 0,
              "portable fopen: sprintf is a finding")
        check(run([script, "--root", case(
            "strcpy", "core/src/a.cpp",
            "// Purpose.\nvoid f() { strcpy(dst, src); }\n")]) != 0,
              "portable fopen: strcpy is a finding")
        check(run([script, "--root", case(
            "getenv", "core/src/a.cpp",
            "// Purpose.\nvoid f() { const char *v = std::getenv(\"HOME\"); }\n")]) != 0,
              "portable fopen: getenv is a finding")
        check(run([script, "--root", case(
            "sscanf_guarded", "core/src/a.cpp",
            "// Purpose.\n#ifndef _WIN32\nstd::sscanf(t, \"%d\", &v);\n#endif\n")]) == 0,
              "portable fopen: a guarded sscanf passes")
        check(run([script, "--root", case(
            "safe_family", "core/src/a.cpp",
            "// Purpose.\nvoid f() { std::snprintf(b, n, \"%d\", v); "
            "sscanf_s(t, \"%d\", &v); my_strcpy(d, s); }\n")]) == 0,
              "portable fopen: the safe variants and look-alikes pass")
        check(run([script, "--root", str(tmp / "empty")]) == 0,
              "portable fopen: an empty tree passes")

    check(run([script]) == 0,
          "portable fopen: this checkout passes the gate")


def test_duplicate_primitive_gate():
    """The duplicate-primitive gate must reject a re-implemented primitive
    in first-party C++, accept the canonical owner, leave the test tree
    alone, and — the case that broke the first draft — still match a
    literal carrying a ULL suffix."""
    script = str(TOOLS / "check_duplicate_primitives.py")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        def case(name, rel, body):
            return str(write_comment_fixture(tmp / name, rel, body))

        check(run([script, "--root", case(
            "copy", "core/src/a.cpp",
            "// Purpose.\nconstexpr auto p = 1099511628211ULL;\n")]) != 0,
              "duplicate primitives: a copied FNV prime is a finding")
        # A trailing \b cannot match here: digit and 'U' are both word
        # characters, so the first draft of this rule passed over every
        # real copy in the tree.
        check(run([script, "--root", case(
            "suffixed", "tools/t/a.cpp",
            "// Purpose.\nauto h = 14695981039346656037ULL;\n")]) != 0,
              "duplicate primitives: a ULL-suffixed literal still matches")
        check(run([script, "--root", case(
            "truncated", "core/src/a.cpp",
            "// Purpose.\nauto h = 1469598103934665603ULL;\n")]) != 0,
              "duplicate primitives: the mistyped offset is a finding too")
        check(run([script, "--root", case(
            "longer", "core/src/a.cpp",
            "// Purpose.\nauto h = 210995116282113ULL;\n")]) == 0,
              "duplicate primitives: a longer number containing the prime "
              "is not a finding")
        check(run([script, "--root", case(
            "clean", "core/src/a.cpp",
            "// Purpose.\nauto h = core::fnv1a_64(\"x\");\n")]) == 0,
              "duplicate primitives: using the primitive passes")
        check(run([script, "--root", case(
            "tests_exempt", "tests/integration/a.cpp",
            "// Purpose.\nauto h = 1099511628211ULL;\n")]) == 0,
              "duplicate primitives: the test tree is out of scope")
        owner = write_comment_fixture(
            tmp / "owner", "core/include/engine/core/hash.h",
            "// Purpose.\nconstexpr auto p = 1099511628211ULL;\n")
        check(run([script, "--root", str(owner)]) == 0,
              "duplicate primitives: the canonical owner may hold the "
              "constants")
        check(run([script, "--root", str(tmp / "empty")]) == 0,
              "duplicate primitives: an empty tree passes")

    check(run([script]) == 0,
          "duplicate primitives: this checkout passes the gate")


def main():
    test_coverage_gate()
    test_perf_gate_evaluate()
    test_metadata_path_check()
    test_binding_generator()
    test_module_dependency_gate()
    test_dependency_pin_gate()
    test_content_attributes_gate()
    test_test_timing_gate()
    test_comment_quality_gate()
    test_error_handling_gate()
    test_portable_fopen_gate()
    test_duplicate_primitive_gate()
    test_asset_identity_gate()
    test_shader_variant_gate()
    if failures:
        print(f"\nFAILED ({len(failures)} failure(s))")
        return 1
    print("\nAll tool gate self-tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
