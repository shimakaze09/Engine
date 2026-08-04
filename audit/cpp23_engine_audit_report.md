# C++23 Game Engine Audit Report

Audit date: 2026-08-01  
Repository: D:/dev/Engine  
Revision inspected: 5c9e99aca10c8bb0843415c9979b0028eee4b4b3d  
Method: static, read-only audit following the supplied C++23 Game Engine Audit Checklist and the cpp-project-review skill.

> **Status (2026-08-04):** historical record. The findings below were reconciled and closed by the 2026-08 campaign — see the resolution addendum in `audit/audit_2026-08-03_followup.md` for the finding→PR ledger. Note that PR #50's closure table misnumbered two rows: its "H-07"/"H-08" entries were actually H-02/H-01; the real H-07/H-08 were re-opened as issues #53/#54 and closed by PR #74. Remaining open scope lives in the issue tracker (#78–#86), which is the source of truth going forward.

## Review Scope

The audit proceeded in the requested order: inventory and build contract; ownership and lifetime; undefined behavior; concurrency and frame ordering; persistence and file I/O; physics; renderer; runtime; scripting; editor; tools; tests; portability; performance; then maintainability.

The repository contained 709 tracked files. The C/C++ surface was 426 files and 90,498 physical lines:

| Area | C/C++ files | Physical lines |
|---|---:|---:|
| app | 1 | 11 |
| audio | 2 | 536 |
| core | 48 | 7,854 |
| editor | 23 | 4,451 |
| math | 11 | 974 |
| physics | 37 | 6,509 |
| renderer | 67 | 14,414 |
| runtime | 55 | 12,432 |
| scripting | 59 | 8,849 |
| tools | 16 | 3,440 |
| tests and test support | 107 | 31,028 |
| Total | 426 | 90,498 |

All 319 non-test C/C++ files were inspected function by function and line by line. The complete test registration surface was reconciled against all 107 test C/C++ files, every test routine and exercised API was mapped, and defect-adjacent tests were inspected in full. All first-party CMake, CI, PowerShell, Python, Lua, GLSL, JSON, Markdown, and configuration files were inspected. Binary assets were not decompiled; their sizes, references, metadata, buffer declarations, and loader contracts were checked. Every glTF external buffer exists and is at least its declared length. All JSON parses except the deliberately corrupt test fixture.

No source, build, test, or asset file was changed. No build or test was run because both would create files outside the user-authorized audit directory. Existing build artifacts show a prior Debug Ninja/clang-cl build, while the newest discovered CTest result is older and cannot establish the current revision's status.

Third-party source was treated as an external dependency boundary and was not re-audited. Findings concern first-party code and the way it configures or calls dependencies.

Severity meanings:

- Critical: credible data loss, memory-safety failure, cross-world identity corruption, or a core scheduling race; release blocker.
- High: major correctness, security-boundary, portability, or resource-lifetime defect; fix before release.
- Medium: material robustness, determinism, API-contract, feature-correctness, or diagnosability defect.
- Low: maintainability, efficiency, tooling, or documentation weakness with limited immediate runtime impact.

## Executive Summary

Final assessment: not release-ready.

The architecture has strong foundations: fixed-capacity component storage, explicit world phases, persistent IDs, centralized mutation bridges, a substantial automated test inventory, pinned fetched dependencies, sanitizer and coverage options, and unusually broad subsystem separation. Those strengths do not offset five confirmed release blockers:

1. Scene loading silently drops AnimationComponent, including one present in the shipped Island Hopper scene.
2. Catch-up fixed steps can run begin_update_step concurrently with update/physics jobs.
3. Retained Lua entity handles can become valid for unrelated entities after a world replacement.
4. editor_set_world can retain Play-in-Editor state and later restore world A's snapshot into world B.
5. Authored scene, prefab, metadata, and tool outputs use truncating, non-atomic writes.

The report contains 5 Critical, 22 High, 29 Medium, and 8 Low finding groups. Several groups contain closely related defects sharing one remediation boundary.

## Critical Findings

### C-01 — Scene commit silently discards AnimationComponent

Evidence:

- runtime/src/scene_serializer.cpp:602-615 parses and adds AnimationComponent to the temporary world.
- copy_world_contents at runtime/src/scene_serializer.cpp:651-705 copies many component stores but omits AnimationComponent.
- The commit at runtime/src/scene_serializer.cpp:1111-1139 replaces the destination from that incomplete copy, and its invariant checks do not cover animation.
- assets/scene.json contains an AnimationComponent at approximately line 1879. assets/scene.json and the packaged template are byte-identical.
- Editor Play/Stop snapshots use the same serializer.

Impact: a successful default scene load destroys valid authored animation data. The shipped scene demonstrates the loss without a synthetic malformed input.

Fix: make commit a complete World move/swap operation or centralize component enumeration so serialization, validation, copy, reset, and tests cannot diverge. Add post-commit equality checks for every persistent component type.

Required regression: load-save-load the shipped scene and compare all component sets and values; enter/stop Play mode and compare the editor world byte-for-byte at the serialization model level.

