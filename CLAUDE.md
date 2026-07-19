# CLAUDE.md — Engine

The single project document: instructions, repository map, conventions, and
roadmap. It replaces the former `AGENTS.md`, `PROJECT_INDEX.md`, `TODO.md`,
and `REVIEW_FINDINGS.md` (their full text and the 2026-07 hardening-campaign
history live in git). Keep this file updated in the same commit as any change
to module structure, build commands, test layout, or roadmap status.

## What this is

C++20 game engine built from scratch: SDL2 window/input, OpenGL renderer
(deferred+forward, PBR/IBL, GLSL 330 core), fixed-capacity ECS (65,536
entities, double-buffered transforms), CPU-deterministic physics, Lua 5.4
scripting, miniaudio, ImGui editor. Goal: production level. Game authors work
through Lua and the editor; engine contributors work in C++ under the rules
below.

Third-party (all SHA-pinned via FetchContent in the root CMakeLists.txt):
SDL2 2.30.11, Lua 5.4.6, ImGui docking + ImGuizmo snapshots, cgltf 1.14
(tools only), stb snapshot, miniaudio 0.11.21, OpenGL 4.5+.

## Hard rules (enforced)

- C++20 only. No exceptions, no RTTI, no `dynamic_cast`/`typeid`
  (`/EHs-c- /GR-`, `_HAS_EXCEPTIONS=0`; `/W4 /WX` or `-Werror`).
- Engine APIs `noexcept`; explicit return values + logged failure paths.
  No silent failure; no process termination for recoverable errors.
- No heap allocation on hot paths (ECS iteration, transform propagation,
  physics stepping, render prep, command buffers, streaming, input, jobs).
  Fixed-size/preallocated storage; no unordered containers, locks, or virtual
  dispatch on hot paths without justification.
- Dependency flow strictly downward, no cycles or sideways deps:
  `app → editor → runtime → renderer/physics/scripting/audio → core/math`.
- Public headers are self-contained and never leak SDL/OpenGL/Lua/ImGui/
  ImGuizmo types. GL stays inside renderer impl; Lua inside scripting impl;
  editor-only behavior stays in `editor/` behind explicit bridges.
- Every file needs a REAL file-level purpose comment, and declarations keep
  concise purpose comments. Both are CI-enforced:
  `tools/check_source_comments.py` (presence) and
  `tools/check_comment_quality.py` (no filler patterns; must stay at zero).
- Changes to math/ECS/physics/renderer/scripting behavior require tests.
  Determinism-sensitive areas (world, serialization, physics, render-prep,
  Lua API) pair changes with determinism tests.
- Test strictness: assert the tested behavior EXACTLY (no loose tolerances on
  the subject under test); never assert wall-clock timing/throughput in
  functional tests — only dedicated `engine_bench_*` tests hold performance
  thresholds (gated against `tests/benchmark/perf_baseline.json`).
- No new third-party dependencies without confirmation; never ones requiring
  exceptions/RTTI in engine code.

## Build / test (Windows, clang-cl + Ninja; build/ dir already configured)

```powershell
cmake --build build --parallel                    # build
ctest --test-dir build --output-on-failure        # all tests
ctest --test-dir build --output-on-failure -LE gpu   # headless-safe subset
ctest --test-dir build --output-on-failure -R engine_unit_
ctest --test-dir build --output-on-failure -R engine_integration_
ctest --test-dir build --output-on-failure -R engine_bench_
python tools/check_source_comments.py             # comment presence audit
python tools/check_comment_quality.py             # comment quality audit
cmake --build build --target analysis             # cppcheck / clang-tidy
```

Reconfigure (only if the cache is broken):

```powershell
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Debug
```

`build/compile_commands.json` is the clangd source of truth (`.clangd` points
at it); never hand-edit it, never commit `build/`. `build-release/` exists for
Release benchmark runs. Linux/macOS: same flow with clang/clang++. CMake
options: `ENGINE_TARGET_PLATFORM` (Win64/Linux/macOS/Android/iOS/Web),
`ENGINE_MAX_ENTITIES` (default 65536), `ENGINE_DETERMINISTIC_FLOATS` (ON:
`/fp:strict` / `-ffp-contract=off`), `ENGINE_SANITIZERS`,
`ENGINE_BUILD_TESTS/TOOLS`. Helper functions live in
`cmake/EngineHelpers.cmake` (module/static, header-only INTERFACE, exe, test).

## Repository map

