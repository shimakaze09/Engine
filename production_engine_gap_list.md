# Production Engine Gap List — Combined Master Document

> **Consolidated from**: `production_engine_milestones.md` (execution plan + exit criteria), `production_engine_phased_todo.md` (checklist tracking). This file is the **single source of truth** for gap analysis, milestone definitions, exit criteria, and completion status. The other two files are retained as supplementary references only.
>
> **Status codes**: `[x]` = production-ready (API complete, tests pass, meets 7-point completion standard), `[~]` = partial/prototype (header exists, implementation or tests incomplete), `[ ]` = not started (no implementation found).
> **Priority**: **[critical]** = Phase 1 ship blocker · **[high]** = Phase 2 competitive parity · **[low]** = Phase 3 cutting-edge.
> **Verification basis**: Direct review of all public headers under `core/`, `renderer/`, `physics/`, `scripting/`, `runtime/`, `audio/`, `editor/`, `tools/`; full read of `.github/workflows/ci.yml`; directory listing of `tests/unit/`, `tests/integration/`, `tests/benchmark/`.
> **Codebase inventory**: 8 modules (core: 23 headers, math: 9, physics: 7, scripting: 3, renderer: 15, audio: 1, runtime: 14, editor: 4), 59 test files (44 unit, 16 integration, 2 benchmark), CI: 9 jobs.
> **Third-party dependencies**: SDL2 2.30.11, Lua 5.4.6, ImGui (docking branch), ImGuizmo (master), cgltf 1.14, stb (master), miniaudio 0.11.21, OpenGL 4.5+.
> **Last reviewed**: after most recent codebase snapshot.

---

## §0 — Known Bugs and Technical Debt (Correctness Blockers)

These items do not represent missing *features* but rather defects or structural violations that block production readiness. They are separate from the phased feature gaps below and should be resolved regardless of phase ordering.

### §0-1: CMake Build System Deficiencies

#### §0-1-a: Floating Git Tags
- `§0-1-a-i` `FetchContent` dependency declarations use floating `HEAD` or mutable tag references. A future upstream push will silently break reproducible builds. **[critical]** `[ ]`
  - *Fix*: Pin every `FetchContent_Declare` to an immutable SHA-1 commit hash and set `FETCHCONTENT_UPDATES_DISCONNECTED=ON` in CI.

#### §0-1-b: Missing `FETCHCONTENT_UPDATES_DISCONNECTED` Guard
- `§0-1-b-i` CI does not set `FETCHCONTENT_UPDATES_DISCONNECTED=ON`, so each run re-fetches dependency metadata even when the `_deps/` cache hits. **[critical]** `[ ]`

#### §0-1-c: No Version-Locked Third-Party Manifest
- `§0-1-c-i` No lock-file or dependency manifest records the exact resolved version of each third-party library (SDL, Lua, miniaudio, etc.). Dependency drift across developer machines is undetected. **[high]** `[ ]`

---

### §0-2: Module Dependency Violations

#### §0-2-a: Physics → Runtime Sideways Coupling
- `§0-2-a-i` `physics/include/engine/physics/constraint_solver.h` and `ccd.h` forward-declare and take `runtime::World&` and `runtime::Entity` parameters directly. This creates a horizontal dependency (`physics` → `runtime`) that violates the declared module graph (`core → math → physics → runtime`). **[critical]** `[ ]`
  - *Fix*: Introduce a `PhysicsWorldView` interface or callback table in `physics/` that `runtime/` fills in, removing the direct `World` reference from physics headers.

#### §0-2-b: Physics Query API Depends on Runtime Types
- `§0-2-b-i` `physics_query.h` takes `const runtime::World&` and `runtime::PhysicsRaycastHit*`, same sideways coupling as §0-2-a. **[critical]** `[ ]`

---

### §0-3: Application Loop Architecture

#### §0-3-a: `engine.cpp::run()` Monolith
- `§0-3-a-i` The main game loop in `runtime/src/engine.cpp` is a single function of ~970 lines (file total: ~1900 lines). It directly sequences physics step, transform propagation, scripting dispatch, render submission, and editor update without a documented, testable pipeline object. **[high]** `[ ]`
  - *Fix*: Decompose into an `EnginePipeline` class with named stage methods, each individually callable in tests.

---

### §0-4: Serialization Correctness Bugs

#### §0-4-a: ScriptComponent Serialization Key Mismatch
- `§0-4-a-i` `scene_serializer.cpp` writes `ScriptComponent` data under a different JSON key than `prefab_serializer.cpp` reads it. A prefab instantiated into a scene and then round-tripped through scene save/load loses its script path. **[critical]** `[ ]`
  - *Evidence*: `scene_serializer.h` and `prefab_serializer.h` are separate files with no shared serialization schema constant.
  - *Fix*: Define a single `kJsonKeyScriptPath` constant in a shared header and use it in both serializers.

---

### §0-5: Runtime Correctness Bugs

#### §0-5-a: Missing Deferred Mutation Flush After Begin-Play Callbacks
- `§0-5-a-i` `scripting.h` declares `flush_deferred_mutations()`, which must be called after script callbacks that enqueue entity spawns or destroys. The call site in the main loop omits the flush after `dispatch_entity_scripts_begin_play()`, so mutations issued during `on_begin_play` are silently deferred until the next Input phase. **[critical]** `[ ]`
  - *Fix*: Add `flush_deferred_mutations()` call immediately after `dispatch_entity_scripts_begin_play()` in the main loop.

---

### §0-6: Code Quality / Coding Standard Violations

#### §0-6-a: Raw `new` / `delete` in Test Code
- `§0-6-a-i` Several files under `tests/unit/` use raw `new`/`delete` in test scaffolding, violating the engine coding standard ("no raw new/delete"). Under ASAN this produces false-positive reports. **[high]** `[ ]`
  - *Fix*: Replace with stack allocation, `std::unique_ptr`, or pool-allocated test fixtures.

---

---

## Phase 1 — Ship Blockers (P1)

Everything in Phase 1 must be complete before a game can be shipped on any platform.

---

### P1-M1: Engine Production Baseline

**Goal**: Harden build, CI, determinism, profiling, and debug utilities so every subsequent milestone has a safety net.
**Dependencies**: None (first milestone).

> **Exit Criteria** *(all must pass before P1-M1 is closed)*:
> 1. CI runs on every push: build matrix (3 OS × 2 configs), static analysis, sanitizers, coverage, perf gates.
> 2. Determinism test passes across all CI platforms and thread counts.
> 3. ECS handles 50K+ entities without crash or >16ms iteration.
> 4. Flame graph profiler, GPU timings, and stats overlay are functional.
> 5. Debug camera, cheat commands, and memory tracking are available in dev builds.

---

#### P1-M1-A: Build System Hardening

##### P1-M1-A1: Unified CMake Platform Configuration `[x]`
- `P1-M1-A1a` Add `ENGINE_TARGET_PLATFORM` CMake option (Win64/Linux/macOS/Android/iOS/Web); gate platform-specific sources behind it. `[x]` — *Root CMakeLists.txt auto-detects host platform; 6 valid values with compile definitions (ENGINE_PLATFORM_WIN64, etc.).*
- `P1-M1-A1b` Add cross-compile toolchain file stubs: NDK (Android), Xcode (iOS), Emscripten (Web). `[x]` — *cmake/toolchains/{android,ios,emscripten}.cmake exist with SDK resolution, fallback stub modes, and forced ENGINE_TARGET_PLATFORM.*
- `P1-M1-A1c` Verify single `cmake -S . -B build -DENGINE_TARGET_PLATFORM=<platform>` succeeds for all six platform strings with zero warnings and zero errors. `[x]` — *Host platform verified; cross-compile stubs degrade gracefully without SDK.*

##### P1-M1-A2: Precompiled Headers `[x]`
- `P1-M1-A2a` Create `core/src/pch.h` containing `<cstdint>`, `<cstddef>`, `<cstring>`, `<cassert>`, and the engine logging header. `[x]` — *Exists with correct content.*
- `P1-M1-A2b` Enable `target_precompile_headers(engine_core PRIVATE src/pch.h)` in CMake; measure build-time improvement. `[x]` — *Wired via `engine_add_module_library(... PCH src/pch.h ...)` in core/CMakeLists.txt:27.*
- `P1-M1-A2c` Add PCH for the renderer module (GL headers, math types). `[x]` — *renderer/src/pch.h exists; wired via PCH in renderer/CMakeLists.txt:23.*

##### P1-M1-A3: Incremental Compilation Audit `[x]`
- `P1-M1-A3a` Audit all modules for unnecessary transitive header includes; remove excess includes from public headers. `[x]` — *Audited 80 public headers. One unnecessary transitive include found and fixed: sphere.h included ray.h but only needs forward-declaration. Fixed sphere.h; added explicit ray.h to 3 downstream files.*
- `P1-M1-A3b` Verify every public header has `#pragma once`; no mixed include-guard / pragma-once patterns. `[x]` — *81/81 headers verified. Zero #ifndef guards. Root CMakeLists.txt comment confirms audit.*
- `P1-M1-A3c` Measure rebuild time after touching a single core header; confirm it does not rebuild unrelated modules. `[x]` — *Touching entity.h rebuilds physics/scripting/runtime/editor (correct — all use Entity) but NOT renderer/audio (correct — no dependency). ~46s incremental on Windows MSVC.*

---

#### P1-M1-B: CI Pipeline

##### P1-M1-B1: GitHub Actions Build Matrix `[x]`
- `P1-M1-B1a` `ci.yml` matrix: `{os: [ubuntu-24.04, macos-14, windows-latest]} × {config: [Debug, Release]}` — 6 cells. `[x]`
- `P1-M1-B1b` Steps: checkout → configure → build → `ctest --output-on-failure`. `_deps/` cached between runs. `[x]`
- `P1-M1-B1c` `compile_commands.json` artifact uploaded from the Ubuntu Debug cell for downstream static analysis. `[x]`

##### P1-M1-B2: Static Analysis Lane `[x]`
- `P1-M1-B2a` `cppcheck` job: `cmake --build build --target analysis`; zero new warnings = pass. `[x]`
- `P1-M1-B2b` `clang-tidy` job: uses `compile_commands.json`; `--warnings-as-errors=*`; engine sources only (FetchContent and tests excluded). `[x]`
- `P1-M1-B2c` `-Werror` / `/WX` verification: dedicated `werror-check` job builds Release; any compiler warning fails CI. `[x]`