### C-02 — Fixed-step catch-up graph contains a real data race

Evidence:

- runtime/src/engine_pipeline.cpp:900-915 makes a later step's beginStepHandle depend on the previous commit.
- Update jobs at runtime/src/engine_pipeline.cpp:947-951 also depend on the previous commit, but do not depend on beginStepHandle.
- Physics is transitively released from the same branch.

After the prior commit completes, begin_update_step and the next update/physics work may execute concurrently. World phase and snapshot preparation can therefore race with readers and writers.

Impact: undefined behavior, inconsistent snapshots, phase-check failures, and nondeterministic physics/script corruption during frames with more than one fixed step.

Fix: make every per-step job depend on that step's beginStepHandle. Prefer a single explicit DAG builder whose invariants assert begin -> update groups -> physics -> commit for each step and commit[n] -> begin[n+1].

Required regression: a deterministic job-system test with at least three catch-up steps, barriers inserted into begin/update/commit, and ThreadSanitizer coverage on a supported platform.

### C-03 — Stale script handles can alias entities in a replacement world

Evidence:

- scripting/src/entity_handle.cpp:17-33 encodes only entity index and generation.
- Scene commit replaces World storage at runtime/src/scene_serializer.cpp:1134 with a freshly constructed world's generation table.
- The Lua VM and several scripting globals intentionally survive scene transitions.

A handle retained by Lua from the old world can match a new entity created at the same index with the same reset generation. Generation checking then approves the wrong entity.

Impact: scripts can read, mutate, or destroy unrelated objects in a newly loaded scene. This defeats the advertised stale-handle safety boundary.

Fix: include a monotonically increasing world epoch in every externally retained handle, or invalidate/rebind all script-visible handles and registries before replacement. Never reset the epoch when assigning World contents.

Required regression: retain a Lua handle, replace the world with one that reuses the index/generation, and prove all accessors reject it.

### C-04 — Switching editor worlds can restore a snapshot into the wrong world

Evidence:

- editor/src/editor.cpp:348-360 clears history and gizmo state on a non-null A-to-B switch but preserves play state and the Play snapshot; those are reset only when world is null.
- editor/src/editor_session.cpp:318-340 restores the stored snapshot into the editor's current world when stopping.
- Selection identity is also index-only.

Impact: switching from world A to B while Playing/Paused and then stopping can overwrite B with A's snapshot. This is direct editor data loss and cross-world identity corruption.

Fix: make set_world an explicit session transition. Stop/abort PIE against the old world before rebinding, discard snapshots, clear all selection state, reset simulation state, and tag snapshots with an immutable world identity that must match on restore.

Required regression: start PIE in A, switch to B, stop, and verify neither world changes unexpectedly; repeat with selected entities and a failed restore.

### C-05 — Authored-file saves are truncating and non-transactional

Evidence:

- Scene, prefab, VFS-backed saves, import metadata rewrites, cook stamps, dependency graphs, and generated asset outputs open final destinations with wb or equivalent direct replacement.
- Several paths do not propagate flush or close failure.
- Multi-output imports have no transaction or rollback.

Impact: process termination, disk-full conditions, encoder failure, or a later multi-file failure can destroy the last valid scene/prefab/metadata/output and leave mismatched artifact sets.

Fix: write a sibling temporary file, flush, check close, optionally fsync where durability is promised, validate the completed payload, then atomically replace. For multi-file imports, stage all outputs and commit a manifest transaction last.

Required regression: fault-inject every write/flush/close/rename point and prove the previous valid artifact remains intact.

## High Findings

### H-01 — Persistence coverage is incomplete beyond the animation blocker

Scene persistence does not preserve custom or HullSource::None convex-hull payloads, heightfield sample data, joints, or world gravity. Built-in cylinder and pyramid hull provenance can be rebuilt and should not be reported as universally lost. Prefab persistence omits SpringArmComponent. runtime/src/serialization_util.cpp:221-240 shows the limited provenance reconstruction.

Fix: version and serialize every stateful component/resource, or explicitly reject save with a precise unsupported-component error. Add an exhaustive component registry test that fails when a persistent component lacks a codec.

### H-02 — EntityPool recycle cleanup is incomplete and ordered unsafely

Recycling removes only a subset of components, marks the slot free before all cleanup has succeeded, and performs bookkeeping under phase-sensitive World APIs. Orphaned components and failed cleanup can leak state into a reused entity.

Fix: destroy through one authoritative World teardown routine, clean all stores and external managers before publishing the slot free, and treat partial cleanup as an invariant failure.

### H-03 — Ray slab intersection performs C++ undefined behavior

math/include/engine/math/ray.h:25-31 takes the address of one scalar member and indexes across adjacent members as though the object were an array. Distinct data members are not an array object. CMakeLists.txt around line 221 suppresses cppcheck's matching objectIndex warning.

Fix: use explicit per-axis access, std::array storage, or a safe vector index operator. Remove the suppression and add zero-direction, signed-zero, infinity, and boundary tests.

### H-04 — Heightfield narrow phase can convert negative floats to size_t

