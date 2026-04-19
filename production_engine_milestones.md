# Production Engine Milestones — Atomic Execution Plan

> **⚠ Combined Document Notice**: The master gap analysis, milestone definitions, exit criteria, and completion status are now consolidated in **`production_engine_gap_list.md`**. That file is the single source of truth.
>
> This file is retained as a **supplementary reference** containing detailed atomic task annotations (`[files:]`, `[test:]`, `[dep:]`) and implementation guidance that may be useful during development.

Source checklist: production_engine_phased_todo.md
Gap list (MASTER): production_engine_gap_list.md

Every milestone is recursively subdivided until each leaf task is a single-session deliverable
(one .cpp/.h file change, one shader, one test, one binding — roughly 1–4 hours of focused work).

**Hierarchy**: Phase > Milestone > Sub-milestone > Work Package > Atomic Task

**Reading this document**:
- Indentation depth = subdivision depth.
- Leaf nodes (deepest indent) are the actual work items.
- `[dep: X]` means "blocked until X is done."
- `[files: ...]` hints at which source files are touched.
- `[test: ...]` states the required test evidence.

---

## Global Rules

- Module dependency: `core → math → physics/scripting/renderer/audio → runtime → editor → app`. Never violate.
- Every public C++ function: `noexcept`, explicit error return (`bool` or enum), logs on failure.
- No heap allocation on hot paths. Use `core::frame_allocator()`, pools, or fixed storage.
- No `std::vector/map/string/unordered_map` in per-frame code.
- Tests accompany every deliverable — unit, integration, or smoke as appropriate.
- Mark phased-todo checkboxes only when the 7-point completion standard (copilot-instructions.md) is met.

---

## Phase 1: Ship Blockers (High Priority)

Everything in Phase 1 must be complete before a game can be shipped on any platform.

---

### P1-M1: Engine Production Baseline

**Goal**: Harden build, CI, determinism, profiling, and debug utilities so every subsequent milestone has a safety net.

**Dependencies**: None (first milestone).

#### P1-M1-A: Build System Hardening

##### P1-M1-A1: Unified CMake Configuration
- **P1-M1-A1a**: Add `ENGINE_TARGET_PLATFORM` CMake option (Win64/Linux/macOS/Android/iOS/Web). Gate platform-specific sources behind it. [files: root CMakeLists.txt] [test: configure succeeds for each platform string]
- **P1-M1-A1b**: Add cross-compile toolchain file stubs for Android (NDK), iOS (Xcode), Emscripten. [files: cmake/toolchains/] [test: cmake configure with `-DCMAKE_TOOLCHAIN_FILE=...` does not error]
- **P1-M1-A1c**: Verify single `cmake -S . -B build -DENGINE_TARGET_PLATFORM=Win64` builds all modules cleanly. [test: zero warnings, zero errors]

##### P1-M1-A2: Precompiled Headers
- **P1-M1-A2a**: Create `core/src/pch.h` with `<cstdint>`, `<cstddef>`, `<cstring>`, `<cassert>`, core logging header. [files: core/src/pch.h, core/CMakeLists.txt]
- **P1-M1-A2b**: Enable `target_precompile_headers(engine_core PRIVATE src/pch.h)` in CMake. Verify compile-time improvement. [test: build time comparison before/after]
- **P1-M1-A2c**: Add PCH for renderer module (GL headers, math types). [files: renderer/src/pch.h, renderer/CMakeLists.txt]

##### P1-M1-A3: Incremental Compilation
- **P1-M1-A3a**: Audit all modules for unnecessary header includes. Remove transitive includes. [files: all public headers]
- **P1-M1-A3b**: Add `#pragma once` audit — verify every header has it, no double-inclusion guards mixed. [test: grep for include guards vs pragma once]
- **P1-M1-A3c**: Measure rebuild time after touching a single core header. Should not rebuild all modules. [test: touch core/include/engine/core/logging.h, rebuild, count recompiled TUs]

#### P1-M1-B: CI Pipeline

##### P1-M1-B1: GitHub Actions Workflow — Build Matrix
- **P1-M1-B1a**: Create `.github/workflows/ci.yml` with matrix: {os: [ubuntu-latest, windows-latest, macos-latest], build_type: [Debug, Release]}. [files: .github/workflows/ci.yml]
- **P1-M1-B1b**: Steps: checkout, configure, build, test (`ctest --output-on-failure`). Cache `_deps/` between runs. [test: CI passes green on all 6 matrix cells]
- **P1-M1-B1c**: Add build artifact upload (compile_commands.json for clang-tidy). [files: ci.yml]

##### P1-M1-B2: Static Analysis Lane
- **P1-M1-B2a**: Add CI job: `cmake --build build --target analysis` (cppcheck). Fail on any new warning. [files: ci.yml] [test: cppcheck returns 0]
- **P1-M1-B2b**: Add clang-tidy job using compile_commands.json. Configure `.clang-tidy` with engine-relevant checks (modernize, bugprone, performance). [files: .clang-tidy, ci.yml]
- **P1-M1-B2c**: Add `-Werror` / `/WX` verification: CI must fail if any compiler warning exists. [test: introduce a deliberate warning, verify CI fails, revert]

##### P1-M1-B3: Sanitizer Lane
- **P1-M1-B3a**: Add CI job: build with `-DENGINE_SANITIZERS=ON` (ASAN+UBSAN). Run full test suite. [files: ci.yml]
- **P1-M1-B3b**: Add TSAN lane (separate because TSAN conflicts with ASAN). Run thread-sensitive tests. [files: ci.yml]
- **P1-M1-B3c**: Configure sanitizer suppressions file for known third-party issues (SDL, Lua). [files: sanitizer_suppressions.txt]

##### P1-M1-B4: Code Coverage
- **P1-M1-B4a**: Add CI job: build with `--coverage` (gcov/llvm-cov). Run tests. [files: ci.yml]
- **P1-M1-B4b**: Generate lcov/llvm-cov HTML report. Upload as artifact. [files: ci.yml]
- **P1-M1-B4c**: Add coverage threshold gate — fail if total line coverage drops below X%. [test: coverage report exists; threshold enforced]

##### P1-M1-B5: Performance Regression Gate
- **P1-M1-B5a**: Create `tests/benchmark/ecs_perf_test.cpp`: measure time to iterate 50K entities with Transform+RigidBody. Record baseline. [files: tests/benchmark/, tests/CMakeLists.txt]
- **P1-M1-B5b**: Create `tests/benchmark/physics_perf_test.cpp`: measure time for 1000-body step. [files: tests/benchmark/]
- **P1-M1-B5c**: Add CI step: run benchmarks, compare against stored baseline JSON, fail if >10% regression. [files: ci.yml, scripts/check_perf.py or .ps1]

#### P1-M1-C: Determinism and Replay Baseline

##### P1-M1-C1: Cross-Platform Determinism Tests
- **P1-M1-C1a**: Extend existing determinism test: run identical physics scenario on all CI matrix platforms, hash final state. [files: tests/integration/determinism_test.cpp]
- **P1-M1-C1b**: Add determinism for varying thread counts (1, 2, 4, 8 workers). Hashes must match. [test: test passes with `--workers=1` through `--workers=8`]
- **P1-M1-C1c**: Document any floating-point strictness flags needed per platform. Add to CMake. [files: cmake/, docs/determinism.md]

##### P1-M1-C2: ECS Stress Test
- **P1-M1-C2a**: Raise `kMaxEntities` to 65536 (or make configurable). [files: runtime/include/engine/runtime/world.h]
- **P1-M1-C2b**: Write stress test: create 50K+ entities, add Transform+RigidBody, iterate all in simulation phase. Measure time. [files: tests/integration/ecs_stress_test.cpp]
- **P1-M1-C2c**: Verify no OOM, no crash, iteration < 16ms at 50K entities. [test: test passes, timing logged]

#### P1-M1-D: Memory and Profiling Infrastructure

##### P1-M1-D1: Allocator Audit and Expansion
- **P1-M1-D1a**: Audit all `new`/`malloc` calls in per-frame paths (physics step, render prep, scripting tick). List them. [output: audit document or comments]
- **P1-M1-D1b**: Replace identified hot-path allocations with `core::frame_allocator()` or `core::thread_frame_allocator()`. [files: physics/src/, renderer/src/, scripting/src/]
- **P1-M1-D1c**: Add `core::PoolAllocator<T, N>` template if not already present. Use for fixed-size temporary buffers. [files: core/include/engine/core/pool_allocator.h]

##### P1-M1-D2: CPU Profiler Enhancement
- **P1-M1-D2a**: Replace flat `ProfileEntry` list with hierarchical tree (parent pointer per entry). [files: core/include/engine/core/profiler.h, core/src/profiler.cpp]
- **P1-M1-D2b**: Add `PROFILE_SCOPE("name")` macro that pushes/pops the profiler stack automatically via RAII. [files: core/include/engine/core/profiler.h]
- **P1-M1-D2c**: Render flame graph in editor debug panel (horizontal bars, depth = nesting). [files: editor/src/editor.cpp]

##### P1-M1-D3: GPU Profiler Baseline
- **P1-M1-D3a**: Implement `GpuTimerQuery` wrapper: begin/end using `glQueryCounter` (GL_TIMESTAMP). [files: renderer/include/engine/renderer/gpu_profiler.h, renderer/src/gpu_profiler.cpp]
- **P1-M1-D3b**: Instrument each render pass (scene, tonemap) with GPU timer queries. [files: renderer/src/command_buffer.cpp]
- **P1-M1-D3c**: Display GPU pass timings in editor stats overlay (ms per pass). [files: editor/src/editor.cpp]

##### P1-M1-D4: In-Game Stats Overlay
- **P1-M1-D4a**: Create `core::EngineStats` struct: fps, frame_time_ms, draw_calls, tri_count, entity_count, memory_used_mb. Updated each frame. [files: core/include/engine/core/engine_stats.h, core/src/engine_stats.cpp]
- **P1-M1-D4b**: Populate stats from renderer (draw calls, triangles), runtime (entities), and OS (memory). [files: renderer/src/, runtime/src/world.cpp, app/main.cpp]
- **P1-M1-D4c**: Render overlay using ImGui in top-left corner, toggled by CVar `r_showStats`. [files: editor/src/editor.cpp]

#### P1-M1-E: Debug Utilities Completion

##### P1-M1-E1: Debug Camera
- **P1-M1-E1a**: Add `debug_camera` module: free-fly camera with WASD+mouse, independent of game camera. [files: editor/include/engine/editor/debug_camera.h, editor/src/debug_camera.cpp]
- **P1-M1-E1b**: Toggle via CVar `debug.camera_detach`. When active, game camera freezes, debug camera takes viewport. [files: editor/src/editor.cpp]
- **P1-M1-E1c**: Ensure frustum of frozen game camera renders as debug wireframe. [files: editor/src/debug_camera.cpp, using debug_draw API]

##### P1-M1-E2: God Mode / Cheat Commands
- **P1-M1-E2a**: Register console command `god` — makes player entity invulnerable (skip damage in gameplay scripts). [files: scripting/src/scripting.cpp or app/main.cpp]
- **P1-M1-E2b**: Register console command `noclip` — disable collision for player entity, enable free movement. [files: scripting/src/scripting.cpp]
- **P1-M1-E2c**: Register console command `spawn <prefab_name> [x y z]` — instantiate prefab at position. [files: scripting/src/scripting.cpp]
- **P1-M1-E2d**: Register console command `kill_all` — destroy all entities except player. [files: scripting/src/scripting.cpp]

##### P1-M1-E3: Memory Subsystem Tracking
- **P1-M1-E3a**: Add per-subsystem memory tags: `MemTag::Physics`, `MemTag::Renderer`, `MemTag::Audio`, `MemTag::Scripting`, `MemTag::ECS`, `MemTag::Assets`. [files: core/include/engine/core/memory.h]
- **P1-M1-E3b**: Track allocations per tag (increment on alloc, decrement on free). Thread-safe atomic counters. [files: core/src/memory.cpp]
- **P1-M1-E3c**: Display memory breakdown in stats overlay (bar chart per subsystem). [files: editor/src/editor.cpp]

**P1-M1 Exit Criteria**:
- CI runs on every push: build matrix (3 OS × 2 configs), static analysis, sanitizers, coverage, perf gates.
- Determinism test passes across all CI platforms and thread counts.
- ECS handles 50K+ entities without crash or >16ms iteration.
- Flame graph profiler, GPU timings, and stats overlay are functional.
- Debug camera, cheat commands, and memory tracking are available in dev builds.

---

### P1-M2: World, ECS, Gameplay Loop Foundation

**Goal**: Production actor/component lifecycle, input system, game state architecture, camera, coroutines, and script safety.

**Dependencies**: P1-M1.

#### P1-M2-A: Actor / Component Lifecycle

##### P1-M2-A1: C++ Lifecycle Hooks in World
- **P1-M2-A1a**: Add `WorldPhase::BeginPlay` between entity creation and first `Simulation` tick. World iterates all new entities and calls their `on_begin_play`. [files: runtime/include/engine/runtime/world.h, runtime/src/world.cpp]
- **P1-M2-A1b**: Add `WorldPhase::EndPlay` before entity destruction. World calls `on_end_play` on entities marked for destroy. [files: runtime/src/world.cpp]
- **P1-M2-A1c**: Add `destroy_entity(Entity e)` that defers destruction to end of frame (destroy list). Process destroy list in `EndPlay` phase. [files: runtime/src/world.cpp]
- **P1-M2-A1d**: Write integration test: create entity, verify BeginPlay fires once, tick 3 frames verifying Tick fires each, destroy and verify EndPlay fires once. [files: tests/integration/lifecycle_test.cpp]

##### P1-M2-A2: Lua Lifecycle Binding
- **P1-M2-A2a**: Add Lua callbacks: `on_begin_play(entity)`, `on_tick(entity, dt)`, `on_end_play(entity)` registration per script. [files: scripting/src/scripting.cpp]
- **P1-M2-A2b**: Store callback function refs per entity in a table. Call during appropriate World phases. [files: scripting/src/scripting.cpp]
- **P1-M2-A2c**: Handle errors in lifecycle callbacks: pcall with traceback, log error with file+line, mark entity script as faulted (skip future calls). [files: scripting/src/scripting.cpp]
- **P1-M2-A2d**: Write test: Lua script that increments a counter in on_tick, verify counter matches frame count after N frames. [files: tests/integration/lua_lifecycle_test.cpp]

##### P1-M2-A3: Entity Pooling
- **P1-M2-A3a**: Add `EntityPool` class: pre-allocates entity handles, `acquire()` returns recycled handle, `release()` returns to free list. [files: runtime/include/engine/runtime/entity_pool.h, runtime/src/entity_pool.cpp]
- **P1-M2-A3b**: Integrate with World: `spawn_from_pool(pool, prefab)` and `destroy_to_pool(pool, entity)`. [files: runtime/src/world.cpp]
- **P1-M2-A3c**: Lua binding: `engine.pool_create(prefab, count)`, `engine.pool_spawn(pool_id)`, `engine.pool_release(pool_id, entity)`. [files: scripting/src/scripting.cpp]
- **P1-M2-A3d**: Write test: spawn 100 entities from pool, release all, re-spawn 100, verify handle reuse (no new IDs). [files: tests/unit/entity_pool_test.cpp]

#### P1-M2-B: Game State Architecture

##### P1-M2-B1: Game Mode / Game State Separation
- **P1-M2-B1a**: Define `GameMode` struct: holds current state enum, rules table (max players, win condition callbacks). Owned by World. [files: runtime/include/engine/runtime/game_mode.h]
- **P1-M2-B1b**: Define `GameState` struct: persistent cross-scene data (score, inventory, checkpoints). Separate from World lifetime. [files: runtime/include/engine/runtime/game_state.h]
- **P1-M2-B1c**: Define `PlayerController` struct: maps input to actions on the controlled entity. One per player. [files: runtime/include/engine/runtime/player_controller.h]
- **P1-M2-B1d**: Lua bindings: `engine.get_game_mode()`, `engine.get_game_state()`, `engine.get_player_controller(player_index)`. [files: scripting/src/scripting.cpp]
- **P1-M2-B1e**: Write integration test: set game mode, transition states, verify Lua callbacks fire. [files: tests/integration/game_mode_test.cpp]

##### P1-M2-B2: Subsystem / Service Locator
- **P1-M2-B2a**: Create `core::ServiceLocator` — type-erased registry mapping `TypeId → void*`. Register/get with `register_service<T>(T*)` / `get_service<T>()`. [files: core/include/engine/core/service_locator.h, core/src/service_locator.cpp]
- **P1-M2-B2b**: Register all existing singletons (physics world, audio system, asset DB, renderer) as services at startup. [files: app/main.cpp or runtime initialization]
- **P1-M2-B2c**: Migrate `g_world`, `g_services`, `g_physics_world` globals in scripting.cpp to use ServiceLocator. [files: scripting/src/scripting.cpp]
- **P1-M2-B2d**: Write unit test: register, retrieve, overwrite, retrieve again. Verify type safety. [files: tests/unit/service_locator_test.cpp]

#### P1-M2-C: Input System

##### P1-M2-C1: Input Action / Axis Mapping
- **P1-M2-C1a**: Define `InputAction` struct: name string, list of bound keys/buttons/axes, callbacks. [files: core/include/engine/core/input_map.h]
- **P1-M2-C1b**: Define `InputAxisMapping` struct: name, bound axis (stick, mouse delta), scale, dead zone. [files: core/include/engine/core/input_map.h]
- **P1-M2-C1c**: Implement `InputMapper` class: load mappings from JSON config, process raw input events, fire action/axis callbacks. [files: core/src/input_map.cpp]
- **P1-M2-C1d**: Integrate with existing SDL event loop in `core/src/input.cpp`: raw events → InputMapper → action callbacks. [files: core/src/input.cpp]
- **P1-M2-C1e**: Write unit test: bind key to action, simulate key press, verify callback fires. [files: tests/unit/input_map_test.cpp]

##### P1-M2-C2: Runtime Rebinding
- **P1-M2-C2a**: Add `InputMapper::rebind(action_name, new_key)` — updates mapping at runtime. [files: core/src/input_map.cpp]
- **P1-M2-C2b**: Add `InputMapper::save_bindings(path)` / `load_bindings(path)` — persist to JSON. [files: core/src/input_map.cpp]
- **P1-M2-C2c**: Lua binding: `engine.rebind_action("jump", "space")`, `engine.save_input_config()`. [files: scripting/src/scripting.cpp]
- **P1-M2-C2d**: Write test: rebind, verify new key triggers action, save/load roundtrip. [files: tests/unit/input_rebind_test.cpp]

##### P1-M2-C3: Touch Input and Gestures
- **P1-M2-C3a**: Add `TouchEvent` struct: touch_id, position, pressure, phase (began/moved/ended/cancelled). Process SDL_FINGERDOWN/MOVE/UP. [files: core/include/engine/core/touch_input.h, core/src/touch_input.cpp]
- **P1-M2-C3b**: Add gesture recognizers: `TapRecognizer`, `SwipeRecognizer`, `PinchRecognizer`, `RotateRecognizer`. Each stateful, processes touch stream. [files: core/src/gesture_recognizer.cpp]
- **P1-M2-C3c**: Integrate with InputMapper: gestures can bind to actions (e.g., "swipe_right" → "dodge"). [files: core/src/input_map.cpp]
- **P1-M2-C3d**: Add touch-to-mouse emulation fallback (first finger = mouse cursor). [files: core/src/touch_input.cpp]
- **P1-M2-C3e**: Lua binding: `engine.on_touch(callback)`, `engine.on_gesture("pinch", callback)`. [files: scripting/src/scripting.cpp]
- **P1-M2-C3f**: Write test: simulate multi-touch sequence, verify gesture callbacks fire. [files: tests/unit/touch_input_test.cpp]

#### P1-M2-D: Timer System

##### P1-M2-D1: Core Timer Manager
- **P1-M2-D1a**: Create `TimerManager` class: per-World, fixed-capacity array of timer entries (id, delay, repeat, callback, elapsed). [files: runtime/include/engine/runtime/timer_manager.h, runtime/src/timer_manager.cpp]
- **P1-M2-D1b**: `set_timeout(delay, callback) → TimerId`, `set_interval(delay, callback) → TimerId`, `cancel(TimerId)`. [files: runtime/src/timer_manager.cpp]
- **P1-M2-D1c**: Tick all timers in World::update simulation phase. Expired one-shots auto-remove. [files: runtime/src/world.cpp]
- **P1-M2-D1d**: Serialization: save/load active timers in scene JSON (delay remaining, repeat flag). [files: runtime/src/scene_serializer.cpp]

##### P1-M2-D2: Lua Timer Binding
- **P1-M2-D2a**: Lua: `engine.set_timeout(seconds, function)`, `engine.set_interval(seconds, function)`, `engine.cancel_timer(id)`. [files: scripting/src/scripting.cpp]
- **P1-M2-D2b**: Timer callbacks call Lua functions via pcall with error handling. [files: scripting/src/scripting.cpp]
- **P1-M2-D2c**: Write test: set timeout 0.5s, advance world 30 frames at 60fps, verify callback fires at frame 30. [files: tests/integration/timer_test.cpp]

#### P1-M2-E: Gameplay Camera System

##### P1-M2-E1: Spring Arm Component
- **P1-M2-E1a**: Define `SpringArmComponent` struct: target_length, collision_radius, lag_speed, socket_offset (POD). [files: runtime/include/engine/runtime/world.h]
- **P1-M2-E1b**: Add SparseSet storage, CRUD methods in World (same pattern as other components). [files: runtime/src/world.cpp]
- **P1-M2-E1c**: Implement spring arm update: sweep from target position backward by target_length, shorten on collision. Smooth via lag_speed. [files: runtime/src/camera_system.cpp]
- **P1-M2-E1d**: Register reflection, serialization, Lua binding. [files: runtime/src/reflect_types.cpp, runtime/src/scene_serializer.cpp, scripting/src/scripting.cpp]

##### P1-M2-E2: Camera Manager
- **P1-M2-E2a**: Create `CameraManager` class: maintains a priority stack of camera entities. Highest priority is active. [files: runtime/include/engine/runtime/camera_manager.h, runtime/src/camera_manager.cpp]
- **P1-M2-E2b**: `push_camera(entity, priority, blend_time)`, `pop_camera(entity, blend_time)`. Blend interpolates position/rotation over blend_time. [files: runtime/src/camera_manager.cpp]
- **P1-M2-E2c**: Lua binding: `engine.push_camera(entity, priority, blend_time)`, `engine.get_active_camera()`. [files: scripting/src/scripting.cpp]