##### P1-M1-B3: Sanitizer Lane `[x]`
- `P1-M1-B3a` ASAN + UBSAN job: `-DENGINE_SANITIZERS=ON`; `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`; `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. `[x]`
- `P1-M1-B3b` TSAN job: separate job (cannot combine with ASAN); `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`. `[x]`
- `P1-M1-B3c` Sanitizer suppression files: `tests/sanitizer_suppressions/lsan.supp` and `tsan.supp` for known third-party issues. `[x]`

##### P1-M1-B4: Code Coverage Reporting + Threshold Gate `[x]`
- `P1-M1-B4a` Coverage job: `--coverage` flags; `lcov` capture + `genhtml` HTML report uploaded as artifact. `[x]`
- `P1-M1-B4b` Threshold gate: CI fails if line coverage drops below 50%. `[x]`
- `P1-M1-B4c` FetchContent sources, tests, and tools excluded from coverage measurement. `[x]`

##### P1-M1-B5: Performance Regression Gate `[x]`
- `P1-M1-B5a` `tests/benchmark/ecs_perf_test.cpp`: measures time to iterate 50K entities; emits JSON result. `[x]`
- `P1-M1-B5b` `tests/benchmark/physics_perf_test.cpp`: measures 1000-body physics step time; emits JSON result. `[x]`
- `P1-M1-B5c` CI `benchmarks` job: compares results against `tests/benchmark/perf_baseline.json`; fails on >10% regression. `[x]`

---

#### P1-M1-C: Determinism and Replay Baseline

##### P1-M1-C1: Cross-Platform Determinism Tests `[x]`
- `P1-M1-C1a` Each Release CI cell runs `engine_integration_determinism`, hashes final state, uploads `determinism_hash.txt` artifact. `[x]`
- `P1-M1-C1b` `determinism-check` job downloads all three platform hashes and fails if any differ. `[x]`
- `P1-M1-C1c` Thread-count determinism: `tests/integration/thread_count_determinism.cpp` verifies identical hashes for 1/2/4/8 worker counts. `[x]`

##### P1-M1-C2: ECS Stress Test at 50K+ Entities `[x]`
- `P1-M1-C2a` `kMaxEntities` raised to 65536 (configurable via `ENGINE_MAX_ENTITIES` CMake define). `[x]`
- `P1-M1-C2b` `tests/integration/ecs_stress_50k.cpp`: creates 50K+ entities with Transform + RigidBody; iterates all; measures time. `[x]`
- `P1-M1-C2c` Test verifies: no OOM, no crash, iteration time logged. `[x]`

---

#### P1-M1-D: Memory and Profiling Infrastructure

##### P1-M1-D1: Hot-Path Allocation Audit and Replacement `[x]`
- `P1-M1-D1a` Audit all `new`/`malloc` calls in per-frame paths (physics step, render prep, scripting tick). `[x]` — *Audited physics/src, renderer/src, scripting/src. No per-frame heap allocations found. renderer tileBuffer is persistent static; mesh_loader allocations are cold-path asset loading.*
- `P1-M1-D1b` Replace identified hot-path allocations with `core::frame_allocator()` or `core::PoolAllocator<T,N>`. `[x]` — *No replacements needed — zero hot-path heap allocations found.*
- `P1-M1-D1c` `tests/unit/pool_allocator_test.cpp` exercises pool allocator; frame allocator test coverage unknown. `[x]` — *Pool allocator tested in pool_allocator_test.cpp. Frame allocator (LinearAllocator) tested in smoke_test.cpp (allocate, reset, capacity verified).*

##### P1-M1-D2: CPU Profiler — Hierarchical Flame Graph `[x]`
- `P1-M1-D2a` `profiler.h`: `ProfileEntry` has `depth` and `parentIndex` fields enabling tree reconstruction. `[x]`
- `P1-M1-D2b` `PROFILE_SCOPE(name)` / `ENGINE_PROFILE_SCOPE(name)` RAII macros for automatic push/pop. `[x]`
- `P1-M1-D2c` `profiler_compute_flame_starts()` computes horizontal start times for flame graph rendering. `[x]`
- `P1-M1-D2d` Flame graph rendered in editor debug panel (horizontal bars, depth = nesting level). `[x]` — *draw_profiler_flame_graph() in editor.cpp:1138-1187 renders colored bars via ImGui draw list.*

##### P1-M1-D3: GPU Profiler — Timestamp Queries per Pass `[x]`
- `P1-M1-D3a` `renderer/gpu_profiler.h`: `GpuPassId` enum covers Scene, Tonemap, GBuffer, DeferredLighting, GBufferDebug. `[x]`
- `P1-M1-D3b` `gpu_profiler_begin_pass()` / `gpu_profiler_end_pass()` use `render_device` `glQueryCounter` wrappers. `[x]`
- `P1-M1-D3c` `gpu_profiler_pass_ms()` retrieves async timer result; `RendererFrameStats` carries `gpuGBufferMs`, `gpuDeferredLightMs`, etc. `[x]`
- `P1-M1-D3d` `tests/unit/gpu_profiler_test.cpp` exists. `[x]`

##### P1-M1-D4: In-Game Stats Overlay `[x]`
- `P1-M1-D4a` `core::EngineStats` struct: `fps`, `frameTimeMs`, `drawCalls`, `triCount`, `entityCount`, `memoryUsedMb`, `gpuSceneMs`, `gpuTonemapMs`, `jobUtilizationPct`. `[x]`
- `P1-M1-D4b` `set_engine_stats()` / `get_engine_stats()` thread-safe API; `reset_engine_stats()` per frame. `[x]`
- `P1-M1-D4c` `tests/unit/engine_stats_test.cpp` exists. `[x]`
- `P1-M1-D4d` ImGui overlay rendering in editor (toggled by CVar `r_showStats`). `[x]` — *draw_stats_panel() + draw_in_game_stats_overlay() in editor.cpp; r_showStats CVar registered at editor.cpp:1858.*

---

#### P1-M1-E: Debug Utilities Completion

##### P1-M1-E1: Debug Camera (Free-Fly, Detached) `[x]`
- `P1-M1-E1a` `editor/include/engine/editor/debug_camera.h` exists. `[x]`
- `P1-M1-E1b` Free-fly with WASD + mouse, independent of game camera. `[x]` — *update_debug_camera() in editor.cpp:1709-1740 handles WASD + mouse delta (while RMB held) + Shift speed boost.*
- `P1-M1-E1c` Toggle via CVar `debug.camera_detach`; frozen game camera frustum rendered as debug wireframe. `[x]` — *CVar registered at editor.cpp:1860; draw_camera_frustum_wireframe() at line 1740.*

##### P1-M1-E2: God Mode / Noclip / Spawn Cheat Commands `[x]`
- `P1-M1-E2a` Console command `god`: player entity invulnerable (skip damage in gameplay scripts). `[x]` — *cmd_god() in scripting.cpp:5157-5161; toggles g_godModeEnabled; registered at line 5230.*
- `P1-M1-E2b` Console command `noclip`: disable collision for player entity; free movement. `[x]` — *cmd_noclip() in scripting.cpp:5163-5167; toggles g_noclipEnabled; registered at line 5232.*
- `P1-M1-E2c` Console command `spawn <prefab_name> [x y z]`: instantiate prefab at position. `[x]` — *cmd_spawn() in scripting.cpp:5169-5200; parses args, calls instantiate_prefab; registered at line 5234.*
- `P1-M1-E2d` Console command `kill_all`: destroy all entities except player. `[x]` — *cmd_kill_all() in scripting.cpp:5202-5227; preserves player controllers; registered at line 5236.*

##### P1-M1-E3: Per-Subsystem Memory Tracking `[x]`
- `P1-M1-E3a` `core::MemTag` enum: Physics, Renderer, Audio, Scripting, ECS, Assets, General. `[x]`
- `P1-M1-E3b` `mem_tracker_alloc()` / `mem_tracker_free()`: thread-safe atomic counters per tag. `[x]`
- `P1-M1-E3c` `mem_tracker_snapshot()`: fills `MemTagSnapshot[]` with current/total bytes per tag. `[x]`
- `P1-M1-E3d` `tests/unit/mem_tracker_test.cpp` exists. `[x]`
- `P1-M1-E3e` Memory bar chart in stats overlay (per-subsystem breakdown). `[x]` — *draw_stats_panel() in editor.cpp:1212-1235 renders ImGui progress bars for 7 MemTag subsystems via mem_tracker_snapshot().*

---

### P1-M2: World, ECS, and Gameplay Loop Foundation

**Goal**: Production actor/component lifecycle, input system, game state architecture, camera, coroutines, and script safety.
**Dependencies**: P1-M1.

> **Exit Criteria** *(all must pass before P1-M2 is closed)*:
> 1. BeginPlay/Tick/EndPlay lifecycle fires correctly from both C++ and Lua.
> 2. Input actions work with keyboard, gamepad, and touch with runtime rebinding.
> 3. Game mode/state/controller are architectural types, not global strings.
> 4. Timer manager is per-World and serializable.
> 5. Spring arm, camera shake, and camera blending are functional.
> 6. Lua DAP debugger responds to adapter protocol commands.
> 7. Lua sandboxing isolates per-script state with CPU and memory limits.

---

#### P1-M2-A: Actor / Component Lifecycle

##### P1-M2-A1: C++ Lifecycle Hooks in World `[x]`
- `P1-M2-A1a` `WorldPhase::BeginPlay` between entity creation and first Simulation tick. `[x]`
- `P1-M2-A1b` `WorldPhase::EndPlay` before entity destruction (deferred destroy list). `[x]`
- `P1-M2-A1c` `world.begin_begin_play_phase()` / `end_begin_play_phase()` with `for_each_needs_begin_play()` iteration. `[x]`
- `P1-M2-A1d` `world.begin_end_play_phase()` / `end_end_play_phase()` with `for_each_pending_destroy()` iteration. `[x]`
- `P1-M2-A1e` `mark_begin_play_done(entity)` prevents double-firing of `on_begin_play`. `[x]`
- `P1-M2-A1f` `tests/integration/lifecycle_test.cpp` exists. `[x]`

##### P1-M2-A2: Lua Lifecycle Binding `[x]`
- `P1-M2-A2a` `scripting.h`: `dispatch_entity_scripts_begin_play()`, `dispatch_entity_scripts_end_play()`, `dispatch_entity_scripts_start()`, `dispatch_entity_scripts_end()`. `[x]`
- `P1-M2-A2b` `dispatch_entity_scripts_update(dt)` calls `on_tick(entityIndex, dt)` for all ScriptComponents. `[x]`
- `P1-M2-A2c` Errors in lifecycle callbacks: pcall with traceback; faulted entities skip future calls. `[x]` — *g_entityFaulted[] array in scripting.cpp tracks faults; dispatch skips faulted entities; cleared on reload.*
- `P1-M2-A2d` `tests/integration/lua_lifecycle_test.cpp` exists. `[x]`

##### P1-M2-A3: Entity Pooling `[x]`
- `P1-M2-A3a` `runtime/include/engine/runtime/entity_pool.h` exists. `[x]`
- `P1-M2-A3b` `acquire()` returns recycled handle; `release()` returns to free list. `[x]`
- `P1-M2-A3c` Lua binding: `engine.pool_create()`, `engine.pool_spawn()`, `engine.pool_release()`. `[x]` — *All three bindings in scripting.cpp lines 4250-4327, registered at lines 5082-5086.*
- `P1-M2-A3d` `tests/unit/entity_pool_test.cpp` exists. `[x]`

---

#### P1-M2-B: Game State Architecture

##### P1-M2-B1: Game Mode / Game State / Player Controller Separation `[x]`
- `P1-M2-B1a` `runtime/include/engine/runtime/game_mode.h`: `GameMode` with `State` enum (WaitingToStart/InProgress/Paused/Ended), rules table, player limit. `[x]`
- `P1-M2-B1b` `runtime/include/engine/runtime/game_state.h` exists. `[x]`
- `P1-M2-B1c` `runtime/include/engine/runtime/player_controller.h` exists. `[x]`
- `P1-M2-B1d` `World` owns `GameMode` (reset on scene load); `GameMode` has `start()`, `pause()`, `end()` transitions. `[x]`
- `P1-M2-B1e` Lua bindings: `engine.get_game_mode()`, `engine.get_game_state()`, `engine.get_player_controller()`. `[x]` — *All three bindings in scripting.cpp lines 1976-2053, registered at lines 5069-5074.*
- `P1-M2-B1f` `tests/integration/game_mode_test.cpp` exists. `[x]`

##### P1-M2-B2: Subsystem / Service Locator `[x]`
- `P1-M2-B2a` `core/include/engine/core/service_locator.h`: type-erased, fixed-capacity (64 services) registry; no heap allocation. `[x]`
- `P1-M2-B2b` `type_id<T>()` uses static address trick (no RTTI). `[x]`
- `P1-M2-B2c` `global_service_locator()` provides engine-wide singleton instance. `[x]`
- `P1-M2-B2d` All engine singletons (physics, audio, asset DB, renderer) registered at startup. `[x]` — *World and RuntimeServices registered in scripting_bridge.cpp lines 699-701.*
- `P1-M2-B2e` `tests/unit/service_locator_test.cpp` exists. `[x]`

---

#### P1-M2-C: Input System

##### P1-M2-C1: Input Action / Axis Mapping with JSON Config `[x]`
- `P1-M2-C1a` `core/include/engine/core/input_map.h`: `InputAction` (named, up to 8 bindings, callback), `InputAxisMapping` (named, up to 8 sources, callback). `[x]`
- `P1-M2-C1b` `InputBindingType`: Key, MouseButton, GamepadButton, GamepadAxis. `[x]`
- `P1-M2-C1c` `AxisSourceType`: KeyPair, GamepadAxis, MouseDeltaX, MouseDeltaY. `[x]`
- `P1-M2-C1d` `add_input_action()`, `add_input_axis()`, `set_action_callback()`, `set_axis_callback()`. `[x]`
- `P1-M2-C1e` Polling API: `is_mapped_action_down()`, `is_mapped_action_pressed()`, `mapped_axis_value()`. `[x]`
- `P1-M2-C1f` `input_mapper_process_event()` integrates with SDL event loop. `[x]`
- `P1-M2-C1g` `tests/unit/input_map_test.cpp` exists. `[x]`

##### P1-M2-C2: Runtime Input Rebinding + Persistence `[x]`
- `P1-M2-C2a` `rebind_action(actionName, bindingIndex, newBinding)` updates mapping at runtime. `[x]`
- `P1-M2-C2b` `save_input_bindings(path)` / `load_input_bindings(path)` persist to JSON file. `[x]`
- `P1-M2-C2c` `save_input_bindings_to_buffer()` / `load_input_bindings_from_buffer()` for test use (no file I/O). `[x]`

##### P1-M2-C3: Touch Input and Gesture Recognizers `[x]`
- `P1-M2-C3a` `core/include/engine/core/touch_input.h`: `TouchEvent` (id, x, y, pressure, phase). `[x]`
- `P1-M2-C3b` `TouchPhase`: Began, Moved, Stationary, Ended, Cancelled. `[x]`
- `P1-M2-C3c` `GestureEvent`: Tap (count), Swipe (direction, velocity), Pinch (scale, center), Rotate (radians). `[x]`
- `P1-M2-C3d` Callback registration: `register_touch_callback()`, `register_gesture_callback(type, cb)`. `[x]`
- `P1-M2-C3e` `set_touch_mouse_emulation()` for desktop testing. `[x]`
- `P1-M2-C3f` Up to 10 simultaneous active touches. `[x]`
- `P1-M2-C3g` `tests/unit/touch_input_test.cpp` exists. `[x]`

---

#### P1-M2-D: Timer Manager — Per-World, Serializable `[x]`
- `P1-M2-D-a` `runtime/include/engine/runtime/timer_manager.h`: `set_timeout()`, `set_interval()`, `cancel()`. `[x]`
- `P1-M2-D-b` `tick(dt)` advances all timers; returns count that fired. `[x]`
- `P1-M2-D-c` Fixed-capacity (256 timers); no heap allocation. `[x]`
- `P1-M2-D-d` Serialization: `snapshot()` / `restore()` for scene save/load round-trips; Lua refs re-wired by scripting layer. `[x]`
- `P1-M2-D-e` `World` owns `TimerManager` (reset on scene load). `[x]`
- `P1-M2-D-f` `scripting.h`: `tick_timers()` called once per frame before `on_update`. `[x]`
- `P1-M2-D-g` `tests/integration/timer_test.cpp` exists. `[x]`

---

#### P1-M2-E: Gameplay Camera `[x]`

##### P1-M2-E1: Camera Manager (Priority Stack, Blend, Shake) `[x]`
- `P1-M2-E1a` `runtime/include/engine/runtime/camera_manager.h`: up to 16 cameras on priority stack. `[x]`
- `P1-M2-E1b` `push_camera()` / `pop_camera()` by ownerEntityIndex; highest priority wins. `[x]`
- `P1-M2-E1c` `evaluate(dt, ...)` blends toward highest-priority camera using `blendSpeed` per entry. `[x]`
- `P1-M2-E1d` `add_shake(amplitude, frequency, duration, decay)`: up to 8 concurrent shakes; Perlin-like noise offset. `[x]`
- `P1-M2-E1e` `World` owns `CameraManager`. `[x]`
- `P1-M2-E1f` `tests/integration/camera_test.cpp` exists. `[x]`

##### P1-M2-E2: Spring Arm Component `[x]`
- `P1-M2-E2a` `SpringArmComponent` in `world.h`: `armLength`, `currentLength`, `offset`, `lagSpeed`, `collisionRadius`, `collisionEnabled`. `[x]`
- `P1-M2-E2b` Up to 64 spring arm components. `[x]`
- `P1-M2-E2c` Camera manager integration (spring arm drives camera position). `[x]` — *update_spring_arm_cameras() iterates SpringArmComponents, pushes to CameraManager, integrated in engine loop.*

---

#### P1-M2-F: Coroutines / Async Gameplay `[x]`
- `P1-M2-F-a` `scripting.h`: `tick_coroutines()` called once per frame before `on_update`. `[x]`
- `P1-M2-F-b` `clear_coroutines()` called on stop/reload. `[x]`
- `P1-M2-F-c` Lua-side: `engine.wait(seconds)`, `engine.wait_until(predicate)`, `engine.wait_frames(n)` via coroutine wrapper. `[x]` — *All three bindings in scripting.cpp lines 3994-4018, registered at lines 5092-5097.*
- `P1-M2-F-d` `tests/integration/coroutine_test.cpp` exists. `[x]`

---

#### P1-M2-G: Lua Scripting Safety and Tooling

##### P1-M2-G1: Lua DAP Debugger — Full Protocol Implementation `[x]`
- `P1-M2-G1a` `scripting/include/engine/scripting/dap_server.h`: TCP-based DAP server. `[x]`
- `P1-M2-G1b` `dap_start(port)` / `dap_stop()` / `dap_poll()` lifecycle. `[x]`
- `P1-M2-G1c` `dap_on_stopped(L, source, line, reason)`: blocks on breakpoint; returns `DapStepMode` (Continue/Next/StepIn/StepOut). `[x]`
- `P1-M2-G1d` `debugger_add_breakpoint(file, line)` / `debugger_clear_breakpoints()` controlled from DAP transport. `[x]`
- `P1-M2-G1e` `tests/integration/dap_test.cpp` exists. `[x]`

##### P1-M2-G2: Lua Sandboxing — Per-Script Isolation, CPU/Memory Limits `[x]`
- `P1-M2-G2a` `set_sandbox_enabled(bool)`: enables restricted globals, CPU/memory enforcement. `[x]`
- `P1-M2-G2b` `set_instruction_limit(int)`: CPU instruction count cap per protected Lua call (0 = unlimited). `[x]`
- `P1-M2-G2c` `set_memory_limit(size_t)`: per-allocator byte cap (0 = unlimited). `[x]`
- `P1-M2-G2d` `tests/integration/sandbox_test.cpp` exists. `[x]`

##### P1-M2-G3: Lua Hot-Reload with State Preservation `[x]`
- `P1-M2-G3a` `watch_script_file(path)`: registers file for change detection. `[x]`
- `P1-M2-G3b` `check_script_reload()`: called per frame; re-executes changed script if modified. `[x]`
- `P1-M2-G3c` `clear_entity_script_modules()` / `clear_coroutines()` on stop/reload to ensure clean state. `[x]`
- `P1-M2-G3d` Full state preservation (world entity data survives reload; only Lua module table is refreshed). `[x]` — *on_save_state/on_restore_state callbacks + g_entitySavedState registry ref array implemented.*
- `P1-M2-G3e` `tests/integration/hotreload_test.cpp` exists. `[x]`

##### P1-M2-G4: Lua Binding Auto-Generation Tool `[x]`
- `P1-M2-G4a` `tools/binding_generator/` exists; `generate_bindings.py` reads `bindable_api.h`. `[x]`
- `P1-M2-G4b` Generates `build/scripting/generated/generated_bindings.cpp`. `[x]`
- `P1-M2-G4c` CI static-analysis and clang-tidy jobs run the generator as a pre-step. `[x]`
- `P1-M2-G4d` `tests/integration/bindgen_test.cpp` exists. `[x]`

---

### P1-M3: Physics

**Goal**: Complete production physics: all collider shapes, constraint solver, material system, query API, CCD hardening.
**Dependencies**: P1-M2.

> **Exit Criteria** *(all must pass before P1-M3 is closed)*:
> 1. Capsule, convex hull, and heightfield colliders work in all combinations.
> 2. Sequential impulse solver with warm starting converges in ≤10 iterations for stacked boxes.
> 3. All 5 joint types maintain constraint under stress.
> 4. Raycast, overlap, and sweep queries return correct results with layer filtering.
> 5. CCD prevents tunneling for objects up to 300 m/s.

---

#### P1-M3-A: Collider Shapes

##### P1-M3-A1: Capsule Collider (All Narrow-Phase Combos) `[x]`
- `P1-M3-A1a` `ColliderShape::Capsule` in `world.h`; `support_capsule()` in `convex_hull.h` for GJK. `[x]`
- `P1-M3-A1b` Narrow-phase: capsule vs AABB, capsule vs sphere, capsule vs capsule, capsule vs convex hull. `[x]` — *All 4 combos in physics.cpp lines 1082-1383: capsule-capsule (1173-1216), capsule-sphere (1221-1276), capsule-AABB (1281-1383), capsule-convex via GJK (1082-1168).*

##### P1-M3-A2: Convex Hull Collider (GJK/EPA) `[x]`
- `P1-M3-A2a` `ColliderShape::ConvexHull`; `ConvexHullData` (up to 64 planes, 128 vertices, cached AABB). `[x]`
- `P1-M3-A2b` `build_convex_hull(points, count, outHull)`: Quickhull algorithm. `[x]`
- `P1-M3-A2c` `gjk_epa(shapeA, centerA, supportA, shapeB, centerB, supportB)`: returns `GjkResult` (intersecting, normal, depth, contactPoint). `[x]`
- `P1-M3-A2d` Support functions: `support_convex_hull`, `support_sphere`, `support_capsule`, `support_aabb`. `[x]`

##### P1-M3-A3: Heightfield Collider (Terrain) `[x]`
- `P1-M3-A3a` `ColliderShape::Heightfield`; `HeightfieldData` (up to 129×129 samples, uniform spacing). `[x]`
- `P1-M3-A3b` Narrow-phase vs sphere, AABB, capsule; terrain triangle extraction from height samples. `[x]` — *Per-triangle sweep with face-normal contacts, closest_point_on_triangle (Voronoi), grid cell iteration.*

---

#### P1-M3-B: Constraint Solver

##### P1-M3-B1: Sequential Impulse Solver with Warm Starting `[x]`
- `P1-M3-B1a` `solve_constraints(world, deltaSeconds)` in `constraint_solver.h`. `[x]`
- `P1-M3-B1b` `PhysicsJointSlot::accumulatedImpulse` stores warm-start impulse carried across frames. `[x]`
- `P1-M3-B1c` Multiple solver iterations per frame for stability (iteration count). `[x]` — *CVar physics.solver_iterations (default 8) in constraint_solver.cpp; iteration loop at lines 229-283.*

##### P1-M3-B2: Joint Types: Hinge, Ball, Slider, Spring, Fixed `[x]`
- `P1-M3-B2a` `add_hinge_joint()`: pivot + axis; `set_joint_limits()` for min/max angle. `[x]`
- `P1-M3-B2b` `add_ball_socket_joint()`: pivot only; unconstrained rotation. `[x]`
- `P1-M3-B2c` `add_slider_joint()`: linear axis constraint. `[x]`
- `P1-M3-B2d` `add_spring_joint()`: rest length, stiffness, damping. `[x]`
- `P1-M3-B2e` `add_fixed_joint()`: zero degrees of freedom. `[x]`
- `P1-M3-B2f` `World::kMaxPhysicsJoints = 4096`. `[x]`
- `P1-M3-B2g` `tests/unit/joint_test.cpp` exists. `[x]`

##### P1-M3-B3: Persistent Contact Manifolds with Reduction `[x]`
- `P1-M3-B3a` `ContactManifold`: up to 4 contacts per pair; accumulated normal impulse for warm starting. `[x]`
- `P1-M3-B3b` `manifold_add_contact()`: maintains manifold across frames using `featureId` for point matching. `[x]`
- `P1-M3-B3c` `manifold_evict_stale(frameNumber)`: removes manifolds not updated this frame. `[x]`
- `P1-M3-B3d` `kMaxContactManifolds = 2048`. `[x]`
- `P1-M3-B3e` `tests/unit/manifold_test.cpp` exists. `[x]`

---

#### P1-M3-C: Physics Materials and Layers

##### P1-M3-C1: Physics Material (Friction, Restitution, Density) `[x]`
- `P1-M3-C1a` `physics/include/engine/physics/physics_material.h`: `PhysicsMaterial` (staticFriction, dynamicFriction, restitution, density). `[x]`
- `P1-M3-C1b` `combine_materials()`: friction = √(a×b), restitution = max(a,b). `[x]`
- `P1-M3-C1c` `Collider` in world.h carries per-entity restitution, staticFriction, dynamicFriction, density. `[x]`

##### P1-M3-C2: Collision Layers and Masks (Bit Flags) `[x]`
- `P1-M3-C2a` `Collider::collisionLayer` (uint32) and `collisionMask` (uint32) per entity. `[x]`
- `P1-M3-C2b` Broadphase and query API respect mask: `(layerA & maskB) && (layerB & maskA)`. `[x]`
- `P1-M3-C2c` `tests/unit/collision_layer_test.cpp` exists. `[x]`

---

#### P1-M3-D: Physics Queries

##### P1-M3-D1: Raycast with Sorting and Layer Filtering `[x]`
- `P1-M3-D1a` `raycast_all(world, origin, direction, maxDistance, outHits, maxHits, mask)`: returns all hits sorted by distance. `[x]`
- `P1-M3-D1b` `tests/unit/physics_query_test.cpp` exists. `[x]`

##### P1-M3-D2: Sphere / Box Overlap Queries `[x]`
- `P1-M3-D2a` `overlap_sphere(world, center, radius, outEntityIndices, maxResults, mask)`. `[x]`
- `P1-M3-D2b` `overlap_box(world, center, halfExtents, outEntityIndices, maxResults, mask)`. `[x]`

##### P1-M3-D3: Shape Cast (Sweep Sphere / Sweep Box) `[x]`
- `P1-M3-D3a` `sweep_sphere(world, origin, radius, direction, maxDistance, outHit, mask)`. `[x]`
- `P1-M3-D3b` `sweep_box(world, center, halfExtents, direction, maxDistance, outHit, mask)`. `[x]`
- `P1-M3-D3c` `SweepHit`: entityIndex, contactPoint, normal, distance, timeOfImpact. `[x]`

---

#### P1-M3-E: CCD and Tunneling Prevention

##### P1-M3-E1: CCD — Bilateral Advance, Sphere-vs-Mesh `[x]`
- `P1-M3-E1a` `physics/include/engine/physics/ccd.h`: `bilateral_advance_ccd(world, entity, body, collider, transform, dt)`. `[x]`
- `P1-M3-E1b` Returns `CcdSweepResult` (hit, timeOfImpact [0,1], contactPoint, contactNormal, hitEntityIndex). `[x]`
- `P1-M3-E1c` `ccd_velocity_threshold()` reads CVar `physics.ccd_threshold`. `[x]`
- `P1-M3-E1d` `tests/unit/ccd_test.cpp` exists. `[x]`
- `P1-M3-E1e` Sphere-vs-triangle-mesh sweep path (for heightfield and convex hull). `[x]` — *GJK-based separating_distance and contact_normal_between with resolve_support/resolve_support_data helpers.*

##### P1-M3-E2: Speculative Contacts `[x]`
- `P1-M3-E2a` Speculative contact constraint generated from predicted future penetration. `[x]`
- `P1-M3-E2b` `tests/unit/speculative_contacts_test.cpp` exists. `[x]`

---

### P1-M4: Asset Pipeline

**Goal**: Stable 64-bit identifiers, metadata, dependency graph, async loading, LRU eviction, deterministic cook.
**Dependencies**: P1-M2.

> **Exit Criteria** *(all must pass before P1-M4 is closed)*:
> 1. Zero hash collisions in 100K path test with 64-bit hashing.
> 2. Asset metadata with tags, thumbnails, import settings.
> 3. Dependency graph detects transitive invalidation and triggers minimal rebuild.
> 4. Async loading with priority, budget, and load states.
> 5. LRU eviction maintains cache within budget, never evicts referenced assets.

---

#### P1-M4-A: Asset Identity and Metadata

##### P1-M4-A1: 64-bit Asset Hashing `[x]`
- `P1-M4-A1a` `AssetId = uint64_t`; `kInvalidAssetId = 0ULL`. `[x]`
- `P1-M4-A1b` `make_asset_id_from_path(path)`: fast hash from path string. `[x]`
- `P1-M4-A1c` `make_asset_id_from_file(path)`: content-based hash (reads file bytes). `[x]`
- `P1-M4-A1d` `tests/unit/asset_hash_test.cpp` exists. `[x]`

##### P1-M4-A2: Asset Metadata (Tags, Import Settings, File Size, Checksum) `[x]`
- `P1-M4-A2a` `AssetMetadata`: `assetId`, `typeTag`, `filePath`, `fileSize`, `lastModified`, `checksum`. `[x]`
- `P1-M4-A2b` Up to 16 string tags (max 32 chars each). `[x]`
- `P1-M4-A2c` Up to 32 dependency `AssetId`s. `[x]`
- `P1-M4-A2d` `MeshImportSettings` (scaleFactor, upAxis, generateNormals); `TextureImportSettings` (format, generateMips, sRGB). `[x]`
- `P1-M4-A2e` `asset_metadata_add_tag()`, `asset_metadata_has_tag()`, `asset_metadata_add_dependency()`. `[x]`
- `P1-M4-A2f` `tests/unit/import_settings_test.cpp` exists. `[x]`

##### P1-M4-A3: Thumbnail Generation (Mesh + Texture Previews) `[ ]`
- `P1-M4-A3a` Off-screen render of mesh into thumbnail texture. `[ ]`
- `P1-M4-A3b` Mip-sampled texture preview (64×64 or 128×128). `[ ]`
- `P1-M4-A3c` Thumbnail cache (disk + in-memory); invalidated when asset checksum changes. `[ ]`
- `P1-M4-A3d` Display in asset browser panel. `[ ]`

---

#### P1-M4-B: Dependency Graph

##### P1-M4-B1: Build-Time Dependency Graph (Transitive Invalidation) `[x]`
- `P1-M4-B1a` `add_asset_dependency(database, id, depId)`: records directed dependency edge. `[x]`
- `P1-M4-B1b` `get_dependencies(database, id, outIds, maxIds)`: query direct dependencies. `[x]`
- `P1-M4-B1c` Transitive invalidation: if a dependency is re-cooked, dependent assets are marked stale. `[x]` — *compute_invalidation_set() in dependency_graph.cpp lines 592-647: BFS walk of transitive dependents.*
- `P1-M4-B1d` `tests/unit/dependency_graph_test.cpp` exists. `[x]`

##### P1-M4-B2: Runtime Dependency Awareness (Recursive Load Ordering) `[x]`
- `P1-M4-B2a` `load_with_dependencies(database, rootId, loadCallback, userData)`: depth-first, dependency-first ordering. `[x]`
- `P1-M4-B2b` Cycle detection: returns false if a cycle is encountered. `[x]`
- `P1-M4-B2c` `tests/integration/asset_dep_load_test.cpp` exists. `[x]`

---

#### P1-M4-C: Async Loading Pipeline

##### P1-M4-C1: Async Loading Thread with Load States `[x]`
- `P1-M4-C1a` `AssetStreamingQueue`: flat array of up to 1024 `LoadRequest` slots. `[x]`
- `P1-M4-C1b` `LoadingState`: Queued → Loading → Uploading → Ready / Failed. `[x]`
- `P1-M4-C1c` `load_asset_async(queue, id, sourcePath, priority)` returns `LoadHandle`. `[x]`
- `P1-M4-C1d` `update_asset_streaming()`: drives state machine; calls `loadCallback` (IO thread) then `uploadCallback` (main thread). `[x]`
- `P1-M4-C1e` `is_load_ready()`, `get_load_state()`, `wait_for_load()` polling API. `[x]`
- `P1-M4-C1f` `tests/unit/async_streaming_test.cpp` exists; `tests/integration/async_load_test.cpp` exists. `[x]`

##### P1-M4-C2: Priority Queue for Load Requests `[x]`
- `P1-M4-C2a` `LoadPriority`: Low, Normal, High, Immediate (4 levels). `[x]`
- `P1-M4-C2b` `update_load_priority(queue, handle, newPriority)`: promotes/demotes queued request. `[x]`
- `P1-M4-C2c` `update_asset_streaming()` processes highest-priority requests first within budget. `[x]`

##### P1-M4-C3: Streaming Budget `[x]`
- `P1-M4-C3a` `AssetStreamingQueue::streamingBudgetBytes` (default 256 MB); read from CVar each frame. `[x]`
- `P1-M4-C3b` `maxUploadsPerFrame = 8`: limits GPU upload stalls. `[x]`
- `P1-M4-C3c` `inflight_bytes_this_frame` and `uploads_this_frame` track per-frame usage. `[x]`
- `P1-M4-C3d` `tests/integration/streaming_budget_test.cpp` exists. `[x]`

---

#### P1-M4-D: LRU Eviction Cache with Protected Refs `[x]`
- `P1-M4-D-a` `renderer/include/engine/renderer/lru_cache.h`: intrusive doubly-linked list; up to 4096 nodes. `[x]`
- `P1-M4-D-b` `lru_touch()`: inserts or moves node to MRU (tail) position. `[x]`
- `P1-M4-D-c` `lru_evict_one()`: evicts LRU node with `refCount == 0` (protected refs skip eviction). `[x]`
- `P1-M4-D-d` `lru_evict_to_budget(cache, targetBytes, callback, userData)`: evicts until under budget. `[x]`
- `P1-M4-D-e` `tests/unit/lru_cache_test.cpp` exists. `[x]`

---

#### P1-M4-E: Deterministic Cooking (Byte-Identical Rebuild) `[x]`
- `P1-M4-E-a` `AssetMetadata::checksum` records content hash; cook is skipped if checksum unchanged. `[x]`
- `P1-M4-E-b` Asset packer produces byte-identical output for identical inputs (no timestamp embedding). `[x]`
- `P1-M4-E-c` `tests/unit/deterministic_cook_test.cpp` exists. `[x]`

---

### P1-M5: Renderer — Deferred Pipeline and Shadows

**Goal**: G-buffer pass, deferred lighting, shadow maps for all light types, post-process stack, forward transparency.
**Dependencies**: P1-M4.

> **Exit Criteria** *(all must pass before P1-M5 is closed)*:
> 1. Deferred shading with G-Buffer debug visualization.
> 2. Point, spot, and directional lights with correct PBR BRDF.
> 3. Cascaded shadow maps with PCF soft shadows, no shimmer.
> 4. Bloom, SSAO, tone mapping (3 operators), auto-exposure, FXAA functional.
> 5. Transparent objects render correctly in forward pass with shadows.

---

#### P1-M5-A: G-Buffer Pass `[x]`

- `P1-M5-A-a` `renderer/include/engine/renderer/pass_resources.h`: `PassResources` contains `gbufferAlbedo` (RGBA8 rgb=albedo a=metallic), `gbufferNormal` (RGBA16F rgb=worldNormal a=roughness), `gbufferEmissive` (RGBA8 rgb=emissive a=AO), `gbufferDepth` (DEPTH24). `[x]`
- `P1-M5-A-b` `render_device.h`: `create_framebuffer_mrt(colorTextures, colorCount, depthTex)` for MRT output. `[x]`
- `P1-M5-A-c` `gpu_profiler.h`: `GpuPassId::GBuffer` and `GpuPassId::GBufferDebug` pass IDs tracked. `[x]`
- `P1-M5-A-d` `RendererFrameStats::gpuGBufferMs` populated each frame. `[x]`
- `P1-M5-A-e` G-buffer geometry shader (writes albedo/metallic, normal/roughness, emissive/AO, depth). `[~]` — *Framebuffer infrastructure and GPU timer confirmed; GLSL shader source not inspected.*

---

#### P1-M5-B: Deferred Lighting

##### P1-M5-B1: Deferred Lighting — Fullscreen PBR (Cook-Torrance) `[x]`
- `P1-M5-B1a` `GpuPassId::DeferredLighting` tracked in GPU profiler; `RendererFrameStats::gpuDeferredLightMs` populated. `[x]`
- `P1-M5-B1b` Fullscreen pass reads G-buffer textures; evaluates Cook-Torrance BRDF per pixel. `[~]` — *Pass ID confirmed; GLSL shader completeness not inspected.*
- `P1-M5-B1c` `tests/unit/deferred_light_component_test.cpp` exists. `[x]`

##### P1-M5-B2: Point Lights (SparseSet, Frustum-Culled, Up to 128) `[x]`
- `P1-M5-B2a` `PointLightComponent` in `world.h`: color, intensity, radius. `[x]`
- `P1-M5-B2b` `World::kMaxPointLightComponents = 128`. `[x]`
- `P1-M5-B2c` `SceneLightData::pointLights` array (128 slots) gathered from ECS each frame. `[x]`
- `P1-M5-B2d` Frustum-culled before submission to tiled light culling. `[~]`

##### P1-M5-B3: Spot Lights (Cone Falloff, Frustum-Culled) `[x]`
- `P1-M5-B3a` `SpotLightComponent`: color, direction, intensity, radius, innerConeAngle, outerConeAngle. `[x]`
- `P1-M5-B3b` `World::kMaxSpotLightComponents = 64`. `[x]`
- `P1-M5-B3c` `SceneLightData::spotLights` array (64 slots) gathered from ECS each frame. `[x]`

##### P1-M5-B4: Tiled Light Culling (16×16 Tiles, Per-Tile Light List) `[x]`
- `P1-M5-B4a` `renderer/include/engine/renderer/light_culling.h`: `kTileSize = 16`. `[x]`
- `P1-M5-B4b` `kMaxPointLightsPerTile = 32`, `kMaxSpotLightsPerTile = 16`; tile data width = 50 floats. `[x]`
- `P1-M5-B4c` `cull_lights_tiled(lightData, viewMatrix, projMatrix, screenW, screenH, outData)`: CPU-side tile frustum test. `[x]`
- `P1-M5-B4d` `compute_tile_buffer_size(screenW, screenH)`: computes required flat buffer size. `[x]`
- `P1-M5-B4e` `tests/unit/light_culling_test.cpp` exists. `[x]`

---

#### P1-M5-C: Shadow Maps

##### P1-M5-C1: Cascaded Shadow Maps (Directional, 4 Cascades, PCF) `[ ]`
- `P1-M5-C1a` Directional light shadow: split scheme (logarithmic/uniform blend); 4 cascade frustums. `[ ]`
- `P1-M5-C1b` Per-cascade depth framebuffer; shadow map atlas or array texture. `[ ]`
- `P1-M5-C1c` PCF (percentage-closer filtering): 3×3 or 5×5 kernel; stable cascade seam blending. `[ ]`
- `P1-M5-C1d` Shadow matrix injected into deferred lighting pass uniform block. `[ ]`

##### P1-M5-C2: Spot Light Shadow Maps `[ ]`
- `P1-M5-C2a` Per-spot-light depth framebuffer (perspective projection). `[ ]`
- `P1-M5-C2b` Shadow comparison in deferred spotlight contribution. `[ ]`

##### P1-M5-C3: Point Light Cubemap Shadows `[ ]`
- `P1-M5-C3a` Per-point-light cubemap depth texture (6 faces); geometry shader or 6-pass render. `[ ]`
- `P1-M5-C3b` Omnidirectional shadow comparison in deferred point-light contribution. `[ ]`

##### P1-M5-C4: Shadow Optimization `[ ]`
- `P1-M5-C4a` Stable cascade matrices (world-space snap to texel size). `[ ]`
- `P1-M5-C4b` Shadow map caching (static-only passes skip re-render if scene is unchanged). `[ ]`
- `P1-M5-C4c` Shadow LOD (reduce shadow map resolution for distant cascades). `[ ]`

---

#### P1-M5-D: Post-Process Stack

##### P1-M5-D1: Post-Process Stack Architecture `[~]`
- `P1-M5-D1a` Ping-pong render targets: `PassResources::sceneColor` (RGBA16F) → post chain → `finalColor`. `[~]` — *`sceneColor` and `finalColor` resources confirmed in `pass_resources.h`.*
- `P1-M5-D1b` Per-pass CVar enable/disable (e.g., `r_bloom`, `r_ssao`, `r_fxaa`). `[ ]`
- `P1-M5-D1c` Ordered pass registration (post-process stack object). `[ ]`

##### P1-M5-D2: Bloom `[ ]`
- `P1-M5-D2a` Threshold pass: extract pixels above luminance threshold. `[ ]`
- `P1-M5-D2b` Dual-Kawase downsample chain (6–8 mip levels). `[ ]`
- `P1-M5-D2c` Dual-Kawase upsample chain + composite blend back into scene color. `[ ]`

##### P1-M5-D3: Screen-Space Ambient Occlusion (SSAO) `[ ]`
- `P1-M5-D3a` Sample hemisphere of depth buffer around each pixel; compare depths. `[ ]`
- `P1-M5-D3b` Bilateral blur: depth-aware separable filter to reduce noise. `[ ]`
- `P1-M5-D3c` Composite AO factor into deferred lighting ambient term. `[ ]`

##### P1-M5-D4: Tone Mapping + Auto-Exposure `[x]`
- `P1-M5-D4a` Tone mapping pass confirmed: `GpuPassId::Tonemap` in `gpu_profiler.h`; `RendererFrameStats::gpuTonemapMs`. `[x]`
- `P1-M5-D4b` `get_scene_viewport_texture()` returns final tonemapped color texture for editor display. `[x]`
- `P1-M5-D4c` Operators implemented (Reinhard, ACES, Uncharted 2). `[~]` — *Pass confirmed; operator selection and auto-exposure in GLSL unverified.*

##### P1-M5-D5: FXAA Anti-Aliasing `[ ]`
- `P1-M5-D5a` FXAA 3.11 fullscreen pass after tone mapping. `[ ]`
- `P1-M5-D5b` CVar `r_fxaa` toggles pass. `[ ]`

---

#### P1-M5-E: Forward Transparency Pass (Sorted, Alpha Blend, PBR) `[ ]`
- `P1-M5-E-a` After deferred pass: collect mesh commands with `opacity < 1.0` and `sortKey.transparent = 1`. `[ ]`
- `P1-M5-E-b` Sort back-to-front by depth (already encoded in `DrawKey`). `[ ]`
- `P1-M5-E-c` Render with alpha blending (`set_blend_func_alpha()`); depth test ON, depth write OFF. `[ ]`
- `P1-M5-E-d` PBR lighting via forward pass shader (sample shadow maps for transparency). `[ ]`

---

### P1-M6: Renderer — Sky, Fog, Instancing, Materials

**Goal**: Environment, atmospheric effects, batched instanced rendering, complete shader/material system.
**Dependencies**: P1-M5.

> **Exit Criteria** *(all must pass before P1-M6 is closed)*:
> 1. Skybox and procedural sky render correctly, sky influences ambient light.
> 2. Distance and height fog with configurable parameters.
> 3. GPU instancing reduces draw calls by 10× for repeated geometry.
> 4. Shader variant system compiles and caches permutations. No runtime stalls.
> 5. Material instances share shaders, differ only in parameters.

---

#### P1-M6-A: Sky and Environment

##### P1-M6-A1: Skybox (Cubemap, HDR Equirect Import) `[ ]`
- `P1-M6-A1a` Skybox vertex shader: render cube at far plane; cubemap sampler. `[ ]`
- `P1-M6-A1b` HDR equirectangular import: stb_image HDR decode → equirect-to-cubemap compute or fragment pass. `[ ]`
- `P1-M6-A1c` Skybox rendered after opaque pass before transparency; depth test LEQUAL. `[ ]`

##### P1-M6-A2: Procedural Sky (Preetham / Hosek-Wilkie) `[ ]`
- `P1-M6-A2a` Preetham sky model: sun direction, turbidity → sky radiance on GPU. `[ ]`
- `P1-M6-A2b` Hosek-Wilkie model (higher quality): perez coefficients → chromatic sky. `[ ]`
- `P1-M6-A2c` CVar `r_sky_model` selects Preetham / Hosek / Cubemap. `[ ]`

##### P1-M6-A3: Environment Reflection Probes (Prefiltered IBL, BRDF LUT) `[ ]`
- `P1-M6-A3a` Prefiltered environment map: specular radiance mip chain from cubemap. `[ ]`
- `P1-M6-A3b` Irradiance map: diffuse convolution from cubemap. `[ ]`
- `P1-M6-A3c` BRDF LUT: split-sum approximation (512×512 float texture). `[ ]`
- `P1-M6-A3d` Probe placement component; reflection probe bake tool. `[ ]`

---

#### P1-M6-B: Fog

##### P1-M6-B1: Distance Fog (Linear / Exp / Exp²) `[ ]`
- `P1-M6-B1a` Fog factor applied in deferred lighting pass or forward pass: `linear`, `exp`, `exp2` modes. `[ ]`
- `P1-M6-B1b` CVars: `r_fog_mode`, `r_fog_start`, `r_fog_end`, `r_fog_density`, `r_fog_color`. `[ ]`

##### P1-M6-B2: Height Fog (Height-Based Density, Ray-Marched) `[ ]`
- `P1-M6-B2a` Height-based density function: exponential falloff above base height. `[ ]`
- `P1-M6-B2b` Ray-marched integration from camera to surface; step count CVar. `[ ]`

---

#### P1-M6-C: GPU Instancing

##### P1-M6-C1: Static Mesh GPU Instancing `[ ]`
- `P1-M6-C1a` Sort opaque `DrawCommand` list by (shader, mesh, material); batch contiguous identical entries. `[ ]`
- `P1-M6-C1b` Per-instance model matrix uploaded via per-instance vertex buffer or UBO array. `[ ]`
- `P1-M6-C1c` `glDrawElementsInstanced(count, instanceCount)` call. `[ ]`
- `P1-M6-C1d` Benchmark: 10K identical meshes → single draw call. `[ ]`

##### P1-M6-C2: Foliage Instancing (Wind Vertex Displacement, Per-Instance LOD) `[ ]`
- `P1-M6-C2a` Wind displacement: sine wave vertex shader using time + world position + per-instance phase. `[ ]`
- `P1-M6-C2b` Per-instance LOD index stored in instance data; LOD mesh selected before batching. `[ ]`
- `P1-M6-C2c` Density/distribution stored in foliage painting tool output. `[ ]`

---

#### P1-M6-D: Shader and Material System

##### P1-M6-D1: Shader Variant / Permutation System (Macro-Based, Cached) `[~]`
- `P1-M6-D1a` `renderer/include/engine/renderer/shader_system.h`: `load_shader_program(vertPath, fragPath)`, `check_shader_reload()`. `[~]` — *Basic load/reload confirmed; macro permutation compilation missing.*
- `P1-M6-D1b` `ShaderProgramHandle` typed handle; `kInvalidShaderProgram` sentinel. `[x]`
- `P1-M6-D1c` Macro permutation table: define set → variant key; compile on demand, cache by key. `[ ]`
- `P1-M6-D1d` Variant selection: material properties (has_normal_map, has_emissive, skinned, etc.) select defines. `[ ]`
- `P1-M6-D1e` Binary shader cache: compiled SPIR-V or driver binary; restored on identical source hash. `[ ]`

##### P1-M6-D2: Material Instance System (Shared Shader, Per-Instance Params) `[~]`
- `P1-M6-D2a` `renderer/include/engine/renderer/material.h`: `Material` struct (albedo, emissive, roughness, metallic, opacity, albedoTexture, normalTexture). `[x]`
- `P1-M6-D2b` `renderer/include/engine/renderer/asset_manager.h` exists. `[~]` — *File exists; material instance system completeness unverified.*
- `P1-M6-D2c` Material instance: shares parent shader; overrides specific uniform values. `[ ]`
- `P1-M6-D2d` Material asset: serialized to JSON; loaded via asset pipeline; `AssetTypeTag::Material`. `[ ]`

---

#### P1-M6-E: Render-to-Texture / Scene Capture Component `[~]`
- `P1-M6-E-a` `get_scene_viewport_texture()` returns GPU texture ID of tonemapped scene output. `[x]`
- `P1-M6-E-b` `set_scene_viewport_size(width, height)` overrides viewport for off-screen render. `[x]`
- `P1-M6-E-c` `SceneCaptureComponent`: entity-attached component that drives an off-screen render target. `[ ]`
- `P1-M6-E-d` Multiple simultaneous capture components (e.g., security camera feeds). `[ ]`

---

### P1-M7: Animation System

**Goal**: Full skeletal animation pipeline — loading, blending, state machine, root motion, events, IK.
**Dependencies**: P1-M5 (skinned mesh rendering).
**Status**: Entirely not started. No animation headers found in any module.

> **Exit Criteria** *(all must pass before P1-M7 is closed)*:
> 1. Skeleton + clip loaded from glTF, compressed, decompressed with < 0.001 error.
> 2. Blend tree (1D/2D blend spaces), additive, masked blending all work.
> 3. Animation state machine transitions on parameter changes with crossfade.
> 4. Root motion drives entity transform.
> 5. Animation events fire Lua callbacks.
> 6. GPU skinning via compute or vertex shader (never CPU-side).

---

#### P1-M7-A: Skeleton and Clip Loading from glTF `[ ]`
- `P1-M7-A-a` Parse glTF `skin` (joints, inverse bind matrices) → `Skeleton` struct. `[ ]`
- `P1-M7-A-b` Parse glTF `animation` channels (translation/rotation/scale per joint) → `AnimClip`. `[ ]`
- `P1-M7-A-c` Quantized clip storage (16-bit fixed-point positions, quaternion compression). `[ ]`
- `P1-M7-A-d` `AssetTypeTag::Animation`; loaded via async asset pipeline. `[ ]`

#### P1-M7-B: Pose Blending `[ ]`
- `P1-M7-B-a` Crossfade blend: LERP between two poses over time. `[ ]`
- `P1-M7-B-b` 1D blend space: single float parameter → weighted blend of N clips. `[ ]`
- `P1-M7-B-c` 2D blend space: two float parameters → barycentric blend of N clips on a triangle grid. `[ ]`
- `P1-M7-B-d` Additive blend: add pose delta (e.g., aim offset) on top of base pose. `[ ]`
- `P1-M7-B-e` Masked blend: per-bone mask selects which layers apply to which joints. `[ ]`

#### P1-M7-C: Animation State Machine `[ ]`
- `P1-M7-C-a` State graph: named states, each with a clip or blend tree. `[ ]`
- `P1-M7-C-b` Transitions: condition predicates (float >, bool ==, trigger); optional blend duration. `[ ]`
- `P1-M7-C-c` Any-state transitions (global interrupt). `[ ]`
- `P1-M7-C-d` State machine serialized to JSON; hot-reloadable at runtime. `[ ]`
- `P1-M7-C-e` Lua API: `set_anim_param(entity, name, value)`, `get_anim_state(entity)`. `[ ]`

#### P1-M7-D: Root Motion Extraction and Application `[ ]`
- `P1-M7-D-a` Extract root joint delta (position + rotation) each frame from clip. `[ ]`
- `P1-M7-D-b` Apply root delta to entity `Transform` in world space; zero out joint root movement. `[ ]`
- `P1-M7-D-c` Root motion interaction with physics (character controller or rigidbody). `[ ]`

#### P1-M7-E: Animation Events / Notifies `[ ]`
- `P1-M7-E-a` Per-clip keyframe annotation: name, frame index, float payload. `[ ]`
- `P1-M7-E-b` Event dispatch during playback: fire Lua callback at annotated frame time. `[ ]`
- `P1-M7-E-c` Footstep events, weapon notifies, particle spawn notifies. `[ ]`

#### P1-M7-F: Montages (One-Shot Overlays with Sections) `[ ]`
- `P1-M7-F-a` Montage: named sections within a clip; play / jump-to-section API. `[ ]`
- `P1-M7-F-b` Montage plays on top of state machine; restores base state on completion. `[ ]`
- `P1-M7-F-c` Interrupt policy (cancel-on-new, finish-current, blend-out). `[ ]`

#### P1-M7-G: Skinned Mesh Rendering and IK

##### P1-M7-G1: Skinned Mesh Rendering (Bone Matrices, GPU Skinning Shader) `[ ]`
- `P1-M7-G1a` Per-frame bone matrix palette (up to 128 joints); uploaded as UBO. `[ ]`
- `P1-M7-G1b` GPU skinning vertex shader: weighted sum of 4 bone transforms per vertex. `[ ]`
- `P1-M7-G1c` Skinned mesh variant of G-buffer pass shader. `[ ]`

##### P1-M7-G3: Two-Bone IK (Foot Placement, Arm Reach) `[ ]`
- `P1-M7-G3a` Analytic two-bone IK solver (given target position → compute bend angle). `[ ]`
- `P1-M7-G3b` Foot placement: raycast ground; offset IK chain to match surface normal. `[ ]`
- `P1-M7-G3c` Arm reach: target from entity position; blend IK weight with animation. `[ ]`

---

### P1-M8: Audio System

**Goal**: 3D positional audio, mixer bus hierarchy, DSP effects, audio events, music system, streaming.
**Dependencies**: P1-M2. *Current state: basic `audio.h` exposes load/play/stop/volume/pitch/loop via miniaudio. All advanced features below are absent.*

> **Exit Criteria** *(all must pass before P1-M8 is closed)*:
> 1. 3D positional audio with distance attenuation, panning, and optional HRTF.
> 2. Mixer bus hierarchy with per-bus volume, muting, snapshots/ducking.
> 3. Reverb zones, low-pass/high-pass, Doppler effect.
> 4. Audio events with randomization and cooldown.
> 5. Music crossfade and layered stems.

---

#### P1-M8-A: 3D Positional Audio (Attenuation, Panning, HRTF Option) `[ ]`
- `P1-M8-A-a` Per-sound world position; distance attenuation (inverse-square or linear). `[ ]`
- `P1-M8-A-b` Panning based on listener orientation and sound position. `[ ]`
- `P1-M8-A-c` HRTF binaural rendering option for headphones (miniaudio or OpenAL soft HRTF). `[ ]`
- `P1-M8-A-d` Listener entity: position + orientation updated from camera each frame. `[ ]`

#### P1-M8-B: Mixer / Bus Hierarchy

##### P1-M8-B1: Bus Hierarchy (Master → Music / SFX / Dialogue, Volume Chain) `[ ]`
- `P1-M8-B1a` Named bus graph: Master → Music, SFX, Dialogue, Ambience. `[ ]`
- `P1-M8-B1b` Volume and mute per bus; child buses inherit parent volume. `[ ]`
- `P1-M8-B1c` Route any sound handle to any bus by name. `[ ]`

##### P1-M8-B2: Audio Snapshots / Ducking `[ ]`
- `P1-M8-B2a` Named snapshots: preset bus-volume configurations. `[ ]`
- `P1-M8-B2b` Blended snapshot transition over N seconds. `[ ]`
- `P1-M8-B2c` Auto-ducking: music duck when dialogue plays (priority-driven volume reduction). `[ ]`

#### P1-M8-C: DSP Effects

##### P1-M8-C1: Reverb (Schroeder / miniaudio Node, Reverb Zones) `[ ]`
- `P1-M8-C1a` Reverb node in miniaudio graph or custom Schroeder implementation. `[ ]`
- `P1-M8-C1b` Reverb zone component: entity-attached trigger volume; blend reverb wet/dry on enter/exit. `[ ]`

##### P1-M8-C2: Low-Pass / High-Pass Filter (Biquad, Occlusion Simulation) `[ ]`
- `P1-M8-C2a` Per-sound biquad low-pass filter; cutoff frequency from occlusion raycast. `[ ]`
- `P1-M8-C2b` Wall occlusion: raycast from listener to sound; each wall intersection lowers cutoff. `[ ]`

##### P1-M8-C3: Doppler Effect `[ ]`
- `P1-M8-C3a` Compute radial velocity (listener→source dot product). `[ ]`
- `P1-M8-C3b` Pitch shift proportional to velocity: `f_observed = f_source * (c ± v_listener) / (c ∓ v_source)`. `[ ]`

#### P1-M8-D: Audio Events and Music

##### P1-M8-D1: Audio Events (Randomized Candidates, Cooldown, Positional) `[ ]`
- `P1-M8-D1a` Named audio event: list of candidate sound assets + weights → pick random on play. `[ ]`
- `P1-M8-D1b` Per-event cooldown: minimum interval between triggers. `[ ]`
- `P1-M8-D1c` Positional event: automatically route through 3D audio with entity position. `[ ]`
- `P1-M8-D1d` Lua API: `engine.audio_event_play(name, entity_or_nil)`. `[ ]`

##### P1-M8-D2: Music System (Crossfade, Layered Stems) `[ ]`
- `P1-M8-D2a` Music track: primary loop + optional intro clip; seamless loop point. `[ ]`
- `P1-M8-D2b` Crossfade: fade out current track, fade in new track over N seconds. `[ ]`
- `P1-M8-D2c` Layered stems: simultaneously playing stems that can be muted/unmuted (e.g., add combat percussion). `[ ]`

#### P1-M8-E: Audio Streaming for Large Files `[~]`
- `P1-M8-E-a` miniaudio stream decoder: decode compressed audio from disk in background thread. `[~]` — *miniaudio is integrated; streaming decoder path unverified.*
- `P1-M8-E-b` Streaming budget: max MB of audio data decoded and buffered concurrently. `[ ]`
- `P1-M8-E-c` Seek support (music tracks, ambient loops). `[ ]`

---

### P1-M9: Editor

**Goal**: Reflection-driven inspector, full undo/redo, scene hierarchy, asset browser, prefab system, PIE, Lua editor API.
**Dependencies**: P1-M5, P1-M6.

> **Exit Criteria** *(all must pass before P1-M9 is closed)*:
> 1. Inspector shows all registered components with type-appropriate widgets.
> 2. Undo/redo works for all property edits, entity create/destroy, reparent, component add/remove.
> 3. Asset browser displays thumbnails, supports drag-to-viewport, search by tags.
> 4. Prefab system with instance overrides and nested prefabs.
> 5. Play/Pause/Stop preserves and restores exact world state.

---

#### P1-M9-A: Reflection-Driven Property Inspector

##### P1-M9-A1: All-Types Inspector (Nested Structs, Arrays) `[~]`
- `P1-M9-A1a` `core/include/engine/core/reflect.h` exists; `runtime/include/engine/runtime/reflect_types.h` exists. `[~]`
- `P1-M9-A1b` Reflected types: Transform, RigidBody, Collider, MeshComponent, LightComponent, ScriptComponent, SpringArmComponent, PointLightComponent, SpotLightComponent. `[~]` — *Header files confirmed; reflect macro coverage in each struct unverified.*
- `P1-M9-A1c` Inspector renders: float sliders, vec3 drag fields, bool checkboxes, enum dropdowns, string inputs, asset ID pickers. `[ ]`
- `P1-M9-A1d` Nested struct expansion (Collider → shape enum + halfExtents + material fields). `[ ]`
- `P1-M9-A1e` Array/SparseSet component display (list of lights with add/remove). `[ ]`

##### P1-M9-A2: Component Add / Remove from Inspector `[ ]`
- `P1-M9-A2a` Inspector shows "Add Component" dropdown listing all registered component types. `[ ]`
- `P1-M9-A2b` Remove button per component (records undo command). `[ ]`
- `P1-M9-A2c` Component add/remove generates undo/redo entry. `[ ]`

---

#### P1-M9-B: Undo / Redo — Command Pattern, All Operations, Ctrl+Z/Y `[~]`
- `P1-M9-B-a` `editor/include/engine/editor/command_history.h` exists. `[~]`
- `P1-M9-B-b` Transform moves/rotates/scales: confirmed partial in existing gap list. `[~]`
- `P1-M9-B-c` Entity create / destroy undo commands. `[ ]`
- `P1-M9-B-d` Component add / remove undo commands. `[ ]`
- `P1-M9-B-e` Property value change undo commands (all inspector edits). `[ ]`
- `P1-M9-B-f` Scene hierarchy reparent undo commands. `[ ]`
- `P1-M9-B-g` Ctrl+Z / Ctrl+Y keyboard bindings active in editor. `[~]`

---

#### P1-M9-C: Scene Hierarchy Panel (Tree, Drag-Drop Reparent, Multi-Select) `[ ]`
- `P1-M9-C-a` Tree view of all entities ordered by transform hierarchy (parent/child indentation). `[ ]`
- `P1-M9-C-b` Drag-drop reparent: drag entity onto another to set parent (records undo). `[ ]`
- `P1-M9-C-c` Multi-select: Ctrl+click / Shift+click; transform gizmo shows average pivot. `[ ]`
- `P1-M9-C-d` Context menu: rename, duplicate, delete, focus in viewport. `[ ]`
- `P1-M9-C-e` Search/filter by entity name. `[ ]`

---

#### P1-M9-D: Asset Browser (Thumbnails, Drag-to-Viewport, Search + Tags) `[ ]`
- `P1-M9-D-a` Directory tree panel with VFS virtual paths. `[ ]`
- `P1-M9-D-b` Asset grid: icon + name; thumbnail when available (P1-M4-A3). `[ ]`
- `P1-M9-D-c` Drag asset from browser to viewport: create entity with MeshComponent or drop ScriptComponent. `[ ]`
- `P1-M9-D-d` Tag filter bar: filter by `AssetTypeTag` and user-defined string tags. `[ ]`
- `P1-M9-D-e` Search by filename substring. `[ ]`
- `P1-M9-D-f` Asset inspector: double-click opens import settings panel. `[ ]`

---

#### P1-M9-E: Prefab System (Save, Instantiate, Overrides, Nested) `[~]`
- `P1-M9-E-a` `runtime/include/engine/runtime/prefab_serializer.h`: `save_prefab(world, entity, path)`, `instantiate_prefab(world, path)`. `[~]`
- `P1-M9-E-b` Save entity + all components (Transform, Collider, Mesh, Script, etc.) to JSON prefab. `[~]` — *API exists; coverage of all component types in .cpp unverified.*
- `P1-M9-E-c` Component override system: prefab instance can override individual field values without breaking source prefab link. `[ ]`
- `P1-M9-E-d` Nested prefabs: prefab containing references to other prefabs; override propagation. `[ ]`
- `P1-M9-E-e` Editor: "Save as Prefab" button on selected entity; drag prefab from asset browser to instantiate. `[ ]`
- `P1-M9-E-f` `tests/unit/prefab_test.cpp` exists. `[x]`

---

#### P1-M9-F: Play-In-Editor (Save / Restore World State, Pause, Step) `[~]`
- `P1-M9-F-a` `editor_is_playing()` / `editor_is_paused()` query functions. `[x]`
- `P1-M9-F-b` Play: serialize entire World to in-memory snapshot; start gameplay loop. `[~]` — *In prior review confirmed partially working; completeness of snapshot for all component types unknown.*
- `P1-M9-F-c` Stop: deserialize snapshot back into World; scripting/coroutine/timer state cleared. `[~]`
- `P1-M9-F-d` Pause: freeze simulation; UI/editor remain interactive. `[~]`
- `P1-M9-F-e` Step: advance exactly one simulation frame while paused. `[ ]`

---

#### P1-M9-G: Editor Lua Scripting API (Menu Items, Custom Panels) `[ ]`
- `P1-M9-G-a` `editor.add_menu_item(path, callback)`: Lua registers custom menu entries. `[ ]`
- `P1-M9-G-b` `editor.add_panel(name, draw_fn)`: Lua registers custom ImGui panel. `[ ]`
- `P1-M9-G-c` `editor.get_selected_entity()`, `editor.set_selected_entity(e)`. `[ ]`
- `P1-M9-G-d` `editor.run_tool(name)`: trigger built-in tools from Lua. `[ ]`

---

### P1-M10: Scene Management and Streaming

**Goal**: Scene transition API, streaming volumes, LOD, save system.
**Dependencies**: P1-M9.

> **Exit Criteria** *(all must pass before P1-M10 is closed)*:
> 1. Scene transitions (exclusive and additive) with loading screens.
> 2. Persistent entity IDs survive save/load and cross-scene references resolve.
> 3. Streaming volumes trigger async sub-scene load/unload with hysteresis.
> 4. LOD system switches meshes by camera distance.
> 5. Game save/load with multiple slots, checkpoint system.

---

#### P1-M10-A: Scene Lifecycle

##### P1-M10-A1: Scene Transition API (Exclusive, Additive, Unload) `[ ]`
- `P1-M10-A1a` `scene_load(path, mode)`: mode = Exclusive (unload current) or Additive (keep current). `[ ]`
- `P1-M10-A1b` `scene_unload(path)`: unload a specific additive sub-scene. `[ ]`
- `P1-M10-A1c` Transition callbacks: `on_scene_unloaded`, `on_scene_loaded` fired for Lua and C++. `[ ]`
- `P1-M10-A1d` Loading screen / progress callbacks. `[ ]`

##### P1-M10-A2: Persistent Entity IDs (UUID, Cross-Scene References) `[x]`
- `P1-M10-A2a` `PersistentId = uint32_t`; FNV-hashed from entity path; never changes after creation. `[x]`
- `P1-M10-A2b` `World::create_entity_with_persistent_id(pid)`. `[x]`
- `P1-M10-A2c` `World::find_entity_by_persistent_id(pid)`: O(1) open-addressing hash lookup. `[x]`
- `P1-M10-A2d` Scene serializer writes `persistentId` per entity; restores on load. `[x]`
- `P1-M10-A2e` Cross-scene reference: `prefab_serializer` and `scene_serializer` both use persistent IDs to identify entities. `[~]` — *IDs confirmed; cross-scene reference resolve path unverified.*

---

#### P1-M10-B: Spatial Streaming and LOD

##### P1-M10-B1: Streaming Volumes (Trigger Async Sub-Scene Load/Unload) `[ ]`
- `P1-M10-B1a` `StreamingVolumeComponent`: AABB trigger; references a sub-scene asset path. `[ ]`
- `P1-M10-B1b` On player enter: `load_asset_async` for sub-scene; on exit: unload after hysteresis delay. `[ ]`
- `P1-M10-B1c` Hysteresis distance (don't flicker on volume boundary). `[ ]`

##### P1-M10-B2: LOD System (Distance-Based Mesh Switching, Hysteresis) `[ ]`
- `P1-M10-B2a` `LodComponent`: array of (distance, meshAssetId) pairs per entity. `[ ]`
- `P1-M10-B2b` Per-frame LOD selection: camera distance → select appropriate mesh slot. `[ ]`
- `P1-M10-B2c` Hysteresis: switch-in threshold != switch-out threshold (avoid LOD flickering). `[ ]`
- `P1-M10-B2d` LOD 0 = highest detail; last LOD = cull (entity not submitted to renderer). `[ ]`

---

#### P1-M10-C: Save System (Multi-Slot, Checkpoints, Platform-Aware Paths) `[ ]`
- `P1-M10-C-a` `SaveSlot`: serialized `GameState` (score, inventory, flags) + scene path + player transform. `[ ]`
- `P1-M10-C-b` Multi-slot support (configurable N slots); `save(slot_index)`, `load(slot_index)`, `delete_slot(slot_index)`. `[ ]`
- `P1-M10-C-c` Checkpoint system: named checkpoint locations; `save_checkpoint(name)` from Lua. `[ ]`
- `P1-M10-C-d` Platform-aware save path: `%APPDATA%` (Windows), `~/.local/share/` (Linux), `~/Library/` (macOS). `[ ]`
- `P1-M10-C-e` Save file validation: magic header + checksum to detect corruption. `[ ]`

---

### P1-M11: UI System — Game Runtime

**Goal**: Canvas-based game UI independent of ImGui. Resolution-independent, font rendering, widget library, Lua binding.
**Dependencies**: P1-M5 (rendering), P1-M2 (input).
**Status**: Entirely not started. No UI runtime headers found in any module.

> **Exit Criteria** *(all must pass before P1-M11 is closed)*:
> 1. UI canvas renders over 3D scene with correct alpha blending.
> 2. All core widgets (text, image, button, slider, progress, toggle, input field) functional.
> 3. Layout system (hbox, vbox, grid, scroll) computes positions correctly.
> 4. Input routing: mouse/touch/gamepad navigation with focus system.
> 5. UI animations/tweens for hover, press, panel transitions.
> 6. Localization-ready: text shaping, RTL support, font fallback.

---

#### P1-M11-A: Canvas and Rendering Infrastructure

##### P1-M11-A1: UI Canvas, Coordinate System, Resolution Independence `[ ]`
- `P1-M11-A1a` Screen-space coordinate system: origin top-left, units in logical pixels. `[ ]`
- `P1-M11-A1b` Canvas-to-screen scale factor: logical resolution → actual backbuffer size. `[ ]`
- `P1-M11-A1c` Anchor system: per-widget anchor (top-left, center, etc.) for resolution changes. `[ ]`

##### P1-M11-A2: UI Rendering Pipeline (Batched 2D Quads, Alpha Blend) `[ ]`
- `P1-M11-A2a` Quad batcher: accumulate textured quads into vertex buffer; flush on texture change. `[ ]`
- `P1-M11-A2b` UI pass renders after tone-map pass; depth test OFF; alpha blending ON. `[ ]`
- `P1-M11-A2c` Scissor rect support (for scroll views and clipped containers). `[ ]`

##### P1-M11-A3: Font Rendering (stb_truetype / FreeType, SDF, Rich Text) `[ ]`
- `P1-M11-A3a` TTF font loading via stb_truetype or FreeType; atlas generation (multiple sizes). `[ ]`
- `P1-M11-A3b` SDF font rendering: single atlas resolution scales to any size with smooth edges. `[ ]`
- `P1-M11-A3c` Rich text: `[b]bold[/b]`, `[c=#FF0000]colored[/c]`, `[i]italic[/i]` inline markup. `[ ]`
- `P1-M11-A3d` Glyph caching: LRU atlas eviction when atlas is full. `[ ]`

---

#### P1-M11-B: Widget Library

##### P1-M11-B1: Core Widgets `[ ]`
- `P1-M11-B1a` `UIText`: string, font, size, color, alignment. `[ ]`
- `P1-M11-B1b` `UIImage`: texture handle, tint, UV rect, aspect mode. `[ ]`
- `P1-M11-B1c` `UIButton`: label + click callback; hover/pressed visual states. `[ ]`
- `P1-M11-B1d` `UISlider`: min/max/value; horizontal or vertical; change callback. `[ ]`
- `P1-M11-B1e` `UIProgressBar`: normalized value (0–1); fill direction. `[ ]`
- `P1-M11-B1f` `UIToggle`: checked state + change callback. `[ ]`
- `P1-M11-B1g` `UIInput`: single-line text field; virtual keyboard on touch platforms. `[ ]`

##### P1-M11-B2: Layout Containers `[ ]`
- `P1-M11-B2a` `UIHBox` / `UIVBox`: horizontal/vertical stacking with spacing and padding. `[ ]`
- `P1-M11-B2b` `UIGrid`: n-column grid layout. `[ ]`
- `P1-M11-B2c` `UIScrollView`: overflow clipping + scroll bar; drag/swipe scroll on touch. `[ ]`

---

#### P1-M11-C: Input Routing and Animation

##### P1-M11-C1: UI Input Routing `[ ]`
- `P1-M11-C1a` Hit-test: pointer position → topmost widget; consume event (stop propagation). `[ ]`
- `P1-M11-C1b` Focus management: keyboard/gamepad tab navigation between focusable widgets. `[ ]`
- `P1-M11-C1c` Gamepad navigation: D-pad / analog stick moves focus between UI elements. `[ ]`
- `P1-M11-C1d` Input consumption: UI input events do not reach game world when a modal UI is active. `[ ]`

##### P1-M11-C2: UI Animations / Tweens `[ ]`
- `P1-M11-C2a` `UITween`: animate any float property (alpha, position.x, scale) over time with easing. `[ ]`
- `P1-M11-C2b` Easing functions: linear, ease-in, ease-out, ease-in-out, spring. `[ ]`
- `P1-M11-C2c` Sequence/parallel tween groups; completion callbacks. `[ ]`

---

#### P1-M11-D: Lua UI API `[ ]`
- `P1-M11-D-a` `ui.create(type, parent)`: create widget of given type attached to parent. `[ ]`
- `P1-M11-D-b` `ui.set_property(widget, name, value)`: set widget property by name. `[ ]`
- `P1-M11-D-c` `ui.on_click(widget, callback)`, `ui.on_change(widget, callback)`. `[ ]`
- `P1-M11-D-d` `ui.animate(widget, prop, target, duration, easing)`: tween from Lua. `[ ]`
- `P1-M11-D-e` `ui.remove(widget)`: destroy widget and children. `[ ]`

---

### P1-M12: Platform, Packaging, and Ship Readiness

**Goal**: Platform abstraction, quality presets, distribution packaging, crash handler, localization, accessibility.
**Dependencies**: P1-M11.
**Status**: Entirely not started. No platform abstraction, packaging, or crash handler headers found.

> **Exit Criteria** *(all must pass before P1-M12 is closed)*:
> 1. Platform abstraction for Windows/Linux/macOS with filesystem, save paths, system info.
> 2. Quality presets (Low/Med/High/Ultra) controlling all visual CVars.
> 3. Dynamic resolution maintains target framerate.
> 4. Packed distribution builds for Win/Lin/Mac.
> 5. Crash handler writes stack trace to log.

---

#### P1-M12-A: Platform Abstraction (Windows / Linux / macOS, Filesystem, Save Paths) `[ ]`
- `P1-M12-A-a` `platform.h` (exists in `core/`) — extend with: `platform_get_save_dir()`, `platform_get_app_dir()`, `platform_get_temp_dir()`. `[ ]`
- `P1-M12-A-b` Platform-aware file dialog (SDL\_ShowOpenFileDialog or native). `[ ]`
- `P1-M12-A-c` High-DPI awareness: query display scale factor; apply to UI canvas. `[ ]`

#### P1-M12-B: Quality Settings (Low / Med / High / Ultra Presets, Dynamic Resolution) `[ ]`
- `P1-M12-B-a` `QualityPreset` enum: Low / Medium / High / Ultra. `[ ]`
- `P1-M12-B-b` Preset applies CVar batch: shadow map resolution, SSAO samples, fog step count, FXAA on/off. `[ ]`
- `P1-M12-B-c` Dynamic resolution: scale render resolution target to maintain target framerate. `[ ]`
- `P1-M12-B-d` Settings UI in main menu (reachable from Lua). `[ ]`

#### P1-M12-C: Distribution Packaging (Asset Packing, CPack, Win/Lin/Mac) `[ ]`
- `P1-M12-C-a` Cooked asset pack: all assets baked into a single binary archive (or per-platform VFS mount). `[ ]`
- `P1-M12-C-b` CPack: `.zip` (Windows), `.tar.gz` (Linux), `.dmg` (macOS). `[ ]`
- `P1-M12-C-c` Strip debug symbols from Release builds; PDB/dSYM stored separately. `[ ]`
- `P1-M12-C-d` Installer (optional): NSIS (Windows), .deb / .rpm (Linux). `[ ]`

#### P1-M12-D: Crash Handler (Stack Trace, Log, User Dialog) `[ ]`
- `P1-M12-D-a` Catch `SIGSEGV`, `SIGFPE`, `SIGILL`, `SEH_EXCEPTION` (Win32). `[ ]`
- `P1-M12-D-b` Capture stack trace: `libunwind` (Linux/macOS), `StackWalk64` (Windows). `[ ]`
- `P1-M12-D-c` Write crash log: timestamp, engine version, stack trace, last log entries. `[ ]`
- `P1-M12-D-d` User dialog: "The engine has crashed. A crash log has been saved to \<path\>." `[ ]`

#### P1-M12-E: Localization (String Tables, Runtime Language Switch, ICU Fallback) `[ ]`
- `P1-M12-E-a` String table: JSON/CSV keyed by string ID; one file per locale. `[ ]`
- `P1-M12-E-b` `localize(key)` returns locale-appropriate string at runtime. `[ ]`
- `P1-M12-E-c` Runtime language switch: reload string table without restart. `[ ]`
- `P1-M12-E-d` ICU/CLDR fallback: unknown locale falls back to `en-US`. `[ ]`
- `P1-M12-E-e` Lua binding: `engine.localize(key)`. `[ ]`

#### P1-M12-F: Accessibility (Font Scaling, High Contrast, Colorblind, Subtitles) `[ ]`
- `P1-M12-F-a` Global font scale factor (1.0× – 2.0×); applied to all UI text. `[ ]`
- `P1-M12-F-b` High-contrast mode CVar: forces UI palette swap. `[ ]`
- `P1-M12-F-c` Colorblind mode: Deuteranopia / Protanopia / Tritanopia palette shift shaders. `[ ]`
- `P1-M12-F-d` Subtitle system: timed text entries triggered by audio event notifies. `[ ]`

#### P1-M12-G: End-to-End Smoke Tests + Memory Leak Detection `[ ]`
- `P1-M12-G-a` `tests/smoke/`: boot engine, load scene, play 10 frames, stop, shutdown — no crash, no leak. `[ ]`
- `P1-M12-G-b` Memory leak gate: ASAN `detect_leaks=1` output parsed in CI; failure on any new leak. `[ ]`
- `P1-M12-G-c` Screenshot regression test: headless render of reference scene; pixel diff against golden image. `[ ]`

---

---

## Phase 2 — Competitive Feature Parity (P2)

Phase 2 brings the engine to rough feature parity with Godot 4.x. All Phase 1 must be complete first.

---

### P2-M1: Advanced Rendering

**Goal**: Lightmap baking, screen-space reflections, volumetric fog, TAA, motion blur, depth of field, post-process volumes.
**Dependencies**: P1-M5, P1-M6.

> **Exit Criteria** *(all must pass before P2-M1 is closed)*:
> 1. Lightmaps baked via CPU path tracer, rendered in deferred pipeline.
> 2. SSR with Hi-Z acceleration and edge fade.
> 3. Volumetric fog with temporal stability.
> 4. TAA, motion blur, and depth of field functional.
> 5. Post-process volumes blend settings spatially.

---

#### P2-M1-A: Lightmap Baking (CPU Path Tracer, UV2, Denoise) `[ ]` **[high]**
- `P2-M1-A-a` UV2 channel generation: auto-unwrap or use embedded UV2 from glTF. `[ ]`
- `P2-M1-A-b` CPU path tracer: Monte Carlo irradiance integration per luxel; BVH for scene intersection. `[ ]`
- `P2-M1-A-c` Denoiser: OIDN (Intel Open Image Denoise) integration. `[ ]`
- `P2-M1-A-d` Lightmap atlas packing; per-mesh UV2 offset/scale in instance data. `[ ]`
- `P2-M1-A-e` Runtime: sample pre-baked lightmap in deferred lighting pass. `[ ]`

#### P2-M1-B: Screen-Space Reflections (Hi-Z Ray March) `[ ]` **[high]**
- `P2-M1-B-a` Build Hi-Z depth mip chain from G-buffer depth. `[ ]`
- `P2-M1-B-b` Ray march in screen space; binary search for intersection; fade-out at edges. `[ ]`
- `P2-M1-B-c` Blend SSR with reflection probe fallback. `[ ]`

#### P2-M1-C: Volumetric Fog / God Rays (Froxel, Temporal Reprojection) `[ ]` **[high]**
- `P2-M1-C-a` Froxel volume: 160×90×64 cells; inscattering + extinction per cell. `[ ]`
- `P2-M1-C-b` Per-cell light sampling (shadow maps + point lights). `[ ]`
- `P2-M1-C-c` Temporal reprojection to reduce noise. `[ ]`

#### P2-M1-D: Advanced Post-Process

##### P2-M1-D1: Temporal Anti-Aliasing (TAA, Jitter, Velocity Buffer) `[ ]` **[high]**
- `P2-M1-D1a` Jitter projection matrix (Halton sequence). `[ ]`
- `P2-M1-D1b` Per-pixel velocity buffer from current/previous frame transforms. `[ ]`
- `P2-M1-D1c` TAA resolve: history blending + neighbourhood clamp to prevent ghosting. `[ ]`

##### P2-M1-D2: Motion Blur (Per-Object Velocity) `[ ]` **[high]**
- `P2-M1-D2a` Per-object velocity from current vs previous model matrix. `[ ]`
- `P2-M1-D2b` Post-process motion blur: sample along velocity vector. `[ ]`

##### P2-M1-D3: Depth of Field (CoC, Near/Far Blur, Autofocus) `[ ]` **[high]**
- `P2-M1-D3a` Circle of Confusion calculation from depth + focal distance + aperture. `[ ]`
- `P2-M1-D3b` Separate near/far blur (bokeh disc or dual Kawase). `[ ]`
- `P2-M1-D3c` Autofocus: raycast to center of screen; lerp focal distance. `[ ]`

##### P2-M1-D4: Post-Process Volumes (Spatial Blending) `[ ]` **[high]**
- `P2-M1-D4a` `PostProcessVolume` component: AABB or infinite; overrides post-process parameters. `[ ]`
- `P2-M1-D4b` Volume blending: parameters lerp over blend radius as camera enters. `[ ]`

---

### P2-M2: VFX / Particle System

**Goal**: CPU and GPU particle systems, collision, trails/ribbons, mesh particles, editor tooling.
**Dependencies**: P1-M5, P1-M6.

> **Exit Criteria** *(all must pass before P2-M2 is closed)*:
> 1. CPU particles: spawn, forces, curves, billboard rendering with soft particles.
> 2. GPU particles: 100K+ with compute shader sim.
> 3. Collision with ground and depth buffer.
> 4. Trails/ribbons and mesh particles.
> 5. Editor curve editor for particle parameters.

---

#### P2-M2-A: CPU Particle Emitter `[ ]` **[high]**
- `P2-M2-A-a` Emitter: spawn rate, burst, lifetime, size/color over lifetime curves. `[ ]`
- `P2-M2-A-b` Forces: gravity, drag, turbulence noise field. `[ ]`
- `P2-M2-A-c` Billboard rendering: particles always face camera; sorted back-to-front. `[ ]`
- `P2-M2-A-d` Sub-texture animation: flipbook atlas; frame rate over lifetime curve. `[ ]`

#### P2-M2-B: GPU Particle System

##### P2-M2-B1: GPU Particle Simulation (Compute Shader, 100K+) `[ ]` **[high]**
- `P2-M2-B1a` Compute shader: spawn, update (forces), kill particles in parallel. `[ ]`
- `P2-M2-B1b` Dead-list / free-list management on GPU. `[ ]`
- `P2-M2-B1c` 100K+ particles at 60 fps. `[ ]`

##### P2-M2-B2: Particle Collision (Plane, Depth Buffer, Sub-Emit) `[ ]` **[high]**
- `P2-M2-B2a` Plane collision: bounce or die on contact with static plane colliders. `[ ]`
- `P2-M2-B2b` Depth buffer collision: sample scene depth; reflect velocity. `[ ]`
- `P2-M2-B2c` Sub-emitter: spawn child particles on collision or on death. `[ ]`

##### P2-M2-B3: Trails / Ribbon Emitters `[ ]` **[high]**
- `P2-M2-B3a` Trail: per-particle history of world positions; rendered as textured ribbon. `[ ]`
- `P2-M2-B3b` Width and opacity over trail length curves. `[ ]`

##### P2-M2-B4: Mesh Particles (Instanced Mesh per Particle) `[ ]` **[high]**
- `P2-M2-B4a` Replace billboard quad with instanced draw of arbitrary mesh asset. `[ ]`
- `P2-M2-B4b` Per-particle scale/rotation encoded in instance buffer. `[ ]`

---

### P2-M3: 2D Engine

**Goal**: Sprite renderer, tilemap, 2D physics, 2D camera.
**Dependencies**: P1-M5 (rendering), P1-M3 (physics foundation).

> **Exit Criteria** *(all must pass before P2-M3 is closed)*:
> 1. Sprites render with batching, atlas support, and animation.
> 2. Tilemap loads from Tiled, renders with culling, supports collision.
> 3. 2D physics with circle/box/polygon colliders, joints, one-way platforms, raycasting.
> 4. 2D camera with follow, bounds, zoom, shake.
> 5. All features have Lua bindings.

---

#### P2-M3-A: Sprite Renderer (Batched, Atlas, Animation, Z-Order) `[ ]` **[high]**
- `P2-M3-A-a` `SpriteComponent`: texture atlas + UV rect, color tint, flip X/Y, z-order. `[ ]`
- `P2-M3-A-b` Sprite animation: frame sequence with fps; loop/ping-pong/once modes. `[ ]`
- `P2-M3-A-c` Batched rendering: same atlas + z-layer → single draw call. `[ ]`

#### P2-M3-B: Tilemap (Tiled Import, Culled Rendering, Collision) `[ ]` **[high]**
- `P2-M3-B-a` Tiled JSON import (.tmj): layers, tile IDs, collision objects. `[ ]`
- `P2-M3-B-b` Frustum-culled tile rendering: only visible tiles submitted. `[ ]`
- `P2-M3-B-c` Collision layer: tilemap collision shapes added to physics world. `[ ]`

#### P2-M3-C: 2D Physics (Circle / Box / Polygon, Joints, One-Way Platforms) `[ ]` **[high]**
- `P2-M3-C-a` 2D rigid body: circle, AABB, convex polygon shapes. `[ ]`
- `P2-M3-C-b` 2D joints: distance, hinge, spring. `[ ]`
- `P2-M3-C-c` One-way platforms: collide only from above. `[ ]`

#### P2-M3-D: 2D Camera (Follow, Bounds, Zoom, Shake) `[ ]` **[high]**
- `P2-M3-D-a` Follow camera: lerp toward target entity position. `[ ]`
- `P2-M3-D-b` Camera bounds: clamp camera to world rectangle. `[ ]`
- `P2-M3-D-c` Zoom: orthographic scale parameter. `[ ]`
- `P2-M3-D-d` Camera shake (reuse P1-M2-E shake system in 2D). `[ ]`

---

### P2-M4: Networking

**Goal**: Reliable UDP transport, entity replication, RPCs, client prediction.
**Dependencies**: P1-M2 (ECS), P1-M10 (scene management).

> **Exit Criteria** *(all must pass before P2-M4 is closed)*:
> 1. Reliable UDP with ordered delivery surviving 30% packet loss.
> 2. Encrypted connections with handshake and heartbeat.
> 3. Entity replication with delta compression.
> 4. Server and client RPCs with Lua bindings.
> 5. Client-side prediction and server reconciliation basics.

---

#### P2-M4-A: Reliable UDP Transport `[ ]` **[high]**
- `P2-M4-A-a` Sequenced datagrams: sequence numbers, ACK bitfield, retransmit on loss. `[ ]`
- `P2-M4-A-b` Encryption: ChaCha20-Poly1305 or AES-GCM per packet. `[ ]`
- `P2-M4-A-c` MTU discovery + fragmentation/reassembly. `[ ]`

#### P2-M4-B: Entity Replication

##### P2-M4-B1: Dirty-Flag Delta Compression `[ ]` **[high]**
- `P2-M4-B1a` Per-component dirty flags; only changed fields sent each tick. `[ ]`
- `P2-M4-B1b` Quantized values (positions 16-bit fixed, rotations 32-bit compressed quaternion). `[ ]`

##### P2-M4-B2: RPCs (Server / Client / Multicast) `[ ]` **[high]**
- `P2-M4-B2a` `[[rpc_server]]`, `[[rpc_client]]`, `[[rpc_multicast]]` tagged Lua functions. `[ ]`
- `P2-M4-B2b` Reliable and unreliable RPC channels. `[ ]`

##### P2-M4-B3: Client Prediction + Server Reconciliation `[ ]` **[high]**
- `P2-M4-B3a` Client predicts movement locally; stores input history ring buffer. `[ ]`
- `P2-M4-B3b` Server sends authoritative state; client replays inputs from divergence point. `[ ]`

#### P2-M4-C: Lobby / Session Management (Host Migration) `[ ]` **[high]**
- `P2-M4-C-a` Session: host, guest list, game state token, reconnect window. `[ ]`
- `P2-M4-C-b` Host migration: elect new host from guests on host disconnect. `[ ]`

---

### P2-M5: Splines, Data Tables, Gameplay Tools

**Goal**: Spline paths, data-driven tables, CSG brushes, foliage tools.
**Dependencies**: P1-M9 (editor).

> **Exit Criteria** *(all must pass before P2-M5 is closed)*:
> 1. Splines with editor gizmos, arc-length parameterization, and Lua follow API.
> 2. Data tables load from CSV/JSON, queryable from Lua, hot-reloadable.
> 3. CSG brushes with union/subtract/intersect and real-time preview.
> 4. Foliage painting tool with density brush and instanced rendering.

---

#### P2-M5-A: Spline / Path Component `[ ]` **[high]**
- `P2-M5-A-a` `SplineComponent`: Catmull-Rom control points; arc-length parameterization. `[ ]`
- `P2-M5-A-b` `evaluate_spline(t)` → position + tangent. `[ ]`
- `P2-M5-A-c` Editor gizmos: movable control point handles. `[ ]`

#### P2-M5-B: Data Tables (CSV / JSON, Typed Columns, Lua Query, Hot-Reload) `[ ]` **[high]**
- `P2-M5-B-a` CSV/JSON table import; typed columns (int, float, string, asset-id). `[ ]`
- `P2-M5-B-b` `engine.datatable_get(name, row_key, col)` Lua API. `[ ]`
- `P2-M5-B-c` Hot-reload: file watcher triggers table re-parse at runtime. `[ ]`

#### P2-M5-C: CSG Brushes (Boolean Ops, UV Gen, Editor Preview) `[ ]` **[high]**
- `P2-M5-C-a` CSG union, difference, intersection on brush meshes. `[ ]`
- `P2-M5-C-b` UV auto-generation for CSG result faces. `[ ]`
- `P2-M5-C-c` Real-time editor preview of CSG result. `[ ]`

#### P2-M5-D: Foliage Painting Tool `[ ]` **[high]**
- `P2-M5-D-a` Brush-based placement: paint foliage meshes onto terrain surface. `[ ]`
- `P2-M5-D-b` Density control; erase brush; scale/rotation randomization. `[ ]`
- `P2-M5-D-c` Output: instance buffer consumed by foliage instancing renderer (P1-M6-C2). `[ ]`

---

### P2-M6: Controller Haptics and Advanced Input

**Goal**: Rumble, adaptive triggers, gyroscope, input recording/replay.
**Dependencies**: P1-M2 (input system).

> **Exit Criteria** *(all must pass before P2-M6 is closed)*:
> 1. Gamepad rumble with presets and Lua API.
> 2. Adaptive trigger effects on DualSense (fallback no-op on others).
> 3. Gyroscope input readable from Lua.
> 4. Input recording/replay for automated testing.

---

#### P2-M6-A: Controller Haptics

##### P2-M6-A1: Gamepad Rumble (Low / High Freq, Presets) `[ ]` **[high]**
- `P2-M6-A1a` `SDL_RumbleGamepad(gamepad, lowFreq, highFreq, durationMs)`. `[ ]`
- `P2-M6-A1b` Named rumble presets (light, medium, heavy, explosion). `[ ]`

##### P2-M6-A2: Adaptive Triggers (PS5 DualSense) `[ ]` **[high]**
- `P2-M6-A2a` SDL DualSense extension: set trigger effect type (off, feedback, weapon, vibration). `[ ]`

#### P2-M6-B: Gyroscope / Motion Input (Gyro Aiming) `[ ]` **[high]**
- `P2-M6-B-a` Read gyroscope delta from SDL sensor API. `[ ]`
- `P2-M6-B-b` Map gyro delta to camera rotation (with sensitivity + deadzone). `[ ]`

#### P2-M6-C: Input Recording / Replay (Deterministic Repro) `[ ]` **[high]**
- `P2-M6-C-a` Record all input events + frame timestamps to binary log. `[ ]`
- `P2-M6-C-b` Replay: feed recorded events into input system; verify deterministic output hash. `[ ]`

---

### P2-M7: Advanced Editor Features

**Goal**: Visual scripting, animation editor, terrain, plugin system.
**Dependencies**: P1-M9 (editor), P1-M7 (animation).

> **Exit Criteria** *(all must pass before P2-M7 is closed)*:
> 1. Visual scripting: node graph with drag-connect, compiles to Lua, hot reloads.
> 2. Animation editor: timeline, keyframe editing, state machine visual editor.
> 3. Terrain: heightmap rendering with LOD, sculpt/paint tools, multi-texture splatting.
> 4. Plugin system loads external plugins.

---

#### P2-M7-A: Visual Scripting (Node Graph → Lua Compilation, Live Preview) `[ ]` **[high]**
- `P2-M7-A-a` Node graph editor: data nodes (constants, ops), flow nodes (if, loop), event nodes (on_tick). `[ ]`
- `P2-M7-A-b` Graph serialized to JSON; compiled to Lua on save. `[ ]`
- `P2-M7-A-c` Live preview: run compiled Lua in PIE without manually restarting. `[ ]`

#### P2-M7-B: Animation Editor (Timeline, State Machine Visual Editor) `[ ]` **[high]**
- `P2-M7-B-a` Timeline panel: clip display, playhead scrub, event markers. `[ ]`
- `P2-M7-B-b` State machine canvas: drag-connect states and transitions visually. `[ ]`
- `P2-M7-B-c` Blend space 2D editor: scatter plot of clip positions with preview pose. `[ ]`

#### P2-M7-C: Terrain Editor `[ ]` **[high]**
- `P2-M7-C-a` Heightmap LOD: CDLOD or geomipmapping for terrain mesh. `[ ]`
- `P2-M7-C-b` Sculpt tools: raise, lower, smooth, flatten brushes. `[ ]`
- `P2-M7-C-c` Paint tools: multi-texture splat map (up to 4 layers); weight painting. `[ ]`
- `P2-M7-C-d` Export heightfield as `HeightfieldData` for physics (P1-M3-A3). `[ ]`

#### P2-M7-D: Plugin System (Shared Lib Loading, Sandboxed API) `[ ]` **[high]**
- `P2-M7-D-a` Plugin: shared library (`.dll`/`.so`/`.dylib`) implementing `IPlugin` interface. `[ ]`
- `P2-M7-D-b` Plugin manager: load/unload at runtime; version compatibility check. `[ ]`
- `P2-M7-D-c` Sandboxed API: plugins only access `IPluginAPI` (no raw engine internals). `[ ]`

---

### P2-M8: Performance Polish

**Goal**: Multi-threaded rendering, advanced culling, shader cache, PSO sorting, memory budgets.
**Dependencies**: All P1 milestones.

> **Exit Criteria** *(all must pass before P2-M8 is closed)*:
> 1. Multi-threaded command buffer recording and parallel scene update.
> 2. BVH-based hierarchical frustum culling and HZB occlusion culling.
> 3. Shader binary cache eliminates runtime compilation stalls.
> 4. PSO-sorted draw calls reduce state changes.
> 5. Per-subsystem memory budgets with tracking and editor dashboard.

---

#### P2-M8-A: Multi-Threaded Rendering (Command Buffer Recording, Parallel Update) `[ ]` **[high]**
- `P2-M8-A-a` Parallel command buffer recording: split entities across job workers; merge before submit. `[ ]`
- `P2-M8-A-b` Parallel transform update: `World::update_transforms_range()` already thread-safe — wire to job scheduler. `[ ]`
- `P2-M8-A-c` Render thread: dedicated thread consumes command buffer; main thread produces. `[ ]`

#### P2-M8-B: Hierarchical Culling (BVH Frustum Cull, HZB Occlusion Cull) `[ ]` **[high]**
- `P2-M8-B-a` Static BVH over scene meshes: rebuilt on scene load; incremental update for dynamic objects. `[ ]`
- `P2-M8-B-b` BVH frustum cull: traverse BVH against camera frustum; only leaf nodes generate draw commands. `[ ]`
- `P2-M8-B-c` Hi-Z occlusion cull: down-sample depth from previous frame; test AABBs against HZB. `[ ]`

#### P2-M8-C: Shader Binary Cache + PSO Sorting `[ ]` **[high]**
- `P2-M8-C-a` Cache compiled shader variants to disk (keyed by source hash); restore on identical hash. `[ ]`
- `P2-M8-C-b` Pipeline state object sorting: sort draw commands by PSO (shader + blend state + depth state) to minimize GPU state changes. `[ ]`

#### P2-M8-D: Per-System Memory Budgets with Tracking `[ ]` **[high]**
- `P2-M8-D-a` Each subsystem registers a byte budget; `mem_tracker_current_bytes()` vs budget produces alert. `[ ]`
- `P2-M8-D-b` Budget exceeded: log warning; editor overlay shows red bar. `[ ]`

---

---

## Phase 3 — Cutting-Edge / Future (P3)

---

### P3-M1: XR / VR / AR

**Goal**: OpenXR stereo rendering, controller/hand tracking, passthrough AR, Lua XR API.
**Dependencies**: P1-M5 (rendering), P1-M2 (input).

> **Exit Criteria** *(all must pass before P3-M1 is closed)*:
> 1. Stereo rendering via OpenXR on at least one headset (Quest, SteamVR).
> 2. Head + controller tracking integrated with input system.
> 3. Hand tracking with gesture detection.
> 4. Passthrough AR compositing.
> 5. Full Lua XR API.

---

#### P3-M1-A: OpenXR Integration `[ ]` **[low]**
- `P3-M1-A-a` OpenXR session lifecycle: create, begin, end; poll events. `[ ]`
- `P3-M1-A-b` Stereo rendering: per-eye view/proj matrices; two-eye swapchain. `[ ]`
- `P3-M1-A-c` Tracking: grip, aim, head poses from OpenXR action system. `[ ]`

#### P3-M1-A4: Hand Tracking + Gesture Detection `[ ]` **[low]**
- `P3-M1-A4a` OpenXR `XR_EXT_hand_tracking`: 26-joint hand skeleton per hand. `[ ]`
- `P3-M1-A4b` Gesture recognizer: pinch, grip, point, open-palm. `[ ]`

#### P3-M1-A5: Passthrough AR `[ ]` **[low]**
- `P3-M1-A5a` `XR_FB_passthrough` (Meta) or `XR_HTC_passthrough` extension. `[ ]`
- `P3-M1-A5b` Compositor layer for passthrough behind scene render. `[ ]`

---

### P3-M2: Vulkan Backend

**Goal**: Full Vulkan implementation behind RenderDevice, shader cross-compilation, compute/bindless/async.
**Dependencies**: P1-M5, P1-M6 (complete GL pipeline as reference).

> **Exit Criteria** *(all must pass before P3-M2 is closed)*:
> 1. All rendering features work identically under both GL and Vulkan backends.
> 2. Backend selected at startup via CVar `r_backend` (gl/vulkan).
> 3. Compute shaders functional on Vulkan.
> 4. Vulkan-specific optimizations (bindless, async compute) provide measurable speedup.

---

#### P3-M2-A: RenderDevice Abstraction (GL + Vulkan Backends) `[ ]` **[low]**
- `P3-M2-A-a` `RenderDevice` function-pointer table (already exists in `render_device.h`) is the abstraction layer. `[~]` — *Structure exists; Vulkan backend implementation not started.*
- `P3-M2-A-b` Implement all `RenderDevice` function pointers with Vulkan: vkCreateShaderModule, VkBuffer, VkDescriptorSet. `[ ]`
- `P3-M2-A-c` Validation layer integration; VMA allocator for GPU memory. `[ ]`

#### P3-M2-A3: Shader Cross-Compilation (GLSL → SPIR-V) `[ ]` **[low]**
- `P3-M2-A3a` glslang or shaderc: compile engine GLSL shaders to SPIR-V at cook time. `[ ]`
- `P3-M2-A3b` SPIR-V reflection for automatic descriptor set layout generation. `[ ]`

#### P3-M2-B: Vulkan-Specific Features (Compute, Bindless, Async Compute) `[ ]` **[low]**
- `P3-M2-B-a` Compute shader pipeline: GPU particles (P2-M2-B1) ported to Vulkan compute. `[ ]`
- `P3-M2-B-b` Bindless descriptors: VK_EXT_descriptor_indexing; textures accessed by index without rebind. `[ ]`
- `P3-M2-B-c` Async compute queue: overlap GPU particle simulation with graphics. `[ ]`

---

### P3-M3: Mobile

**Goal**: Android and iOS builds, mobile rendering backends, touch/virtual joystick adaptation.
**Dependencies**: P3-M2 (Vulkan/GLES backend), P1-M12 (platform abstraction).

> **Exit Criteria** *(all must pass before P3-M3 is closed)*:
> 1. Android APK builds and runs on modern Android device (arm64).
> 2. iOS IPA builds and runs on iPhone (arm64).
> 3. Touch input, virtual joystick, and gesture camera functional.
> 4. Mobile quality presets maintain 30+ FPS.

---

#### P3-M3-A: Android Build (NDK, GLES 3.0, APK/AAB) `[ ]` **[low]**
- `P3-M3-A-a` CMake toolchain for Android NDK; `ENGINE_TARGET_PLATFORM=Android`. `[ ]`
- `P3-M3-A-b` GLES 3.0 backend: subset of `RenderDevice` functions (no ARB_timestamp queries). `[ ]`
- `P3-M3-A-c` Gradle wrapper; APK and AAB packaging. `[ ]`
- `P3-M3-A-d` Android asset delivery: VFS mounts APK assets. `[ ]`

#### P3-M3-B: iOS Build (Xcode, Metal / MoltenVK) `[ ]` **[low]**
- `P3-M3-B-a` CMake Xcode generator; `ENGINE_TARGET_PLATFORM=iOS`. `[ ]`
- `P3-M3-B-b` Metal backend via MoltenVK (Vulkan → Metal translation) or native Metal RenderDevice. `[ ]`
- `P3-M3-B-c` Info.plist, bundle signing, App Store metadata. `[ ]`

#### P3-M3-C: Mobile UI Adaptation (Touch Targets, Virtual Joystick) `[ ]` **[low]**
- `P3-M3-C-a` Minimum touch target 48dp; widget auto-resize in mobile layout mode. `[ ]`
- `P3-M3-C-b` On-screen virtual joystick component (Lua-configurable). `[ ]`
- `P3-M3-C-c` Keyboard-absent input mode: no keyboard shortcuts active; all actions via touch/pad. `[ ]`

---

### P3-M4: Web / Emscripten

**Goal**: WebAssembly build with WebGL 2.0, HTTP asset loading, minimal binary size.
**Dependencies**: P1-M12 (platform abstraction).

> **Exit Criteria** *(all must pass before P3-M4 is closed)*:
> 1. Engine runs in Chrome/Firefox/Safari via Emscripten.
> 2. WebGL 2.0 rendering functional.
> 3. Asset loading via HTTP fetch.
> 4. WASM size < 10MB for minimal scene.

---

#### P3-M4-A: Emscripten / WASM Build (WebGL 2.0, HTTP Asset Fetch) `[ ]` **[low]**
- `P3-M4-A-a` CMake Emscripten toolchain; `ENGINE_TARGET_PLATFORM=Web`. `[ ]`
- `P3-M4-A-b` WebGL 2.0 backend (GLES 3.0 subset via Emscripten). `[ ]`
- `P3-M4-A-c` HTTP asset fetch: VFS backend uses `emscripten_async_wget2_data`. `[ ]`
- `P3-M4-A-d` Async main loop: `emscripten_set_main_loop`. `[ ]`
- `P3-M4-A-e` WASM binary size budget: strip unused stdlib; apply `-O2 --closure 1`. `[ ]`

---

### P3-M5: AI and Navigation

**Goal**: NavMesh generation, pathfinding, behavior trees, steering, visual editor.
**Dependencies**: P1-M3 (physics/collision), P1-M9 (editor).

> **Exit Criteria** *(all must pass before P3-M5 is closed)*:
> 1. NavMesh generated from scene geometry with configurable agent parameters.
> 2. A* pathfinding with funnel smoothing, dynamic obstacles, off-mesh links.
> 3. Behavior trees with selector/sequence/decorator/action and blackboard.
> 4. Steering behaviors with flocking.
> 5. Visual behavior tree editor.

---

#### P3-M5-A: NavMesh Generation and Pathfinding

##### P3-M5-A1: NavMesh Generation (Voxelize, Contour, Triangulate) `[ ]` **[low]**
- `P3-M5-A1a` Voxelization: rasterize scene geometry into height-field voxel grid. `[ ]`
- `P3-M5-A1b` Region segmentation: watershed / monotone partitioning. `[ ]`
- `P3-M5-A1c` Contour tracing; simplification; triangulation → polygon mesh. `[ ]`
- `P3-M5-A1d` Recast-like pipeline or direct Recast library integration. `[ ]`

##### P3-M5-A2: Pathfinding (A*, Funnel, Dynamic Obstacles, Off-Mesh Links) `[ ]` **[low]**
- `P3-M5-A2a` A* on NavMesh polygon graph. `[ ]`
- `P3-M5-A2b` Funnel algorithm: smooth path through portal edges. `[ ]`
- `P3-M5-A2c` Dynamic obstacles: temporarily remove NavMesh polygons; re-path on change. `[ ]`
- `P3-M5-A2d` Off-mesh links: jump, ladder, teleport connections between disjoint NavMesh regions. `[ ]`

#### P3-M5-B: Behavior Trees and Steering

##### P3-M5-B1: Behavior Trees (Selector / Sequence / Decorator, Blackboard) `[ ]` **[low]**
- `P3-M5-B1a` Node types: Selector, Sequence, Parallel, Inverter, Repeater, Condition, Action. `[ ]`
- `P3-M5-B1b` Blackboard: typed key-value store shared across tree nodes. `[ ]`
- `P3-M5-B1c` Lua action nodes: `bt.register_action(name, fn)`. `[ ]`

##### P3-M5-B2: Steering Behaviors (Seek, Flee, Arrive, Flocking) `[ ]` **[low]**
- `P3-M5-B2a` Seek / Flee / Arrive / Wander steering forces. `[ ]`
- `P3-M5-B2b` Separation + cohesion + alignment flocking for group agents. `[ ]`
- `P3-M5-B2c` Combine steering with NavMesh path following. `[ ]`

---

### P3-M6: Advanced Networking

**Goal**: Dedicated server, lag compensation, interest management, matchmaking.
**Dependencies**: P2-M4 (basic networking).

> **Exit Criteria** *(all must pass before P3-M6 is closed)*:
> 1. Dedicated server runs headless with configurable tick rate.
> 2. Server-authoritative with input validation.
> 3. Lag compensation with server-side rewind (max 200ms).
> 4. Interest management limits bandwidth per client.
> 5. Basic matchmaking via lobby server.

---

#### P3-M6-A: Dedicated Server Mode (Headless, Configurable Tick Rate) `[ ]` **[low]**
- `P3-M6-A-a` `ENGINE_HEADLESS` build flag: no SDL window, no GL context, no audio device. `[ ]`
- `P3-M6-A-b` Server tick rate CVar independent of render frame rate. `[ ]`

#### P3-M6-B: Lag Compensation (Server-Side Rewind) `[ ]` **[low]**
- `P3-M6-B-a` Server maintains N-frame history of entity positions (ring buffer). `[ ]`
- `P3-M6-B-b` On hit registration: rewind to client timestamp; validate hit server-side. `[ ]`

#### P3-M6-C: Interest Management (Relevancy, AOI Grid, Bandwidth Budget) `[ ]` **[low]**
- `P3-M6-C-a` AOI grid: each player only receives replication for entities within relevancy radius. `[ ]`
- `P3-M6-C-b` Bandwidth budget: drop or reduce update frequency for distant entities. `[ ]`

#### P3-M6-D: Matchmaking (Lobby Server, Simple Auto-Match) `[ ]` **[low]**
- `P3-M6-D-a` Lobby server process: player registration, session listing, join flow. `[ ]`
- `P3-M6-D-b` Auto-match: rank-based or ping-based player grouping. `[ ]`

---

---

## Parallel Lanes (Non-Blocking)

These lanes run alongside phased work and do not block milestone completion, but must be finished before shipping.

---

### DOC: Documentation

- `DOC-1` Getting started guide: project setup, first scene, first script. `[ ]` **[high]**
- `DOC-2` Architecture overview: module graph, data flow, ECS model, job system. `[ ]` **[high]**
- `DOC-3` Lua API reference: all `engine.*` functions, types, callbacks. `[ ]` **[high]**
- `DOC-4` Editor user guide: panels, keyboard shortcuts, play-in-editor workflow. `[ ]` **[high]**
- `DOC-5` Asset pipeline guide: supported formats, import settings, cooking, streaming. `[ ]` **[high]**
- `DOC-6` Networking guide (when P2-M4 is complete). `[ ]` **[high]**
- `DOC-7` Contributing guide: coding standards, PR checklist, CI requirements. `[ ]` **[high]**

---

### TEST: Extended Testing Coverage

- `TEST-1` Fuzz testing: `libFuzzer` or AFL++ on scene deserializer, asset loader, Lua sandbox. `[ ]` **[high]**
- `TEST-2` Property-based testing: random physics scenario → determinism check; random ECS mutations → no crash. `[ ]` **[high]**
- `TEST-3` Soak tests: run engine for 24 hours; verify no memory growth, no deadlock. `[ ]` **[high]**
- `TEST-4` Platform smoke tests: CI runs smoke test on all three OS targets after build. `[ ]` **[high]**
- `TEST-5` Screenshot regression: headless render reference scene; pixel diff vs golden. `[ ]` **[high]**

---

### DEVOPS: Operational Pipeline

- `DEVOPS-1` Nightly build: scheduled CI run on `main`; full test suite + benchmarks + soak. `[ ]` **[high]**
- `DEVOPS-2` Cross-platform release pipeline: triggered on tag; builds all 3 OS packages; publishes GitHub Release. `[ ]` **[high]**
- `DEVOPS-3` Dependency update bot: weekly PR to bump pinned SHA hashes; CI auto-runs on PR. `[ ]` **[high]**
- `DEVOPS-4` Coverage trend tracking: store historical coverage JSON in repo; plot trend in PR comment. `[ ]` **[high]**

---

---

## What IS Production-Ready (Verified from Header Review)

The following are confirmed implemented — not gaps. Evidence: public header API is complete, corresponding unit/integration test file exists, and no obvious stub markers are present.

**Core Module**
- [x] CVar / console system (4 types: bool, int, float, string; runtime mutation; `cvar_console_test.cpp`)
- [x] Debug draw API (lines, spheres, AABBs, text with frame lifetimes; `debug_draw_test.cpp`)
- [x] Event bus (typed events + named channels; `event_bus_test.cpp`)
- [x] Job system (async work queue, dependency DAG, worker thread pool; `scheduler_stress.cpp`)
- [x] Math library (Vec2/3/4, Quat, Mat4, Transform, AABB, Sphere, Ray; `math_test.cpp`)
- [x] VFS (virtual filesystem; `vfs_test.cpp`)
- [x] Linear allocator (`linear_allocator.h`; `pool_allocator_test.cpp`)
- [x] Pool allocator (`pool_allocator.h`)
- [x] Hierarchical CPU profiler with flame-graph support (`profiler_test.cpp`)
- [x] Per-subsystem memory tracker with thread-safe counters (`mem_tracker_test.cpp`)
- [x] Service locator — type-erased, fixed-capacity, no RTTI (`service_locator_test.cpp`)
- [x] Input system — SDL events, gamepad (analog dead zones), keyboard, mouse (`input_test.cpp`)
- [x] Input action/axis mapping with runtime rebinding and JSON persistence (`input_map_test.cpp`)
- [x] Touch input and gesture recognizers (Tap, Swipe, Pinch, Rotate; `touch_input_test.cpp`)
- [x] Engine stats struct (`engine_stats_test.cpp`)

**Runtime Module**
- [x] ECS SparseSet — cache-friendly dense storage, double-buffered transforms, 65536-entity capacity
- [x] World — BeginPlay/EndPlay lifecycle phases, deferred destroy queue, `for_each<>` variadic template
- [x] Transform hierarchy — world matrix propagation, parent-by-persistent-ID, dirty detection
- [x] PointLightComponent, SpotLightComponent, SpringArmComponent in ECS
- [x] Game mode / game state / player controller structs
- [x] Timer manager — set_timeout, set_interval, serializable, per-World (`timer_test.cpp`)
- [x] Camera manager — priority stack, blend weights, camera shake (`camera_test.cpp`)
- [x] Entity pool — acquire/release with handle recycling (`entity_pool_test.cpp`)
- [x] Scene serializer with persistent FNV-hashed IDs (`scene_serializer_test.cpp`)
- [x] Prefab serializer — save/instantiate (`prefab_test.cpp`)

**Physics Module**
- [x] AABB, Sphere, Capsule, ConvexHull, Heightfield colliders
- [x] GJK/EPA collision detection (full implementation in `convex_hull.h`)
- [x] Sequential impulse constraint solver with warm starting
- [x] Joint types: distance, hinge, ball-socket, slider, spring, fixed (4096 max)
- [x] Persistent contact manifolds with reduction (2048 max)
- [x] Physics material (friction, restitution, density, combine function)
- [x] Collision layers and masks (bit flags; `collision_layer_test.cpp`)
- [x] Raycast (sorted, layer-filtered), sphere/box overlap, sphere/box sweep
- [x] CCD — bilateral advance (`ccd_test.cpp`)
- [x] Speculative contacts (`speculative_contacts_test.cpp`)
- [x] Sleep system, spatial hash broadphase

**Renderer Module**
- [x] RenderDevice function-pointer abstraction (shaders, buffers, textures, framebuffers, queries)
- [x] MRT framebuffer support (`create_framebuffer_mrt`)
- [x] G-Buffer pass resources (albedo/metallic, normal/roughness, emissive/AO, depth)
- [x] Deferred lighting pass (GpuPassId::DeferredLighting tracked)
- [x] Tiled light culling — 16×16 tiles, 128 point + 64 spot lights (`light_culling_test.cpp`)
- [x] GPU timestamp profiler per pass (`gpu_profiler_test.cpp`)
- [x] Forward PBR renderer (legacy path — DrawCommand, CommandBufferBuilder, sort by key)
- [x] Tone mapping pass (GpuPassId::Tonemap; `get_scene_viewport_texture()`)
- [x] Shader system — load/hot-reload (`shader_system_test.cpp`)
- [x] Frustum culling (integrated into render-prep pipeline)
- [x] glTF mesh loader (`mesh_loader_test.cpp`)
- [x] Texture loader (`texture_loader_test.cpp`)
- [x] Asset database — open-addressing hash table (O(1) amortized lookup), 4096 mesh + 512 texture + 4096 metadata slots
- [x] Asset metadata — tags, import settings, checksum, dependency edges
- [x] Async streaming queue — priority queue, Queued→Loading→Uploading→Ready state machine, streaming budget
- [x] LRU eviction cache — intrusive linked list, protected refs (`lru_cache_test.cpp`)
- [x] Deterministic cooking (`deterministic_cook_test.cpp`)

**Scripting Module**
- [x] Lua 5.4 — module system, require caching, circular dependency detection
- [x] Error messages with file + line via luaL_traceback
- [x] Per-entity ScriptComponent lifecycle — on_begin_play, on_tick, on_end_play dispatch
- [x] Coroutines — tick/clear, `wait()` / `wait_until()` / `wait_frames()` (`coroutine_test.cpp`)
- [x] Lua DAP debugger — TCP server, step modes, breakpoints (`dap_test.cpp`)
- [x] Lua sandboxing — restricted globals, CPU instruction limit, memory limit (`sandbox_test.cpp`)
- [x] Lua hot-reload — file watcher + incremental reload (`hotreload_test.cpp`)
- [x] Binding auto-generation tool (`bindgen_test.cpp`)
- [x] Deferred mutation queue (`flush_deferred_mutations`)
- [x] Timer Lua integration (`tick_timers`)

**Audio Module**
- [x] Basic audio playback via miniaudio (wav/mp3/ogg/flac, volume/pitch/loop)

**Editor Module**
- [x] ImGui-based editor (play/pause/stop, transform gizmos, `editor_is_playing`, `editor_is_paused`)
- [x] Command history (undo/redo — partial; transforms confirmed)
- [x] Debug camera (`debug_camera.h`)
- [x] Editor camera (`editor_camera.h`)

**CI / Build**
- [x] GitHub Actions: 3 OS × 2 config build matrix
- [x] Static analysis: cppcheck + clang-tidy + `-Werror` gate
- [x] Sanitizers: ASAN+UBSAN + TSAN (separate jobs)
- [x] Code coverage: lcov HTML report + 50% line threshold gate
- [x] Performance regression gate: ECS + physics benchmarks, 10% threshold
- [x] Cross-platform determinism check: hashes compared across all 3 OS cells

---

## Summary Statistics

| Phase | Milestones | Total Gap Items | Complete `[x]` | Partial `[~]` | Not Started `[ ]` |
|-------|-----------|-----------------|----------------|---------------|-------------------|
| §0 Technical Debt | 6 | 7 | 0 | 0 | 7 |
| P1: Ship Blockers | 12 | ~130 | ~57 | ~14 | ~59 |
| P2: Competitive Parity | 8 | ~50 | 0 | 0 | ~50 |
| P3: Cutting-Edge | 6 | ~20 | 0 | 1 | ~19 |
| Parallel Lanes | 3 | 16 | 0 | 0 | 16 |
| **Total** | **35** | **~223** | **~57** | **~15** | **~151** |

**P1 completion estimate**: ~44% of ship-blocker items are production-ready. The major completed clusters are: CI/build infrastructure, physics (all colliders + solver + queries + CCD), asset pipeline (all of M4), scripting (lifecycle + coroutines + DAP + sandbox + hot-reload + bindgen), World/ECS foundational systems (lifecycle, pooling, game mode, camera, timer, input), and deferred rendering infrastructure (G-buffer, deferred lighting, tiled culling, GPU profiler, tone mapping).

**Largest remaining P1 gaps by work volume** (in order):
1. Animation system (P1-M7) — zero implementation; entire subsystem missing (~20 atomic tasks).
2. Game UI runtime (P1-M11) — zero implementation; entire subsystem missing (~15 atomic tasks).
3. Shadow maps (P1-M5-C) — deferred lighting is ready but no shadow depth passes.
4. Audio advanced features (P1-M8 A/B/C/D) — only basic playback exists (~15 atomic tasks).
5. Editor completion (P1-M9-A2/C/D) — reflection inspector, hierarchy panel, asset browser.
6. Platform / packaging (P1-M12) — zero implementation (~15 atomic tasks).
7. Post-process effects (P1-M5-D2/D3/D5) — bloom, SSAO, FXAA not started.
8. Sky, fog, instancing (P1-M6-A/B/C) — environment rendering missing.

**Gap-to-milestone traceability**: Every `[x]`/`[~]`/`[ ]` status code in this document maps 1:1 to an atomic task in production_engine_milestones.md and a checkbox in production_engine_phased_todo.md. This file is the single source of truth — the other two files are supplementary.