physics/src/narrow_phase.cpp:516-523 clamps cMax/rMax only at the upper bound in the non-affine path. A collider wholly on the negative side of a grid, especially one whose declared extents disagree with payload dimensions, can convert a negative float to size_t and drive huge or out-of-bounds loops. The affine path correctly clamps both ends.

Fix: validate the payload/extent contract, clamp in signed space before conversion, use checked integer conversions, and reject non-finite coordinates.

### H-05 — Joint solvers do not implement their declared constraints

Hinge angle error is derived from distance units, fixed joints do not constrain relative rotation, local anchors are not consistently rotated into world space, and slider rotational/anchor semantics are incomplete.

Impact: unstable or physically incorrect constraints despite APIs suggesting production hinge/fixed/slider behavior.

Fix: define each Jacobian and effective mass from a documented constraint model, warm start consistently, and verify against analytical single-joint and articulated-chain cases.

### H-06 — Physics accepts unbounded and non-finite control values

Solver iteration CVars are unbounded and can hang a frame. Collider dimensions, material parameters, masses, transforms, damping, velocities, and restitution/friction paths lack consistent finite/non-negative validation. sqrt and normalization then propagate NaN or inject energy.

Fix: validate at every public ingress, clamp tunables to documented safe ranges, and make invalid state observable rather than silently defaulting.

### H-07 — Broad phase and CCD can lose or misidentify collisions

The grid converts large/non-finite floats to int32, integer cell loops can overflow, and a fixed per-collider node cap silently drops coverage. Speculative pair generation is order-dependent. CCD snapshots rely on dense-array position rather than stable entity identity and combine/resolve speed inconsistently.

Fix: checked coordinate quantization, overflow-safe iteration, explicit overflow reporting/fallback, stable entity-keyed snapshots, and deterministic pair ordering.

### H-08 — Contact generation and impulse response contain major correctness gaps

Coincident spheres receive no useful separation direction; sphere-inside-AABB contact normal/penetration is wrong; heightfield overlap can degrade to AABB-only; sweeps use target AABBs and produce false positives; angular effective mass/inertia terms are incomplete; a fixed callback buffer silently drops events.

Fix: correct inside/degenerate cases, implement shape-accurate queries or rename APIs to bounds queries, validate solver equations, and expose overflow rather than dropping callbacks.

### H-09 — Shader hot reload leaves backend program IDs and uniform caches stale

Reload swaps/destroys GL programs, while BackendState retains raw program IDs and cached uniform locations. Later draws can use deleted programs or locations from the old link.

Fix: represent programs with generation-aware handles and notify every dependent cache on successful relink. Swap only after complete compilation/link/reflection succeeds.

### H-10 — Public light counts can index fixed arrays out of bounds

Shadow command generation iterates raw SceneLightData spotLightCount and pointLightCount in command_buffer_flush_shadows.cpp around lines 201 and 306. The arrays are fixed capacity, and these loops do not clamp the public counts as other paths do.

Fix: make counts construction-safe, clamp once at API ingress, and reject inconsistent structures in debug and release builds.

### H-11 — CpuMeshData can cause host over-read and GPU out-of-bounds access

upload_mesh_data_to_gpu validates pointers, counts, and index range but not that vertexFloatCount equals vertexCount times stride or that layout/stride is internally consistent. It can request a GPU upload beyond the caller's allocation. Skin joint indices are not guaranteed to fit BonePalette.

Fix: use spans with sizes, checked multiplication, exact layout contracts, per-attribute range validation, and bounded joint indices/normalized weights.

### H-12 — Render-target initialization and failure handling are unsafe

Depth-only shadow FBOs do not consistently set draw/read buffers to GL_NONE or check completeness. Multiple pass init/resize return values are ignored; dimensions can be recorded after failure, preventing retry. Shutdown without a current GL context skips deletion but resets ownership handles.

Fix: transactional resource creation with framebuffer completeness checks, retain old valid resources on resize failure, propagate init failure, and require context-bound destruction via an explicit render-device lifetime.

### H-13 — The declared macOS build cannot create the requested GL context

core/src/platform.cpp:203-205 requests OpenGL 4.5 core on every desktop OS. macOS exposes at most the deprecated 4.1 core profile. default.frag declares GLSL 450 and is required by renderer startup. The macOS CI lane compiles but excludes GPU execution, so it cannot detect this.

Fix: either lower the renderer contract to macOS-supported GL/GLSL with feature fallbacks, or remove macOS from supported runtime targets and state build-only status.

### H-14 — noexcept construction paths can terminate and leak partial state

JobSystem and AssetStreaming construct std::thread objects inside noexcept initialization. Thread construction may throw; partial construction has no rollback before std::terminate. Many noexcept functions also grow vector/string/filesystem state, making allocation failure an unconditional termination policy that is neither documented nor consistently designed.

Fix: move throwing work behind a catching transaction, use RAII temporary owners, publish initialized state only after success, and document the no-exception/OOM contract.

### H-15 — The Lua sandbox is not a file-system security boundary

