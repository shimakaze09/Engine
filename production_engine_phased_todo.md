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

- [ ] P1-M2-D1a: TimerManager class (per-World, fixed-capacity)
- [ ] P1-M2-D1b: set_timeout / set_interval / cancel
- [ ] P1-M2-D1c: Tick timers in World::update simulation phase
- [ ] P1-M2-D1d: Serialization: save/load active timers in scene JSON
- [ ] P1-M2-D2a: Lua engine.set_timeout / engine.set_interval / engine.cancel_timer
- [ ] P1-M2-D2b: Timer callbacks via pcall with error handling
- [ ] P1-M2-D2c: Test: set timeout 0.5s, advance 30 frames at 60fps, verify fires

#### P1-M2-E: Gameplay Camera

- [ ] P1-M2-E1a: SpringArmComponent struct (POD, SparseSet)
- [ ] P1-M2-E1b: SparseSet storage + CRUD in World
- [ ] P1-M2-E1c: Spring arm sweep + shorten on collision + lag smooth
- [ ] P1-M2-E1d: Reflection, serialization, Lua binding
- [ ] P1-M2-E2a: CameraManager (priority stack of cameras)
- [ ] P1-M2-E2b: push_camera / pop_camera with blend interpolation
- [ ] P1-M2-E2c: Lua engine.push_camera / engine.get_active_camera
- [ ] P1-M2-E3a: CameraShake struct (amplitude, frequency, duration, decay)
- [ ] P1-M2-E3b: Perlin noise-based shake (offset position + rotation)
- [ ] P1-M2-E3c: Stack multiple shakes additively
- [ ] P1-M2-E3d: Lua engine.camera_shake(amplitude, freq, duration)
- [ ] P1-M2-E3e: Test: shake offset ≠ 0 during duration, == 0 after

#### P1-M2-F: Coroutines / Async Gameplay

- [ ] P1-M2-F1a: CoroutineScheduler class (list of suspended Lua coroutines)
- [ ] P1-M2-F1b: engine.wait(seconds) — yield and resume after elapsed time
- [ ] P1-M2-F1c: engine.wait_until(condition) — yield and resume when true
- [ ] P1-M2-F1d: engine.wait_frames(n) — yield and resume after N frames
- [ ] P1-M2-F1e: Tick scheduler each frame (check conditions, resume)
- [ ] P1-M2-F1f: Error handling: pcall resume + traceback, remove faulted
- [ ] P1-M2-F1g: Test: wait(0.5) then set flag, verify at correct frame

#### P1-M2-G: Script Runtime Safety

- [ ] P1-M2-G1a: DAP JSON-RPC transport over TCP socket
- [ ] P1-M2-G1b: Handle initialize / launch / configurationDone
- [ ] P1-M2-G1c: setBreakpoints via lua_sethook
- [ ] P1-M2-G1d: continue / next / stepIn / stepOut
- [ ] P1-M2-G1e: stackTrace — walk Lua call stack
- [ ] P1-M2-G1f: scopes + variables (locals, upvalues, globals)
- [ ] P1-M2-G1g: evaluate expression in paused context
- [ ] P1-M2-G1h: Test: mock DAP client → breakpoint → verify pause at line
- [ ] P1-M2-G2a: Per-script sandbox (restricted globals, no io/os.execute)
- [ ] P1-M2-G2b: Allow-list safe functions (math, string, table, engine.*)
- [ ] P1-M2-G2c: CPU instruction limit per script per frame
- [ ] P1-M2-G2d: Memory limit per script environment
- [ ] P1-M2-G2e: Test: io.open→error, infinite loop→terminate, huge alloc→fail
- [~] P1-M2-G3a: Snapshot script state before reload (engine.persist vars)
- [~] P1-M2-G3b: Re-execute modified scripts in fresh sandbox
- [~] P1-M2-G3c: Restore snapshots, re-register callbacks
- [ ] P1-M2-G3d: On error: revert to previous version
- [ ] P1-M2-G3e: Test: modify script → reload → verify state survives
- [ ] P1-M2-G4a: Binding generator tool: parse annotated C++ headers
- [ ] P1-M2-G4b: Annotation syntax: // LUA_BIND: func(args) -> return
- [ ] P1-M2-G4c: Generator emits: validation, conversion, call, push return
- [ ] P1-M2-G4d: CMake pre-build step → generated_bindings.cpp
- [ ] P1-M2-G4e: Migrate ≥20 hand-written bindings to generated
- [ ] P1-M2-G4f: Test: bindgen processes header → valid C++ compiles

