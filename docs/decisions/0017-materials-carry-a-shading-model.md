# 0017 — A material carries its shading model; passes are chosen per draw

**Date:** 2026-09-21. Owner decision, taken with
[0015](0015-commercial-anime-engine-on-six-platforms.md).

**Status:** Implemented as material schema v3, differently in two
respects. The G-buffer carries no shading-model channel: non-PBR opaque
draws render forward over the deferred depth instead. It landed apart
from the AssetRef cutover; material schema v4 later moved the texture
and parent references from paths to AssetRefs. Point 5 is superseded by
[0018](0018-authors-compose-shading.md).

## Context

`renderer::Material` is a fixed physically-based parameter set — albedo,
emissive, roughness, metallic, opacity, cutoff, UV transform — with no
field that could say a material is anything else. Whether a frame is lit
through the G-buffer or the forward path is one global cvar,
`r_deferred`, read once per frame. So a toon-shaded character cannot
share a frame with a physically lit environment, and there is no place in
the authored data for the choice.

Anime rendering is per-object stylisation: which ramp, whether an
outline, how the face is lit. Engines that ship the style either run
forward-first or carry a shading-model identifier through the G-buffer
and dispatch lighting on it. Both need the material to say which model it
uses, and both need the pass decision made per draw.

Every material authored and every shader written before this field
exists is one more to migrate. The AssetRef reference cutover is about to
rewrite every `.mat` document and bump its schema anyway.

## Decision

1. **`Material` gains a `shadingModel` field.** Initial models: `PBR`
   (today's behaviour), `Toon`, `Unlit`. An unknown value refuses the
   load ([0013](0013-malformed-authored-fields-refuse-the-load.md));
   models are added by extending the enum and the shader set together,
   never by a string a shader silently ignores.
2. **Pass selection is per draw, by shading model.** The G-buffer carries
   a shading-model identifier and deferred lighting dispatches on it; a
   model that needs the forward path is drawn forward in the same frame.
   Deferred and forward coexist. `r_deferred` becomes a diagnostic
   override, not the mechanism.
3. **This lands in the same migration as the AssetRef cutover.** One
   material schema bump, one tree-wide rewrite, no dual-read layer — the
   terms the owner set for asset identity.
4. **The shading model is authored material data.** It is part of the
   material's content hash and of nothing else's cook key; a mesh does
   not recook because a material's model changed.
5. **Stylisation features are sub-paths of a model.** Ramp textures,
   outlines, rim light and face-shadow maps belong to `Toon`; they are
   not top-level material flags.

## Consequences

- The renderer's forward fallback stops being a fallback and becomes a
  peer path; the `docs/architecture.md` invariant that deferred changes
  preserve forward behaviour now cuts both ways.
- Verification is the renderer tier of the `verify` skill: a green suite
  is no evidence, and each model's first appearance carries a dated human
  observation.
- Material schema v2 is the last version without a shading model; v3 is
  the first with one.
