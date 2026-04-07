# Engine

An open-source C++20 game engine focused on accessibility and a low learning curve.

Game authors primarily work with Lua scripts and the editor. Engine contributors extend core systems in C++ with strict performance and safety rules.

## What this repository contains

- A runnable editor application: `engine_editor_app`
- Runtime systems for ECS/world simulation, rendering, physics, audio, and scripting
- Lua 5.4 gameplay scripting bridge (`engine` Lua API)
- Asset examples under `assets/`
- Test suites (unit, integration, smoke) wired into CTest
- Tooling for mesh conversion (`asset_packer`)

## Core goals

- Keep the engine usable by non-programmers through scripting and editor-driven workflows
- Maintain predictable runtime behavior (no exceptions, no RTTI, explicit error paths)
- Keep module dependencies explicit and moving downward (avoid cycles)

## Tech stack

- Language: C++20
- Build: CMake (minimum 3.28)
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
- `tests/`: unit, integration, and smoke tests
- `tools/asset_packer/`: glTF/GLB to engine mesh conversion utility

## Build prerequisites

- CMake 3.28+
- A C++20 compiler
	- MSVC (Windows) or
	- Clang/GCC (Linux/macOS)
- OpenGL development support

Notes:

- SDL2 is discovered with `find_package(SDL2 CONFIG QUIET)` first, then fetched from source if unavailable.
- First configure/build may need internet access due to dependency fetches.

## Quick start

### Option A (Windows convenience)

From repository root:

```powershell
.\run.cmd
```

This script:

- Configures and builds `engine_editor_app` if needed
- Runs `build/engine_editor_app.exe`

### Option B (canonical cross-platform commands)

From repository root:

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the app after build:

- Windows: `build\engine_editor_app.exe`
- Linux/macOS: `./build/engine_editor_app`

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
```

The suite includes targets such as:

- `engine_unit_math`
- `engine_unit_runtime_world`
- `engine_integration_ecs`
- `engine_integration_vertical_slice`
- `engine_smoke`

## Lua gameplay scripting

The runtime exposes an `engine` table to Lua scripts.

Current script conventions in `assets/`:

- Scene-level module (`assets/main.lua`)
	- `M.on_start(self)` is called once when play starts
	- `M.on_update(self, dt)` is called every simulation step
- Entity behavior module example (`assets/scripts/player.lua`)
- Reusable utility module example (`assets/lib/utils.lua`)

Typical script actions include:

- Spawning entities
- Setting transforms and materials
- Adding rigid bodies/colliders
- Reacting to key input and collisions
- Scheduling timers/coroutines

## Assets and mesh conversion

The runtime loads assets from `assets/` (copied into build output by CMake).

For mesh conversion, build and run `asset_packer`:

```powershell
cmake --build build --target asset_packer
build\tools\asset_packer\asset_packer.exe <input.gltf|input.glb> <output.mesh>
```

Tool behavior:

- Reads first mesh primitive from the source file
- Writes engine mesh binary (`.mesh`)
- Writes metadata sidecar (`.meta.json`)

## Engine contributor rules

- Use C++20 only (no compiler extensions)
- Do not use exceptions, RTTI, `dynamic_cast`, or `typeid`
- Keep engine API functions `noexcept`
- Use explicit return values + logging for runtime failures
- Keep dependency flow downward; do not introduce or expand cycles

If you modify core behavior in math, ECS/runtime, physics, renderer/mesh loading, reflection, or scripting, add or update tests in `tests/`.

## Troubleshooting

- Configure fails finding SDL2:
	- Ensure internet access for first-time fetch, or install SDL2 CMake package config locally.
- App starts but missing assets:
	- Build from repository root and run from the build output where `assets/` was copied.
- Shader/runtime render issues:
	- Verify GLSL files exist under `assets/shaders/` and were copied to build output.

## License

This project is licensed under the terms in `LICENSE`.
