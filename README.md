# Engine

An open-source C++23 game engine.

The repository is not production-complete yet. Game authors primarily work through Lua scripts and the editor. Engine contributors extend core systems in C++ under strict performance, safety, and correctness constraints.

The engine itself is the product: the bundled Island Hopper template and sample content are integration/test fixtures, not deliverables. Current work prioritizes engine robustness and foundations over template-driven features.

## Documentation map

Each fact has one home. Nothing mirrors anything else.

| Where | What |
| --- | --- |
| [`CLAUDE.md`](CLAUDE.md) | The contributor contract: the rules a change must satisfy |
| [`docs/architecture.md`](docs/architecture.md) | Invariants a change must preserve, and what each module owns |
| [`docs/vision.md`](docs/vision.md) | Product direction and priorities |
| [`docs/decisions/`](docs/decisions/) | One record per decision, with its date and rationale |
| [`.claude/skills/`](.claude/skills/) | Procedures: verifying a change, closing a finding, commenting, serialization, consolidating a primitive, stewarding a pull request |
| GitHub tracker | Open scope — findings, bugs, tech debt |
| The code | Current behavior |

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

Every tracked source, script, shader, build, and test file starts with a short file-level comment explaining its role. Public declarations in `include/` headers document purpose, ownership, failure behavior, and threading; private and self-evident declarations carry no comment.

The standard is in `.claude/skills/comment/`. `tools/check_source_comments.py` enforces file-level comment presence and `tools/check_comment_quality.py` rejects filler, misplaced doc comments, commented-out code, and untracked TODOs; both must report zero findings.

## What state the engine is in

There is no feature-status list in this repository, by decision
([0008](docs/decisions/0008-evidence-before-status.md)). Status labels
applied to code that nobody had run produced an inventory of features that
did not actually work — a post-processing stage that rendered every frame
and was never read, shadow types no producer could enable. So, instead:

- **The test suite is the inventory.** `ctest --test-dir build -N` lists
  every contract the engine currently holds itself to. That list, not a
  document, is what "works" means here.
- **Open scope lives on the GitHub tracker.** It is the only source of
  truth for what is broken, missing, or deferred.
- **On-screen renderer behavior is not covered by CI.** No CI lane draws a
  frame, and every lane but the canonical matrix builds with the shader
  cook off. A rendering feature is only as verified as the last time
  somebody ran the editor and looked at it.

The engine builds, runs an editor, simulates a deterministic world, and
plays the bundled template. It is not production-complete.

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
- `tools/`: asset packer (glTF/GLB → `.mesh`), Lua binding generator, audit gates and their self-tests, CI helpers
- `docs/`: architecture invariants, product vision, decision records
- `.claude/skills/`: the procedures agents and contributors follow
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

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `ENGINE_TARGET_PLATFORM` | host | `Win64`, `Linux`, `macOS`, `Android`, `iOS`, `Web` |
| `ENGINE_RENDERER_BACKEND` | `bgfx` | The only accepted value; the variable survives so existing `-D` invocations keep working (see [decision 0001](docs/decisions/0001-bgfx-as-the-rhi.md)) |
| `ENGINE_BGFX_SHADERC` | `ON` | Builds `shaderc` and cooks the shader manifest. Lanes that never consume cooked binaries turn it off, and the cooked test sections skip |
| `ENGINE_MAX_ENTITIES` | `65536` | ECS fixed capacity |
| `ENGINE_DETERMINISTIC_FLOATS` | `ON` | `/fp:strict` / `-ffp-contract=off` |
| `ENGINE_SANITIZERS` | `OFF` | ASAN/UBSAN or TSAN, per the sanitizer presets |
| `ENGINE_BUILD_TESTS` | `ON` | CTest suites |
| `ENGINE_BUILD_TOOLS` | `ON` | `asset_packer` and the generators |

Sanitizer flags are declared before the first `FetchContent_MakeAvailable`,
so they instrument bgfx and SDL3 as well as the engine's own targets;
`add_compile_options` applies only to targets created after it. The
determinism flags are declared after those two fetches: they cover the
engine and the Lua VM, which the simulation depends on, and not bgfx or
SDL3. The per-target warning and conformance flags are applied by
`engine_apply_strict_compile_options` in `cmake/EngineHelpers.cmake`, so
third-party `FetchContent` targets never inherit those.

On Linux, bgfx's CMake requires the OpenGL and X11/Wayland development
headers; `.github/scripts/install-linux-deps.sh` installs the set CI uses.

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
- Determinism hash comparison across every platform and build
  configuration, through the production pipeline
- `cppcheck` static analysis plus the audit gates (source comments, comment
  quality, module dependencies, dependency pins, test timing, error
  handling, asset metadata paths)
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
- Writes engine mesh binary (`.mesh`) and metadata sidecar (`.cookmeta`)
- Generates asset thumbnails and maintains the asset dependency graph

## Engine contributor rules

The binding rules live in [`CLAUDE.md`](CLAUDE.md) — read it before
changing engine code. It is deliberately the only copy: a summary here
would drift from it, and a contributor following a looser restatement of a
safety rule is how the rule gets broken.

In outline, and not a substitute for reading it: C++23 with no exceptions
or RTTI, `std::expected` for new error paths, dependency flow strictly
downward, no heap allocation on hot paths, self-contained public headers,
authored data written through staged atomic replacement, and tests required
for any change to math, ECS, physics, renderer, or scripting behavior.

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