Although unsafe standard libraries are restricted, Engine bindings expose scene/prefab load/save, module require, persistence, and path-based asset operations. Scripts can therefore cause host/VFS file effects through trusted engine APIs.

Fix: define the threat model. If untrusted mods are supported, provide capability-scoped virtual roots and deny authored-file mutation by default. If scripts are trusted, stop describing the environment as a security sandbox.

### H-16 — Scripting resources and managers survive scene transitions incorrectly

Scene async-load handles use a fixed 1024-entry table with no reclamation. Failed editor loads can remain Loading. Timer, player-controller, and other global managers can retain old-world identity or Lua references across replacement. Scene paths are stored in fixed buffers and may truncate.

Fix: use generation handles with release, bind all managers to a world epoch, perform a single scene-transition reset protocol, and reject rather than truncate paths.

### H-17 — Animation data can trigger unbounded allocation or an infinite update loop

animation_assets.cpp:115-135 trusts binary counts/offset arithmetic sufficiently to request unbounded vector sizes in noexcept code. animation_system.cpp:396-400 wraps time with a repeated subtraction loop; infinite/non-finite time, extreme speed, or malformed duration can hang forever.

Fix: impose file-size-derived count limits and checked arithmetic; validate finite positive durations/speeds; wrap with fmod and a bounded negative-time policy.

### H-18 — reset_world can leave a half-reset runtime

Entity destruction/allocation may fail because reset uses phase-sensitive APIs, yet non-entity managers and services are cleared independently. Animation controller resets can leave stale controllerSlot fields in components.

Fix: provide an internal, phase-independent destructive reset with no per-entity failure mode, then reset dependent managers in a declared order and assert no surviving handles.

### H-19 — Asset packer trusts incompatible glTF selections and layouts

A selected mesh with zero primitives is indexed at [0] after only mesh-zero validation. Skin zero and the selected primitive are combined regardless of actual node/skin association. Primitive mode is not required to be triangles. Count multiplication, normals, weights, finite values, and animation layouts are incompletely validated.

Fix: validate the selected node/mesh/primitive/skin graph as one unit; use checked sizes; require supported modes/layouts; fail with precise source locations.

### H-20 — Cooking keys and output commits are insufficiently reproducible

External .bin payloads are absent from discovered cook-stamp dependency digests despite comments implying dependency hashing. Importer/tool version and platform are not in the key. generateNormals and upAxis are accepted, hashed, and written but not applied. Duplicate sanitized clip names overwrite. Multiple outputs commit independently.

Fix: hash the full transitive byte inputs plus normalized settings, importer schema/version, and relevant platform ABI; stage all outputs; reject name collisions.

### H-21 — Editor import-settings save destroys metadata fields

editor/src/editor_panels_assets.cpp:124-127 and 277-297 rewrite .meta.json as a small stub using wb. Existing schema, source/output mapping, and unknown forward-compatible fields are discarded.

Fix: parse-update-preserve the complete document, validate it, and atomically replace. Add round-trip tests containing unknown keys and all current schema fields.

### H-22 — Audio initialization and enum indexing can corrupt state

Bus creation is chained and guarded by one busesReady flag, so partial success leaks earlier buses. Public bus enum values index arrays without robust validation. This is an out-of-bounds risk at the API boundary.

Fix: validate enums before indexing and create buses into temporary RAII owners with rollback on any failure.

## Medium Findings

### M-01 — Simulation cadence is internally inconsistent

Scripts, timers, and coroutines advance once per rendered frame when any fixed steps occur, while camera/spring-arm work uses a fixed 1/60 value rather than each catch-up step's time. Camera work occurs after render preparation, leaving culling/render data one update stale.

Fix: declare per-frame versus per-fixed-step systems, pass the real step delta, and build the graph from that declaration.

### M-02 — Spring-arm behavior is incomplete and transform math is inconsistent

runtime/src/spring_arm_update.cpp explicitly leaves collision unimplemented. Local/world offset calculations do not consistently account for rotation and scale.

Fix: either label it a non-colliding offset component or implement a shape sweep with stable lag and correct parent-space transforms.

### M-03 — Several render paths omit skinning

Forward, transparent, scene-capture, and point-shadow paths do not apply the skeletal deformation available elsewhere. The repository documentation acknowledges some of these cuts, but the component/API surface does not prevent affected assets from using them.

Fix: implement shared skinning inputs across passes or reject/reroute skinned draws with an explicit diagnostic.

### M-04 — Exposure and post-processing behavior does not match the feature names

Auto exposure does not read scene luminance; its target is effectively the current exposure. Bloom composition overwrites rather than clearly performing additive energy-preserving composition, and stale mip/resource states can survive partial failures. std::clamp is called without guaranteeing min <= max.

Fix: compute exposure from the luminance chain, validate configuration order, and regression-test known HDR inputs.

### M-05 — GL state and offscreen bake ownership are leaky

IBL baking and several helper passes do not fully restore all modified GL state. Some success flags can be set with zero underlying resources. FBO completeness is not consistently checked.

Fix: a scoped state guard plus transactional pass-resource objects; test state before and after each public render utility.