- `app/` — `engine_editor_app` entry point; whole-archives `engine_editor` so
  the editor bridge registers before bootstrap.
- `core/` — bootstrap/config, platform (SDL glue, paths), logging, cvars,
  console, event bus, input + input maps + touch, VFS, JSON, job system
  (frame graph), allocators (linear/pool), profiler, mem tracker, reflection,
  entity handle, service locator, shared utilities (`sparse_set.h`,
  `fixed_hash_table.h`, `hash.h` FNV-1a, `string_util.h`).
- `math/` — header-only INTERFACE lib (no `math/src/`): Vec2/3/4, Mat4, Quat,
  Transform, AABB/ray/sphere, component PODs; SSE2 paths in `math_detail.h`.
- `physics/` — bodies, colliders, convex hull (GJK/EPA), heightfields, CCD +
  speculative contacts, manifolds, sequential-impulse solver + joints
  (`src/joints/`), queries, materials. Talks to the world ONLY through
  `PhysicsWorldView`; shape payloads live in World-owned `PhysicsContext`.
- `renderer/` — asset database/manager/streaming (fixed slots + tombstones,
  worker-thread queue, LRU), mesh/texture loading, procedural
  `mesh_primitives`, shader system (variants, hot reload), `RenderDevice`
  function table (GL impl in `render_device_gl.cpp`), command buffer frontend
  (`CommandBufferBuilder`, 64-bit DrawKey sort) with backend split across
  `command_buffer{,_flush,_sky,_ibl,_post_resources,_context,_builder,_math}`,
  pass resources, shadows (cascade/spot/point), light culling, post stack
  (bloom/SSAO/auto-exposure/tonemap/FXAA), GPU profiler.
- `audio/` — miniaudio-backed load/play/stop/volume.
- `scripting/` — Lua runtime + sandbox (instruction/memory caps), DAP
  debugger, hot reload with state persist, generated bindings
  (`bindable_api.h` → binding generator), and domain binding TUs in `src/`
  (entity lifecycle, body, mesh/material, physics, lights, camera, audio,
  asset, game, input, scene, timers, coroutines, collision, touch, cheat,
  debug, persist, entity pool/script/handle, `binding_util`). ~180 functions
  on one global `engine` table.
- `runtime/` — public `engine::bootstrap/run/shutdown` + `EngineConfig`,
  `EnginePipeline` (13 named frame stages, fixed 1/60 step, job-graph frame),
  `World` ECS (13 component types on SparseSets, WorldPhase gating,
  double-buffered transforms, persistent ids), scene/prefab serializers
  (shared `serialization_util`, reflection-backed components), physics/
  scripting/editor bridges, render-prep pipeline, service registry, timers,
  cameras, spring arms, game mode/state, player controllers, entity pool.
- `editor/` — ImGui editor: `editor_session` (state + play lifecycle),
  `editor_commands` (undoable edits), panel TUs (main/inspector/diagnostics/
  assets/viewport), editor + debug cameras, command history.
- `assets/` — GLSL shaders, sample Lua scripts, sample meshes (synced to the
  build dir by CMake). `tools/` — asset_packer (deterministic cook,
  thumbnails, glTF mesh/skeleton/animation import, dependency graph), binding
  generator, comment audits, CI helpers. `tests/` — unit / integration /
  smoke (`gpu` label) / benchmark + `test_harness.h`.
  `.github/workflows/ci.yml` — 10 jobs: build matrix (3 OS × 2 configs),
  determinism hash compare, static analysis + comment audits, clang-tidy,
  werror, ASAN/UBSAN, TSAN, coverage (≥50%), benchmarks (>10% regression
  fails), quality gate.

## Architecture invariants

- Entity = `{index, generation}`; index 0 invalid. Component mutation is only
  legal in `WorldPhase::Input`; writable transforms during Simulation require
  the `SimulationAccessToken`. Never break transform double-buffering,
  persistent-id behavior, or entity-capacity assumptions.
- Frame: per fixed step, chunked update jobs → chunked physics jobs → one
  resolve_collisions job → commit swap; then render-prep jobs fill per-thread
  command buffers merged for the GL flush. Preserve deterministic stepping
  and thread-count independence.
- Serialization format changes need migration handling + tests; scene loads
  stage into a replacement World and commit only on success.
- Renderer: command construction stays separate from GL execution; preserve
  forward fallback and transparency behavior when touching deferred paths;
  prefer CPU-verifiable renderer tests (GPU tests carry the `gpu` label).