---

### P1-M3: Physics Engine Hardening

**Dependencies**: P1-M1, P1-M2

#### P1-M3-A: Collision Shapes

- [ ] P1-M3-A1a–g: Capsule collider (struct, 4 narrow-phase combos, broadphase, Lua, tests)
- [ ] P1-M3-A2a–f: Convex hull collider (quickhull, GJK/EPA, cross-shape, cooked, tests)
- [ ] P1-M3-A3a–e: Heightfield collider (ray, AABB, sphere queries, tests)

#### P1-M3-B: Constraint Solver

- [ ] P1-M3-B1a–c: Sequential impulse solver core (iterate N, warm start, CVar)
- [ ] P1-M3-B2a–g: Joint types (hinge, ball, slider, spring, fixed, Lua, tests)
- [ ] P1-M3-B3a–c: Contact manifold caching + reduction (4 contacts max)

#### P1-M3-C: Physics Materials and Layers

- [ ] P1-M3-C1a–d: Material (friction, restitution, density, combo rules, Lua)
- [ ] P1-M3-C2a–d: Collision layers + masks (bit flags, broadphase filter, Lua, tests)

#### P1-M3-D: Physics Queries

- [ ] P1-M3-D1a–f: Raycast (broadphase+narrow, sorted, closest-only, Lua, tests)
- [ ] P1-M3-D2a–d: Overlap sphere/box (Lua, tests)
- [ ] P1-M3-D3a–e: Sweep sphere/box (SweepHit, Lua, tests)

#### P1-M3-E: CCD Hardening

- [~] P1-M3-E1a–d: Bilateral advance (sphere-vs-mesh, CVar threshold, bullet test)
- [ ] P1-M3-E2a–c: Speculative contacts (expanded AABB, clamped impulse, tests)

---

### P1-M4: Asset Pipeline Production

**Dependencies**: P1-M1, P1-M3

#### P1-M4-A: Asset Database

- [ ] P1-M4-A1a–d: 64-bit hashing (xxHash64, update all typedefs, collision test)
- [ ] P1-M4-A2a–d: Metadata (struct, sidecar .meta, tags, import settings)
- [ ] P1-M4-A3a–d: Thumbnails (mesh render, texture downscale, editor display, invalidate)

#### P1-M4-B: Dependency Graph

- [ ] P1-M4-B1a–d: Build-time DAG (populate during import, persist JSON, transitive rebuild, tests)
- [ ] P1-M4-B2a–c: Runtime dependency awareness (get_dependencies, recursive load, tests)

#### P1-M4-C: Async Streaming

- [ ] P1-M4-C1a–d: Background load thread (LoadHandle, states, tests)
- [ ] P1-M4-C2a–c: Priority queue (Immediate/High/Normal/Low, updateable)
- [ ] P1-M4-C3a–c: Streaming budget (max MB, max uploads, tests)

#### P1-M4-D: LRU Eviction

- [ ] P1-M4-D1a–c: LRU tracking (access frame, doubly-linked list, evict head)
- [ ] P1-M4-D2a–d: Policy (cache_size_mb CVar, protected refs, eviction callbacks, tests)

#### P1-M4-E: Deterministic Cooking

- [ ] P1-M4-E1a–c: Byte-identical rebuild (no timestamps, content-hash, tests)
- [ ] P1-M4-E2a–c: Import settings round-trip (meta file, editor UI, tests)

---

### P1-M5: Renderer — Deferred Pipeline and Shadows

**Dependencies**: P1-M1, P1-M4

#### P1-M5-A: G-Buffer

- [ ] P1-M5-A1a–c: G-Buffer layout (4 MRT, FBO, completeness check)
- [ ] P1-M5-A2a–c: G-Buffer shaders (gbuffer.vert/frag, default textures)
- [ ] P1-M5-A3a–c: Debug visualization (CVar, fullscreen debug shader)

#### P1-M5-B: Deferred Lighting

- [ ] P1-M5-B1a–c: Fullscreen lighting (reconstruct pos, PBR BRDF, HDR output)
- [ ] P1-M5-B2a–e: Point lights (struct, uniform array, BRDF, frustum cull, Lua)
- [ ] P1-M5-B3a–d: Spot lights (cone falloff, frustum cull, Lua)
- [ ] P1-M5-B4a–d: Tiled light culling (16×16, min/max depth, per-tile list, tests)