### M-06 — GPU profiling and tile arithmetic are fragile

The query ring can overwrite unresolved frames. Tile/count arithmetic can overflow before allocation or dispatch. Cull/texture setup failures are often logged or ignored without suppressing dependent work.

Fix: use checked arithmetic, per-frame query ownership with availability back-pressure, and failure-propagating command construction.

### M-07 — Synchronous asset wait can deadlock

AssetStreaming wait_for_load requires some external caller to pump update. A main-thread caller that waits without that pump can deadlock despite a synchronous-looking API.

Fix: make wait perform the required progress safely, or split it into poll and explicitly named wait_while_pumping APIs with thread assertions.

### M-08 — Debug drawing is updated twice and only partly rendered

Debug-draw lifetime ticking occurs in more than one frame path. Spheres and text are collected but not rendered.

Fix: assign one owner for ticking and expose capability/status for unsupported primitives.

### M-09 — Shader math has unguarded degenerate inputs

Deferred point lighting divides by distance/radius, cascade math assumes valid split distances, point shadows assume a positive far plane, and SSAO normalization assumes a nonzero denominator. Public values are not consistently validated, so NaN can reach comparisons and sorting; a NaN comparator violates strict weak ordering.

Fix: validate light/camera/shadow inputs at submission and guard degenerate shader denominators.

### M-10 — CVar parsing accepts malformed configuration

Invalid boolean text can become false with success. Integer/float parsing permits trailing garbage, range overflow, and non-finite values. The bootstrap lifecycle does not clearly initialize the CVar system before all users.

Fix: from_chars-style full-token parsing, range/finite checks, explicit initialization, and error diagnostics containing the variable name and input.

### M-11 — Input/event edge cases violate contracts

Input-map load destroys current mappings before the replacement is fully validated, enum/name inputs are weakly checked, recursive event dispatch relies on an assertion-only cap, touch lifecycle is not integrated with general input startup/shutdown, and mouse emulation hard-codes 1920x1080.

Fix: transactional load, release-mode recursion enforcement, validated enums/names, and live drawable dimensions.

### M-12 — Job wait has surprising global and TLS side effects

core/src/job_system.cpp:258-305 can drain unrelated pending jobs while waiting and overwrites the calling thread's TLS worker index.

Fix: preserve/restore TLS and provide scoped helping restricted to the target dependency tree or clearly document global helping semantics.

### M-13 — Engine statistics are not thread-safe as documented

The stats API copies plain global state while producers can update it. The header advertises thread safety without synchronization or an atomic snapshot scheme.

Fix: single-writer published snapshots, a mutex, or atomics with a documented consistency model.

### M-14 — Service replacement is not stack-safe

Runtime/scripting binding overwrites ServiceRegistry entries and unbind removes them instead of restoring the prior provider.

Fix: scoped registration tokens that restore the displaced service or reject duplicate ownership.

### M-15 — Some mutation paths bypass World phase and payload invariants

Heightfield/hull payload setters and selected physics/scripting operations bypass the central deferred-mutation policy. Replacing a collider type does not reliably clear incompatible old payload.

Fix: one mutation gateway per world, with atomic component-plus-payload replacement and phase assertions in release behavior.

### M-16 — Timer restore cannot reliably reconstruct callbacks

Serialized timers restore Lua registry references that are VM-local; a fresh VM cannot resolve them, and permissive restore can leave null callbacks indefinitely. Non-finite intervals/delays are insufficiently rejected.

Fix: persist stable callback names/module IDs, resolve during load transaction, and fail or quarantine unresolved timers.

### M-17 — Lua persistence formats silently narrow or skip data

Save accepts longer keys/strings than load's fixed 127/255-byte buffers, so round-trip can silently skip/truncate entries. Numeric values narrow to float.

Fix: a length-prefixed bounded format with symmetric limits, versioning, full error propagation, and double preservation where Lua-number fidelity is promised.

### M-18 — Deferred mutation and kill_all outcomes are inaccurate

The global deferred queue is not thread-safe and silently ignores several apply failures. scripting/src/cheat_bindings.cpp:94-101 destroys entities directly while World::for_each_alive stops when visited reaches the shrinking alive count, so kill_all can process only part of the world and still reports each request as destroyed.

Fix: snapshot handles first, route every request through apply_or_queue, and count only confirmed success.

### M-19 — Script lifecycle failure becomes sticky

Entities can be marked begin-play complete before module loading succeeds; a transient initial error then prevents retry. Hot-reload rollback restores only shallow globals, not nested table mutations or engine-side effects. The fixed module cache has no eviction.

Fix: commit lifecycle state only after successful module initialization and use an isolated candidate environment or restartable VM transaction for reload.

### M-20 — Sandbox allocator accounting and limit semantics are inconsistent

The custom allocator is installed after initial VM allocations, so accounting does not represent total VM memory. Addition can overflow. Public limit zero is described as unlimited, while the implementation can reject all growth.

Fix: install accounting at VM creation, checked arithmetic, explicit unlimited handling, and tests for shrink/grow/realloc/zero.

### M-21 — Script bindings accept invalid physics/render/input values

