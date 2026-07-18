# Engine Project Index

This is the stable repository map for Codex, clangd, and engine contributors.
Update it when module responsibilities, dependency direction, build commands,
test layout, indexing setup, or major runtime/renderer/physics/scripting/editor
architecture changes.

## Project Summary

Engine is a C++20 game engine with an editor application, runtime ECS/world,
OpenGL renderer, physics simulation, Lua scripting, audio playback, asset
tooling, and CI coverage. The project is still productionizing, but the
current tree already has deterministic ECS/runtime tests, a decomposed frame
pipeline, async asset streaming, deferred/forward rendering, editor play
controls, and Lua integration. Current open lanes tracked in `TODO.md` include
full animation, runtime UI, platform packaging/project workflow, release
operations, advanced audio, editor completion, and save-game/content streaming
systems. A production-hardening campaign (bug/perf/duplication/architecture
findings from the 2026-07-17 full-codebase review) is tracked separately in
`REVIEW_FINDINGS.md`.

## Tech Stack

- Language: C++20, exceptions disabled, RTTI disabled.
- Build: CMake 3.28+ with Ninja recommended on Windows.
- Window/input: SDL2.
- Rendering: OpenGL, GLSL shader assets under `assets/shaders/`.
- Editor UI: ImGui and ImGuizmo.
- Scripting: Lua 5.4 plus generated and hand-written bindings.
- Audio: miniaudio.
- Asset import/cooking: `tools/asset_packer`, cgltf, stb.
- Tests: CTest unit, integration, smoke, and benchmark targets.

## Build Commands

Preferred Windows clang-cl configure:

```powershell
cmake -S . -B build `
  -G Ninja `
  -DCMAKE_C_COMPILER=clang-cl `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
  -DCMAKE_BUILD_TYPE=Debug
```

Build and test:

```powershell
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Useful subsets:

```powershell
ctest --test-dir build --output-on-failure -R engine_unit_
ctest --test-dir build --output-on-failure -R engine_integration_
ctest --test-dir build --output-on-failure -R engine_smoke
ctest --test-dir build --output-on-failure -R engine_bench_
```

## clangd Indexing Setup

- Source of truth: `build/compile_commands.json`.
- Current local check: `build/compile_commands.json` exists.
- `.clangd` points `CompileFlags.CompilationDatabase` at `build`.
- Do not manually create or edit `compile_commands.json`; regenerate it with
  CMake and `CMAKE_EXPORT_COMPILE_COMMANDS=ON`.

## CMake Options

- `ENGINE_TARGET_PLATFORM`: `Win64`, `Linux`, `macOS`, `Android`, `iOS`, `Web`.
- `ENGINE_DETERMINISTIC_FLOATS`: strict floating-point flags, default `ON`.
- `ENGINE_MAX_ENTITIES`: compile-time ECS capacity, default `65536`.
- `ENGINE_BUILD_TESTS`: build tests, default `ON`.
- `ENGINE_BUILD_TOOLS`: build tools, default `ON`.
- `ENGINE_SANITIZERS`: ASAN/UBSAN on supported non-MSVC compilers, default `OFF`.
- `ENGINE_REQUIRE_ANALYSIS_TOOL`: make `analysis` fail when no analyzer exists.
- `ENGINE_ASSET_SOURCE_DIR` and `ENGINE_ASSET_OUTPUT_DIR`: local asset sync.

CMake helper macros in `cmake/EngineHelpers.cmake` define the common static
module library, header-only (INTERFACE) library, executable, and CTest
executable patterns. Module libraries are static and use explicit
public/private include and link dependency lists; `engine_math` is header-only
via `engine_add_header_library`.

## Module Map

- `app`: `engine_editor_app` entry point; links `engine_runtime` and whole-archives
  `engine_editor` so the editor bridge registers before bootstrap.
- `core`: bootstrap, platform/window paths, logging, cvars, console, event bus,
  input, input maps, touch, VFS, JSON, jobs/frame graph, allocators, profiler,
  memory tracker, reflection, entity handles, service locator, and shared
  data-structure/utility headers (`sparse_set.h`, `fixed_hash_table.h`,
  `hash.h` FNV-1a, `string_util.h` bounded copy).