- Scripting: don't break the Lua API without tests + doc updates; validate
  stack usage; preserve traceback, sandbox, and hot-reload behavior.
- Private headers in `src/` are the established pattern for module-internal
  APIs — keep using it; do not move them into `include/`.
- When adding shared utilities, put them in `core` and migrate ALL duplicate
  call sites in the same series.

## Working conventions

- Small focused changes; one concern per commit; no drive-by rewrites; do not
  modify unrelated systems. Concise imperative commit messages.
- `git status` before editing; never overwrite uncommitted changes that are
  not yours; do not commit unless asked. Never delete source files or hide
  build failures.
- Verification per change: zero-warning build → headless ctest → targeted
  determinism/bench suites when the area is sensitive → both comment audits.
  New behavior requires a new or extended test (or an explanation).
- Prefer `bool`+log, small status objects, or optional-like returns;
  assertions only for programmer errors.

## Product vision (2026-07-19 — priorities derive from this)

The engine's users are beginners making games, scenes, and interactive
things with no game-dev or modeling background, on whatever hardware they
have — and the same tool must scale to professional use ("absolute beginner
to master in the game industry"). What that implies, in priority order:
device reach over high-end rendering; a commercial-grade editor experience;
built-in creation tools so no external DCC is ever required (shape/blockout
tools, starter templates, bundled assets); radically good defaults; and
one-click sharing, with web export as the headline distribution feature.
Platforms follow the vision: Windows/Linux editor first, iOS/iPadOS
runtime, web export once the bgfx migration lands; a macOS editor is likely
(creators and students use Macs) even though shipping games on macOS stays
a non-goal; Android is a low-cost later option (bgfx GLES), not a
commitment.

## Roadmap

Production-ready foundations (verified; details in git history of the former
TODO.md): build/CI/determinism/profiling baseline, ECS + gameplay loop
(lifecycle, input incl. touch/rebinding, game mode/state, timers, cameras,
coroutines, DAP + sandbox + hot reload, binding generator), physics (all
collider shapes incl. capsule/hull/heightfield, warm-started solver, 6 joint
types, manifolds, materials/layers, queries, CCD + speculative contacts),
asset pipeline (64-bit ids, metadata/tags, dependency graph, async streaming
with budgets, LRU, deterministic cook + thumbnails), renderer through
deferred+forward, shadows (cascade/spot/point), sky (cubemap/Preetham/Hosek),
IBL + reflection probes, fog, instancing + foliage, post stack; 2026-07
production-hardening campaign (27 findings: correctness, perf, dedup,
architecture splits, comment quality — all closed, quality CI-enforced).

Open — Phase 1 ship blockers:

- **P1-M6 residuals**: CLOSED 2026-07-19 — `SceneCaptureComponent`
  (render-to-texture) landed: up to 8 enabled captures per frame render
  the forward-lit scene from the owning entity's world transform (-Z
  forward) into per-slot LDR targets inside `flush_renderer`
  (`command_buffer_capture.cpp` owns request storage + GL slots;
  captures skip sky/shadows/post by design). Runtime gathers requests in
  `stage_render`; textures come back via
  `renderer::get_scene_capture_texture(slot)`. Serialized in scene +
  prefab JSON through the reflection path; editable in the inspector
  (with a live preview image). Capture-to-material binding:
  `MeshComponent.sceneCaptureSourceId` names the capture entity's
  persistent id, resolved during render prep to a stable external
  `TextureHandle` (`texture_loader` external registrations alias
  renderer-owned GL ids and are never destroyed by the texture system)
  and applied as the mesh's albedo texture.
  The shader binary cache was CUT 2026-07-19: it is GL-specific work
  that the planned RHI migration (below) discards — bgfx/modern APIs
  bring their own pipeline caching. Done 2026-07-18: material instances
  + JSON material assets — `material_loader` resolves parent-chain
  overrides at load into flat `AssetDatabase` records
  (`AssetTypeTag::Material`), referenced by
  `MeshComponent.materialAssetId` and applied during render prep (inline
  PBR fields remain the fallback).
- **P1-M7 Animation**: clip quantization + async animation assets (import of
  glTF skins/clips already lands in asset_packer); pose blending (crossfade,
  1D/2D blend spaces, additive, masked); state machine (JSON, hot reload,
  Lua params); root motion; animation events/notifies; montages; GPU skinning
  (bone palette UBO, skinned G-buffer variant); two-bone IK (foot placement,
  arm reach).