Bindings often accept NaN, negative sizes, invalid enum integers, truncated paths, and out-of-range controller counts. Unknown collider shapes can default to AABB; add/set failures are frequently ignored. Angular velocity does not consistently wake bodies, and unlocking rotation restores a made-up inverse inertia of one.

Fix: shared typed validators, exact enum parsing, error-return conventions, and restoration of stored physical properties.

### M-22 — Clone and player-controller operations are non-transactional

Entity cloning copies only a subset of components/payloads and ignores partial failure. Controller ownership can be cleared before queued destruction is known to succeed.

Fix: schema-driven clone with rollback and commit controller state only after confirmed entity mutation.

### M-23 — Editor selection and history are not generation/world safe

Selection is primarily index-based, so index reuse can select a different entity. selectedEntityIndex is cleared in paths that leave selectedEntityCount stale. Undo/redo remains available during play or invalid phases; void commands are recorded even if mutation failed.

Fix: store full entity handles plus world epoch, centralize selection reset, and make commands return a committed result before entering history.

### M-24 — Inspector edits bypass validation and undo

Several panels mutate World/component data directly, including quaternion/physics values. Animation controller path changes can leave a stale controllerSlot. Changing to hull/heightfield can create a collider without required payload. Shared static material path buffers can cross-contaminate selections.

Fix: route all edits through validated commands with per-entity edit state and atomic component/payload updates.

### M-25 — Editor traversal is cycle-prone and recursion-heavy

Asset browsing uses directory_entry::is_directory, which follows directory links; recursive browsing has no visited-set and can loop through symlink cycles. Subtree editor operations lack a robust cycle/depth guard.

Fix: do not follow directory symlinks by default, track canonical visited directories, and use iterative bounded hierarchy traversal.

### M-26 — Skeleton/animation import loses transform semantics

Negative scale/shear and non-joint intermediary transforms can be lost. Animation times and channel counts are not comprehensively validated; rootJoint and output offsets can index invalid data.

Fix: preserve or explicitly reject unsupported transform decompositions, flatten intermediaries mathematically, and validate every accessor/count/range before allocation.

### M-27 — Generator and quality-gate scripts can report false success

Generators write multiple outputs directly with no rollback. Duplicate Lua names can create uncompilable bindings, and annotation parameter identifiers are trusted. check_coverage_threshold.py accepts NaN because comparisons with NaN are false; the performance gate has the same issue and does not require positive finite baselines.

Fix: finite numeric parsing, schema/identifier validation, staged output, duplicate detection, and self-tests for malformed reports.

### M-28 — Asset database/cache limits fail silently or hash too little

Fixed-capacity dependency/material/texture/module tables often drop or conflate overflow. Thumbnail hashes omit settings, primitive, and scale. File hashing and close/read results are not always propagated. Path storage uses fixed buffers and truncation-prone APIs.

Fix: include all semantic inputs in keys, expose capacity errors, reject truncation, and use streaming reads with checked completion.

### M-29 — Audio control semantics are misleading

Setting master volume does not update the stored master-volume value. stop_all affects registered sources but not all one-shots/music despite its name. Volume, pitch, position, and direction lack finite/range validation. Music uses physical paths rather than the VFS/archive abstraction.

Fix: define and enforce one audio ownership model, validate inputs, make stop_all literal or rename it, and route asset access through VFS.

## Low Findings

### L-01 — Hierarchy operations are unnecessarily quadratic

Repeated parent scans and recursive subtree work can become O(depth times entity count) or O(N squared). Maintain child adjacency or cache traversal order.

### L-02 — Dependency graph order is nondeterministic

unordered storage influences traversal/output, while a zero topological count conflates an empty graph, cycle, and capacity/error case. Use stable ordering and a result type with explicit failure.

### L-03 — Tracked metadata contains stale absolute developer paths

Twenty-one .meta.json files contain D:/dev/Engine/.claude/worktrees/interesting-shirley-14c0f0/... paths. Current runtime lookup does not appear to consume them, but they leak local layout and undermine reproducible metadata.

Fix: repository-relative normalized source paths and a metadata scrub check in CI.

### L-04 — Documentation overstates production hardening

README/CLAUDE wording presents closed hardening work more strongly than the implementation supports, particularly for persistence, macOS runtime support, skinning paths, exposure, and spring-arm collision.

Fix: distinguish implemented, tested, known-cut, and planned capability tables.

### L-05 — Performance benchmarks are statistically weak

Several gates use a single measurement/sample and broad fixed baselines. This is noise-sensitive and may miss regressions.

Fix: warmup, repeated samples, robust percentile/median statistics, and environment recording.

### L-06 — Packaging and version contracts are incomplete

There are no CMake presets, install/export/package rules, dependency license inventory, or a clear minimum compiler/driver matrix. System SDL may resolve to a different version than the pinned fetched fallback.

### L-07 — Fixed UI/path buffers reduce diagnosability

Thumbnail, material, scene, and console paths commonly use fixed arrays; truncation is sometimes silent. Prefer length-aware values at boundaries and explicit error messages.