- `math`: vectors, matrices, quaternions, transforms, bounds, rays, spheres, and
  shared component POD types; header-only — every function is inline in the
  public headers, with SSE2 paths in `math_detail.h` (no `math/src/`).
- `physics`: rigid bodies, colliders, convex hulls, heightfields, CCD,
  speculative contacts, contact manifolds, queries, joints, materials, and
  `PhysicsWorldView` so physics does not depend on runtime.
- `scripting`: Lua runtime, generated binding pipeline, entity scripts,
  timers, coroutines, input/touch/game/scene bindings, sandboxing, hot reload,
  profiler/debugger hooks, DAP debugger, and runtime binding owner modules.
- `renderer`: asset database/manager/streaming, mesh and texture loading,
  procedural mesh primitives (`mesh_primitives.h`), shader variants, render
  device table, command buffers, deferred/forward passes, skybox/environment
  maps, reflection probe baking, distance/height fog, shadows, post-processing,
  light culling, GPU profiler, and caches.
- `audio`: miniaudio-backed sound loading, playback, stop, volume, pitch, loop,
  and frame update.
- `runtime`: public `engine::bootstrap/run/shutdown`, `EnginePipeline`, World
  ECS, physics bridge, scripting bridge, render preparation, scene/prefab
  serialization, services, timers, cameras, spring arms, player controllers,
  game mode/state, and entity pool.
- `editor`: ImGui editor integration, editor/debug cameras, command history,
  play/pause/stop controls, gizmos, thumbnails, and editor-runtime bridge.
- `tools`: asset packer, dependency graph, glTF mesh import, glTF skeleton skin
  parsing, glTF animation clip parsing, binding generator, source comment
  presence audit (`check_source_comments.py`), comment quality audit
  (`check_comment_quality.py`), and CI helper scripts.
- `assets`: sample Lua scripts, sample mesh, and GLSL shader assets.
- `tests`: unit, integration, smoke, benchmark, performance baselines, and
  sanitizer suppressions.
- `.github/workflows`: CI build/test/static-analysis/sanitizer/coverage and
  benchmark automation plus platform dependency setup scripts.

## Dependency Graph

Preferred dependency direction:

```text
app
  -> editor
  -> runtime
  -> renderer / physics / scripting / audio
  -> core / math
```

Actual target relationships from CMake:

- `engine_core` depends on SDL2 and Threads.
- `engine_math` is a header-only INTERFACE library that depends on `engine_core`.
- `engine_physics` publicly depends on `engine_math` and privately on
  `engine_core`.
- `engine_scripting` depends on `engine_core`, `engine_math`, `engine_lua`, and
  privately `engine_physics`; it includes runtime headers privately for bridge
  declarations.
- `engine_renderer` depends on `engine_core`, `engine_math`, and OpenGL.
- `engine_audio` depends on `engine_core` and `engine_math`.
- `engine_runtime` depends on core, math, physics, scripting, renderer, audio,
  SDL2, and OpenGL.
- `engine_editor` depends on ImGui, core, math, runtime, and renderer.
- `asset_packer` depends on core, math, physics, cgltf, and stb.

## Runtime Entry Points

- `app/main.cpp` calls the public runtime API.
- `runtime/include/engine/engine.h` exposes `EngineConfig`, `bootstrap`,
  `active_config`, `run`, and `shutdown`.
- `runtime/src/engine.cpp` owns high-level bootstrap/shutdown glue.
- `runtime/include/engine/runtime/engine_pipeline.h` and
  `runtime/src/engine_pipeline.cpp` own the decomposed frame loop stages.
- `runtime/include/engine/runtime/world.h` is the central ECS/world API and
  implements `physics::PhysicsWorldView`.
- `runtime/include/engine/runtime/camera_manager.h`,
  `spring_arm_update.h`, `game_mode.h`, `game_state.h`, and
  `player_controller.h` provide gameplay-facing camera and state scaffolding.