#### P1-M5-C: Shadow Mapping

- [ ] P1-M5-C1a–h: CSM (FBO depth array, cascade splits, per-cascade render, PCF, CVar)
- [ ] P1-M5-C2a–c: Spot light shadows (perspective, CVar max)
- [ ] P1-M5-C3a–d: Point light cubemap shadows (6-face, geometry/multi-pass, CVar max)
- [ ] P1-M5-C4a–d: Shadow optimization (stable cascades, cache, LOD, visual test)

#### P1-M5-D: HDR Post-Processing

- [ ] P1-M5-D1a–c: PostProcessStack class (ordered passes, CVar toggles)
- [ ] P1-M5-D2a–e: Bloom (threshold, downsample, upsample, composite, test)
- [ ] P1-M5-D3a–e: SSAO (hemisphere, kernel+noise, bilateral blur, multiply ambient, CVars)
- [~] P1-M5-D4a–c: Tone mapping (3 operators, auto-exposure histogram, CVars)
- [ ] P1-M5-D5a–c: FXAA 3.11 (post-process pass, CVar, test)

#### P1-M5-E: Forward Transparency

- [ ] P1-M5-E1a–c: Transparent sort (back-to-front, alpha blend, depth write OFF)
- [ ] P1-M5-E2a–c: Transparent PBR shader (forward, shadows, Lua alpha)

---

### P1-M6: Renderer — Sky, Fog, Instancing, Materials

**Dependencies**: P1-M5

#### P1-M6-A: Sky and Atmosphere

- [ ] P1-M6-A1a–f: Skybox (cubemap component, shaders, depth, equirect import, Lua)
- [ ] P1-M6-A2a–e: Procedural sky (Preetham, params, dome, ambient influence, Lua)
- [ ] P1-M6-A3a–f: Environment probes (capture, prefilter IBL, BRDF LUT, sample, blend, editor)

#### P1-M6-B: Environment Fog

- [ ] P1-M6-B1a–c: Distance fog (linear/exp/exp2, CVars)
- [ ] P1-M6-B2a–c: Height fog (density falloff, ray-march, Lua)

#### P1-M6-C: GPU Instancing

- [ ] P1-M6-C1a–e: Static mesh instancing (group by mesh+material, SSBO, shader, cull, test)
- [ ] P1-M6-C2a–d: Foliage instancing (wind params, vertex displacement, LOD, Lua)

#### P1-M6-D: Material and Shader Variants

- [ ] P1-M6-D1a–e: Shader permutation manager (macro keys, bitmask, compile+cache, warm, test)
- [ ] P1-M6-D2a–e: Material instances (template+overrides, shared shader, Lua, editor)

#### P1-M6-E: Render-to-Texture

- [ ] P1-M6-E1a–b: RenderTarget class (FBO wrapper, pool by resolution)
- [ ] P1-M6-E2a–e: SceneCapture component (camera params, capture+render, expose texture, Lua, test)

---

### P1-M7: Animation System

**Dependencies**: P1-M4, P1-M5

- [ ] P1-M7-A1a–d: Skeleton loading (struct, glTF extract, binary cook, runtime load)
- [ ] P1-M7-A2a–e: Clip loading (struct, glTF extract, compress, decompress, roundtrip test)
- [ ] P1-M7-A3a–c: Clip sampling (binary search, lerp/slerp, loop/clamp)
- [ ] P1-M7-B1a–b: Two-clip blend (lerp/slerp per joint, crossfade)
- [ ] P1-M7-B2a–d: Blend tree (node tree, 1D/2D blend spaces)
- [ ] P1-M7-B3a–c: Additive blending (subtract reference, apply weighted)
- [ ] P1-M7-B4a–c: Masked/layered blending (BoneMask, upper/lower body split)
- [ ] P1-M7-C1a–c: State machine definition (AnimState, AnimTransition, parameters)
- [ ] P1-M7-C2a–d: State machine eval (transition check, crossfade, any-state)
- [ ] P1-M7-C3a–d: Lua API (set_param, play, get_current_state, test)
- [ ] P1-M7-D1a–d: Root motion (extract delta, remove from pose, apply to entity, Lua)
- [ ] P1-M7-E1a–b: Event definition (AnimEvent struct, store per clip)
- [ ] P1-M7-E2a–d: Event dispatch (detect crossing, fire C++/Lua, Lua binding, test)
- [ ] P1-M7-F1a–d: Montages (one-shot, bone mask overlay, sections, Lua)
- [ ] P1-M7-G1a–b: Bone matrix upload (compute final, uniform/SSBO)
- [ ] P1-M7-G2a–c: Skinning shader (bone weights, SKINNED variant, 4 influences)
- [ ] P1-M7-G3a–d: Two-bone IK (solver, apply after pose, Lua, foot placement)