##### P1-M2-E3: Camera Shake
- **P1-M2-E3a**: Create `CameraShake` struct: amplitude, frequency, duration, decay curve, current_time. [files: runtime/include/engine/runtime/camera_shake.h]
- **P1-M2-E3b**: Implement Perlin noise-based shake: offset camera position/rotation by noise(time * frequency) * amplitude * decay(t). [files: runtime/src/camera_shake.cpp]
- **P1-M2-E3c**: `CameraManager::add_shake(shake_params)` — stack multiple shakes additively. [files: runtime/src/camera_manager.cpp]
- **P1-M2-E3d**: Lua binding: `engine.camera_shake(amplitude, frequency, duration)`. [files: scripting/src/scripting.cpp]
- **P1-M2-E3e**: Write test: trigger shake, verify camera offset is non-zero during duration, returns to zero after. [files: tests/unit/camera_shake_test.cpp]

#### P1-M2-F: Coroutines / Async Gameplay

##### P1-M2-F1: Lua Coroutine Scheduler
- **P1-M2-F1a**: Create `CoroutineScheduler` class: manages a list of suspended Lua coroutines with resume conditions. [files: scripting/include/engine/scripting/coroutine_scheduler.h, scripting/src/coroutine_scheduler.cpp]
- **P1-M2-F1b**: Implement `engine.wait(seconds)` — yields current coroutine, resumes after elapsed time. [files: scripting/src/scripting.cpp]
- **P1-M2-F1c**: Implement `engine.wait_until(condition_func)` — yields, resumes when condition returns true. [files: scripting/src/scripting.cpp]
- **P1-M2-F1d**: Implement `engine.wait_frames(n)` — yields, resumes after N frames. [files: scripting/src/scripting.cpp]
- **P1-M2-F1e**: Tick scheduler each frame: check each suspended coroutine's condition, resume if met. [files: scripting/src/coroutine_scheduler.cpp]
- **P1-M2-F1f**: Handle coroutine errors: pcall resume, log error with traceback, remove faulted coroutines. [files: scripting/src/coroutine_scheduler.cpp]
- **P1-M2-F1g**: Write test: Lua script with `engine.wait(0.5)` then sets a flag. Advance 30 frames. Verify flag set at correct frame. [files: tests/integration/coroutine_test.cpp]

#### P1-M2-G: Script Runtime Safety

##### P1-M2-G1: DAP Debugger Implementation
- **P1-M2-G1a**: Implement DAP JSON-RPC transport over TCP socket (listen on configurable port). [files: scripting/src/dap_transport.cpp]
- **P1-M2-G1b**: Handle `initialize`, `launch`, `configurationDone` requests. [files: scripting/src/dap_session.cpp]
- **P1-M2-G1c**: Implement `setBreakpoints` — add Lua debug hooks at specified file:line. [files: scripting/src/dap_session.cpp]
- **P1-M2-G1d**: Implement `continue`, `next` (step over), `stepIn`, `stepOut` using `lua_sethook`. [files: scripting/src/dap_session.cpp]
- **P1-M2-G1e**: Implement `stackTrace` — walk Lua call stack, return frames with file/line/function. [files: scripting/src/dap_session.cpp]
- **P1-M2-G1f**: Implement `scopes` and `variables` — enumerate locals, upvalues, globals at each frame. [files: scripting/src/dap_session.cpp]
- **P1-M2-G1g**: Implement `evaluate` — execute expression in paused context, return result. [files: scripting/src/dap_session.cpp]
- **P1-M2-G1h**: Write integration test: connect mock DAP client, set breakpoint, run script, verify pause at correct line. [files: tests/integration/dap_debugger_test.cpp]

##### P1-M2-G2: Lua Sandboxing
- **P1-M2-G2a**: Create per-script sandbox: new Lua environment table with restricted global set (no `io`, `os.execute`, `loadfile`, `dofile`). [files: scripting/src/sandbox.cpp]
- **P1-M2-G2b**: Allow-list safe functions: math, string, table, engine.*, custom require. [files: scripting/src/sandbox.cpp]
- **P1-M2-G2c**: Add CPU instruction count limit per script per frame (using `lua_sethook` with `LUA_MASKCOUNT`). [files: scripting/src/sandbox.cpp]
- **P1-M2-G2d**: Add memory limit per script environment (custom allocator that tracks bytes, fails at limit). [files: scripting/src/sandbox.cpp]
- **P1-M2-G2e**: Write test: script tries `io.open()`→ error. Script infinite loops → terminates at instruction limit. Script allocates huge table → fails at memory limit. [files: tests/integration/sandbox_test.cpp]

##### P1-M2-G3: Hot-Reload with State Preservation
- **P1-M2-G3a**: Before reload: snapshot all script-owned entity state (globals marked as persistent by user via `engine.persist("var_name")`). [files: scripting/src/hot_reload.cpp]
- **P1-M2-G3b**: Re-execute modified script files in fresh sandbox. [files: scripting/src/hot_reload.cpp]
- **P1-M2-G3c**: After reload: restore snapshots. Re-register lifecycle callbacks. [files: scripting/src/hot_reload.cpp]
- **P1-M2-G3d**: On reload error: revert to previous script version, log error, mark script as modified-but-failed. [files: scripting/src/hot_reload.cpp]
- **P1-M2-G3e**: Write test: modify script during play, trigger reload, verify persistent state survives and new code runs. [files: tests/integration/hot_reload_test.cpp]

##### P1-M2-G4: Binding Generation
- **P1-M2-G4a**: Create binding generator tool: parse annotated C++ headers, emit `lua_engine_*` static functions. [files: tools/bindgen/main.cpp]
- **P1-M2-G4b**: Define annotation syntax: `// LUA_BIND: get_position(entity) -> Vec3` in C++ header comments. [files: tools/bindgen/parser.cpp]
- **P1-M2-G4c**: Generator emits: argument validation, type conversion, function call, return value push. [files: tools/bindgen/codegen.cpp]
- **P1-M2-G4d**: Integrate into CMake: run bindgen as pre-build step, output `scripting/src/generated_bindings.cpp`. [files: scripting/CMakeLists.txt, tools/CMakeLists.txt]
- **P1-M2-G4e**: Migrate at least 20 existing hand-written bindings to generated bindings as validation. [files: scripting/src/scripting.cpp]
- **P1-M2-G4f**: Write test: bindgen tool processes test header, generates valid C++ that compiles and runs. [files: tests/unit/bindgen_test.cpp]

**P1-M2 Exit Criteria**:
- BeginPlay/Tick/EndPlay lifecycle fires correctly from both C++ and Lua.
- Input actions work with keyboard, gamepad, and touch with runtime rebinding.
- Game mode/state/controller are architectural types, not global strings.
- Timer manager is per-World and serializable.
- Spring arm, camera shake, and camera blending are functional.
- `engine.wait(seconds)` works in Lua coroutines.
- DAP debugger connects, sets breakpoints, and inspects variables.
- Per-script sandboxing prevents `io` access and enforces CPU/memory limits.
- Hot-reload preserves marked persistent state.
- Binding generator produces at least 20 bindings automatically.

---

### P1-M3: Physics Engine Hardening

**Goal**: Complete constraint solver, material system, collision shapes, CCD, physics queries, and Lua bindings to production level.

**Dependencies**: P1-M1, P1-M2 (lifecycle hooks).

#### P1-M3-A: Collision Shape Expansion

##### P1-M3-A1: Capsule Collider
- **P1-M3-A1a**: Define `CapsuleCollider` struct: half_height, radius. Store in SparseSet. [files: physics/include/engine/physics/collider.h, physics/src/collider.cpp]
- **P1-M3-A1b**: Implement capsule-vs-AABB narrow phase (closest-point on segment to box). [files: physics/src/narrow_phase.cpp]
- **P1-M3-A1c**: Implement capsule-vs-sphere narrow phase. [files: physics/src/narrow_phase.cpp]
- **P1-M3-A1d**: Implement capsule-vs-capsule narrow phase (segment-segment closest point). [files: physics/src/narrow_phase.cpp]
- **P1-M3-A1e**: Update spatial hash broadphase: compute capsule AABB for insertion. [files: physics/src/broadphase.cpp]
- **P1-M3-A1f**: Lua binding: `engine.add_capsule_collider(entity, half_height, radius)`. [files: scripting/src/scripting.cpp]
- **P1-M3-A1g**: Write test: capsule-vs-AABB, capsule-vs-sphere, capsule-vs-capsule contact generation. [files: tests/unit/capsule_test.cpp]

##### P1-M3-A2: Mesh Collider (Convex Hull)
- **P1-M3-A2a**: Define `ConvexHullCollider` struct: array of planes (up to 64), array of vertices, AABB cache. [files: physics/include/engine/physics/collider.h]
- **P1-M3-A2b**: Implement convex hull builder: input mesh vertices → quickhull → half-edge → plane array. [files: physics/src/convex_hull.cpp]
- **P1-M3-A2c**: Implement GJK/EPA for convex-vs-convex narrow phase. [files: physics/src/gjk_epa.cpp]
- **P1-M3-A2d**: Implement convex-vs-sphere, convex-vs-capsule using GJK support functions. [files: physics/src/gjk_epa.cpp]
- **P1-M3-A2e**: Cook convex hull at asset import time (asset packer). Store cooked data. [files: tools/asset_packer/main.cpp]
- **P1-M3-A2f**: Write test: cube mesh → same result as AABB; tetrahedron contact points are correct. [files: tests/unit/convex_hull_test.cpp]

##### P1-M3-A3: Heightfield Collider
- **P1-M3-A3a**: Define `HeightfieldCollider` struct: 2D height array, x/z spacing, min/max y cached. [files: physics/include/engine/physics/collider.h]
- **P1-M3-A3b**: Implement ray-vs-heightfield (grid march + bilinear interpolation). [files: physics/src/heightfield.cpp]
- **P1-M3-A3c**: Implement AABB-vs-heightfield (check overlapping grid cells, generate contact for each). [files: physics/src/heightfield.cpp]
- **P1-M3-A3d**: Implement sphere-vs-heightfield (closest point on triangle fan at grid cell). [files: physics/src/heightfield.cpp]
- **P1-M3-A3e**: Write test: flat heightfield behaves like plane; single bump deflects sphere. [files: tests/unit/heightfield_test.cpp]

#### P1-M3-B: Constraint Solver

##### P1-M3-B1: Sequential Impulse Solver Core
- **P1-M3-B1a**: Implement `ConstraintSolver` class: accumulate constraints, iterate N times (configurable), apply impulse corrections. [files: physics/include/engine/physics/constraint_solver.h, physics/src/constraint_solver.cpp]
- **P1-M3-B1b**: Add warm starting: cache impulse from previous frame, apply before first iteration. [files: physics/src/constraint_solver.cpp]
- **P1-M3-B1c**: CVar `physics.solver_iterations` (default 8). [files: physics/src/constraint_solver.cpp]

##### P1-M3-B2: Joint Types
- **P1-M3-B2a**: Implement hinge joint: constrain relative rotation to one axis, with optional angle limits. [files: physics/src/joints/hinge_joint.cpp]
- **P1-M3-B2b**: Implement ball-socket joint: constrain relative position, free rotation. [files: physics/src/joints/ball_joint.cpp]
- **P1-M3-B2c**: Implement slider joint: constrain movement to one axis, with optional distance limits. [files: physics/src/joints/slider_joint.cpp]
- **P1-M3-B2d**: Implement spring joint: distance constraint with configurable stiffness and damping. [files: physics/src/joints/spring_joint.cpp]
- **P1-M3-B2e**: Implement fixed joint: zero relative motion (welding). [files: physics/src/joints/fixed_joint.cpp]
- **P1-M3-B2f**: Lua bindings for all joint types: `engine.add_hinge_joint(a, b, pivot, axis)` etc. [files: scripting/src/scripting.cpp]
- **P1-M3-B2g**: Write test per joint: verify constraint holds under load, limits are respected. [files: tests/unit/joint_tests.cpp]

##### P1-M3-B3: Contact Caching and Manifold Reduction
- **P1-M3-B3a**: Implement persistent contact manifold: match contacts across frames by feature ID (edge-edge, face-vertex). [files: physics/src/contact_manifold.cpp]
- **P1-M3-B3b**: Implement manifold reduction: keep at most 4 contacts per pair (maximize contact area). [files: physics/src/contact_manifold.cpp]
- **P1-M3-B3c**: Write test: box resting on box retains 4 contacts across 100 frames. [files: tests/unit/manifold_test.cpp]

#### P1-M3-C: Physics Material System

##### P1-M3-C1: Material Properties
- **P1-M3-C1a**: Define `PhysicsMaterial` struct: friction (static, dynamic), restitution, density. [files: physics/include/engine/physics/physics_material.h]
- **P1-M3-C1b**: Assign material per collider (default material if unset). [files: physics/src/collider.cpp]
- **P1-M3-C1c**: Implement material combination rules: friction = sqrt(a*b), restitution = max(a,b). [files: physics/src/narrow_phase.cpp]
- **P1-M3-C1d**: Lua binding: `engine.create_physics_material(friction, restitution, density)`, `engine.set_collider_material(entity, mat)`. [files: scripting/src/scripting.cpp]

##### P1-M3-C2: Collision Layers and Masks
- **P1-M3-C2a**: Add `collision_layer` and `collision_mask` uint32 per collider (bit flags). [files: physics/include/engine/physics/collider.h]
- **P1-M3-C2b**: Broadphase filters pairs by `(a.layer & b.mask) && (b.layer & a.mask)`. [files: physics/src/broadphase.cpp]
- **P1-M3-C2c**: Lua binding: `engine.set_collision_layer(entity, layer_bits)`, `engine.set_collision_mask(entity, mask_bits)`. [files: scripting/src/scripting.cpp]
- **P1-M3-C2d**: Write test: entity on layer 2, mask excludes layer 2 → no collision. [files: tests/unit/collision_layer_test.cpp]

#### P1-M3-D: Physics Queries

##### P1-M3-D1: Raycast
- **P1-M3-D1a**: Implement `PhysicsWorld::raycast(origin, direction, max_distance, mask) → RaycastHit[]`. [files: physics/src/physics_world.cpp]
- **P1-M3-D1b**: `RaycastHit` struct: entity, position, normal, distance, surface material. [files: physics/include/engine/physics/query.h]
- **P1-M3-D1c**: Broadphase: ray-vs-cell enumeration. Narrow: per-collider ray test. [files: physics/src/broadphase.cpp, physics/src/narrow_phase.cpp]
- **P1-M3-D1d**: Sort results by distance. Option for closest-only (early out). [files: physics/src/physics_world.cpp]
- **P1-M3-D1e**: Lua binding: `engine.raycast(ox,oy,oz, dx,dy,dz, max_dist)` → table of hits. [files: scripting/src/scripting.cpp]
- **P1-M3-D1f**: Write test: ray through 3 aligned spheres returns 3 hits sorted by distance. [files: tests/unit/raycast_test.cpp]

##### P1-M3-D2: Sphere/Box Overlap Query
- **P1-M3-D2a**: Implement `PhysicsWorld::overlap_sphere(center, radius, mask) → Entity[]`. [files: physics/src/physics_world.cpp]
- **P1-M3-D2b**: Implement `PhysicsWorld::overlap_box(center, half_extents, rotation, mask) → Entity[]`. [files: physics/src/physics_world.cpp]
- **P1-M3-D2c**: Lua bindings: `engine.overlap_sphere(cx,cy,cz, radius)`, `engine.overlap_box(...)`. [files: scripting/src/scripting.cpp]
- **P1-M3-D2d**: Write test: 10 entities in a cluster, overlap sphere catches correct subset. [files: tests/unit/overlap_test.cpp]

##### P1-M3-D3: Shape Cast (Sweep)
- **P1-M3-D3a**: Implement `PhysicsWorld::sweep_sphere(origin, radius, direction, distance, mask) → SweepHit`. [files: physics/src/physics_world.cpp]
- **P1-M3-D3b**: Implement `PhysicsWorld::sweep_box(center, half_extents, rotation, direction, distance, mask) → SweepHit`. [files: physics/src/physics_world.cpp]
- **P1-M3-D3c**: `SweepHit` struct: entity, contact_point, normal, distance, time_of_impact. [files: physics/include/engine/physics/query.h]
- **P1-M3-D3d**: Lua bindings: `engine.sweep_sphere(...)`, `engine.sweep_box(...)`. [files: scripting/src/scripting.cpp]
- **P1-M3-D3e**: Write test: sweep sphere through corridor, hits wall at correct distance. [files: tests/unit/sweep_test.cpp]

#### P1-M3-E: Continuous Collision Detection (CCD) Hardening

##### P1-M3-E1: Bilateral Advance
- **P1-M3-E1a**: Replace current CCD with bilateral advancement algorithm (Erwin Coumans GDC style). [files: physics/src/ccd.cpp]
- **P1-M3-E1b**: Support sphere-vs-mesh CCD (sweep + refine). [files: physics/src/ccd.cpp]
- **P1-M3-E1c**: Add CVar `physics.ccd_threshold` — minimum velocity magnitude to trigger CCD. [files: physics/src/rigidbody.cpp]
- **P1-M3-E1d**: Write test: fast bullet vs thin wall — no tunneling at 300 m/s. [files: tests/unit/ccd_test.cpp]

##### P1-M3-E2: Speculative Contacts
- **P1-M3-E2a**: Implement speculative contact generation: expand AABB by velocity * dt, detect contacts before penetration. [files: physics/src/broadphase.cpp, physics/src/narrow_phase.cpp]
- **P1-M3-E2b**: Clamp speculative impulse to prevent ghost collisions. [files: physics/src/constraint_solver.cpp]
- **P1-M3-E2c**: Write test: ball rolling toward wall stops without visible penetration frame. [files: tests/unit/speculative_contacts_test.cpp]

**P1-M3 Exit Criteria**:
- Capsule, convex hull, and heightfield colliders work in all combinations.
- Sequential impulse solver with warm starting converges in ≤10 iterations for stacked boxes.
- All 5 joint types maintain constraint under stress.
- Raycast, overlap, and sweep queries return correct results with layer filtering.
- CCD prevents tunneling for objects up to 300 m/s.
- All features have Lua bindings and unit tests.

---

### P1-M4: Asset Pipeline Production

**Goal**: 64-bit hashing, metadata with tags/thumbnails, async streaming, LRU eviction, dependency graph, and deterministic cooking.

**Dependencies**: P1-M1 (CI), P1-M3 (cooked collision data).

#### P1-M4-A: Asset Database Hardening

##### P1-M4-A1: 64-bit Asset Hashing
- **P1-M4-A1a**: Replace 32-bit FNV hash with 64-bit FNV-1a or xxHash64 for asset IDs. [files: assets includes, tools/asset_packer/]
- **P1-M4-A1b**: Update all `AssetId` typedefs from `uint32_t` to `uint64_t`. Audit all comparisons. [files: runtime/include/engine/runtime/asset_database.h, tools/asset_packer/]
- **P1-M4-A1c**: Migrate existing asset registry files to 64-bit IDs (conversion tool or clean rebuild). [files: tools/asset_packer/]
- **P1-M4-A1d**: Write collision test: hash 100K random asset paths, verify zero collisions. [files: tests/unit/asset_hash_test.cpp]

##### P1-M4-A2: Asset Metadata System
- **P1-M4-A2a**: Define `AssetMetadata` struct: asset_id, type_tag, file_path, file_size, last_modified, checksum, import_settings, tags[]. [files: runtime/include/engine/runtime/asset_metadata.h]
- **P1-M4-A2b**: Store metadata in sidecar `.meta` JSON files alongside source assets. [files: tools/asset_packer/main.cpp]
- **P1-M4-A2c**: Add tag system: `tags: ["environment", "prop", "lod0"]`. Queryable from editor and runtime. [files: runtime/src/asset_database.cpp]
- **P1-M4-A2d**: Add import settings per asset type: mesh (scale, up-axis, gen-normals), texture (format, mip-gen, sRGB). [files: tools/asset_packer/main.cpp]

##### P1-M4-A3: Thumbnail Generation
- **P1-M4-A3a**: After mesh import: render 64×64 thumbnail using offscreen GL context. Store as PNG in `.thumbnails/`. [files: tools/asset_packer/, renderer/src/]
- **P1-M4-A3b**: After texture import: downscale to 64×64, store thumbnail. [files: tools/asset_packer/]
- **P1-M4-A3c**: Load thumbnails in editor asset browser. Fallback to type icon if missing. [files: editor/src/editor.cpp]
- **P1-M4-A3d**: Invalidate thumbnail if source asset changes (mtime check). [files: tools/asset_packer/]

#### P1-M4-B: Dependency Graph

##### P1-M4-B1: Build-Time Dependency Tracking
- **P1-M4-B1a**: Create `DependencyGraph` class: DAG of `AssetId → AssetId` edges (A depends on B). [files: tools/asset_packer/dependency_graph.h, tools/asset_packer/dependency_graph.cpp]
- **P1-M4-B1b**: Populate during import: mesh depends on materials, materials depend on textures, prefabs depend on meshes. [files: tools/asset_packer/main.cpp]
- **P1-M4-B1c**: Persist graph to `build/asset_deps.json`. Incremental rebuild: if B changes, rebuild all assets that depend on B. [files: tools/asset_packer/]
- **P1-M4-B1d**: Write test: change a texture, verify dependent material and mesh are recooked. [files: tests/unit/dependency_graph_test.cpp]

##### P1-M4-B2: Runtime Dependency Awareness
- **P1-M4-B2a**: `AssetDatabase::get_dependencies(asset_id) → AssetId[]` — return all assets this asset requires. [files: runtime/src/asset_database.cpp]
- **P1-M4-B2b**: When loading asset, load dependencies first (recursive). Track load order to prevent cycles. [files: runtime/src/asset_database.cpp]
- **P1-M4-B2c**: Write test: load a prefab, verify its mesh and textures are loaded first. [files: tests/integration/asset_dep_load_test.cpp]

#### P1-M4-C: Async Streaming

##### P1-M4-C1: Background Load Thread
- **P1-M4-C1a**: Create `AssetLoadThread` using job system: separate worker thread pool for IO. [files: runtime/src/asset_streaming.cpp]
- **P1-M4-C1b**: `AssetDatabase::load_async(asset_id) → LoadHandle`. Handle has: `is_ready()`, `get<T>()`, `wait()`. [files: runtime/include/engine/runtime/asset_database.h]
- **P1-M4-C1c**: Loading states: `Queued → Loading → Uploading → Ready` (upload = GPU upload for textures/meshes on main thread). [files: runtime/src/asset_streaming.cpp]
- **P1-M4-C1d**: Write test: queue 50 mesh loads, poll until all ready, verify all loaded correctly. [files: tests/integration/async_load_test.cpp]

