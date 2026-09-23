# 0018 — Authors compose shading; the engine ships the pieces and the presets

**Date:** 2026-09-22. Owner decision, extending
[0017](0017-materials-carry-a-shading-model.md) and superseding its
point 5.

**Status:** Step 1 in progress: shading-program ids and the
`kMaxShadingPrograms` limit of 128 exist, while materials and the draw
key still select by `ShadingModel`.

## Context

[0017](0017-materials-carry-a-shading-model.md) gave a material a
`shadingModel` field over a closed enum — `PBR`, `Toon`, `Unlit` — and
said stylisation features are sub-paths of a model: ramp textures,
outlines, rim light and face-shadow maps belong to `Toon`. That assumed
the engine can enumerate what an anime look is made of.

It cannot, and the cost of assuming so is now measured. Getting the
first toon response to read as cel shading took three GPU round trips
over #638: a response that was pi times overbright, then one whose
shadow side fell to ambient and read as a harshly lit sphere with a dark
half, then the flat tone it needed. Every round was an art decision
being made in C++ by someone who could not see the frame, and each
carried a rebuild, a capture and a judgement. The third round was
accepted with two remaining notes — a terminator that softens as the
object grows on screen, and a shadow tone with no hue shift — both of
which are again art decisions, and both of which the next art direction
would answer differently.

The range is the point. Genshin, Arcane, Guilty Gear and Blue Protocol
are all "anime", and their shadow shapes, ramp counts, outline rules and
specular treatments have less in common with each other than any of them
has with physically based shading. A fixed enum with fixed sub-paths
serves one of them. [0015](0015-commercial-anime-engine-on-six-platforms.md)
commits to an engine for anime games, not to one anime game's look.

Four things already in the tree make the alternative reachable rather
than speculative:

- The flush already binds **one program per run** and partitions draws
  into runs by the key's shading field, so more programs is more runs,
  not a new mechanism.
- That key field is already **7 bits wide** (`kDrawKeyShadingModelMask`),
  so it addresses 128 programs without a layout change.
- `tools/check_shader_variants.py` already gates the set of define sets
  the engine can request against the set the cook produces, which is the
  rule that stops a missing variant falling back to a stage default in
  silence — the failure mode that gets worse, not better, when the
  number of programs is authored rather than fixed.
- The cook already carries variants per profile, content-addressed
  outputs and cook-stamp identity.

What makes it hard is equally concrete. A graph is a compiler: graph to
intermediate form to per-profile source to cooked binary, with a type
system, a node library and constant folding. It has to be that under
this engine's constraints — no exceptions, no RTTI, fixed-capacity
storage, deterministic — where a graph's natural dynamic sizing meets a
capacity policy that must refuse and report. And the variant count
multiplies: every graph against every define set against every profile,
which an all-variants-upfront manifest cannot hold.

## Decision

1. **The engine ships shading programs, not a closed set of shading
   models.** A material names the program it shades with. The built-in
   `PBR`, `Toon` and `Unlit` become the first three registered programs
   and keep their current semantics.

2. **The built-ins remain presets, permanently.** A graph with no good
   default is worse than a fixed shader, because the first thing an
   author needs is a correct starting point rather than an empty canvas.
   Shipping the presets alongside the composer is the arrangement, not a
   transitional stage.

3. **The built-ins are the reference implementation of the node
   semantics.** The pieces #638 settled are not incidental to the toon
   program, they are what its nodes must do: a diffuse response carries
   the Lambertian normalisation whatever shapes its falloff, and a flat
   shadow tone belongs to the surface rather than to each light and is
   therefore outside both the light loop and the shadow multiply. A
   composer that lets an author reassemble those wrongly is a faster way
   to get a wrong picture.

4. **Stylisation features are nodes an author composes, not sub-paths
   the engine enumerates.** This supersedes 0017 point 5. Ramp
   lighting, outlines, rim light, authored shadow colour and face-shadow
   maps are graph nodes. The `Toon` preset is a graph built from them,
   and an author edits a copy of it rather than asking for a flag.

5. **The order is decouple, then author in source, then compose in a
   graph.** Program selection stops being an enum first; a user-authored
   shader source path proves the cook, identity and variant chain with
   one hand-written program second; the node editor comes last, because
   its value depends entirely on the first two working and it is the
   expensive piece. Each step is useful on its own and none of them is a
   prerequisite rewrite of the others.

6. **A shading program is a source asset with its own identity.** It
   carries a GUID in a sidecar like any other authored asset, its
   compiled variants are cooked outputs claimed by a cook stamp, and a
   material references it by `AssetRef`. Variants cook on demand and
   cache; the manifest stays the list of what the engine itself
   requires.

7. **Programs and graphs are fixed-capacity, and the limits are
   published.** The draw key's 7-bit field caps registered programs at
   128, which is the engine's stated limit rather than an implementation
   detail; node and edge counts per graph carry their own caps. One past
   capacity refuses and reports, per `docs/architecture.md`.

8. **A graph is compiled, never interpreted at draw time.** Nothing in
   the render hot path walks a node graph. Composition happens in the
   editor and the cook; the frame sees a program handle.

What 0017 established and this does not disturb: shading is a material's
property and never the build's; pass selection is per draw; the deferred
and forward paths coexist as peers; an unknown authored value refuses
the load; and the shading choice is part of the material's content hash
and of nothing else's cook key.

## Consequences

- `docs/architecture.md`'s rule that adding a model means adding its
  program and its run, never a global mode or a cvar, extends: never a
  global mode, a cvar, **or an engine code change**.
- `ShadingModel` stops being the type the draw key carries; the key
  carries a program id over the same 7 bits, and `kShadingModelCount`
  becomes a published maximum rather than a count of three.
  *(The published maximum landed as `kMaxShadingPrograms`;
  `kShadingModelCount` stays the count of built-in presets.)*
- `tools/check_shader_variants.py` generalises from enum rows against
  manifest rows to registered programs against cooked variants. Its
  vacuity guards matter more once the left-hand side is authored.
- The node editor needs a canvas with typed connections, and the
  candidate libraries are third-party, which is an
  [OWNER] approval under `CLAUDE.md` rather than a choice made while
  implementing.
- Verification stays the renderer tier of the `verify` skill for every
  slice: a green suite is no evidence, and a preset's appearance needs a
  dated observation. The #638 history is the argument for keeping that
  bar rather than an argument against it.
- A shader an author wrote can fail to compile. That is a content error
  with a diagnostic and a preserved previous state, on the terms
  [0013](0013-malformed-authored-fields-refuse-the-load.md) sets for
  authored data — not a crash and not a silent fallback to a default
  program.
