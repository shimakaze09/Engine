# Engine

An open-source C++23 game engine.

The repository is not production-complete yet. Game authors primarily work through Lua scripts and the editor. Engine contributors extend core systems in C++ under strict performance, safety, and correctness constraints.

The engine itself is the product (owner decision 2026-08-25): the bundled Island Hopper template and sample content are integration/test fixtures, not deliverables. Current work prioritizes engine robustness, module-boundary cleanup, and audit-driven hardening over template-driven features; see the product vision and roadmap in `CLAUDE.md`.

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

## Source commenting standard

Every tracked source, script, shader, build, and test file should start with a short file-level comment explaining its role. Every class, struct, enum, and function added or changed in future work should also keep a concise purpose comment close to its declaration or definition. Update the comment when behavior changes.

Two audit tools enforce this in CI: `tools/check_source_comments.py` checks comment presence, and `tools/check_comment_quality.py` flags machine-generated filler patterns; both must report zero findings.

## Current verified state

The engine has strong foundations, but it is still being delivered milestone by milestone.

Verified working areas in the current tree include:

- Core systems such as logging, CVars, debug draw, job system, event bus, VFS, and math primitives
- Runtime ECS/world simulation with SparseSet storage, double-buffered transforms, scene serialization, persistent IDs, and 65,536-entity capacity
- Hybrid deferred/forward rendering on the bgfx backend (Vulkan proven on
  desktop, WebGL2 for the web target; D3D11/12 are explicit opt-ins) with
  G-buffer resources, deferred lighting for opaque geometry, forward
  transparency, PBR shaders, frustum culling, mesh loading, and editor
  integration; shaders are shaderc-cooked `.sc` sources
- Physics systems including rigid bodies, collider shapes, spatial broadphase,
  CCD/speculative contacts, joints, materials, and query APIs — colliders
  follow the transform hierarchy (child colliders form compound bodies owned
  by their nearest rigid-body ancestor), and the built-in cylinder/pyramid
  shapes collide as mesh-matched convex hulls
- Scene-object model with parenting (`set_parent`/`get_children` from Lua),
  cascade destruction of transform subtrees, and non-removable Name/Transform
  identity in the editor
- Render-to-texture scene captures with material binding, JSON material
  assets with parent-chain overrides, and a depth-tested debug-line pass for
  shape-accurate collider overlays
- Editor play/pause/stop flow, gizmo transforms, and transform undo support
- Audio playback via miniaudio with wav/mp3/ogg/flac support plus volume, pitch, and loop control
- Lua module loading, traceback-based error reporting, generated and hand-written bindings, per-World timers, coroutine helpers, sandbox controls, and hot-reload coverage
- GitHub Actions CI for multi-platform build/test, determinism comparison,
  `cppcheck`, `clang-tidy`, sanitizers, coverage, and benchmark gates

The engine is no longer forward-only; the deferred path is active behind
`r_deferred` with forward fallback/transparency. Large roadmap items such as
full animation production, game UI runtime, platform packaging, project
workflow/commandlets, and release operations remain open in the roadmap
section of `CLAUDE.md`, the single project document for contributor rules,
the repository map, and roadmap status — though template-driven feature
slices are suspended under the 2026-08-25 engine-first pivot in favor of
the hardening queue. The 2026-07 production-hardening campaign (27
correctness/performance/structure findings) is complete.

## Tech stack

- Language: C++23
- Build: CMake 3.28+
- Window/input: SDL3
- Rendering: bgfx (Vulkan/WebGL2 proven; shaderc-cooked `.sc` shaders)
- UI/editor: ImGui + ImGuizmo
- Scripting: Lua 5.4 (C API)
- Audio: miniaudio

Most third-party dependencies are fetched automatically via CMake `FetchContent` when not found locally.

## Repository layout

- `app/`: executable entry point (`engine_editor_app`)
- `core/`: platform, input, job system, logging, reflection base, VFS
- `math/`: math primitives and transforms
- `physics/`: simulation and collision stepping
- `renderer/`: mesh, texture, shader, command buffer, bgfx backend
- `audio/`: runtime audio services
- `scripting/`: Lua runtime and engine bindings
- `runtime/`: engine bootstrap/run loop, world/ECS, scene and prefab serialization
- `editor/`: editor integration, camera, command history
- `assets/`: scripts, shaders, and sample content
- `tests/`: unit, integration, smoke, and benchmark tests
- `tools/`: asset packer (glTF/GLB → `.mesh`), Lua binding generator, source-comment audits, CI helpers
- `.github/workflows/`: CI definitions

## Build prerequisites

- CMake 3.28+ and Ninja
- Python 3 (required for generated Lua bindings during configure/build)
- A C++23-capable compiler (see the toolchain policy below)

### Compiler support policy

The engine centers on one canonical LLVM toolchain per platform, with two
secondary compilers validated for portability:

- **Tier 1 — canonical (used for development and primary CI)**
	- Windows x64: `clang-cl`
	- Linux x64: `clang++`
	- macOS: AppleClang
- **Tier 2 — portability validation (dedicated CI compatibility lanes)**
	- Windows x64: MSVC
	- Linux x64: GCC

Engine code stays standard C++23 with no compiler-specific language
extensions; Tier 2 exists to prove that, not to relax it. The
`CMakePresets.json` presets encode the canonical flows and are the
recommended way to configure.