##### P1-M4-C2: Priority Queue
- **P1-M4-C2a**: Add priority to load requests: `Immediate` (cutscene asset), `High` (visible on screen), `Normal` (nearby), `Low` (distance preload). [files: runtime/src/asset_streaming.cpp]
- **P1-M4-C2b**: Load thread processes highest priority first. Priority can be updated while queued. [files: runtime/src/asset_streaming.cpp]
- **P1-M4-C2c**: Lua binding: `engine.load_asset_async(path, priority)` → handle. [files: scripting/src/scripting.cpp]

##### P1-M4-C3: Streaming Budget
- **P1-M4-C3a**: CVar `asset.streaming_budget_mb` — maximum memory for in-flight loads per frame. [files: runtime/src/asset_streaming.cpp]
- **P1-M4-C3b**: CVar `asset.max_uploads_per_frame` — limit GPU uploads to N meshes + M textures per frame. [files: runtime/src/asset_streaming.cpp]
- **P1-M4-C3c**: Write test: queue more than budget allows, verify spreading across frames. [files: tests/integration/streaming_budget_test.cpp]

#### P1-M4-D: LRU Eviction Cache

##### P1-M4-D1: LRU Tracking
- **P1-M4-D1a**: Add `last_access_frame` field to every loaded asset entry. Update on access. [files: runtime/src/asset_database.cpp]
- **P1-M4-D1b**: Maintain doubly-linked list sorted by access time (most recent at tail). [files: runtime/src/lru_cache.cpp]
- **P1-M4-D1c**: `evict()` removes head of list (least recently used). Free GPU + CPU memory. [files: runtime/src/lru_cache.cpp]

##### P1-M4-D2: Eviction Policy
- **P1-M4-D2a**: CVar `asset.cache_size_mb` — target cache size. When exceeded, evict until below target. [files: runtime/src/lru_cache.cpp]
- **P1-M4-D2b**: Protected assets: actively referenced assets (ref count > 0) cannot be evicted. Skip in LRU. [files: runtime/src/lru_cache.cpp]
- **P1-M4-D2c**: Eviction callback: before evicting, notify systems (renderer drops GPU handles, physics drops cooked data). [files: runtime/src/lru_cache.cpp]
- **P1-M4-D2d**: Write test: load 100 assets into a 50-asset-sized cache. Access subset repeatedly. Verify LRU evicts the stale ones. [files: tests/unit/lru_cache_test.cpp]

#### P1-M4-E: Deterministic Cooking

##### P1-M4-E1: Byte-Identical Rebuild
- **P1-M4-E1a**: Ensure asset packer produces byte-identical output given identical input (remove timestamps, sort deterministically). [files: tools/asset_packer/main.cpp]
- **P1-M4-E1b**: Use content-hash (not mtime) for rebuild decision when available. [files: tools/asset_packer/main.cpp]
- **P1-M4-E1c**: Write test: cook assets, cook again without changing anything, diff output — must be zero. [files: tests/unit/deterministic_cook_test.cpp]

##### P1-M4-E2: Import Settings Round-Trip
- **P1-M4-E2a**: All import settings stored in `.meta` file. Changing settings re-triggers cook. [files: tools/asset_packer/main.cpp]
- **P1-M4-E2b**: Editor UI: inspector panel for selected asset shows import settings, change triggers recook. [files: editor/src/asset_inspector.cpp]
- **P1-M4-E2c**: Write test: change mesh scale in meta, recook, verify mesh vertices are scaled. [files: tests/unit/import_settings_test.cpp]

**P1-M4 Exit Criteria**:
- Zero hash collisions in 100K path test with 64-bit hashing.
- Asset metadata with tags, thumbnails, import settings.
- Dependency graph detects transitive invalidation and triggers minimal rebuild.
- Async loading with priority, budget, and load states.
- LRU eviction maintains cache within budget, never evicts referenced assets.
- Deterministic cooking: byte-identical rebuild.

---

### P1-M5: Renderer — Deferred Pipeline and Shadows

**Goal**: Replace forward-only rendering with deferred shading, implement shadow maps, screen-space ambient occlusion, and HDR post-processing.

**Dependencies**: P1-M1 (CI, profiling), P1-M4 (asset streaming for textures).

#### P1-M5-A: G-Buffer Pass

##### P1-M5-A1: G-Buffer Layout Design
- **P1-M5-A1a**: Define G-Buffer layout: RT0 = albedo.rgb + metallic.a (RGBA8), RT1 = world normal.xyz + roughness.a (RGBA16F), RT2 = emissive.rgb + AO.a (RGBA8), Depth = depth24_stencil8. [files: renderer/include/engine/renderer/gbuffer.h]
- **P1-M5-A1b**: Create FBO with MRT (multiple render targets) using `glDrawBuffers`. [files: renderer/src/gbuffer.cpp]
- **P1-M5-A1c**: Verify FBO completeness check and error reporting. [files: renderer/src/gbuffer.cpp] [test: FBO status == GL_FRAMEBUFFER_COMPLETE]

##### P1-M5-A2: G-Buffer Shader
- **P1-M5-A2a**: Write `gbuffer.vert` — transform position, pass TBN matrix, UV. [files: assets/shaders/gbuffer.vert]
- **P1-M5-A2b**: Write `gbuffer.frag` — sample albedo/normal/metallic/roughness textures, write to G-Buffer MRT targets. [files: assets/shaders/gbuffer.frag]
- **P1-M5-A2c**: Handle missing textures gracefully: default 1×1 white albedo, flat normal, 0 metallic, 0.5 roughness. [files: renderer/src/material.cpp]

##### P1-M5-A3: G-Buffer Debug Visualization
- **P1-M5-A3a**: CVar `r_gbuffer_debug` (0=off, 1=albedo, 2=normals, 3=metallic, 4=roughness, 5=depth). [files: renderer/src/renderer.cpp]
- **P1-M5-A3b**: Write `gbuffer_debug.frag` — fullscreen quad reading selected G-Buffer target. [files: assets/shaders/gbuffer_debug.frag]
- **P1-M5-A3c**: Render debug view as fullscreen pass when CVar set. [files: renderer/src/renderer.cpp]

#### P1-M5-B: Deferred Lighting Pass

##### P1-M5-B1: Fullscreen Lighting Shader
- **P1-M5-B1a**: Write `deferred_lighting.frag` — reconstruct world position from depth, sample G-Buffer, apply PBR BRDF (Cook-Torrance) for directional light. [files: assets/shaders/deferred_lighting.frag]
- **P1-M5-B1b**: Use existing `fullscreen.vert` to draw fullscreen triangle. [files: renderer/src/renderer.cpp]
- **P1-M5-B1c**: Output to HDR render target (RGBA16F). [files: renderer/src/renderer.cpp]

##### P1-M5-B2: Point Lights
- **P1-M5-B2a**: Define `PointLight` struct: position, color, intensity, radius. SparseSet in World. [files: runtime/include/engine/runtime/world.h]
- **P1-M5-B2b**: Upload point lights to shader as uniform array (max 128 lights). [files: renderer/src/renderer.cpp]
- **P1-M5-B2c**: Deferred shader: iterate lights, apply attenuation (inverse square falloff with radius cutoff), accumulate BRDF result. [files: assets/shaders/deferred_lighting.frag]
- **P1-M5-B2d**: Frustum-cull lights: skip lights whose bounding sphere is outside camera frustum. [files: renderer/src/renderer.cpp]
- **P1-M5-B2e**: Lua binding: `engine.add_point_light(entity, r,g,b, intensity, radius)`. [files: scripting/src/scripting.cpp]

##### P1-M5-B3: Spot Lights
- **P1-M5-B3a**: Define `SpotLight` struct: position, direction, color, intensity, inner_angle, outer_angle, range. [files: runtime/include/engine/runtime/world.h]
- **P1-M5-B3b**: Deferred shader: spot light contribution with cone falloff. [files: assets/shaders/deferred_lighting.frag]
- **P1-M5-B3c**: Frustum-cull spot lights by bounding cone approximation. [files: renderer/src/renderer.cpp]
- **P1-M5-B3d**: Lua binding: `engine.add_spot_light(entity, ...)`. [files: scripting/src/scripting.cpp]

##### P1-M5-B4: Light Culling Optimization
- **P1-M5-B4a**: Implement tiled light culling: divide screen into 16×16 tiles, compute min/max depth per tile. [files: renderer/src/light_culling.cpp]
- **P1-M5-B4b**: Per-tile: test each light sphere against tile frustum. Store per-tile light list. [files: renderer/src/light_culling.cpp]
- **P1-M5-B4c**: Deferred shader reads per-tile light list, only iterates relevant lights. [files: assets/shaders/deferred_lighting.frag]
- **P1-M5-B4d**: Write test: 256 lights scene renders correctly, tile-culled vs brute-force are pixel-identical. [files: tests/unit/light_culling_test.cpp]

#### P1-M5-C: Shadow Mapping

##### P1-M5-C1: Directional Light Cascaded Shadow Maps (CSM)
- **P1-M5-C1a**: Create shadow map FBO: depth-only texture array (4 cascades × 2048×2048). [files: renderer/src/shadow_map.cpp]
- **P1-M5-C1b**: Compute cascade splits: practical split scheme (logarithmic + linear blend). CVar `r_shadow_cascade_count` (2–4). [files: renderer/src/shadow_map.cpp]
- **P1-M5-C1c**: Per cascade: compute light-space orthographic projection enclosing the cascade frustum. [files: renderer/src/shadow_map.cpp]
- **P1-M5-C1d**: Render scene from light's view per cascade (depth-only pass, no color write). [files: renderer/src/shadow_map.cpp]
- **P1-M5-C1e**: Write `shadow_depth.vert` — minimal vertex shader for shadow pass (transform by light MVP). [files: assets/shaders/shadow_depth.vert]
- **P1-M5-C1f**: In deferred lighting: sample shadow map, compare depth, output shadow factor (0 or 1). [files: assets/shaders/deferred_lighting.frag]
- **P1-M5-C1g**: Add PCF (percentage closest filtering): 3×3 kernel for soft shadow edges. [files: assets/shaders/deferred_lighting.frag]
- **P1-M5-C1h**: CVar `r_shadow_resolution` (512, 1024, 2048, 4096). [files: renderer/src/shadow_map.cpp]

##### P1-M5-C2: Spot Light Shadow Map
- **P1-M5-C2a**: Per shadow-casting spot light: allocate one shadow map slice (perspective projection). [files: renderer/src/shadow_map.cpp]
- **P1-M5-C2b**: Render from spot light's view. Sample in deferred shader. [files: renderer/src/shadow_map.cpp, assets/shaders/deferred_lighting.frag]
- **P1-M5-C2c**: CVar `r_max_shadow_casting_lights` limits how many spot lights cast shadows (default 4). [files: renderer/src/shadow_map.cpp]

##### P1-M5-C3: Point Light Shadow Map (Omnidirectional)
- **P1-M5-C3a**: Implement cubemap shadow map for point lights: 6-face FBO per light. [files: renderer/src/shadow_map.cpp]
- **P1-M5-C3b**: Geometry shader or multi-pass to render all 6 faces. [files: assets/shaders/shadow_cube.geom or renderer/src/shadow_map.cpp]
- **P1-M5-C3c**: Sample cubemap in deferred shader using world direction to light. [files: assets/shaders/deferred_lighting.frag]
- **P1-M5-C3d**: Limit point light shadows to N closest lights (CVar `r_max_point_shadows`, default 2). [files: renderer/src/shadow_map.cpp]

##### P1-M5-C4: Shadow Optimization
- **P1-M5-C4a**: Implement stable cascades: snap projection to texel grid to eliminate shimmer. [files: renderer/src/shadow_map.cpp]
- **P1-M5-C4b**: Implement shadow cache: re-render cascade only if camera moves significantly or light angle changes. [files: renderer/src/shadow_map.cpp]
- **P1-M5-C4c**: Implement shadow LOD: distant cascades use lower resolution. [files: renderer/src/shadow_map.cpp]
- **P1-M5-C4d**: Write visual test: rotating light source with shadow-receiving plane. No shimmer, no peter-panning. [files: tests/smoke/shadow_visual_test.cpp]

#### P1-M5-D: HDR Post-Processing Pipeline

##### P1-M5-D1: Post-Process Stack Architecture
- **P1-M5-D1a**: Create `PostProcessStack` class: ordered list of post-process passes, each with enable/disable. [files: renderer/include/engine/renderer/post_process.h, renderer/src/post_process.cpp]
- **P1-M5-D1b**: Each pass: takes input RT, produces output RT. Ping-pong between two HDR RTs. [files: renderer/src/post_process.cpp]
- **P1-M5-D1c**: CVar per pass: `r_bloom_enabled`, `r_ssao_enabled`, `r_fxaa_enabled`, etc. [files: renderer/src/post_process.cpp]

##### P1-M5-D2: Bloom
- **P1-M5-D2a**: Brightness filter pass: extract pixels above `r_bloom_threshold` (default 1.0) into a separate RT. [files: assets/shaders/bloom_threshold.frag, renderer/src/post_process.cpp]
- **P1-M5-D2b**: Downsample chain: 5 levels of half-res, each a 13-tap filter (dual kawase or gaussian). [files: assets/shaders/bloom_downsample.frag]
- **P1-M5-D2c**: Upsample chain: bilinear + tent filter back up to full res. [files: assets/shaders/bloom_upsample.frag]
- **P1-M5-D2d**: Composite: additive blend bloom result into HDR scene RT, weighted by `r_bloom_intensity`. [files: assets/shaders/bloom_composite.frag]
- **P1-M5-D2e**: Write test: render bright sphere on dark background, verify bloom glow in output. [files: tests/smoke/bloom_test.cpp]

##### P1-M5-D3: Screen-Space Ambient Occlusion (SSAO)
- **P1-M5-D3a**: Implement SSAO: sample hemisphere around each pixel's world-space normal, compare depths. [files: assets/shaders/ssao.frag]
- **P1-M5-D3b**: Generate random kernel (64 samples) and 4×4 noise texture for rotation. [files: renderer/src/ssao.cpp]
- **P1-M5-D3c**: Bilateral blur pass to remove noise (preserve edges using normal/depth comparison). [files: assets/shaders/ssao_blur.frag]
- **P1-M5-D3d**: Multiply SSAO result into ambient lighting term in deferred shader. [files: assets/shaders/deferred_lighting.frag]
- **P1-M5-D3e**: CVars: `r_ssao_radius`, `r_ssao_samples`, `r_ssao_intensity`. [files: renderer/src/ssao.cpp]

##### P1-M5-D4: Tone Mapping Enhancement
- **P1-M5-D4a**: Add multiple tone map operators: Reinhard, ACES Filmic, Uncharted 2. Selectable via CVar `r_tonemap_operator`. [files: assets/shaders/tonemap.frag]
- **P1-M5-D4b**: Add auto-exposure: compute average luminance via downsampling, adapt exposure over time. [files: renderer/src/auto_exposure.cpp, assets/shaders/luminance.frag]
- **P1-M5-D4c**: CVar `r_exposure` for manual override. `r_auto_exposure` toggle. [files: renderer/src/auto_exposure.cpp]

##### P1-M5-D5: Anti-Aliasing
- **P1-M5-D5a**: Implement FXAA 3.11 as post-process pass. [files: assets/shaders/fxaa.frag]
- **P1-M5-D5b**: CVar `r_aa_mode` (0=none, 1=FXAA). TAA deferred to P2. [files: renderer/src/post_process.cpp]
- **P1-M5-D5c**: Write test: render aliased edge, verify FXAA smooths pixel-level jaggies. [files: tests/smoke/fxaa_test.cpp]

#### P1-M5-E: Forward Transparency Pass

##### P1-M5-E1: Transparent Object Sorting
- **P1-M5-E1a**: After deferred lighting, render transparent objects in a forward pass. [files: renderer/src/renderer.cpp]
- **P1-M5-E1b**: Sort transparent draw calls back-to-front by camera distance. [files: renderer/src/renderer.cpp]
- **P1-M5-E1c**: Alpha blend into HDR scene RT. Depth write OFF, depth test ON. [files: renderer/src/renderer.cpp]

##### P1-M5-E2: Transparent PBR Shader
- **P1-M5-E2a**: Write `forward_transparent.frag` — same PBR BRDF as deferred, but single-pass with light array. [files: assets/shaders/forward_transparent.frag]
- **P1-M5-E2b**: Sample shadow maps in forward pass for transparent shadows. [files: assets/shaders/forward_transparent.frag]
- **P1-M5-E2c**: Lua binding: `engine.set_material_transparent(entity, alpha)`. [files: scripting/src/scripting.cpp]

**P1-M5 Exit Criteria**:
- Deferred shading with G-Buffer debug visualization.
- Point, spot, and directional lights with correct PBR BRDF.
- Cascaded shadow maps with PCF soft shadows, no shimmer.
- Bloom, SSAO, tone mapping (3 operators), auto-exposure, FXAA functional.
- Transparent objects render correctly in forward pass with shadows.
- GPU profiler shows pass timings for all deferred/shadow/post-process passes.
- 256 lights + shadows at 60 fps (1080p).

---

### P1-M6: Renderer — Sky, Fog, Instancing, and Materials

**Goal**: Skybox/procedural sky, distance fog, GPU instancing, material/shader variant system, and render-to-texture.

**Dependencies**: P1-M5 (deferred pipeline).

#### P1-M6-A: Sky and Atmosphere

##### P1-M6-A1: Skybox (Cubemap)
- **P1-M6-A1a**: Define `SkyboxComponent`: cubemap texture handle (6-face or equirect). [files: renderer/include/engine/renderer/skybox.h]
- **P1-M6-A1b**: Write `skybox.vert` — centered unit cube, view matrix with translation removed. [files: assets/shaders/skybox.vert]
- **P1-M6-A1c**: Write `skybox.frag` — sample cubemap by vertex direction. [files: assets/shaders/skybox.frag]
- **P1-M6-A1d**: Render skybox as last opaque pass, depth test set to LE, depth write OFF. [files: renderer/src/renderer.cpp]
- **P1-M6-A1e**: Asset import: load HDR equirect → convert to cubemap at import time. [files: tools/asset_packer/]
- **P1-M6-A1f**: Lua binding: `engine.set_skybox(cubemap_path)`. [files: scripting/src/scripting.cpp]

##### P1-M6-A2: Procedural Sky (Preetham/Hosek-Wilkie)
- **P1-M6-A2a**: Implement Preetham sky model (sun direction → sky color gradient). [files: assets/shaders/procedural_sky.frag]
- **P1-M6-A2b**: Parameters: sun_direction, turbidity, ground_albedo. CVar or Lua settable. [files: renderer/src/sky.cpp]
- **P1-M6-A2c**: Render on fullscreen dome or inverted sphere. [files: renderer/src/sky.cpp]
- **P1-M6-A2d**: Integrate with lighting: sky color influences ambient light. [files: renderer/src/renderer.cpp]
- **P1-M6-A2e**: Lua binding: `engine.set_procedural_sky(turbidity, sun_dir_x, sun_dir_y, sun_dir_z)`. [files: scripting/src/scripting.cpp]

##### P1-M6-A3: Environment Reflection Probes
- **P1-M6-A3a**: Capture environment cubemap from probe position (6-face render). [files: renderer/src/env_probe.cpp]
- **P1-M6-A3b**: Prefilter cubemap for roughness levels (split-sum IBL). [files: renderer/src/env_probe.cpp, assets/shaders/prefilter.frag]
- **P1-M6-A3c**: Generate BRDF LUT texture (Schlick-GGX integration). [files: renderer/src/env_probe.cpp, assets/shaders/brdf_lut.frag]
- **P1-M6-A3d**: Sample prefiltered environment map in deferred lighting for specular IBL. [files: assets/shaders/deferred_lighting.frag]
- **P1-M6-A3e**: Multiple probes: blend by proximity or volume. [files: renderer/src/env_probe.cpp]
- **P1-M6-A3f**: Editor: place probe, capture on demand, show proxy sphere. [files: editor/src/editor.cpp]

#### P1-M6-B: Environment Fog

##### P1-M6-B1: Distance Fog
- **P1-M6-B1a**: Post-process pass: compute linear distance from camera, blend scene color toward fog color. [files: assets/shaders/fog.frag]
- **P1-M6-B1b**: Support exponential and exponential-squared falloff modes. [files: assets/shaders/fog.frag]
- **P1-M6-B1c**: CVars: `r_fog_enabled`, `r_fog_color`, `r_fog_density`, `r_fog_start`, `r_fog_end`, `r_fog_mode` (linear/exp/exp2). [files: renderer/src/post_process.cpp]

##### P1-M6-B2: Height Fog
- **P1-M6-B2a**: Add height-based density falloff: density peaks at `r_fog_height_base`, decays exponentially upward. [files: assets/shaders/fog.frag]
- **P1-M6-B2b**: Ray-march from camera to pixel to integrate density along view ray. [files: assets/shaders/fog.frag]
- **P1-M6-B2c**: Lua binding: `engine.set_fog(density, start, end, color_r, color_g, color_b, height_base)`. [files: scripting/src/scripting.cpp]

#### P1-M6-C: GPU Instancing

##### P1-M6-C1: Static Mesh Instancing
- **P1-M6-C1a**: Group draw calls by mesh+material. For groups with >1 instance, use `glDrawElementsInstanced`. [files: renderer/src/renderer.cpp]
- **P1-M6-C1b**: Upload instance data (model matrices) via SSBO or instance VBO attribute. [files: renderer/src/renderer.cpp]
- **P1-M6-C1c**: Modify vertex shader to read per-instance model matrix. [files: assets/shaders/gbuffer.vert]
- **P1-M6-C1d**: Frustum cull individual instances before upload (CPU-side). [files: renderer/src/renderer.cpp]
- **P1-M6-C1e**: Write test: render 10K identical boxes, verify draw call count = 1 (instanced). Screen-compare output. [files: tests/smoke/instancing_test.cpp]

##### P1-M6-C2: Foliage/Vegetation Instancing
- **P1-M6-C2a**: Special foliage instance buffer: model matrix + wind params (phase, amplitude). [files: renderer/src/foliage_renderer.cpp]
- **P1-M6-C2b**: Foliage vertex shader: apply vertex-displacement wind animation using sin(time + phase). [files: assets/shaders/foliage.vert]
- **P1-M6-C2c**: LOD selection per instance based on distance to camera (swap mesh at thresholds). [files: renderer/src/foliage_renderer.cpp]
- **P1-M6-C2d**: Lua binding: `engine.add_foliage_instance(mesh, material, position, scale)`. [files: scripting/src/scripting.cpp]

