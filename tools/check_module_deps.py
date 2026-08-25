#!/usr/bin/env python3
"""Audit first-party include edges against the declared module graph.

CLAUDE.md requires dependencies to "flow strictly downward, no cycles or
sideways deps". Until now that rule was [REVIEW]-only, which is how the
issue #309 cycle accumulated 37 upward include sites across 20 scripting
TUs without tripping anything. This gate makes the target graph the
enforced source of truth, in the shape of the existing comment audits:
it reports findings and exits non-zero, and CI holds it at zero.

Three checks, one root cause each:

  1. Declared-graph direction. Every `#include "engine/<module>/..."` is
     validated against ALLOWED_DEPENDENCIES below. The declared chain in
     CLAUDE.md describes DIRECTION, not adjacency, so reaching past a
     tier downward (editor -> renderer) is legal while anything upward
     (scripting -> runtime) or sideways (scripting -> physics) is not.

  2. Cross-module private headers. A quoted include that resolves only
     into another module's src/ bypasses that module's public surface.
     The one sanctioned case is editor -> runtime/src/component_registry.h
     (issue #156), which is allowlisted rather than special-cased.

  3. Hand-wired foreign include directories. A `*_INCLUDE_DIRS` entry
     pointing into another module grants header access without declaring
     the dependency, so CMake's usage requirements stop being the source
     of truth. A dependency is expressed only as a dep on the target.

Today's known violations are listed in KNOWN_VIOLATIONS with the issue
that tracks each. An entry that no longer matches anything is itself a
finding, so the list can only shrink: the fix that removes the last
site of a tracked edge must delete its entries in the same commit, and
the gate is red on that fix's base revision.

Tests are deliberately out of scope: test targets legitimately reach
into module src/ directories through PRIVATE_INCLUDE_DIRS to exercise
internal APIs, which is the established pattern, not a layering break.

Usage:
  python tools/check_module_deps.py           # report, exit 1 on findings
  python tools/check_module_deps.py --root DIR  # audit an alternate tree
"""

from __future__ import annotations

import argparse
import pathlib
import re

# Every dependency each module may express, as the transitive downward
# closure of the CLAUDE.md chain
# (app -> editor -> runtime -> renderer/physics/scripting/audio ->
#  content -> core/math) with the two documented narrowings applied:
# `content` depends only on `core`, and within the bottom tier the
# direction is math -> core, never the reverse. The four mid-tier
# subsystem modules are siblings and so are absent from each other's
# sets; they meet only in `runtime`, which owns the bridges.
ALLOWED_DEPENDENCIES: dict[str, frozenset[str]] = {
    "core": frozenset(),
    "math": frozenset({"core"}),
    "content": frozenset({"core"}),
    "renderer": frozenset({"content", "core", "math"}),
    "physics": frozenset({"content", "core", "math"}),
    "scripting": frozenset({"content", "core", "math"}),
    "audio": frozenset({"content", "core", "math"}),
    "runtime": frozenset(
        {"renderer", "physics", "scripting", "audio", "content", "core", "math"}
    ),
    "editor": frozenset(
        {
            "runtime",
            "renderer",
            "physics",
            "scripting",
            "audio",
            "content",
            "core",
            "math",
        }
    ),
    "app": frozenset(
        {
            "editor",
            "runtime",
            "renderer",
            "physics",
            "scripting",
            "audio",
            "content",
            "core",
            "math",
        }
    ),
    # Offline tools sit above the engine and consume it like an
    # application would; they are never consumed by it.
    "tools": frozenset(
        {
            "editor",
            "runtime",
            "renderer",
            "physics",
            "scripting",
            "audio",
            "content",
            "core",
            "math",
        }
    ),
}

# Directories that carry a module's own headers, in resolution order.
MODULE_SOURCE_DIRS: tuple[str, ...] = ("include", "src")

CPP_SUFFIXES = frozenset({".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"})

