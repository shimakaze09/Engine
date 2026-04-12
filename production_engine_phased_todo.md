# Production Engine Phased Todo

> Source: production_engine_milestones.md (atomic execution plan)
> Gap inventory: production_engine_gap_list.md
>
> Every checkbox maps 1:1 to an atomic task in the milestones document.
> Mark [x] ONLY when the 7-point completion standard is met (see copilot-instructions.md).
> Items marked [~] are partially implemented — treat as incomplete.

---

## Phase 1: Ship Blockers

Everything in Phase 1 must be complete before a game can be shipped.

---

### P1-M1: Engine Production Baseline

**Dependencies**: None (first milestone)

#### P1-M1-A: Build System Hardening

- [x] P1-M1-A1a: ENGINE_TARGET_PLATFORM CMake option (Win64/Linux/macOS/Android/iOS/Web)
- [x] P1-M1-A1b: Cross-compile toolchain stubs (Android NDK, iOS Xcode, Emscripten)
- [x] P1-M1-A1c: Single cmake configure builds all modules cleanly (zero warnings)
- [x] P1-M1-A2a: core/src/pch.h with essential includes
- [x] P1-M1-A2b: target_precompile_headers for engine_core
- [x] P1-M1-A2c: PCH for renderer module
- [x] P1-M1-A3a: Audit all modules for unnecessary header includes
- [x] P1-M1-A3b: #pragma once audit (no mixed include guards)
- [x] P1-M1-A3c: Measure rebuild time after touching single core header

#### P1-M1-B: CI Pipeline

- [x] P1-M1-B1a: .github/workflows/ci.yml — build matrix (3 OS × 2 configs)
- [x] P1-M1-B1b: Steps: checkout → configure → build → test, cache _deps/
- [x] P1-M1-B1c: Build artifact upload (compile_commands.json)
- [x] P1-M1-B2a: cppcheck CI job (fail on new warning)
- [x] P1-M1-B2b: clang-tidy CI job with .clang-tidy config
- [x] P1-M1-B2c: -Werror / /WX verification in CI
- [x] P1-M1-B3a: ASAN+UBSAN sanitizer CI lane
- [x] P1-M1-B3b: TSAN CI lane (separate from ASAN)
- [x] P1-M1-B3c: Sanitizer suppressions for third-party (SDL, Lua)
- [x] P1-M1-B4a: --coverage build in CI (gcov/llvm-cov)
- [x] P1-M1-B4b: Generate coverage HTML report + upload artifact
- [x] P1-M1-B4c: Coverage threshold gate (fail if below X%)
- [x] P1-M1-B5a: ECS perf benchmark (50K entities iterate time)
- [x] P1-M1-B5b: Physics perf benchmark (1000-body step time)
- [x] P1-M1-B5c: CI perf regression check (>10% regression = fail)

#### P1-M1-C: Determinism and Replay

- [x] P1-M1-C1a: Cross-platform determinism test (hash final state on all CI platforms)
- [x] P1-M1-C1b: Thread-count independence (hashes match for 1–8 workers)
- [x] P1-M1-C1c: Document FP strictness flags per platform in CMake
- [x] P1-M1-C2a: Raise kMaxEntities to 65536 or configurable
- [x] P1-M1-C2b: Stress test: 50K+ entities with Transform+RigidBody
- [x] P1-M1-C2c: Verify no OOM, iteration < 16ms at 50K

#### P1-M1-D: Memory and Profiling

- [x] P1-M1-D1a: Audit all new/malloc in per-frame paths
- [x] P1-M1-D1b: Replace hot-path allocations with frame_allocator / thread_frame_allocator
- [x] P1-M1-D1c: core::PoolAllocator<T,N> template
- [x] P1-M1-D2a: Hierarchical profiler tree (parent pointer per entry)
- [x] P1-M1-D2b: PROFILE_SCOPE("name") RAII macro
- [x] P1-M1-D2c: Flame graph in editor debug panel
- [x] P1-M1-D3a: GpuTimerQuery wrapper (glQueryCounter GL_TIMESTAMP)
- [x] P1-M1-D3b: Instrument each render pass with GPU timer queries
- [x] P1-M1-D3c: Display GPU pass timings in editor stats overlay
- [x] P1-M1-D4a: core::EngineStats struct (fps, frame_time, draw_calls, etc.)
- [x] P1-M1-D4b: Populate stats from renderer, runtime, OS
- [x] P1-M1-D4c: ImGui stats overlay toggled by r_showStats CVar

#### P1-M1-E: Debug Utilities

- [x] P1-M1-E1a: Debug camera (free-fly WASD+mouse)
- [x] P1-M1-E1b: Toggle via debug.camera_detach CVar
- [x] P1-M1-E1c: Render frozen game camera frustum as wireframe
- [x] P1-M1-E2a: Console command: god
- [x] P1-M1-E2b: Console command: noclip
- [x] P1-M1-E2c: Console command: spawn <prefab> [x y z]
- [x] P1-M1-E2d: Console command: kill_all
- [x] P1-M1-E3a: Per-subsystem memory tags (Physics, Renderer, Audio, etc.)
- [x] P1-M1-E3b: Thread-safe atomic counters per tag
- [x] P1-M1-E3c: Memory breakdown bar chart in stats overlay

---

### P1-M2: World, ECS, Gameplay Loop Foundation

**Dependencies**: P1-M1

#### P1-M2-A: Actor / Component Lifecycle

- [x] P1-M2-A1a: WorldPhase::BeginPlay between entity creation and first Simulation
- [x] P1-M2-A1b: WorldPhase::EndPlay before entity destruction
- [x] P1-M2-A1c: destroy_entity defers to end of frame
- [x] P1-M2-A1d: Integration test: BeginPlay→Tick×3→EndPlay lifecycle
- [x] P1-M2-A2a: Lua on_begin_play / on_tick / on_end_play callbacks
- [x] P1-M2-A2b: Per-entity callback function refs in table
- [x] P1-M2-A2c: Error handling: pcall + traceback, faulted entity skip
- [x] P1-M2-A2d: Test: Lua counter in on_tick matches frame count
- [x] P1-M2-A3a: EntityPool class (pre-alloc, acquire, release, free list)
- [x] P1-M2-A3b: World::spawn_from_pool / destroy_to_pool
- [x] P1-M2-A3c: Lua engine.pool_create / pool_spawn / pool_release
- [x] P1-M2-A3d: Test: 100 spawn, 100 release, 100 re-spawn verifies handle reuse

#### P1-M2-B: Game State Architecture

- [x] P1-M2-B1a: GameMode struct (state enum, rules table, owned by World)
- [x] P1-M2-B1b: GameState struct (persistent cross-scene data)
- [x] P1-M2-B1c: PlayerController struct (input → entity action mapping)
- [x] P1-M2-B1d: Lua bindings: get_game_mode, get_game_state, get_player_controller
- [x] P1-M2-B1e: Integration test: state transitions fire callbacks
- [x] P1-M2-B2a: core::ServiceLocator type-erased registry
- [x] P1-M2-B2b: Register all singletons as services at startup
- [x] P1-M2-B2c: Migrate globals in scripting.cpp to ServiceLocator
- [x] P1-M2-B2d: Unit test: register, retrieve, overwrite, type safety

#### P1-M2-C: Input System

- [x] P1-M2-C1a: InputAction struct (name, bound keys/buttons, callbacks)
- [x] P1-M2-C1b: InputAxisMapping struct (axis, scale, dead zone)
- [x] P1-M2-C1c: InputMapper class (JSON config, process raw → actions)
- [x] P1-M2-C1d: Integrate with SDL event loop in input.cpp
- [x] P1-M2-C1e: Unit test: bind key → simulate press → verify callback
- [x] P1-M2-C2a: InputMapper::rebind(action_name, new_key) runtime
- [x] P1-M2-C2b: InputMapper::save_bindings/load_bindings (JSON persist)
- [x] P1-M2-C2c: Lua engine.rebind_action / engine.save_input_config
- [x] P1-M2-C2d: Test: rebind → new key triggers → save/load roundtrip
- [x] P1-M2-C3a: TouchEvent struct (SDL_FINGER events), process touch stream
- [x] P1-M2-C3b: Gesture recognizers (tap, swipe, pinch, rotate)
- [x] P1-M2-C3c: Gestures bind to InputMapper actions
- [x] P1-M2-C3d: Touch-to-mouse emulation fallback
- [x] P1-M2-C3e: Lua engine.on_touch / engine.on_gesture
- [x] P1-M2-C3f: Test: simulate multi-touch → verify gesture callbacks

#### P1-M2-D: Timer System

- [x] P1-M2-D1a: TimerManager class (per-World, fixed-capacity)
- [x] P1-M2-D1b: set_timeout / set_interval / cancel
- [x] P1-M2-D1c: Tick timers in World::update simulation phase
- [x] P1-M2-D1d: Serialization: save/load active timers in scene JSON
- [x] P1-M2-D2a: Lua engine.set_timeout / engine.set_interval / engine.cancel_timer
- [x] P1-M2-D2b: Timer callbacks via pcall with error handling
- [x] P1-M2-D2c: Test: set timeout 0.5s, advance 30 frames at 60fps, verify fires

#### P1-M2-E: Gameplay Camera

- [x] P1-M2-E1a: SpringArmComponent struct (POD, SparseSet)
- [x] P1-M2-E1b: SparseSet storage + CRUD in World
- [x] P1-M2-E1c: Spring arm sweep + shorten on collision + lag smooth
- [x] P1-M2-E1d: Reflection, serialization, Lua binding
- [x] P1-M2-E2a: CameraManager (priority stack of cameras)
- [x] P1-M2-E2b: push_camera / pop_camera with blend interpolation
- [x] P1-M2-E2c: Lua engine.push_camera / engine.get_active_camera
- [x] P1-M2-E3a: CameraShake struct (amplitude, frequency, duration, decay)
- [x] P1-M2-E3b: Perlin noise-based shake (offset position + rotation)
- [x] P1-M2-E3c: Stack multiple shakes additively
- [x] P1-M2-E3d: Lua engine.camera_shake(amplitude, freq, duration)
- [x] P1-M2-E3e: Test: shake offset ≠ 0 during duration, == 0 after

#### P1-M2-F: Coroutines / Async Gameplay

- [x] P1-M2-F1a: CoroutineScheduler class (list of suspended Lua coroutines)
- [x] P1-M2-F1b: engine.wait(seconds) — yield and resume after elapsed time
- [x] P1-M2-F1c: engine.wait_until(condition) — yield and resume when true
- [x] P1-M2-F1d: engine.wait_frames(n) — yield and resume after N frames
- [x] P1-M2-F1e: Tick scheduler each frame (check conditions, resume)
- [x] P1-M2-F1f: Error handling: pcall resume + traceback, remove faulted
- [x] P1-M2-F1g: Test: wait(0.5) then set flag, verify at correct frame

#### P1-M2-G: Script Runtime Safety

