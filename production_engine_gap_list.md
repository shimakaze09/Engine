# Production Engine Gap List

> Cross-referenced with production_engine_milestones.md.
> Every item links to its milestone, sub-milestone, or work package.
> Priority: **[critical]** = Phase 1 ship blocker, **[high]** = Phase 2 competitive parity, **[low]** = Phase 3 cutting-edge.
> Status: '[ ]' = not started, '[~]' = partial/prototype, '[x]' = production-ready.

---

## Build, CI, and Infrastructure

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P1-M1-A | Unified CMake per-platform configuration | [critical] | [ ] |
| P1-M1-A2 | Precompiled headers (core, renderer) | [critical] | [ ] |
| P1-M1-A3 | Incremental compilation audit | [critical] | [ ] |
| P1-M1-B1 | GitHub Actions CI — build matrix (3 OS × 2 configs) | [critical] | [ ] |
| P1-M1-B2 | Static analysis lane (cppcheck + clang-tidy) | [critical] | [ ] |
| P1-M1-B3 | Sanitizer lane (ASAN + UBSAN + TSAN) | [critical] | [ ] |
| P1-M1-B4 | Code coverage reporting + threshold gate | [critical] | [ ] |
| P1-M1-B5 | Performance regression gate (ECS + physics benchmarks) | [critical] | [ ] |
| P1-M1-C1 | Cross-platform determinism tests (all CI platforms + thread counts) | [critical] | [ ] |
| P1-M1-C2 | ECS stress test at 50K+ entities | [critical] | [~] |
| P1-M1-D1 | Hot-path allocation audit and replacement | [critical] | [ ] |
| P1-M1-D2 | CPU profiler — hierarchical flame graph | [critical] | [~] |
| P1-M1-D3 | GPU profiler — timestamp queries per pass | [critical] | [ ] |
| P1-M1-D4 | In-game stats overlay (fps, draw calls, tri count, memory) | [critical] | [ ] |
| P1-M1-E1 | Debug camera (free-fly, detached from game camera) | [critical] | [ ] |
| P1-M1-E2 | God mode / noclip / spawn cheat commands | [critical] | [ ] |
| P1-M1-E3 | Per-subsystem memory tracking | [critical] | [ ] |

## World, ECS, and Gameplay Loop

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P1-M2-A | Actor lifecycle: BeginPlay / Tick / EndPlay (C++ and Lua) | [critical] | [ ] |
| P1-M2-A3 | Entity pooling (production-grade, not just free list) | [critical] | [ ] |
| P1-M2-B1 | Game mode / game state / player controller separation | [critical] | [ ] |
| P1-M2-B2 | Subsystem / service locator — replace ad-hoc globals | [critical] | [ ] |
| P1-M2-C1 | Input action / axis mapping with JSON config | [critical] | [ ] |
| P1-M2-C2 | Runtime input rebinding + persistence | [critical] | [ ] |
| P1-M2-C3 | Touch input and gesture recognizers | [critical] | [ ] |
| P1-M2-D | Timer manager — per-world, serializable | [critical] | [ ] |
| P1-M2-E | Gameplay camera (spring arm, camera manager, shake, blend) | [critical] | [ ] |
| P1-M2-F | Coroutines / async gameplay (engine.wait, wait_until, wait_frames) | [critical] | [ ] |
| P1-M2-G1 | Lua DAP debugger — full protocol implementation | [critical] | [ ] |
| P1-M2-G2 | Lua sandboxing — per-script isolation, CPU/memory limits | [critical] | [ ] |
| P1-M2-G3 | Lua hot-reload with state preservation | [critical] | [~] |
| P1-M2-G4 | Lua binding auto-generation tool | [critical] | [ ] |

## Physics

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P1-M3-A1 | Capsule collider (all narrow-phase combos) | [critical] | [ ] |
| P1-M3-A2 | Convex hull collider (GJK/EPA) | [critical] | [ ] |
| P1-M3-A3 | Heightfield collider (terrain) | [critical] | [ ] |
| P1-M3-B1 | Sequential impulse solver with warm starting | [critical] | [ ] |
| P1-M3-B2 | Joint types: hinge, ball, slider, spring, fixed | [critical] | [ ] |
| P1-M3-B3 | Persistent contact manifolds with reduction | [critical] | [ ] |
| P1-M3-C1 | Physics material (friction, restitution, density combos) | [critical] | [ ] |
| P1-M3-C2 | Collision layers and masks (bit flags) | [critical] | [ ] |
| P1-M3-D1 | Raycast with sorting and layer filtering | [critical] | [ ] |
| P1-M3-D2 | Sphere / box overlap queries | [critical] | [ ] |
| P1-M3-D3 | Shape cast (sweep sphere / sweep box) | [critical] | [ ] |
| P1-M3-E1 | CCD hardening — bilateral advance, sphere-vs-mesh | [critical] | [~] |
| P1-M3-E2 | Speculative contacts | [critical] | [ ] |