### L-08 — Analysis policy leaves blind spots

Static-analysis helpers exclude tests/tools, cppcheck is configured for C++20 despite a C++23 project, and the exact ray object-index warning is suppressed. Clang-tidy enables only a small check set and command-only compile database sanitation misses entries using arguments.

## Informational Findings

Positive controls worth preserving:

- World phases and centralized mutation intent are explicit and make many illegal transitions diagnosable.
- Fixed-capacity ECS storage limits uncontrolled runtime allocation in core loops.
- Persistent IDs exist for serialized identity.
- Scene load generally follows parse-validate-commit rather than mutating the destination incrementally; the commit's incomplete component coverage is the defect.
- Dependency fetches are pinned when the fallback path is used.
- CI exposes warning-as-error, sanitizer, static-analysis, coverage, and performance options.
- The test tree is broad: 107 C/C++ test/support files and 95 tests in the latest discovered CTest metadata.
- JSON fixtures and shipped scenes are generally structurally valid; the corrupt fixture is intentionally invalid.
- glTF external buffer relationships checked during this audit are present and satisfy declared byte lengths.
- Built-in cylinder/pyramid hull provenance is reconstructable; the persistence gap is specifically custom/no-source hull data and heightfields.

## Build and Tooling

### Build contract

- The project requests C++23 and disables compiler extensions.
- Warnings-as-errors and strict warning sets are available.
- Exceptions are disabled, but the codebase still uses many standard-library operations that can throw inside noexcept functions. Treating OOM/thread-construction failure as process termination must be explicit if retained.
- OpenGL 4.5/GLSL 450 is a hidden runtime floor and conflicts with the macOS lane.
- Mobile/Web platform branches are stubs, not functional ports.
- copy_assets is an ALL target and copies the full tree on each build, increasing iteration cost.

### Static analysis

- cppcheck runs as C++20, excludes tests/tools, and suppresses a confirmed objectIndex UB finding in ray.h.
- Clang-tidy enables a narrow check set. The helper sanitizes PCH flags only for compile_commands entries with command, not arguments.
- Header-filter/path behavior should be verified against absolute Windows paths.
- No repository-wide fuzzing, MSan lane, shader validator, serialization schema checker, or binary parser corpus was found.

### CI

- Windows/Linux/macOS build coverage is useful, but macOS does not execute GPU startup.
- Sanitizer labels exclude integration tests, reducing coverage of the most cross-subsystem code.
- Coverage is line-only with a 50 percent threshold and removes tests/tools; there is no branch or changed-line requirement.
- The quality-gate scripts must reject NaN/infinite inputs before their results can be trusted.

### Reproducibility

- Fetched dependencies are pinned; system dependency paths are not equivalently version-constrained.
- Cook keys omit important transitive inputs/tool identity.
- Absolute paths in tracked metadata and missing package/install rules weaken reproducibility.

## Tests

### What is covered well

The test suite spans math, ECS/world behavior, serialization corruption cases, runtime phases, physics shapes/solver/CCD, renderer command behavior, scripting bindings/lifecycle/hot reload/persistence, editor commands/PIE, VFS/assets, audio, tools, and performance smoke tests. Every discovered test .cpp file is registered by the test build.

### High-value missing regressions

1. Shipped-scene AnimationComponent preservation through scene load and PIE stop.
2. Exhaustive component/payload codec registry coverage.
3. Three-step catch-up DAG ordering under forced interleavings.
4. Old-world Lua handle rejection after world replacement.
5. editor_set_world while Playing/Paused and restore-failure behavior.
6. Fault-injected atomic scene/prefab/metadata/multi-output saves.
7. Heightfield contact entirely in negative grid coordinates.
8. Ray slab tests that exercise all axes without member-pointer indexing.
9. Shader reload followed by draws from every BackendState cache.
10. Malformed public SceneLightData counts and CpuMeshData strides/joints.
11. Depth-only FBO completeness on real GL implementations.
12. macOS renderer startup or an explicit build-only expectation test.
13. EntityPool reuse after an entity containing every component/payload.
14. Custom convex hull, heightfield, joint, gravity, and SpringArm round trips.
15. Animation files with overflow counts, non-finite duration/time, and huge playback speed.
16. Lua allocator limit zero, overflow, pre-accounting, realloc, and shrink cases.
17. Initial script module-load failure followed by successful retry.
18. kill_all over a mixed player/non-player world.
19. Import metadata preservation with unknown/newer fields.
20. External glTF .bin modification invalidating cook stamps.
21. NaN/infinity rejection in coverage and performance gates.
22. Audio partial-bus initialization rollback and invalid bus enum.

### Test infrastructure risks

- The newest discovered LastTest.log predates the current audit date and does not prove current HEAD passes.
- Custom test harness behavior should remain simple and fail-fast; fixed filenames/manual cleanup make parallel CTest execution risky.
- Sanitizer integration coverage is incomplete.
- Performance checks need repeated sampling.
- Static analyzers should include test and tool code because those parsers and harnesses also process untrusted data and encode expected contracts.

## Prioritized Fix List