QUOTED_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
ENGINE_INCLUDE_RE = re.compile(r"^engine/([a-z_]+)/")
# A hand-wired path into another module's headers, e.g.
# ${CMAKE_SOURCE_DIR}/runtime/include.
CMAKE_FOREIGN_DIR_RE = re.compile(
    r"\$\{CMAKE_SOURCE_DIR\}/([a-z_]+)/(include|src)\b"
)

# Violations that exist today, each tracked by an open issue. Entries are
# exact so that fixing one site removes exactly one line; a stale entry
# fails the audit, which is what keeps the list shrinking to empty.
#
# Direction and private-header findings: (source file, included path).
KNOWN_VIOLATIONS: dict[tuple[str, str], str] = {
    # Sanctioned exception, recorded in CLAUDE.md rather than pending a
    # fix: the editor generates its Inspector dispatch from the runtime's
    # X-macro component table so a new persistent component cannot skip
    # the Inspector (issue #156). Kept here, not hard-coded, so it stays
    # visible as the single crossing it is.
    (
        "editor/src/editor_component_registry.h",
        "component_registry.h",
    ): "sanctioned: issue #156 Inspector metadata generated from the runtime registry",
}

# Every scripting TU that reaches upward into runtime (issue #309) or
# sideways into physics (issue #310). Enumerated by header so each
# migrated call site deletes its own line.
_SCRIPTING_UPWARD_INCLUDES: dict[str, tuple[str, ...]] = {
    "engine/physics/primitive_hulls.h": ("mesh_material_bindings.cpp",),
    "engine/runtime/entity_pool.h": ("entity_pool_bindings.cpp",),
    "engine/runtime/game_binding_state.h": ("game_bindings.cpp",),
    "engine/runtime/game_mode.h": ("game_bindings.cpp",),
    "engine/runtime/game_state.h": ("game_bindings.cpp",),
    "engine/runtime/physics_bridge.h": ("mesh_material_bindings.cpp",),
    "engine/runtime/player_controller.h": ("game_bindings.cpp",),
    "engine/runtime/scripting_bridge.h": (
        "asset_bindings.cpp",
        "audio_bindings.cpp",
        "body_bindings.cpp",
        "camera_bindings.cpp",
        "deferred_mutations.cpp",
        "entity_lifecycle_bindings.cpp",
        "light_bindings.cpp",
        "mesh_material_bindings.cpp",
        "persist_bindings.cpp",
        "physics_bindings.cpp",
        "runtime_binding.h",
        "scripting.cpp",
    ),
    "engine/runtime/timer_manager.h": ("timer_bindings.cpp",),
    "engine/runtime/world.h": (
        "asset_bindings.cpp",
        "audio_bindings.cpp",
        "body_bindings.cpp",
        "camera_bindings.cpp",
        "cheat_bindings.cpp",
        "deferred_mutations.h",
        "entity_handle.h",
        "entity_handle_value.h",
        "entity_lifecycle_bindings.cpp",
        "entity_pool_bindings.cpp",
        "entity_script_bindings.cpp",
        "entity_script_bindings.h",
        "game_bindings.cpp",
        "light_bindings.cpp",
        "mesh_material_bindings.cpp",
        "physics_bindings.cpp",
        "scripting.cpp",
        "timer_bindings.cpp",
    ),
}

for _header, _sources in _SCRIPTING_UPWARD_INCLUDES.items():
    _issue = "#310" if _header.startswith("engine/physics/") else "#309"
    for _source in _sources:
        KNOWN_VIOLATIONS[(f"scripting/src/{_source}", _header)] = (
            f"tracked: issue {_issue} scripting bridge migration"
        )

