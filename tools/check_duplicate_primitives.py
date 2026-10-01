#!/usr/bin/env python3
"""Audit first-party C++ for hand-written copies of a consolidated primitive.

A primitive that exists once in `core` but is re-implemented at a call
site stops being one primitive: the copies drift, and the drift is silent
because a hash with the wrong seed, or a slot table with a different wrap
rule, still appears to work. The FNV-1a seed was already mistyped in two
copies (a digit dropped from the 64-bit offset) without anything noticing.

Each consolidation adds one rule here, so the merge it performed cannot be
undone by the next call site that needs the same thing. Rules name the
canonical owner and the evidence of a copy — usually a magic constant,
since a copied algorithm carries the original's numbers.

Scope is non-test first-party code for the production primitives. A test
may legitimately want the arithmetic without the contract
(`scheduler_stress` uses these multipliers as CPU work, not as a hash),
and migrating the test tree is tracked separately on #484. Python copies
are out of reach of a C++ primitive and are tracked there too. The test
tree has primitives of its own -- test doubles every suite used to copy --
and TEST_RULES holds those, checked under `tests` only.

Usage:
  python tools/check_duplicate_primitives.py            # report, exit 1 on findings
  python tools/check_duplicate_primitives.py --root DIR # audit an alternate tree
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

from source_roots import ENGINE_SOURCE_ROOTS

CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"}

# First-party roots holding production and tool code. `tests` is excluded;
# see the module docstring.
AUDITED_ROOTS = ENGINE_SOURCE_ROOTS + ("tools",)


class Rule:
    """One consolidated primitive and the evidence that it was copied."""

    def __init__(self, name: str, owner: str, pattern: str, remedy: str,
                 allowed: tuple[str, ...] = ()) -> None:
        self.name = name
        self.owner = owner
        self.pattern = re.compile(pattern)
        self.remedy = remedy
        # Files that match the pattern for a different primitive, each
        # named with the reason beside the rule.
        self.allowed = allowed


RULES: tuple[Rule, ...] = (
    Rule(
        name="strict float-token parsing",
        owner="core/src/text_parse.cpp",
        # Parsing a whole token as a finite float is what a cvar value, a
        # stored preference and a console argument share; the cvar and
        # the preference reader each carried a copy, and the console
        # parsed with atof, which turned a typo into 0. The JSON reader
        # parses its own number grammar and the fog-colour reader scans a
        # list with a cursor, so both may call strtof themselves.
        pattern=r"\bstd::(?:strtof|strtod|atof)\s*\(",
        remedy="include engine/core/text_parse.h and use parse_float_token",
        allowed=("core/src/json.cpp", "renderer/src/render_settings.cpp"),
    ),
    Rule(
        name="FNV-1a hashing",
        owner="core/include/engine/core/hash.h",
        # The 32- and 64-bit offsets and primes, plus the truncated offset
        # that copy-paste has already produced in this tree. The bounds are
        # digit-only lookarounds, not \b: a literal carries a ULL suffix, so
        # a trailing \b never matches (digit and 'U' are both word
        # characters) and the rule would pass over every real copy.
        pattern=r"(?<![0-9])(14695981039346656037|1099511628211|2166136261"
        r"|16777619|1469598103934665603)(?![0-9])",
        remedy="include engine/core/hash.h and use fnv1a_32/fnv1a_64 or "
        "their _append forms",
    ),
    Rule(
        name="the draw sort key's bit layout",
        owner="renderer/include/engine/renderer/command_buffer.h",
        # The shifts that place the key's fields. Render prep, the sort and
        # the flush each carried their own copy of these, so a field could
        # move in one and not the others and draws would silently sort
        # wrong. A copy is always a shift by one of the field offsets
        # applied to a 64-bit literal, which is what this matches.
        pattern=r"1ULL\s*<<\s*63U|<<\s*(?:56U|36U)(?![0-9])",
        remedy="include engine/renderer/command_buffer.h and use the "
        "kDrawKey* constants or the draw_key_* accessors",
    ),
    Rule(
        name="the per-draw forward uniform upload",
        owner="renderer/src/command_buffer_flush_uniforms.cpp",
        # Writing one of the per-draw material or transform locations is
        # what a copied forward draw loop looks like. The forward pass,
        # the deferred path's transparent pass and each scene capture
        # each carried one, so a uniform added to one draw could be
        # forgotten in the other two and the same material shaded
        # differently depending on which pass drew it. Per-frame
        # uniforms (time, camera, lighting, fog) legitimately stay in
        # the passes and are deliberately not matched here.
        pattern=r"set_param_\w+\(\s*backend\.pbr(?:Albedo|Roughness|Metallic"
        r"|Opacity|Emissive|HasAlbedoTexture|Model|Mvp|NormalMatrix)Location",
        remedy="call upload_forward_material and draw_forward_command from "
        "command_buffer_flush_internal.h instead of uploading the set by hand",
    ),
    Rule(
        name="the engine's one asset catalog",
        owner="runtime/src/engine_pipeline.cpp",
        # Constructing a catalog is what a second one looks like: a
        # by-value member or variable, or AssetCatalog() behind a new. The
        # renderer once embedded its own, and every system that named an
        # asset reached it through the renderer. Pointers and references
        # to the one the pipeline owns are how everything else reaches it,
        # and are not matched; nor is the type's own declaration, its
        # defaulted constructor or its destructor.
        pattern=r"(?<!~)\bAssetCatalog\s*\(\s*\)(?!\s*noexcept\s*=)"
        r"|\bAssetCatalog\s+(?!final\b)[A-Za-z_]\w*\s*(?:\{|;|=)",
        remedy="take a content::AssetCatalog pointer from "
        "EngineAssetDatabaseService or as a parameter instead of holding one",
    ),
    Rule(
        name="frustum plane extraction",
        owner="math/include/engine/math/frustum.h",
        # A Gribb-Hartmann copy adds or subtracts a matrix's x, y or z row
        # to or from its w row: `c0.w + c0.x` on columns, `vp[3] + vp[0]`
        # on a float array. Render prep and light culling each carried
        # one, and only render prep's placed the near plane by the
        # device's depth convention.
        pattern=r"(\b\w+)\.w\s*[-+]\s*\1\.[xyz]\b"
        r"|(\b\w+)\[3\]\s*[-+]\s*\2\[[012]\]",
        remedy="include engine/math/frustum.h and use "
        "frustum_from_view_projection and the frustum_excludes_* tests",
    ),
    Rule(
        name="the perspective divide of an unprojection",
        owner="math/include/engine/math/frustum.h",
        # Dividing a homogeneous point's x, y or z by its own w, or taking
        # its w's reciprocal, is what a copied unproject looks like. The
        # shadow cascade fit and the editor camera each carried one, with
        # different degenerate-w thresholds and failure handling.
        pattern=r"(\b\w+)\.[xyz]\s*/=?\s*\1\.w\b"
        r"|\b1\.0F?\s*/\s*\w+\.w\b",
        remedy="include engine/math/frustum.h and use unproject_ndc or "
        "frustum_corners",
    ),
    Rule(
        name="the affine point and vector transforms",
        owner="math/include/engine/math/mat4.h",
        # Defining a Vec3-returning transform_point or transform_vector is
        # what a copy looks like: the contact clipper, the collider
        # geometry and the physics queries each defined their own.
        pattern=r"\bVec3\s+transform_(?:point|vector)\s*\(",
        remedy="include engine/math/mat4.h and use math::transform_point "
        "or math::transform_vector",
    ),
    Rule(
        name="the built-in primitive colliders",
        owner="runtime/src/primitive_collider.cpp",
        # Assigning a primitive's hull source, or writing the pyramid's
        # 0.58 half-depth, is what a copy looks like: the editor's Create
        # menu and Lua's spawn_shape each described the six primitives,
        # and only one of them kept the plane's top on its surface.
        pattern=r"=\s*(?:\w+::)*HullSource::(?:Cylinder|Pyramid)\b"
        r"|\bVec3\s*\(\s*0\.5F?\s*,\s*0\.5F?\s*,\s*0\.58F?\s*\)",
        remedy="include engine/runtime/primitive_collider.h and use "
        "runtime::primitive_collider",
    ),
    Rule(
        name="UI-scaled pixel sizes",
        owner="editor/src/editor_session.h",
        # A literal pixel size in a layout call is what an unscaled one
        # looks like: the toolbar, Console, Inspector and remove buttons
        # each laid out at fixed pixels, which stopped fitting as the
        # font grew with the UI scale. Zero and -1 are ImGui's "auto" and
        # "fill" values, not sizes; a literal inside editor_px or a string
        # is scaled or is text.
        pattern=r"\b(?:SetNextItemWidth|PushItemWidth|SameLine|SetCursorPos[XY]?"
        r"|Indent|Dummy|SetNextWindowSize)\s*\([^;]*?"
        r"(?<![-\w.\"])(?<!editor_px\()[1-9][0-9]*\.[0-9]*F?\b",
        remedy="wrap the size in editor_px, or derive it from the style or "
        "a text size",
    ),
    Rule(
        name="the ASCII case fold and case-insensitive search",
        owner="core/include/engine/core/string_util.h",
        # Folding case with the C library is what a copy looks like: the
        # reference pickers, the asset query, the Log filter and the Add
        # Component menu each carried a case-insensitive substring loop,
        # and std::tolower answers by the C locale besides.
        pattern=r"(?<![\w.])(?:std::)?to(?:lower|upper)\s*\(",
        remedy="include engine/core/string_util.h and use core::ascii_lower "
        "or core::contains_ignoring_case",
    ),
)


# Test doubles consolidated into one shared fake. Production code may and
# must define these (the real device TU does), so they are checked only in
# the test tree.
TEST_RULES: tuple[Rule, ...] = (
    Rule(
        name="the fake render device seam",
        owner="tests/fake_render_device.cpp",
        # Defining the seam is what a copied fake device starts with: the
        # renderer suites each carried one, with its own handle counter,
        # alive counts and failure switch beside it.
        pattern=r"(?<!\w)(?:initialize_|shutdown_)?render_device\(\)"
        r"\s*noexcept\s*\{",
        remedy="link tests/fake_render_device.cpp and configure "
        "engine::tests::fake_device() instead",
    ),
    Rule(
        name="the bundled-asset root walk",
        owner="tests/asset_root.h",
        # Probing a candidate directory for a bundled file is what the walk
        # looks like: 55 suites each carried one, under four names, each
        # probing a different file of the same assets directory.
        pattern=r"exists\([^;]*/\s*\"assets[/\"]"
        r"|weakly_canonical\(\s*candidate\b",
        remedy="include tests/asset_root.h and call "
        "engine::tests::enter_asset_root() or find_asset_root()",
    ),
    Rule(
        name="the test PNG encoder",
        owner="tests/png_fixture.h",
        # The chunk CRC polynomial and the header chunk name are what a
        # hand-written PNG encoder carries: three GPU suites each had one.
        pattern=r"0xEDB88320|\"IHDR\"",
        remedy="include tests/png_fixture.h and call "
        "engine::tests::encode_rgb8_png() or uniform_rgb8_png()",
    ),
    Rule(
        name="the render prep frame driver",
        owner="tests/render_prep_harness.h",
        # Enqueuing render prep by hand is where a copied driver starts:
        # four suites each carried the phase jobs, the enqueue and the end
        # of the frame, and every signature change had to touch all four.
        pattern=r"(?<!\w)enqueue_render_prep_pipeline\(",
        remedy="include tests/render_prep_harness.h and call "
        "engine::tests::run_render_prep()",
    ),
)


def audited_files(
    root: pathlib.Path, roots=AUDITED_ROOTS
) -> list[pathlib.Path]:
    """Returns every first-party C++ source under the given roots."""
    files: list[pathlib.Path] = []
    for name in roots:
        directory = root / name
        if not directory.is_dir():
            continue
        files.extend(
            path
            for path in directory.rglob("*")
            if path.is_file() and path.suffix.lower() in CPP_SUFFIXES
        )
    return sorted(files)


def check_file(path: pathlib.Path, rel: str, rules=RULES) -> list[str]:
    """Returns one finding per copied-primitive hit in the file."""
    findings: list[str] = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return findings

    for rule in rules:
        if rel == rule.owner or rel in rule.allowed:
            continue
        for number, line in enumerate(lines, 1):
            if rule.pattern.search(line):
                findings.append(
                    f"  {rel}:{number}: re-implements {rule.name}, which "
                    f"{rule.owner} owns; {rule.remedy}"
                )
    return findings


def main() -> int:
    """Runs the audit and reports findings; exit 1 when any exist."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        default=str(pathlib.Path(__file__).resolve().parents[1]),
        help="repository root to audit (defaults to this checkout)",
    )
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()

    findings: list[str] = []
    files = audited_files(root)
    for path in files:
        findings.extend(check_file(path, path.relative_to(root).as_posix()))
    test_files = audited_files(root, ("tests",))
    for path in test_files:
        findings.extend(
            check_file(path, path.relative_to(root).as_posix(), TEST_RULES)
        )

    if findings:
        print("duplicate-primitive audit failed:")
        for finding in findings:
            print(finding)
        return 1

    print(
        f"duplicate-primitive audit passed: {len(files) + len(test_files)} "
        f"file(s), {len(RULES) + len(TEST_RULES)} consolidated primitive(s) "
        "checked"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