- [x] P1-M2-G1a: DAP JSON-RPC transport over TCP socket
- [x] P1-M2-G1b: Handle initialize / launch / configurationDone
- [x] P1-M2-G1c: setBreakpoints via lua_sethook
- [x] P1-M2-G1d: continue / next / stepIn / stepOut
- [x] P1-M2-G1e: stackTrace — walk Lua call stack
- [x] P1-M2-G1f: scopes + variables (locals, upvalues, globals)
- [x] P1-M2-G1g: evaluate expression in paused context
- [x] P1-M2-G1h: Test: mock DAP client → breakpoint → verify pause at line
- [x] P1-M2-G2a: Per-script sandbox (restricted globals, no io/os.execute)
- [x] P1-M2-G2b: Allow-list safe functions (math, string, table, engine.*)
- [x] P1-M2-G2c: CPU instruction limit per script per frame
- [x] P1-M2-G2d: Memory limit per script environment
- [x] P1-M2-G2e: Test: io.open→error, infinite loop→terminate, huge alloc→fail
- [x] P1-M2-G3a: Snapshot script state before reload (engine.persist vars)
- [x] P1-M2-G3b: Re-execute modified scripts in fresh sandbox
- [x] P1-M2-G3c: Restore snapshots, re-register callbacks
- [x] P1-M2-G3d: On error: revert to previous version
- [x] P1-M2-G3e: Test: modify script → reload → verify state survives
- [x] P1-M2-G4a: Binding generator tool: parse annotated C++ headers
- [x] P1-M2-G4b: Annotation syntax: // LUA_BIND: func(args) -> return
- [x] P1-M2-G4c: Generator emits: validation, conversion, call, push return
- [x] P1-M2-G4d: CMake pre-build step → generated_bindings.cpp
- [x] P1-M2-G4e: Migrate ≥20 hand-written bindings to generated
- [x] P1-M2-G4f: Test: bindgen processes header → valid C++ compiles

---

### P1-M3: Physics Engine Hardening

**Dependencies**: P1-M1, P1-M2

#### P1-M3-A: Collision Shapes

- [x] P1-M3-A1a: CapsuleCollider struct (half_height, radius), SparseSet storage
- [x] P1-M3-A1b: Capsule-vs-AABB narrow phase (closest point on segment to box)
- [x] P1-M3-A1c: Capsule-vs-sphere narrow phase
- [x] P1-M3-A1d: Capsule-vs-capsule narrow phase (segment-segment closest point)
- [x] P1-M3-A1e: Spatial hash broadphase: compute capsule AABB for insertion
- [x] P1-M3-A1f: Lua binding: engine.add_capsule_collider(entity, half_height, radius)
- [x] P1-M3-A1g: Test: capsule-vs-AABB, capsule-vs-sphere, capsule-vs-capsule contact generation
- [x] P1-M3-A2a: ConvexHullCollider struct (planes array ≤64, vertices, AABB cache)
- [x] P1-M3-A2b: Convex hull builder (quickhull → half-edge → plane array)
- [x] P1-M3-A2c: GJK/EPA for convex-vs-convex narrow phase
- [x] P1-M3-A2d: Convex-vs-sphere, convex-vs-capsule using GJK support functions
- [x] P1-M3-A2e: Cook convex hull at asset import time (asset packer)
- [x] P1-M3-A2f: Test: cube mesh → same result as AABB; tetrahedron contacts correct
- [x] P1-M3-A3a: HeightfieldCollider struct (2D height array, x/z spacing, min/max y)
- [x] P1-M3-A3b: Ray-vs-heightfield (grid march + bilinear interpolation)
- [x] P1-M3-A3c: AABB-vs-heightfield (overlapping grid cells, contact per cell)
- [x] P1-M3-A3d: Sphere-vs-heightfield (closest point on triangle fan at grid cell)
- [x] P1-M3-A3e: Test: flat heightfield behaves like plane; single bump deflects sphere

#### P1-M3-B: Constraint Solver

- [x] P1-M3-B1a: ConstraintSolver class (accumulate constraints, iterate N times, apply impulse)
- [x] P1-M3-B1b: Warm starting (cache impulse from previous frame, apply before first iteration)
- [x] P1-M3-B1c: CVar physics.solver_iterations (default 8)
- [x] P1-M3-B2a: Hinge joint (constrain rotation to one axis, optional angle limits)
- [x] P1-M3-B2b: Ball-socket joint (constrain position, free rotation)
- [x] P1-M3-B2c: Slider joint (constrain to one axis, optional distance limits)
- [x] P1-M3-B2d: Spring joint (distance constraint, configurable stiffness/damping)
- [x] P1-M3-B2e: Fixed joint (zero relative motion, welding)
- [x] P1-M3-B2f: Lua bindings for all joint types (engine.add_hinge_joint etc.)
- [x] P1-M3-B2g: Test per joint: verify constraint holds under load, limits respected
- [x] P1-M3-B3a: Persistent contact manifold (match contacts across frames by feature ID)
- [x] P1-M3-B3b: Manifold reduction (keep ≤4 contacts per pair, maximize contact area)
- [x] P1-M3-B3c: Test: box resting on box retains 4 contacts across 100 frames

#### P1-M3-C: Physics Materials and Layers

- [x] P1-M3-C1a: PhysicsMaterial struct (static/dynamic friction, restitution, density)
- [x] P1-M3-C1b: Assign material per collider (default material if unset)
- [x] P1-M3-C1c: Material combination rules (friction = sqrt(a*b), restitution = max(a,b))
- [x] P1-M3-C1d: Lua binding: engine.create_physics_material / engine.set_collider_material
- [x] P1-M3-C2a: collision_layer and collision_mask uint32 per collider (bit flags)
- [x] P1-M3-C2b: Broadphase filters pairs by (a.layer & b.mask) && (b.layer & a.mask)
- [x] P1-M3-C2c: Lua binding: engine.set_collision_layer / engine.set_collision_mask
- [x] P1-M3-C2d: Test: entity on layer 2, mask excludes layer 2 → no collision

#### P1-M3-D: Physics Queries

- [x] P1-M3-D1a: PhysicsWorld::raycast(origin, direction, max_distance, mask) → RaycastHit[]
- [x] P1-M3-D1b: RaycastHit struct (entity, position, normal, distance, surface material)
- [x] P1-M3-D1c: Broadphase ray-vs-cell enumeration, narrow per-collider ray test
- [x] P1-M3-D1d: Sort results by distance, closest-only option (early out)
- [x] P1-M3-D1e: Lua binding: engine.raycast(ox,oy,oz, dx,dy,dz, max_dist) → table of hits
- [x] P1-M3-D1f: Test: ray through 3 aligned spheres returns 3 hits sorted by distance
- [x] P1-M3-D2a: PhysicsWorld::overlap_sphere(center, radius, mask) → Entity[]
- [x] P1-M3-D2b: PhysicsWorld::overlap_box(center, half_extents, rotation, mask) → Entity[]
- [x] P1-M3-D2c: Lua bindings: engine.overlap_sphere / engine.overlap_box
- [x] P1-M3-D2d: Test: 10 entities in cluster, overlap sphere catches correct subset
- [x] P1-M3-D3a: PhysicsWorld::sweep_sphere(origin, radius, direction, distance, mask) → SweepHit
- [x] P1-M3-D3b: PhysicsWorld::sweep_box(center, half_extents, rotation, direction, distance, mask) → SweepHit
- [x] P1-M3-D3c: SweepHit struct (entity, contact_point, normal, distance, time_of_impact)
- [x] P1-M3-D3d: Lua bindings: engine.sweep_sphere / engine.sweep_box
- [x] P1-M3-D3e: Test: sweep sphere through corridor, hits wall at correct distance

#### P1-M3-E: CCD Hardening

- [x] P1-M3-E1a: Bilateral advancement algorithm (Erwin Coumans GDC style)
- [x] P1-M3-E1b: Sphere-vs-mesh CCD (sweep + refine)
- [x] P1-M3-E1c: CVar physics.ccd_threshold (minimum velocity to trigger CCD)
- [x] P1-M3-E1d: Test: fast bullet vs thin wall — no tunneling at 300 m/s
- [x] P1-M3-E2a: Speculative contact generation (expand AABB by velocity*dt, detect pre-penetration)
- [x] P1-M3-E2b: Clamp speculative impulse to prevent ghost collisions
- [x] P1-M3-E2c: Test: ball rolling toward wall stops without visible penetration frame

---

### P1-M4: Asset Pipeline Production

**Dependencies**: P1-M1, P1-M3

#### P1-M4-A: Asset Database

- [ ] P1-M4-A1a: Replace 32-bit FNV hash with 64-bit FNV-1a or xxHash64 for asset IDs
- [ ] P1-M4-A1b: Update all AssetId typedefs from uint32_t to uint64_t, audit comparisons
- [ ] P1-M4-A1c: Migrate existing asset registry files to 64-bit IDs
- [ ] P1-M4-A1d: Test: hash 100K random asset paths, verify zero collisions
- [ ] P1-M4-A2a: AssetMetadata struct (asset_id, type_tag, file_path, file_size, checksum, tags[])
- [ ] P1-M4-A2b: Store metadata in sidecar .meta JSON files alongside source assets
- [ ] P1-M4-A2c: Tag system: queryable from editor and runtime
- [ ] P1-M4-A2d: Import settings per asset type (mesh: scale/up-axis/normals, texture: format/mip/sRGB)
- [ ] P1-M4-A3a: Mesh thumbnail: render 64×64 offscreen, store as PNG in .thumbnails/
- [ ] P1-M4-A3b: Texture thumbnail: downscale to 64×64
- [ ] P1-M4-A3c: Load thumbnails in editor asset browser, fallback to type icon
- [ ] P1-M4-A3d: Invalidate thumbnail on source asset change (mtime check)

#### P1-M4-B: Dependency Graph

- [ ] P1-M4-B1a: DependencyGraph class (DAG of AssetId → AssetId edges)
- [ ] P1-M4-B1b: Populate during import (mesh→materials→textures, prefab→meshes)
- [ ] P1-M4-B1c: Persist graph to build/asset_deps.json, incremental rebuild on dependency change
- [ ] P1-M4-B1d: Test: change texture, verify dependent material and mesh recooked
- [ ] P1-M4-B2a: AssetDatabase::get_dependencies(asset_id) → AssetId[]
- [ ] P1-M4-B2b: Load dependencies first (recursive), track order to prevent cycles
- [ ] P1-M4-B2c: Test: load prefab, verify mesh and textures loaded first

#### P1-M4-C: Async Streaming

- [ ] P1-M4-C1a: AssetLoadThread using job system (separate worker pool for IO)
- [ ] P1-M4-C1b: AssetDatabase::load_async(asset_id) → LoadHandle (is_ready, get<T>, wait)
- [ ] P1-M4-C1c: Loading states: Queued → Loading → Uploading → Ready
- [ ] P1-M4-C1d: Test: queue 50 mesh loads, poll until ready, verify all loaded correctly
- [ ] P1-M4-C2a: Priority levels: Immediate / High / Normal / Low
- [ ] P1-M4-C2b: Load thread processes highest priority first, priority updatable while queued
- [ ] P1-M4-C2c: Lua binding: engine.load_asset_async(path, priority) → handle
- [ ] P1-M4-C3a: CVar asset.streaming_budget_mb (max memory for in-flight loads per frame)
- [ ] P1-M4-C3b: CVar asset.max_uploads_per_frame (limit GPU uploads per frame)
- [ ] P1-M4-C3c: Test: queue more than budget allows, verify spreading across frames

#### P1-M4-D: LRU Eviction

- [ ] P1-M4-D1a: last_access_frame field per loaded asset entry, update on access
- [ ] P1-M4-D1b: Doubly-linked list sorted by access time (most recent at tail)
- [ ] P1-M4-D1c: evict() removes head of list (least recently used), free GPU + CPU memory
- [ ] P1-M4-D2a: CVar asset.cache_size_mb — evict until below target when exceeded
- [ ] P1-M4-D2b: Protected assets: ref count > 0 cannot be evicted, skip in LRU
- [ ] P1-M4-D2c: Eviction callback: notify systems before evicting (renderer drops GPU handles)
- [ ] P1-M4-D2d: Test: load 100 assets into 50-asset cache, access subset, verify LRU evicts stale ones

#### P1-M4-E: Deterministic Cooking

- [ ] P1-M4-E1a: Byte-identical output (remove timestamps, sort deterministically)
- [ ] P1-M4-E1b: Content-hash (not mtime) for rebuild decision when available
- [ ] P1-M4-E1c: Test: cook, cook again unchanged, diff output — must be zero
- [ ] P1-M4-E2a: All import settings stored in .meta file, change triggers recook
- [ ] P1-M4-E2b: Editor UI: inspector for selected asset shows import settings
- [ ] P1-M4-E2c: Test: change mesh scale in meta, recook, verify mesh vertices scaled