#### P1-M6-D: Material and Shader Variant System

##### P1-M6-D1: Shader Permutation Manager
- **P1-M6-D1a**: Define macro-based shader variant keys: `HAS_NORMAL_MAP`, `HAS_EMISSIVE`, `HAS_VERTEX_COLOR`, `ALPHA_TEST`, `SKINNED`. [files: renderer/include/engine/renderer/shader_variants.h]
- **P1-M6-D1b**: At material load: compute variant bitmask from material features. [files: renderer/src/material.cpp]
- **P1-M6-D1c**: Compile variant on first request: `#define` active features, compile GL program, cache in hash map (bitmask → program). [files: renderer/src/shader_variants.cpp]
- **P1-M6-D1d**: Warm cache at load time for known material variants (avoid runtime shader stall). [files: renderer/src/shader_variants.cpp]
- **P1-M6-D1e**: Write test: material with normal map uses different shader than material without. Both render correctly. [files: tests/unit/shader_variant_test.cpp]

##### P1-M6-D2: Material Instance System
- **P1-M6-D2a**: Define `MaterialTemplate` — base shader + default parameters. [files: renderer/include/engine/renderer/material.h]
- **P1-M6-D2b**: Define `MaterialInstance` — references template, overrides specific parameters (tint color, roughness multiplier). [files: renderer/include/engine/renderer/material.h]
- **P1-M6-D2c**: Instances share compiled shader program with template, only differ in uniform values. [files: renderer/src/material.cpp]
- **P1-M6-D2d**: Lua binding: `engine.create_material_instance(template_path, overrides_table)`. [files: scripting/src/scripting.cpp]
- **P1-M6-D2e**: Editor: material inspector shows template + per-instance overrides with color pickers and sliders. [files: editor/src/material_editor.cpp]

#### P1-M6-E: Render-to-Texture and Scene Capture

##### P1-M6-E1: Render Target Management
- **P1-M6-E1a**: Create `RenderTarget` class: wraps FBO + color/depth attachments, configurable resolution and format. [files: renderer/include/engine/renderer/render_target.h, renderer/src/render_target.cpp]
- **P1-M6-E1b**: Pool of render targets by resolution bucket to avoid excessive FBO creation. [files: renderer/src/render_target.cpp]

##### P1-M6-E2: Scene Capture Component
- **P1-M6-E2a**: Define `SceneCaptureComponent`: camera parameters, target render target, capture frequency (every frame / on demand). [files: renderer/include/engine/renderer/scene_capture.h]
- **P1-M6-E2b**: On capture: render scene from capture camera into RT using full deferred pipeline. [files: renderer/src/scene_capture.cpp]
- **P1-M6-E2c**: Expose captured texture for sampling on materials (mirrors, security cameras, minimap). [files: renderer/src/scene_capture.cpp]
- **P1-M6-E2d**: Lua binding: `engine.create_scene_capture(entity, width, height)`, `engine.capture_now(entity)`. [files: scripting/src/scripting.cpp]
- **P1-M6-E2e**: Write test: scene capture produces non-black texture with correct dimensions. [files: tests/smoke/scene_capture_test.cpp]

**P1-M6 Exit Criteria**:
- Skybox and procedural sky render correctly, sky influences ambient light.
- Distance and height fog with configurable parameters.
- GPU instancing reduces draw calls by 10× for repeated geometry.
- Shader variant system compiles and caches permutations. No runtime stalls.
- Material instances share shaders, differ only in parameters.
- Scene capture renders to texture usable by other materials.
- All features have Lua bindings.

---

### P1-M7: Animation System

**Goal**: Skeleton loading, skinned mesh rendering, blend trees, animation state machine, root motion, IK, montages, and Lua API.

**Dependencies**: P1-M4 (asset pipeline for animation data), P1-M5 (renderer for skinned rendering).

#### P1-M7-A: Skeleton and Clip Data

##### P1-M7-A1: Skeleton Loading
- **P1-M7-A1a**: Define `Skeleton` struct: array of joints (name, parent_index, inverse_bind_pose Mat4). Max 256 joints. [files: runtime/include/engine/runtime/skeleton.h]
- **P1-M7-A1b**: Load skeleton from glTF skin data in asset packer: extract joint hierarchy and inverse bind matrices. [files: tools/asset_packer/main.cpp]
- **P1-M7-A1c**: Store cooked skeleton as binary blob (joint count + flat arrays). [files: tools/asset_packer/main.cpp]
- **P1-M7-A1d**: Runtime loader: read skeleton binary, populate Skeleton struct. [files: runtime/src/animation_loader.cpp]

##### P1-M7-A2: Animation Clip Loading
- **P1-M7-A2a**: Define `AnimationClip` struct: duration, sample_rate, array of channels. Each channel: joint_index, array of keyframes (time, value). Separate tracks for position (Vec3), rotation (Quat), scale (Vec3). [files: runtime/include/engine/runtime/animation_clip.h]
- **P1-M7-A2b**: Load from glTF animation data: extract channels per joint per clip. [files: tools/asset_packer/main.cpp]
- **P1-M7-A2c**: Cook to compressed binary: quantize rotations (smallest-3 quaternion), delta-encode positions. [files: tools/asset_packer/anim_compress.cpp]
- **P1-M7-A2d**: Runtime loader: decompress clip into playback-ready format. [files: runtime/src/animation_loader.cpp]
- **P1-M7-A2e**: Write test: roundtrip compress/decompress, verify error < 0.001 per joint. [files: tests/unit/anim_compress_test.cpp]

##### P1-M7-A3: Clip Sampling and Interpolation
- **P1-M7-A3a**: Implement `sample_clip(clip, time) → JointPose[]`: for each channel, binary search keyframes, lerp position/scale, slerp rotation. [files: runtime/src/animation_sampler.cpp]
- **P1-M7-A3b**: Handle looping: wrap time past duration. [files: runtime/src/animation_sampler.cpp]
- **P1-M7-A3c**: Handle non-looping: clamp at last frame. [files: runtime/src/animation_sampler.cpp]

#### P1-M7-B: Pose Blending

##### P1-M7-B1: Two-Clip Blend
- **P1-M7-B1a**: `blend_poses(poseA, poseB, alpha) → blended_pose`: lerp position/scale, slerp rotation per joint. [files: runtime/src/animation_blending.cpp]
- **P1-M7-B1b**: Use for crossfade transitions. [files: runtime/src/animation_blending.cpp]

##### P1-M7-B2: Blend Tree
- **P1-M7-B2a**: Define `BlendTreeNode` — tree of blend operations: Clip leaf, Lerp2 (2 children + weight), Lerp3 (3 children + 2D weight). [files: runtime/include/engine/runtime/blend_tree.h]
- **P1-M7-B2b**: Evaluate tree bottom-up: leaf samples clip, internal nodes blend children. [files: runtime/src/blend_tree.cpp]
- **P1-M7-B2c**: Implement 1D blend space: N clips arranged along parameter axis, auto-weight by parameter value. [files: runtime/src/blend_tree.cpp]
- **P1-M7-B2d**: Implement 2D blend space: triangulated clip positions in 2D parameter space (e.g., speed/direction). [files: runtime/src/blend_tree.cpp]

##### P1-M7-B3: Additive Blending
- **P1-M7-B3a**: Compute additive pose: `additive = clip_pose - reference_pose` (subtract base from clip). [files: runtime/src/animation_blending.cpp]
- **P1-M7-B3b**: Apply additive: `result = base_pose + additive * weight`. [files: runtime/src/animation_blending.cpp]
- **P1-M7-B3c**: Use for layered animations (lean, aim offset). [files: runtime/src/animation_blending.cpp]

##### P1-M7-B4: Masked/Layered Blending
- **P1-M7-B4a**: Define `BoneMask`: per-joint weight (0.0–1.0). Up to 4 named masks (full_body, upper_body, lower_body, face). [files: runtime/include/engine/runtime/bone_mask.h]
- **P1-M7-B4b**: Apply mask during blend: `final_joint = lerp(base_joint, overlay_joint, mask_weight[joint])`. [files: runtime/src/animation_blending.cpp]
- **P1-M7-B4c**: Use for playing a shoot animation on upper body while legs run. [files: runtime/src/animation_blending.cpp]

#### P1-M7-C: Animation State Machine

##### P1-M7-C1: State and Transition Definitions
- **P1-M7-C1a**: Define `AnimState` struct: name, blend tree (or single clip), looping flag, playback speed. [files: runtime/include/engine/runtime/anim_state_machine.h]
- **P1-M7-C1b**: Define `AnimTransition` struct: from_state, to_state, condition (parameter comparison), crossfade_duration, blend_curve. [files: runtime/include/engine/runtime/anim_state_machine.h]
- **P1-M7-C1c**: Define `AnimStateMachine`: array of states, array of transitions, parameter table (float/bool/int by name). [files: runtime/include/engine/runtime/anim_state_machine.h]

##### P1-M7-C2: State Machine Evaluation
- **P1-M7-C2a**: Each frame: check current state's transitions in priority order. If condition met, begin crossfade. [files: runtime/src/anim_state_machine.cpp]
- **P1-M7-C2b**: During crossfade: blend outgoing state pose with incoming state pose using crossfade progress. [files: runtime/src/anim_state_machine.cpp]
- **P1-M7-C2c**: After crossfade complete: fully switch to new state. [files: runtime/src/anim_state_machine.cpp]
- **P1-M7-C2d**: Support any-state transitions (transitions that trigger from any state). [files: runtime/src/anim_state_machine.cpp]

##### P1-M7-C3: Lua Animation API
- **P1-M7-C3a**: `engine.anim_set_param(entity, "speed", 5.0)`, `engine.anim_set_param(entity, "grounded", true)`. [files: scripting/src/scripting.cpp]
- **P1-M7-C3b**: `engine.anim_play(entity, "attack")` — force-transition to named state. [files: scripting/src/scripting.cpp]
- **P1-M7-C3c**: `engine.anim_get_current_state(entity) → state_name`. [files: scripting/src/scripting.cpp]
- **P1-M7-C3d**: Write test: set parameter, verify state machine transitions, verify correct pose applied. [files: tests/integration/anim_state_machine_test.cpp]

#### P1-M7-D: Root Motion

##### P1-M7-D1: Root Motion Extraction
- **P1-M7-D1a**: Identify root bone (joint_index 0 or explicit flag). Extract delta position/rotation per frame from root channel. [files: runtime/src/root_motion.cpp]
- **P1-M7-D1b**: Remove root motion from animation pose (zero root bone displacement). [files: runtime/src/root_motion.cpp]
- **P1-M7-D1c**: Apply root motion delta to entity transform (via physics/character controller or direct transform). [files: runtime/src/root_motion.cpp]
- **P1-M7-D1d**: Lua binding: `engine.enable_root_motion(entity, true/false)`. [files: scripting/src/scripting.cpp]

#### P1-M7-E: Animation Events / Notifies

##### P1-M7-E1: Event Definition
- **P1-M7-E1a**: Define `AnimEvent` struct: time (normalized 0–1), name, optional payload (string/float). [files: runtime/include/engine/runtime/animation_clip.h]
- **P1-M7-E1b**: Store events per clip (authored in editor or imported from glTF extras). [files: tools/asset_packer/main.cpp]

##### P1-M7-E2: Event Dispatch
- **P1-M7-E2a**: During clip sampling, detect when playback crosses an event time (forward or backward scrub). [files: runtime/src/animation_sampler.cpp]
- **P1-M7-E2b**: Fire event callback: C++ delegate and/or Lua callback. [files: runtime/src/anim_event_dispatcher.cpp]
- **P1-M7-E2c**: Lua binding: `engine.on_anim_event(entity, "footstep", function(payload) ... end)`. [files: scripting/src/scripting.cpp]
- **P1-M7-E2d**: Write test: play clip with event at t=0.5, verify callback fires exactly once at correct frame. [files: tests/unit/anim_event_test.cpp]

#### P1-M7-F: Montages (One-Shot Overlays)

##### P1-M7-F1: Montage Playback
- **P1-M7-F1a**: Define `AnimMontage`: a clip (or sequence of sections) that plays once, blending in/out over the state machine output. [files: runtime/include/engine/runtime/anim_montage.h]
- **P1-M7-F1b**: Blend montage on top of state machine output using bone mask and blend weight. [files: runtime/src/anim_montage.cpp]
- **P1-M7-F1c**: Support montage sections: named time ranges within a clip (e.g., "windup", "swing", "recovery"). [files: runtime/src/anim_montage.cpp]
- **P1-M7-F1d**: Lua binding: `engine.play_montage(entity, montage_name, slot)`, `engine.stop_montage(entity, slot)`. [files: scripting/src/scripting.cpp]

#### P1-M7-G: Skinned Mesh Rendering

##### P1-M7-G1: Bone Matrix Upload
- **P1-M7-G1a**: Compute final bone matrices: `bone_matrix[i] = joint_world_transform[i] * inverse_bind_pose[i]`. [files: runtime/src/skinning.cpp]
- **P1-M7-G1b**: Upload bone matrices as uniform array (max 256 mat4) or SSBO. [files: renderer/src/renderer.cpp]

##### P1-M7-G2: Skinning Shader
- **P1-M7-G2a**: Write `skinned.vert` — read bone indices and weights from vertex attributes, transform position/normal by blend of bone matrices. [files: assets/shaders/skinned.vert]
- **P1-M7-G2b**: G-Buffer shader variant: `SKINNED` flag activates bone transform in vertex stage. [files: assets/shaders/gbuffer.vert]
- **P1-M7-G2c**: Support 4 bone influences per vertex (standard). [files: assets/shaders/skinned.vert]

##### P1-M7-G3: Inverse Kinematics (Two-Bone IK)
- **P1-M7-G3a**: Implement two-bone IK solver: given target position, solve joint angles for 2-bone chain (e.g., arm, leg). [files: runtime/src/ik_solver.cpp]
- **P1-M7-G3b**: Apply IK result after animation pose, before bone matrix computation. [files: runtime/src/skinning.cpp]
- **P1-M7-G3c**: Lua binding: `engine.set_ik_target(entity, chain_name, target_x, target_y, target_z)`. [files: scripting/src/scripting.cpp]
- **P1-M7-G3d**: Use case: foot placement on uneven terrain — raycast down, set IK target to ground. [files: runtime/src/ik_solver.cpp]

**P1-M7 Exit Criteria**:
- Skeleton + clip loaded from glTF, compressed, decompressed with < 0.001 error.
- Blend tree (1D/2D blend spaces), additive, masked blending all work.
- Animation state machine transitions on parameter changes with crossfade.
- Root motion drives entity transform.
- Animation events fire Lua callbacks.
- Montages overlay the state machine output.
- Skinned mesh renders with up to 256 bones.
- Two-bone IK solves correctly for foot/hand placement.
- All features have Lua bindings and tests.

---

### P1-M8: Audio Production

**Goal**: 3D positional audio, mixer/bus system, DSP effects, audio events, streaming, and Lua API.

**Dependencies**: P1-M1 (profiling), P1-M2 (lifecycle for audio components).

#### P1-M8-A: 3D Positional Audio

##### P1-M8-A1: Listener and Emitter
- **P1-M8-A1a**: Define `AudioListener` struct: position, forward, up vectors. One per world (attached to active camera). [files: audio/include/engine/audio/audio.h]
- **P1-M8-A1b**: Define `AudioEmitter` component: position, source handle, min_distance, max_distance, rolloff_factor. SparseSet in World. [files: runtime/include/engine/runtime/world.h, audio/include/engine/audio/audio.h]
- **P1-M8-A1c**: Update listener from camera transform each frame. [files: audio/src/audio.cpp]
- **P1-M8-A1d**: miniaudio spatialization: set position/direction on `ma_sound`, configure distance model (inverse, linear, exponential). [files: audio/src/audio.cpp]

##### P1-M8-A2: Distance Attenuation
- **P1-M8-A2a**: Implement inverse distance clamped model (OpenAL-style). [files: audio/src/audio.cpp]
- **P1-M8-A2b**: Add configurable rolloff curves per emitter. [files: audio/src/audio.cpp]
- **P1-M8-A2c**: Write test: play sound at increasing distance, verify volume decreases according to curve. [files: tests/unit/audio_attenuation_test.cpp]

##### P1-M8-A3: HRTF / Binaural
- **P1-M8-A3a**: Enable miniaudio HRTF (if supported) via CVar `audio.hrtf_enabled`. [files: audio/src/audio.cpp]
- **P1-M8-A3b**: Fallback to simple panning if HRTF unavailable. [files: audio/src/audio.cpp]

#### P1-M8-B: Mixer / Bus System

##### P1-M8-B1: Audio Bus Architecture
- **P1-M8-B1a**: Define `AudioBus` struct: name, volume, muted flag, parent bus pointer, child bus list. [files: audio/include/engine/audio/audio_bus.h]
- **P1-M8-B1b**: Create default bus hierarchy: Master → {Music, SFX → {Dialogue, Environment, UI}}. [files: audio/src/audio_bus.cpp]
- **P1-M8-B1c**: Each sound plays on a specific bus. Final volume = sound volume × bus volume chain to master. [files: audio/src/audio.cpp]
- **P1-M8-B1d**: Lua binding: `engine.set_bus_volume("SFX", 0.8)`, `engine.mute_bus("Music", true)`. [files: scripting/src/scripting.cpp]

##### P1-M8-B2: Snapshots / Ducking
- **P1-M8-B2a**: Define `AudioSnapshot`: named set of bus volume overrides. Blend to snapshot over time. [files: audio/include/engine/audio/audio_bus.h]
- **P1-M8-B2b**: Use for ducking (dialogue plays → lower music), pause menus (muffle SFX), underwater effects. [files: audio/src/audio_bus.cpp]
- **P1-M8-B2c**: Lua binding: `engine.apply_audio_snapshot("underwater", blend_time)`. [files: scripting/src/scripting.cpp]

#### P1-M8-C: DSP Effects

##### P1-M8-C1: Reverb
- **P1-M8-C1a**: Integrate miniaudio reverb node or implement simple Schroeder reverb (4 comb filters + 2 all-pass). [files: audio/src/dsp_effects.cpp]
- **P1-M8-C1b**: Per-bus or per-emitter reverb. Parameters: room_size, damping, wet_level. [files: audio/src/dsp_effects.cpp]
- **P1-M8-C1c**: Reverb zones: spatial volumes that activate reverb when listener enters. [files: audio/src/reverb_zone.cpp]

##### P1-M8-C2: Low-Pass / High-Pass Filter
- **P1-M8-C2a**: Implement biquad low-pass filter on audio bus. Parameters: cutoff_frequency, Q. [files: audio/src/dsp_effects.cpp]
- **P1-M8-C2b**: Use for occlusion simulation (wall between listener and emitter → low-pass). [files: audio/src/audio.cpp]
- **P1-M8-C2c**: Lua binding: `engine.set_bus_lowpass("SFX", cutoff_hz)`. [files: scripting/src/scripting.cpp]

##### P1-M8-C3: Pitch Shifting and Doppler
- **P1-M8-C3a**: Pitch shift per emitter (already basic via miniaudio pitch). [files: audio/src/audio.cpp]
- **P1-M8-C3b**: Doppler effect: compute relative velocity between listener and emitter, adjust pitch accordingly. [files: audio/src/audio.cpp]
- **P1-M8-C3c**: CVar `audio.doppler_factor` (scale effect, 0 = off, 1 = realistic). [files: audio/src/audio.cpp]

#### P1-M8-D: Audio Events

##### P1-M8-D1: Event-Based Playback
- **P1-M8-D1a**: Define `AudioEvent`: name, array of candidate sound assets, random selection mode, volume/pitch randomization ranges, cooldown. [files: audio/include/engine/audio/audio_event.h]
- **P1-M8-D1b**: `play_event(event_name, position)` — picks random sound from candidates, applies randomization, plays. [files: audio/src/audio_event.cpp]
- **P1-M8-D1c**: Store audio events in JSON definition files. Load at startup. [files: audio/src/audio_event.cpp]
- **P1-M8-D1d**: Lua binding: `engine.play_audio_event("footstep_concrete", x, y, z)`. [files: scripting/src/scripting.cpp]

##### P1-M8-D2: Music System
- **P1-M8-D2a**: Music tracks: crossfade between tracks with configurable fade time. [files: audio/src/music_manager.cpp]
- **P1-M8-D2b**: Music layers: multiple stems playing simultaneously (drums, melody, bass), independently volume-controllable. [files: audio/src/music_manager.cpp]
- **P1-M8-D2c**: Lua binding: `engine.play_music("boss_theme", fade_in_time)`, `engine.set_music_layer("drums", volume)`. [files: scripting/src/scripting.cpp]

#### P1-M8-E: Audio Streaming