## Asset Pipeline

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P1-M4-A1 | 64-bit asset hashing (replace 32-bit FNV) | [critical] | [ ] |
| P1-M4-A2 | Asset metadata (tags, import settings, file size, checksum) | [critical] | [ ] |
| P1-M4-A3 | Thumbnail generation (mesh + texture previews) | [critical] | [ ] |
| P1-M4-B1 | Build-time dependency graph (transitive invalidation) | [critical] | [ ] |
| P1-M4-B2 | Runtime dependency awareness (recursive load ordering) | [critical] | [ ] |
| P1-M4-C1 | Async loading thread with load states (Queued→Loading→Ready) | [critical] | [ ] |
| P1-M4-C2 | Priority queue for load requests | [critical] | [ ] |
| P1-M4-C3 | Streaming budget (max MB in flight, max uploads per frame) | [critical] | [ ] |
| P1-M4-D | LRU eviction cache with protected refs | [critical] | [ ] |
| P1-M4-E | Deterministic cooking (byte-identical rebuild) | [critical] | [ ] |

## Renderer — Deferred Pipeline and Shadows (P1-M5)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P1-M5-A | G-Buffer pass (MRT: albedo+metallic, normal+roughness, emissive+AO, depth) | [critical] | [ ] |
| P1-M5-B1 | Deferred lighting — fullscreen PBR (Cook-Torrance) | [critical] | [ ] |
| P1-M5-B2 | Point lights (SparseSet, frustum-culled, up to 128) | [critical] | [ ] |
| P1-M5-B3 | Spot lights (cone falloff, frustum-culled) | [critical] | [ ] |
| P1-M5-B4 | Tiled light culling (16×16 tiles, per-tile light list) | [critical] | [ ] |
| P1-M5-C1 | Cascaded shadow maps (directional, 4 cascades, PCF) | [critical] | [ ] |
| P1-M5-C2 | Spot light shadow maps | [critical] | [ ] |
| P1-M5-C3 | Point light cubemap shadows | [critical] | [ ] |
| P1-M5-C4 | Shadow optimization (stable cascades, cache, LOD) | [critical] | [ ] |
| P1-M5-D1 | Post-process stack architecture (ping-pong RT, per-pass CVar) | [critical] | [ ] |
| P1-M5-D2 | Bloom (threshold + dual-kawase down/up + composite) | [critical] | [ ] |
| P1-M5-D3 | Screen-space ambient occlusion (SSAO + bilateral blur) | [critical] | [ ] |
| P1-M5-D4 | Tone mapping (Reinhard, ACES, Uncharted 2) + auto-exposure | [critical] | [~] |
| P1-M5-D5 | FXAA anti-aliasing | [critical] | [ ] |
| P1-M5-E | Forward transparency pass (sorted, alpha blend, PBR) | [critical] | [ ] |

## Renderer — Sky, Fog, Instancing, Materials (P1-M6)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P1-M6-A1 | Skybox (cubemap, HDR equirect import) | [critical] | [ ] |
| P1-M6-A2 | Procedural sky (Preetham/Hosek-Wilkie) | [critical] | [ ] |
| P1-M6-A3 | Environment reflection probes (prefiltered IBL, BRDF LUT) | [critical] | [ ] |
| P1-M6-B1 | Distance fog (linear / exp / exp2) | [critical] | [ ] |
| P1-M6-B2 | Height fog (height-based density, ray-marched) | [critical] | [ ] |
| P1-M6-C1 | Static mesh GPU instancing (glDrawElementsInstanced) | [critical] | [ ] |
| P1-M6-C2 | Foliage instancing (wind vertex displacement, per-instance LOD) | [critical] | [ ] |
| P1-M6-D1 | Shader variant / permutation system (macro-based, cached) | [critical] | [ ] |
| P1-M6-D2 | Material instance system (shared shader, per-instance params) | [critical] | [ ] |
| P1-M6-E | Render-to-texture / scene capture component | [critical] | [ ] |