---

### P1-M8: Audio Production

**Dependencies**: P1-M1, P1-M2

- [ ] P1-M8-A1a–d: Listener + emitter (structs, listener from camera, distance model)
- [ ] P1-M8-A2a–c: Distance attenuation (inverse clamped, per-emitter curves, test)
- [ ] P1-M8-A3a–b: HRTF / binaural (miniaudio CVar, fallback)
- [ ] P1-M8-B1a–d: Bus architecture (struct, default hierarchy, volume chain, Lua)
- [ ] P1-M8-B2a–c: Snapshots/ducking (AudioSnapshot, blend to, Lua)
- [ ] P1-M8-C1a–c: Reverb (Schroeder or miniaudio node, per-bus or per-emitter, zones)
- [ ] P1-M8-C2a–c: Low-pass/high-pass filter (biquad, occlusion sim, Lua)
- [ ] P1-M8-C3a–c: Doppler (relative velocity, pitch adjust, CVar factor)
- [ ] P1-M8-D1a–d: Audio events (candidates, randomization, cooldown, Lua)
- [ ] P1-M8-D2a–c: Music system (crossfade tracks, layered stems, Lua)
- [~] P1-M8-E1a–c: Streaming (>1MB files, configurable buffer, music=stream/SFX=memory)

---

### P1-M9: Editor Production

**Dependencies**: P1-M1, P1-M2, P1-M4

- [ ] P1-M9-A1a–e: Property inspector (iterate components, widget factory, nested, arrays, undo)
- [ ] P1-M9-A2a–c: Component add/remove (dropdown, remove button, undo)
- [~] P1-M9-B1a–d: Undo system (EditorCommand interface, two stacks, merge, limit 100)
- [ ] P1-M9-B2a–f: Command types (Transform, Property<T>, Create/Destroy, Add/Remove, Reparent, Ctrl+Z/Y)
- [ ] P1-M9-C1a–e: Hierarchy panel (tree, click/multi-select, drag-drop, context menu, search)
- [ ] P1-M9-D1a–e: Asset browser (folder tree, thumbnails, double-click inspect, drag-to-viewport, search)
- [ ] P1-M9-D2a–b: Asset operations (reimport, delete with dep warning, auto-detect changes)
- [ ] P1-M9-E1a–c: Prefab definition (JSON snapshot, save-as-prefab, instantiate)
- [ ] P1-M9-E2a–c: Prefab overrides (track overrides, inspector bold, revert/apply)
- [ ] P1-M9-E3a–c: Nested prefabs (recursive instantiation, propagation, test)
- [~] P1-M9-F1a–c: Play-in-editor (serialize to buffer, restore on stop, pause)
- [ ] P1-M9-F2a–b: Single-frame step (button + console command)
- [ ] P1-M9-G1a–d: Editor Lua API (menu items, custom panels, selected entities, undo)

---

### P1-M10: Scene Management and World Streaming

**Dependencies**: P1-M4, P1-M9

- [ ] P1-M10-A1a–e: Scene transition API (load, additive, unload, loading screen, Lua)
- [~] P1-M10-A2a–d: Persistent entity IDs (UUID, map on load, cross-scene refs, test)
- [ ] P1-M10-B1a–e: Streaming volumes (component, sub-scene ref, manager, hysteresis, async)
- [ ] P1-M10-B2a–d: LOD system (LODGroup component, distance select, hysteresis, Lua)
- [ ] P1-M10-C1a–e: Save system (SaveData struct, JSON slots, platform paths, Lua)
- [ ] P1-M10-C2a–c: Checkpoint system (auto-save, load checkpoint, test)

---

### P1-M11: UI System (Runtime Game UI)

**Dependencies**: P1-M5, P1-M2