---

### P1-M5: Renderer — Deferred Pipeline and Shadows

**Dependencies**: P1-M1, P1-M4

#### P1-M5-A: G-Buffer

- [ ] P1-M5-A1a: G-Buffer layout (RT0=albedo+metallic, RT1=normal+roughness, RT2=emissive+AO, depth24_stencil8)
- [ ] P1-M5-A1b: Create FBO with MRT using glDrawBuffers
- [ ] P1-M5-A1c: FBO completeness check and error reporting
- [ ] P1-M5-A2a: gbuffer.vert — transform position, pass TBN matrix, UV
- [ ] P1-M5-A2b: gbuffer.frag — sample textures, write to G-Buffer MRT targets
- [ ] P1-M5-A2c: Handle missing textures (default 1×1 white albedo, flat normal, etc.)
- [ ] P1-M5-A3a: CVar r_gbuffer_debug (0=off, 1=albedo, 2=normals, 3=metallic, 4=roughness, 5=depth)
- [ ] P1-M5-A3b: gbuffer_debug.frag — fullscreen quad reading selected G-Buffer target
- [ ] P1-M5-A3c: Render debug view as fullscreen pass when CVar set

#### P1-M5-B: Deferred Lighting

- [ ] P1-M5-B1a: deferred_lighting.frag — reconstruct world pos, PBR BRDF (Cook-Torrance)
- [ ] P1-M5-B1b: Fullscreen triangle draw using fullscreen.vert
- [ ] P1-M5-B1c: Output to HDR render target (RGBA16F)
- [ ] P1-M5-B2a: PointLight struct (position, color, intensity, radius), SparseSet in World
- [ ] P1-M5-B2b: Upload point lights as uniform array (max 128)
- [ ] P1-M5-B2c: Deferred shader iterates lights, inverse square falloff + radius cutoff
- [ ] P1-M5-B2d: Frustum-cull lights (skip if bounding sphere outside camera frustum)
- [ ] P1-M5-B2e: Lua binding: engine.add_point_light(entity, r,g,b, intensity, radius)
- [ ] P1-M5-B3a: SpotLight struct (position, direction, color, intensity, inner/outer angle, range)
- [ ] P1-M5-B3b: Deferred shader: spot light contribution with cone falloff
- [ ] P1-M5-B3c: Frustum-cull spot lights by bounding cone approximation
- [ ] P1-M5-B3d: Lua binding: engine.add_spot_light(entity, ...)
- [ ] P1-M5-B4a: Tiled light culling: divide screen into 16×16 tiles, compute min/max depth
- [ ] P1-M5-B4b: Per-tile: test each light sphere against tile frustum, store per-tile light list
- [ ] P1-M5-B4c: Deferred shader reads per-tile light list, only iterates relevant lights
- [ ] P1-M5-B4d: Test: 256 lights scene, tile-culled vs brute-force pixel-identical

#### P1-M5-C: Shadow Mapping

- [ ] P1-M5-C1a: Shadow map FBO: depth-only texture array (4 cascades × 2048×2048)
- [ ] P1-M5-C1b: Cascade splits: practical split scheme (logarithmic + linear blend)
- [ ] P1-M5-C1c: Per cascade: light-space orthographic projection enclosing cascade frustum
- [ ] P1-M5-C1d: Render scene from light's view per cascade (depth-only pass)
- [ ] P1-M5-C1e: shadow_depth.vert — minimal vertex shader (transform by light MVP)
- [ ] P1-M5-C1f: Deferred lighting: sample shadow map, compare depth, shadow factor
- [ ] P1-M5-C1g: PCF: 3×3 kernel for soft shadow edges
- [ ] P1-M5-C1h: CVar r_shadow_resolution (512, 1024, 2048, 4096)
- [ ] P1-M5-C2a: Per shadow-casting spot light: allocate shadow map slice (perspective)
- [ ] P1-M5-C2b: Render from spot light's view, sample in deferred shader
- [ ] P1-M5-C2c: CVar r_max_shadow_casting_lights (default 4)
- [ ] P1-M5-C3a: Cubemap shadow map for point lights (6-face FBO per light)
- [ ] P1-M5-C3b: Geometry shader or multi-pass to render all 6 faces
- [ ] P1-M5-C3c: Sample cubemap in deferred shader using world direction to light
- [ ] P1-M5-C3d: Limit point light shadows to N closest (CVar r_max_point_shadows, default 2)
- [ ] P1-M5-C4a: Stable cascades: snap projection to texel grid to eliminate shimmer
- [ ] P1-M5-C4b: Shadow cache: re-render only if camera/light changes significantly
- [ ] P1-M5-C4c: Shadow LOD: distant cascades use lower resolution
- [ ] P1-M5-C4d: Visual test: rotating light, no shimmer, no peter-panning

#### P1-M5-D: HDR Post-Processing

- [ ] P1-M5-D1a: PostProcessStack class (ordered list of passes, enable/disable)
- [ ] P1-M5-D1b: Ping-pong between two HDR RTs per pass
- [ ] P1-M5-D1c: CVar per pass: r_bloom_enabled, r_ssao_enabled, r_fxaa_enabled, etc.
- [ ] P1-M5-D2a: Bloom brightness filter: extract pixels above r_bloom_threshold
- [ ] P1-M5-D2b: Downsample chain: 5 levels half-res, 13-tap filter
- [ ] P1-M5-D2c: Upsample chain: bilinear + tent filter back to full res
- [ ] P1-M5-D2d: Composite: additive blend bloom into HDR scene RT, r_bloom_intensity
- [ ] P1-M5-D2e: Test: bright sphere on dark background, verify bloom glow
- [ ] P1-M5-D3a: SSAO: sample hemisphere around pixel normal, compare depths
- [ ] P1-M5-D3b: Random kernel (64 samples) and 4×4 noise texture
- [ ] P1-M5-D3c: Bilateral blur pass (preserve edges via normal/depth comparison)
- [ ] P1-M5-D3d: Multiply SSAO result into ambient lighting term
- [ ] P1-M5-D3e: CVars: r_ssao_radius, r_ssao_samples, r_ssao_intensity
- [~] P1-M5-D4a: Tone map operators: Reinhard, ACES Filmic, Uncharted 2 (CVar r_tonemap_operator)
- [~] P1-M5-D4b: Auto-exposure: compute average luminance via downsampling, adapt over time
- [~] P1-M5-D4c: CVar r_exposure manual override, r_auto_exposure toggle
- [ ] P1-M5-D5a: FXAA 3.11 as post-process pass
- [ ] P1-M5-D5b: CVar r_aa_mode (0=none, 1=FXAA)
- [ ] P1-M5-D5c: Test: aliased edge → verify FXAA smooths jaggies

#### P1-M5-E: Forward Transparency

- [ ] P1-M5-E1a: Render transparent objects in forward pass after deferred lighting
- [ ] P1-M5-E1b: Sort transparent draw calls back-to-front by camera distance
- [ ] P1-M5-E1c: Alpha blend into HDR scene RT, depth write OFF, depth test ON
- [ ] P1-M5-E2a: forward_transparent.frag — PBR BRDF, single-pass with light array
- [ ] P1-M5-E2b: Sample shadow maps in forward pass for transparent shadows
- [ ] P1-M5-E2c: Lua binding: engine.set_material_transparent(entity, alpha)

---

### P1-M6: Renderer — Sky, Fog, Instancing, Materials

**Dependencies**: P1-M5

#### P1-M6-A: Sky and Atmosphere

- [ ] P1-M6-A1a: SkyboxComponent: cubemap texture handle (6-face or equirect)
- [ ] P1-M6-A1b: skybox.vert — centered unit cube, view matrix with translation removed
- [ ] P1-M6-A1c: skybox.frag — sample cubemap by vertex direction
- [ ] P1-M6-A1d: Render skybox as last opaque pass, depth LE, depth write OFF
- [ ] P1-M6-A1e: Asset import: HDR equirect → cubemap at import time
- [ ] P1-M6-A1f: Lua binding: engine.set_skybox(cubemap_path)
- [ ] P1-M6-A2a: Preetham sky model (sun direction → sky color gradient)
- [ ] P1-M6-A2b: Parameters: sun_direction, turbidity, ground_albedo (CVar or Lua)
- [ ] P1-M6-A2c: Render on fullscreen dome or inverted sphere
- [ ] P1-M6-A2d: Sky color influences ambient light
- [ ] P1-M6-A2e: Lua binding: engine.set_procedural_sky(turbidity, sun_dir_x/y/z)
- [ ] P1-M6-A3a: Capture environment cubemap from probe position (6-face render)
- [ ] P1-M6-A3b: Prefilter cubemap for roughness levels (split-sum IBL)
- [ ] P1-M6-A3c: Generate BRDF LUT texture (Schlick-GGX integration)
- [ ] P1-M6-A3d: Sample prefiltered env map in deferred lighting for specular IBL
- [ ] P1-M6-A3e: Multiple probes: blend by proximity or volume
- [ ] P1-M6-A3f: Editor: place probe, capture on demand, show proxy sphere

#### P1-M6-B: Environment Fog

- [ ] P1-M6-B1a: Post-process pass: linear distance fog, blend toward fog color
- [ ] P1-M6-B1b: Exponential and exponential-squared falloff modes
- [ ] P1-M6-B1c: CVars: r_fog_enabled, r_fog_color, r_fog_density, r_fog_start/end, r_fog_mode
- [ ] P1-M6-B2a: Height-based density falloff (peaks at r_fog_height_base, decays upward)
- [ ] P1-M6-B2b: Ray-march from camera to pixel to integrate density
- [ ] P1-M6-B2c: Lua binding: engine.set_fog(density, start, end, color, height_base)

#### P1-M6-C: GPU Instancing

- [ ] P1-M6-C1a: Group draw calls by mesh+material, use glDrawElementsInstanced for >1 instance
- [ ] P1-M6-C1b: Upload instance data (model matrices) via SSBO or instance VBO
- [ ] P1-M6-C1c: Vertex shader reads per-instance model matrix
- [ ] P1-M6-C1d: Frustum cull individual instances before upload (CPU-side)
- [ ] P1-M6-C1e: Test: 10K identical boxes, draw call count = 1 (instanced)
- [ ] P1-M6-C2a: Foliage instance buffer (model matrix + wind phase/amplitude)
- [ ] P1-M6-C2b: Foliage vertex shader: vertex-displacement wind (sin(time + phase))
- [ ] P1-M6-C2c: LOD selection per instance based on camera distance
- [ ] P1-M6-C2d: Lua binding: engine.add_foliage_instance(mesh, material, position, scale)

#### P1-M6-D: Material and Shader Variants