##### P1-M8-E1: Stream Large Files
- **P1-M8-E1a**: Use miniaudio decoder streaming for files > 1MB (don't decode entire file into memory). [files: audio/src/audio.cpp]
- **P1-M8-E1b**: Configure stream buffer size via CVar `audio.stream_buffer_size_kb`. [files: audio/src/audio.cpp]
- **P1-M8-E1c**: Music should always stream. SFX should decompress to memory. [files: audio/src/audio.cpp]

**P1-M8 Exit Criteria**:
- 3D positional audio with distance attenuation, panning, and optional HRTF.
- Mixer bus hierarchy with per-bus volume, muting, snapshots/ducking.
- Reverb zones, low-pass/high-pass, Doppler effect.
- Audio events with randomization and cooldown.
- Music crossfade and layered stems.
- Large files stream from disk without full decode.
- All features have Lua bindings and tests.

---

### P1-M9: Editor Production

**Goal**: Full property inspector, multi-command undo/redo, asset browser, prefab system, editor scripting API, and play-in-editor polish.

**Dependencies**: P1-M1 (debug tools), P1-M2 (lifecycle), P1-M4 (asset metadata/thumbnails).

#### P1-M9-A: Property Inspector

##### P1-M9-A1: Reflection-Driven Inspector
- **P1-M9-A1a**: Create `PropertyInspector` class: given an entity, iterate its registered components (via reflection), render ImGui widgets per field. [files: editor/src/property_inspector.cpp]
- **P1-M9-A1b**: Widget factory: float→DragFloat, Vec3→DragFloat3, bool→Checkbox, enum→Combo, Color→ColorEdit4. [files: editor/src/property_inspector.cpp]
- **P1-M9-A1c**: Support nested structs: expand inline with tree nodes. [files: editor/src/property_inspector.cpp]
- **P1-M9-A1d**: Support arrays/lists: show count, per-element editors, add/remove buttons. [files: editor/src/property_inspector.cpp]
- **P1-M9-A1e**: Every property edit generates an undo command (see P1-M9-B). [files: editor/src/property_inspector.cpp]

##### P1-M9-A2: Component Add/Remove
- **P1-M9-A2a**: "Add Component" dropdown: list all registered component types. Add selected component with default values. [files: editor/src/property_inspector.cpp]
- **P1-M9-A2b**: Per-component "Remove" button with confirmation. [files: editor/src/property_inspector.cpp]
- **P1-M9-A2c**: Both operations generate undo commands. [files: editor/src/property_inspector.cpp]

#### P1-M9-B: Undo/Redo System

##### P1-M9-B1: Command Pattern Architecture
- **P1-M9-B1a**: Define `EditorCommand` interface: `execute()`, `undo()`, `description()`. [files: editor/include/engine/editor/undo_system.h]
- **P1-M9-B1b**: Define `UndoSystem`: two stacks (undo, redo). Execute pushes to undo, clears redo. Undo pops and reverts. Redo re-executes. [files: editor/src/undo_system.cpp]
- **P1-M9-B1c**: Command merging: consecutive same-property edits within 0.5s merge into single command. [files: editor/src/undo_system.cpp]
- **P1-M9-B1d**: Undo history limit: cap at 100 commands, oldest discarded. [files: editor/src/undo_system.cpp]

##### P1-M9-B2: Command Types
- **P1-M9-B2a**: `TransformCommand` — stores before/after Transform. Already partially exists; verify it uses UndoSystem. [files: editor/src/undo_system.cpp]
- **P1-M9-B2b**: `PropertyCommand<T>` — stores component type, field offset, before/after value. Generic for all POD fields. [files: editor/src/undo_system.cpp]
- **P1-M9-B2c**: `CreateEntityCommand` / `DestroyEntityCommand` — stores full entity state snapshot for undo. [files: editor/src/undo_system.cpp]
- **P1-M9-B2d**: `AddComponentCommand<T>` / `RemoveComponentCommand<T>` — stores component data for undo. [files: editor/src/undo_system.cpp]
- **P1-M9-B2e**: `ReparentCommand` — stores old/new parent for hierarchy undo. [files: editor/src/undo_system.cpp]
- **P1-M9-B2f**: Keyboard shortcuts: Ctrl+Z = undo, Ctrl+Y = redo. [files: editor/src/editor.cpp]

#### P1-M9-C: Scene Hierarchy

##### P1-M9-C1: Hierarchy Panel
- **P1-M9-C1a**: ImGui tree view showing all entities with names, parent-child nesting. [files: editor/src/hierarchy_panel.cpp]
- **P1-M9-C1b**: Click to select entity (update inspector). Multi-select with Ctrl+click. [files: editor/src/hierarchy_panel.cpp]
- **P1-M9-C1c**: Drag-and-drop to reparent entities (generates ReparentCommand). [files: editor/src/hierarchy_panel.cpp]
- **P1-M9-C1d**: Right-click context menu: Create Empty, Duplicate, Delete, Copy, Paste. [files: editor/src/hierarchy_panel.cpp]
- **P1-M9-C1e**: Search/filter bar at top. [files: editor/src/hierarchy_panel.cpp]

#### P1-M9-D: Asset Browser

##### P1-M9-D1: Filesystem View
- **P1-M9-D1a**: Left panel: folder tree (assets/ directory). Right panel: grid/list of assets in selected folder. [files: editor/src/asset_browser.cpp]
- **P1-M9-D1b**: Show thumbnails for textures and meshes (from P1-M4-A3). Type icons for scripts, sounds, etc. [files: editor/src/asset_browser.cpp]
- **P1-M9-D1c**: Double-click asset: opens inspector showing metadata, import settings, preview. [files: editor/src/asset_browser.cpp]
- **P1-M9-D1d**: Drag asset from browser to viewport: instantiate as entity with appropriate components. [files: editor/src/asset_browser.cpp]
- **P1-M9-D1e**: Search bar with tag filtering. [files: editor/src/asset_browser.cpp]

##### P1-M9-D2: Asset Operations
- **P1-M9-D2a**: Right-click: Reimport, Show in Explorer, Copy Path, Delete (with dependency warning). [files: editor/src/asset_browser.cpp]
- **P1-M9-D2b**: Auto-detect new/changed files (file watcher or poll). Trigger reimport. [files: editor/src/asset_browser.cpp]

#### P1-M9-E: Prefab System

##### P1-M9-E1: Prefab Definition
- **P1-M9-E1a**: Define `Prefab` asset: JSON snapshot of an entity hierarchy (components, children, properties). [files: runtime/include/engine/runtime/prefab.h]
- **P1-M9-E1b**: "Save as Prefab" from selected entity in hierarchy → write prefab JSON to assets folder. [files: editor/src/prefab_system.cpp]
- **P1-M9-E1c**: Instantiate prefab: create entity hierarchy from prefab JSON, assign new IDs. [files: runtime/src/prefab.cpp]

##### P1-M9-E2: Prefab Overrides
- **P1-M9-E2a**: Instances track which properties are overridden vs from-prefab. [files: runtime/src/prefab.cpp]
- **P1-M9-E2b**: Inspector: show overridden properties in bold. "Revert to Prefab" per property. [files: editor/src/property_inspector.cpp]
- **P1-M9-E2c**: "Apply to Prefab" propagates instance changes back to prefab definition and all other instances. [files: editor/src/prefab_system.cpp]

##### P1-M9-E3: Nested Prefabs
- **P1-M9-E3a**: A prefab can contain references to other prefabs. Instantiation is recursive. [files: runtime/src/prefab.cpp]
- **P1-M9-E3b**: Modification propagation: updating inner prefab updates all outer prefabs that use it. [files: runtime/src/prefab.cpp]
- **P1-M9-E3c**: Write test: nest 3 levels of prefabs, modify innermost, verify propagation to outermost instances. [files: tests/integration/prefab_test.cpp]

#### P1-M9-F: Play-in-Editor

##### P1-M9-F1: Save/Restore World State
- **P1-M9-F1a**: On Play: serialize entire world to in-memory buffer (scene JSON). [files: editor/src/play_mode.cpp]
- **P1-M9-F1b**: On Stop: deserialize buffer, restore world to exact pre-play state. [files: editor/src/play_mode.cpp]
- **P1-M9-F1c**: On Pause: freeze world tick but keep editor responsive (can inspect, but not modify). [files: editor/src/play_mode.cpp]

##### P1-M9-F2: Single-Frame Step
- **P1-M9-F2a**: While paused, "Step" button advances exactly one frame. [files: editor/src/play_mode.cpp]
- **P1-M9-F2b**: Step N frames via console command `step <N>`. [files: editor/src/play_mode.cpp]

#### P1-M9-G: Editor Scripting API

##### P1-M9-G1: Editor Extension Points
- **P1-M9-G1a**: Lua `editor.register_menu_item(menu_path, callback)` — adds custom menu entries. [files: editor/src/editor_scripting.cpp]
- **P1-M9-G1b**: Lua `editor.register_panel(name, draw_callback)` — adds custom ImGui panel driven by Lua. [files: editor/src/editor_scripting.cpp]
- **P1-M9-G1c**: Lua `editor.get_selected_entities()`, `editor.select_entity(id)`. [files: editor/src/editor_scripting.cpp]
- **P1-M9-G1d**: Lua `editor.execute_command(undo_command)` — integrates with undo system. [files: editor/src/editor_scripting.cpp]

**P1-M9 Exit Criteria**:
- Inspector shows all registered components with type-appropriate widgets.
- Undo/redo works for all property edits, entity create/destroy, reparent, component add/remove.
- Asset browser displays thumbnails, supports drag-to-viewport, search by tags.
- Prefab system with instance overrides and nested prefabs.
- Play/Pause/Stop preserves and restores exact world state.
- Editor Lua API allows custom menu items and panels.

---

### P1-M10: Scene Management and World Streaming

**Goal**: Multi-level scene loading, streaming volumes, persistent entity IDs across loads, additive scenes, and save/load game state.

**Dependencies**: P1-M4 (async asset streaming), P1-M9 (editor scene save/load).

#### P1-M10-A: Scene Management

##### P1-M10-A1: Scene Transition API
- **P1-M10-A1a**: Implement `SceneManager::load_scene(scene_path)` — unload current, load new. [files: runtime/src/scene_manager.cpp]
- **P1-M10-A1b**: Implement `SceneManager::load_scene_additive(scene_path)` — load on top of current (merge entities). [files: runtime/src/scene_manager.cpp]
- **P1-M10-A1c**: Implement `SceneManager::unload_scene(scene_id)` — remove an additively loaded scene's entities. [files: runtime/src/scene_manager.cpp]
- **P1-M10-A1d**: Loading screen support: show loading UI while scene loads (callback for progress). [files: runtime/src/scene_manager.cpp]
- **P1-M10-A1e**: Lua binding: `engine.load_scene("level2.json")`, `engine.load_scene_additive("dungeon.json")`, `engine.unload_scene(id)`. [files: scripting/src/scripting.cpp]

##### P1-M10-A2: Persistent Entity IDs
- **P1-M10-A2a**: Assign stable UUIDs (or deterministic 64-bit IDs) to entities at creation. Persist in scene JSON. [files: runtime/include/engine/runtime/world.h, runtime/src/scene_serializer.cpp]
- **P1-M10-A2b**: On load: map serialized IDs to runtime entity handles. Maintain lookup table. [files: runtime/src/scene_serializer.cpp]
- **P1-M10-A2c**: Cross-scene references: entity A in scene 1 references entity B in scene 2 by UUID. Resolved at load. [files: runtime/src/scene_serializer.cpp]
- **P1-M10-A2d**: Write test: save scene, load, verify entity IDs match, cross-reference resolves. [files: tests/integration/persistent_id_test.cpp]

#### P1-M10-B: World Streaming

##### P1-M10-B1: Streaming Volumes
- **P1-M10-B1a**: Define `StreamingVolume` component: AABB that triggers scene load/unload when player enters/leaves. [files: runtime/include/engine/runtime/streaming_volume.h]
- **P1-M10-B1b**: Each volume references a sub-scene asset. [files: runtime/src/streaming_volume.cpp]
- **P1-M10-B1c**: StreamingManager: tick each frame, check player position against all volumes, load/unload scenes. [files: runtime/src/streaming_manager.cpp]
- **P1-M10-B1d**: Hysteresis: don't unload immediately when player leaves — wait until distance exceeds threshold. [files: runtime/src/streaming_manager.cpp]
- **P1-M10-B1e**: Async load: use P1-M4 async loading. Show LOD placeholder until sub-scene is ready. [files: runtime/src/streaming_manager.cpp]

##### P1-M10-B2: LOD System
- **P1-M10-B2a**: Define `LODGroup` component: array of {mesh, max_distance}. [files: runtime/include/engine/runtime/world.h]
- **P1-M10-B2b**: Each frame, select LOD level per entity based on camera distance. [files: runtime/src/lod_system.cpp]
- **P1-M10-B2c**: Hysteresis band to prevent popping (switch at different distances going in vs out). [files: runtime/src/lod_system.cpp]
- **P1-M10-B2d**: Lua binding: `engine.set_lod_group(entity, {mesh1, dist1, mesh2, dist2, ...})`. [files: scripting/src/scripting.cpp]

#### P1-M10-C: Save System

##### P1-M10-C1: Game Save/Load
- **P1-M10-C1a**: Define `SaveData` struct: current scene, game state (P1-M2-B1), per-entity overrides from baseline scene, timer states, player stats. [files: runtime/include/engine/runtime/save_system.h]
- **P1-M10-C1b**: Serialize to JSON. `SaveSystem::save(slot_index)`, `SaveSystem::load(slot_index)`. [files: runtime/src/save_system.cpp]
- **P1-M10-C1c**: Multiple save slots (configurable, default 10). [files: runtime/src/save_system.cpp]
- **P1-M10-C1d**: Save file location: platform-appropriate (AppData on Windows, ~/Library on Mac, etc). [files: runtime/src/save_system.cpp]
- **P1-M10-C1e**: Lua binding: `engine.save_game(slot)`, `engine.load_game(slot)`, `engine.list_save_slots()`. [files: scripting/src/scripting.cpp]

##### P1-M10-C2: Checkpoint System
- **P1-M10-C2a**: `engine.save_checkpoint()` — auto-save at scripted moment (unlike manual save slots, only one active checkpoint). [files: runtime/src/save_system.cpp]
- **P1-M10-C2b**: On death/failure: offer "Load Checkpoint" option. [files: runtime/src/save_system.cpp]
- **P1-M10-C2c**: Write test: save checkpoint, modify world, load checkpoint, verify world restored. [files: tests/integration/save_system_test.cpp]

**P1-M10 Exit Criteria**:
- Scene transitions (exclusive and additive) with loading screens.
- Persistent entity IDs survive save/load and cross-scene references resolve.
- Streaming volumes trigger async sub-scene load/unload with hysteresis.
- LOD system switches meshes by camera distance.
- Game save/load with multiple slots, checkpoint system.
- All features have Lua bindings.

---

### P1-M11: UI System (Runtime Game UI)

**Goal**: Immediate-mode game UI separate from ImGui editor UI. Canvas, widgets, layout, data binding, and Lua scripting.

**Dependencies**: P1-M5 (renderer for text/sprite rendering), P1-M2 (input for UI interaction).

#### P1-M11-A: UI Canvas and Rendering

##### P1-M11-A1: Canvas System
- **P1-M11-A1a**: Define `UICanvas`: screen-space overlay rendered after 3D scene. Owns a tree of `UIElement`s. [files: runtime/include/engine/runtime/ui_canvas.h]
- **P1-M11-A1b**: Coordinate system: normalized (0–1) or pixel absolute. Anchors (top-left, center, bottom-right). [files: runtime/src/ui_canvas.cpp]
- **P1-M11-A1c**: Resolution independence: scale based on reference resolution (1920×1080). [files: runtime/src/ui_canvas.cpp]

##### P1-M11-A2: UI Rendering Pipeline
- **P1-M11-A2a**: Batch UI draw calls: sort by material/texture, merge into single VBO per batch. [files: renderer/src/ui_renderer.cpp]
- **P1-M11-A2b**: Write `ui.vert` + `ui.frag` — simple 2D transform, texture sampling, color tinting. [files: assets/shaders/ui.vert, assets/shaders/ui.frag]
- **P1-M11-A2c**: Alpha blending, depth test OFF. Render after all 3D and post-processing. [files: renderer/src/ui_renderer.cpp]

##### P1-M11-A3: Font Rendering
- **P1-M11-A3a**: Integrate stb_truetype (already in deps) or Freetype for font loading. Generate font atlas at runtime. [files: renderer/src/font_renderer.cpp]
- **P1-M11-A3b**: Support SDF (Signed Distance Field) fonts for clean scaling. [files: renderer/src/font_renderer.cpp, assets/shaders/ui_sdf.frag]
- **P1-M11-A3c**: Text layout: line wrapping, alignment (left/center/right), vertical alignment. [files: renderer/src/font_renderer.cpp]
- **P1-M11-A3d**: Rich text: inline color changes `<color=red>text</color>`. [files: renderer/src/font_renderer.cpp]

#### P1-M11-B: Widget Library

##### P1-M11-B1: Core Widgets
- **P1-M11-B1a**: `UIImage` — displays a texture with optional tint and 9-slice mode. [files: runtime/src/ui_widgets.cpp]
- **P1-M11-B1b**: `UIText` — displays text with font, size, color, alignment. [files: runtime/src/ui_widgets.cpp]
- **P1-M11-B1c**: `UIButton` — image + text + hover/pressed states + on_click callback. [files: runtime/src/ui_widgets.cpp]
- **P1-M11-B1d**: `UISlider` — horizontal or vertical, min/max, current value, on_change callback. [files: runtime/src/ui_widgets.cpp]
- **P1-M11-B1e**: `UIProgressBar` — fill amount (0–1), color gradient. [files: runtime/src/ui_widgets.cpp]
- **P1-M11-B1f**: `UIToggle` — checkbox or switch, on/off state. [files: runtime/src/ui_widgets.cpp]
- **P1-M11-B1g**: `UIInputField` — text entry with caret, selection, keyboard input. [files: runtime/src/ui_widgets.cpp]

##### P1-M11-B2: Containers / Layout
- **P1-M11-B2a**: `UIHBox` / `UIVBox` — arrange children horizontally/vertically with spacing and padding. [files: runtime/src/ui_layout.cpp]
- **P1-M11-B2b**: `UIGrid` — grid layout with row/column count. [files: runtime/src/ui_layout.cpp]
- **P1-M11-B2c**: `UIScrollView` — scrollable area with content larger than viewport. [files: runtime/src/ui_layout.cpp]
- **P1-M11-B2d**: Layout engine: calculate sizes recursively (min/preferred/flexible), assign positions. [files: runtime/src/ui_layout.cpp]

#### P1-M11-C: UI Interaction

##### P1-M11-C1: Input Routing
- **P1-M11-C1a**: UI raycast: mouse position → hit test against UI element rects (front to back). [files: runtime/src/ui_input.cpp]
- **P1-M11-C1b**: Focus system: Tab to cycle focusable elements, Enter/Space to activate. [files: runtime/src/ui_input.cpp]
- **P1-M11-C1c**: Input consumption: if UI handles click/key, event does NOT propagate to game input. [files: runtime/src/ui_input.cpp]
- **P1-M11-C1d**: Gamepad navigation: D-pad to move between focusable widgets, A button to activate. [files: runtime/src/ui_input.cpp]

##### P1-M11-C2: Animations / Transitions
- **P1-M11-C2a**: UI tweens: animate any property (position, opacity, scale) over time with easing curves. [files: runtime/src/ui_animation.cpp]
- **P1-M11-C2b**: Button hover: scale up 1.05× with ease-out. Press: scale 0.95× with ease-in. [files: runtime/src/ui_animation.cpp]
- **P1-M11-C2c**: Panel fade-in/out: opacity 0→1 over 0.3s. [files: runtime/src/ui_animation.cpp]

#### P1-M11-D: Lua UI API

##### P1-M11-D1: Widget Creation
- **P1-M11-D1a**: `engine.ui_create_canvas(name)` → canvas_id. [files: scripting/src/scripting.cpp]
- **P1-M11-D1b**: `engine.ui_add_text(canvas_id, text, x, y, font_size)` → element_id. [files: scripting/src/scripting.cpp]
- **P1-M11-D1c**: `engine.ui_add_button(canvas_id, text, x, y, w, h, on_click)` → element_id. [files: scripting/src/scripting.cpp]
- **P1-M11-D1d**: `engine.ui_add_image(canvas_id, texture_path, x, y, w, h)`. [files: scripting/src/scripting.cpp]
- **P1-M11-D1e**: `engine.ui_add_progress_bar(canvas_id, x, y, w, h, fill)`. [files: scripting/src/scripting.cpp]
- **P1-M11-D1f**: `engine.ui_set_property(element_id, property_name, value)`. [files: scripting/src/scripting.cpp]
- **P1-M11-D1g**: `engine.ui_remove(element_id)`, `engine.ui_clear(canvas_id)`. [files: scripting/src/scripting.cpp]

##### P1-M11-D2: Data Binding
- **P1-M11-D2a**: `engine.ui_bind(element_id, "text", "player_health")` — element.text auto-updates when `player_health` game variable changes. [files: scripting/src/scripting.cpp]
- **P1-M11-D2b**: Observable variables: `engine.set_observable("player_health", 100)` triggers UI updates. [files: scripting/src/scripting.cpp]

**P1-M11 Exit Criteria**:
- UI canvas renders over 3D scene with correct alpha blending.
- All core widgets (text, image, button, slider, progress, toggle, input field) functional.
- Layout system (hbox, vbox, grid, scroll) computes positions correctly.
- Input routing: mouse/touch/gamepad navigation with focus system.
- UI animations/tweens for hover, press, panel transitions.
- Complete Lua API for creating and data-binding UI from scripts.
- Font rendering supports SDF and rich text.

---

### P1-M12: Platform, Packaging, and Ship Readiness

**Goal**: Cross-platform abstraction, packaging for Windows/Linux/macOS, quality settings, crash reporting, and final integration tests.

**Dependencies**: All P1 milestones.

#### P1-M12-A: Platform Abstraction Layer

##### P1-M12-A1: Platform Interface
- **P1-M12-A1a**: Define `Platform` interface: `init()`, `shutdown()`, `get_save_path()`, `get_data_path()`, `open_url()`, `get_memory_info()`, `get_cpu_info()`, `get_gpu_info()`. [files: core/include/engine/core/platform.h]
- **P1-M12-A1b**: Windows implementation: Win32 API calls. [files: core/src/platform_win32.cpp]
- **P1-M12-A1c**: Linux implementation: POSIX + /proc/. [files: core/src/platform_linux.cpp]
- **P1-M12-A1d**: macOS implementation: Cocoa/Foundation. [files: core/src/platform_macos.mm]
- **P1-M12-A1e**: Compile-time platform selection via CMake (`ENGINE_TARGET_PLATFORM`). [files: core/CMakeLists.txt]

##### P1-M12-A2: Filesystem Abstraction
- **P1-M12-A2a**: `core::FileSystem::read_file(path)`, `write_file(path, data)`, `exists(path)`, `list_directory(path)`. Platform-specific underneath. [files: core/include/engine/core/filesystem.h, core/src/filesystem.cpp]
- **P1-M12-A2b**: Virtual file system: mount points (e.g., "assets/" → packed archive or loose directory). [files: core/src/filesystem.cpp]
- **P1-M12-A2c**: Write test: read/write roundtrip, directory listing, mount point resolution. [files: tests/unit/filesystem_test.cpp]

#### P1-M12-B: Quality Settings

##### P1-M12-B1: Scalability Presets
- **P1-M12-B1a**: Define quality levels: Low, Medium, High, Ultra, Custom. [files: runtime/include/engine/runtime/quality_settings.h]
- **P1-M12-B1b**: Each level sets: shadow resolution, shadow cascade count, SSAO samples, bloom enabled, AA mode, draw distance, LOD bias, texture quality, fog quality. [files: runtime/src/quality_settings.cpp]
- **P1-M12-B1c**: Apply quality: update CVars in batch for selected preset. [files: runtime/src/quality_settings.cpp]
- **P1-M12-B1d**: Lua binding: `engine.set_quality("high")`, `engine.get_quality()`. [files: scripting/src/scripting.cpp]

##### P1-M12-B2: Dynamic Resolution
- **P1-M12-B2a**: Monitor frame time. If consistently above target (16.67ms for 60fps), reduce render resolution. If below, increase. [files: renderer/src/dynamic_resolution.cpp]
- **P1-M12-B2b**: Scale range: 50%–100% of native resolution. [files: renderer/src/dynamic_resolution.cpp]
- **P1-M12-B2c**: Upscale with bilinear or CAS (AMD FidelityFX sharpening). [files: renderer/src/dynamic_resolution.cpp]
- **P1-M12-B2d**: CVar `r_dynamic_resolution` toggle. [files: renderer/src/dynamic_resolution.cpp]

#### P1-M12-C: Packaging

##### P1-M12-C1: Asset Packing for Distribution
- **P1-M12-C1a**: Asset packer bundles all cooked assets into single archive (custom binary format or zip). [files: tools/asset_packer/main.cpp]
- **P1-M12-C1b**: Runtime loads from archive using virtual filesystem mount. [files: core/src/filesystem.cpp]
- **P1-M12-C1c**: Stripping: exclude editor-only assets, debug scripts, test scenes. [files: tools/asset_packer/main.cpp]

##### P1-M12-C2: Executable Packaging
- **P1-M12-C2a**: CMake install target: copies executable + runtime DLLs (SDL, etc.) + asset archive to install directory. [files: CMakeLists.txt]
- **P1-M12-C2b**: Windows: generate ZIP or installer via CPack. [files: CMakeLists.txt]
- **P1-M12-C2c**: Linux: generate tar.gz or AppImage. [files: CMakeLists.txt]
- **P1-M12-C2d**: macOS: generate .app bundle. [files: CMakeLists.txt]

#### P1-M12-D: Crash Reporting

##### P1-M12-D1: Crash Handler
- **P1-M12-D1a**: Install signal handlers (SIGSEGV, SIGABRT) / Windows structured exception handler. [files: core/src/crash_handler.cpp]
- **P1-M12-D1b**: On crash: dump stack trace (using platform-specific backtrace), engine state snapshot (frame number, scene name, active entities). [files: core/src/crash_handler.cpp]
- **P1-M12-D1c**: Write crash log to file (timestamped in crash_logs/ directory). [files: core/src/crash_handler.cpp]
- **P1-M12-D1d**: Display user-friendly crash dialog (optional). [files: core/src/crash_handler.cpp]

##### P1-M12-D2: Telemetry (Opt-In)
- **P1-M12-D2a**: Optional: collect anonymized crash reports. Send to configurable HTTP endpoint. [files: core/src/telemetry.cpp]
- **P1-M12-D2b**: CVar `engine.telemetry_enabled` (default off). Must be explicitly opt-in. [files: core/src/telemetry.cpp]

#### P1-M12-E: Localization

##### P1-M12-E1: String Table System
- **P1-M12-E1a**: Define `StringTable`: key → localized string map. Load from JSON per language (en.json, fr.json, de.json, etc.). [files: runtime/include/engine/runtime/localization.h, runtime/src/localization.cpp]
- **P1-M12-E1b**: `localize(key) → string` — returns localized string for current language. Fallback to English if missing. [files: runtime/src/localization.cpp]
- **P1-M12-E1c**: `set_language(language_code)` — switch language at runtime, fire event to refresh UI. [files: runtime/src/localization.cpp]
- **P1-M12-E1d**: Lua binding: `engine.localize("ui.start_game")`, `engine.set_language("fr")`. [files: scripting/src/scripting.cpp]
- **P1-M12-E1e**: Write test: set language to "de", verify correct string returned for known key. [files: tests/unit/localization_test.cpp]

#### P1-M12-F: Accessibility

##### P1-M12-F1: Accessibility Features
- **P1-M12-F1a**: UI font size scaling (CVar `ui.font_scale` 0.5–3.0). [files: runtime/src/ui_canvas.cpp]
- **P1-M12-F1b**: High contrast mode: override UI colors to high-contrast palette. [files: runtime/src/ui_canvas.cpp]
- **P1-M12-F1c**: Colorblind mode: post-process filter simulating deuteranopia/protanopia/tritanopia, or artist-defined alternate color schemes. [files: assets/shaders/colorblind.frag, renderer/src/post_process.cpp]
- **P1-M12-F1d**: Subtitle system: display timed text from subtitle file (SRT-like format). [files: runtime/src/subtitle_system.cpp]
- **P1-M12-F1e**: Lua binding: `engine.set_colorblind_mode("deuteranopia")`, `engine.set_font_scale(1.5)`. [files: scripting/src/scripting.cpp]

#### P1-M12-G: Final Integration Tests

##### P1-M12-G1: End-to-End Smoke Tests
- **P1-M12-G1a**: Write smoke test: launch engine, load scene, play 300 frames, save screenshot, exit. Verify no crash. [files: tests/smoke/end_to_end_test.cpp]
- **P1-M12-G1b**: Write smoke test: load scene, spawn 1000 entities, run physics+animation+audio for 600 frames. [files: tests/smoke/stress_test.cpp]
- **P1-M12-G1c**: Write smoke test: save game, load game, verify world state matches. [files: tests/smoke/save_load_test.cpp]
- **P1-M12-G1d**: Write smoke test: transition between 3 scenes, verify no leaks (entity count returns to baseline). [files: tests/smoke/scene_transition_test.cpp]

##### P1-M12-G2: Memory Leak Detection
- **P1-M12-G2a**: Run full test suite under Valgrind/ASAN. Zero leaks policy. [files: ci.yml]
- **P1-M12-G2b**: Run 5-minute play session, compare memory before/after — delta < 1MB. [files: tests/smoke/leak_test.cpp]

**P1-M12 Exit Criteria**:
- Platform abstraction for Windows/Linux/macOS with filesystem, save paths, system info.
- Quality presets (Low/Med/High/Ultra) controlling all visual CVars.
- Dynamic resolution maintains target framerate.
- Packed distribution builds for Win/Lin/Mac.
- Crash handler writes stack trace to log.
- Localization with runtime language switching.
- Accessibility: font scaling, high contrast, colorblind modes, subtitles.
- All smoke tests pass. Zero memory leaks under ASAN/Valgrind.

---

## Phase 2: Competitive Feature Parity (Medium Priority)

Phase 2 builds on the fully-shipped Phase 1 engine, adding advanced rendering, VFX, 2D,
networking, advanced editor tooling, and polish features that bring the engine to competitive
parity with Godot/Unreal/Unity.

---

### P2-M1: Advanced Rendering

**Goal**: Lightmap baking, global illumination probes, volumetric effects, TAA, motion blur, depth of field, screen-space reflections.

**Dependencies**: P1-M5/M6 (deferred pipeline, post-process stack).

#### P2-M1-A: Lightmap Baking

##### P2-M1-A1: UV2 Lightmap Layout
- **P2-M1-A1a**: Generate unique UV2 (lightmap UV) per mesh at import time using xatlas or custom packer. [files: tools/asset_packer/]
- **P2-M1-A1b**: Store UV2 in vertex attributes. Pass to shaders. [files: renderer/src/mesh.cpp]

##### P2-M1-A2: CPU Path Tracer for Baking
- **P2-M1-A2a**: Implement simple CPU path tracer: emit rays from lightmap texel positions, bounce N times (default 3), accumulate irradiance. [files: tools/lightmap_baker/path_tracer.cpp]
- **P2-M1-A2b**: Multi-threaded: distribute texel rows across job system threads. [files: tools/lightmap_baker/path_tracer.cpp]
- **P2-M1-A2c**: Output: per-mesh lightmap texture (RGBA16F). [files: tools/lightmap_baker/]
- **P2-M1-A2d**: Denoise: bilateral filter or OIDN integration. [files: tools/lightmap_baker/]

##### P2-M1-A3: Runtime Lightmap Sampling
- **P2-M1-A3a**: Load lightmap textures at runtime. Bind per-mesh. [files: renderer/src/renderer.cpp]
- **P2-M1-A3b**: Deferred lighting: multiply lightmap sample into indirect diffuse term. [files: assets/shaders/deferred_lighting.frag]
- **P2-M1-A3c**: Editor: "Bake Lightmaps" button triggers bake, stores results in asset folder. [files: editor/src/editor.cpp]

#### P2-M1-B: Screen-Space Reflections (SSR)

##### P2-M1-B1: Ray Marching SSR
- **P2-M1-B1a**: Post-process pass: per pixel, compute reflection ray from view direction + normal. March through depth buffer. [files: assets/shaders/ssr.frag]
- **P2-M1-B1b**: Hi-Z acceleration: use depth mip pyramid for large steps, refine with binary search. [files: renderer/src/ssr.cpp]
- **P2-M1-B1c**: Fade at screen edges (no information to reflect). Fallback to environment probe. [files: assets/shaders/ssr.frag]
- **P2-M1-B1d**: CVar `r_ssr_enabled`, `r_ssr_max_steps`, `r_ssr_thickness`. [files: renderer/src/ssr.cpp]

#### P2-M1-C: Volumetric Fog / God Rays

##### P2-M1-C1: Volumetric Fog Implementation
- **P2-M1-C1a**: Froxel grid: 3D texture representing view frustum slices. [files: renderer/src/volumetric_fog.cpp]
- **P2-M1-C1b**: Per froxel: compute participating media density, in-scattered light from shadow map sampling. [files: renderer/src/volumetric_fog.cpp, assets/shaders/volumetric_fog.frag]
- **P2-M1-C1c**: Temporal reprojection: reuse previous frame's fog data to reduce noise. [files: renderer/src/volumetric_fog.cpp]
- **P2-M1-C1d**: Composite: blend fog into scene during deferred lighting. [files: assets/shaders/deferred_lighting.frag]

#### P2-M1-D: Advanced Post-Processing

##### P2-M1-D1: Temporal Anti-Aliasing (TAA)
- **P2-M1-D1a**: Jitter projection matrix per frame (Halton sequence). [files: renderer/src/renderer.cpp]
- **P2-M1-D1b**: Write `taa.frag`: reproject previous frame pixel, blend with current using velocity buffer and neighborhood clamping. [files: assets/shaders/taa.frag]
- **P2-M1-D1c**: Motion vectors pass: compute per-pixel velocity from current/previous MVP. [files: assets/shaders/motion_vectors.frag]
- **P2-M1-D1d**: CVar `r_aa_mode` value 2 = TAA. [files: renderer/src/post_process.cpp]

##### P2-M1-D2: Motion Blur
- **P2-M1-D2a**: Post-process: sample along velocity vector (from motion vectors) to blur moving objects. [files: assets/shaders/motion_blur.frag]
- **P2-M1-D2b**: Variable sample count based on velocity magnitude. [files: assets/shaders/motion_blur.frag]
- **P2-M1-D2c**: CVar `r_motion_blur_enabled`, `r_motion_blur_samples`, `r_motion_blur_scale`. [files: renderer/src/post_process.cpp]

##### P2-M1-D3: Depth of Field
- **P2-M1-D3a**: Compute circle of confusion (CoC) per pixel based on depth relative to focus distance. [files: assets/shaders/dof.frag]
- **P2-M1-D3b**: Separate near and far blur passes (disc sampling with bokeh approximation). [files: assets/shaders/dof.frag]
- **P2-M1-D3c**: Configurable: focal distance, aperture (f-stop), autofocus (trace center ray). [files: renderer/src/dof.cpp]
- **P2-M1-D3d**: Lua binding: `engine.set_dof(focal_distance, aperture)`. [files: scripting/src/scripting.cpp]

##### P2-M1-D4: Post-Process Volumes
- **P2-M1-D4a**: Define `PostProcessVolume` component: AABB + blendable settings (bloom intensity, fog density, exposure, etc.). [files: runtime/include/engine/runtime/post_process_volume.h]
- **P2-M1-D4b**: When camera inside volume: lerp settings from default toward volume settings by blend weight. [files: runtime/src/post_process_volume.cpp]
- **P2-M1-D4c**: Prioritized volumes: highest priority wins. Overlapping volumes blend by distance to boundary. [files: runtime/src/post_process_volume.cpp]
- **P2-M1-D4d**: Lua binding: `engine.set_post_process_volume(entity, settings_table)`. [files: scripting/src/scripting.cpp]

**P2-M1 Exit Criteria**:
- Lightmaps baked via CPU path tracer, rendered in deferred pipeline.
- SSR with Hi-Z acceleration and edge fade.
- Volumetric fog with temporal stability.
- TAA, motion blur, and depth of field functional.
- Post-process volumes blend settings spatially.
- No visual regression in existing rendering features.

---

### P2-M2: VFX / Particle System

**Goal**: GPU particle system with emitters, forces, collision, trails, mesh particles, and Lua API.

**Dependencies**: P1-M5/M6 (renderer), P1-M3 (physics for particle collision).

#### P2-M2-A: Particle Core

##### P2-M2-A1: CPU Particle Manager
- **P2-M2-A1a**: Define `ParticleEmitter` component: spawn rate, max particles, lifetime range, initial velocity, size, color, texture. [files: runtime/include/engine/runtime/particle_system.h]
- **P2-M2-A1b**: Particle buffer: SoA (struct of arrays) for position, velocity, age, size, color, rotation. Fixed capacity per emitter. [files: runtime/src/particle_system.cpp]
- **P2-M2-A1c**: Spawn: emit N particles per frame (rate × dt), initialize from emitter shape (point, sphere, cone, box). [files: runtime/src/particle_system.cpp]
- **P2-M2-A1d**: Update: integrate velocity, apply forces, age particles, kill expired. [files: runtime/src/particle_system.cpp]

##### P2-M2-A2: Particle Forces
- **P2-M2-A2a**: Gravity: global downward acceleration. [files: runtime/src/particle_system.cpp]
- **P2-M2-A2b**: Wind: directional force. [files: runtime/src/particle_system.cpp]
- **P2-M2-A2c**: Turbulence: curl noise-based force field. [files: runtime/src/particle_system.cpp]
- **P2-M2-A2d**: Attractor/repulsor: point force (attract or repel particles). [files: runtime/src/particle_system.cpp]
- **P2-M2-A2e**: Drag: velocity-dependent deceleration. [files: runtime/src/particle_system.cpp]

##### P2-M2-A3: Particle Rendering
- **P2-M2-A3a**: Billboard quad per particle: always face camera. Batch into single draw call per emitter. [files: renderer/src/particle_renderer.cpp]
- **P2-M2-A3b**: Write `particle.vert` / `particle.frag` — texture, color over life, size over life. [files: assets/shaders/particle.vert, assets/shaders/particle.frag]
- **P2-M2-A3c**: Additive blending mode (fire), alpha blend mode (smoke), soft particles (depth fade). [files: renderer/src/particle_renderer.cpp]
- **P2-M2-A3d**: Sort particles back-to-front for correct alpha blending. [files: renderer/src/particle_renderer.cpp]

##### P2-M2-A4: Curves Over Lifetime
- **P2-M2-A4a**: Define `ParticleCurve`: keyframed float or color values over normalized lifetime (0→1). [files: runtime/include/engine/runtime/particle_system.h]
- **P2-M2-A4b**: Apply curves: size over life, color over life, velocity over life, rotation speed over life. [files: runtime/src/particle_system.cpp]
- **P2-M2-A4c**: Editor: visual curve editor for particle parameters. [files: editor/src/particle_editor.cpp]

#### P2-M2-B: Advanced Particle Features

##### P2-M2-B1: GPU Particles
- **P2-M2-B1a**: Compute shader particle simulation: position/velocity update on GPU. [files: assets/shaders/particle_sim.comp or SSBO-based approach]
- **P2-M2-B1b**: Indirect draw: GPU-side particle count, no CPU readback. [files: renderer/src/particle_renderer.cpp]
- **P2-M2-B1c**: Support 100K+ particles per system. [files: renderer/src/particle_renderer.cpp]

##### P2-M2-B2: Particle Collision
- **P2-M2-B2a**: Simple plane collision: bounce particles off ground plane. [files: runtime/src/particle_system.cpp]
- **P2-M2-B2b**: Depth buffer collision: read scene depth, kill or bounce particle when it hits. [files: assets/shaders/particle_sim.comp]
- **P2-M2-B2c**: Spawn sub-particles on collision (splash effect). [files: runtime/src/particle_system.cpp]

##### P2-M2-B3: Trails / Ribbons
- **P2-M2-B3a**: Emit trail points per particle each frame. Create ribbon mesh connecting points. [files: runtime/src/trail_renderer.cpp]
- **P2-M2-B3b**: UV mapping along trail length for animated texture. [files: renderer/src/trail_renderer.cpp]
- **P2-M2-B3c**: Width over life curve for trail tapering. [files: runtime/src/trail_renderer.cpp]

##### P2-M2-B4: Mesh Particles
- **P2-M2-B4a**: Instead of billboard: render instanced mesh per particle. [files: renderer/src/particle_renderer.cpp]
- **P2-M2-B4b**: Per-particle rotation, scale, color applied via instance data. [files: renderer/src/particle_renderer.cpp]

##### P2-M2-B5: Lua Particle API
- **P2-M2-B5a**: `engine.spawn_particle_emitter(entity, emitter_definition)`. [files: scripting/src/scripting.cpp]
- **P2-M2-B5b**: `engine.stop_emitter(entity)`, `engine.set_emitter_rate(entity, rate)`. [files: scripting/src/scripting.cpp]
- **P2-M2-B5c**: Particle events: on_collision callback, on_death callback. [files: scripting/src/scripting.cpp]

**P2-M2 Exit Criteria**:
- CPU particles: spawn, forces, curves, billboard rendering with soft particles.
- GPU particles: 100K+ with compute shader sim.
- Collision with ground and depth buffer.
- Trails/ribbons and mesh particles.
- Editor curve editor for particle parameters.
- Full Lua API.

---

### P2-M3: 2D Engine

**Goal**: Sprite renderer, sprite animation, tilemap, 2D physics, 2D camera, and Lua 2D API.

**Dependencies**: P1-M5 (renderer batch pipeline), P1-M3 (physics broadphase patterns).

#### P2-M3-A: Sprite Renderer

##### P2-M3-A1: Sprite Component
- **P2-M3-A1a**: Define `SpriteComponent`: texture_id, source_rect (atlas sub-image), tint color, flip_x, flip_y, sort_order (z-layer). [files: runtime/include/engine/runtime/sprite.h]
- **P2-M3-A1b**: SparseSet storage, CRUD methods in World. [files: runtime/src/world.cpp]
- **P2-M3-A1c**: Batch sprites by texture: sort by z-layer → texture → position. Single draw call per batch. [files: renderer/src/sprite_renderer.cpp]
- **P2-M3-A1d**: Write `sprite.vert` / `sprite.frag` — 2D position transform, texture atlas sampling, color multiply. [files: assets/shaders/sprite.vert, assets/shaders/sprite.frag]

##### P2-M3-A2: Sprite Atlas
- **P2-M3-A2a**: Asset packer: pack multiple sprite images into atlas texture (rect packing algorithm). [files: tools/asset_packer/atlas_packer.cpp]
- **P2-M3-A2b**: Output atlas image + JSON map (name → x, y, w, h). [files: tools/asset_packer/]
- **P2-M3-A2c**: Runtime: load atlas, resolve sprite names to source rects. [files: runtime/src/sprite_atlas.cpp]

##### P2-M3-A3: Sprite Animation
- **P2-M3-A3a**: Define `SpriteAnimation`: array of frames (source_rect + duration). [files: runtime/include/engine/runtime/sprite_animation.h]
- **P2-M3-A3b**: `SpriteAnimator` component: current animation, current frame, elapsed time, looping, speed. [files: runtime/src/sprite_animation.cpp]
- **P2-M3-A3c**: Lua: `engine.sprite_play(entity, "run")`, `engine.sprite_set_speed(entity, 2.0)`. [files: scripting/src/scripting.cpp]

#### P2-M3-B: Tilemap

##### P2-M3-B1: Tilemap Renderer
- **P2-M3-B1a**: Define `Tilemap` component: tile_width, tile_height, map_width, map_height, tile data array (tile_id per cell), tileset texture. [files: runtime/include/engine/runtime/tilemap.h]
- **P2-M3-B1b**: Render: only draw visible tiles (camera-frustum cull grid cells). Upload as single batched quad array. [files: renderer/src/tilemap_renderer.cpp]
- **P2-M3-B1c**: Multiple layers (background, foreground, collision layer). [files: runtime/src/tilemap.cpp]
- **P2-M3-B1d**: Import from Tiled JSON format (.tmj). [files: tools/asset_packer/tiled_importer.cpp]

##### P2-M3-B2: Tilemap Collision
- **P2-M3-B2a**: Flag tiles as solid/non-solid in tileset. Generate collision shapes from solid tile regions. [files: physics/src/tilemap_collision.cpp]
- **P2-M3-B2b**: Merge adjacent solid tiles into larger rectangles (rectangle merging algorithm). [files: physics/src/tilemap_collision.cpp]
- **P2-M3-B2c**: Register merged rectangles as static AABB colliders. [files: physics/src/tilemap_collision.cpp]

#### P2-M3-C: 2D Physics

##### P2-M3-C1: 2D Collision Shapes
- **P2-M3-C1a**: 2D circle collider: center, radius. [files: physics/include/engine/physics/collider_2d.h]
- **P2-M3-C1b**: 2D box collider: center, half_extents, rotation. [files: physics/include/engine/physics/collider_2d.h]
- **P2-M3-C1c**: 2D polygon collider: convex polygon (max 8 vertices). [files: physics/include/engine/physics/collider_2d.h]
- **P2-M3-C1d**: Narrow phase: circle-circle, circle-box, box-box, polygon-polygon (SAT). [files: physics/src/narrow_phase_2d.cpp]
- **P2-M3-C1e**: Broadphase: grid-based spatial hash for 2D. [files: physics/src/broadphase_2d.cpp]

##### P2-M3-C2: 2D Rigidbody
- **P2-M3-C2a**: `RigidBody2D`: position (Vec2), rotation (float), velocity (Vec2), angular_velocity, mass, gravity_scale. [files: physics/include/engine/physics/rigidbody_2d.h]
- **P2-M3-C2b**: 2D physics step: gravity, integration, broadphase, narrow phase, resolve. [files: physics/src/physics_world_2d.cpp]
- **P2-M3-C2c**: 2D joints: distance, spring, hinge (revolute). [files: physics/src/joints_2d.cpp]
- **P2-M3-C2d**: One-way platforms: collide from above only. [files: physics/src/narrow_phase_2d.cpp]

##### P2-M3-C3: 2D Raycasting
- **P2-M3-C3a**: `raycast_2d(origin, direction, distance, mask)` → hit list. [files: physics/src/physics_world_2d.cpp]
- **P2-M3-C3b**: Lua: `engine.raycast_2d(ox, oy, dx, dy, dist)`. [files: scripting/src/scripting.cpp]

#### P2-M3-D: 2D Camera

##### P2-M3-D1: Orthographic Camera Controller
- **P2-M3-D1a**: Follow target entity with configurable smoothing (lerp-based follow). [files: runtime/src/camera_2d.cpp]
- **P2-M3-D1b**: Camera bounds: clamp camera position to world bounds (don't show outside map). [files: runtime/src/camera_2d.cpp]
- **P2-M3-D1c**: Zoom: orthographic size changes smoothly. Pinch gesture for mobile. [files: runtime/src/camera_2d.cpp]
- **P2-M3-D1d**: Camera shake (2D): offset position by noise. [files: runtime/src/camera_2d.cpp]
- **P2-M3-D1e**: Lua: `engine.camera_2d_follow(entity)`, `engine.camera_2d_zoom(level)`. [files: scripting/src/scripting.cpp]

**P2-M3 Exit Criteria**:
- Sprites render with batching, atlas support, and animation.
- Tilemap loads from Tiled, renders with culling, supports collision.
- 2D physics with circle/box/polygon colliders, joints, one-way platforms, raycasting.
- 2D camera with follow, bounds, zoom, shake.
- All features have Lua bindings.

---

### P2-M4: Networking Foundation

**Goal**: Client-server transport, basic replication, authoritative server model, RPCs, and network prediction basics.

**Dependencies**: P1-M2 (game state architecture), P1-M1 (determinism).

#### P2-M4-A: Transport Layer

##### P2-M4-A1: UDP Socket Abstraction
- **P2-M4-A1a**: Define `NetworkSocket` class: wraps platform UDP socket (Winsock/BSD). `send_to(addr, data, len)`, `receive_from(addr, data, max_len)`. [files: core/include/engine/core/network_socket.h, core/src/network_socket.cpp]
- **P2-M4-A1b**: Non-blocking mode. Poll with `select` or platform equivalent. [files: core/src/network_socket.cpp]

##### P2-M4-A2: Reliable Channel
- **P2-M4-A2a**: Implement sequence numbers (uint16 wrapping). [files: core/src/reliable_channel.cpp]
- **P2-M4-A2b**: Implement ACK bitfield: 32-bit window of received packet acknowledgments. [files: core/src/reliable_channel.cpp]
- **P2-M4-A2c**: Resend unacknowledged packets after timeout (configurable RTT estimation). [files: core/src/reliable_channel.cpp]
- **P2-M4-A2d**: Ordered delivery: buffer out-of-order packets, deliver in sequence. [files: core/src/reliable_channel.cpp]
- **P2-M4-A2e**: Write test: simulate 30% packet loss, verify all messages arrive in order. [files: tests/unit/reliable_channel_test.cpp]

##### P2-M4-A3: Connection Management
- **P2-M4-A3a**: Handshake: client sends connect request, server responds with session token. [files: core/src/connection.cpp]
- **P2-M4-A3b**: Heartbeat: periodic keep-alive packets. Timeout disconnects after 10s silence. [files: core/src/connection.cpp]
- **P2-M4-A3c**: Disconnect: graceful with reason code, or timeout. [files: core/src/connection.cpp]
- **P2-M4-A3d**: Encryption: XChaCha20-Poly1305 for packet payloads (using a lightweight crypto lib). [files: core/src/connection.cpp]

#### P2-M4-B: Replication

##### P2-M4-B1: Entity Replication
- **P2-M4-B1a**: Mark components as replicated (registration flag). [files: runtime/src/replication.cpp]
- **P2-M4-B1b**: Server: serialize dirty component data, send to clients. Delta compression: only send changed fields. [files: runtime/src/replication.cpp]
- **P2-M4-B1c**: Client: receive updates, apply to local entities. [files: runtime/src/replication.cpp]
- **P2-M4-B1d**: Priority: entities closer to client or more relevant get higher update frequency. [files: runtime/src/replication.cpp]

##### P2-M4-B2: RPCs (Remote Procedure Calls)
- **P2-M4-B2a**: Define RPC: function name + serialized args, sent over reliable channel. [files: runtime/include/engine/runtime/rpc.h]
- **P2-M4-B2b**: Server RPC: client invokes, server executes (input commands, ability usage). [files: runtime/src/rpc.cpp]
- **P2-M4-B2c**: Client RPC: server invokes, client executes (effects, UI updates). [files: runtime/src/rpc.cpp]
- **P2-M4-B2d**: Multicast RPC: server sends to all clients. [files: runtime/src/rpc.cpp]
- **P2-M4-B2e**: Lua binding: `engine.rpc_server("use_ability", ability_id)`, `engine.on_rpc("play_effect", callback)`. [files: scripting/src/scripting.cpp]

##### P2-M4-B3: Network Prediction
- **P2-M4-B3a**: Client-side prediction: locally apply input immediately, send to server. [files: runtime/src/prediction.cpp]
- **P2-M4-B3b**: Server reconciliation: when server state arrives, compare with predicted state. If different, snap or interpolate. [files: runtime/src/prediction.cpp]
- **P2-M4-B3c**: Entity interpolation: smoothly interpolate remote entities between server updates. [files: runtime/src/interpolation.cpp]
- **P2-M4-B3d**: Input buffer: store N frames of input for replay on misprediction. [files: runtime/src/prediction.cpp]

#### P2-M4-C: Lobby and Session

##### P2-M4-C1: Session Management
- **P2-M4-C1a**: `NetworkSession`: tracks connected players (id, name, latency, state). [files: runtime/include/engine/runtime/network_session.h]
- **P2-M4-C1b**: Host migration: if host disconnects, promote another player. [files: runtime/src/network_session.cpp]
- **P2-M4-C1c**: Lua: `engine.host_game(port)`, `engine.join_game(ip, port)`, `engine.get_players()`. [files: scripting/src/scripting.cpp]

**P2-M4 Exit Criteria**:
- Reliable UDP with ordered delivery surviving 30% packet loss.
- Encrypted connections with handshake and heartbeat.
- Entity replication with delta compression.
- Server and client RPCs with Lua bindings.
- Client-side prediction and server reconciliation basics.
- Session management with host migration.

---

### P2-M5: Splines, Data Tables, and Gameplay Tools

**Goal**: Spline/path system, data tables, CSG brushes, foliage painter, and gameplay utility features.

**Dependencies**: P1-M2 (gameplay architecture), P1-M9 (editor tools).

#### P2-M5-A: Spline / Path System

##### P2-M5-A1: Spline Data Structure
- **P2-M5-A1a**: Define `Spline` component: array of control points (Vec3), tangent mode (auto/manual), closed flag. [files: runtime/include/engine/runtime/spline.h]
- **P2-M5-A1b**: Catmull-Rom interpolation: `spline.evaluate(t)` → position, `spline.evaluate_tangent(t)` → direction. [files: runtime/src/spline.cpp]
- **P2-M5-A1c**: Arc-length parameterization: remap t to distance-based for constant speed traversal. [files: runtime/src/spline.cpp]
- **P2-M5-A1d**: `spline.get_nearest_point(position)` → closest point on spline + t parameter. [files: runtime/src/spline.cpp]

##### P2-M5-A2: Spline Editor
- **P2-M5-A2a**: Visualize spline as polyline in editor viewport using debug draw. [files: editor/src/spline_editor.cpp]
- **P2-M5-A2b**: Gizmos for control points: drag to move, shift-click to add, delete key to remove. [files: editor/src/spline_editor.cpp]
- **P2-M5-A2c**: Show tangent handles for manual tangent mode. [files: editor/src/spline_editor.cpp]

##### P2-M5-A3: Spline Lua API
- **P2-M5-A3a**: `engine.spline_evaluate(entity, t)` → x, y, z. [files: scripting/src/scripting.cpp]
- **P2-M5-A3b**: `engine.spline_follow(entity, spline_entity, speed)` — move entity along spline at constant speed. [files: scripting/src/scripting.cpp]
- **P2-M5-A3c**: Use cases: camera rails, patrol paths, moving platforms, racing lines. [files: scripting/src/scripting.cpp]

#### P2-M5-B: Data Tables

##### P2-M5-B1: Data Table System
- **P2-M5-B1a**: Define `DataTable`: named 2D table (rows × typed columns). Loaded from CSV or JSON. [files: runtime/include/engine/runtime/data_table.h]
- **P2-M5-B1b**: Column types: int, float, string, bool, AssetRef. [files: runtime/src/data_table.cpp]
- **P2-M5-B1c**: Lookup: `table.get_row("sword_iron")` → row, `row.get_float("damage")`. [files: runtime/src/data_table.cpp]
- **P2-M5-B1d**: Hot reload: detect CSV change, reload table. [files: runtime/src/data_table.cpp]

##### P2-M5-B2: Lua Data Table API
- **P2-M5-B2a**: `engine.load_data_table("weapons.csv")` → table_id. [files: scripting/src/scripting.cpp]
- **P2-M5-B2b**: `engine.dt_get(table_id, "sword_iron", "damage")` → value. [files: scripting/src/scripting.cpp]
- **P2-M5-B2c**: `engine.dt_get_row(table_id, "sword_iron")` → table of all columns. [files: scripting/src/scripting.cpp]

#### P2-M5-C: CSG Brushes

##### P2-M5-C1: CSG Operations
- **P2-M5-C1a**: Define `CSGBrush` component: shape (box, cylinder, sphere), operation (union, subtract, intersect). [files: runtime/include/engine/runtime/csg.h]
- **P2-M5-C1b**: Implement CSG mesh boolean: union, subtraction, intersection using BSP tree method. [files: runtime/src/csg.cpp]
- **P2-M5-C1c**: Generate mesh from CSG tree: traverse operations, output triangles. [files: runtime/src/csg.cpp]
- **P2-M5-C1d**: UV generation: planar projection for CSG faces. [files: runtime/src/csg.cpp]
- **P2-M5-C1e**: Editor: drag CSG brush, boolean operation dropdown, real-time preview. [files: editor/src/csg_editor.cpp]

#### P2-M5-D: Foliage Painting

##### P2-M5-D1: Foliage Paint Tool
- **P2-M5-D1a**: Editor tool: brush radius, density, random rotation/scale range. [files: editor/src/foliage_painter.cpp]
- **P2-M5-D1b**: Click/drag on terrain/mesh: raycast, scatter foliage instances within brush radius. [files: editor/src/foliage_painter.cpp]
- **P2-M5-D1c**: Erase mode: remove instances within brush. [files: editor/src/foliage_painter.cpp]
- **P2-M5-D1d**: Store foliage instances in `FoliageComponent` (array of transform + mesh reference). Render via instancing (P1-M6-C2). [files: runtime/src/foliage.cpp]

**P2-M5 Exit Criteria**:
- Splines with editor gizmos, arc-length parameterization, and Lua follow API.
- Data tables load from CSV/JSON, queryable from Lua, hot-reloadable.
- CSG brushes with union/subtract/intersect and real-time preview.
- Foliage painting tool with density brush and instanced rendering.

---

### P2-M6: Controller Haptics and Advanced Input

**Goal**: Gamepad haptics, gyroscope input, adaptive triggers, and input recording/playback for testing.

**Dependencies**: P1-M2 (input system).

#### P2-M6-A: Haptics

##### P2-M6-A1: Gamepad Rumble
- **P2-M6-A1a**: Implement `engine.set_rumble(low_frequency, high_frequency, duration)` using SDL rumble API. [files: core/src/input.cpp]
- **P2-M6-A1b**: Rumble presets: light (0.2, 0), heavy (0, 0.8), pulse (alternating). [files: core/src/haptics.cpp]
- **P2-M6-A1c**: Lua binding: `engine.rumble(0.5, 0.3, 0.2)`. [files: scripting/src/scripting.cpp]

##### P2-M6-A2: Adaptive Triggers (PS5)
- **P2-M6-A2a**: Define trigger effect types: resistance, vibration, weapon feedback. [files: core/include/engine/core/haptics.h]
- **P2-M6-A2b**: Platform-gated: only active on DualSense controllers. Fallback to no-op. [files: core/src/haptics.cpp]
- **P2-M6-A2c**: Lua binding: `engine.set_trigger_effect("left", "resistance", {start=0.3, end=0.8, force=0.5})`. [files: scripting/src/scripting.cpp]

#### P2-M6-B: Gyroscope / Motion Input

##### P2-M6-B1: Gyro Aiming
- **P2-M6-B1a**: Read gyroscope data from SDL sensor API. [files: core/src/input.cpp]
- **P2-M6-B1b**: Convert angular velocity to camera rotation delta. Configurable sensitivity. [files: core/src/gyro_input.cpp]
- **P2-M6-B1c**: Lua: `engine.get_gyro()` → pitch_rate, yaw_rate, roll_rate. [files: scripting/src/scripting.cpp]

#### P2-M6-C: Input Recording / Replay

##### P2-M6-C1: Input Recorder
- **P2-M6-C1a**: Record all input events (key, mouse, gamepad, touch) with frame timestamps. [files: core/src/input_recorder.cpp]
- **P2-M6-C1b**: Save to binary file. Load and replay: feed recorded events as if live. [files: core/src/input_recorder.cpp]
- **P2-M6-C1c**: Use for: deterministic replay testing, automated QA. [files: core/src/input_recorder.cpp]
- **P2-M6-C1d**: Console commands: `record_start`, `record_stop`, `replay <file>`. [files: core/src/input_recorder.cpp]

**P2-M6 Exit Criteria**:
- Gamepad rumble with presets and Lua API.
- Adaptive trigger effects on DualSense (fallback no-op on others).
- Gyroscope input readable from Lua.
- Input recording/replay for automated testing.

---

### P2-M7: Advanced Editor Features

**Goal**: Visual scripting (node graph), animation editor, particle editor, terrain editor, and plugin system.

**Dependencies**: P1-M9 (base editor), P1-M7 (animation), P2-M2 (particles).

#### P2-M7-A: Visual Scripting (Node Graph)

##### P2-M7-A1: Node Graph Core
- **P2-M7-A1a**: Define `Node` struct: type, inputs (named typed pins), outputs (named typed pins), position on canvas. [files: editor/include/engine/editor/node_graph.h]
- **P2-M7-A1b**: Define `Link` struct: from (node_id, pin_name) → to (node_id, pin_name). Check type compatibility. [files: editor/src/node_graph.cpp]
- **P2-M7-A1c**: Render nodes using ImGui custom draw: rectangles with pin circles, bezier links. [files: editor/src/node_graph_renderer.cpp]
- **P2-M7-A1d**: Interaction: drag to connect pins, right-click to add node, delete to remove. [files: editor/src/node_graph_renderer.cpp]

##### P2-M7-A2: Script Compilation
- **P2-M7-A2a**: Compile node graph to Lua script: topological sort → emit Lua statements. [files: editor/src/node_graph_compiler.cpp]
- **P2-M7-A2b**: Node library: engine API nodes (get_position, set_position, play_sound, etc.), math nodes (add, multiply, lerp), flow nodes (if, for, delay). [files: editor/src/node_library.cpp]
- **P2-M7-A2c**: Live preview: recompile on graph edit, hot-reload script. [files: editor/src/node_graph_compiler.cpp]

#### P2-M7-B: Animation Editor

##### P2-M7-B1: Timeline View
- **P2-M7-B1a**: Horizontal timeline with playback scrubber. Show clip name, duration. [files: editor/src/anim_editor.cpp]
- **P2-M7-B1b**: Keyframe markers on timeline per channel. Click to select keyframe. [files: editor/src/anim_editor.cpp]
- **P2-M7-B1c**: Edit keyframe values in inspector panel. [files: editor/src/anim_editor.cpp]

##### P2-M7-B2: State Machine Editor
- **P2-M7-B2a**: Visual state machine: states as boxes, transitions as arrows with condition labels. [files: editor/src/anim_state_editor.cpp]
- **P2-M7-B2b**: Drag to create transitions, double-click state to edit blend tree, property panel for transition conditions. [files: editor/src/anim_state_editor.cpp]
- **P2-M7-B2c**: Live preview: highlight active state during play mode. [files: editor/src/anim_state_editor.cpp]

#### P2-M7-C: Terrain Editor

##### P2-M7-C1: Heightmap Terrain
- **P2-M7-C1a**: Define `TerrainComponent`: heightmap texture, tile count, height scale. [files: runtime/include/engine/runtime/terrain.h]
- **P2-M7-C1b**: Generate mesh from heightmap: grid of vertices, per-vertex height from texture. [files: renderer/src/terrain_renderer.cpp]
- **P2-M7-C1c**: LOD: clipmap or quadtree-based terrain LOD. [files: renderer/src/terrain_renderer.cpp]
- **P2-M7-C1d**: Multi-texture splatting: blend 4 textures using splat map weights. [files: assets/shaders/terrain.frag]

##### P2-M7-C2: Terrain Sculpt/Paint Tools
- **P2-M7-C2a**: Sculpt: raise/lower/smooth/flatten brush tools. Modify heightmap at mouse position. [files: editor/src/terrain_editor.cpp]
- **P2-M7-C2b**: Paint: select texture layer, paint splat weights with brush. [files: editor/src/terrain_editor.cpp]
- **P2-M7-C2c**: Brush parameters: radius, strength, falloff. [files: editor/src/terrain_editor.cpp]

#### P2-M7-D: Plugin System

##### P2-M7-D1: Editor Plugin API
- **P2-M7-D1a**: Define plugin interface: `on_load()`, `on_unload()`, `on_tick()`, `get_name()`. [files: editor/include/engine/editor/plugin.h]
- **P2-M7-D1b**: Plugin loader: scan plugins/ directory for shared libraries (.dll/.so), load at startup. [files: editor/src/plugin_loader.cpp]
- **P2-M7-D1c**: Plugin sandbox: plugins can register menu items, panels, component inspectors, but cannot bypass module boundaries. [files: editor/src/plugin_loader.cpp]

**P2-M7 Exit Criteria**:
- Visual scripting: node graph with drag-connect, compiles to Lua, hot reloads.
- Animation editor: timeline, keyframe editing, state machine visual editor.
- Terrain: heightmap rendering with LOD, sculpt/paint tools, multi-texture splatting.
- Plugin system loads external plugins.

---

### P2-M8: Performance Polish and Profiling

**Goal**: Multi-threaded rendering, culling optimization, memory budgets, async compute, shader cache, and performance reporting.

**Dependencies**: All P1 milestones, P2-M1 (advanced rendering).

#### P2-M8-A: Multi-Threaded Rendering

##### P2-M8-A1: Command Buffer Recording
- **P2-M8-A1a**: Record draw commands on worker threads (not GL calls — command structs). [files: renderer/src/command_buffer.cpp]
- **P2-M8-A1b**: Main thread: playback command buffers, execute GL calls. [files: renderer/src/command_buffer.cpp]
- **P2-M8-A1c**: Parallel frustum culling + draw command generation. [files: renderer/src/renderer.cpp]

##### P2-M8-A2: Parallel Scene Update
- **P2-M8-A2a**: ECS system scheduling: identify independent systems (e.g., animation and audio) and run in parallel. [files: runtime/src/world.cpp]
- **P2-M8-A2b**: Read/write dependency tracking per system per component type. [files: runtime/src/system_scheduler.cpp]
- **P2-M8-A2c**: Job graph: build DAG of system dependencies, dispatch to job system. [files: runtime/src/system_scheduler.cpp]

#### P2-M8-B: Culling Optimization

##### P2-M8-B1: Hierarchical Culling
- **P2-M8-B1a**: Build bounding volume hierarchy (BVH) for static scene geometry. [files: renderer/src/bvh.cpp]
- **P2-M8-B1b**: Frustum cull BVH: skip entire subtrees when parent node is outside frustum. [files: renderer/src/bvh.cpp]
- **P2-M8-B1c**: Occlusion culling: software rasterizer for occluders (HZB — Hierarchical Z-Buffer). Test occludees against HZB. [files: renderer/src/occlusion_culling.cpp]

#### P2-M8-C: Shader Cache and Pipeline State

##### P2-M8-C1: Shader Compilation Cache
- **P2-M8-C1a**: Cache compiled GL programs to disk (binary format via `glGetProgramBinary`). [files: renderer/src/shader_cache.cpp]
- **P2-M8-C1b**: On load: check cache for matching source hash. Skip compilation if cached. [files: renderer/src/shader_cache.cpp]
- **P2-M8-C1c**: Invalidate cache on shader source change (content hash mismatch). [files: renderer/src/shader_cache.cpp]

##### P2-M8-C2: Pipeline State Object (PSO) Sorting
- **P2-M8-C2a**: Sort draw calls by PSO (shader → material → mesh) to minimize state changes. [files: renderer/src/renderer.cpp]
- **P2-M8-C2b**: Encode PSO as 64-bit sort key for radix sort. [files: renderer/src/renderer.cpp]

#### P2-M8-D: Memory Budgets

##### P2-M8-D1: Per-System Memory Budget
- **P2-M8-D1a**: Define memory budgets per subsystem (physics: 64MB, renderer: 256MB, audio: 32MB, etc.). [files: core/include/engine/core/memory.h]
- **P2-M8-D1b**: Track usage against budget. Log warning at 80%, error at 100%. [files: core/src/memory.cpp]
- **P2-M8-D1c**: Editor memory dashboard: bar chart per subsystem showing usage vs budget. [files: editor/src/editor.cpp]

**P2-M8 Exit Criteria**:
- Multi-threaded command buffer recording and parallel scene update.
- BVH-based hierarchical frustum culling and HZB occlusion culling.
- Shader binary cache eliminates runtime compilation stalls.
- PSO-sorted draw calls reduce state changes.
- Per-subsystem memory budgets with tracking and editor dashboard.

---

## Phase 3: Future / Cutting-Edge (Low Priority)

Phase 3 adds next-generation features that differentiate the engine. Only begin after Phase 2 is complete.

---

### P3-M1: XR / VR / AR

**Goal**: Stereo rendering, head tracking, controller tracking, hand tracking, passthrough AR, and Lua XR API.

**Dependencies**: P2-M1 (advanced rendering), P2-M6 (advanced input).

#### P3-M1-A: OpenXR Integration

##### P3-M1-A1: OpenXR Session
- **P3-M1-A1a**: Initialize OpenXR runtime: create instance, get system, create session with GL graphics binding. [files: renderer/src/xr_session.cpp]
- **P3-M1-A1b**: Handle session lifecycle: ready → focused → stopping → exiting. [files: renderer/src/xr_session.cpp]
- **P3-M1-A1c**: Reference space: create LOCAL and STAGE reference spaces. [files: renderer/src/xr_session.cpp]

##### P3-M1-A2: Stereo Rendering
- **P3-M1-A2a**: Get per-eye view and projection matrices from OpenXR. [files: renderer/src/xr_renderer.cpp]
- **P3-M1-A2b**: Render scene twice (once per eye) into swapchain images. [files: renderer/src/xr_renderer.cpp]
- **P3-M1-A2c**: Multi-view extension: single render pass for both eyes if available. [files: renderer/src/xr_renderer.cpp]
- **P3-M1-A2d**: Submit frames via `xrEndFrame`. Handle frame timing. [files: renderer/src/xr_renderer.cpp]

##### P3-M1-A3: Head and Controller Tracking
- **P3-M1-A3a**: Poll head pose (position + orientation) each frame. Apply to camera. [files: renderer/src/xr_input.cpp]
- **P3-M1-A3b**: Poll controller poses (left/right hand). Map to entity transforms. [files: renderer/src/xr_input.cpp]
- **P3-M1-A3c**: Controller buttons/triggers/thumbsticks: map to input actions from P1-M2-C. [files: core/src/input.cpp]

##### P3-M1-A4: Hand Tracking
- **P3-M1-A4a**: Enable OpenXR hand tracking extension. Get 26-joint hand skeleton per hand. [files: renderer/src/xr_hand_tracking.cpp]
- **P3-M1-A4b**: Map hand joints to visual hand mesh. [files: renderer/src/xr_hand_tracking.cpp]
- **P3-M1-A4c**: Gesture detection: pinch, grab, point. [files: renderer/src/xr_hand_tracking.cpp]

##### P3-M1-A5: Passthrough AR
- **P3-M1-A5a**: Enable passthrough layer if supported (Meta Quest API). [files: renderer/src/xr_passthrough.cpp]
- **P3-M1-A5b**: Composite 3D scene over passthrough feed. [files: renderer/src/xr_passthrough.cpp]

##### P3-M1-A6: Lua XR API
- **P3-M1-A6a**: `engine.xr_is_active()` → bool. [files: scripting/src/scripting.cpp]
- **P3-M1-A6b**: `engine.xr_get_head_transform()` → position, rotation. [files: scripting/src/scripting.cpp]
- **P3-M1-A6c**: `engine.xr_get_hand_transform(hand)` → position, rotation. [files: scripting/src/scripting.cpp]
- **P3-M1-A6d**: `engine.xr_get_hand_joints(hand)` → table of 26 joint transforms. [files: scripting/src/scripting.cpp]
- **P3-M1-A6e**: `engine.xr_haptic_pulse(hand, amplitude, duration)`. [files: scripting/src/scripting.cpp]

**P3-M1 Exit Criteria**:
- Stereo rendering via OpenXR on at least one headset (Quest, SteamVR).
- Head + controller tracking integrated with input system.
- Hand tracking with gesture detection.
- Passthrough AR compositing.
- Full Lua XR API.

---

### P3-M2: Vulkan / Modern Graphics Backend

**Goal**: Vulkan renderer backend alongside OpenGL, abstracted behind RenderDevice interface.

**Dependencies**: P2-M8 (multi-threaded rendering, PSO sorting).

#### P3-M2-A: Render Abstraction Layer

##### P3-M2-A1: Backend Interface
- **P3-M2-A1a**: Abstract `RenderDevice` to pure interface: `create_buffer()`, `create_texture()`, `create_shader()`, `create_pipeline()`, `submit_commands()`. [files: renderer/include/engine/renderer/render_device.h]
- **P3-M2-A1b**: Move current GL code behind `GLRenderDevice : RenderDevice`. [files: renderer/src/gl_render_device.cpp]
- **P3-M2-A1c**: All renderer code uses `RenderDevice*` — never raw GL calls above device layer. [files: renderer/src/]

##### P3-M2-A2: Vulkan Device
- **P3-M2-A2a**: Create Vulkan instance, physical device selection, logical device, queue families. [files: renderer/src/vulkan/vk_device.cpp]
- **P3-M2-A2b**: Swapchain creation and management. [files: renderer/src/vulkan/vk_swapchain.cpp]
- **P3-M2-A2c**: Command buffer pool and recording. [files: renderer/src/vulkan/vk_command_buffer.cpp]
- **P3-M2-A2d**: Implement all `RenderDevice` methods for Vulkan: buffer, texture, shader (SPIR-V), pipeline, submit. [files: renderer/src/vulkan/vk_render_device.cpp]
- **P3-M2-A2e**: Synchronization: fences, semaphores for frame-in-flight. [files: renderer/src/vulkan/vk_sync.cpp]

##### P3-M2-A3: Shader Cross-Compilation
- **P3-M2-A3a**: Author shaders in GLSL. Cross-compile to SPIR-V via glslang (offline in asset packer). [files: tools/asset_packer/shader_compiler.cpp]
- **P3-M2-A3b**: Keep GL GLSL and Vulkan SPIR-V as parallel outputs. [files: tools/asset_packer/]
- **P3-M2-A3c**: Runtime: load appropriate format based on active backend. [files: renderer/src/shader_loader.cpp]

#### P3-M2-B: Vulkan-Specific Features

##### P3-M2-B1: Compute Shaders
- **P3-M2-B1a**: Expose compute dispatch through `RenderDevice::dispatch_compute()`. [files: renderer/include/engine/renderer/render_device.h]
- **P3-M2-B1b**: Migrate GPU particles (P2-M2-B1) to compute pipline. [files: renderer/src/particle_renderer.cpp]
- **P3-M2-B1c**: Add async compute queue: particle sim overlaps with graphics. [files: renderer/src/vulkan/vk_device.cpp]

##### P3-M2-B2: Bindless Resources
- **P3-M2-B2a**: Descriptor indexing: one large descriptor set with all textures. Access by index in shader. [files: renderer/src/vulkan/vk_bindless.cpp]
- **P3-M2-B2b**: Eliminates per-material texture binding overhead. [files: renderer/src/vulkan/vk_bindless.cpp]

**P3-M2 Exit Criteria**:
- All rendering features work identically under both GL and Vulkan backends.
- Backend selected at startup via CVar `r_backend` (gl/vulkan).
- Compute shaders functional on Vulkan.
- Vulkan-specific optimizations (bindless, async compute) provide measurable speedup.

---

### P3-M3: Mobile Platform Support

**Goal**: Android and iOS builds, touch-first UI, mobile GPU optimization, and app store packaging.

**Dependencies**: P1-M12 (platform abstraction), P2-M3 (2D engine for mobile-friendly content).

#### P3-M3-A: Android Build

##### P3-M3-A1: NDK Build Pipeline
- **P3-M3-A1a**: CMake toolchain: cross-compile with NDK, target arm64-v8a. [files: cmake/toolchains/android.cmake]
- **P3-M3-A1b**: SDL2 Android backend: create NativeActivity. [files: app/src/android/]
- **P3-M3-A1c**: Gradle wrapper: build APK/AAB from CMake output. [files: app/android/build.gradle]

##### P3-M3-A2: Android-Specific
- **P3-M3-A2a**: OpenGL ES 3.0 rendering path (subset of GL 3.3 features). [files: renderer/src/gles_render_device.cpp]
- **P3-M3-A2b**: Touch input integration (P1-M2-C3 activates). [files: core/src/touch_input.cpp]
- **P3-M3-A2c**: Performance: reduce shadow resolution, disable SSAO, use lower quality presets by default. [files: runtime/src/quality_settings.cpp]
- **P3-M3-A2d**: Battery/thermal management: reduce target FPS when throttling detected. [files: app/src/android/thermal.cpp]

#### P3-M3-B: iOS Build

##### P3-M3-B1: Xcode Build Pipeline
- **P3-M3-B1a**: CMake toolchain for iOS: target arm64, set deployment target. [files: cmake/toolchains/ios.cmake]
- **P3-M3-B1b**: Generate Xcode project from CMake. [files: cmake/toolchains/ios.cmake]
- **P3-M3-B1c**: Metal rendering backend (or MoltenVK via Vulkan path). [files: renderer/src/metal/]

##### P3-M3-B2: iOS-Specific
- **P3-M3-B2a**: App lifecycle: handle backgrounding, foregrounding, memory warnings. [files: app/src/ios/app_delegate.mm]
- **P3-M3-B2b**: Haptic engine (CoreHaptics) integration for feedback. [files: core/src/haptics_ios.mm]

#### P3-M3-C: Mobile UI Adaptation

##### P3-M3-C1: Touch-First UI
- **P3-M3-C1a**: UI widgets have minimum touch target size (44×44pt). [files: runtime/src/ui_canvas.cpp]
- **P3-M3-C1b**: Virtual joystick overlay for movement. [files: runtime/src/virtual_joystick.cpp]
- **P3-M3-C1c**: Gesture-based camera: swipe to look, pinch to zoom. [files: runtime/src/camera_2d.cpp or camera system]

**P3-M3 Exit Criteria**:
- Android APK builds and runs on modern Android device (arm64).
- iOS IPA builds and runs on iPhone (arm64).
- Touch input, virtual joystick, and gesture camera functional.
- Mobile quality presets maintain 30+ FPS.

---

### P3-M4: Web / Emscripten Build

**Goal**: Run engine in browser via Emscripten/WebAssembly.

**Dependencies**: P1-M12 (packaging), P3-M3 (GLES rendering path).

#### P3-M4-A: Emscripten Compilation

##### P3-M4-A1: Build Pipeline
- **P3-M4-A1a**: CMake toolchain: Emscripten (`emcmake cmake`). [files: cmake/toolchains/emscripten.cmake]
- **P3-M4-A1b**: Compile all C++ to WASM. Link SDL2 Emscripten port. [files: CMakeLists.txt]
- **P3-M4-A1c**: Output: .wasm + .js + .html shell. [files: app/web/shell.html]

##### P3-M4-A2: Web-Specific Adaptations
- **P3-M4-A2a**: Main loop: use `emscripten_set_main_loop` instead of while(true). [files: app/main.cpp]
- **P3-M4-A2b**: Asset loading: fetch over HTTP, use Emscripten async filesystem. [files: core/src/filesystem.cpp]
- **P3-M4-A2c**: WebGL 2.0 rendering (maps to GLES 3.0). [files: renderer/src/gles_render_device.cpp]
- **P3-M4-A2d**: Audio via Web Audio API (miniaudio provides this via Emscripten backend). [files: audio/src/audio.cpp]

##### P3-M4-A3: Web Optimization
- **P3-M4-A3a**: WASM size: strip debug symbols, enable LTO, use `-Oz`. [files: cmake/toolchains/emscripten.cmake]
- **P3-M4-A3b**: Streaming instantiation: load WASM in parallel with JS. [files: app/web/loader.js]
- **P3-M4-A3c**: Progressive asset loading: show loading screen while assets fetch. [files: runtime/src/scene_manager.cpp]

**P3-M4 Exit Criteria**:
- Engine runs in Chrome/Firefox/Safari via Emscripten.
- WebGL 2.0 rendering functional.
- Asset loading via HTTP fetch.
- WASM size < 10MB for minimal scene.

---

### P3-M5: AI and Navigation

**Goal**: Navigation mesh, pathfinding, AI behavior trees, steering behaviors, and Lua AI API.

**Dependencies**: P1-M3 (physics for nav mesh generation), P1-M2 (gameplay architecture).

#### P3-M5-A: Navigation Mesh

##### P3-M5-A1: NavMesh Generation
- **P3-M5-A1a**: Voxelization: rasterize scene geometry into 3D voxel grid. [files: runtime/src/navmesh_builder.cpp]
- **P3-M5-A1b**: Walkable surface detection: identify voxels with solid ground and sufficient headroom. [files: runtime/src/navmesh_builder.cpp]
- **P3-M5-A1c**: Region partitioning: watershed or monotone decomposition of walkable surface. [files: runtime/src/navmesh_builder.cpp]
- **P3-M5-A1d**: Contour tracing: extract region contours as simplified polygons. [files: runtime/src/navmesh_builder.cpp]
- **P3-M5-A1e**: Polygon mesh: triangulate contours into navigation triangles. [files: runtime/src/navmesh_builder.cpp]
- **P3-M5-A1f**: Parameters: agent_radius, agent_height, max_slope, step_height. [files: runtime/src/navmesh_builder.cpp]
- **P3-M5-A1g**: Editor: "Build NavMesh" button, visualize nav mesh overlay. [files: editor/src/navmesh_editor.cpp]

##### P3-M5-A2: Pathfinding
- **P3-M5-A2a**: A* on navigation mesh dual graph (triangle adjacency graph). [files: runtime/src/pathfinder.cpp]
- **P3-M5-A2b**: String pulling (simple stupid funnel algorithm) for smooth paths. [files: runtime/src/pathfinder.cpp]
- **P3-M5-A2c**: Dynamic obstacle avoidance: insert temporary obstacles into nav mesh (carve or mark as blocked). [files: runtime/src/navmesh_obstacle.cpp]
- **P3-M5-A2d**: Off-mesh links: connect disconnected nav mesh regions (ladders, jumps, teleports). [files: runtime/src/navmesh_link.cpp]
- **P3-M5-A2e**: Lua: `engine.find_path(start_x, start_y, start_z, end_x, end_y, end_z)` → path table of waypoints. [files: scripting/src/scripting.cpp]

#### P3-M5-B: AI Behavior

##### P3-M5-B1: Behavior Tree
- **P3-M5-B1a**: Define tree nodes: Selector (try children in order until success), Sequence (run children in order until failure), Decorator (invert, repeat, cooldown), Action (leaf). [files: runtime/include/engine/runtime/behavior_tree.h]
- **P3-M5-B1b**: Tick tree: depth-first evaluate starting at root. Return Running/Success/Failure per node. [files: runtime/src/behavior_tree.cpp]
- **P3-M5-B1c**: Blackboard: per-entity key-value store for AI state (target entity, health, alertness). [files: runtime/src/blackboard.cpp]
- **P3-M5-B1d**: Built-in actions: MoveTo, Wait, PlayAnimation, LookAt, Attack, Flee. [files: runtime/src/bt_actions.cpp]
- **P3-M5-B1e**: Editor: behavior tree visual editor (node graph similar to P2-M7-A). [files: editor/src/bt_editor.cpp]

##### P3-M5-B2: Steering Behaviors
- **P3-M5-B2a**: Seek: move toward target. [files: runtime/src/steering.cpp]
- **P3-M5-B2b**: Flee: move away from target. [files: runtime/src/steering.cpp]
- **P3-M5-B2c**: Arrive: decelerate near target. [files: runtime/src/steering.cpp]
- **P3-M5-B2d**: Obstacle avoidance: local avoidance using raycast feelers. [files: runtime/src/steering.cpp]
- **P3-M5-B2e**: Separation / cohesion / alignment (flocking). [files: runtime/src/steering.cpp]
- **P3-M5-B2f**: Blend behaviors: weighted sum of steering forces. [files: runtime/src/steering.cpp]

##### P3-M5-B3: Lua AI API
- **P3-M5-B3a**: `engine.ai_move_to(entity, x, y, z)` — pathfind and follow. [files: scripting/src/scripting.cpp]
- **P3-M5-B3b**: `engine.ai_set_behavior_tree(entity, tree_asset)`. [files: scripting/src/scripting.cpp]
- **P3-M5-B3c**: `engine.ai_blackboard_set(entity, key, value)`, `engine.ai_blackboard_get(entity, key)`. [files: scripting/src/scripting.cpp]

**P3-M5 Exit Criteria**:
- NavMesh generated from scene geometry with configurable agent parameters.
- A* pathfinding with funnel smoothing, dynamic obstacles, off-mesh links.
- Behavior trees with selector/sequence/decorator/action and blackboard.
- Steering behaviors with flocking.
- Visual behavior tree editor.
- Full Lua AI API.

---

### P3-M6: Advanced Networking and Dedicated Server

**Goal**: Dedicated server mode, lag compensation, interest management, matchmaking, and anti-cheat basics.

**Dependencies**: P2-M4 (networking foundation).

#### P3-M6-A: Dedicated Server

##### P3-M6-A1: Headless Server Mode
- **P3-M6-A1a**: Add `-dedicated` command line flag: skip renderer, editor, audio initialization. [files: app/main.cpp]
- **P3-M6-A1b**: Server tick rate configurable (default 60Hz, common: 20/30/60/128). [files: runtime/src/network_session.cpp]
- **P3-M6-A1c**: Console-only interface: log output, command input via stdin. [files: app/main.cpp]

##### P3-M6-A2: Server Authority
- **P3-M6-A2a**: All game state mutations happen on server. Client sends input, server sends state. [files: runtime/src/replication.cpp]
- **P3-M6-A2b**: Server validates client actions (anti-cheat: speed check, position validation, cooldown enforcement). [files: runtime/src/server_validation.cpp]
- **P3-M6-A2c**: Server reconciliation messages: correct client prediction errors. [files: runtime/src/prediction.cpp]

#### P3-M6-B: Lag Compensation

##### P3-M6-B1: Server-Side Rewind
- **P3-M6-B1a**: Store N frames of world state history (ring buffer of entity transforms + health). [files: runtime/src/lag_compensation.cpp]
- **P3-M6-B1b**: On hit verification: rewind world to shooter's perceived time (client RTT/2 in the past), perform hit check. [files: runtime/src/lag_compensation.cpp]
- **P3-M6-B1c**: Maximum rewind limit (configurable, default 200ms). [files: runtime/src/lag_compensation.cpp]

#### P3-M6-C: Interest Management

##### P3-M6-C1: Relevancy System
- **P3-M6-C1a**: Per-client: only replicate entities within relevancy radius. [files: runtime/src/interest_manager.cpp]
- **P3-M6-C1b**: Priority: closer entities get higher update frequency. [files: runtime/src/interest_manager.cpp]
- **P3-M6-C1c**: AOI (Area of Interest) grid: divide world into cells, replicate entities in client's cell + neighbors. [files: runtime/src/interest_manager.cpp]
- **P3-M6-C1d**: Bandwidth budget: limit total bytes/sec per client. Prioritize within budget. [files: runtime/src/interest_manager.cpp]

#### P3-M6-D: Matchmaking and Lobby

##### P3-M6-D1: Simple Matchmaking
- **P3-M6-D1a**: Lobby server: REST API for game session listing (create, join, list, delete). [files: tools/lobby_server/ (separate executable)]
- **P3-M6-D1b**: Client: browse available games, join by choice or auto-match by skill/region. [files: runtime/src/matchmaking.cpp]
- **P3-M6-D1c**: Lua: `engine.find_match(options)`, `engine.create_lobby(settings)`. [files: scripting/src/scripting.cpp]

**P3-M6 Exit Criteria**:
- Dedicated server runs headless with configurable tick rate.
- Server-authoritative with input validation.
- Lag compensation with server-side rewind (max 200ms).
- Interest management limits bandwidth per client.
- Basic matchmaking via lobby server.

---

## Parallel Lanes (Can be done alongside any milestone)

These items do not block any milestone and can be picked up opportunistically:

### Documentation Lane
- **DOC-1**: Write `docs/getting_started.md` — clone, build, run first scene.
- **DOC-2**: Write `docs/architecture.md` — module dependency diagram, data flow.
- **DOC-3**: Write `docs/lua_api.md` — auto-generated from binding annotations.
- **DOC-4**: Write `docs/editor_guide.md` — annotated screenshots of each editor panel.
- **DOC-5**: Write `docs/asset_pipeline.md` — import formats, cooking process, metadata.
- **DOC-6**: Write `docs/networking.md` — authority model, replication, prediction.
- **DOC-7**: Write `docs/contributing.md` — code style, PR process, test requirements.

### Testing Lane
- **TEST-1**: Add fuzz tests for asset parsers (mesh, texture, audio).
- **TEST-2**: Add property-based tests for math library (QuickCheck-style).
- **TEST-3**: Add soak tests: 1-hour continuous play, measure memory growth.
- **TEST-4**: Add platform-specific integration tests for each target OS.
- **TEST-5**: Add screenshot comparison tests for renderer output (golden image diffs).

### DevOps Lane
- **DEVOPS-1**: Nightly build with extended test suite and soak tests.
- **DEVOPS-2**: Automated build for each platform target (cross-compilation CI).
- **DEVOPS-3**: Release pipeline: tag → build → package → publish artifacts.
- **DEVOPS-4**: Dependency update bot: alert on new SDL/Lua/miniaudio releases.

---

## Tracking Checklist

Use this checklist to track milestone completion at a glance:

### Phase 1
- [ ] P1-M1: Engine Production Baseline
- [ ] P1-M2: World, ECS, Gameplay Loop Foundation
- [ ] P1-M3: Physics Engine Hardening
- [ ] P1-M4: Asset Pipeline Production
- [ ] P1-M5: Renderer — Deferred Pipeline and Shadows
- [ ] P1-M6: Renderer — Sky, Fog, Instancing, Materials
- [ ] P1-M7: Animation System
- [ ] P1-M8: Audio Production
- [ ] P1-M9: Editor Production
- [ ] P1-M10: Scene Management and World Streaming
- [ ] P1-M11: UI System (Runtime Game UI)
- [ ] P1-M12: Platform, Packaging, Ship Readiness

### Phase 2
- [ ] P2-M1: Advanced Rendering
- [ ] P2-M2: VFX / Particle System
- [ ] P2-M3: 2D Engine
- [ ] P2-M4: Networking Foundation
- [ ] P2-M5: Splines, Data Tables, Gameplay Tools
- [ ] P2-M6: Controller Haptics and Advanced Input
- [ ] P2-M7: Advanced Editor Features
- [ ] P2-M8: Performance Polish and Profiling

### Phase 3
- [ ] P3-M1: XR / VR / AR
- [ ] P3-M2: Vulkan / Modern Graphics Backend
- [ ] P3-M3: Mobile Platform Support
- [ ] P3-M4: Web / Emscripten Build
- [ ] P3-M5: AI and Navigation
- [ ] P3-M6: Advanced Networking and Dedicated Server

### Parallel Lanes
- [ ] DOC-1 through DOC-7: Documentation
- [ ] TEST-1 through TEST-5: Extended Testing
- [ ] DEVOPS-1 through DEVOPS-4: DevOps

---

**Total atomic tasks**: ~600+
**Estimated milestones**: 26 (12 P1 + 8 P2 + 6 P3)
**Hierarchy depth**: Phase → Milestone → Sub-milestone → Work Package → Atomic Task (5 levels)
