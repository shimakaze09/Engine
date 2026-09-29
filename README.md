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

- A runnable editor application, `engine_editor_app`, and a player, `engine_player`, that runs a game without the editor
- Runtime systems for ECS/world simulation, rendering, physics, audio, and scripting
- Lua 5.4 gameplay scripting bridge (`engine` Lua API)
- Generated Lua binding pipeline for annotated scripting accessors
- A sample game, `samples/island/`: a project of its own (`island.project` and its `assets/`)
- Test suites (unit, integration, smoke, benchmark, CMake configure-rejection) wired into CTest
- Asset tooling: `asset_packer` (mesh, skeleton and animation cook, shader cook, metadata init) and the `engine_validate` scene checker (`--project <dir>` checks every scene a project lists)
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
- **On-screen renderer behavior is not covered by CI.** No CI lane checks
  what a frame shows. The web lane draws frames in headless Chromium on
  SwiftShader's WebGL2 and fails on a page, engine or WebGL error, but
  compares no image. Only the Windows, Linux and macOS Release lanes cook
  shaders; every other lane builds with the cook off. A rendering feature
  is only as verified as the last time somebody ran the editor and looked
  at it.

The engine builds, runs an editor, simulates a deterministic world, and
plays the bundled template. It is not production-complete.

## Tech stack

- Language: C++23
- Build: CMake 3.28+
- Window/input: SDL3
- Rendering: bgfx (Vulkan is the proven backend; D3D11, D3D12 and Metal
  are selectable but unproven; WebGL2 runs the shipped page error-free in
  CI (`engine_web_page_boots`) with no image check, so its shadows have
  not been observed in a browser; shaderc-cooked `.sc` shaders)
- UI/editor: ImGui + ImGuizmo
- Scripting: Lua 5.4 (C API)
- Audio: miniaudio

Every third-party dependency is fetched through CMake `FetchContent` at a
pinned commit; only SDL3 is looked up locally first.

## Repository layout

