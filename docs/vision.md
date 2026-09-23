# Product vision

Priorities derive from this document. It states direction, not status —
no feature is described as working here. Integration state lives on the
GitHub tracker; decisions and their dates live in `docs/decisions/`.

## The goal

A commercial engine for anime-style games, with two co-equal halves:

**Unreal-level scene rendering with Unity/Godot-level ease of use.**

Both halves are engine qualities. Rendering depth is earned through
correct, scalable foundations and is judged by how well it renders the
target style, not by photorealism alone; ease of use through disciplined
APIs, presets and diagnostics. Neither is pursued by patching around the
other.

Beginner ease comes from strong defaults, presets, templates, validation,
guidance, undo and recovery — never from weaker correctness, hidden
ambiguity or nonstandard semantics. Advanced users must be able to inspect,
profile, override and scale the same systems rather than graduating to a
different architecture.

## Reference engines are a bar, not a blueprint

Understand each mechanism from first principles and improve on it. Never
import a reference engine's known defects for familiarity's sake.

The canonical example: Unity's nondeterministic script execution order is
explicitly rejected. Deterministic stepping, ordered lifecycle dispatch and
registry-defined ordering are invariants here, and any future scheduling
feature must preserve an explicit, deterministic, author-visible order.

## Priorities, in order

1. Correctness and user-data safety.
2. A clear beginner creation loop.
3. Scalable, physically coherent rendering, physics and runtime
   foundations.
4. Measured performance budgets and quality tiers.
5. A commercial-grade editor with built-in blockout and starter content.
6. One-click sharing.

Device reach is delivered through explicit quality tiers and fallbacks, not
by capping the high-end path.

## The engine is the product

The bundled templates and sample content are integration and test
fixtures, not deliverables. They stay in the tree because they exercise the
whole stack at once, and because a creation loop nobody runs is a creation
loop nobody has verified.

Template and script content bugs are fixed only when their root cause is an
engine defect — and then the fix lands in the engine with a
production-path regression, never as a content or script workaround.

## Slices

Each slice is done when its acceptance demo passes, not when its feature
list is exhausted.

**The slice plays.** A small third-person collect-a-thon is built *in the
editor* from the bundled kit, played start to finish with sound, and feels
smooth at 60 Hz simulation. This demo is the engine's own regression test
for the creation loop: run it, and the defects a human hits become the
queue.

**Ships.** A project made in the editor is packaged, without leaving it,
into a build that runs by itself on Windows, Linux and macOS, from a web
link, and on an Android and an iOS device.

**Looks like anime.** A character with a toon shading model, an outline,
a morph-target expression and spring-bone hair is authored in the editor
and confirmed by a dated human observation on real hardware.

**The hour test.** Five external testers with no game-dev background each
produce and share a playable variation of a template in under an hour,
unassisted.

Sequencing: content before replatform. Porting a stabilized renderer is far
cheaper than porting a moving target, and the hour test cannot be validated
until the creation loop exists.

Platforms: the editor targets Windows, Linux and macOS; games are to ship
to those three, Android, iOS and the web. The mobile proofs wait on hardware
the project owns.

## Parking lot

Cut from v1, not canceled: advanced photorealistic rendering (lightmap
baking, SSR, volumetrics), a 2D engine, networking and multiplayer,
foliage painting, CSG, haptics and input replay, XR, a general-purpose
shader language beyond authored shading programs (those left the parking
lot by [0018](decisions/0018-authors-compose-shading.md)), a plugin system.

Parallel lanes stay live throughout: documentation, extended test coverage
(golden-image renderer tests are a goal; none exist yet), and the devops
pipeline.