- `runtime::process_editor_input_event` routes SDL events through the editor
  bridge before gameplay input mutates core input state.

## Editor Entry Points

- `editor/include/engine/editor/editor.h` exposes editor lifecycle, event,
  render, world binding, and play-state APIs.
- `runtime/include/engine/runtime/editor_bridge.h` is the runtime-facing bridge.
- `app/CMakeLists.txt` whole-archives `engine_editor` so bridge registration is
  linked even when no direct app symbols reference editor code.
- Editor command history is in `editor/include/engine/editor/command_history.h`.

## Renderer Pipeline Summary

- Render frontend data is prepared in runtime render-prep code and submitted
  through fixed-capacity renderer command buffers.
- Command buffers sort by packed draw keys and support static mesh/foliage
  batching without heap allocation in the builder.
- `RenderDevice` is a backend function table; current implementation is OpenGL
  in `renderer/src/render_device_gl.cpp`.
- Deferred path uses G-buffer shader assets, deferred lighting, tiled light
  culling, and forward fallback/transparency.
- Environment rendering includes skybox cubemaps, prefiltered environment
  maps, irradiance maps, BRDF LUT handling, reflection probe bake settings,
  distance fog, and height fog normalization.
- Post-processing stack includes bloom, SSAO, auto exposure, tone mapping, and
  FXAA.
- Shadow support includes directional cascades, spot maps, and point cubemaps.
- Editor scene rendering can read back the tonemapped scene viewport texture.
- Asset systems include mesh/texture loading, metadata, dependency tracking,
  async streaming, and LRU eviction.

## Physics Pipeline Summary

- `runtime::World` stores transforms, rigid bodies, colliders, movement
  authority, and `PhysicsContext`.
- Physics steps through runtime bridge calls, using `PhysicsWorldView` instead
  of a runtime dependency.
- Broadphase, collision detection, speculative contacts, CCD, manifold storage,
  solver, sleep/wake behavior, materials, collision layers, and joints are
  CPU-testable.
- Convex hull and heightfield shape payloads live in `PhysicsContext`, scoped to
  the owning world.

## Scripting API Summary

- Public Lua runtime APIs live in `scripting/include/engine/scripting/`.
- `bindable_api.h` is the generator input for annotated `LUA_BIND` accessors.
- `scripting/src/scripting.cpp` registers the main `engine` table and delegates
  ownership-heavy behavior into focused binding modules.
- Runtime integration is through `runtime/src/scripting_bridge.cpp` and explicit
  service registration, not global runtime ownership.
- Generated bindings cover frame time, input/action queries, game mode/state,
  entity liveness/light queries, camera FOV, and audio controls.
- Hand-written Lua systems include entity lifecycle callbacks, timers,
  coroutines, async asset requests, scene operations, input/touch hooks,
  physics callbacks, sandbox limits, hot reload, profiler/debugger APIs, and
  DAP transport.

## Asset Pipeline Summary

- Runtime assets are mounted through core VFS.
- `tools/asset_packer` converts glTF/GLB mesh primitives into `.mesh` files and
  metadata sidecars, computes dependency graphs, cook stamps, thumbnails, and
  convex hull sidecars.
- New animation groundwork: `tools/asset_packer/skeleton_import.*` parses glTF
  skins into `Skeleton` data with joint parent indices and inverse bind
  matrices, and `tools/asset_packer/animation_import.*` parses transform
  animation channels into `AnimClip` tracks.
- Renderer asset metadata tracks type tags, paths, tags, dependencies, import
  settings, checksums, size, and modification times.
- `AssetStreamingQueue` owns worker threads for CPU-side loading and performs
  budgeted main-thread upload transitions.
- Shader assets live under `assets/shaders/`; Lua sample scripts live under
  `assets/`.

## Test Map

- `tests/unit`: focused module tests for core, math, physics, renderer, runtime,
  scripting, editor command history, audio, tooling, platform paths, engine
  pipeline routing, environment rendering helpers, fog normalization, skeleton
  import, animation clip import, sparse-set and fixed-hash-table storage,
  world name lookup, and job-handle generation wraparound.
