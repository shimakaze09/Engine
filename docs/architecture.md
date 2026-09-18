# Architecture

The invariants a change must preserve, and what each module is responsible
for. Current behavior lives in the code — this document states the rules
that behavior must satisfy, not what the code presently does. Where the two
disagree, that is a defect in one of them; report it.

## Module dependency graph

```
app → editor → runtime → renderer / physics / scripting / audio → content → core / math
```

The chain states direction, not adjacency: reaching further down
(`editor → renderer`) is legal. Within the bottom tier the direction is
`math → core`; `core → math` is forbidden. The four mid-tier subsystem
modules are siblings and must not include or link each other — they meet
only in `runtime`, which owns the bridges. `content` is the generic asset
layer: it depends only on `core` and must never link a subsystem module.

`tools/check_module_deps.py` enforces this. It validates every first-party
`#include "engine/<module>/..."`, rejects includes of another module's
private `src/` headers, rejects foreign include directories hand-wired via
`*_INCLUDE_DIRS` (a dependency is expressed only as a dep on the target, so
CMake usage requirements stay the single source of truth), and requires a
module whose *public headers* include another's to declare that module as a
PUBLIC dep.

The gate carries an allowlist of tracked violations. **That file is the
authoritative list** — do not restate it here. An entry that no longer
matches anything is itself a finding, so each fix deletes its own entries.
The rule is [CI] for every edge not on the list and [REVIEW] for the listed
ones until it empties.

The visibility check runs one direction only: an under-declared dep is
[CI]; a PUBLIC dep no public header needs stays [REVIEW], because proving a
dep unnecessary means proving no consumer relies on the propagation, which
include edges cannot show.

## Subsystem ownership

Every mutable subsystem has exactly one owner, in four tiers:

- **Engine tier** — `engine::bootstrap` / `engine::shutdown` own core
  services (LIFO, with partial-init rollback), the cvar and console tables,
  the scripting VM, the audio device, DAP, the texture registry, renderer
  teardown, and the active `EngineConfig`. Caller-supplied configuration
  strings are copied into engine-owned storage, staged so a rejected path
  leaves the previously adopted configuration intact — the active
  configuration is read for the whole process lifetime and must not borrow
  an embedder's buffers.
- **Run tier** — `EnginePipeline::Impl` owns the World, the asset
  database/manager/streaming, the command buffer, the mesh registry, the
  service locator and registry, game-binding state, and published bridge
  services. Its `teardown` is the single audit point for run residue: a
  second pipeline run in one process starts clean.
- **World tier** — `World` owns ECS, hierarchy, persistent ids, the physics
  context, timers and cameras.
- **Editor tier** — `EditorSession`, behind the bridge. A world rebind is a
  full session transition.

Compatibility globals are non-owning aliases, set and cleared by their
owner. **No global may lazily resurrect a subsystem**, and there is
deliberately no process-global service locator. Pure caches and immutable
config tables are exempt from de-globalization. The single-engine-process
assumption stands.

## Entities and the World

- Internal `Entity` is `{index, generation}`; index 0 is invalid. Any handle
  that can outlive, cross or be rebound between Worlds also carries and
  validates World identity. Generation reuse must not silently alias a
  stale handle within the supported lifetime; capacity and wrap behavior
  require explicit tests.
- Component mutation is legal only in `WorldPhase::Input`. Writable
  transforms during Simulation require the `SimulationAccessToken`. Never
  break transform double-buffering or persistent-id behavior.
- User-facing objects are created through `create_scene_object` and always
  own a non-removable Transform; `create_entity` is the internal bare-ECS
  escape hatch. Spatial components and children consume the composed world
  matrix.
- Dynamic rigid bodies must be hierarchy roots; descendant colliders form
  one compound body owned by their nearest rigid-body ancestor. Collision
  and queries consume the composed world pose.
- Destroying an entity destroys its whole transform subtree; deferred
  destruction queues the subtree so EndPlay fires for every member.

## Frame

Every fixed step has one explicit dependency chain:

```
begin[n] → all update/physics work → collision resolve → commit[n] → begin[n+1]
```

The happens-before relation must hold for zero jobs, disabled systems,
submission failure, catch-up steps, and every worker count. After the last
commit, transform propagation and camera/spring-arm publication run on the
main thread before any render-prep job; then render-prep jobs fill
per-thread command buffers that are merged for the backend flush.

Preserve deterministic stepping and thread-count independence. Test the
production pipeline, never a copied scheduler model.

Every scene-derived input to one submission — camera, prepared draws,
lights, capture requests — comes from a single World content epoch: a
pending scene op commits in its own stage after the render stage has
submitted the frame.

## Capacity

Fixed-capacity storage is the engine's memory model, so what happens *at*
capacity is part of the contract, not a per-call-site choice. One past
capacity must:

- **Refuse, and report.** Return an explicit failure the caller must
  handle. Never terminate the process, never silently drop the item, never
  truncate an identity, and never log a fallback that did not happen.
- **Be reachable in tests.** Zero, one, many, exactly at capacity, one
  past, and handle reuse across the generation wrap.

A capacity limit that can be met in normal use is a design defect, not a
robustness feature: raise it or make the storage grow at a cold boundary.

## Serialization

One authoritative persistent-component registry generates or mechanically
validates parse, copy, reset, migration and codec coverage; a missing row
fails to compile rather than silently skipping a type. Reflected fields
serialize under a stable wire key, never the C++ member name, so a rename
declares the old key instead of migrating content.

Format changes require migrations and production-path tests. Parse, load or
restore failure leaves the destination unchanged; a scene load stages into
a replacement World and commits only on success.

See the `serialization` skill for the procedure.

## Renderer

- Command construction stays separate from backend execution. Code above
  the backend translation unit speaks only the engine-facing `RenderDevice`
  vocabulary — handles, descriptors, shader params, render state. bgfx
  concepts must not reappear above the backend.
- Device resources are owned by whichever system created them; destruction
  is immediate and idempotent, and device shutdown invalidates every
  outstanding handle. Owners release before the device goes, and a registry
  closed with no live device reports every resource it could not release
  rather than dropping it silently.
- The device's lifetime is its own: a run may never flush, so shutdown
  releases a live device on its cold path as well as its warm one.
- Preserve forward fallback and transparency behavior when touching
  deferred paths. Prefer CPU-verifiable tests; GPU tests carry the `gpu`
  label. **A green test suite is not evidence a renderer change works** —
  see the `verify` skill.

## Physics

Public joint names follow their conventional degrees of freedom,
anchor-frame, limit and motor semantics; a simpler constraint takes a
different public name. Correcting serialized joint behavior requires an
explicit behavior version, a migration policy, and before/after tests.

Physics talks to the world only through `PhysicsWorldView`; shape payloads
live in the World-owned `PhysicsContext`.

## Scripting

Do not break the Lua API without tests and doc updates. Validate stack
usage; preserve traceback, sandbox and hot-reload behavior. Scripting
consumes the engine only through sanctioned bridge APIs — never core or
runtime internals.

Every script-reachable filesystem path is VFS-jailed as defence in depth:
relative, forward slashes, no drive designators, no `..`.

## Shared code

- Private headers in `src/` are the established pattern for module-internal
  APIs. Keep using them; do not move them into `include/`.
- Shared utilities go in `core`. One concept has one implementation: see
  the `consolidate-primitive` skill before writing a second copy of a
  handle table, hash, string copy, ring, file reader or path check.

## Modules

One line each. The code is the detail.

| Module | Responsibility |
| --- | --- |
| `app/` | Editor entry point; whole-archives the editor so its bridge registers before bootstrap. |
| `core/` | Bootstrap/config, platform (SDL glue, paths, native handles), logging, cvars, console, event bus, input and input maps, VFS, JSON, job system, allocators, profiler, reflection, entity handle, service locator, shared primitives. |
| `math/` | Header-only vectors, matrices, quaternions, transforms, bounding volumes, component PODs. |
| `content/` | Generic asset layer: identity, metadata store, dependency edges and ordered load, transition queue, async streaming, staleness diagnostics, LRU cache. |
| `physics/` | Bodies, colliders, convex hull, heightfields, CCD, contact manifolds and solver, joints, queries, materials, primitive hull builders, diagnostics. |
| `renderer/` | Asset database, mesh/texture loading, shader system, the `RenderDevice` contract and its bgfx backend, command buffer frontend and backend, pass resources, shadows, light culling, post stack, GPU profiler. |
| `audio/` | Sound handles, bus groups, one-shot instance pool, 3D listener, streaming music, decode budgets. |
| `scripting/` | Lua runtime and sandbox, DAP debugger, hot reload with state persist, generated bindings, per-domain binding translation units. |
| `runtime/` | Public bootstrap/run/shutdown, the frame pipeline, `World` ECS, scene and prefab serializers, the subsystem bridges, render prep, skeletal animation, render interpolation, save data, timers, cameras, game mode and state, entity pool. |
| `editor/` | ImGui editor: session and play lifecycle, hierarchy, undoable commands, asset index and content browser, panels, inspector metadata and drawers, material editor, live and multi edit, cameras, command history. |
| `assets/` | Shaders and their cook manifest, sample scripts and meshes, the bundled prop pack, sounds, starter templates. |
| `tools/` | Asset packer, binding generator, asset generators, the audit gates and their self-tests, CI helpers. |
| `tests/` | Unit, integration, smoke (`gpu` label), benchmark, plus the shared harness. |