- [ ] P1-M11-A1a–c: Canvas system (UICanvas, coordinate system, resolution independence)
- [ ] P1-M11-A2a–c: UI rendering (batched quads, ui.vert/frag, alpha blend)
- [ ] P1-M11-A3a–d: Font rendering (stb_truetype, SDF, text layout, rich text)
- [ ] P1-M11-B1a–g: Core widgets (image, text, button, slider, progress, toggle, input)
- [ ] P1-M11-B2a–d: Layout containers (hbox, vbox, grid, scroll, recursive sizing)
- [ ] P1-M11-C1a–d: Input routing (hit test, focus, input consumption, gamepad nav)
- [ ] P1-M11-C2a–c: UI animations (tween engine, hover/press, panel fade)
- [ ] P1-M11-D1a–g: Lua widget API (create canvas/text/button/image/progress, set props, remove)
- [ ] P1-M11-D2a–b: Data binding (ui_bind, observable variables)

---

### P1-M12: Platform, Packaging, Ship Readiness

**Dependencies**: All P1 milestones

- [ ] P1-M12-A1a–e: Platform interface (init/shutdown/paths, Win/Lin/Mac impls, CMake select)
- [ ] P1-M12-A2a–c: Filesystem abstraction (read/write/exists/list, VFS mount, test)
- [ ] P1-M12-B1a–d: Quality presets (Low/Med/High/Ultra, CVar batch, Lua)
- [ ] P1-M12-B2a–d: Dynamic resolution (frame time monitor, scale 50–100%, CAS upscale, CVar)
- [ ] P1-M12-C1a–c: Asset packing (archive bundle, VFS mount, strip editor-only)
- [ ] P1-M12-C2a–d: Executable packaging (CMake install, CPack Win/Lin/Mac)
- [ ] P1-M12-D1a–d: Crash handler (signal handlers, stack trace, crash log, dialog)
- [ ] P1-M12-D2a–b: Telemetry opt-in (anonymized, CVar default off)
- [ ] P1-M12-E1a–e: Localization (StringTable, localize(key), set_language, Lua, test)
- [ ] P1-M12-F1a–e: Accessibility (font scale, high contrast, colorblind, subtitles, Lua)
- [ ] P1-M12-G1a–d: End-to-end smoke tests (launch+play+save, 1000 entities, save/load, scene transitions)
- [ ] P1-M12-G2a–b: Memory leak detection (Valgrind/ASAN zero leaks, 5-min soak < 1MB delta)

---

## Phase 2: Competitive Feature Parity

---

### P2-M1: Advanced Rendering

**Dependencies**: P1-M5, P1-M6

- [ ] P2-M1-A1–A3: Lightmap baking (UV2, CPU path tracer, denoise, runtime sample, editor bake)
- [ ] P2-M1-B1a–d: SSR (Hi-Z ray march, edge fade, probe fallback, CVars)
- [ ] P2-M1-C1a–d: Volumetric fog (froxels, in-scatter, temporal reproject, composite)
- [ ] P2-M1-D1a–d: TAA (jitter, reproject, motion vectors, CVar)
- [ ] P2-M1-D2a–c: Motion blur (velocity sample, variable count, CVars)
- [ ] P2-M1-D3a–d: Depth of field (CoC, near/far blur, autofocus, Lua)
- [ ] P2-M1-D4a–d: Post-process volumes (PPV component, blend, priority, Lua)

---

### P2-M2: VFX / Particle System

**Dependencies**: P1-M5, P1-M6, P1-M3

- [ ] P2-M2-A1a–d: CPU particles (emitter, SoA buffer, spawn shapes, update)
- [ ] P2-M2-A2a–e: Forces (gravity, wind, turbulence, attractor, drag)
- [ ] P2-M2-A3a–d: Rendering (billboard, shaders, blend modes, sort)
- [ ] P2-M2-A4a–c: Curves over lifetime (size, color, velocity, editor)
- [ ] P2-M2-B1a–c: GPU particles (compute sim, indirect draw, 100K+)
- [ ] P2-M2-B2a–c: Collision (plane, depth buffer, sub-emit)
- [ ] P2-M2-B3a–c: Trails / ribbons (emit points, UV, width taper)
- [ ] P2-M2-B4a–b: Mesh particles (instanced mesh, per-particle transform)
- [ ] P2-M2-B5a–c: Lua particle API

---

### P2-M3: 2D Engine

**Dependencies**: P1-M5, P1-M3