- **P1-M8 Audio**: 3D positional (attenuation/panning/HRTF option, listener
  entity); bus hierarchy + snapshots/ducking; DSP (reverb, biquad filters for
  occlusion, Doppler); audio events + layered music; finish streaming for
  large files.
- **P1-M9 Editor**: inspector for nested structs/arrays; full undo coverage;
  scene hierarchy panel (tree, drag-drop reparent, multi-select); asset
  browser upgrades (drag-to-viewport, search/tags); prefab overrides +
  nesting; PIE pause/step; editor Lua API (menu items, custom panels);
  **UX overhaul to commercial-editor standard** (2026-07-19 priority call:
  the look/feel gap is a real problem, not polish) — icon set, layout and
  spacing pass, toolbar/hierarchy/inspector usability, DPI-aware font
  scaling. The base theme + Roboto font landed 2026-07-19
  (`apply_editor_style` in editor.cpp, assets/fonts/).
- **P1-M10 Scene/Streaming**: scene transition API (exclusive/additive);
  UUID cross-scene references; streaming volumes; distance LOD with
  hysteresis; multi-slot save system with platform-aware paths.
- **RHI migration** (decided 2026-07-19; scheduled after P1-M10, before
  P1-M11): adopt **bgfx** as the render backend, replacing the GL
  implementation behind `RenderDevice` and porting the backend TUs +
  GLSL shaders to bgfx's model. The command-buffer frontend (builder,
  DrawKey sort, render prep) is designed to survive unchanged. Ship
  targets after migration: Windows, Linux, iOS/iPadOS (bgfx Metal
  backend). macOS game shipping is a non-goal, but a macOS *editor* is
  likely for creator/student reach; macOS CI lanes stay regardless (Apple
  requires Mac hosts to build for iOS, and they are free portability
  checks meanwhile).
- **Web export** (elevated from Phase 3 by the product vision; feasible
  once bgfx lands): Emscripten runtime builds plus a share-ready HTML
  shell, so creations run from a link — the beginner sharing story.
- **Creator starter kit** (elevated from Phase 2): built-in shape and
  CSG-style blockout tools (moved up from P2-M5), playable starter
  templates, and a bundled drag-in asset library — the
  no-modeling-knowledge path to a first scene (project templates
  coordinate with P1-M13-B).
- **P1-M11 Runtime UI**: canvas + resolution independence, batched 2D quad
  pipeline, font rendering (SDF, rich text), core widgets + layout
  containers, input routing, tweens, Lua UI API, localization +
  accessibility.
- **P1-M12 Ship readiness**: finish platform abstraction; quality presets +
  dynamic resolution; **frame pacing** (vsync modes, frame cap, and
  fixed-step render interpolation — currently the renderer runs uncapped
  against the 60 Hz sim with no interpolation, which will visibly stutter
  once anything moves fast); distribution packaging (asset packs, CPack);
  crash handler; accessibility (UI scaling, subtitles); end-to-end smoke +
  leak detection.
- **P1-M13 Production ops**: versioned data migrations; project manifest/
  templates; headless commandlets; content validation + cook reports;
  platform asset variants/compression; support diagnostics + crash dumps;
  signing + safe mode; compliance/dependency governance; hardware QA matrix;
  editor preferences/autosave/recovery.

Open — Phase 2 (competitive parity): advanced rendering (lightmap baking,
SSR, volumetric fog, advanced post), CPU/GPU particles, 2D engine (sprites,
tilemaps, 2D physics/camera), networking (reliable UDP, replication, lobby),
splines + data tables + foliage painting (CSG moved into the Phase 1
creator starter kit), haptics/gyro/input replay, advanced editor features,
performance polish.

Open — Phase 3 (future): XR, iOS/iPadOS ship targets (on the bgfx Metal
backend from the RHI migration; the former "Vulkan backend" item is absorbed
by that migration; web export moved up to Phase 1), AI + navigation,
advanced networking. macOS game shipping and Android are options, not
commitments (see the product vision). Parallel lanes: documentation,
extended test coverage (including golden-image renderer tests — offscreen
render + image-hash compare where a GL context exists, closing the
pixel-verification gap), devops pipeline.

Known renderer issue (2026-07-19): `deferred_lighting.frag` exceeds NVIDIA's
fragment uniform register limit (C6020) and fails to link, so the deferred
path is silently unavailable on at least NVIDIA — the engine renders via the
forward fallback. Fix by moving per-light data into the tile texture/UBO or
shrinking the shader-side arrays.
