# Engine Project Index

This is the stable repository map for Codex, clangd, and engine contributors.
Update it when module responsibilities, dependency direction, build commands,
test layout, indexing setup, or major runtime/renderer/physics/scripting/editor
architecture changes.

## Project Summary

Engine is a C++20 game engine with an editor application, runtime ECS/world,
OpenGL renderer, physics simulation, Lua scripting, audio playback, asset
tooling, and CI coverage. The project is still productionizing. Current open
lanes include full animation, runtime UI, platform packaging, production
operations, advanced audio, editor completion, and save/streaming systems.

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

## Module Map

- `app`: `engine_editor_app` entry point; links `engine_runtime` and whole-archives
  `engine_editor` so the editor bridge registers before bootstrap.
- `core`: bootstrap, platform paths, logging, cvars, console, event bus, input,
  touch, VFS, JSON, jobs, allocators, profiler, memory tracker, reflection, and
  service locator.
- `math`: vectors, matrices, quaternions, transforms, bounds, rays, spheres, and
  shared component POD types.
- `physics`: rigid bodies, colliders, convex hulls, CCD, contacts, queries,
  joints, materials, and `PhysicsWorldView` so physics does not depend on
  runtime.
- `scripting`: Lua runtime, generated binding pipeline, entity scripts,
  timers, coroutines, sandboxing, hot reload, DAP debugger, and runtime binding
  owner modules.
- `renderer`: asset database/manager/streaming, mesh and texture loading,
  shader variants, render device table, command buffers, deferred/forward
  passes, shadows, post-processing, light culling, GPU profiler, and caches.
- `audio`: miniaudio-backed sound loading, playback, stop, volume, pitch, loop,
  and frame update.
- `runtime`: public `engine::bootstrap/run/shutdown`, `EnginePipeline`, World
  ECS, physics bridge, scripting bridge, render preparation, scene/prefab
  serialization, services, timers, cameras, game mode/state, and entity pool.
- `editor`: ImGui editor integration, editor/debug cameras, command history,
  play/pause/stop controls, gizmos, thumbnails, and editor-runtime bridge.
- `tools`: asset packer, dependency graph, glTF mesh import, glTF skeleton skin
  parsing, binding generator, source comment audit, and CI helper scripts.
- `assets`: sample Lua scripts, sample mesh, and GLSL shader assets.
- `tests`: unit, integration, smoke, benchmark, and sanitizer suppressions.

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
- `engine_math` depends on `engine_core`.
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

## Editor Entry Points

- `editor/include/engine/editor/editor.h` exposes editor lifecycle, event,
  render, world binding, and play-state APIs.
- `runtime/include/engine/runtime/editor_bridge.h` is the runtime-facing bridge.
- `app/CMakeLists.txt` whole-archives `engine_editor` so bridge registration is
  linked even when no direct app symbols reference editor code.
- Editor command history is in `editor/include/engine/editor/command_history.h`.

## Renderer Pipeline Summary

- Render frontend data is prepared in runtime render-prep code and submitted
  through renderer command buffers.
- `RenderDevice` is a backend function table; current implementation is OpenGL
  in `renderer/src/render_device_gl.cpp`.
- Deferred path uses G-buffer shader assets, deferred lighting, tiled light
  culling, and forward fallback/transparency.
- Post-processing stack includes bloom, SSAO, auto exposure, tone mapping, and
  FXAA.
- Shadow support includes directional cascades, spot maps, and point cubemaps.
- Asset systems include mesh/texture loading, metadata, dependency tracking,
  async streaming, and LRU eviction.

## Physics Pipeline Summary

- `runtime::World` stores transforms, rigid bodies, colliders, movement
  authority, and `PhysicsContext`.
- Physics steps through runtime bridge calls, using `PhysicsWorldView` instead
  of a runtime dependency.
- Broadphase, collision detection, speculative contacts, CCD, manifold storage,
  solver, and joints are CPU-testable.
- Convex hull and heightfield shape payloads live in `PhysicsContext`, scoped to
  the owning world.

## Scripting API Summary

- Public Lua runtime APIs live in `scripting/include/engine/scripting/`.
- `bindable_api.h` is the generator input for annotated `LUA_BIND` accessors.
- `scripting/src/scripting.cpp` registers the main `engine` table and delegates
  ownership-heavy behavior into focused binding modules.
- Runtime integration is through `runtime/src/scripting_bridge.cpp` and explicit
  service registration, not global runtime ownership.
- Lua systems include entity lifecycle callbacks, timers, coroutines, scene
  operations, input, physics callbacks, sandbox limits, hot reload, and DAP.

## Asset Pipeline Summary

- Runtime assets are mounted through core VFS.
- `tools/asset_packer` converts glTF/GLB mesh primitives into `.mesh` files and
  metadata sidecars, computes dependency graphs, cook stamps, thumbnails, and
  convex hull sidecars.
- New animation groundwork: `tools/asset_packer/skeleton_import.*` parses glTF
  skins into `Skeleton` data with joint parent indices and inverse bind
  matrices.
- Renderer asset metadata tracks type tags, paths, tags, dependencies, import
  settings, checksums, size, and modification times.
- Shader assets live under `assets/shaders/`; Lua sample scripts live under
  `assets/`.

## Test Map

- `tests/unit`: focused module tests for core, math, physics, renderer, runtime,
  scripting, editor command history, audio, tooling, and new skeleton import.
- `tests/integration`: cross-module ECS, lifecycle, determinism, scripting,
  coroutine, sandbox, hot reload, DAP, asset streaming, and dependency tests.
- `tests/smoke`: `engine_smoke` high-level boot path, labelled `gpu`.
- `tests/benchmark`: ECS, physics, and instancing performance targets.
- CI runs build matrix, non-GPU CTest, determinism comparison, source comment
  audit, cppcheck, clang-tidy, Werror, sanitizers, coverage, and benchmarks.

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
3. `runtime/src/scene_serializer.cpp`
4. `runtime/src/prefab_serializer.cpp`
5. `tests/unit/runtime_world_test.cpp`
6. ECS integration and stress tests

For renderer:

1. `renderer/include/engine/renderer/`
2. `renderer/src/render_settings.cpp`
3. `renderer/src/command_buffer*.cpp`
4. `renderer/src/render_device_gl.cpp`
5. `renderer/src/shader_system.cpp`
6. `renderer/src/mesh_loader.cpp`
7. Renderer unit tests
8. `assets/shaders/`

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

1. `.codex/skills/engine-animation-work/SKILL.md`
2. `tools/asset_packer/skeleton_import.h`
3. `tools/asset_packer/skeleton_import.cpp`
4. `tests/unit/skeleton_import_test.cpp`
5. Renderer shader variant tests and skinned shader assets when adding GPU
   skinning