## Animation System (P1-M7)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P1-M7-A | Skeleton + clip loading from glTF, compressed storage | [critical] | [ ] |
| P1-M7-B | Pose blending (crossfade, 1D/2D blend space, additive, masked) | [critical] | [ ] |
| P1-M7-C | Animation state machine (states, transitions, parameters) | [critical] | [ ] |
| P1-M7-D | Root motion extraction and application | [critical] | [ ] |
| P1-M7-E | Animation events / notifies (fire Lua callbacks at keyframe times) | [critical] | [ ] |
| P1-M7-F | Montages (one-shot overlays with sections) | [critical] | [ ] |
| P1-M7-G1 | Skinned mesh rendering (bone matrices, GPU skinning shader) | [critical] | [ ] |
| P1-M7-G3 | Two-bone IK (foot placement, arm reach) | [critical] | [ ] |

## Audio System (P1-M8)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P1-M8-A | 3D positional audio (attenuation, panning, HRTF option) | [critical] | [ ] |
| P1-M8-B | Mixer / bus hierarchy (master→music/sfx/dialogue, volume chain) | [critical] | [ ] |
| P1-M8-B2 | Audio snapshots / ducking | [critical] | [ ] |
| P1-M8-C1 | Reverb (Schroeder or miniaudio node, reverb zones) | [critical] | [ ] |
| P1-M8-C2 | Low-pass / high-pass filter (biquad, occlusion sim) | [critical] | [ ] |
| P1-M8-C3 | Doppler effect | [critical] | [ ] |
| P1-M8-D | Audio events (randomized candidates, cooldown, positional) | [critical] | [ ] |
| P1-M8-D2 | Music system (crossfade, layered stems) | [critical] | [ ] |
| P1-M8-E | Audio streaming for large files | [critical] | [~] |

## Editor (P1-M9)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P1-M9-A | Reflection-driven property inspector (all types, nested structs, arrays) | [critical] | [ ] |
| P1-M9-A2 | Component add/remove from inspector | [critical] | [ ] |
| P1-M9-B | Undo/redo — command pattern, all operations, Ctrl+Z/Y | [critical] | [~] |
| P1-M9-C | Scene hierarchy panel (tree, drag-drop reparent, multi-select) | [critical] | [ ] |
| P1-M9-D | Asset browser (thumbnails, drag-to-viewport, search+tags) | [critical] | [ ] |
| P1-M9-E | Prefab system (save, instantiate, overrides, nested) | [critical] | [ ] |
| P1-M9-F | Play-in-editor (save/restore world state, pause, step) | [critical] | [~] |
| P1-M9-G | Editor Lua scripting API (menu items, custom panels) | [critical] | [ ] |

## Scene Management and Streaming (P1-M10)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P1-M10-A1 | Scene transition API (exclusive, additive, unload) | [critical] | [ ] |
| P1-M10-A2 | Persistent entity IDs (UUID, cross-scene references) | [critical] | [~] |
| P1-M10-B1 | Streaming volumes (trigger async sub-scene load/unload) | [critical] | [ ] |
| P1-M10-B2 | LOD system (distance-based mesh switching, hysteresis) | [critical] | [ ] |
| P1-M10-C | Save system (multi-slot, checkpoints, platform-aware paths) | [critical] | [ ] |

## UI System — Game Runtime (P1-M11)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P1-M11-A | UI canvas, coordinate system, resolution independence | [critical] | [ ] |
| P1-M11-A2 | UI rendering pipeline (batched 2D quads, alpha blend) | [critical] | [ ] |
| P1-M11-A3 | Font rendering (stb_truetype/Freetype, SDF, rich text) | [critical] | [ ] |
| P1-M11-B | Widget library (text, image, button, slider, progress, toggle, input) | [critical] | [ ] |
| P1-M11-B2 | Layout containers (hbox, vbox, grid, scroll view) | [critical] | [ ] |
| P1-M11-C1 | UI input routing (hit test, focus, gamepad nav, input consumption) | [critical] | [ ] |
| P1-M11-C2 | UI animations / tweens | [critical] | [ ] |
| P1-M11-D | Lua UI API (create, bind, animate, remove widgets) | [critical] | [ ] |

## Platform, Packaging, Ship Readiness (P1-M12)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P1-M12-A | Platform abstraction (Windows/Linux/macOS, filesystem, save paths) | [critical] | [ ] |
| P1-M12-B | Quality settings (Low/Med/High/Ultra presets, dynamic resolution) | [critical] | [ ] |
| P1-M12-C | Distribution packaging (asset packing, CPack, Win/Lin/Mac) | [critical] | [ ] |
| P1-M12-D | Crash handler (stack trace, log, user dialog) | [critical] | [ ] |
| P1-M12-E | Localization (string tables, runtime language switch, ICU fallback) | [critical] | [ ] |
| P1-M12-F | Accessibility (font scaling, high contrast, colorblind, subtitles) | [critical] | [ ] |
| P1-M12-G | End-to-end smoke tests + memory leak detection | [critical] | [ ] |

