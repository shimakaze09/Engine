# 0010 — A generational handle always bumps on release

**Date:** 2026-09-18. Decided by the maintainer agent on the owner's
delegation; the owner may override.

**Status:** Implemented in `EntityPool` through `World::recycle_entity`
ahead of the slot-table consolidation, which has not landed; the
"lands with the consolidation" consequence did not hold.

## Context

`EntityPool` recycles an entity without bumping its generation, so a handle
cached before the recycle silently addresses the next acquirer. The
question was whether to bump on recycle or to write the aliasing down as an
explicit pool contract. Both close the finding on paper.

## Decision

**Bump.** The generation is incremented on release, and the pool stores the
new handle. A handle held across a recycle misses.

This is stated as a general rule, not a fix to one container: **every
generational handle in the engine bumps on release.** No container is
exempt, and a shared slot-table primitive does not offer a no-bump mode.

## Rationale

- It is the invariant the architecture already states: *generation reuse
  must not silently alias a stale handle within the supported lifetime.*
  Writing the aliasing down instead would not have made the design
  coherent — it would have made the document agree with one outlier.
- Every other handle system here already bumps: the device slot table,
  joint handles, asset streaming, and the mesh and texture codecs. Leaving
  the pool as the single place where a stale handle silently works is worse
  than either uniform choice, because a reader cannot know which semantics
  a given handle has without checking its implementation.
- Silent aliasing produces the defect that presents as intermittent and
  unreproducible rather than as a clean failure. Those are the expensive
  ones.
- A no-bump container cannot share the consolidated slot table that the
  core-primitives work introduces, or it forces that primitive to carry a
  mode that weakens it for every other consumer. One exception costs the
  consolidation its value.
- The cost is one increment on release.

## Consequences

- A script or system holding a pooled handle across a recycle now gets a
  miss instead of the wrong entity. That is a behavior change, and it may
  surface latent bugs in content that relied on the aliasing — which is the
  point. It needs a test either way under the tests-are-contracts rule.
- The pool's own bookkeeping stays correct because it stores the new
  handle.
- This lands **with** the slot-table consolidation, not before it: the
  consolidated primitive should own the behavior so there is one
  implementation of it rather than a ninth.
