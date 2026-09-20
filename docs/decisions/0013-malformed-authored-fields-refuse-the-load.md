# 0013 — A malformed authored field refuses the load

**Date:** 2026-09-18. Decided by the maintainer agent on the owner's
delegation under the tests-are-contracts rule; the owner may override.

## Context

Four fields in the shared scene/prefab component readers — `roughness`,
`metallic`, `opacity` and `LightComponent.intensity` — discarded their
parse result. A present-but-malformed value, such as a hand-edited or
bit-rotted `"roughness": "0.5"`, loaded as the component default with no
diagnostic, and the next save persisted that default over the authored
value. Opening and saving a file destroyed the field.

The choice was between making them strict, which changes load behavior, and
documenting the leniency as intentional.

## Decision

**Strict.** A present-but-malformed field refuses the load; an absent field
still takes the component's default.

## Rationale

The decisive fact is that **the strictness level was already set by every
other field in the same readers.** `albedo`, `sceneCaptureSourceId`, all of
`read_collider_component` and `read_foliage_patch_component` already refuse.
A scene with a malformed `albedo` has never loaded. These four were the
anomaly, so making them strict removes an inconsistency rather than raising
the bar — the "too strict" risk was already the status quo for the other
sixteen fields.

Against that, the leniency's cost is unbounded and silent: it alters
authored data on load and destroys the original on the next save, with no
diagnostic at any point. That is the failure mode the authored-data rule
exists to prevent.

Absent stays absent. Strictness applies to a value that is present and
wrong, never to one that was never written, so files that simply omit these
fields keep loading.

## Consequences

- A refusal must stay **recoverable**, which is what keeps strictness from
  being the worse error: the diagnostic names the offending field, and the
  load leaves the destination unchanged (a scene load stages into a
  replacement World and commits only on success). A refusal the author
  cannot act on would be no better than the silent substitution.
- Both formats are affected, because the readers are shared. Regressions
  cover scene and prefab separately.
- The remaining lenient readers listed on the same finding —
  the packer's import-settings reader and the editor's `.meta`
  reader — follow the same rule. The editor's should move onto one shared
  import-settings schema rather than becoming a fourth copy.
- The general form of this — `read_optional_*` helpers that return false
  only on present-and-malformed — is the shared codec layer's job. Until it
  exists, each reader carries its own strict helper.