# Hand-wired foreign include directories that exist today:
# (CMakeLists path, granted module, granted subdirectory).
KNOWN_CMAKE_GRANTS: dict[tuple[str, str, str], str] = {
    (
        "scripting/CMakeLists.txt",
        "runtime",
        "include",
    ): "tracked: issue #309 — the grant that enables the upward includes above",
    (
        "editor/CMakeLists.txt",
        "runtime",
        "src",
    ): "sanctioned: issue #156 component registry, paired with the include above",
    (
        "editor/CMakeLists.txt",
        "runtime",
        "include",
    ): "tracked: issue #311 — redundant, PUBLIC_DEPS engine_runtime already propagates it",
    (
        "editor/CMakeLists.txt",
        "renderer",
        "include",
    ): "tracked: issue #311 — redundant, PUBLIC_DEPS engine_renderer already propagates it",
    (
        "physics/CMakeLists.txt",
        "core",
        "include",
    ): "tracked: issue #311 — core belongs in PUBLIC_DEPS; public headers include engine/core/entity.h",
}


class Finding:
    """One layering violation, rendered as a single report line."""

    def __init__(self, location: str, message: str) -> None:
        self.location = location
        self.message = message

    def __str__(self) -> str:
        return f"  {self.location}: {self.message}"


def module_of(root: pathlib.Path, path: pathlib.Path) -> str | None:
    """Returns the audited module a file belongs to, if any."""
    try:
        relative = path.relative_to(root)
    except ValueError:
        return None
    top = relative.parts[0] if relative.parts else ""
    return top if top in ALLOWED_DEPENDENCIES else None


def audited_sources(root: pathlib.Path) -> list[pathlib.Path]:
    """Returns every C++ source and header inside an audited module."""
    found: list[pathlib.Path] = []
    for module in sorted(ALLOWED_DEPENDENCIES):
        module_dir = root / module
        if not module_dir.is_dir():
            continue
        for path in sorted(module_dir.rglob("*")):
            if path.is_file() and path.suffix.lower() in CPP_SUFFIXES:
                found.append(path)
    return found


def quoted_includes(path: pathlib.Path) -> list[str]:
    """Returns the quoted include paths a file names, in file order."""
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8-sig")
    includes: list[str] = []
    for line in text.splitlines():
        match = QUOTED_INCLUDE_RE.match(line)
        if match is not None:
            includes.append(match.group(1))
    return includes


def resolves_locally(
    root: pathlib.Path, source: pathlib.Path, module: str, include: str
) -> bool:
    """Returns whether an include resolves inside its own module."""
    neighbour = source.parent / include
    if neighbour.is_file():
        # A `..` path can climb out of the module while still resolving
        # next to the file, which would otherwise be the way around the
        # private-header check below.
        return is_inside(root / module, neighbour)
    return any(
        (root / module / directory / include).is_file()
        for directory in MODULE_SOURCE_DIRS
    )


def is_inside(directory: pathlib.Path, path: pathlib.Path) -> bool:
    """Returns whether a resolved path lies within a directory."""
    try:
        path.resolve().relative_to(directory.resolve())
    except ValueError:
        return False
    return True


def foreign_private_owner(
    root: pathlib.Path, source: pathlib.Path, module: str, include: str
) -> str | None:
    """Returns the other module whose sources hold this header, if one does."""
    for other in sorted(ALLOWED_DEPENDENCIES):
        if other == module:
            continue
        if (root / other / "src" / include).is_file():
            return other
    # A `..` include that resolved next to the file but climbed out of
    # the module names its owner by where it landed, not by name.
    neighbour = source.parent / include
    if neighbour.is_file():
        owner = module_of(root, neighbour.resolve())
        if owner is not None and owner != module:
            return owner
    return None


