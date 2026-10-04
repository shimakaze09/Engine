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
- Asset tooling: `asset_packer` (mesh, skeleton and animation cook, shader cook, metadata init) and the `engine_validate` content checker (`--project <dir>` catalogues the project as the engine does, checks every scene it lists, and loads every catalogued prefab, material and animation controller through its own loader; any reference that names no file or catalogued asset, a document that does not load, or a mount that does not index cleanly, fails it; `--bake-navmesh` writes each scene's navigation meshes and `--check-navmesh` fails on a stale one)
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
- Audio: miniaudio (WAV, Ogg Vorbis through the stb_vorbis decoder in the
  same miniaudio checkout, MP3, FLAC)

Every third-party dependency is fetched through CMake `FetchContent` at a
pinned commit; only SDL3 is looked up locally first.

## Repository layout

- `app/`: the editor's entry point (`engine_editor_app`)
- `player/`: the player's entry point (`engine_player`), which links no editor code
- `core/`: platform, input, job system, logging, reflection base, VFS
- `math/`: math primitives and transforms
- `content/`: asset catalog, identity and `.meta` sidecars, cook-stamp staleness checks, streaming
- `physics/`: simulation and collision stepping
- `navigation/`: the navigation mesh bake and path queries
- `renderer/`: mesh, texture, shader, command buffer, bgfx backend
- `audio/`: runtime audio services
- `scripting/`: Lua runtime and engine bindings
- `runtime/`: engine bootstrap/run loop, world/ECS, scene and prefab serialization
- `editor/`: editor integration, camera, command history
- `samples/island/`: the sample game, a project of its own: `island.project` and the scripts, scenes and content under its `assets/`, mounted at `assets/`
- `engine_assets/`: the engine's own content (shaders and their cook manifest, editor fonts, the web shell, the bootstrap mesh), mounted at `engine/`
- `tests/`: unit, integration, smoke, benchmark, and CMake configure-rejection tests
- `tools/`: asset packer (glTF/GLB → `.mesh`, shader-manifest cook, `--init-meta`), `engine_validate` content checker, Lua binding generator, content generators, audit gates and their self-tests, CI helpers
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
path/to/my_game`), on its startup scene. Started with none, as from a file
manager, it shows the project hub, as Unity Hub and Godot's project manager
do:
- the projects opened recently (the first start lists the sample the build
  copies beside it, `build/samples/island`); Open or double-click one;
- **Open...** picks a `.project` file;
- **New Project...** makes one from the empty 3D template (a camera, a light
  and an empty main script) in a folder you pick, and opens it.

File > Open Project... and File > Close Project leave the open project for
another or for the hub, asking about unsaved changes first. A project named
on the command line that cannot be opened says why in an error box.

Save never writes over a scene or material whose file changed on disk after
the editor opened or last saved it (a teammate's pull, another tool). It stops
with the file untouched and offers Overwrite, Reload or Save As (a material:
Overwrite or Reload from Disk), as Unity and Godot ask about an asset changed
outside the editor. A file deleted in the meantime is simply written again
(`engine_unit_editor_scene_document`, `engine_unit_editor_material_edit`).

`engine_player` runs a game without the editor, as a Unity player build or a
Godot export does: `engine_player path/to/my_game` (the sample beside it
with none) opens a window titled with the project's name and plays its
startup scene, loaded before the first frame so its scripts begin play
once, as Unity and Unreal load the first scene before any gameplay code
runs. `--headless` runs it without a window and `--max-frames N`
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
write cvars; Tab completes, Up and Down recall), as Unreal's Output Log does. A
quoted value is one argument, and a text cvar takes the rest of the line, so
`set r_fog_color 0.2 0.3 0.4` sets all three numbers; a line that is too long
or has an unclosed quote is refused with a message rather than run in part.

If the editor has to close on an internal error or a graphics-device failure
(a driver reset, a lost GPU), it first saves the unsaved scene to `Recovery/`
in the project's per-user data directory, as Unity keeps a `_Recovery`
folder, and the error box names the file; open it with File > Open Scene.
During Play the copy is the scene as it was before Play. A device failure
exits with code 4 (`engine_integration_fatal_recovery`,
`engine_integration_fatal_device_recovery`).

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
the Linux Release lane, on Mesa's software Vulkan (lavapipe) under Xvfb, and
on the Windows Release lane, on WARP (Microsoft's software rasterizer)
through D3D11 and again through D3D12; every other lane excludes the label. The
`r_bgfx_software_adapter` cvar (set it with
`ENGINE_CVAR_r_bgfx_software_adapter=1`) asks for that software adapter on
any machine.

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
  `gpu`-labelled suites on lavapipe (software Vulkan) under Xvfb, and the
  Windows Release lane runs them on WARP through D3D11 and D3D12
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

A run holds up to 1,024 distinct script files, entity scripts and
`engine.require`d modules together. Past that a script does not load,
and the Log names each refused file once.

`engine.start_coroutine`, `engine.on_collision_handler`,
`engine.on_trigger_handler` and `engine.pool_create` return an id, or `nil`
and a reason. The reason separates a bad argument from a full table. The
tables hold 1,024 coroutines, 64 collision handlers, 64 trigger handlers and
64 pools of up to 1,024 entities each.
The first refusal by a full table logs a Warning.

`print` writes to the log, so its line shows in the editor's Log panel and
the log file, prefixed by the calling script's file and line, at Info
(`engine_integration_lua_print_log`).

An `engine.*` call given an entity it cannot act on still returns `false`
or `nil`. This covers a value that is not a handle, a handle from before
the last scene load, or one naming a destroyed entity. The first such
call from each script line logs a Warning naming the file, the line, the
binding and the reason (`engine_integration_lua_entity_argument_report`).

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
	- Every hook visits the scripted entities in one order, ascending
	  entity index: the scene file's own entity order, which a save and a
	  load keep, with entities spawned during play in the slot they take.
	  Removing and re-adding a script does not move its entity. Editor
	  Play rebuilds the scene from the snapshot it takes, as Stop does
	  and as the player loads it, so the first Play after an edit runs
	  exactly as every later one and as the shipped game
	  (`engine_integration_play_matches_reload`)
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
- Reacting to key input, collisions and trigger volumes
- Scheduling timers with `engine.set_timeout()` and `engine.set_interval()`
- Coroutine helpers such as `engine.wait()`, `engine.wait_frames()` (fixed
  simulation steps, not rendered frames), and `engine.wait_until()`
- Sandbox, generated binding, and hot-reload coverage in integration tests

Only a Camera renders the game, as in Unity and Godot. The Game view and
the player draw the highest-priority active Camera component, from its
entity's transform; with none, they show black, and the editor's Game
view says so and offers **Create Camera** (also Entity > Camera and the
Entities panel's right-click menu). A camera only renders. Following a
character is a script moving the camera's entity, e.g.
`engine.set_position(cam, x, y, z)` then `engine.look_at(cam, px, py, pz)`,
and `engine.add_spring_arm` works only on an entity with a Camera. No
script reaches the editor's Scene view camera, and there is no hidden
script camera (`engine_integration_game_camera_gpu`,
`engine_integration_camera_producer_removal`,
`engine_unit_editor_entity_menus`).

A rigid body has a type, as in Jolt, Unity and Godot:
- **Dynamic** (the default): moved by gravity, forces and collisions.
- **Kinematic**: moved only by its velocity or a script. It pushes and
  carries what it touches but is never pushed back, feels no gravity and
  never sleeps. Use it for moving platforms, doors and lifts.
- **Static**: never moves.

A collision is reported to `on_collision` and the collision handlers only
when at least one of the two bodies can move. Two colliders that cannot
(static, kinematic, or asleep and undisturbed) have no response and report
nothing, as in Unity, so overlapping level geometry costs no events
(`engine_unit_collision_frame_events`).

A collider marked **Is Trigger** (the Inspector, or
`engine.set_trigger(e, true)`; `engine.is_trigger(e)` reads it) reports
overlaps instead of colliding, as Unity's triggers and Godot's areas do.
Bodies pass through it, it adds no mass to its body, and raycasts, sweeps
and overlap queries ignore it. `engine.on_trigger_handler(function(trigger,
other, phase) ... end)` is called with `phase` `"enter"` when a solid
collider begins overlapping a trigger and `"exit"` when it stops; remove
one with `engine.remove_trigger_handler(id)`. A pair reports only when at
least one side has a dynamic or kinematic body. Two triggers never report
each other, a sleeping body stays inside, and a destroyed participant
reports its exit as `nil`. Events reach the handlers once per rendered
frame in step order, each step's exits before its enters, ordered by
entity, not by storage. Up to 1,024 trigger overlaps are tracked at once;
past that the Log warns and events wait until the count falls
(`engine_unit_physics_trigger`, `engine_integration_lua_trigger_events`).

Shape casts come as `engine.sweep_sphere`, `engine.sweep_box` and
`engine.sweep_capsule(ax, ay, az, bx, by, bz, radius, dx, dy, dz,
max_dist [, mask [, skip_entity]])`, whose capsule runs between the
hemisphere centers a and b in any orientation, as Unity's `CapsuleCast`
does. Each returns the entity hit, the distance, the shape's center at
impact and the surface normal, or `nil` (`engine_unit_physics_query`).

Collision layers are the 32 bits of a collider's layer and mask. A project
names them and chooses which layers collide in Edit > Project Settings... >
Physics (Godot's layer names, Unity's Layer Collision Matrix); the names
only label the bits, so naming or renaming a layer changes no scene. Two
colliders collide when each one's layer is in the other's mask and the
matrix lets their layers meet; triggers follow the same rule. The
Inspector labels the layer and mask checkboxes with the project's names.
From Lua, `engine.layer_mask("Player", "Enemy")` returns the mask of the
named layers (Unity's `LayerMask.GetMask`; an unknown name is an error,
never a mask that means every layer), `engine.layer_bit(name)` the bit
index and `engine.layer_name(bit)` the name or `nil`;
`engine.set_collision_layer(e, bits)`, `engine.set_collision_mask(e, bits)`
and `engine.get_collision_layer(e)` / `engine.get_collision_mask(e)` read
and write a collider's. Every query takes a mask and hits only colliders on
those layers, ignoring the matrix as Unity's queries do: `raycast_all`,
the overlaps and the sweeps take it where their signatures show it, and
`engine.raycast(ox, oy, oz, dx, dy, dz, max_dist [, skip_entity [,
mask]])` after the skip entity. `~engine.layer_mask("Enemy")` is every
layer but Enemy; a mask that is not an integer fails the query with a
warning (`engine_unit_collision_layer_matrix`,
`engine_integration_lua_collision_layers`).

A Character Controller (Add Component > Physics, or
`engine.add_character_controller(e [, slope_limit [, step_offset [,
skin_width]]])`) moves its entity's own Capsule Collider the way Unity's
CharacterController does: `local grounded, collisions, ground =
engine.move_character(e, dx, dy, dz)` carries it by a displacement at once,
sliding along walls, climbing steps no higher than the step offset (0.3 m by
default), treating slopes steeper than the limit (45 degrees) as walls,
keeping the skin width (0.02 m) between it and what it touches, and keeping
a grounded character on the ground walking down. `collisions` holds
`engine.COLLIDED_BELOW`, `engine.COLLIDED_SIDES` and `engine.COLLIDED_ABOVE`
bits, `ground` is what it stands on, and `engine.is_grounded(e)` reads the
last move. It is not a rigid body: gravity, jumping and speed are the
script's, as in Unity. Triggers and layers the matrix keeps apart never
block it; dynamic bodies block it without being pushed. The capsule stays
upright, and the entity must be a transform root, scaled uniformly, with no
rigid body or a kinematic one; `engine.get_character_controller(e)` and
`engine.remove_character_controller(e)` read and remove it
(`engine_unit_character_move`, `engine_integration_lua_character_controller`).
Like the shape sweeps and overlaps, it meets a heightfield as its bounding
box, so a character cannot yet walk heightfield terrain.

A Nav Mesh Surface (Add Component > Navigation) marks where agents walk, as
Unity's NavMeshSurface does. Its box (Half Extents, around the entity's
position, never rotated or scaled) and its agent (radius, height, the step
it climbs and the steepest slope it walks) set what its Bake button samples:
the static colliders inside the box, through the physics ray queries. Bake
writes the mesh to the surface's Nav Mesh File, a `.navmesh` asset, choosing
`assets/<scene>.navmesh` when it has none and never writing over another
file; a box with nothing walkable writes nothing. A loaded scene reads each
surface's file, again whenever a bake rewrites it; a file that is missing
or damaged leaves that surface without a mesh and is logged, and loading a
scene reports a missing one as `missing_nav_mesh`
(`engine_unit_nav_mesh_surface`, `engine_unit_editor_nav_mesh_bake`).
A build bakes the same files without the editor:
`engine_validate --bake-navmesh` loads each scene and writes every surface's
file through the Bake button's own path, byte for byte what the editor
writes, and `engine_validate --check-navmesh` fails on a surface whose file
is missing or no longer what its colliders bake, so CI refuses a stale mesh;
a surface never baked has no file name yet and is reported, since neither
mode edits the scene (`engine_unit_nav_mesh_surface_file`,
`engine_integration_engine_validate_navmesh`).
`local path, why = engine.find_path(sx, sy, sz, ex, ey, ez)` asks the mesh
whose box holds the start for the shortest walk to the end, as Unity's
`NavMesh.CalculatePath` does: `path` lists its corners as `{x=, y=, z=}`
tables, from the start to the end, each snapped onto the mesh. Otherwise
`path` is nil and `why` is `"off_mesh"` (the start or end is not on a
mesh), `"unreachable"` (no walkable route joins them), `"too_long"` (more
than 256 corners) or `"invalid"` (an argument that is not a number). A query
allocates nothing (`engine_integration_lua_navigation`).

A Nav Agent (Add Component > Navigation) walks those meshes, as Unity's
NavMeshAgent does: `engine.set_nav_destination(e, x, y, z)` sends it to a
point, its path is found on its next fixed step, and each fixed step it
walks the path at its Speed, speeding up and braking at its Acceleration so
it comes to rest at its Stopping Distance, and turns at its Angular Speed
to face where it walks. Base Offset is the height of the entity's origin
above the ground it walks. An agent with a Character Controller walks
through it, so it slides along what it meets; otherwise its transform is
set. `engine.nav_agent_status(e)` returns `"idle"`, `"pending"` (no mesh
loaded yet), `"moving"` or `"arrived"` with the path length left, or
`"failed"` with why: `"off_mesh"`, `"unreachable"`, `"too_long"` (more than
64 corners) or `"cannot_move"` (its controller refused). `engine.stop_nav_agent(e)`
stops it where it stands. Agents step after the scripts' `on_fixed_tick`,
in the order the World stores them, so a run is the same at any worker
count (`engine_unit_nav_agent`, `engine_integration_lua_nav_agent`). Agents
do not yet steer around each other or around obstacles that are not baked
into the mesh.

Entities carry gameplay tags, as Godot's groups and Unreal's actor Tags
do, so scripts find what they act on without a unique name. Give tags in
the Inspector's Tags row under the name, or from Lua:
- `engine.add_tag(e, "coin")` returns `true`, or `false` and why;
- `engine.remove_tag(e, "coin")` and `engine.has_tag(e, "coin")`;
- `engine.get_tags(e)` returns the entity's tags in order;
- `local list, total = engine.find_entities_by_tag("coin")` returns the
  entities that carry it, in entity-index order, and how many do (the list
  holds at most 1,024).

A tag is 1 to 31 bytes of letters, digits, `_`, `-` or `.`, compared ignoring
case. Letters include Chinese, Japanese and Korean ones (`敌人` is a tag of six
bytes), as asset labels, collision-layer names, save slots and recording
names do. A decomposed spelling, such as `か` followed by a combining voiced
mark, is refused, so a name has one spelling only. An entity carries up to 8
tags. Scenes and prefabs save them as
`"Tags": ["coin", "gold"]` (`engine_unit_world_tag_set`,
`engine_integration_lua_entity_tags`, `engine_unit_editor_entity_tags`).

Set the type with the Inspector's Body Type or
`engine.set_body_type(e, "kinematic")`, and read it with
`engine.get_body_type(e)`. `engine.set_position`, `set_rotation`,
`look_at` and `set_scale` teleport an entity: its body keeps its type,
velocity and collisions, and wakes. Drive a moving platform with
`engine.set_velocity` on a kinematic body, so contacts see its motion
(`engine_integration_script_moved_bodies`, `engine_unit_body_type`).

Gameplay is written in scripts, as in Unity and Godot: the engine has no
built-in player controller, game mode, score store or cheat flags. Scripts
read input (`engine.is_key_down`, `engine.is_action_down`), move entities
and bodies, and keep state across scene loads in Lua globals, which live
for the whole run (`engine_integration_scene_flow`); `engine.save_data` and
`engine.load_data` keep it between runs, in named save slots as Unreal's
`SaveGameToSlot` has them. Each takes an optional slot name, `"default"`
when left out: 1 to 31 bytes of letters, digits, `_`, `-` or `.`, case ignored
(`engine.save_data(t, "slot2")`); any other name is a Lua error rather
than a save somewhere else. A slot is `saves/<slot>.save` in the
project's per-user data, a one-line header (format version, save time,
payload length and checksum) and then the data, so a file cut off or
changed after it was written is caught rather than read as a shorter save.
`local data, status = engine.load_data(slot)` answers the table and
`"ok"`, or nil and why: `"absent"` (no save yet), `"corrupt"` (the file is
damaged or cut off, or it does not parse; the log says where),
`"unsupported"` (a newer build wrote it) or `"unreadable"`. A slot that
did not load is kept, never overwritten, as Unreal's `DoesSaveGameExist`
keeps a damaged slot apart from a missing one: `engine.save_data` refuses
that slot until the game calls `engine.discard_save(slot)`, which moves it
aside to `<slot>.save.discarded-<n>` (`engine_integration_save_corrupt_kept`).
`engine.list_saves()` answers every slot, sorted, as `{slot, saved_at,
bytes, status, legacy}` read from the headers alone, and
`engine.get_save_limit()` the largest save the project allows (4 MiB
unless Project Settings sets another); a project holds at most 256
slots. A `save.json` from before slots existed loads as the `"default"`
slot until the game next saves it, which moves it aside to
`save.json.migrated-<n>` (`engine_unit_save_data`,
`engine_unit_save_data_bindings`). The console's `spawn <prefab>
[x y z]` instantiates a prefab by a path inside the project, placed at
x y z when given while keeping the prefab's rotation and scale; anything
but three finite numbers refuses the command (`engine_integration_sandbox`). It changes the running game, so the
editor's Log runs it only in Play; in Edit mode it is refused, since the
entity would bypass undo and the unsaved-changes prompt: use the Create
menu or drag the prefab in instead (`engine_unit_editor_console_commands`).
A prefab, a cooked mesh or a model dragged from the Assets panel into the
Scene view lands on what the cursor points at (the ground when nothing),
and dropped on an entity in the Entities panel it becomes that entity's
child; double-clicking one places it under the camera's focus. Each is
one Undo step. A prefab instance keeps every component and its
rotation and scale. A model places the mesh cooked from it, and a skinned
mesh plays the animation controller in its folder that drives its
skeleton, when exactly one does (`engine_unit_editor_asset_place`).

Up to four controllers are tracked, each in the slot it arrived in; a slot
is not reused by another controller while its own stays connected, so
unplugging the first leaves the second in slot 1. Actions and axes bound to
a gamepad button or stick read any connected controller, as Unity's and
Godot's unpaired actions do, and so do `engine.is_gamepad_connected()`,
`engine.is_gamepad_button_down(button)` and `engine.gamepad_axis_value(axis)`
with no slot. Pass a slot from 0 (`engine.is_gamepad_button_down(button, 1)`,
`engine.gamepad_axis_value(axis, deadzone, 1)`) to read one player's own
controller; `engine.gamepad_count()` says how many are connected
(`engine_unit_input_map`, `engine_unit_input_ingress_lua`).

Scripts run sandboxed: `io`, `os`, `debug` and `package` are not there, all
scripts share a budget of Lua instructions per frame (1,000,000 by default;
a script that runs past it stops with an error rather than freezing the
game, and only that script stops: the hooks after it that frame are
skipped and run again the next (`engine_integration_lua_hardening`)), and
the Lua allocator is capped (64 MiB by default). A project sets
its own limits in Edit > Project Settings..., 0 for unlimited; they are
saved in its `.project` document (an optional `"scripting"` object with
`instructionLimit` and `memoryLimitMiB`, left out while both are the
defaults), take effect at once, and apply wherever the project runs,
editor or player (`engine_unit_editor_project_settings`,
`engine_integration_project_open`). Its collision layers are saved there
too, as an optional `"physics"` object: `"layers": [{"bit": 3, "name":
"Player"}]` and `"ignoredPairs": [[3, 4]]`, left out while every layer is
unnamed and every pair collides. Its largest save slot, set in Project
Settings > Saves from 1 to 256 MiB, is saved as `"saves": {"maxSlotMiB":
16}`, left out at the 4 MiB default; a lower limit still loads the larger
saves written before it.

The scripting surface is still evolving. Some APIs are generated from annotated accessors, while the hand-written surface lives in domain binding translation units under `scripting/src/` (entity lifecycle, body, mesh/material, physics, lights, camera, audio, input, timers, coroutines, and more).

## Assets and mesh conversion

Selecting a mesh, texture or sound source in the Assets panel shows its Import Settings, saved to the source's `.meta` as they are edited, as Unity's importer inspector does. A mesh's (mesh and primitive index, scale, up axis, generated normals) are part of its cook. A texture's are applied when it loads: Color Space (Auto leaves it to the material slot, sRGB for base colour and emissive and linear for data maps; sRGB or Linear says what the file holds whatever samples it), Generate Mip Maps, Filter (Linear, or Nearest for pixel art) and Wrap (Repeat or Clamp). Editing them reloads the texture (`engine_unit_texture_loader_handles`, `engine_unit_editor_import_settings`). A sound's are applied when `load_sound` decodes it: Sample Rate (Preserve keeps the file's, or 8,000 to 192,000 Hz resamples it) and Force To Mono, each shrinking the memory the decoded sound holds; they take effect the next time the sound loads, and `play_music` streams the file as it is (`engine_unit_audio_import_settings`). Other asset types have no import settings yet.

The runtime mounts the open project's content root at `assets/` and the engine's own content from `engine_assets/` at `engine/` (`EngineConfig::assetRoot` and `engineRoot`). `engine::open_project` sets the content root from the project document. A project's packages, the add-ons it depends on (Unity's packages, Godot's addons), are listed in its `.project` document as `"dependencies": [{"name": "ui_kit", "source": "packages/ui_kit"}]`; each lives in the project's `packages/<name>/` folder, is mounted at `packages/<name>/` and catalogued like the project's own content, so its assets are referenced and its scripts loaded by those paths (`engine_integration_project_open`). A package asset that claims another's identity is reported by path at startup (`engine_unit_asset_catalog`). The Assets panel does not list packages yet (#759). The engine's content is found through `ENGINE_ROOT`, then beside the executable, then in the working directory, unless the config names it; CMake copies it and the sample project into the build output, and bootstrap refuses an engine root that is not there.

For mesh conversion, build and run `asset_packer`:

```powershell
cmake --build build --target asset_packer
build\tools\asset_packer\asset_packer.exe <input.gltf|input.glb> <output.mesh>
```

Tool behavior:

- Deterministic cook: identical inputs produce byte-identical outputs
- Imports one primitive of one glTF mesh per `.mesh` (chosen by
  `importSettings.meshIndex` and `primitiveIndex` in the source's `.meta`,
  mesh 0 primitive 0 by default), plus skeletons and animation clips. A
  cook names on stderr every primitive it leaves out
  (`engine_integration_asset_packer_uncooked_primitives`)
- Writes `.mesh`, `.cookmeta`, `.cookstamp` and a collision `.hull`, plus
  `.skel` and `<clip>.anim` for rigged input
- Keys a cook to no host: the stamp reads `PLATFORM Any`, so a cook
  committed on one OS is up to date on every other, and
  `tools/check_asset_identity.py` fails a committed stamp keyed to a
  platform. `--platform <tag>` keys a target-specific cook
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