Notes:

- SDL3 is discovered with `find_package(SDL3 CONFIG QUIET)` first, then fetched from source if unavailable.
- First configure/build may need internet access due to dependency fetches.

## Quick start

From repository root, configure with the canonical preset for your platform
(`windows-clang-cl-debug`, `linux-clang-debug`, or `macos-clang-debug`),
then build and test:

```powershell
cmake --preset windows-clang-cl-debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

```bash
cmake --preset linux-clang-debug     # or: macos-clang-debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`cmake --list-presets` shows every configure/build/test preset, including
the GCC compatibility flows and the sanitizer lanes. A generic
`cmake -S . -B build` with the environment-default compiler may work but is
not a supported configuration; CI validates the canonical presets plus the
MSVC/GCC compatibility lanes.

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

GitHub Actions configuration lives in `.github/workflows/ci.yml` and currently
runs eleven jobs:

- Windows, Linux, and macOS builds in Debug and Release on the canonical
  toolchains (`clang-cl` via the VS ClangCL toolset, `clang++`, AppleClang),
  with headless-safe CTest filtering
- MSVC (Windows) and GCC (Linux) Release compatibility lanes (build + test)
- Cross-platform determinism hash comparison
- `cppcheck` static analysis (plus the source comment audit)
- `clang-tidy` with warnings-as-errors
- A dedicated `-Werror` build check
- ASAN/UBSAN and TSAN sanitizer lanes
- Coverage with a minimum-threshold gate
- Benchmark runs gated against `tests/benchmark/perf_baseline.json`
- A final quality gate that requires all of the above

Remaining follow-up work includes coverage trend reporting and broader
GPU-path automation.

## Lua gameplay scripting

The runtime exposes an `engine` table to Lua scripts.

Current script conventions in `assets/`:

- Scene-level module (`assets/main.lua`)
	- `M.on_begin_play(self)` is called once when play starts
	- `M.on_tick(self, dt)` is called once per rendered frame that
	  advanced simulation (not once per fixed step); `dt` is that
	  frame's total simulated time, summing every catch-up fixed step
	- `M.on_end_play(self)`, `M.on_save_state(self)`, and
	  `M.on_reload(self, state)` cover teardown and hot reload
	- Legacy `on_start`/`on_update`/`on_end` names remain as fallbacks
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

The scripting surface is still evolving. Some APIs are generated from annotated accessors, while the hand-written surface lives in domain binding translation units under `scripting/src/` (entity lifecycle, body, mesh/material, physics, lights, camera, audio, input, timers, coroutines, and more).

## Assets and mesh conversion

The runtime loads assets from `assets/` (copied into the build output by CMake).

For mesh conversion, build and run `asset_packer`:

```powershell
cmake --build build --target asset_packer
build\tools\asset_packer\asset_packer.exe <input.gltf|input.glb> <output.mesh>
```

Tool behavior:

- Deterministic cook: identical inputs produce byte-identical outputs
- Imports glTF meshes plus skeletons and animation clips
- Writes engine mesh binary (`.mesh`) and metadata sidecar (`.meta.json`)
- Generates asset thumbnails and maintains the asset dependency graph

## Engine contributor rules

- Use C++23 only (no compiler extensions); features must compile on every CI
  lane, including AppleClang
- Do not use exceptions, RTTI, `dynamic_cast`, or `typeid`
- Mark a public real-time or leaf runtime API `noexcept` only when every
  operation it invokes is proven non-throwing; a recoverable `noexcept` path
  must not call allocation, filesystem, or thread-creation operations that
  can terminate under the no-exception build, because a failure there ends
  the process instead of returning an error. Cold initialization, editor,
  tool, and filesystem work uses staged transactions, explicit error results,
  and rollback (the binding rule is in `CLAUDE.md`, "Hard rules")
- Use explicit return values plus logging for runtime failures; prefer
  `std::expected<T, E>` in new APIs and never call `.value()` (with
  exceptions disabled it aborts — use `has_value()`/`operator*`/`error()`)
- Keep dependency flow strictly downward; do not introduce upward or sideways cycles
- Do not heap-allocate on hot paths
- Keep public headers self-contained and free of SDL, bgfx, Lua, and ImGui types

If you modify core behavior in math, ECS/runtime, physics, renderer/mesh loading, reflection, or scripting, add or update tests in `tests/`.

## Troubleshooting

- Configure fails finding SDL3:
	- Ensure internet access for first-time fetch, or install SDL3 CMake package config locally.
	- On Linux, SDL3 requires X11 extension dev headers that SDL2 treated as optional (Xcursor, Xi, Xtst, Xfixes, Xrandr, XScrnSaver); install them or configure the matching `SDL_X11_*` options off.
- Configure fails because Python is missing:
	- Install Python 3 and ensure it is available to CMake as `Python3_EXECUTABLE`.
- App starts but assets are missing:
	- Build from repository root and run from the build output where `assets/` was copied.
- Shader or render issues:
	- Verify the shaderc cook ran (`ENGINE_BGFX_SHADERC=ON`) and the cooked binaries exist under the build tree's `shaders/bgfx/cooked/`.

## License

This project is licensed under the terms in `LICENSE`.