def check_include_edges(
    root: pathlib.Path, used: set[tuple[str, str]], allowlisted: bool
) -> list[Finding]:
    """Validates every first-party include against the declared graph."""
    findings: list[Finding] = []
    for source in audited_sources(root):
        module = module_of(root, source)
        if module is None:
            continue
        relative = source.relative_to(root).as_posix()
        allowed = ALLOWED_DEPENDENCIES[module]
        for include in quoted_includes(source):
            key = (relative, include)
            engine_match = ENGINE_INCLUDE_RE.match(include)
            if engine_match is not None:
                target = engine_match.group(1)
                if target == module or target not in ALLOWED_DEPENDENCIES:
                    continue
                if target in allowed:
                    continue
                if allowlisted and key in KNOWN_VIOLATIONS:
                    used.add(key)
                    continue
                findings.append(
                    Finding(
                        relative,
                        f'includes "{include}": {module} may not depend on '
                        f"{target}",
                    )
                )
                continue
            if resolves_locally(root, source, module, include):
                continue
            owner = foreign_private_owner(root, source, module, include)
            if owner is None:
                continue
            if allowlisted and key in KNOWN_VIOLATIONS:
                used.add(key)
                continue
            findings.append(
                Finding(
                    relative,
                    f'includes "{include}" from {owner}/src: a module\'s '
                    "private headers are not a public surface",
                )
            )
    return findings


def check_cmake_grants(
    root: pathlib.Path, used: set[tuple[str, str, str]], allowlisted: bool
) -> list[Finding]:
    """Flags hand-wired include directories into other modules."""
    findings: list[Finding] = []
    for module in sorted(ALLOWED_DEPENDENCIES):
        lists_file = root / module / "CMakeLists.txt"
        if not lists_file.is_file():
            continue
        relative = lists_file.relative_to(root).as_posix()
        for number, line in enumerate(
            lists_file.read_text(encoding="utf-8").splitlines(), start=1
        ):
            match = CMAKE_FOREIGN_DIR_RE.search(line)
            if match is None:
                continue
            target, subdirectory = match.group(1), match.group(2)
            if target == module:
                continue
            key = (relative, target, subdirectory)
            if allowlisted and key in KNOWN_CMAKE_GRANTS:
                used.add(key)
                continue
            findings.append(
                Finding(
                    f"{relative}:{number}",
                    f"hand-wires {target}/{subdirectory}: declare a dep on "
                    f"engine_{target} instead so usage requirements carry it",
                )
            )
    return findings


def check_stale_allowlist(
    used_includes: set[tuple[str, str]], used_grants: set[tuple[str, str, str]]
) -> list[Finding]:
    """Flags allowlist entries that no longer match anything."""
    findings: list[Finding] = []
    for (source, include), reason in sorted(KNOWN_VIOLATIONS.items()):
        if (source, include) not in used_includes:
            findings.append(
                Finding(
                    "tools/check_module_deps.py",
                    f'stale allowlist entry: {source} no longer includes '
                    f'"{include}" ({reason}) — delete the entry',
                )
            )
    for (lists_file, target, subdirectory), reason in sorted(
        KNOWN_CMAKE_GRANTS.items()
    ):
        if (lists_file, target, subdirectory) not in used_grants:
            findings.append(
                Finding(
                    "tools/check_module_deps.py",
                    f"stale allowlist entry: {lists_file} no longer "
                    f"hand-wires {target}/{subdirectory} ({reason}) — delete "
                    "the entry",
                )
            )
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        default=str(pathlib.Path(__file__).resolve().parents[1]),
        help="repository root to audit (defaults to this checkout)",
    )
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()

    # The allowlist enumerates paths in THIS repository, so it applies
    # only when auditing this checkout. An alternate root — the gate's
    # own self-tests, or a repository that vendors these modules — is
    # audited against the declared graph alone, with nothing excused.
    allowlisted = root == pathlib.Path(__file__).resolve().parents[1]

    used_includes: set[tuple[str, str]] = set()
    used_grants: set[tuple[str, str, str]] = set()

    findings = check_include_edges(root, used_includes, allowlisted)
    findings += check_cmake_grants(root, used_grants, allowlisted)
    if allowlisted:
        findings += check_stale_allowlist(used_includes, used_grants)

    if findings:
        print("module dependency audit failed:")
        for finding in findings:
            print(str(finding))
        return 1

    excused = len(used_includes) + len(used_grants)
    print(
        "module dependency audit passed: "
        f"{len(ALLOWED_DEPENDENCIES)} modules checked, "
        f"{excused} tracked violations still allowlisted"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