---

## Phase 2 — Competitive Feature Parity

### Advanced Rendering (P2-M1)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P2-M1-A | Lightmap baking (CPU path tracer, UV2, denoise) | [high] | [ ] |
| P2-M1-B | Screen-space reflections (Hi-Z ray march) | [high] | [ ] |
| P2-M1-C | Volumetric fog / god rays (froxel, temporal reprojection) | [high] | [ ] |
| P2-M1-D1 | Temporal anti-aliasing (TAA, jitter, velocity buffer) | [high] | [ ] |
| P2-M1-D2 | Motion blur (per-object velocity) | [high] | [ ] |
| P2-M1-D3 | Depth of field (CoC, near/far blur, autofocus) | [high] | [ ] |
| P2-M1-D4 | Post-process volumes (spatial blending) | [high] | [ ] |

### VFX / Particle System (P2-M2)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P2-M2-A | CPU particle emitter (spawn, forces, curves, billboard rendering) | [high] | [ ] |
| P2-M2-B1 | GPU particle simulation (compute shader, 100K+) | [high] | [ ] |
| P2-M2-B2 | Particle collision (plane, depth buffer, sub-emit) | [high] | [ ] |
| P2-M2-B3 | Trails / ribbon emitters | [high] | [ ] |
| P2-M2-B4 | Mesh particles (instanced mesh per particle) | [high] | [ ] |

### 2D Engine (P2-M3)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P2-M3-A | Sprite renderer (batched, atlas, animation, z-order) | [high] | [ ] |
| P2-M3-B | Tilemap (Tiled import, culled rendering, collision) | [high] | [ ] |
| P2-M3-C | 2D physics (circle/box/polygon, joints, one-way platforms) | [high] | [ ] |
| P2-M3-D | 2D camera (follow, bounds, zoom, shake) | [high] | [ ] |

### Networking (P2-M4)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P2-M4-A | Reliable UDP transport (sequence, ACK, retransmit, encryption) | [high] | [ ] |
| P2-M4-B1 | Entity replication (dirty-flag delta compression) | [high] | [ ] |
| P2-M4-B2 | RPCs (server/client/multicast) | [high] | [ ] |
| P2-M4-B3 | Client prediction + server reconciliation | [high] | [ ] |
| P2-M4-C | Lobby / session management (host migration) | [high] | [ ] |

### Splines, Data Tables, Gameplay Tools (P2-M5)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P2-M5-A | Spline / path component (Catmull-Rom, arc-length, editor gizmos) | [high] | [ ] |
| P2-M5-B | Data tables (CSV/JSON, typed columns, Lua query, hot-reload) | [high] | [ ] |
| P2-M5-C | CSG brushes (boolean ops, UV gen, editor preview) | [high] | [ ] |
| P2-M5-D | Foliage painting tool (brush, density, erase, instanced render) | [high] | [ ] |

### Controller Haptics and Advanced Input (P2-M6)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P2-M6-A1 | Gamepad rumble (low/high freq, presets) | [high] | [ ] |
| P2-M6-A2 | Adaptive triggers (PS5 DualSense) | [high] | [ ] |
| P2-M6-B | Gyroscope / motion input (gyro aiming) | [high] | [ ] |
| P2-M6-C | Input recording / replay (deterministic repro) | [high] | [ ] |

### Advanced Editor Features (P2-M7)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P2-M7-A | Visual scripting (node graph → Lua compilation, live preview) | [high] | [ ] |
| P2-M7-B | Animation editor (timeline, state machine visual editor) | [high] | [ ] |
| P2-M7-C | Terrain editor (heightmap LOD, sculpt, paint, multi-texture splat) | [high] | [ ] |
| P2-M7-D | Plugin system (shared lib loading, sandboxed API) | [high] | [ ] |

### Performance Polish (P2-M8)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P2-M8-A | Multi-threaded rendering (command buffer recording, parallel update) | [high] | [ ] |
| P2-M8-B | Hierarchical culling (BVH frustum cull, HZB occlusion cull) | [high] | [ ] |
| P2-M8-C | Shader binary cache + PSO sorting | [high] | [ ] |
| P2-M8-D | Per-system memory budgets with tracking | [high] | [ ] |

---

## Phase 3 — Future / Cutting-Edge