- `app/`: the editor's entry point (`engine_editor_app`)
- `player/`: the player's entry point (`engine_player`), which links no editor code
- `core/`: platform, input, job system, logging, reflection base, VFS
- `math/`: math primitives and transforms
- `content/`: asset catalog, identity and `.meta` sidecars, cook-stamp staleness checks, streaming
- `physics/`: simulation and collision stepping
- `renderer/`: mesh, texture, shader, command buffer, bgfx backend
- `audio/`: runtime audio services
- `scripting/`: Lua runtime and engine bindings
- `runtime/`: engine bootstrap/run loop, world/ECS, scene and prefab serialization
- `editor/`: editor integration, camera, command history
- `samples/island/`: the sample game, a project of its own: `island.project` and the scripts, scenes and content under its `assets/`, mounted at `assets/`
- `engine_assets/`: the engine's own content (shaders and their cook manifest, editor fonts, the web shell, the bootstrap mesh), mounted at `engine/`
- `tests/`: unit, integration, smoke, benchmark, and CMake configure-rejection tests
- `tools/`: asset packer (glTF/GLB → `.mesh`, shader-manifest cook, `--init-meta`), `engine_validate` scene checker, Lua binding generator, content generators, audit gates and their self-tests, CI helpers
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
	- Windows x64: `clang-cl`. Executables embed a UTF-8 active-code-page
	  manifest (`cmake/windows_utf8.manifest`), so running them needs
	  Windows 10 version 1903 or newer
	- Linux x64: `clang++` 19 or newer (clang 18 cannot compile libstdc++'s `<expected>`)
	- macOS: AppleClang 16 (Xcode 16) or newer. macOS is an editor platform by
	  [decision 0015](docs/decisions/0015-commercial-anime-engine-on-six-platforms.md)
	  and cooks the `metal` shader profile; CI has no GPU, so the live editor
	  on a Mac rests on a human observation
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

From repository root, configure with the preset for your platform
(`windows-clang-cl-debug`, `linux-clang-debug` or `macos-clang-debug`, or
`-release` for an optimised build), then build. These presets build the
engine, the editor and the tools only.

On Windows, run from a Visual Studio Developer PowerShell: the preset is
Ninja + clang-cl, which needs the MSVC environment.

```powershell
cmake --preset windows-clang-cl-debug
cmake --build build --parallel
```

```bash
cmake --preset linux-clang-debug
# macOS (Xcode 16 or newer, which shaderc needs):
# cmake --preset macos-clang-debug
cmake --build build --parallel
```

### Contributing: building and running the tests

The test suites are opt-in, as SDL3's `SDL_TESTS` and Godot's `tests=no`
default: a user building the engine does not compile them. Contributors
configure with the `-dev` preset (Debug plus tests; `-bench` is Release
plus tests, for the benchmarks), which uses the same `build/` directory:

```bash
cmake --preset linux-clang-dev      # or windows-clang-cl-dev, macos-clang-dev
cmake --build build --parallel
ctest --preset linux-clang-dev-headless   # every suite but the gpu-labelled ones
```

A test preset run against a build without tests fails rather than passing
empty. `ENGINE_BUILD_TESTS=ON` on any other configure does the same as a
`-dev` preset.

`cmake --list-presets=all` shows every configure/build/test preset
available on the host, including the GCC flows and the ASAN+UBSAN presets.
A generic `cmake -S . -B build` with the environment-default compiler may
work but is not a supported configuration. CI configures the canonical
toolchains with flags equivalent to the presets (it does not invoke them),
plus the MSVC/GCC compatibility lanes.

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `ENGINE_TARGET_PLATFORM` | host | `Win64`, `Linux`, `macOS` (headless tests only for now), `Web` (Emscripten plus `ENGINE_WEB_COOKED_DIR`); `Android` and `iOS` are rejected at configure |
| `ENGINE_RENDERER_BACKEND` | `bgfx` | The only accepted value; the variable survives so existing `-D` invocations keep working (see [decision 0001](docs/decisions/0001-bgfx-as-the-rhi.md)) |
| `ENGINE_BGFX_SHADERC` | `ON` | Builds `shaderc` and cooks the shader manifest. Lanes that never consume cooked binaries turn it off, and the cooked test sections skip. Needs `ENGINE_BUILD_TOOLS=ON`; forced off for Web |
| `ENGINE_MAX_ENTITIES` | `65536` | ECS fixed capacity |
| `ENGINE_DETERMINISTIC_FLOATS` | `ON` | `/fp:strict` / `-ffp-contract=off` |
| `ENGINE_SANITIZERS` | `OFF` | ASAN + UBSAN (GCC/Clang; ignored on MSVC). TSAN has no option: CI passes `-fsanitize=thread` through `CMAKE_CXX_FLAGS` |
| `ENGINE_BUILD_TESTS` | `OFF` | The CTest suites, for contributors; the `-dev` and `-bench` presets and CI turn it on |
| `ENGINE_BUILD_TOOLS` | `ON` | `asset_packer`, `engine_validate` and the shader cook |

Sanitizer flags are declared before the first `FetchContent_MakeAvailable`,
so they instrument bgfx and SDL3 as well as the engine's own targets;
`add_compile_options` applies only to targets created after it. The
determinism flags are declared after those two fetches: they cover the
engine and the Lua VM, which the simulation depends on, and not bgfx or
SDL3. The per-target warning and conformance flags are applied by
`engine_apply_strict_compile_options` in `cmake/EngineHelpers.cmake`, so
third-party `FetchContent` targets never inherit those.

On Linux, bgfx's CMake requires the OpenGL and X11/Wayland development
headers; the package set CI passes to
`.github/scripts/install-linux-deps.sh` is listed in
`.github/workflows/ci.yml`.

Run the app after build:

- Windows: `build\engine_editor_app.exe`, a windowed application that opens
  no console window; started from a terminal, it prints there.
- Linux: `./build/engine_editor_app`, or open `build/engine_editor_app.desktop`
  from a file manager or launcher, which runs it without a terminal.
- macOS builds it as `build/engine_editor_app.app`, a bundle Finder launches
  without Terminal (it cannot run until the shader cook works there).

The editor opens the project named on its command line, either the
project's directory or its `.project` file (`engine_editor_app
path/to/my_game`). Started with none, as from a file manager, it opens the
sample project the build copies beside it (`build/samples/island`), until a
project hub takes that role. A project that cannot be opened says why in an
error box.

`engine_player` runs a game without the editor, as a Unity player build or a
Godot export does: `engine_player path/to/my_game` (the sample beside it
with none) opens a window titled with the project's name and plays its
startup scene. `--headless` runs it without a window and `--max-frames N`
stops it after N frames. It exits 0 when the game quits, 1 when the project
cannot be opened or an option is wrong (the reason is printed, and shown in
a box when windowed), and 3 when the startup scene does not load
(`engine_player_executable` runs the real binary through each case). On the
web, `engine_player.html` is the page a game is shared as, and
`engine_editor_app.html` is the editor.

Each windowed run writes its log to `logs/editor.log` under the per-user data
directory (`logs/player.log` for `engine_player`), keeping the previous run's as
`editor-prev.log`, as Unity keeps `Editor.log`; a failed start shows an error
box naming it. The same messages appear in the editor's own Log panel, whose
command line runs console commands (`help` lists them, `get` and `set` read and
write cvars; Tab completes, Up and Down recall), as Unreal's Output Log does.

F9, the Game view's Screenshot button, Edit > Take Screenshot or the
`screenshot` console command saves what the Game view shows as a PNG under
`Screenshots/` in the project's per-user data directory, as Unreal's F9 saves
into `Saved/Screenshots`; the Log names the file. A Game view tab behind
another comes to the front first, and nothing the editor draws over the view
is in the picture.

Edit > Record Play (or `demorec [name]` in the Log) enters Play and records the
session, as Unreal's `demorec` does; Stop saves it under `Recordings/` in the
project's per-user data directory. Edit > Replay Latest Recording, the Replay
submenu or `demoplay [name]` enters Play again with the recorded input in place
of the live devices, from the same scene, and the Log reports whether the
replay reproduced the recording (the world's state hash at every tick both runs
observed) or the first tick and state (transforms, rigid bodies, random...) that
differ. `demostop` stops either. The toolbar shows REC or REPLAY meanwhile.

Assets can be labelled, as Unity's Asset Labels are: select one in the Assets
panel, type a label into the Labels row and press Enter; its button removes it.
Labels are kept in the asset's `.meta` sidecar, so they are committed with the
project, and `l:<label>` in the Assets search keeps the assets carrying it
(`l:env rock` also needs "rock" in the name).

It starts on an empty 3D scene, as a new Unity project does. The scene holds a
Main Camera, a Directional Light, and a Scene Controller entity running
`assets/main.lua`, whose hooks start empty. The player boots
`assets/main.scene`, the same scene; `engine_integration_startup_template`
keeps the two identical. File > Open Scene... opens the Island Hopper
template (`assets/templates/island_hopper.scene`) and the sample scenes
(`assets/samples/playground.scene`, `assets/coin_run.scene`,
`assets/shading_models.scene`).

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

Some tests are labeled `gpu`: they open a device and draw. CI runs them on
the Linux Release lane only, on Mesa's software Vulkan (lavapipe) under Xvfb;
every other lane excludes the label.

## Continuous integration

GitHub Actions configuration lives in `.github/workflows/ci.yml` and currently
runs eleven jobs. Every job except the cross-platform determinism comparison
and the web lane starts at once; engine targets build with warnings as
errors on every lane, and CTest runs four tests at a time. Each native lane
restores its built dependencies from a cache that only pushes to `main`
save, one entry per OS and configuration:

- Windows, Linux, and macOS builds in Debug and Release on the canonical
  toolchains (`clang-cl` via Ninja, `clang++-19`, AppleClang),
  with headless-safe CTest filtering; the Linux Release lane also runs the
  `gpu`-labelled suites on lavapipe (software Vulkan) under Xvfb
- MSVC (Windows) and GCC (Linux) Release compatibility lanes (build + test)
- A web lane: Emscripten builds the shipped page and the lifecycle harness
  in `tests/web/` against the Linux Release lane's shader cook, then
  headless Chromium runs the `web`-labelled tests (the page boots and runs
  frames; maxFrames, quit and a fatal frame each close every engine tier
  and a second bootstrap in the same page runs clean; a save survives a
  page reload, since web saves live on an IndexedDB-backed mount). It
  waits for the build matrix for that cook, and restores the Linux
  sources cache without saving one of its own
- Determinism hash comparison across every platform and build
  configuration, through the production pipeline
- `cppcheck` static analysis plus the audit gates (source comments, comment
  quality, module dependencies, dependency pins, content attributes, test
  timing, error handling, portable fopen, duplicate primitives, asset
  metadata paths, asset identity, shader variants, document references, array value-initialization)
- `clang-tidy` with warnings-as-errors
- ASAN/UBSAN and TSAN sanitizer lanes
- Coverage with a minimum-threshold gate
- Benchmark runs gated against `tests/benchmark/perf_baseline.json`
- A final quality gate that requires all of the above

## Lua gameplay scripting

The runtime exposes an `engine` table to Lua scripts.

Current script conventions in the sample's `assets/`:

- Scene-level module (`assets/main.lua`)
	- `M.on_begin_play(self)` is called once when play starts, or when
	  the entity is created during play. It may spawn entities and attach
	  scripts; an entity spawned there begins play later in the same frame
	- `M.on_fixed_tick(self, dt)` is called once per fixed step, with the
	  fixed delta. Input queries made inside it (`engine.is_key_pressed`,
	  `engine.is_action_pressed`, gamepad and mouse reads) answer for that
	  step alone, so a tap is seen once whatever the frame rate: gameplay
	  that reacts to input belongs here. What each step read can be
	  recorded to an input log and replayed, reproducing the run at any
	  frame rate (`core::begin_input_recording` and
	  `core::begin_input_replay` in `core/include/engine/core/input.h`;
	  the editor's Record Play and Replay check the replay against
	  the recording through `engine/runtime/play_recording.h`); input
	  read in `on_tick` is not recorded
	- `M.on_tick(self, dt)` is called once per rendered frame that
	  advanced simulation (not once per fixed step); `dt` is that
	  frame's total simulated time, summing every catch-up fixed step
	- `M.on_end_play(self)`, `M.on_save_state(self)`, and
	  `M.on_reload(self, state)` cover teardown and hot reload; a saved
	  module that fails to load changes nothing, and `on_save_state` runs
	  only once the new module has loaded. On an
	  editor Stop, `on_end_play` runs before the authored scene is
	  restored, so it reads the state the session ended in
	- Legacy `on_start`/`on_update`/`on_end` names remain as fallbacks
- Scene-level module example that spawns a controllable player and
  physics props when play begins (`assets/samples/playground.lua`, run by
  `assets/samples/playground.scene`; `engine_integration_playground_sample`
  plays it)
- Entity behavior module example (`assets/scripts/player.lua`)
- Reusable utility module example (`assets/lib/utils.lua`)

Current scripting/runtime support in the tree includes:

- Spawning entities
- Setting transforms and materials
- Adding rigid bodies and colliders
- Reacting to key input and collisions
- Scheduling timers with `engine.set_timeout()` and `engine.set_interval()`
- Coroutine helpers such as `engine.wait()`, `engine.wait_frames()` (fixed
  simulation steps, not rendered frames), and `engine.wait_until()`
- Sandbox, generated binding, and hot-reload coverage in integration tests

The scripting surface is still evolving. Some APIs are generated from annotated accessors, while the hand-written surface lives in domain binding translation units under `scripting/src/` (entity lifecycle, body, mesh/material, physics, lights, camera, audio, input, timers, coroutines, and more).

## Assets and mesh conversion

The runtime mounts the open project's content root at `assets/` and the engine's own content from `engine_assets/` at `engine/` (`EngineConfig::assetRoot` and `engineRoot`). `engine::open_project` sets the content root from the project document. The engine's content is found through `ENGINE_ROOT`, then beside the executable, then in the working directory, unless the config names it; CMake copies it and the sample project into the build output, and bootstrap refuses an engine root that is not there.

For mesh conversion, build and run `asset_packer`:

```powershell
cmake --build build --target asset_packer
build\tools\asset_packer\asset_packer.exe <input.gltf|input.glb> <output.mesh>
```

Tool behavior:

- Deterministic cook: identical inputs produce byte-identical outputs
- Imports glTF meshes plus skeletons and animation clips
- Writes `.mesh`, `.cookmeta`, `.cookstamp` and a collision `.hull`, plus
  `.skel` and `<clip>.anim` for rigged input
- Generates asset thumbnails, and records in the cook stamp every file the
  cook read beside the source, so an edit to one forces a recook

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
	- Build from repository root; the build output holds the copied `engine_assets/` and `samples/island/` beside the editor.
- Shader or render issues:
	- Verify the shaderc cook ran (`ENGINE_BGFX_SHADERC=ON`) and the cooked binaries exist under `build/engine_assets/shaders/bgfx/cooked/`.

## License

This project is licensed under the terms in `LICENSE`.