- [ ] P1-M6-D1a: Macro-based variant keys: HAS_NORMAL_MAP, HAS_EMISSIVE, ALPHA_TEST, SKINNED
- [ ] P1-M6-D1b: Compute variant bitmask from material features at load
- [ ] P1-M6-D1c: Compile variant on first request (#define active features, cache in hash map)
- [ ] P1-M6-D1d: Warm cache at load time for known material variants
- [ ] P1-M6-D1e: Test: material with/without normal map uses different shader, both render correctly
- [ ] P1-M6-D2a: MaterialTemplate — base shader + default parameters
- [ ] P1-M6-D2b: MaterialInstance — references template, overrides specific parameters
- [ ] P1-M6-D2c: Instances share compiled shader program, differ only in uniform values
- [ ] P1-M6-D2d: Lua binding: engine.create_material_instance(template_path, overrides_table)
- [ ] P1-M6-D2e: Editor: material inspector with template + per-instance overrides

#### P1-M6-E: Render-to-Texture

- [ ] P1-M6-E1a: RenderTarget class (FBO + color/depth attachments, configurable resolution/format)
- [ ] P1-M6-E1b: Pool of render targets by resolution bucket
- [ ] P1-M6-E2a: SceneCaptureComponent (camera params, target RT, capture frequency)
- [ ] P1-M6-E2b: Render scene from capture camera via full deferred pipeline
- [ ] P1-M6-E2c: Expose captured texture for sampling on materials (mirrors, minimap)
- [ ] P1-M6-E2d: Lua binding: engine.create_scene_capture / engine.capture_now
- [ ] P1-M6-E2e: Test: scene capture produces non-black texture with correct dimensions

---

### P1-M7: Animation System

**Dependencies**: P1-M4, P1-M5

#### P1-M7-A: Skeleton and Clip Data

- [ ] P1-M7-A1a: Skeleton struct (joints array: name, parent_index, inverse_bind_pose, max 256)
- [ ] P1-M7-A1b: Load skeleton from glTF skin data in asset packer
- [ ] P1-M7-A1c: Store cooked skeleton as binary blob (joint count + flat arrays)
- [ ] P1-M7-A1d: Runtime loader: read skeleton binary, populate Skeleton struct
- [ ] P1-M7-A2a: AnimationClip struct (duration, sample_rate, channels per joint: pos/rot/scale)
- [ ] P1-M7-A2b: Load from glTF animation data (extract channels per joint per clip)
- [ ] P1-M7-A2c: Cook to compressed binary (quantize rotations smallest-3, delta-encode positions)
- [ ] P1-M7-A2d: Runtime loader: decompress clip into playback-ready format
- [ ] P1-M7-A2e: Test: roundtrip compress/decompress, error < 0.001 per joint
- [ ] P1-M7-A3a: sample_clip(clip, time) → JointPose[] (binary search, lerp/slerp)
- [ ] P1-M7-A3b: Handle looping: wrap time past duration
- [ ] P1-M7-A3c: Handle non-looping: clamp at last frame

#### P1-M7-B: Pose Blending

- [ ] P1-M7-B1a: blend_poses(poseA, poseB, alpha) → blended_pose (lerp pos/scale, slerp rot)
- [ ] P1-M7-B1b: Use for crossfade transitions
- [ ] P1-M7-B2a: BlendTreeNode (Clip leaf, Lerp2 + weight, Lerp3 + 2D weight)
- [ ] P1-M7-B2b: Evaluate tree bottom-up (leaf samples clip, internal nodes blend children)
- [ ] P1-M7-B2c: 1D blend space (N clips along parameter axis, auto-weight)
- [ ] P1-M7-B2d: 2D blend space (triangulated clip positions in 2D parameter space)
- [ ] P1-M7-B3a: Compute additive pose (clip_pose - reference_pose)
- [ ] P1-M7-B3b: Apply additive (result = base_pose + additive * weight)
- [ ] P1-M7-B3c: Use for layered animations (lean, aim offset)
- [ ] P1-M7-B4a: BoneMask: per-joint weight 0–1 (full_body, upper_body, lower_body, face)
- [ ] P1-M7-B4b: Apply mask during blend (lerp base/overlay by mask_weight per joint)
- [ ] P1-M7-B4c: Use for upper body shoot while legs run

#### P1-M7-C: Animation State Machine

- [ ] P1-M7-C1a: AnimState struct (name, blend tree, looping, playback speed)
- [ ] P1-M7-C1b: AnimTransition struct (from/to state, condition, crossfade_duration, blend_curve)
- [ ] P1-M7-C1c: AnimStateMachine (states array, transitions array, parameter table)
- [ ] P1-M7-C2a: Each frame: check transitions in priority order, begin crossfade if met
- [ ] P1-M7-C2b: During crossfade: blend outgoing+incoming poses by progress
- [ ] P1-M7-C2c: After crossfade: fully switch to new state
- [ ] P1-M7-C2d: Support any-state transitions
- [ ] P1-M7-C3a: Lua: engine.anim_set_param(entity, name, value)
- [ ] P1-M7-C3b: Lua: engine.anim_play(entity, state_name) — force-transition
- [ ] P1-M7-C3c: Lua: engine.anim_get_current_state(entity) → state_name
- [ ] P1-M7-C3d: Test: set parameter, verify transition, verify correct pose

#### P1-M7-D: Root Motion

- [ ] P1-M7-D1a: Identify root bone, extract delta position/rotation per frame
- [ ] P1-M7-D1b: Remove root motion from animation pose (zero root bone displacement)
- [ ] P1-M7-D1c: Apply root motion delta to entity transform
- [ ] P1-M7-D1d: Lua binding: engine.enable_root_motion(entity, true/false)

#### P1-M7-E: Animation Events / Notifies

- [ ] P1-M7-E1a: AnimEvent struct (time normalized 0–1, name, optional payload)
- [ ] P1-M7-E1b: Store events per clip (authored in editor or imported from glTF extras)
- [ ] P1-M7-E2a: Detect when playback crosses event time (forward or backward)
- [ ] P1-M7-E2b: Fire event callback (C++ delegate and/or Lua)
- [ ] P1-M7-E2c: Lua binding: engine.on_anim_event(entity, name, callback)
- [ ] P1-M7-E2d: Test: clip with event at t=0.5, verify callback fires once at correct frame

#### P1-M7-F: Montages

- [ ] P1-M7-F1a: AnimMontage: clip/section sequence, plays once, blending in/out
- [ ] P1-M7-F1b: Blend montage on top of state machine output using bone mask
- [ ] P1-M7-F1c: Montage sections: named time ranges (windup, swing, recovery)
- [ ] P1-M7-F1d: Lua binding: engine.play_montage / engine.stop_montage

#### P1-M7-G: Skinned Mesh Rendering

- [ ] P1-M7-G1a: Compute final bone matrices (joint_world * inverse_bind_pose)
- [ ] P1-M7-G1b: Upload bone matrices as uniform array (max 256) or SSBO
- [ ] P1-M7-G2a: skinned.vert — bone indices/weights vertex attribs, transform by bone blend
- [ ] P1-M7-G2b: G-Buffer variant: SKINNED flag activates bone transform in vertex stage
- [ ] P1-M7-G2c: Support 4 bone influences per vertex
- [ ] P1-M7-G3a: Two-bone IK solver (solve joint angles for 2-bone chain)
- [ ] P1-M7-G3b: Apply IK after animation pose, before bone matrix computation
- [ ] P1-M7-G3c: Lua binding: engine.set_ik_target(entity, chain_name, x, y, z)
- [ ] P1-M7-G3d: Foot placement on uneven terrain (raycast down, set IK target)

---

### P1-M8: Audio Production

**Dependencies**: P1-M1, P1-M2

#### P1-M8-A: 3D Positional Audio

- [ ] P1-M8-A1a: AudioListener struct (position, forward, up), one per world
- [ ] P1-M8-A1b: AudioEmitter component (position, source handle, min/max distance, rolloff)
- [ ] P1-M8-A1c: Update listener from camera transform each frame
- [ ] P1-M8-A1d: miniaudio spatialization: set position/direction, configure distance model
- [ ] P1-M8-A2a: Inverse distance clamped model (OpenAL-style)
- [ ] P1-M8-A2b: Configurable rolloff curves per emitter
- [ ] P1-M8-A2c: Test: sound at increasing distance, verify volume decreases per curve
- [ ] P1-M8-A3a: Enable miniaudio HRTF via CVar audio.hrtf_enabled
- [ ] P1-M8-A3b: Fallback to simple panning if HRTF unavailable

#### P1-M8-B: Mixer / Bus System

- [ ] P1-M8-B1a: AudioBus struct (name, volume, muted, parent, children)
- [ ] P1-M8-B1b: Default hierarchy: Master → {Music, SFX → {Dialogue, Environment, UI}}
- [ ] P1-M8-B1c: Each sound plays on bus, final volume = sound × bus chain to master
- [ ] P1-M8-B1d: Lua binding: engine.set_bus_volume / engine.mute_bus
- [ ] P1-M8-B2a: AudioSnapshot: named bus volume overrides, blend over time
- [ ] P1-M8-B2b: Ducking: dialogue → lower music, pause → muffle SFX, underwater
- [ ] P1-M8-B2c: Lua binding: engine.apply_audio_snapshot(name, blend_time)

#### P1-M8-C: DSP Effects

- [ ] P1-M8-C1a: Reverb (miniaudio node or Schroeder: 4 comb + 2 all-pass)
- [ ] P1-M8-C1b: Per-bus or per-emitter reverb (room_size, damping, wet_level)
- [ ] P1-M8-C1c: Reverb zones: spatial volumes activate reverb when listener enters
- [ ] P1-M8-C2a: Biquad low-pass filter (cutoff_frequency, Q)
- [ ] P1-M8-C2b: Occlusion simulation (wall between listener/emitter → low-pass)
- [ ] P1-M8-C2c: Lua binding: engine.set_bus_lowpass(bus, cutoff_hz)
- [ ] P1-M8-C3a: Pitch shift per emitter (miniaudio pitch)
- [ ] P1-M8-C3b: Doppler effect (relative velocity → pitch adjust)
- [ ] P1-M8-C3c: CVar audio.doppler_factor (0=off, 1=realistic)

#### P1-M8-D: Audio Events

- [ ] P1-M8-D1a: AudioEvent (name, candidate sounds, random select, volume/pitch randomization, cooldown)
- [ ] P1-M8-D1b: play_event(name, position) — pick random, apply randomization, play
- [ ] P1-M8-D1c: Store audio events in JSON definition files, load at startup
- [ ] P1-M8-D1d: Lua binding: engine.play_audio_event(name, x, y, z)
- [ ] P1-M8-D2a: Music crossfade between tracks (configurable fade time)
- [ ] P1-M8-D2b: Music layers: multiple stems, independently volume-controllable
- [ ] P1-M8-D2c: Lua binding: engine.play_music / engine.set_music_layer

#### P1-M8-E: Audio Streaming

- [~] P1-M8-E1a: miniaudio decoder streaming for files >1MB
- [~] P1-M8-E1b: CVar audio.stream_buffer_size_kb
- [~] P1-M8-E1c: Music always streams, SFX decompresses to memory

---

### P1-M9: Editor Production

**Dependencies**: P1-M1, P1-M2, P1-M4

#### P1-M9-A: Property Inspector

- [ ] P1-M9-A1a: PropertyInspector class: iterate components via reflection, render ImGui widgets
- [ ] P1-M9-A1b: Widget factory: float→DragFloat, Vec3→DragFloat3, bool→Checkbox, enum→Combo, Color→ColorEdit4
- [ ] P1-M9-A1c: Support nested structs (expand inline with tree nodes)
- [ ] P1-M9-A1d: Support arrays/lists (count, per-element editors, add/remove)
- [ ] P1-M9-A1e: Every property edit generates an undo command
- [ ] P1-M9-A2a: "Add Component" dropdown: list all registered component types
- [ ] P1-M9-A2b: Per-component "Remove" button with confirmation
- [ ] P1-M9-A2c: Both operations generate undo commands

#### P1-M9-B: Undo/Redo System

- [~] P1-M9-B1a: EditorCommand interface: execute(), undo(), description()
- [~] P1-M9-B1b: UndoSystem: two stacks (undo, redo), execute pushes undo clears redo
- [~] P1-M9-B1c: Command merging: consecutive same-property edits within 0.5s merge
- [~] P1-M9-B1d: Undo history limit: cap at 100, oldest discarded
- [ ] P1-M9-B2a: TransformCommand (before/after Transform, verify uses UndoSystem)
- [ ] P1-M9-B2b: PropertyCommand<T> (component type, field offset, before/after value)
- [ ] P1-M9-B2c: CreateEntityCommand / DestroyEntityCommand (full entity state snapshot)
- [ ] P1-M9-B2d: AddComponentCommand<T> / RemoveComponentCommand<T>
- [ ] P1-M9-B2e: ReparentCommand (old/new parent for hierarchy undo)
- [ ] P1-M9-B2f: Keyboard shortcuts: Ctrl+Z = undo, Ctrl+Y = redo

#### P1-M9-C: Scene Hierarchy

- [ ] P1-M9-C1a: ImGui tree view: all entities with names, parent-child nesting
- [ ] P1-M9-C1b: Click to select (update inspector), multi-select with Ctrl+click
- [ ] P1-M9-C1c: Drag-and-drop to reparent (generates ReparentCommand)
- [ ] P1-M9-C1d: Right-click context menu: Create Empty, Duplicate, Delete, Copy, Paste
- [ ] P1-M9-C1e: Search/filter bar at top

#### P1-M9-D: Asset Browser

- [ ] P1-M9-D1a: Left panel: folder tree, right panel: grid/list of assets in selected folder
- [ ] P1-M9-D1b: Thumbnails for textures/meshes, type icons for scripts/sounds
- [ ] P1-M9-D1c: Double-click: open inspector with metadata, import settings, preview
- [ ] P1-M9-D1d: Drag asset to viewport: instantiate as entity with appropriate components
- [ ] P1-M9-D1e: Search bar with tag filtering
- [ ] P1-M9-D2a: Right-click: Reimport, Show in Explorer, Copy Path, Delete (dep warning)
- [ ] P1-M9-D2b: Auto-detect new/changed files (file watcher or poll), trigger reimport

#### P1-M9-E: Prefab System

- [ ] P1-M9-E1a: Prefab asset: JSON snapshot of entity hierarchy (components, children, properties)
- [ ] P1-M9-E1b: "Save as Prefab" from selected entity → write JSON to assets folder
- [ ] P1-M9-E1c: Instantiate prefab: create entity hierarchy, assign new IDs
- [ ] P1-M9-E2a: Instances track overridden vs from-prefab properties
- [ ] P1-M9-E2b: Inspector: overridden properties bold, "Revert to Prefab" per property
- [ ] P1-M9-E2c: "Apply to Prefab" propagates changes to definition and all instances
- [ ] P1-M9-E3a: Prefab can contain references to other prefabs, recursive instantiation
- [ ] P1-M9-E3b: Updating inner prefab updates all outer prefabs using it
- [ ] P1-M9-E3c: Test: nest 3 levels, modify innermost, verify propagation

#### P1-M9-F: Play-in-Editor

- [~] P1-M9-F1a: On Play: serialize entire world to in-memory buffer
- [~] P1-M9-F1b: On Stop: deserialize, restore world to pre-play state
- [~] P1-M9-F1c: On Pause: freeze world tick, keep editor responsive
- [ ] P1-M9-F2a: Step button: advance exactly one frame while paused
- [ ] P1-M9-F2b: Console command: step <N>

#### P1-M9-G: Editor Scripting API

- [ ] P1-M9-G1a: Lua editor.register_menu_item(menu_path, callback)
- [ ] P1-M9-G1b: Lua editor.register_panel(name, draw_callback)
- [ ] P1-M9-G1c: Lua editor.get_selected_entities / editor.select_entity
- [ ] P1-M9-G1d: Lua editor.execute_command(undo_command) — integrates with undo

---

### P1-M10: Scene Management and World Streaming

**Dependencies**: P1-M4, P1-M9

#### P1-M10-A: Scene Management

- [ ] P1-M10-A1a: SceneManager::load_scene(path) — unload current, load new
- [ ] P1-M10-A1b: SceneManager::load_scene_additive(path) — merge entities
- [ ] P1-M10-A1c: SceneManager::unload_scene(id) — remove additive scene entities
- [ ] P1-M10-A1d: Loading screen support (callback for progress)
- [ ] P1-M10-A1e: Lua binding: engine.load_scene / engine.load_scene_additive / engine.unload_scene

#### P1-M10-A2: Persistent Entity IDs

- [~] P1-M10-A2a: Assign stable UUIDs to entities at creation, persist in scene JSON
- [~] P1-M10-A2b: On load: map serialized IDs to runtime entity handles
- [~] P1-M10-A2c: Cross-scene references by UUID, resolved at load
- [~] P1-M10-A2d: Test: save scene, load, verify IDs match, cross-ref resolves

#### P1-M10-B: World Streaming

- [ ] P1-M10-B1a: StreamingVolume component (AABB triggers scene load/unload)
- [ ] P1-M10-B1b: Each volume references a sub-scene asset
- [ ] P1-M10-B1c: StreamingManager: check player position vs volumes each frame
- [ ] P1-M10-B1d: Hysteresis: don't unload until distance exceeds threshold
- [ ] P1-M10-B1e: Async load via P1-M4, show LOD placeholder until ready
- [ ] P1-M10-B2a: LODGroup component: array of {mesh, max_distance}
- [ ] P1-M10-B2b: Select LOD level per entity based on camera distance each frame
- [ ] P1-M10-B2c: Hysteresis band to prevent popping
- [ ] P1-M10-B2d: Lua binding: engine.set_lod_group(entity, {mesh1, dist1, ...})

#### P1-M10-C: Save System

- [ ] P1-M10-C1a: SaveData struct (scene, game state, entity overrides, timers, player stats)
- [ ] P1-M10-C1b: Serialize to JSON: SaveSystem::save(slot) / load(slot)
- [ ] P1-M10-C1c: Multiple save slots (configurable, default 10)
- [ ] P1-M10-C1d: Platform-appropriate save path (AppData, ~/Library, etc.)
- [ ] P1-M10-C1e: Lua binding: engine.save_game / engine.load_game / engine.list_save_slots
- [ ] P1-M10-C2a: engine.save_checkpoint() — auto-save at scripted moment
- [ ] P1-M10-C2b: On death/failure: offer "Load Checkpoint"
- [ ] P1-M10-C2c: Test: save checkpoint, modify world, load, verify restored

---

### P1-M11: UI System (Runtime Game UI)

**Dependencies**: P1-M5, P1-M2

#### P1-M11-A: UI Canvas and Rendering

- [ ] P1-M11-A1a: UICanvas: screen-space overlay, owns tree of UIElements
- [ ] P1-M11-A1b: Coordinate system: normalized (0–1) or pixel, anchors
- [ ] P1-M11-A1c: Resolution independence: scale based on reference resolution (1920×1080)
- [ ] P1-M11-A2a: Batch UI draw calls: sort by material/texture, merge into single VBO
- [ ] P1-M11-A2b: ui.vert + ui.frag — simple 2D transform, texture sampling, color tinting
- [ ] P1-M11-A2c: Alpha blending, depth test OFF, render after all 3D and post-processing
- [ ] P1-M11-A3a: Integrate stb_truetype or Freetype, generate font atlas at runtime
- [ ] P1-M11-A3b: SDF fonts for clean scaling
- [ ] P1-M11-A3c: Text layout: line wrapping, alignment (left/center/right), vertical alignment
- [ ] P1-M11-A3d: Rich text: inline color changes <color=red>text</color>

#### P1-M11-B: Widget Library

- [ ] P1-M11-B1a: UIImage — texture with optional tint and 9-slice mode
- [ ] P1-M11-B1b: UIText — font, size, color, alignment
- [ ] P1-M11-B1c: UIButton — image + text + hover/pressed states + on_click
- [ ] P1-M11-B1d: UISlider — horizontal/vertical, min/max, on_change
- [ ] P1-M11-B1e: UIProgressBar — fill amount 0–1, color gradient
- [ ] P1-M11-B1f: UIToggle — checkbox/switch, on/off state
- [ ] P1-M11-B1g: UIInputField — text entry with caret, selection, keyboard input
- [ ] P1-M11-B2a: UIHBox / UIVBox — arrange children with spacing and padding
- [ ] P1-M11-B2b: UIGrid — grid layout with row/column count
- [ ] P1-M11-B2c: UIScrollView — scrollable area with content larger than viewport
- [ ] P1-M11-B2d: Layout engine: calculate sizes recursively, assign positions

#### P1-M11-C: UI Interaction

- [ ] P1-M11-C1a: UI raycast: mouse → hit test against UI element rects (front to back)
- [ ] P1-M11-C1b: Focus system: Tab to cycle, Enter/Space to activate
- [ ] P1-M11-C1c: Input consumption: UI click/key does NOT propagate to game input
- [ ] P1-M11-C1d: Gamepad navigation: D-pad between widgets, A to activate
- [ ] P1-M11-C2a: UI tweens: animate property (position, opacity, scale) with easing
- [ ] P1-M11-C2b: Button hover: scale 1.05× ease-out, press: scale 0.95× ease-in
- [ ] P1-M11-C2c: Panel fade-in/out: opacity 0→1 over 0.3s

#### P1-M11-D: Lua UI API

- [ ] P1-M11-D1a: engine.ui_create_canvas(name) → canvas_id
- [ ] P1-M11-D1b: engine.ui_add_text(canvas_id, text, x, y, font_size) → element_id
- [ ] P1-M11-D1c: engine.ui_add_button(canvas_id, text, x, y, w, h, on_click) → element_id
- [ ] P1-M11-D1d: engine.ui_add_image(canvas_id, texture_path, x, y, w, h)
- [ ] P1-M11-D1e: engine.ui_add_progress_bar(canvas_id, x, y, w, h, fill)
- [ ] P1-M11-D1f: engine.ui_set_property(element_id, property_name, value)
- [ ] P1-M11-D1g: engine.ui_remove(element_id) / engine.ui_clear(canvas_id)
- [ ] P1-M11-D2a: engine.ui_bind(element_id, "text", "player_health") — auto-update
- [ ] P1-M11-D2b: engine.set_observable(name, value) — triggers UI updates

---

### P1-M12: Platform, Packaging, Ship Readiness

**Dependencies**: All P1 milestones

#### P1-M12-A: Platform Abstraction Layer

- [ ] P1-M12-A1a: Platform interface: init/shutdown/get_save_path/get_data_path/open_url/get_memory_info
- [ ] P1-M12-A1b: Windows implementation (Win32 API)
- [ ] P1-M12-A1c: Linux implementation (POSIX + /proc/)
- [ ] P1-M12-A1d: macOS implementation (Cocoa/Foundation)
- [ ] P1-M12-A1e: Compile-time selection via CMake (ENGINE_TARGET_PLATFORM)
- [ ] P1-M12-A2a: core::FileSystem read/write/exists/list_directory
- [ ] P1-M12-A2b: Virtual file system: mount points (assets/ → archive or loose dir)
- [ ] P1-M12-A2c: Test: read/write roundtrip, directory listing, mount point resolution

#### P1-M12-B: Quality Settings

- [ ] P1-M12-B1a: Quality levels: Low, Medium, High, Ultra, Custom
- [ ] P1-M12-B1b: Each level sets shadow res, cascade count, SSAO, bloom, AA, draw distance, LOD bias
- [ ] P1-M12-B1c: Apply quality: update CVars in batch for selected preset
- [ ] P1-M12-B1d: Lua binding: engine.set_quality / engine.get_quality
- [ ] P1-M12-B2a: Dynamic resolution: monitor frame time, reduce render res if above target
- [ ] P1-M12-B2b: Scale range: 50%–100% of native resolution
- [ ] P1-M12-B2c: Upscale with bilinear or CAS (FidelityFX sharpening)
- [ ] P1-M12-B2d: CVar r_dynamic_resolution toggle

#### P1-M12-C: Packaging

- [ ] P1-M12-C1a: Asset packer bundles cooked assets into single archive
- [ ] P1-M12-C1b: Runtime loads from archive via VFS mount
- [ ] P1-M12-C1c: Strip editor-only assets, debug scripts, test scenes
- [ ] P1-M12-C2a: CMake install target: executable + runtime DLLs + asset archive
- [ ] P1-M12-C2b: Windows: ZIP or installer via CPack
- [ ] P1-M12-C2c: Linux: tar.gz or AppImage
- [ ] P1-M12-C2d: macOS: .app bundle

#### P1-M12-D: Crash Reporting

- [ ] P1-M12-D1a: Signal handlers (SIGSEGV, SIGABRT) / Windows SEH
- [ ] P1-M12-D1b: On crash: dump stack trace + engine state (frame number, scene, entities)
- [ ] P1-M12-D1c: Write crash log to crash_logs/ directory
- [ ] P1-M12-D1d: User-friendly crash dialog (optional)
- [ ] P1-M12-D2a: Opt-in anonymized crash reports to configurable HTTP endpoint
- [ ] P1-M12-D2b: CVar engine.telemetry_enabled (default off)

#### P1-M12-E: Localization

- [ ] P1-M12-E1a: StringTable: key → localized string, load from JSON per language
- [ ] P1-M12-E1b: localize(key) → string, fallback to English if missing
- [ ] P1-M12-E1c: set_language(code) — switch at runtime, fire refresh event
- [ ] P1-M12-E1d: Lua binding: engine.localize / engine.set_language
- [ ] P1-M12-E1e: Test: set language "de", verify correct string returned

#### P1-M12-F: Accessibility

- [ ] P1-M12-F1a: UI font size scaling (CVar ui.font_scale 0.5–3.0)
- [ ] P1-M12-F1b: High contrast mode (override UI colors)
- [ ] P1-M12-F1c: Colorblind mode (post-process filter or alternate color schemes)
- [ ] P1-M12-F1d: Subtitle system (timed text from SRT-like format)
- [ ] P1-M12-F1e: Lua binding: engine.set_colorblind_mode / engine.set_font_scale

#### P1-M12-G: Final Integration Tests

- [ ] P1-M12-G1a: Smoke test: launch, load scene, play 300 frames, screenshot, exit — no crash
- [ ] P1-M12-G1b: Smoke test: 1000 entities, physics+animation+audio for 600 frames
- [ ] P1-M12-G1c: Smoke test: save game, load game, verify world state matches
- [ ] P1-M12-G1d: Smoke test: transition 3 scenes, verify no leaks (entity count returns to baseline)
- [ ] P1-M12-G2a: Run full test suite under Valgrind/ASAN, zero leaks
- [ ] P1-M12-G2b: 5-minute play session, memory delta < 1MB

---

## Phase 2: Competitive Feature Parity

---

### P2-M1: Advanced Rendering

**Dependencies**: P1-M5, P1-M6

#### P2-M1-A: Lightmap Baking

- [ ] P2-M1-A1a: Generate UV2 (lightmap UV) per mesh at import time (xatlas or custom)
- [ ] P2-M1-A1b: Store UV2 in vertex attributes, pass to shaders
- [ ] P2-M1-A2a: CPU path tracer: emit rays from lightmap texels, bounce N times, accumulate irradiance
- [ ] P2-M1-A2b: Multi-threaded: distribute texel rows across job system threads
- [ ] P2-M1-A2c: Output: per-mesh lightmap texture (RGBA16F)
- [ ] P2-M1-A2d: Denoise: bilateral filter or OIDN integration
- [ ] P2-M1-A3a: Load lightmap textures at runtime, bind per-mesh
- [ ] P2-M1-A3b: Deferred lighting: multiply lightmap sample into indirect diffuse
- [ ] P2-M1-A3c: Editor: "Bake Lightmaps" button

#### P2-M1-B: Screen-Space Reflections

- [ ] P2-M1-B1a: SSR post-process: reflect ray from view+normal, march depth buffer
- [ ] P2-M1-B1b: Hi-Z acceleration: depth mip pyramid for large steps, binary search refine
- [ ] P2-M1-B1c: Fade at screen edges, fallback to env probe
- [ ] P2-M1-B1d: CVars: r_ssr_enabled, r_ssr_max_steps, r_ssr_thickness

#### P2-M1-C: Volumetric Fog / God Rays

- [ ] P2-M1-C1a: Froxel grid: 3D texture representing frustum slices
- [ ] P2-M1-C1b: Per froxel: participating media density, in-scattered light from shadow map
- [ ] P2-M1-C1c: Temporal reprojection: reuse previous frame fog data
- [ ] P2-M1-C1d: Composite: blend fog into scene during deferred lighting

#### P2-M1-D: Advanced Post-Processing

- [ ] P2-M1-D1a: TAA: jitter projection matrix (Halton sequence)
- [ ] P2-M1-D1b: taa.frag: reproject previous frame, blend with neighborhood clamping
- [ ] P2-M1-D1c: Motion vectors pass: per-pixel velocity from current/previous MVP
- [ ] P2-M1-D1d: CVar r_aa_mode value 2 = TAA
- [ ] P2-M1-D2a: Motion blur: sample along velocity vector
- [ ] P2-M1-D2b: Variable sample count based on velocity magnitude
- [ ] P2-M1-D2c: CVars: r_motion_blur_enabled, r_motion_blur_samples/scale
- [ ] P2-M1-D3a: Depth of field: circle of confusion per pixel
- [ ] P2-M1-D3b: Separate near/far blur passes (disc sampling, bokeh)
- [ ] P2-M1-D3c: Configurable: focal distance, aperture, autofocus
- [ ] P2-M1-D3d: Lua binding: engine.set_dof(focal_distance, aperture)
- [ ] P2-M1-D4a: PostProcessVolume component: AABB + blendable settings
- [ ] P2-M1-D4b: Camera inside volume: lerp settings by blend weight
- [ ] P2-M1-D4c: Prioritized volumes, overlapping blend by boundary distance
- [ ] P2-M1-D4d: Lua binding: engine.set_post_process_volume(entity, settings_table)

---

### P2-M2: VFX / Particle System

**Dependencies**: P1-M5, P1-M6, P1-M3

#### P2-M2-A: Particle Core

- [ ] P2-M2-A1a: ParticleEmitter component (spawn rate, max, lifetime, velocity, size, color, texture)
- [ ] P2-M2-A1b: Particle buffer: SoA (position, velocity, age, size, color, rotation)
- [ ] P2-M2-A1c: Spawn: emit N per frame, initialize from shape (point, sphere, cone, box)
- [ ] P2-M2-A1d: Update: integrate velocity, apply forces, age, kill expired
- [ ] P2-M2-A2a: Gravity force
- [ ] P2-M2-A2b: Wind force (directional)
- [ ] P2-M2-A2c: Turbulence (curl noise)
- [ ] P2-M2-A2d: Attractor/repulsor (point force)
- [ ] P2-M2-A2e: Drag (velocity-dependent deceleration)
- [ ] P2-M2-A3a: Billboard quad per particle, batch into single draw call per emitter
- [ ] P2-M2-A3b: particle.vert / particle.frag — texture, color/size over life
- [ ] P2-M2-A3c: Blend modes: additive (fire), alpha (smoke), soft particles (depth fade)
- [ ] P2-M2-A3d: Sort particles back-to-front for alpha blending
- [ ] P2-M2-A4a: ParticleCurve: keyframed values over normalized lifetime
- [ ] P2-M2-A4b: Apply curves: size/color/velocity/rotation over life
- [ ] P2-M2-A4c: Editor: visual curve editor for particle parameters

#### P2-M2-B: Advanced Particle Features

- [ ] P2-M2-B1a: Compute shader particle simulation (GPU)
- [ ] P2-M2-B1b: Indirect draw: GPU-side particle count, no CPU readback
- [ ] P2-M2-B1c: Support 100K+ particles per system
- [ ] P2-M2-B2a: Simple plane collision (bounce)
- [ ] P2-M2-B2b: Depth buffer collision (kill or bounce on hit)
- [ ] P2-M2-B2c: Sub-particle spawn on collision (splash)
- [ ] P2-M2-B3a: Trail points per particle, ribbon mesh connecting points
- [ ] P2-M2-B3b: UV mapping along trail length
- [ ] P2-M2-B3c: Width over life for trail tapering
- [ ] P2-M2-B4a: Instanced mesh per particle instead of billboard
- [ ] P2-M2-B4b: Per-particle rotation, scale, color via instance data
- [ ] P2-M2-B5a: Lua: engine.spawn_particle_emitter(entity, definition)
- [ ] P2-M2-B5b: Lua: engine.stop_emitter / engine.set_emitter_rate
- [ ] P2-M2-B5c: Particle events: on_collision, on_death callbacks

---

### P2-M3: 2D Engine

**Dependencies**: P1-M5, P1-M3

#### P2-M3-A: Sprite Renderer

- [ ] P2-M3-A1a: SpriteComponent (texture_id, source_rect, tint, flip, sort_order)
- [ ] P2-M3-A1b: SparseSet storage, CRUD in World
- [ ] P2-M3-A1c: Batch by texture (sort z→texture→position), single draw call per batch
- [ ] P2-M3-A1d: sprite.vert / sprite.frag — 2D transform, atlas sampling, color multiply
- [ ] P2-M3-A2a: Atlas packer: rect packing multiple sprites into atlas texture
- [ ] P2-M3-A2b: Output atlas image + JSON map (name → x, y, w, h)
- [ ] P2-M3-A2c: Runtime: load atlas, resolve sprite names to source rects
- [ ] P2-M3-A3a: SpriteAnimation (array of frames: source_rect + duration)
- [ ] P2-M3-A3b: SpriteAnimator component (current anim, frame, elapsed, looping, speed)
- [ ] P2-M3-A3c: Lua: engine.sprite_play / engine.sprite_set_speed

#### P2-M3-B: Tilemap

- [ ] P2-M3-B1a: Tilemap component (tile_width/height, map_width/height, tile data, tileset)
- [ ] P2-M3-B1b: Render only visible tiles (camera-frustum cull grid cells), batched quads
- [ ] P2-M3-B1c: Multiple layers (background, foreground, collision)
- [ ] P2-M3-B1d: Import from Tiled JSON format (.tmj)
- [ ] P2-M3-B2a: Flag tiles solid/non-solid, generate collision shapes
- [ ] P2-M3-B2b: Merge adjacent solid tiles into larger rectangles
- [ ] P2-M3-B2c: Register merged rectangles as static AABB colliders

#### P2-M3-C: 2D Physics

- [ ] P2-M3-C1a: 2D circle collider (center, radius)
- [ ] P2-M3-C1b: 2D box collider (center, half_extents, rotation)
- [ ] P2-M3-C1c: 2D polygon collider (convex, max 8 vertices)
- [ ] P2-M3-C1d: 2D narrow phase: circle-circle, circle-box, box-box, polygon SAT
- [ ] P2-M3-C1e: 2D broadphase: grid-based spatial hash
- [ ] P2-M3-C2a: RigidBody2D (Vec2 pos, float rot, Vec2 vel, angular_vel, mass, gravity_scale)
- [ ] P2-M3-C2b: 2D physics step: gravity, integration, broadphase, narrow, resolve
- [ ] P2-M3-C2c: 2D joints: distance, spring, hinge (revolute)
- [ ] P2-M3-C2d: One-way platforms (collide from above only)
- [ ] P2-M3-C3a: raycast_2d(origin, direction, distance, mask) → hit list
- [ ] P2-M3-C3b: Lua: engine.raycast_2d(ox, oy, dx, dy, dist)

#### P2-M3-D: 2D Camera

- [ ] P2-M3-D1a: Follow target entity (lerp-based smoothing)
- [ ] P2-M3-D1b: Camera bounds: clamp to world bounds
- [ ] P2-M3-D1c: Zoom: orthographic size, pinch gesture for mobile
- [ ] P2-M3-D1d: Camera shake (2D noise offset)
- [ ] P2-M3-D1e: Lua: engine.camera_2d_follow / engine.camera_2d_zoom

---

### P2-M4: Networking Foundation

**Dependencies**: P1-M2, P1-M1

#### P2-M4-A: Transport Layer

- [ ] P2-M4-A1a: NetworkSocket class (UDP, Winsock/BSD), send_to / receive_from
- [ ] P2-M4-A1b: Non-blocking mode, poll with select
- [ ] P2-M4-A2a: Sequence numbers (uint16 wrapping)
- [ ] P2-M4-A2b: ACK bitfield: 32-bit window of received acknowledgments
- [ ] P2-M4-A2c: Resend unacknowledged after timeout (RTT estimation)
- [ ] P2-M4-A2d: Ordered delivery: buffer out-of-order, deliver in sequence
- [ ] P2-M4-A2e: Test: simulate 30% loss, verify all messages arrive in order
- [ ] P2-M4-A3a: Handshake: connect request → session token
- [ ] P2-M4-A3b: Heartbeat: keep-alive, timeout disconnect after 10s
- [ ] P2-M4-A3c: Disconnect: graceful with reason code, or timeout
- [ ] P2-M4-A3d: Encryption: XChaCha20-Poly1305 for packet payloads

#### P2-M4-B: Replication

- [ ] P2-M4-B1a: Mark components as replicated (registration flag)
- [ ] P2-M4-B1b: Server: serialize dirty data, delta compression (only changed fields)
- [ ] P2-M4-B1c: Client: receive updates, apply to local entities
- [ ] P2-M4-B1d: Priority: closer entities get higher update frequency
- [ ] P2-M4-B2a: RPC: function name + serialized args over reliable channel
- [ ] P2-M4-B2b: Server RPC: client invokes, server executes
- [ ] P2-M4-B2c: Client RPC: server invokes, client executes
- [ ] P2-M4-B2d: Multicast RPC: server sends to all clients
- [ ] P2-M4-B2e: Lua binding: engine.rpc_server / engine.on_rpc
- [ ] P2-M4-B3a: Client-side prediction: apply input locally, send to server
- [ ] P2-M4-B3b: Server reconciliation: compare predicted vs server state
- [ ] P2-M4-B3c: Entity interpolation: smooth remote entities between updates
- [ ] P2-M4-B3d: Input buffer: store N frames for replay on misprediction

#### P2-M4-C: Lobby and Session

- [ ] P2-M4-C1a: NetworkSession: track connected players (id, name, latency, state)
- [ ] P2-M4-C1b: Host migration: promote player if host disconnects
- [ ] P2-M4-C1c: Lua: engine.host_game / engine.join_game / engine.get_players

---

### P2-M5: Splines, Data Tables, Gameplay Tools

**Dependencies**: P1-M2, P1-M9

#### P2-M5-A: Spline / Path System

- [ ] P2-M5-A1a: Spline component (control points Vec3[], tangent mode, closed flag)
- [ ] P2-M5-A1b: Catmull-Rom interpolation: evaluate(t) → position, evaluate_tangent(t)
- [ ] P2-M5-A1c: Arc-length parameterization (constant speed traversal)
- [ ] P2-M5-A1d: get_nearest_point(position) → closest point + t parameter
- [ ] P2-M5-A2a: Visualize spline as polyline in editor (debug draw)
- [ ] P2-M5-A2b: Gizmos for control points (drag, shift-click add, delete remove)
- [ ] P2-M5-A2c: Show tangent handles for manual tangent mode
- [ ] P2-M5-A3a: Lua: engine.spline_evaluate(entity, t) → x,y,z
- [ ] P2-M5-A3b: Lua: engine.spline_follow(entity, spline_entity, speed)
- [ ] P2-M5-A3c: Use cases: camera rails, patrol paths, moving platforms

#### P2-M5-B: Data Tables

- [ ] P2-M5-B1a: DataTable (rows × typed columns), load from CSV or JSON
- [ ] P2-M5-B1b: Column types: int, float, string, bool, AssetRef
- [ ] P2-M5-B1c: Lookup: table.get_row(key), row.get_float(column)
- [ ] P2-M5-B1d: Hot reload: detect CSV change, reload
- [ ] P2-M5-B2a: Lua: engine.load_data_table(path) → table_id
- [ ] P2-M5-B2b: Lua: engine.dt_get(table_id, row_key, column) → value
- [ ] P2-M5-B2c: Lua: engine.dt_get_row(table_id, row_key) → table

#### P2-M5-C: CSG Brushes

- [ ] P2-M5-C1a: CSGBrush component (shape: box/cylinder/sphere, operation: union/subtract/intersect)
- [ ] P2-M5-C1b: CSG boolean mesh operations (BSP tree method)
- [ ] P2-M5-C1c: Generate mesh from CSG tree (traverse operations, output triangles)
- [ ] P2-M5-C1d: UV generation: planar projection for CSG faces
- [ ] P2-M5-C1e: Editor: drag brush, boolean dropdown, real-time preview

#### P2-M5-D: Foliage Painting

- [ ] P2-M5-D1a: Editor brush tool (radius, density, random rotation/scale)
- [ ] P2-M5-D1b: Click/drag: raycast, scatter foliage instances within brush
- [ ] P2-M5-D1c: Erase mode: remove instances within brush
- [ ] P2-M5-D1d: FoliageComponent (array of transform + mesh ref), render via instancing

---

### P2-M6: Controller Haptics and Advanced Input

**Dependencies**: P1-M2

#### P2-M6-A: Haptics

- [ ] P2-M6-A1a: engine.set_rumble(low_freq, high_freq, duration) via SDL rumble API
- [ ] P2-M6-A1b: Rumble presets: light, heavy, pulse (alternating)
- [ ] P2-M6-A1c: Lua binding: engine.rumble(low, high, duration)
- [ ] P2-M6-A2a: Adaptive trigger effect types (resistance, vibration, weapon feedback)
- [ ] P2-M6-A2b: Platform-gated: DualSense only, fallback no-op
- [ ] P2-M6-A2c: Lua binding: engine.set_trigger_effect(hand, type, params)

#### P2-M6-B: Gyroscope / Motion Input

- [ ] P2-M6-B1a: Read gyroscope data from SDL sensor API
- [ ] P2-M6-B1b: Convert angular velocity to camera rotation delta (configurable sensitivity)
- [ ] P2-M6-B1c: Lua: engine.get_gyro() → pitch_rate, yaw_rate, roll_rate

#### P2-M6-C: Input Recording / Replay

- [ ] P2-M6-C1a: Record all input events with frame timestamps
- [ ] P2-M6-C1b: Save to binary file, load and replay (feed as live)
- [ ] P2-M6-C1c: Use for deterministic replay testing, automated QA
- [ ] P2-M6-C1d: Console commands: record_start, record_stop, replay <file>

---

### P2-M7: Advanced Editor Features

**Dependencies**: P1-M9, P1-M7, P2-M2

#### P2-M7-A: Visual Scripting (Node Graph)

- [ ] P2-M7-A1a: Node struct (type, inputs/outputs as typed pins, canvas position)
- [ ] P2-M7-A1b: Link struct (from node/pin → to node/pin, type compatibility check)
- [ ] P2-M7-A1c: Render nodes via ImGui custom draw (rectangles, pin circles, bezier links)
- [ ] P2-M7-A1d: Interaction: drag to connect, right-click add node, delete to remove
- [ ] P2-M7-A2a: Compile graph to Lua (topological sort → emit Lua statements)
- [ ] P2-M7-A2b: Node library: engine API, math, flow nodes (if, for, delay)
- [ ] P2-M7-A2c: Live preview: recompile on edit, hot-reload script

#### P2-M7-B: Animation Editor

- [ ] P2-M7-B1a: Horizontal timeline with playback scrubber, clip name/duration
- [ ] P2-M7-B1b: Keyframe markers per channel, click to select
- [ ] P2-M7-B1c: Edit keyframe values in inspector panel
- [ ] P2-M7-B2a: Visual state machine: states as boxes, transitions as arrows
- [ ] P2-M7-B2b: Drag to create transitions, double-click state for blend tree
- [ ] P2-M7-B2c: Live preview: highlight active state during play

#### P2-M7-C: Terrain Editor

- [ ] P2-M7-C1a: TerrainComponent (heightmap texture, tile count, height scale)
- [ ] P2-M7-C1b: Generate mesh from heightmap (grid vertices, per-vertex height)
- [ ] P2-M7-C1c: LOD: clipmap or quadtree-based terrain LOD
- [ ] P2-M7-C1d: Multi-texture splatting (blend 4 textures via splat map)
- [ ] P2-M7-C2a: Sculpt tools: raise/lower/smooth/flatten brush
- [ ] P2-M7-C2b: Paint: select texture layer, paint splat weights
- [ ] P2-M7-C2c: Brush parameters: radius, strength, falloff

#### P2-M7-D: Plugin System

- [ ] P2-M7-D1a: Plugin interface: on_load, on_unload, on_tick, get_name
- [ ] P2-M7-D1b: Plugin loader: scan plugins/ for shared libs, load at startup
- [ ] P2-M7-D1c: Plugin sandbox: register menus/panels/inspectors, can't bypass modules

---

### P2-M8: Performance Polish and Profiling

**Dependencies**: All P1, P2-M1

#### P2-M8-A: Multi-Threaded Rendering

- [ ] P2-M8-A1a: Record draw commands on worker threads (command structs, not GL calls)
- [ ] P2-M8-A1b: Main thread: playback command buffers, execute GL calls
- [ ] P2-M8-A1c: Parallel frustum culling + draw command generation

#### P2-M8-A2: Parallel Scene Update

- [ ] P2-M8-A2a: ECS system scheduling: identify independent systems, run in parallel
- [ ] P2-M8-A2b: Read/write dependency tracking per system per component type
- [ ] P2-M8-A2c: Job graph: build DAG of system deps, dispatch to job system

#### P2-M8-B: Culling Optimization

- [ ] P2-M8-B1a: BVH for static scene geometry
- [ ] P2-M8-B1b: Frustum cull BVH: skip subtrees when parent outside frustum
- [ ] P2-M8-B1c: Occlusion culling: software rasterizer for occluders (HZB)

#### P2-M8-C: Shader Cache and Pipeline State

- [ ] P2-M8-C1a: Cache compiled GL programs to disk (glGetProgramBinary)
- [ ] P2-M8-C1b: On load: check cache for matching source hash, skip compilation
- [ ] P2-M8-C1c: Invalidate cache on shader source change
- [ ] P2-M8-C2a: Sort draw calls by PSO (shader→material→mesh) to minimize state changes
- [ ] P2-M8-C2b: Encode PSO as 64-bit sort key for radix sort

#### P2-M8-D: Memory Budgets

- [ ] P2-M8-D1a: Define memory budgets per subsystem (physics 64MB, renderer 256MB, etc.)
- [ ] P2-M8-D1b: Track usage: warning at 80%, error at 100%
- [ ] P2-M8-D1c: Editor memory dashboard: bar chart per subsystem usage vs budget

---

## Phase 3: Future / Cutting-Edge

---

### P3-M1: XR / VR / AR

**Dependencies**: P2-M1, P2-M6

#### P3-M1-A: OpenXR Integration

- [ ] P3-M1-A1a: Initialize OpenXR runtime (instance, system, session with GL binding)
- [ ] P3-M1-A1b: Handle session lifecycle (ready → focused → stopping → exiting)
- [ ] P3-M1-A1c: Create LOCAL and STAGE reference spaces

#### P3-M1-A2: Stereo Rendering

- [ ] P3-M1-A2a: Get per-eye view and projection matrices from OpenXR
- [ ] P3-M1-A2b: Render scene twice (per eye) into swapchain images
- [ ] P3-M1-A2c: Multi-view extension: single pass for both eyes if available
- [ ] P3-M1-A2d: Submit frames via xrEndFrame, handle frame timing

#### P3-M1-A3: Head and Controller Tracking

- [ ] P3-M1-A3a: Poll head pose each frame, apply to camera
- [ ] P3-M1-A3b: Poll controller poses (left/right), map to entity transforms
- [ ] P3-M1-A3c: Controller buttons/triggers/thumbsticks → input actions

#### P3-M1-A4: Hand Tracking

- [ ] P3-M1-A4a: OpenXR hand tracking extension (26-joint hand skeleton per hand)
- [ ] P3-M1-A4b: Map hand joints to visual hand mesh
- [ ] P3-M1-A4c: Gesture detection: pinch, grab, point

#### P3-M1-A5: Passthrough AR

- [ ] P3-M1-A5a: Enable passthrough layer (Meta Quest API)
- [ ] P3-M1-A5b: Composite 3D scene over passthrough feed

#### P3-M1-A6: Lua XR API

- [ ] P3-M1-A6a: engine.xr_is_active() → bool
- [ ] P3-M1-A6b: engine.xr_get_head_transform() → position, rotation
- [ ] P3-M1-A6c: engine.xr_get_hand_transform(hand) → position, rotation
- [ ] P3-M1-A6d: engine.xr_get_hand_joints(hand) → table of 26 joint transforms
- [ ] P3-M1-A6e: engine.xr_haptic_pulse(hand, amplitude, duration)

---

### P3-M2: Vulkan / Modern Graphics Backend

**Dependencies**: P2-M8

#### P3-M2-A: Render Abstraction Layer

- [ ] P3-M2-A1a: Abstract RenderDevice interface (create_buffer, create_texture, create_shader, etc.)
- [ ] P3-M2-A1b: Move current GL code behind GLRenderDevice
- [ ] P3-M2-A1c: All renderer code uses RenderDevice* — never raw GL above device layer
- [ ] P3-M2-A2a: Vulkan: instance, physical device, logical device, queue families
- [ ] P3-M2-A2b: Swapchain creation and management
- [ ] P3-M2-A2c: Command buffer pool and recording
- [ ] P3-M2-A2d: Implement all RenderDevice methods for Vulkan
- [ ] P3-M2-A2e: Synchronization: fences, semaphores for frame-in-flight
- [ ] P3-M2-A3a: Cross-compile GLSL → SPIR-V via glslang (offline in asset packer)
- [ ] P3-M2-A3b: Keep GL GLSL and Vulkan SPIR-V as parallel outputs
- [ ] P3-M2-A3c: Runtime: load appropriate format based on active backend

#### P3-M2-B: Vulkan-Specific Features

- [ ] P3-M2-B1a: Expose compute dispatch through RenderDevice::dispatch_compute()
- [ ] P3-M2-B1b: Migrate GPU particles to compute pipeline
- [ ] P3-M2-B1c: Async compute queue: particle sim overlaps with graphics
- [ ] P3-M2-B2a: Descriptor indexing: one large descriptor set with all textures
- [ ] P3-M2-B2b: Eliminate per-material texture binding overhead

---

### P3-M3: Mobile Platform Support

**Dependencies**: P1-M12, P2-M3

#### P3-M3-A: Android Build

- [ ] P3-M3-A1a: CMake toolchain: cross-compile with NDK, target arm64-v8a
- [ ] P3-M3-A1b: SDL2 Android backend: NativeActivity
- [ ] P3-M3-A1c: Gradle wrapper: build APK/AAB from CMake output
- [ ] P3-M3-A2a: OpenGL ES 3.0 rendering path
- [ ] P3-M3-A2b: Touch input integration (P1-M2-C3)
- [ ] P3-M3-A2c: Reduce shadow resolution, disable SSAO, lower quality defaults
- [ ] P3-M3-A2d: Battery/thermal management: reduce FPS when throttling

#### P3-M3-B: iOS Build

- [ ] P3-M3-B1a: CMake toolchain for iOS (arm64, deployment target)
- [ ] P3-M3-B1b: Generate Xcode project from CMake
- [ ] P3-M3-B1c: Metal rendering backend (or MoltenVK)
- [ ] P3-M3-B2a: App lifecycle: backgrounding, foregrounding, memory warnings
- [ ] P3-M3-B2b: CoreHaptics integration

#### P3-M3-C: Mobile UI Adaptation

- [ ] P3-M3-C1a: UI widgets minimum 44×44pt touch target
- [ ] P3-M3-C1b: Virtual joystick overlay for movement
- [ ] P3-M3-C1c: Gesture-based camera: swipe to look, pinch to zoom

---

### P3-M4: Web / Emscripten Build

**Dependencies**: P1-M12, P3-M3

#### P3-M4-A: Emscripten Compilation

- [ ] P3-M4-A1a: CMake toolchain: Emscripten (emcmake cmake)
- [ ] P3-M4-A1b: Compile all C++ to WASM, link SDL2 Emscripten port
- [ ] P3-M4-A1c: Output: .wasm + .js + .html shell
- [ ] P3-M4-A2a: Main loop: emscripten_set_main_loop instead of while(true)
- [ ] P3-M4-A2b: Asset loading: HTTP fetch, Emscripten async filesystem
- [ ] P3-M4-A2c: WebGL 2.0 rendering (maps to GLES 3.0)
- [ ] P3-M4-A2d: Audio via Web Audio API (miniaudio Emscripten backend)
- [ ] P3-M4-A3a: WASM size: strip debug, LTO, -Oz
- [ ] P3-M4-A3b: Streaming instantiation: load WASM in parallel with JS
- [ ] P3-M4-A3c: Progressive asset loading: show loading screen while fetching

---

### P3-M5: AI and Navigation

**Dependencies**: P1-M3, P1-M2

#### P3-M5-A: Navigation Mesh

- [ ] P3-M5-A1a: Voxelization: rasterize scene geometry into 3D voxel grid
- [ ] P3-M5-A1b: Walkable surface detection (solid ground, sufficient headroom)
- [ ] P3-M5-A1c: Region partitioning (watershed or monotone decomposition)
- [ ] P3-M5-A1d: Contour tracing: extract region contours as simplified polygons
- [ ] P3-M5-A1e: Polygon mesh: triangulate contours into nav triangles
- [ ] P3-M5-A1f: Parameters: agent_radius, agent_height, max_slope, step_height
- [ ] P3-M5-A1g: Editor: "Build NavMesh" button, visualize overlay

#### P3-M5-A2: Pathfinding

- [ ] P3-M5-A2a: A* on nav mesh dual graph (triangle adjacency)
- [ ] P3-M5-A2b: String pulling (funnel algorithm) for smooth paths
- [ ] P3-M5-A2c: Dynamic obstacle avoidance (carve or mark blocked)
- [ ] P3-M5-A2d: Off-mesh links (ladders, jumps, teleports)
- [ ] P3-M5-A2e: Lua: engine.find_path(start, end) → waypoints

#### P3-M5-B: AI Behavior

- [ ] P3-M5-B1a: Behavior tree nodes: Selector, Sequence, Decorator, Action
- [ ] P3-M5-B1b: Tick tree: depth-first, return Running/Success/Failure
- [ ] P3-M5-B1c: Blackboard: per-entity key-value store for AI state
- [ ] P3-M5-B1d: Built-in actions: MoveTo, Wait, PlayAnimation, LookAt, Attack, Flee
- [ ] P3-M5-B1e: Editor: behavior tree visual editor (node graph)
- [ ] P3-M5-B2a: Seek: move toward target
- [ ] P3-M5-B2b: Flee: move away from target
- [ ] P3-M5-B2c: Arrive: decelerate near target
- [ ] P3-M5-B2d: Obstacle avoidance: raycast feelers
- [ ] P3-M5-B2e: Separation / cohesion / alignment (flocking)
- [ ] P3-M5-B2f: Blend behaviors: weighted sum of steering forces

#### P3-M5-B3: Lua AI API

- [ ] P3-M5-B3a: engine.ai_move_to(entity, x, y, z) — pathfind and follow
- [ ] P3-M5-B3b: engine.ai_set_behavior_tree(entity, tree_asset)
- [ ] P3-M5-B3c: engine.ai_blackboard_set / engine.ai_blackboard_get

---

### P3-M6: Advanced Networking and Dedicated Server

**Dependencies**: P2-M4

#### P3-M6-A: Dedicated Server

- [ ] P3-M6-A1a: -dedicated flag: skip renderer/editor/audio init
- [ ] P3-M6-A1b: Configurable tick rate (default 60Hz)
- [ ] P3-M6-A1c: Console-only interface: log output, command input via stdin
- [ ] P3-M6-A2a: All mutations on server, client sends input, server sends state
- [ ] P3-M6-A2b: Server validates client actions (speed check, position, cooldowns)
- [ ] P3-M6-A2c: Server reconciliation messages

#### P3-M6-B: Lag Compensation

- [ ] P3-M6-B1a: Store N frames of world state history (ring buffer)
- [ ] P3-M6-B1b: On hit verify: rewind to shooter's perceived time, perform hit check
- [ ] P3-M6-B1c: Maximum rewind limit (configurable, default 200ms)

#### P3-M6-C: Interest Management

- [ ] P3-M6-C1a: Per-client: only replicate entities within relevancy radius
- [ ] P3-M6-C1b: Priority: closer entities get higher update frequency
- [ ] P3-M6-C1c: AOI grid: replicate entities in client's cell + neighbors
- [ ] P3-M6-C1d: Bandwidth budget: limit bytes/sec per client, prioritize within

#### P3-M6-D: Matchmaking and Lobby

- [ ] P3-M6-D1a: Lobby server: REST API for session listing (create, join, list, delete)
- [ ] P3-M6-D1b: Client: browse/auto-match by skill/region
- [ ] P3-M6-D1c: Lua: engine.find_match / engine.create_lobby

---

## Parallel Lanes

### Documentation Lane
- [ ] DOC-1: `docs/getting_started.md` — clone, build, run first scene
- [ ] DOC-2: `docs/architecture.md` — module dependency diagram, data flow
- [ ] DOC-3: `docs/lua_api.md` — auto-generated from binding annotations
- [ ] DOC-4: `docs/editor_guide.md` — annotated screenshots of each panel
- [ ] DOC-5: `docs/asset_pipeline.md` — import formats, cooking, metadata
- [ ] DOC-6: `docs/networking.md` — authority model, replication, prediction
- [ ] DOC-7: `docs/contributing.md` — code style, PR process, test requirements

### Testing Lane
- [ ] TEST-1: Fuzz tests for asset parsers (mesh, texture, audio)
- [ ] TEST-2: Property-based tests for math library (QuickCheck-style)
- [ ] TEST-3: Soak tests (1-hour continuous play, memory growth)
- [ ] TEST-4: Platform-specific integration tests per target OS
- [ ] TEST-5: Screenshot comparison tests (golden image diffs)

### DevOps Lane
- [ ] DEVOPS-1: Nightly build with extended test suite and soak tests
- [ ] DEVOPS-2: Automated cross-compilation CI for each platform
- [ ] DEVOPS-3: Release pipeline (tag → build → package → publish)
- [ ] DEVOPS-4: Dependency update bot (SDL/Lua/miniaudio alerts)

---

## Tracking Summary

| Phase | Milestones | Atomic Tasks | Complete |
|-------|-----------|-------------|----------|
| P1: Ship Blockers | 12 | ~350 | ~130 (M1+M2) |
| P2: Competitive Parity | 8 | ~160 | 0 |
| P3: Cutting-Edge | 6 | ~90 | 0 |
| Parallel Lanes | 3 | 16 | 0 |
| **Total** | **29** | **~616** | **~130** |