- [ ] P2-M3-A1a–d: Sprite component (struct, SparseSet, batch, shaders)
- [ ] P2-M3-A2a–c: Sprite atlas (pack, JSON map, runtime resolve)
- [ ] P2-M3-A3a–c: Sprite animation (frames, animator, Lua)
- [ ] P2-M3-B1a–d: Tilemap (component, culled render, layers, Tiled import)
- [ ] P2-M3-B2a–c: Tilemap collision (solid flags, rect merge, static colliders)
- [ ] P2-M3-C1a–e: 2D collision shapes (circle, box, polygon, narrow, broadphase)
- [ ] P2-M3-C2a–d: 2D rigidbody (struct, step, joints, one-way)
- [ ] P2-M3-C3a–b: 2D raycasting (Lua)
- [ ] P2-M3-D1a–e: 2D camera (follow, bounds, zoom, shake, Lua)

---

### P2-M4: Networking Foundation

**Dependencies**: P1-M2, P1-M1

- [ ] P2-M4-A1a–b: UDP socket abstraction (Winsock/BSD, non-blocking)
- [ ] P2-M4-A2a–e: Reliable channel (sequence, ACK bitfield, resend, ordered, test)
- [ ] P2-M4-A3a–d: Connection management (handshake, heartbeat, disconnect, encryption)
- [ ] P2-M4-B1a–d: Entity replication (dirty flag, delta compress, client apply, priority)
- [ ] P2-M4-B2a–e: RPCs (definition, server/client/multicast, Lua)
- [ ] P2-M4-B3a–d: Network prediction (client predict, reconcile, interpolate, input buffer)
- [ ] P2-M4-C1a–c: Session management (player tracking, host migration, Lua)

---

### P2-M5: Splines, Data Tables, Gameplay Tools

**Dependencies**: P1-M2, P1-M9

- [ ] P2-M5-A1a–d: Spline data (control points, Catmull-Rom, arc-length, nearest point)
- [ ] P2-M5-A2a–c: Spline editor (debug draw, gizmos, tangent handles)
- [ ] P2-M5-A3a–c: Spline Lua API (evaluate, follow, use cases)
- [ ] P2-M5-B1a–d: Data table system (struct, typed columns, lookup, hot-reload)
- [ ] P2-M5-B2a–c: Lua data table API (load, get, get_row)
- [ ] P2-M5-C1a–e: CSG brushes (component, boolean ops, mesh gen, UV, editor)
- [ ] P2-M5-D1a–d: Foliage painting (brush, scatter, erase, instanced render)

---

### P2-M6: Controller Haptics and Advanced Input

**Dependencies**: P1-M2

- [ ] P2-M6-A1a–c: Gamepad rumble (SDL API, presets, Lua)
- [ ] P2-M6-A2a–c: Adaptive triggers (PS5 DualSense, fallback, Lua)
- [ ] P2-M6-B1a–c: Gyro input (SDL sensor, angular velocity to camera, Lua)
- [ ] P2-M6-C1a–d: Input recording/replay (record events, save/load, console commands)

---

### P2-M7: Advanced Editor Features

**Dependencies**: P1-M9, P1-M7, P2-M2

- [ ] P2-M7-A1a–d: Node graph core (Node, Link, ImGui render, interaction)
- [ ] P2-M7-A2a–c: Visual script compilation (graph → Lua, node library, live preview)
- [ ] P2-M7-B1a–c: Animation timeline (scrubber, keyframes, edit values)
- [ ] P2-M7-B2a–c: State machine editor (visual states, transitions, live highlight)
- [ ] P2-M7-C1a–d: Terrain (component, heightmap mesh, LOD, multi-texture splat)
- [ ] P2-M7-C2a–c: Terrain sculpt/paint tools (raise/lower/smooth, paint splat, brush params)
- [ ] P2-M7-D1a–c: Plugin system (interface, loader, sandbox)

---

### P2-M8: Performance Polish and Profiling

**Dependencies**: All P1, P2-M1

- [ ] P2-M8-A1a–c: Command buffer recording (worker threads, main playback, parallel cull)
- [ ] P2-M8-A2a–c: Parallel scene update (system scheduling, R/W deps, job DAG)
- [ ] P2-M8-B1a–c: Hierarchical culling (BVH, frustum cull subtrees, HZB occlusion)
- [ ] P2-M8-C1a–c: Shader binary cache (disk, content hash, invalidation)
- [ ] P2-M8-C2a–b: PSO sorting (64-bit sort key, radix sort)
- [ ] P2-M8-D1a–c: Memory budgets (per-system, warning at 80%, editor dashboard)