### P0 — Release blockers

1. Replace scene component-by-component commit with complete, registry-driven World transfer; restore AnimationComponent immediately.
2. Repair the fixed-step job DAG so beginStep[n] dominates all work in step n.
3. Add a world epoch to every externally retained entity handle and reset all world-bound scripting/editor managers on transition.
4. Make editor world switches terminate/discard the prior PIE session and validate snapshot/world identity.
5. Introduce one atomic file-commit utility and migrate all authored scene, prefab, metadata, dependency, and generated-output writes.

### P1 — Correctness and memory safety

6. Remove ray.h member-pointer indexing and its cppcheck suppression.
7. Harden heightfield signed bounds, payload dimensions, and checked conversions.
8. Make EntityPool teardown exhaustive and transactional.
9. Complete persistence for custom hull/heightfield payloads, joints, gravity, and SpringArm.
10. Correct joint Jacobians/anchors/rotation constraints and add analytical tests.
11. Centralize finite/range validation for all physics state and bound solver CVars.
12. Rework broad-phase overflow/capacity behavior and key CCD snapshots by entity identity.
13. Correct degenerate contact generation and expose callback/pair overflow.
14. Make shader program reload invalidate/rebuild every dependent cache.
15. Validate public light counts and CpuMeshData size/layout/joint contracts.
16. Make framebuffer/pass creation transactional and context-owned.
17. Resolve the macOS GL 4.5/GLSL 450 incompatibility or correct the supported-platform claim.
18. Wrap thread/allocation-prone noexcept initialization in rollback-capable transactions.
19. Define Lua's trust model and capability-restrict file-affecting engine bindings if untrusted scripts are supported.
20. Add generation/release semantics to async load handles and world-bind scripting managers.
21. Bound and validate all animation binary counts/time math.
22. Replace phase-sensitive reset_world with an internal total reset.
23. Harden glTF mesh/primitive/skin/channel validation and checked arithmetic.
24. Make cooking keys transitive and multi-output commits atomic.
25. Preserve full import metadata on edit.
26. Roll back partial audio initialization and validate enum indices.

### P2 — Robustness and feature truthfulness

27. Normalize per-frame/per-step cadence and move camera updates before render preparation.
28. Finish or explicitly narrow SpringArm, skinning-path, auto-exposure, sweep, and heightfield feature contracts.
29. Fix Lua persistence symmetry, timer callback identity, lifecycle retry, and reload isolation.
30. Route all script/editor mutations through validated transactional commands.
31. Make editor selection/history world-and-generation aware.
32. Harden generator inputs, reject NaN gates, and stage generated files.
33. Make asset/database/cache overflow and truncation explicit.
34. Correct audio stop_all/master-volume/VFS semantics.
35. Expand static analysis to C++23 across production, tests, and tools; add fuzz/schema/shader validation.
36. Add the 22 regression categories listed in Tests before considering a release candidate.

### P3 — Maintainability and delivery

37. Replace repeated hierarchy scans with stable adjacency/iterative traversal.
38. Normalize dependency graph ordering and result types.
39. Remove tracked absolute paths and add a metadata scrub.
40. Publish an accurate platform/feature matrix and dependency/license/compiler contract.
41. Stabilize benchmarks and raise coverage toward branch and changed-line policies.

## Final Assessment

This codebase is architecturally promising and has a much broader testing/tooling foundation than a typical small engine. It is nevertheless unsafe to call production-ready at the inspected revision. The first four critical defects violate core identity, scheduling, and editor-data invariants; the fifth makes any save path vulnerable to destructive interruption. The renderer and physics boundaries also accept malformed public data capable of out-of-bounds behavior or non-finite state propagation.

The safest release path is to freeze feature expansion, implement P0 in order, add the corresponding regression tests, then clear the memory-safety and persistence items in P1. Only after a clean build/test/static-analysis/sanitizer run on the repaired revision should the team reassess release readiness.

## Coverage Ledger

The following first-party surfaces were included:

- C/C++: app, audio, core, editor, math, physics, renderer, runtime, scripting, tools/asset_packer, all test/support/benchmark sources, and public/private headers.
- Build: root and nested CMake files, custom analysis targets, test registration, warning/sanitizer/coverage options.
- Automation: GitHub workflow, Linux CI shell, PowerShell helpers, ten Python generator/analysis/gate scripts.
- Runtime content: seven Lua scripts and thirty GLSL vertex/fragment shaders.
- Configuration/docs: clangd, clang-tidy, gitignore, imgui settings, README, CLAUDE, license, character animation controller, performance baseline, fixtures, and suppressions.
- Structured assets: all 27 JSON files parsed; scene identity/parent/component checks performed; all glTF-to-bin references and declared lengths checked; meta/cookstamp/checksum relationships reviewed.
- Binary/media assets: presence, size, references, and consuming-code contracts checked; no claim is made that image/audio/mesh binary payloads received a semantic decoder-level audit.

This is a static audit. Findings are evidence-backed from source and configuration, but runtime-only behavior remains to be confirmed after the user permits build/test outputs outside audit.
