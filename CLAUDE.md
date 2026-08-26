# CLAUDE.md — Engine

This is the canonical contributor contract for the Engine repository. Keep it
updated in the same commit as changes to architecture, module structure,
build commands, test layout, supported behavior, or roadmap status.

Normative sections take precedence in this order: release safety and finding
closure, hard rules, architecture invariants, working conventions, then the
roadmap. A roadmap status or historical note never overrides a safety rule.
When this document conflicts with code or tests, report and resolve the
mismatch instead of assuming the document is correct.

## What this is

C++23 game engine built from scratch: SDL3 window/input, bgfx renderer
(deferred+forward, PBR/IBL; shaders are shaderc-cooked .sc sources,
proven on Vulkan and WebGL2/GLES), fixed-capacity ECS (65,536
entities, double-buffered transforms), CPU-deterministic physics, Lua 5.4
scripting, miniaudio, ImGui editor. Goal: production level. Game authors work
through Lua and the editor; engine contributors work in C++ under the rules
below.

Third-party (all SHA-pinned via FetchContent in the root CMakeLists.txt):
SDL3 3.4.12, Lua 5.4.6, ImGui docking + ImGuizmo snapshots, cgltf 1.14
(tools only), stb snapshot, miniaudio 0.11.21, bgfx v1.153 via the
bgfx.cmake superproject (bundles bx/bimg; the only backend — default
since the 2026-08-22 flip, with the legacy GL backend deleted at parity
the same day, #138).

## Hard rules

Rule labels state how they are enforced:

- **[CI]** has a named mechanical gate.
- **[REVIEW]** must be demonstrated in the PR and checked by a reviewer.
- **[OWNER]** requires explicit project-owner approval.

Calling a rule "enforced" without a CI gate, review evidence, or owner decision
is prohibited.

- **[CI]** C++23 only. No exceptions, no RTTI, no `dynamic_cast`/`typeid`
  (`/EHs-c- /GR-`, `_HAS_EXCEPTIONS=0`; `/W4 /WX` or `-Werror`). Language
  features must compile on every CI lane (AppleClang is the laggard — no
  deducing `this` until its Xcode catches up). Error paths prefer
  `std::expected<T, E>` in new APIs; never call `.value()` — with exceptions
  disabled it aborts. Use `has_value()`/`operator*`/`error()`.
- **[REVIEW]** Public real-time and leaf runtime APIs are `noexcept` only when
  every operation they invoke is proven non-throwing. A recoverable `noexcept`
  path must not call allocation-, filesystem-, or thread-creation operations
  that can terminate under the no-exception build. Cold initialization,
  editor, tool, and filesystem work uses staged RAII transactions, explicit
  error results, and rollback. No silent failure and no process termination
  for recoverable errors.
- **[REVIEW]** No heap allocation on hot paths (ECS iteration, transform
  propagation, physics stepping, render prep, command buffers, streaming,
  input, jobs). Fixed-size/preallocated storage; no unordered containers,
  locks, or virtual dispatch on hot paths without justification backed by a
  profile and budget.
- **[CI][REVIEW]** Dependency flow strictly downward, no cycles or sideways
  deps: `app → editor → runtime → renderer/physics/scripting/audio → content →
  core/math` (`content` is the generic asset layer, #171; it depends only
  on `core` and must never link a subsystem module). Within the bottom
  tier the direction is `math → core` (component PODs carry entity
  handles); `core → math` is forbidden. The four mid-tier subsystem
  modules are siblings and must not include or link each other; they meet
  only in `runtime`, which owns the bridges. The chain states direction,
  not adjacency: reaching past a tier downward (`editor → renderer`) is
  legal. `tools/check_module_deps.py` (#311) is the mechanical gate, run
  in the static-analysis CI job beside the comment audits: it validates
  every first-party `#include "engine/<module>/..."` against that graph,
  rejects includes of another module's private `src/` headers, and rejects
  foreign include directories hand-wired via `*_INCLUDE_DIRS` — a
  dependency is expressed only as a dep on the target, so CMake usage
  requirements stay the single source of truth. The gate carries an
  allowlist of today's tracked violations (the #309 scripting→runtime
  cycle, the #311 CMake declarations, and
  the sanctioned editor→`runtime/src/component_registry.h` crossing from
  #156). The #310 scripting→physics edge is no longer among them: no
  scripting TU includes `engine/physics/` (2026-08-26), so that direction
  is enforced with no exception, and scripting's remaining
  `engine_physics` dep carries only the physics include paths that
  runtime's public headers need under the #309 grant.
  Entries cannot be left behind once fixed: one that no longer
  matches anything is itself a finding, so each fix deletes its own
  entries and the gate is red on that fix's base. Adding a new entry is
  not mechanically prevented and stays [REVIEW]. The rule is [CI] for
  every edge not on that list and [REVIEW] for the listed ones until the
  allowlist empties. The gate reads include edges and hand-wired
  `*_INCLUDE_DIRS` only; dependency *visibility* (a PUBLIC dep that
  should be PRIVATE) is not mechanically checked and stays [REVIEW].
- **[REVIEW]** Public headers are self-contained and never leak SDL/bgfx/
  Lua/ImGui/ImGuizmo types. bgfx stays inside renderer impl (plus the
  editor's sanctioned ImGui backend); Lua inside scripting impl;
  editor-only behavior stays in `editor/` behind explicit bridges.
- **[CI]** Every source/header file needs a real file-level purpose comment,
  and declarations keep concise purpose comments. Both are CI-enforced:
  `tools/check_source_comments.py` (presence) and
  `tools/check_comment_quality.py` (no filler patterns; must stay at zero).
  Function-body comments are reserved for non-obvious invariants, ordering,
  units, ownership, or external constraints and explain why, not what.
- **[CI][REVIEW]** Changes to math/ECS/physics/renderer/scripting behavior
  require tests. Determinism-sensitive areas (world, serialization, physics,
  render-prep, Lua API) pair changes with determinism tests.
- **[REVIEW]** Tests assert the semantic contract at the strictest valid
  precision. Integer state, serialized data, and promised deterministic hashes
  are exact. Floating-point physics/render tests use justified absolute and/or
  relative tolerances plus invariants; arbitrary loose tolerances are
  forbidden. Functional tests never assert wall-clock timing or throughput —
  only dedicated `engine_bench_*` tests hold performance thresholds (gated
  against `tests/benchmark/perf_baseline.json`).
- **[OWNER]** Existing behavioral tests are contracts, not append-only relics.
  They may change only when the old contract is defective or an intentional
  behavior/version migration is approved. The change explains old versus new
  behavior, proves the new contract, and preserves a legacy mode when content
  compatibility requires it. Never weaken or delete a test merely to make a
  change pass.
- **[REVIEW]** No god files: one responsibility per translation unit. When a TU
  accretes a second concern, split it (the `command_buffer_*` backend split is
  the model); ~1,000 lines is the review trigger for engine sources. The
  2026-07-30 split campaign resolved the then-standing offenders; the
  2026-07-31 review found nine TUs back over the trigger, of which
  scene_serializer dropped back under during the 2026-08 audit campaign —
  eight remained (json, render_device_gl, engine_pipeline, world.h,
  narrow_phase, editor_panels_inspector, dap_server, asset_database). The
  2026-08-15 L-04 re-verification (issue #86) found five more had grown past
  the trigger since — thirteen remained (the eight above plus physics_query,
  scripting_bridge, convex_hull, world_components, scripting); the issue #156
  Inspector-metadata split moved editor_panels_inspector's per-component
  branching into editor_component_registry, editor_inspector_metadata,
  editor_reference_pickers, editor_panels_inspector_generic, and
  editor_panels_inspector_custom (1263 -> 349 lines), dropping it back under
  the trigger; the #138 GL deletion (2026-08-22) removed render_device_gl
  from the list — eleven remain, queued for the next split pass, owner
  directs each split. Split growing tests into focused suite
  files while preserving test names/history unless an approved contract
  migration requires a move.
- **[OWNER]** No new third-party dependencies without confirmation; never ones
  requiring exceptions/RTTI in engine code.
- **[REVIEW]** Beginner-friendly APIs never justify incorrect internal
  semantics. Simplicity comes from presets, defaults, validation, diagnostics,
  undo/recovery, and progressive disclosure. Standard physics names such as
  hinge, slider, ball socket, and fixed implement their standard degrees of
  freedom; simplified alternatives use explicit names and serialized behavior
  versions.
- **[REVIEW]** Authored user data is never written by truncating the final
  destination. Scene, prefab, save, project, metadata, editor settings, input
  maps, and cooked/generated outputs use staged sibling writes with checked
  write/flush/sync/close and atomic replacement. Multi-file outputs commit as a
  transaction or manifest. Failed load, restore, migration, or save preserves
  the previous valid state.

## Release-safety and finding-closure contract

- A Critical or High finding is closed only when every evidence location and
  impact in its original scope is addressed. A partial fix may merge, but the
  unresolved scope remains open under an explicitly linked finding.
- A regression fails on the base revision and passes on the fixed revision.
  Record the reproduction and the exact test that proves closure.
- Tests exercise the production entry point and real dependency wiring. A
  copied model of a scheduler, serializer, parser, or state machine is useful
  only as supplementary coverage and never proves the production path.
- Boundary coverage is mandatory where applicable: zero work, one item, many
  items, capacity, malformed input, partial failure, cancellation, concurrent
  access, and repeated lifecycle/world transitions.
- Data-loss fixes include fault injection for write, flush, sync, close,
  rename, parse, restore, and rollback boundaries relevant to the change.
- Concurrency fixes prove happens-before relationships for every branch,
  including empty ranges, disabled subsystems, submission failure, and
  catch-up/repeated steps. TSAN is supporting evidence, not proof of the DAG.
- A PR description distinguishes fixed, partially fixed, deferred, and
  pre-existing behavior. It must not say "all," "never," "production-ready,"
  or "closed" beyond what tests and scope demonstrate.
- "Verified," "landed," and "production-ready" status claims name the commit,
  test/acceptance evidence, platform scope, and date. Contradicting audit or CI
  evidence reopens the claim immediately.

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
python tools/check_module_deps.py                 # module dependency audit
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

`CMakePresets.json` mirrors these exact flows as named configure/build/test
presets (e.g. `cmake --preset windows-clang-cl-debug`,
`ctest --preset linux-clang-debug-headless`, `*-asan` for the sanitizer
lane); presets add no flags beyond the documented commands.

`build/compile_commands.json` is the clangd source of truth (`.clangd` points
at it); never hand-edit it, never commit `build/`. `build-release/` exists for
Release benchmark runs. Linux/macOS: same flow with clang/clang++. macOS is
a build + headless-test lane only (owner decision 2026-08-09, issue #82,
originally forced by Apple's OpenGL cap; the lane's remaining value is
AppleClang conformance until the bgfx Metal path is proven). CMake
options: `ENGINE_TARGET_PLATFORM` (Win64/Linux/macOS/Android/iOS/Web),
`ENGINE_RENDERER_BACKEND` (bgfx, the only accepted value — the legacy
`gl` backend was deleted at parity 2026-08-22, #138; the cache variable
survives so existing -D invocations keep working), `ENGINE_BGFX_SHADERC`
(ON: builds shaderc
and cooks the shader manifest; heavy CI lanes that never consume cooked
binaries turn it off and the cooked test sections skip),
`ENGINE_MAX_ENTITIES` (default 65536), `ENGINE_DETERMINISTIC_FLOATS` (ON:
`/fp:strict` / `-ffp-contract=off`), `ENGINE_SANITIZERS`,
`ENGINE_BUILD_TESTS/TOOLS`. Helper functions live in
`cmake/EngineHelpers.cmake` (module/static, header-only INTERFACE, exe, test);
they apply the warning/conformance flags per-target
(`engine_apply_strict_compile_options`), so third-party FetchContent targets
never inherit them — determinism, sanitizer, and platform-define flags stay
directory-global by design.

## Repository map

- `app/` — `engine_editor_app` entry point; whole-archives `engine_editor` so
  the editor bridge registers before bootstrap.
- `core/` — bootstrap/config, platform (SDL glue, paths; #138:
  `PlatformConfig.externalRenderContext` creates the window without an
  OpenGL context for swapchain-owning backends and exposes native
  window/display handles), logging, cvars,
  console, event bus, input + input maps + touch, VFS, JSON, job system
  (frame graph), allocators (linear/pool), profiler, mem tracker, reflection,
  entity handle, service locator, shared utilities (`sparse_set.h`,
  `fixed_hash_table.h`, `hash.h` FNV-1a, `string_util.h`).
- `math/` — header-only INTERFACE lib (no `math/src/`): Vec2/3/4, Mat4, Quat,
  Transform, AABB/ray/sphere, component PODs; SSE2 paths in `math_detail.h`.
- `content/` — generic asset/content layer (#171, depends only on core):
  `AssetId`/`AssetTypeTag`/`AssetMetadata` identity + tag + dependency-edge
  helpers, the `MetadataStore` (registration/lookup, tags, type queries,
  dependency edges, dependency-ordered load with cycle rejection), the
  asset transition-request queue, the async streaming queue (engine-neutral
  load/upload callbacks, worker threads, budget cvars), the cooked-asset
  staleness diagnostic, and the LRU cache. Non-render consumers and tools
  include `engine/content/...` directly (the tools packer shares
  `make_asset_id_from_path`, so cooked and runtime ids cannot drift); the
  renderer keeps the vocabulary re-exported in `asset_database.h` for its
  own mesh/texture/material services — the old per-header shims are
  deleted.
- `physics/` — bodies, colliders, convex hull (GJK/EPA), heightfields, CCD +
  speculative contacts, clipped contact manifolds (`contact_clip`,
  `contact_resolution`, `narrow_phase`, `physics_step`,
  `physics_payloads` TUs), sequential-impulse contact solver plus six public
  joint APIs (`src/joints/`; H-05 rework 2026-08-01: position-projection
  constraints with velocity projection over shared generalized-inverse-mass
  helpers in `joint_projection` — body-local anchor frames, hinge
  axis-alignment + radian twist limits, fixed/slider relative-orientation
  locks, prismatic rail Jacobian; Jacobian/effective-mass derivations in
  per-file comments; joint motors remain unimplemented, and joints are
  runtime-only state, never serialized), queries, materials, primitive hull
  builders
  (`primitive_hulls` — cylinder/pyramid spawn shapes collide as mesh-matched
  convex hulls; box/sphere/capsule stay analytic, mirroring the Unity/Unreal
  collider model), and a blocked-body warning diagnostic
  (`blocked_body_diagnostic` TU: logs once per episode when a
  velocity-driven body's achieved displacement persistently falls below its
  command; `physics.blocked_warn_steps` cvar, 0 disables). Talks to the
  world ONLY through
  `PhysicsWorldView`; shape payloads live in World-owned `PhysicsContext`;
  `collider.cpp` centralizes affine world geometry for every shape.
- `renderer/` — asset database/manager (fixed slots + tombstones, LRU
  eviction over `content`'s queue/cache), mesh/texture loading, procedural
  `mesh_primitives`, shader system (variants, hot reload), `RenderDevice` —
  the engine-owned backend-neutral device contract (#165): generational
  handles for buffers/textures/programs/geometry/render targets/queries,
  descriptor-based creation (textures created with pixels are
  immutable; per-frame CPU-refreshed data — the tile/light culling
  tables, laid out 2-D so 4K drawables stay inside D3D's 16384
  dimension cap — opts into `TextureDesc.cpuUpdatable`), named
  `ShaderParam` binding, whole
  `RenderState` application, and a `DeviceCaps` capability struct; the
  backend is the #138
  `render_device_bgfx.cpp` + `render_device_bgfx_programs.cpp` +
  `render_device_bgfx_translate.cpp` trio (the legacy
  `render_device_gl.cpp` was deleted at parity, 2026-08-22)
  (bgfx single-threaded; windowed
  runs initialize the real renderer — `r_bgfx_renderer` cvar, auto =
  bgfx's platform pick except Windows, where auto selects Vulkan
  explicitly (proven backend; the D3D11/12 backends are runnable —
  the #301 shadow-array unit map fits DXBC's 16-register cap and
  Win64 builds cook the `dx11` profile by default — but stay explicit
  `d3d11`/`d3d12` opt-ins until verified on hardware, an owner call
  to flip) — over the platform's native
  window/display handles, present through `present_render_device`
  (which also applies live `r_vsync` and drawable resizes via
  bgfx::reset), while headless runs stay on Noop;
  the editor's ImGui runs on bgfx through src/imgui_impl_bgfx.cpp
  (embedded ocornut-imgui shaders, view 255, ImTextureID =
  native_texture_id) with the stock SDL3 platform backend;
  the forward path renders under bgfx: pbr/default cooked over the #138
  flat uniform vocabulary shared with GLSL — packed vec4/mat4 element
  arrays via the contract's set_param_vec4_array/set_param_mat4_array,
  per-slot shadow samplers (`uShadowMap0..3` etc.), `u_modelMatrix`
  (bgfx reserves `u_model`) — with uniform sets staged and applied once
  per submit (GL last-write-wins semantics), vertex buffers realized at
  their attachment's stride (bgfx rejects stride overrides), and a
  backend-owned 3-vertex stream standing in for gl_VertexID fullscreen
  draws; the deferred path also renders under bgfx (gbuffer MRT +
  tile/light-data texelFetch lighting cooked; #301: the cascade and
  spot shadow maps are Tex2DArrays — one sampler register per set,
  cascades uniformly 2048² since array layers share dimensions — so
  the whole unit map tops out at register 15, inside DXBC's 16-sampler
  cap and WebGL2's 16-unit floor, and the deferred+PBR_FULL gates now
  pass on 16-unit devices); sky (cubemap/Preetham/procedural), IBL
  generation, the
  full post stack, and CSM/spot/point shadow sampling all run under
  bgfx — the entire pass list incl. GPU skinning (plain mat4-array
  palettes replace the BonePalette UBO on both backends) boots clean
  on Vulkan; instanced batching (both backends' flushes gate
  instancing on the shader toggle's presence) and forward-path
  shadow/IBL sampling defer to their units;
  programs link only from the Phase C shaderc cook's binaries via
  `create_program_binary` + `caps.cookedPrograms` — spirv is the
  canonical profile: GLSL-flavor binaries embed no uniform table and
  DXBC tables lose Load-only samplers to fxc stripping, so the glsl,
  essl, and dx11 profiles all link through the spirv-introspected
  entry (pinned by the reload-contract suite's dx11 scenario);
  runtime GLSL compile and mip generation stay unavailable; caps
  report uniform blocks and timestamp queries unavailable; suites
  `engine_unit_render_device_bgfx` + `engine_unit_shader_cook`) — both
  backends map engine
  handles through the generational slot tables in `device_slot_table.h`
  so stale device handles are detected instead of aliasing recycled
  backend objects; the one sanctioned native-id escape is
  `RenderDevice::native_texture_id`, consumed only by the editor's ImGui
  image binding (the null backend from #196 stays selectable via
  `r_null_device`), command buffer frontend
  (`CommandBufferBuilder`, 64-bit DrawKey sort) with backend split across
  `command_buffer{,_init_*,_flush,_flush_*,_sky,_ibl,_post_resources,_context,_builder,_math}`
  (init stages behind `command_buffer_init_internal.h`; flush passes share
  `FrameFlushContext` in `command_buffer_flush_internal.h`),
  pass resources, shadows (cascade/spot/point), light culling, post stack
  (bloom/SSAO/auto-exposure/tonemap/FXAA), GPU profiler.
- `audio/` — miniaudio-backed: loaded-sound handles, master/music/sfx
  bus groups, a fixed one-shot instance pool (spatialized
  `play_sound_at` + 2D `play_sound_oneshot`), the camera-following 3D
  listener, and VFS-resolved loose-file streaming music (archive-backed
  streaming remains pending; `tools/gen_sounds.py` generates the
  bundled placeholder WAVs in `assets/sounds/`).
- `scripting/` — Lua runtime + sandbox (instruction/memory caps), DAP
  debugger, hot reload with state persist, generated bindings
  (`bindable_api.h` → binding generator), and domain binding TUs in `src/`
  (entity lifecycle, body, mesh/material, physics, lights, camera, audio,
  asset, game, input, scene, timers, coroutines, collision, animation,
  touch, cheat, debug, persist, entity pool/script/handle, `binding_util`).
  ~180 functions on one global `engine` table.
- `runtime/` — public `engine::bootstrap/run/shutdown` + `EngineConfig`,
  convex-hull provenance (`Collider::hullSource` serializes which primitive
  builder made a ConvexHull payload; `World::add_collider` rebuilds it on
  every install path — scene/prefab load, world copy, editor undo — while
  Heightfield payloads remain unserialized, test-only reachable; #310:
  `primitive_collider.{h,cpp}`'s `apply_primitive_hull` is the one place a
  spawn path turns a `HullSource` into a collider — ConvexHull shape,
  provenance tag, the builder's own half extents — so script and editor
  spawns describe a collider instead of building hull payloads of their
  own, and `World::has_convex_hull_payload` is the observable a spawn uses
  to see whether the fixed hull slots had room),
  `EnginePipeline` (17 named frame stages, fixed 1/60 step, job-graph frame
  split into a simulation graph and a render-prep graph; animation evaluates
  per fixed step BEFORE the simulation graph so render prep bakes
  current-frame palette slots; the camera stage runs spring arms, authored
  CameraComponent publishing, and camera evaluation between the last fixed
  step and render prep so culling and interpolation see the frame's camera;
  frame pacing waits out r_max_fps as the final stage),
  `World` ECS (15 component types on SparseSets, WorldPhase gating,
  double-buffered transforms, persistent ids; #166: the storage dispatch,
  removal list, and both serializer directions expand from the registry —
  world.h's storage table + `world_query.h` hold the generated dispatch
  and query machinery, and a missing row/codec/descriptor fails to
  compile instead of silently skipping the type), scene/prefab serializers
  (one authoritative persistent-component registry —
  `src/component_registry.h` X-macro table, compile-time cross-checked
  against `World::PersistentComponentTypes` and cross-validated per row by
  `engine_unit_component_registry`; shared `serialization_util` codecs;
  reflection-backed components; registry row order is the serialized key
  order of both formats; `AnimationComponent` persists its whole authored
  set — controller path plus `playing` and `playbackSpeed` (#253) —
  through one string-or-object codec both formats share: a component still
  holding the default playing/speed writes the bare controller-path
  string, so pre-#253 files stay byte-identical and the scene version is
  unchanged, and that string remains readable as the legacy form; the
  runtime members (slots, current/previous state, blend timers, params)
  are never serialized), physics/
  scripting/editor bridges, render-prep pipeline, skeletal animation (CPU
  pose evaluation in `animation.cpp`, cooked .skel/.anim loaders, the
  controller state machine + palette handoff in `animation_system.cpp`),
  fixed-step render interpolation (per-entity world-TRS history in the
  World, blended in render prep; `frame_pacing.{h,cpp}` holds the
  vsync/cap helpers and the fixed-step count decision incl. the paused
  editor's single step), the single-slot game save (`save_data.{h,cpp}` over
  `platform_get_save_dir`), service registry, timers (runtime-only,
  per-scene state, never serialized — scripts re-arm them in
  `on_begin_play`; legacy scene `timers` blocks load but restore nothing,
  #209), cameras, spring
  arms, game mode/state, player controllers, entity pool. Authored
  `CameraComponent` (issue #161: projection/fov-or-orthographic-size/
  near/far/priority/blendSpeed/active) derives pose from the owning
  entity's transform (never stores its own) and `update_persistent_
  cameras` republishes it into the World's `CameraManager` priority stack
  every frame, so it is the CameraManager's normal scene-facing input
  alongside Lua-pushed and spring-arm cameras rather than a second camera
  stack; a same-entity `SpringArmComponent` supplies the pose (its
  collision-aware boom) while the CameraComponent supplies the lens/
  priority/blend, the standard authored third-person rig. Orthographic
  projection renders end to end (#221): one shared
  `camera_projection_matrix` serves the flush, render-prep culling, scene
  captures, and the editor frustum gizmo; cascaded shadows take the camera
  near/far explicitly; the lighting view vector switches to the camera
  forward under ortho; the sky pass keeps perspective directional sampling
  (parallel rays would sample one sky direction).
- `editor/` — ImGui editor: `editor_session` (state + play lifecycle,
  multi-selection, single-step request, embedded `ContentBrowserState`),
  hierarchy tree panel (drag-drop reparent through the undoable
  ReparentCommand), `editor_commands` (undoable edits incl. entity
  create/delete with persistent-id-preserving subtree restore and asset
  drag-spawn; commands resolve targets by persistent id; `execute_asset_open`
  dispatches the content browser's typed Open action through production
  entry points), `editor_asset_index` (issue #157: a cold, process-wide
  content-browser index — one filesystem walk classifies every file into
  AssetKind by extension/content-sniff and hides sidecar/internal files;
  rebuilt only on explicit Rescan or startup, never per frame; change-driven
  `AssetFilterCache`/`AssetChildFolderCache` recompute only when the
  request or the index generation changed), panel TUs (main/inspector/
  diagnostics/assets/viewport; the assets panel is the content browser:
  search/type-filter/folder-nav with persisted last-folder+filter state,
  thumbnails, and typed double-click/context-menu actions — mesh drag-spawn
  is unchanged and still pinned by `editor_asset_spawn_name_test`; Rename/
  Move/Duplicate/Delete/Reimport/Find Dependencies stay disabled pending
  #150's asset identity and dependency graph), editor + debug cameras,
  command history. Inspector metadata (issue #156):
  `editor_component_registry` generates `ComponentEditType`/
  `ComponentEditSnapshot`/capture-apply-remove dispatch from the runtime's
  `ENGINE_PERSISTENT_COMPONENT_TABLE` (X-macro over
  `runtime/src/component_registry.h`, reached via a `runtime/src` private
  include on the editor target) so a new persistent component cannot skip
  the Inspector; `editor_inspector_metadata` holds per-field/per-component
  display metadata (name/category/tooltip/range/units/widget kind incl.
  Color/AngleDegrees/EulerDegrees/LayerMask/Enum) plus the pure Euler-
  degrees<->quaternion helpers (storage stays quaternion; presentation
  recomputed every call, exact round trip incl. through the pitch = +-90
  gimbal pole); `editor_reference_pickers` holds searchable entity
  (World name/liveness) and asset (via two `runtime::editor_query_assets`/
  `editor_asset_display_path` bridge functions) reference pickers plus a
  VFS-path picker for script/controller fields, each with a broken-
  reference Clear action; `editor_panels_inspector_generic` is the
  metadata-driven reflected-field drawer and `editor_panels_inspector_custom`
  holds the typed drawers for fields core::TypeField cannot represent
  (64-bit asset ids, VFS paths: Mesh/Script/Animation/Foliage/Light-type/
  SceneCapture-preview) plus the registry-generated, categorized Add
  Component menu; `editor_panels_inspector` is a thin per-section
  orchestrator. Play-mode inspection (issue #159, LANDED 2026-08-17):
  `editor_live_edit` gives Play/Pause an explicit opt-in
  (`EditorSession::liveEditEnabled`, off by default) — with it off the
  Inspector shows the running World's live component values read-only
  (WorldPhase::Input holds between fixed steps and while paused, so the
  same capture path used at rest already reads live state); with it on,
  edits write straight to the running World through the same
  apply_component_snapshot dispatch but never enter CommandHistory or scene
  dirty state, and Stop's snapshot restore always discards them unless the
  author explicitly queues "Apply to authored value" (frozen at click
  time), which Stop replays as one ordinary undoable ComponentEditCommand
  against the just-restored authored World once the restore succeeds;
  "Revert runtime edit" restores the play-session baseline captured at the
  first live touch. `editor_multi_edit` extends the #156 registry-generated
  editing to N-entity selections: common-component detection is per
  ComponentEditType across the selection, per-field mixed-value detection
  reads core::TypeField offset/size so a "(mixed)" field never gets
  silently overwritten to one entity's value by an edit to a sibling field,
  and one field edit applies to every selected entity as a single
  atomic `MultiComponentEditCommand` (one gesture, one undo step; a
  mid-batch failure rolls every already-touched entity back and pushes no
  history entry). Both live-edit and multi-edit currently cover the
  reflected-field component set (Transform, RigidBody, Collider, Light,
  PointLight, SpotLight, ReflectionProbe, SpringArm); the custom-drawer
  components (Mesh, FoliagePatch, Script, Animation, SceneCapture, Name)
  stay Stop-only/single-entity-only pending a follow-up that threads their
  drawers' own structural sub-edits (asset picks, foliage instance add/
  remove) through the same per-field batch/live path.
- `assets/` — bgfx shaders (`shaders/bgfx/`: shaderc `.sc` sources,
  `varying.def.sc`, and the `shaders.json` cook manifest — #138 Phase C;
  the GLSL sources died with the GL backend, but each manifest entry's
  `output` stem is still the name passes request shaders by;
  bgfx builds cook them per profile into the build tree's
  `shaders/bgfx/cooked/` through `asset_packer --shader-manifest`, one
  cook stamp certifying the whole output set, and `shader_system`
  prefers the cooked binaries whenever the device's
  `caps.cookedPrograms` is set), sample Lua scripts, sample meshes, the bundled
  prop pack (`props/`, cooked from generated glTFs), sounds, and the Island
  Hopper template (`templates/island_hopper.json`, installed as
  `scene.json`) — synced to the build dir by CMake. `tools/` — asset_packer
  (deterministic cook, thumbnails, glTF mesh/skeleton/animation import,
  dependency graph), binding generator, asset generators (`gen_character`,
  `gen_props`, `gen_sounds`, `gen_island_scene`), comment audits, the
  module dependency audit (`check_module_deps.py`, #311), CI
  helpers (the gates' own self-tests run as
  `engine_integration_tool_gates`). `tests/` — unit / integration /
  smoke (`gpu` label) / benchmark + `test_harness.h`.
  `.github/workflows/ci.yml` — 11 jobs: canonical-toolchain build matrix
  (3 OS × 2 configs; clang-cl via VS ClangCL / clang / AppleClang, issue
  #130), MSVC + GCC Release compatibility lanes, determinism hash compare,
  static analysis + comment audits, clang-tidy, werror, ASAN/UBSAN, TSAN,
  coverage (≥50%), benchmarks (>10% regression fails), quality gate (the
  gl regression lane retired with the backend's deletion, #138). The
  canonical matrix builds bgfx incl. the shader cook (macOS excepted:
  tint's overloaded-CTAD pattern does not compile under AppleClang, and
  the headless-only lane never consumes cooked binaries, so it passes
  ENGINE_BGFX_SHADERC=OFF); sanitizer/analysis/
  werror/benchmark lanes pass ENGINE_BGFX_SHADERC=OFF.

## Architecture invariants

- Subsystem ownership (#168, landed 2026-08-18 across PRs #237-#240,
  #242-#244, #246): every mutable subsystem has one owner in a four-tier
  model. **Engine tier** — `engine::bootstrap`/`engine::shutdown` own core
  services (via `core::initialize_core`/`shutdown_core`, LIFO with
  partial-init rollback), the cvar/console tables, scripting VM, audio
  device, DAP, texture registry, renderer teardown, and the active
  `EngineConfig`. **Run tier** — `EnginePipeline::Impl` owns the World,
  asset database/manager/streaming, command buffer, mesh registry, service
  locator + registry, game-binding state, and published bridge services;
  its `teardown` is the single audit point for run residue and resets
  scripting run state, animation controllers, gameplay input bindings,
  scene audio content, and per-run public renderer state — a second
  pipeline run in one process starts clean (pinned by
  `engine_integration_pipeline_teardown_run_state`). **World tier** —
  `World` owns ECS/hierarchy/persistent ids, physics context, timers,
  cameras. **Editor tier** — `EditorSession` behind the bridge; a world
  rebind is a full session transition. Compatibility globals must be
  non-owning aliases set and cleared by their owner (the
  `runtime_binding`/`g_editorAssetService` pattern); no global may lazily
  resurrect a subsystem, and there is deliberately no process-global
  service locator. The single-engine-process assumption stands; pure
  caches and immutable config tables are exempt from mechanical
  de-globalization.
- Internal Entity = `{index, generation}`; index 0 invalid. Any entity handle
  that can outlive, cross, or be rebound between Worlds also carries and
  validates World identity. Generation reuse must not silently alias a stale
  handle within the supported lifetime; capacity and wrap behavior require
  explicit tests. Component mutation is only legal in `WorldPhase::Input`;
  writable transforms during Simulation require the
  `SimulationAccessToken`. Never break transform double-buffering or
  persistent-id behavior.
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
- Frame: every fixed step has one explicit dependency chain:
  `begin[n]` → all update/physics work → collision resolve → `commit[n]` →
  `begin[n+1]`. The happens-before relation must hold for zero jobs, disabled
  systems, submission failure, catch-up steps, and every worker count. After
  the last commit, transform propagation and camera/spring-arm publication
  run on the main thread before any render-prep job; then render-prep jobs
  fill per-thread command buffers merged for the backend flush.
  Preserve deterministic stepping and thread-count independence, and test the
  production pipeline rather than a copied scheduler model.
- Serialization has one authoritative persistent-component registry from
  which parse, copy, reset, migration, and codec coverage are generated or
  mechanically validated. Format changes require migrations and production-
  path tests. Parse/load/restore failure leaves the destination unchanged;
  scene loads stage into a replacement World and commit only on success.
- Public physics joint names follow their conventional degrees of freedom,
  anchor-frame, limit, and motor semantics. A simpler constraint must use a
  different public name. Correcting serialized joint behavior requires an
  explicit behavior version, migration policy, and before/after tests.
- Authored files use staged atomic replacement; related multi-file outputs
  use a manifest or transaction so interruption cannot create a mixed state.
- Renderer: command construction stays separate from backend execution, and
  code above the backend TU (`render_device_bgfx.cpp`) speaks only the
  engine-facing `RenderDevice` vocabulary (handles, descriptors, shader
  params, render state) — bgfx concepts must not reappear above the
  backend; device
  resources are owned by whichever system created them, destruction is
  immediate and idempotent, and `shutdown_render_device` invalidates every
  outstanding handle. The command-buffer backend still initializes lazily
  from the first flush, but only inside a lifetime its owner opened:
  `engine::bootstrap` calls `initialize_renderer`, `engine::shutdown`
  calls `shutdown_renderer`, and between the two a flush or probe bake is
  a logged no-op rather than a rebuild against the destroyed device
  (#318, contract #168 — no global may lazily resurrect a subsystem).
  Preserve
  forward fallback and transparency behavior when touching deferred paths;
  prefer CPU-verifiable renderer tests (GPU tests carry the `gpu` label).
- Scripting: don't break the Lua API without tests + doc updates; validate
  stack usage; preserve traceback, sandbox, and hot-reload behavior.
  Trust model (owner decision 2026-08-09, issue #83): v0.x scripts are
  author-local and trusted, but every script-reachable filesystem path is
  VFS-jailed as defence-in-depth (`core::vfs_path_is_jailed` via
  `script_path_in_jail`: relative, forward slashes, no drive designators,
  no `..`); untrusted-content isolation for shared creations arrives with
  the v0.5 web sandbox.
- Private headers in `src/` are the established pattern for module-internal
  APIs — keep using it; do not move them into `include/`.
- When adding shared utilities, put them in `core`. Migrate all in-scope
  duplicate call sites in the same series; if a safe full migration is too
  broad, enumerate the remaining sites and keep the deduplication finding
  explicitly open.

## Working conventions

- Review and design from first principles (owner directive 2026-08-25):
  derive every change from the product goal and the architecture invariants,
  not from what a demo, template, or symptom needs today. A fix addresses
  the root cause at the layer that owns it; a symptom-level patch at the
  nearest convenient layer is rejected in review even when it works.
- Small focused changes; one concern per commit; no drive-by rewrites; do not
  modify unrelated systems. Concise imperative commit messages.
- `git status` before editing; never overwrite uncommitted changes that are
  not yours; do not commit unless asked. Never delete source files or hide
  build failures.
- Verification per change: zero-warning build → headless ctest → targeted
  determinism/bench suites when the area is sensitive → both comment audits.
  A correctness fix needs a production-path regression that is red on the
  base revision and green on the proposed revision, plus applicable boundary
  cases. If that evidence is impossible, record why and do not claim closure.
- Every PR carries a scope/closure table for findings it references: fixed,
  partially fixed, deferred, or pre-existing, with the exact evidence for
  each status. Reviewers reject broader claims than the patch proves.
- Audit findings, bugs, and tech-debt are queued as GitHub issues through the
  structured templates in `.github/ISSUE_TEMPLATE/` (audit finding, bug
  report, weakness). Residual scope from a partial fix gets its own linked
  issue rather than living only in a PR body or a closed issue's comments;
  the tracker, not the audit reports, is the source of truth for open scope.
- Prefer `bool`+log, small status objects, or optional-like returns;
  assertions only for programmer errors.

## Product vision (2026-08-01, amended 2026-08-25 — priorities derive from this)

**Engine-first pivot (owner decision 2026-08-25).** The engine is the
product; the game is not. Island Hopper and the starter templates are
demoted from product deliverables to integration/test fixtures. Template
and script content bugs are frozen — they are fixed only when their root
cause is an engine defect, and then the fix lands in the engine with a
production-path regression, never as a content or script workaround.
Feature work driven by template needs (runtime UI, additional templates,
onboarding) is suspended. The active queue is engine hardening:
boundary/layering cleanup (scripting consumes the engine only through
sanctioned bridge APIs, never core or runtime internals), audit findings,
and the open tech-debt ladder. The hour-test north star below is deferred,
not deleted — it resumes only after the hardening campaign closes.

The engine has two co-equal goals: a high-end, next-generation-capable core
and an editor/Lua creation workflow usable by complete beginners. Stated
concretely (owner, 2026-08-25): **Unreal-level scene rendering with
Unity/Godot-level ease of use.** Both halves are engine qualities —
rendering depth is earned through correct, scalable foundations, and ease
of use through disciplined APIs, presets, and diagnostics — so neither is
pursued by patching around the other. Reference engines are a bar, not a
blueprint (owner, 2026-08-25): understand each mechanism from first
principles and improve on it; never import a reference engine's known
defects for familiarity's sake. Canonical example: Unity's
nondeterministic script execution order is explicitly rejected — this
engine's deterministic stepping, ordered lifecycle dispatch, and
registry-defined ordering are invariants, and any future
behavior/scheduling feature must preserve an explicit, deterministic,
author-visible order. Beginner
ease comes from strong defaults, presets, templates, validation, guidance,
undo, and recovery—not from weaker correctness, hidden ambiguity, or
nonstandard semantics. Advanced users must be able to inspect, profile,
override, and scale the same systems instead of graduating to a different
engine architecture.

Priorities are: correctness and user-data safety; a clear beginner creation
loop; scalable, physically coherent rendering/physics/runtime foundations;
measured performance budgets and quality tiers; a commercial-grade editor
with built-in blockout and starter content; and one-click sharing. Device
reach is delivered through explicit quality tiers and fallbacks, not by
setting a permanent ceiling on the high-end path. Platforms follow the
vision: Windows/Linux editor first, web export after the RHI migration, and
an iOS/iPadOS runtime proof. A macOS editor remains likely; macOS game
shipping and Android remain product decisions rather than commitments.

## Roadmap

Implemented foundations (inventory, not release certification; details live
in code, tests, and git history): build/CI/determinism/profiling baseline,
ECS + gameplay loop
(lifecycle, input incl. touch/rebinding, game mode/state, timers, cameras,
coroutines, DAP + sandbox + hot reload, binding generator), physics (all
collider shapes incl. capsule/hull/heightfield, warm-started contact solver,
six public joint APIs, manifolds, materials/layers, queries, CCD + speculative
contacts; the H-05 rework closed the standard hinge/fixed/slider rotational
and anchor-frame semantics gap 2026-08-01, joint motors remain unimplemented),
asset pipeline (64-bit ids, metadata/tags, dependency graph, async streaming
with budgets, LRU, deterministic cook + thumbnails), renderer through
deferred+forward, shadows (cascade/spot/point), sky (cubemap/Preetham/
procedural scatter — `procedural_sky.frag`, the default),
IBL + reflection probes, fog, instancing + foliage, post stack; 2026-07
production-hardening campaign (27 findings recorded as closed at that time).
Current audits and regressions supersede historical blanket closure claims;
any unresolved scope remains open until it meets the closure contract above.
2026-08 audit campaign: the 2026-08-01 audit and 2026-08-03 follow-up were
closed across PRs #48–#52 and #58–#77 (merged 2026-08-01 through
2026-08-04), each fix carrying red-on-base production-path regressions per
the closure contract; the finding→PR ledger and git-history permalinks to
the full reports live in reference issue #87 (the repo carries no audit
report files — per the tracking policy, findings live only on the issue
tracker), and all residual scope and pending owner decisions are tracked
as open GitHub issues (#78–#86 as of 2026-08-09).

Roadmap status labels describe integration state, not release certification.
`LANDED` means the named change is present on the stated date; it is
`VERIFIED` or `PRODUCTION-READY` only when the required commit, production-
path test evidence, platform scope, and acceptance result are also recorded.

North star — the v1.0 acceptance test (2026-07-30; **DEFERRED 2026-08-25**
per the engine-first pivot above — it resumes when the hardening campaign
closes; priorities derive from the product vision above):

> A person with zero game-dev experience opens the editor, follows one
> starter template, and shares a playable web link of their own variation
> within an hour.

Everything ships in three outcome slices, each with an acceptance demo. A
slice is done when its demo passes, not when its feature list is exhausted.

**The reference slice** (2026-08-25: now an integration/test fixture, not
a deliverable — see the engine-first pivot; it stays in the tree because it
exercises the whole stack at once, and its bug list is triaged for engine
root causes only): *Island Hopper*, a small
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
  VFS-resolved loose-file streaming music (archive-backed streaming is
  still pending), and Lua audio (`engine.play_sound_at`,
  `set_bus_volume`, `play_music`, `stop_music`); the sample scene's
  character walks with footstep animation events playing positional
  sounds over a streamed ambient loop. CUT: HRTF, DSP
  (reverb/filters/occlusion), Doppler, snapshots/ducking.
- **Editor (from P1-M9, cut down) — LANDED 2026-08-01**: scene hierarchy
  panel (tree, drag-drop reparent, multi-select); undoable entity
  create/delete — delete captures the transform subtree as per-member
  component snapshots and undo re-creates every member under its
  original persistent id, and all undo commands re-target entities by
  persistent id so redo chains survive a delete/re-create round trip;
  asset browser drag-to-viewport spawn (.mesh entries drop onto the
  scene at the ground-plane hit and request their async load through
  the editor bridge's published asset service); the full foliage
  instance list with undoable per-entry add/remove (the nested-array
  driving case; nested structs render via the reflection field kinds);
  PIE pause plus a Step button that simulates exactly one fixed step
  while paused (`fixed_step_decision` in frame_pacing); gizmo edits
  were already undoable via TransformEditCommand. The commercial-grade
  UX pass continues throughout. CUT: prefab overrides/nesting, editor
  Lua API.
- **Scenes (from P1-M10, cut down) — LANDED 2026-08-01**: the exclusive
  scene transition flow is pinned end to end by an integration test
  (`engine.load_scene` from a playing script → pending-op commit →
  scene B `on_begin_play` refires → `engine.new_scene` teardown);
  cross-scene state rides Lua globals, which survive transitions because
  the VM persists across scene loads (the templates' handoff channel).
  `process_pending_scene_op` (`runtime/src/engine_pipeline.cpp`) dispatches
  `on_end_play` to every outgoing-scene scripted entity before either op
  commits, matching editor Stop (audit #198, closed 2026-08-16); a failed
  load skips the dispatch along with the rest of the reset, and a handler
  that reenters with its own `load_scene`/`new_scene` call is rejected
  with a logged warning rather than corrupting the transition already in
  flight (`tests/integration/scene_transition_end_play_test.cpp`). And the
  single save slot lands as `engine.save_data(table)` /
  `engine.load_data()` — a flat string-keyed table (number/string/bool)
  as JSON in the per-user platform save directory
  (`runtime/save_data.{h,cpp}`, `platform_get_save_dir`). CUT: additive
  scenes, streaming volumes, LOD hysteresis, multi-slot saves.
- **Creator kit v1 (elevated by the vision) — LANDED 2026-08-01**:
  blockout authoring (the Entities panel's Add Primitive menu spawns the
  built-in shapes as undoable static scene objects with matching
  colliders — cylinder/pyramid hull payloads are rebuilt on redo; a
  toolbar Snap toggle drives ImGuizmo grid/angle snapping; the mesh
  inspector applies named material presets as undoable edits); the
  bundled asset pack (`tools/gen_props.py` → 21 cooked blocky props in
  `assets/props/`, `tools/gen_sounds.py` → 15 deterministic WAVs, plus
  the rigged character); and the Island Hopper starter template
  (`tools/gen_island_scene.py` → `assets/templates/island_hopper.json`,
  also installed as `assets/scene.json` so File → Load Scene → Play runs
  it out of the box): island blockout, coins + bonus gem, hop route with
  moving platform and falling-rock hazard, goal flag with best-time save
  via `engine.save_data`, and the animated character with follow camera
  (`assets/scripts/island_*.lua`, `moving_platform.lua`,
  `falling_rock.lua`). The v1.0 "new project from template" flow will
  formalize template instantiation. CUT: CSG tools.
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

### v1.0 — "The hour test" (SUSPENDED 2026-08-25, engine-first pivot)

This slice is suspended until the engine-hardening campaign closes; its
feature list below is preserved for when the hour test resumes. Until
then, active work is the hardening queue: the scripting/core boundary
cleanup (#309 scripting<->runtime cycle + bridge migration, #310
scripting->physics sideways edge, #311 mechanical dependency gate + CMake
visibility, #312 SDL containment — from the 2026-08-25 audit campaign,
tracked on #226), the campaign's robustness findings (#313-#318), and the
tech-debt ladder (serialization enforcement #252/#253, asset identity
#172/#179, physics correctness #201/#169, quality gates #180, the
remaining god-file splits, #131).

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
