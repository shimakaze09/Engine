# Engine

An open-source C++20 game engine.

The repository is not production-complete yet. Game authors primarily work through Lua scripts and the editor. Engine contributors extend core systems in C++ under strict performance, safety, and correctness constraints.

## What this repository contains

- A runnable editor application: `engine_editor_app`
- Runtime systems for ECS/world simulation, rendering, physics, audio, and scripting
- Lua 5.4 gameplay scripting bridge (`engine` Lua API)
- Generated Lua binding pipeline for annotated scripting accessors
- Asset examples under `assets/`
- Test suites (unit, integration, smoke, benchmark) wired into CTest
- Tooling for mesh conversion (`asset_packer`)
- GitHub Actions CI under `.github/workflows/ci.yml`

## Core goals

- Keep the engine usable by non-programmers through scripting and editor-driven workflows
- Maintain predictable runtime behavior (no exceptions, no RTTI, explicit error paths)
- Keep module dependencies explicit and strictly downward

## Current verified state

The engine has strong foundations, but it is still being delivered milestone by milestone.

Verified working areas in the current tree include:

- Core systems such as logging, CVars, debug draw, job system, event bus, VFS, and math primitives
- Runtime ECS/world simulation with SparseSet storage, double-buffered transforms, scene serialization, persistent IDs, and 65,536-entity capacity
- Forward rendering with PBR shaders, frustum culling, mesh loading, and editor integration
- Physics basics including rigid bodies, colliders, spatial broadphase, CCD, and distance joints
- Editor play/pause/stop flow, gizmo transforms, and transform undo support
- Audio playback via miniaudio with wav/mp3/ogg/flac support plus volume, pitch, and loop control
- Lua module loading, traceback-based error reporting, generated and hand-written bindings, per-World timers, coroutine helpers, sandbox controls, and hot-reload coverage
- GitHub Actions CI for multi-platform build/test, determinism comparison, `cppcheck`, and `clang-tidy`

The engine is still forward-only, asset streaming and eviction are not yet fully production-grade, 3D audio and mixer buses are incomplete, and large roadmap items such as full animation production, advanced rendering, and platform shipping work remain open in `production_engine_milestones.md`.

## Tech stack

- Language: C++20
- Build: CMake 3.28+
- Window/input: SDL2
- Rendering: OpenGL (GLSL 330 core shaders)
- UI/editor: ImGui + ImGuizmo
- Scripting: Lua 5.4 (C API)
- Audio: miniaudio

Most third-party dependencies are fetched automatically via CMake `FetchContent` when not found locally.

## Repository layout

- `app/`: executable entry point (`engine_editor_app`)
- `core/`: platform, input, job system, logging, reflection base, VFS
- `math/`: math primitives and transforms
- `physics/`: simulation and collision stepping
- `renderer/`: mesh, texture, shader, command buffer, GL backend
- `audio/`: runtime audio services
- `scripting/`: Lua runtime and engine bindings
- `runtime/`: engine bootstrap/run loop, world/ECS, scene and prefab serialization
- `editor/`: editor integration, camera, command history
- `assets/`: scripts, shaders, and sample content
- `tests/`: unit, integration, smoke, and benchmark tests
- `tools/asset_packer/`: glTF/GLB to engine mesh conversion utility
- `.github/workflows/`: CI definitions

## Build prerequisites

- CMake 3.28+
- Python 3 (required for generated Lua bindings during configure/build)
- A C++20 compiler
	- MSVC (Windows) or
	- Clang/GCC (Linux/macOS)
- OpenGL development support

Notes:

- SDL2 is discovered with `find_package(SDL2 CONFIG QUIET)` first, then fetched from source if unavailable.
- First configure/build may need internet access due to dependency fetches.

## Quick start

From repository root:

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the app after build:

- Windows: `build\engine_editor_app.exe`
- Linux/macOS: `./build/engine_editor_app`

Build benchmark targets as needed:

```powershell
cmake --build build --target engine_bench_ecs_perf
cmake --build build --target engine_bench_physics_perf
```

## Running tests

Run all tests:

```powershell
ctest --test-dir build --output-on-failure
```

Run a subset by name pattern:

```powershell
ctest --test-dir build --output-on-failure -R engine_unit_
ctest --test-dir build --output-on-failure -R engine_integration_
ctest --test-dir build --output-on-failure -R engine_smoke
ctest --test-dir build --output-on-failure -R engine_bench_
```

The suite includes targets such as:

- `engine_unit_foundation`
- `engine_unit_math`
- `engine_unit_runtime_world`
- `engine_integration_ecs`
- `engine_integration_vertical_slice`
- `engine_integration_determinism`
- `engine_integration_thread_count_determinism`
- `engine_integration_coroutine`
- `engine_integration_timer`
- `engine_smoke`
- `engine_bench_ecs_perf`
- `engine_bench_physics_perf`

Some tests are labeled `gpu`; CI excludes those where headless execution is required.

## Continuous integration

GitHub Actions configuration lives in `.github/workflows/ci.yml` and currently runs:

- Windows, Linux, and macOS builds in Debug and Release
- CTest runs with headless-safe filtering
- Cross-platform determinism hash comparison
- `cppcheck`
- `clang-tidy`

Sanitizer, coverage, and automated performance-threshold gates are still separate follow-up work.

## Lua gameplay scripting

The runtime exposes an `engine` table to Lua scripts.

Current script conventions in `assets/`:

- Scene-level module (`assets/main.lua`)
	- `M.on_start(self)` is called once when play starts
	- `M.on_update(self, dt)` is called every simulation step
- Entity behavior module example (`assets/scripts/player.lua`)
- Reusable utility module example (`assets/lib/utils.lua`)

Current scripting/runtime support in the tree includes:

- Spawning entities
- Setting transforms and materials
- Adding rigid bodies and colliders
- Reacting to key input and collisions
- Scheduling timers with `engine.set_timeout()` and `engine.set_interval()`
- Coroutine helpers such as `engine.wait()`, `engine.wait_frames()`, and `engine.wait_until()`
- Sandbox, generated binding, and hot-reload coverage in integration tests

The scripting surface is still evolving. Some APIs are generated from annotated accessors, while much of the engine-facing Lua surface is still hand-written in `scripting/src/scripting.cpp`.

## Assets and mesh conversion

The runtime loads assets from `assets/` (copied into the build output by CMake).

For mesh conversion, build and run `asset_packer`:

```powershell
cmake --build build --target asset_packer
build\tools\asset_packer\asset_packer.exe <input.gltf|input.glb> <output.mesh>
```

Tool behavior:

- Reads the first mesh primitive from the source file
- Writes engine mesh binary (`.mesh`)
- Writes metadata sidecar (`.meta.json`)

## Engine contributor rules

- Use C++20 only (no compiler extensions)
- Do not use exceptions, RTTI, `dynamic_cast`, or `typeid`
- Keep engine API functions `noexcept`
- Use explicit return values plus logging for runtime failures
- Keep dependency flow strictly downward; do not introduce upward or sideways cycles
- Do not heap-allocate on hot paths
- Keep public headers self-contained and free of SDL, OpenGL, Lua, and ImGui types

If you modify core behavior in math, ECS/runtime, physics, renderer/mesh loading, reflection, or scripting, add or update tests in `tests/`.

## Troubleshooting

- Configure fails finding SDL2:
	- Ensure internet access for first-time fetch, or install SDL2 CMake package config locally.
- Configure fails because Python is missing:
	- Install Python 3 and ensure it is available to CMake as `Python3_EXECUTABLE`.
- App starts but assets are missing:
	- Build from repository root and run from the build output where `assets/` was copied.
- Shader or render issues:
	- Verify GLSL files exist under `assets/shaders/` and were copied to the build output.

## License

This project is licensed under the terms in `LICENSE`.
