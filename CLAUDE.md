# CLAUDE.md — Engine

The single project document: instructions, repository map, conventions, and
roadmap. It replaced the former `PROJECT_INDEX.md`, `TODO.md`,
`REVIEW_FINDINGS.md`, and the `AGENTS.md` mirror (their full text lives in
git history). Keep this file updated in the same commit as any change to
module structure, build commands, test layout, or roadmap status.

## What this is

C++23 game engine built from scratch: SDL3 window/input, OpenGL renderer
(deferred+forward, PBR/IBL, GLSL 330 core), fixed-capacity ECS (65,536
entities, double-buffered transforms), CPU-deterministic physics, Lua 5.4
scripting, miniaudio, ImGui editor. Goal: production level. Game authors work
through Lua and the editor; engine contributors work in C++ under the rules
below.

Third-party (all SHA-pinned via FetchContent in the root CMakeLists.txt):
SDL3 3.4.12, Lua 5.4.6, ImGui docking + ImGuizmo snapshots, cgltf 1.14
(tools only), stb snapshot, miniaudio 0.11.21, OpenGL 4.5+.

## Hard rules (enforced)

- C++23 only. No exceptions, no RTTI, no `dynamic_cast`/`typeid`
  (`/EHs-c- /GR-`, `_HAS_EXCEPTIONS=0`; `/W4 /WX` or `-Werror`).
  Language features must compile on every CI lane (AppleClang is the
  laggard — no deducing `this` until its Xcode catches up). Error paths
  prefer `std::expected<T, E>` in new APIs; never call `.value()` — with
  exceptions disabled it aborts. Use `has_value()`/`operator*`/`error()`.
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
  Comments live ONLY in those two places — file top and above declarations;
  no comments inside function bodies or on variables unless a constraint
  genuinely cannot be expressed at the declaration.
- Changes to math/ECS/physics/renderer/scripting behavior require tests.
  Determinism-sensitive areas (world, serialization, physics, render-prep,
  Lua API) pair changes with determinism tests.
- Test strictness: assert the tested behavior EXACTLY (no loose tolerances on
  the subject under test); never assert wall-clock timing/throughput in
  functional tests — only dedicated `engine_bench_*` tests hold performance
  thresholds (gated against `tests/benchmark/perf_baseline.json`).
- Tests are append-only: adding tests is always welcome, but an existing
  test may only be modified when the test itself is defective. A deliberate
  behavior change that invalidates a pinned test is a decision for the
  project owner, not a silent test edit.
- No god files: one responsibility per translation unit. When a TU accretes
  a second concern, split it (the `command_buffer_*` backend split is the
  model); ~1,000 lines is the review trigger for engine sources. The
  2026-07-30 split campaign resolved the then-standing offenders; the
  2026-07-31 review found nine TUs back over the trigger (json,
  render_device_gl, engine_pipeline, world.h, scene_serializer,
  narrow_phase, editor_panels_inspector, dap_server, asset_database) —
  queued for the next split pass, owner directs each split. Test files
  grow by appending (rule above) — split them by starting new suite
  files, never by relocating existing tests.
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

When launching builds/tests from a GUI or agent harness (anything without its
own console), wrap the command in `tools/run_quiet.ps1` so the dozens of
child processes stop flashing console windows — output still streams to the
calling terminal and the exit code is preserved:

```powershell
pwsh -NoProfile -File tools/run_quiet.ps1 -- ctest --test-dir build --output-on-failure -LE gpu
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
  speculative contacts, clipped contact manifolds (`contact_clip`,
  `contact_resolution`, `narrow_phase`, `physics_step`,
  `physics_payloads` TUs), sequential-impulse solver + joints
  (`src/joints/`), queries, materials, primitive hull builders
  (`primitive_hulls` — cylinder/pyramid spawn shapes collide as mesh-matched
  convex hulls; box/sphere/capsule stay analytic, mirroring the Unity/Unreal
  collider model). Talks to the world ONLY through
  `PhysicsWorldView`; shape payloads live in World-owned `PhysicsContext`;
  `collider.cpp` centralizes affine world geometry for every shape.
- `renderer/` — asset database/manager/streaming (fixed slots + tombstones,
  worker-thread queue, LRU), mesh/texture loading, procedural
  `mesh_primitives`, shader system (variants, hot reload), `RenderDevice`
  function table (GL impl in `render_device_gl.cpp`), command buffer frontend
  (`CommandBufferBuilder`, 64-bit DrawKey sort) with backend split across
  `command_buffer{,_init_*,_flush,_flush_*,_sky,_ibl,_post_resources,_context,_builder,_math}`
  (init stages behind `command_buffer_init_internal.h`; flush passes share
  `FrameFlushContext` in `command_buffer_flush_internal.h`),
  pass resources, shadows (cascade/spot/point), light culling, post stack
  (bloom/SSAO/auto-exposure/tonemap/FXAA), GPU profiler.
- `audio/` — miniaudio-backed: loaded-sound handles, master/music/sfx
  bus groups, a fixed one-shot instance pool (spatialized
  `play_sound_at` + 2D `play_sound_oneshot`), the camera-following 3D
  listener, and VFS-streamed music (`tools/gen_sounds.py` generates the
  bundled placeholder WAVs in `assets/sounds/`).
- `scripting/` — Lua runtime + sandbox (instruction/memory caps), DAP
  debugger, hot reload with state persist, generated bindings
  (`bindable_api.h` → binding generator), and domain binding TUs in `src/`
  (entity lifecycle, body, mesh/material, physics, lights, camera, audio,
  asset, game, input, scene, timers, coroutines, collision, animation,
  touch, cheat, debug, persist, entity pool/script/handle, `binding_util`).
  ~180 functions on one global `engine` table.
- `runtime/` — public `engine::bootstrap/run/shutdown` + `EngineConfig`,
  `EnginePipeline` (15 named frame stages, fixed 1/60 step, job-graph frame;
  animation evaluates per fixed step BEFORE the frame graph so render prep
  bakes current-frame palette slots; frame pacing waits out r_max_fps as
  the final stage),
  `World` ECS (14 component types on SparseSets, WorldPhase gating,
  double-buffered transforms, persistent ids), scene/prefab serializers
  (shared `serialization_util`, reflection-backed components), physics/
  scripting/editor bridges, render-prep pipeline, skeletal animation (CPU
  pose evaluation in `animation.cpp`, cooked .skel/.anim loaders, the
  controller state machine + palette handoff in `animation_system.cpp`),
  fixed-step render interpolation (per-entity world-TRS history in the
  World, blended in render prep; `frame_pacing.{h,cpp}` holds the
  vsync/cap helpers), the single-slot game save (`save_data.{h,cpp}` over
  `platform_get_save_dir`), service registry, timers, cameras, spring
  arms, game mode/state, player controllers, entity pool.
- `editor/` — ImGui editor: `editor_session` (state + play lifecycle,
  multi-selection), hierarchy tree panel (drag-drop reparent through the
  undoable ReparentCommand),
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
- User-facing scene objects are created through `create_scene_object` and
  always own a non-removable Transform (position, rotation, scale);
  `create_entity` is the internal bare-ECS escape hatch. Spatial components
  and children consume the composed world matrix. Dynamic rigid bodies must
  be hierarchy roots, and descendant colliders form one compound body owned
  by their nearest rigid-body ancestor
  (`PhysicsWorldView::rigid_body_owner`); collision and queries consume the
  composed world pose via `get_physics_transform` /
  `get_simulation_physics_transform`. Lua exposes
  `set_parent`/`get_parent`/`get_children`; the inspector treats Name and
  Transform as non-removable identity. Destroying an entity destroys its
  whole transform subtree (deferred destruction queues the subtree so
  EndPlay fires for every member).
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
deferred+forward, shadows (cascade/spot/point), sky (cubemap/Preetham/
procedural scatter — `procedural_sky.frag`, the default),
IBL + reflection probes, fog, instancing + foliage, post stack; 2026-07
production-hardening campaign (27 findings: correctness, perf, dedup,
architecture splits, comment quality — all closed, quality CI-enforced).

North star — the v1.0 acceptance test (2026-07-30, supersedes the former
milestone list; priorities derive from the product vision above):

> A person with zero game-dev experience opens the editor, follows one
> starter template, and shares a playable web link of their own variation
> within an hour.

Everything ships in three outcome slices, each with an acceptance demo. A
slice is done when its demo passes, not when its feature list is exhausted.

**The reference slice** that drives all scoping: *Island Hopper*, a small
third-person collect-a-thon. One island scene built from blockout shapes
and bundled props; an animated character (idle/walk/jump) the player
steers; pickups with sounds and a counter; one moving platform and one
falling hazard (both already-working physics); a goal that ends the game
with a win screen and best-time save. Every milestone below is scoped to
exactly what this slice needs. Anything it does not need is in the parking
lot — cut from v1, not canceled.

Sequencing decision (2026-07-30): **content before replatform.** The RHI
migration gates web export (the headline differentiator), but the
hour-test cannot be validated until the creation loop exists, and porting
a stabilized renderer is far cheaper than porting a moving target — the
2026-07-30 flush/init decomposition made the pass list an enumerable port
surface. So: v0.1 proves the loop on desktop GL, v0.5 replatforms and
ships the web story, v1.0 passes the hour test.

### v0.1 — "The slice plays" (desktop GL, Windows/Linux)

Acceptance demo: Island Hopper is built *in the editor* from the bundled
kit, played start-to-finish with sound, and feels smooth (no visible
stutter) at 60 Hz sim.

- **Animation (from P1-M7, cut down) — LANDED 2026-07-31** (PR #39): glTF
  skinned character import (cooked .skel/.anim/.mesh v3 formats, joint
  reorder + vertex remap), clip playback + crossfade blending, GPU
  skinning (bone palette UBO, skinned G-buffer and cascade/spot
  shadow-depth variants; the forward path, scene captures, and
  point-light shadows have no skinned variants yet, so characters render
  bind-pose on those surfaces), a minimal JSON state machine
  (idle/walk/jump controller, Lua `engine.set_anim_param`; transitions
  match in array order — first match wins — non-looping states latch at
  their final frame until a parameter drives them out, and event names
  cap at 31 chars), animation events (`on_anim_event`, footsteps), and a
  generated blocky rigged character (`tools/gen_character.py` →
  `assets/character.*`) in the bootstrap scene. Authoring: the inspector
  edits AnimationComponent (controller path, playing, speed) with
  undoable add/remove. CUT to parking lot: blend spaces, additive/masked
  blending, montages, root motion, IK.
- **Audio (from P1-M8, cut down) — LANDED 2026-08-01**: 3D positional
  one-shots from a fixed instance pool (attenuation + pan, listener
  follows the presented camera), master/music/sfx buses with volumes,
  VFS-streamed music, and Lua audio (`engine.play_sound_at`,
  `set_bus_volume`, `play_music`, `stop_music`); the sample scene's
  character walks with footstep animation events playing positional
  sounds over a streamed ambient loop. CUT: HRTF, DSP
  (reverb/filters/occlusion), Doppler, snapshots/ducking.
- **Editor (from P1-M9, cut down) — IN PROGRESS (feat/editor-authoring)**:
  scene hierarchy panel (tree, drag-drop reparent, multi-select) LANDED
  2026-08-01; still open: asset browser drag-to-viewport, inspector
  nested structs/arrays, undo covering every operation the slice's
  authoring flow uses (entity create/delete undo remains), PIE
  pause/step. The commercial-grade UX pass continues throughout. CUT:
  prefab overrides/nesting, editor Lua API.
- **Scenes (from P1-M10, cut down) — LANDED 2026-08-01**: the exclusive
  scene transition flow is pinned end to end by an integration test
  (`engine.load_scene` from a playing script → pending-op commit →
  scene B `on_begin_play` refires → `engine.new_scene` teardown);
  cross-scene state rides Lua globals, which survive transitions because
  the VM persists across scene loads (the templates' handoff channel);
  and the single save slot lands as `engine.save_data(table)` /
  `engine.load_data()` — a flat string-keyed table (number/string/bool)
  as JSON in the per-user platform save directory
  (`runtime/save_data.{h,cpp}`, `platform_get_save_dir`). CUT: additive
  scenes, streaming volumes, LOD hysteresis, multi-slot saves.
- **Creator kit v1 (elevated by the vision)**: blockout authoring with the
  built-in primitives (grid snapping, material presets), the Island Hopper
  starter template itself, and a bundled mini asset pack (1 rigged
  character, ~20 props, ~15 sounds). CUT: CSG tools.
- **Frame pacing (pulled forward from P1-M12) — LANDED 2026-08-01**:
  `r_vsync` (0/1/adaptive, applied live), `r_max_fps` sleep-then-spin cap
  (verified 60 frames per wall second), and fixed-step render
  interpolation — render prep blends model matrices between the previous
  and current fixed-step world TRS (epoch-validated World history,
  cleared on play start/scene load) and the active camera interpolates
  between fixed-step samples. The FPS overlay now reports the presented
  frame rate (the old stat measured pre-render stages only and hid
  vsync entirely).

### v0.5 — "Runs everywhere" (the replatform slice)

Acceptance demo: Island Hopper plays from a shared web link in a browser,
and the iOS runtime boots it.

- **RHI migration (decided 2026-07-19)**: adopt **bgfx** behind
  `RenderDevice`, porting the init/flush pass TUs and GLSL shaders to
  bgfx's model. The command-buffer frontend (builder, DrawKey sort, render
  prep) is designed to survive unchanged. bgfx brings pipeline caching
  (the GL shader binary cache stays CUT).
- **Web export (elevated from Phase 3 by the vision)**: Emscripten runtime
  builds plus a share-ready HTML shell — creations run from a link.
- **Device reach**: quality presets + dynamic resolution; iOS/iPadOS
  runtime proof on bgfx Metal (macOS editor remains likely later; macOS
  game shipping stays a non-goal; Android stays an option).

### v1.0 — "The hour test"

Acceptance demo: five external testers with no game-dev background each
produce and share a playable variation of a template in under an hour,
unassisted.

- **Runtime UI (from P1-M11, cut down)**: canvas + resolution
  independence, batched 2D quads, font rendering, the widgets the
  templates' HUD/menus need, Lua UI API. CUT: rich text, tweens, full
  localization framework (keep text externalized).
- **Ship readiness (from P1-M12, remainder)**: distribution packaging,
  crash handler, accessibility baseline (UI scaling, subtitles),
  end-to-end smoke + leak detection.
- **Production ops (from P1-M13, cut down)**: project manifest + "new
  project from template" flow, versioned data migrations, editor
  preferences/autosave/recovery, support diagnostics. CUT: commandlets,
  cook reports, platform asset variants, signing/safe mode, HW QA matrix
  (revisit for stores).
- **Two more starter templates** (three total) + an onboarding pass over
  the first-run experience.

### Parking lot (post-1.0, unordered)

Everything cut above, plus the former Phase 2/3 lists: advanced rendering
(lightmap baking, SSR, volumetrics, advanced post), particles, the 2D
engine, networking, splines/data tables/foliage painting, CSG,
haptics/gyro/input replay, XR, AI + navigation, iOS/iPadOS as *ship
targets* (the v0.5 proof becomes a product decision here), macOS/Android
calls per the vision. Parallel lanes stay live throughout: documentation,
extended test coverage (including golden-image renderer tests — offscreen
render + image-hash compare where a GL context exists), devops pipeline.

Historical note: the pre-2026-07-30 phase/milestone breakdown (P1-M6..M13
feature lists) lives in git history; M-numbers above reference it so old
tasks and commit messages stay legible. Completed-milestone records:
P1-M6 residuals closed 2026-07-19 (`SceneCaptureComponent` render-to-
texture; capture-to-material binding via `MeshComponent.
sceneCaptureSourceId`; material instances + JSON material assets landed
2026-07-18 via `material_loader`). The editor base theme + Roboto font
landed 2026-07-19; DPI-aware UI scaling landed 2026-07-30.


Former known renderer issue, FIXED 2026-07-19: `deferred_lighting.frag`
exceeded NVIDIA's fragment uniform register limit (C6020) and failed to
link, leaving the deferred path silently unavailable there. Per-light data
now lives in an R32F light-data texture (one row per light slot; layout
constants + `pack_light_data` in light_culling.h/.cpp, uploaded each frame
beside the tile texture, sampled via `texelFetch`) instead of 128-point/
64-spot uniform arrays. The same change fixed a latent tile-lookup bug:
the shader read the spot section at `MAX_POINT_LIGHTS + 1` (129) while the
CPU packs tile rows as `1 + 32 + 1 + 16` texels, so deferred spot lights
sampled out of bounds and never lit. Verified on NVIDIA hardware 2026-07-20
(RTX 5070, driver 32.0.16.1074): the shader links cleanly, no renderer
warnings across a live editor session, and the deferred path is active. A
deliberately broken shader was used as a control to confirm link failures
do surface in the startup log.