### XR / VR / AR (P3-M1)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P3-M1-A | OpenXR integration (session, stereo rendering, tracking) | [low] | [ ] |
| P3-M1-A4 | Hand tracking + gesture detection | [low] | [ ] |
| P3-M1-A5 | Passthrough AR | [low] | [ ] |

### Vulkan Backend (P3-M2)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P3-M2-A | RenderDevice abstraction (GL + Vulkan backends) | [low] | [ ] |
| P3-M2-A3 | Shader cross-compilation (GLSL → SPIR-V) | [low] | [ ] |
| P3-M2-B | Vulkan-specific (compute, bindless, async compute) | [low] | [ ] |

### Mobile (P3-M3)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P3-M3-A | Android build (NDK, GLES 3.0, APK/AAB) | [low] | [ ] |
| P3-M3-B | iOS build (Xcode, Metal/MoltenVK) | [low] | [ ] |
| P3-M3-C | Mobile UI adaptation (touch targets, virtual joystick) | [low] | [ ] |

### Web / Emscripten (P3-M4)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P3-M4-A | Emscripten/WASM build (WebGL 2.0, HTTP asset fetch) | [low] | [ ] |

### AI and Navigation (P3-M5)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P3-M5-A | NavMesh generation (voxelize, contour, triangulate) | [low] | [ ] |
| P3-M5-A2 | Pathfinding (A*, funnel, dynamic obstacles, off-mesh links) | [low] | [ ] |
| P3-M5-B1 | Behavior trees (selector/sequence/decorator, blackboard) | [low] | [ ] |
| P3-M5-B2 | Steering behaviors (seek, flee, arrive, flocking) | [low] | [ ] |

### Advanced Networking (P3-M6)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| P3-M6-A | Dedicated server mode (headless, configurable tick rate) | [low] | [ ] |
| P3-M6-B | Lag compensation (server-side rewind) | [low] | [ ] |
| P3-M6-C | Interest management (relevancy, AOI grid, bandwidth budget) | [low] | [ ] |
| P3-M6-D | Matchmaking (lobby server, simple auto-match) | [low] | [ ] |

---

## Parallel Lanes (Non-Blocking)

| Ref | Gap | Priority | Status |
|-----|-----|----------|--------|
| DOC-1..7 | Documentation (getting started, arch, Lua API, editor, assets, net, contributing) | [high] | [ ] |
| TEST-1..5 | Extended testing (fuzz, property-based, soak, platform, screenshot) | [high] | [ ] |
| DEVOPS-1..4 | DevOps (nightly, cross-platform, release pipeline, dep updates) | [high] | [ ] |

---

## Summary Statistics

| Phase | Milestones | Gap Items | Status |
|-------|-----------|-----------|--------|
| P1: Ship Blockers | 12 | ~115 | 0 complete, ~7 partial |
| P2: Competitive Parity | 8 | ~40 | 0 complete |
| P3: Cutting-Edge | 6 | ~15 | 0 complete |
| Parallel Lanes | 3 | ~16 | 0 complete |
| **Total** | **29** | **~186** | **0 complete** |

---

## What IS Production-Ready (Verified)

These items are NOT gaps — they are confirmed working:

- [x] Console / CVar system (4 types, runtime mutation) — core
- [x] Debug draw API (lines, spheres, text with frame lifetimes) — core
- [x] Lua module system (require caching, circular dependency detection) — scripting
- [x] Lua error messages (file + line via luaL_traceback) — scripting
- [x] Lua profiler hooks (per-function call sample counts) — scripting
- [x] Gamepad input (SDL gamepad, proper analog dead zones) — core
- [x] Math library (Vec2/3/4, Quat, Mat4, Transform, AABB, Sphere, Ray) — math
- [x] Job system (async work queue, dependency DAG, worker thread pool) — core
- [x] Event bus (type-safe typed events + named channels) — core
- [x] ECS SparseSet (cache-friendly dense storage, double-buffered transforms) — runtime
- [x] Physics basics (AABB/sphere colliders, sleep, spatial hash, CCD, distance joints) — physics
- [x] Scene serialization (JSON save/load with persistent IDs) — runtime
- [x] Forward renderer (PBR shader, frustum culling, glTF mesh loading) — renderer
- [x] Editor (ImGui inspector, gizmo transforms, play/pause/stop, undo for transforms) — editor
- [x] Audio (basic playback via miniaudio: wav/mp3/ogg/flac, volume/pitch/loop) — audio
- [x] Asset packer (glTF mesh import, incremental mtime-based rebuild) — tools