---

## Phase 3: Future / Cutting-Edge

---

### P3-M1: XR / VR / AR

**Dependencies**: P2-M1, P2-M6

- [ ] P3-M1-A1a–c: OpenXR session (instance, lifecycle, reference space)
- [ ] P3-M1-A2a–d: Stereo rendering (per-eye, swapchain, multi-view, frame submit)
- [ ] P3-M1-A3a–c: Head + controller tracking (pose, entity map, input actions)
- [ ] P3-M1-A4a–c: Hand tracking (26-joint, visual mesh, gestures)
- [ ] P3-M1-A5a–b: Passthrough AR (layer, composite)
- [ ] P3-M1-A6a–e: Lua XR API (is_active, head, hand, joints, haptics)

---

### P3-M2: Vulkan / Modern Graphics Backend

**Dependencies**: P2-M8

- [ ] P3-M2-A1a–c: RenderDevice abstraction (interface, GL impl, no raw GL above)
- [ ] P3-M2-A2a–e: Vulkan device (instance, swapchain, command buffers, all methods, sync)
- [ ] P3-M2-A3a–c: Shader cross-compilation (GLSL → SPIR-V, parallel outputs, runtime select)
- [ ] P3-M2-B1a–c: Compute shaders (dispatch, migrate particles, async compute)
- [ ] P3-M2-B2a–b: Bindless resources (descriptor indexing, perf gain)

---

### P3-M3: Mobile Platform Support

**Dependencies**: P1-M12, P2-M3

- [ ] P3-M3-A1a–c: Android NDK build (toolchain, NativeActivity, Gradle APK)
- [ ] P3-M3-A2a–d: Android specifics (GLES 3.0, touch, reduced quality, thermal)
- [ ] P3-M3-B1a–c: iOS Xcode build (toolchain, project gen, Metal)
- [ ] P3-M3-B2a–b: iOS specifics (lifecycle, CoreHaptics)
- [ ] P3-M3-C1a–c: Mobile UI (44pt touch targets, virtual joystick, gesture camera)

---

### P3-M4: Web / Emscripten Build

**Dependencies**: P1-M12, P3-M3

- [ ] P3-M4-A1a–c: Emscripten build (toolchain, WASM+JS+HTML, SDL port)
- [ ] P3-M4-A2a–d: Web adaptations (main loop, HTTP fetch, WebGL 2.0, Web Audio)
- [ ] P3-M4-A3a–c: Web optimization (strip+LTO+Oz, streaming instantiation, progressive load)

---

### P3-M5: AI and Navigation

**Dependencies**: P1-M3, P1-M2

- [ ] P3-M5-A1a–g: NavMesh generation (voxelize, walkable, regions, contours, triangulate, params, editor)
- [ ] P3-M5-A2a–e: Pathfinding (A*, funnel, dynamic obstacles, off-mesh links, Lua)
- [ ] P3-M5-B1a–e: Behavior tree (nodes, tick, blackboard, built-in actions, editor)
- [ ] P3-M5-B2a–f: Steering (seek, flee, arrive, avoid, flock, blend)
- [ ] P3-M5-B3a–c: Lua AI API (move_to, set_behavior_tree, blackboard)

---

### P3-M6: Advanced Networking and Dedicated Server

**Dependencies**: P2-M4

- [ ] P3-M6-A1a–c: Headless dedicated server (-dedicated, tick rate, console)
- [ ] P3-M6-A2a–c: Server authority (server mutates, validation, reconciliation)
- [ ] P3-M6-B1a–c: Lag compensation (state history, rewind hit check, max limit)
- [ ] P3-M6-C1a–d: Interest management (relevancy radius, priority, AOI grid, bandwidth budget)
- [ ] P3-M6-D1a–c: Matchmaking (lobby REST, browse/auto-match, Lua)

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
| P1: Ship Blockers | 12 | ~350 | 0 |
| P2: Competitive Parity | 8 | ~160 | 0 |
| P3: Cutting-Edge | 6 | ~90 | 0 |
| Parallel Lanes | 3 | 16 | 0 |
| **Total** | **29** | **~616** | **0** |
