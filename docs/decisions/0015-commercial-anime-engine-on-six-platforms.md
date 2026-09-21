# 0015 — The product is a commercial anime-game engine: three editor platforms, six shipping targets

**Date:** 2026-09-21. Owner decision, recorded during the asset-identity
foundation work. Supersedes the editor-target and shipping clauses of
[0002](0002-macos-is-a-test-lane.md); amends
[0004](0004-engine-first.md); rewrites the goal, platform and parking-lot
sections of `docs/vision.md`.

## Context

The vision stated a generic goal — Unreal-level rendering with Unity-level
ease — and listed Windows and Linux as the editor platforms, web export as
the headline differentiator, iOS as a runtime proof, and macOS shipping
and Android as undecided. It parked particles, a sequencer, navigation,
root motion and retargeting.

The owner's product is narrower and larger at once: a commercial engine
specialised for anime-style games, with an editor on Windows, Linux and
macOS, and games shipping to those three plus Android, iOS and the web.
Several parked items are not optional for that genre, and several
genre-defining capabilities — stylised shading, facial morphs, secondary
bone motion, runtime text in CJK scripts — had never been written down
anywhere, so nothing in the tree was optimising for them. At the time of
this record the tracker held sixty open items and not one was a feature.

## Decision

1. **The product is a commercial engine for anime-style games.** The two
   co-equal halves stand — rendering depth and ease of use — and the
   rendering half is now judged by how well it renders that style, not by
   photorealism alone.
2. **Editor platforms: Windows, Linux, macOS.** 0002's "no macOS editor
   target" no longer holds. Its other clause stands: AppleClang remains
   the conformance gate for the language feature set.
3. **Shipping targets: Windows, Linux, macOS, Android, iOS, Web.** The
   Android and iOS proofs are gated on hardware the project does not yet
   own; that gate is recorded on their epic, never silently assumed away.
4. **Genre-required capabilities are v1 scope**, not parking lot:
   stylised shading models, morph targets, secondary bone motion, VRM and
   FBX import, a runtime UI with CJK text and IME, a sequencer, particles,
   dialogue and localisation, navigation, root motion and retargeting,
   splines and data tables.
5. **Still parked:** networking and multiplayer, a 2D engine, CSG, XR,
   haptics and input replay, user-authored shaders, a plugin system, and
   advanced photorealistic rendering (lightmap baking, SSR, volumetrics).
6. **Runtime UI is a shipping requirement**, which amends 0004: it is no
   longer template-driven feature work to be suspended. 0004's core
   stands — the engine is the product and the templates are fixtures.
7. **Sequencing stays content-before-replatform.** The creation loop and
   the stylised renderer are proven on the desktop editor platforms
   before the mobile runtimes are attempted.

## Consequences

- The tracker gains feature epics. A capability in point 4 may be
  scheduled without a slice demo demanding it; a capability in point 5
  still needs an owner decision to leave the parking lot.
- `docs/vision.md` replaces the "Runs everywhere" slice with "Ships",
  adds "Looks like anime", and replaces its platform paragraph.
- 0002's macOS lane keeps running as is until a macOS editor exists;
  nothing in the CI matrix changes with this record.
