#!/usr/bin/env python3
"""Audit that every shader variant the engine can request is cooked.

A missing cooked variant is the quietest failure in the renderer. When a
stage's requested define set has no cooked binary, bgfx falls back to that
stage's default binary and says nothing: the draw succeeds, the frame
looks plausible, and a toon surface is shaded physically based while the
engine believes it asked for toon. Nothing in a headless suite can see
it, and on a GPU it reads as a tuning problem rather than a missing file.

Two tables have to agree for that not to happen, and until this gate they
agreed only by hand:

- **What the engine asks for.** `ShadingModel` enumerates the models, and
  the variant table in `command_buffer_init_core.cpp` gives each
  non-default model the define that selects its program. A model missing
  from that table gets no program at all and silently draws as
  physically based.
- **What the cook produces.** `shaders.manifest` lists, per source, the
  define sets cooked into binaries.

This gate reads both and fails when the first names a set the second does
not cook. It also fails when a model is absent from the variant table,
because that is the same silent fallback one layer earlier.

`PBR_FULL` is requested or not depending on the device's sampler budget,
so both forms of every model's set must be cooked — the engine picks
between them at runtime and cannot know at cook time which a given GPU
will take.

Usage:
  python tools/check_shader_variants.py            # report, exit 1 on findings
  python tools/check_shader_variants.py --root DIR # audit an alternate tree
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
MATERIAL_HEADER = "renderer/include/engine/renderer/material.h"
VARIANT_SOURCE = "renderer/src/command_buffer_init_core.cpp"
MANIFEST = "assets/shaders/bgfx/shaders.manifest"
FORWARD_SOURCE = "pbr.fs.sc"
# The model whose program is the forward default: it needs no define,
# because it is what every other model falls back to.
DEFAULT_MODEL = "Pbr"
# Requested alongside a model's define when the device has the sampler
# budget for the full forward path, and omitted when it does not.
OPTIONAL_DEFINE = "PBR_FULL"

ENUM_RE = re.compile(r"enum\s+class\s+ShadingModel\s*:[^{]*\{([^}]*)\}")
ENUMERATOR_RE = re.compile(r"(\w+)\s*=")
# {ShadingModel::Toon, "ENGINE_SHADING_TOON", "toon"}
VARIANT_ROW_RE = re.compile(
    r"\{\s*ShadingModel::(\w+)\s*,\s*\"([^\"]+)\"\s*,\s*\"[^\"]*\"\s*\}")


def shading_models(root: pathlib.Path) -> list[str]:
    """Every ShadingModel enumerator, in declaration order."""
    text = (root / MATERIAL_HEADER).read_text(encoding="utf-8")
    match = ENUM_RE.search(text)
    if match is None:
        raise SystemExit("check_shader_variants: could not find the "
                         "ShadingModel enum; the audit would pass vacuously")
    models = ENUMERATOR_RE.findall(match.group(1))
    if not models:
        raise SystemExit("check_shader_variants: parsed no shading models "
                         "from the enum; the audit would pass vacuously")
    return models


def model_defines(root: pathlib.Path) -> dict[str, str]:
    """Each non-default model's define, from the engine's variant table."""
    text = (root / VARIANT_SOURCE).read_text(encoding="utf-8")
    rows = VARIANT_ROW_RE.findall(text)
    if not rows:
        raise SystemExit("check_shader_variants: parsed no rows from the "
                         "shading-model variant table; the audit would pass "
                         "vacuously")
    return {model: define for model, define in rows}


def cooked_variant_keys(root: pathlib.Path, source: str) -> set[frozenset]:
    """The define sets the manifest cooks for `source`."""
    document = json.loads((root / MANIFEST).read_text(encoding="utf-8"))
    keys: set[frozenset] = set()
    found = False
    for entry in document.get("shaders", []):
        if entry.get("source") != source:
            continue
        found = True
        for variant in entry.get("variants", []):
            keys.add(frozenset(variant))
    if not found:
        raise SystemExit(f"check_shader_variants: the manifest has no entry "
                         f"for {source}; the audit would pass vacuously")
    return keys


def describe(defines: frozenset) -> str:
    """A define set as the cook names it, for a readable finding."""
    return "-".join(sorted(defines)) if defines else "default"


def main() -> int:
    """Runs the audit and reports findings; exit 1 when any exist."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(REPO_ROOT))
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()

    models = shading_models(root)
    defines = model_defines(root)
    cooked = cooked_variant_keys(root, FORWARD_SOURCE)

    findings: list[str] = []
    required: list[frozenset] = []
    for model in models:
        if model == DEFAULT_MODEL:
            continue
        define = defines.get(model)
        if define is None:
            findings.append(
                f"  ShadingModel::{model}: no row in the variant table in "
                f"{VARIANT_SOURCE}, so it loads no program of its own and "
                f"draws as {DEFAULT_MODEL} without saying so")
            continue
        required.append(frozenset({define}))
        required.append(frozenset({define, OPTIONAL_DEFINE}))

    for wanted in required:
        if wanted not in cooked:
            findings.append(
                f"  {FORWARD_SOURCE}: the engine can request "
                f"[{', '.join(sorted(wanted))}] but the manifest does not "
                f"cook it, so that stage falls back to its default binary "
                f"in silence; add \"{describe(wanted)}\" to its variants")

    if findings:
        print("shader variant audit failed:")
        for finding in findings:
            print(finding)
        return 1

    print(f"shader variant audit passed: {len(models)} shading model(s), "
          f"{len(required)} requestable {FORWARD_SOURCE} variant(s) all "
          f"cooked among the manifest's {len(cooked)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