- `tests/integration`: cross-module ECS, lifecycle, determinism, scripting,
  coroutine, sandbox, hot reload, generated bindings, DAP, camera/game mode,
  asset streaming, and dependency tests.
- `tests/smoke`: `engine_smoke` high-level boot path, labelled `gpu`.
- `tests/benchmark`: ECS, physics, and instancing performance targets with
  `tests/benchmark/perf_baseline.json`.
- CI runs build matrix, non-GPU CTest, determinism comparison, source comment
  audit, cppcheck, clang-tidy, Werror, sanitizers, coverage, and benchmarks.

## Known Risks

- `build/compile_commands.json` exists locally and `.clangd` uses it, but it is
  generated state; rerun CMake after CMake graph, compiler, or option changes.
- GPU-labelled smoke/integration tests depend on graphics context availability;
  prefer CPU-verifiable renderer tests for CI-safe coverage.
- Runtime/ECS, serialization, Lua API, physics, and render-prep changes are
  determinism-sensitive and should be paired with focused tests.
- Large same-domain implementation files remain in scripting
  (`scripting.cpp`) and editor (`editor.cpp`); their opportunistic split is
  finding A3 in `REVIEW_FINDINGS.md`. The renderer command-buffer god file
  was split (A1) into context/builder/math/sky/ibl/post-resources/flush
  translation units; `command_buffer_flush.cpp` remains large but is one
  same-domain pass driver. Keep future edits narrow and avoid mixing
  ownership changes with feature work.
- Comment quality is CI-enforced: `tools/check_source_comments.py` (presence)
  and `tools/check_comment_quality.py` (no filler patterns) both run in the
  static-analysis job and must stay at zero findings.
- Public headers should continue avoiding SDL/OpenGL/Lua/ImGui/ImGuizmo type
  leaks; inspect new headers for module-boundary drift.
- `TODO.md` is the source of truth for open production lanes;
  `REVIEW_FINDINGS.md` is the active tracker for the production-hardening
  fix campaign.

## Suggested Reading Order

For general work:

1. `README.md`
2. `AGENTS.md`
3. `.clangd`
4. `PROJECT_INDEX.md`
5. Root `CMakeLists.txt`
6. Target module `CMakeLists.txt`
7. Public headers for the target subsystem
8. Implementation files for the target subsystem
9. Related unit tests
10. Related integration tests

For runtime/ECS:

1. `runtime/include/engine/runtime/world.h`
2. `runtime/src/world.cpp`
3. `runtime/include/engine/runtime/engine_pipeline.h`
4. `runtime/src/engine_pipeline.cpp`
5. `runtime/src/scene_serializer.cpp`
6. `runtime/src/prefab_serializer.cpp`
7. `tests/unit/runtime_world_test.cpp`
8. ECS integration and stress tests

For renderer:

1. `renderer/include/engine/renderer/`
2. `renderer/src/render_settings.cpp`
3. `renderer/src/command_buffer*.cpp`
4. `renderer/src/render_device_gl.cpp`
5. `renderer/src/shader_system.cpp`
6. `renderer/src/mesh_loader.cpp`
7. `renderer/src/post_process_stack.cpp`
8. Renderer unit tests
9. `assets/shaders/`

For scripting:

1. `scripting/include/engine/scripting/`
2. `scripting/src/scripting.cpp`
3. Focused binding source files under `scripting/src/`
4. `tools/binding_generator/generate_bindings.py`
5. Lua scripts under `assets/`
6. Scripting unit and integration tests

For physics:

1. `physics/include/engine/physics/`
2. `physics/src/physics.cpp`
3. Collision, CCD, manifold, query, and joint source files
4. Physics unit tests
5. Determinism tests

For animation work:

1. `tools/asset_packer/skeleton_import.h`
2. `tools/asset_packer/skeleton_import.cpp`
3. `tools/asset_packer/animation_import.h`
4. `tools/asset_packer/animation_import.cpp`
5. `tests/unit/skeleton_import_test.cpp`
6. `tests/unit/animation_import_test.cpp`
7. Renderer shader variant tests and skinned shader assets when adding GPU
   skinning
