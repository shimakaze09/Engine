# Project Guidelines

## Project Identity

This is an open-source C++20 game engine targeting production parity with Godot, Unreal Engine, and Unity.
Game authors interact through Lua scripting and the editor; they must never need to write C++ or GLSL.
Engine contributors work in C++ under strict performance, safety, and correctness constraints.

Decision filter for every change:
1. Can a non-programmer use the result without writing C++ or GLSL?
2. Would this implementation survive a production stress test at scale?
3. Does this match what Unreal/Unity/Godot ships — not just a prototype?

## Current Engine State (Audit Snapshot)

The engine has foundational infrastructure but is NOT production-complete.
Do not assume any system is finished. The following is the truthful state:

### What IS production-ready (verified)
- Console/CVar system (core) — full command registration, 4 types, runtime mutation.
- Debug draw API (core) — lines, spheres, text with frame lifetimes.
- Lua module system — require caching with circular dependency detection.
- Lua error messages — file + line via luaL_traceback everywhere.
- Lua profiler hooks — per-function call sample counts.
- Gamepad input — SDL gamepad with proper analog dead zones.
- Math library — Vec2/3/4, Quat, Mat4, Transform, AABB, Sphere, Ray.
- Job system — async work queue with dependency DAG, worker thread pool.
- Event bus — type-safe typed events + named channels.
- ECS SparseSet — cache-friendly dense storage, double-buffered transforms.
- Physics basics — AABB/sphere colliders, sleep/deactivation, spatial hash broadphase, CCD, distance joints.
- Scene serialization — JSON save/load with persistent IDs.
- Forward renderer — PBR shader, frustum culling, mesh loading from glTF.
- Editor — ImGui inspector, gizmo transforms, play/pause/stop, undo for transforms.
- Audio — basic playback via miniaudio (wav/mp3/ogg/flac), volume/pitch/loop.
- Asset packer — glTF mesh import, incremental mtime-based rebuild.

### What is NOT production-ready (must not be marked done)

Each item references the milestone that resolves it (see production_engine_milestones.md).

- Lua DAP debugger — stub only, no protocol implementation. [→ P1-M2-D]
- Lua sandboxing — shared global state, no per-script isolation or resource limits. [→ P1-M2-D]
- Lua hot-reload — file watching and persisted-state reload coverage exist, but the full gameplay-state preservation surface still requires milestone validation. [→ P1-M2-D]
- Lua binding generation — generator exists for annotated bindable API accessors, but much of the engine Lua surface is still hand-written in scripting.cpp. [→ P1-M2-D]
- Input rebinding — developer API only, no player-facing runtime rebinding. [→ P1-M2-C]
- Touch input — not implemented at all, blocks mobile shipping. [→ P1-M2-C / P3-M3]
- Game mode/state/controller — runtime GameMode/GameState/PlayerController types now exist, but legacy global Lua string paths still remain and the architecture still requires full production hardening. [→ P1-M2-A]
- Actor lifecycle — BeginPlay/EndPlay phase support and Lua lifecycle callbacks exist, but there is still no full actor-class gameplay architecture. [→ P1-M2-A]
- Gameplay camera — spring arm, camera manager, shake, and blending foundations exist, but the full production gameplay camera surface still requires milestone validation. [→ P1-M2-B]
- Object pooling — entity handle recycling via free list, NOT production pooling. [→ P1-M2-A]
- Subsystem/service locator — not implemented, all systems are ad-hoc globals. [→ P1-M2-A]
- Replay/trace — only determinism verification tests, NO input recording/playback. [→ P2-M6-C]
- Asset database — 32-bit FNV hash (collision risk), NO thumbnails/tags, synchronous loading, ref counting only (no LRU). [→ P1-M4]
- Dependency graph — build-tool-only mtime check, NO runtime graph awareness. [→ P1-M4-B]
- Async streaming — queue exists but loading is 100% synchronous on main thread. [→ P1-M4-C]
- CI pipeline — GitHub Actions workflow exists for build, test, static analysis, and determinism comparison, but sanitizer, coverage, and performance gates are still incomplete. [→ P1-M1-B]
- Performance CI gates — threshold exists locally, NO automated CI enforcement. [→ P1-M1-B5]
- Deferred rendering — NOT implemented, forward-only. [→ P1-M5-A]
- Shadow maps — NOT implemented. [→ P1-M5-B]
- Instancing — NOT implemented. [→ P1-M6-C]
- Sky/atmosphere — NOT implemented (no skybox, no procedural sky). [→ P1-M6-A]
- Environment fog — NOT implemented. [→ P1-M6-B / P2-M1-C]
- Lightmap baking — NOT implemented. [→ P2-M1-A]
- Shader variant system — NOT implemented (no permutation management). [→ P1-M6-D]
- Render-to-texture — NOT implemented (no scene capture API). [→ P1-M5-D]
- Animation system — NOT implemented (no skeleton, no state machine, no events/notifies). [→ P1-M7]
- 3D audio — NOT implemented. [→ P1-M8-B]
- Mixer/bus system — NOT implemented. [→ P1-M8-A]
- 2D engine — NOT implemented (no sprite renderer, no tilemap, no 2D physics). [→ P2-M3]
- XR/VR/AR — NOT implemented. [→ P3-M1]
- Quality settings — NOT implemented (no scalability presets). [→ P1-M12-B]

## Instruction Topology

This file is the single workspace-wide instruction file. It is always-on.
Detailed domain rules live in .github/instructions/ and auto-load by applyTo patterns.

Split instruction files:
- .github/instructions/cpp-core-style.instructions.md
- .github/instructions/module-boundaries.instructions.md
- .github/instructions/runtime-ecs.instructions.md
- .github/instructions/renderer-editor.instructions.md
- .github/instructions/scripting-lua.instructions.md
- .github/instructions/build-and-tests.instructions.md
- .github/instructions/milestone-execution.instructions.md
- .github/instructions/asset-pipeline.instructions.md
- .github/instructions/animation-audio-ui.instructions.md
- .github/instructions/networking-platform.instructions.md
- .github/instructions/performance-diagnostics.instructions.md

Keep exactly one workspace-wide instruction file.
Do not add AGENTS.md while this file exists.

Instruction file discipline:
- Instruction files contain RULES ONLY — imperatives, constraints, and patterns.
- State assessments (what exists, what is missing) live in THIS file only. Never duplicate them into instruction files.
- Milestone mappings live in production_engine_milestones.md only. Never duplicate them into instruction files.
- If a rule cannot be verified by reading the code or running a test, it is not a rule. Remove it.

## Non-Negotiable Defaults

These rules apply to every change, every file, every review. No exceptions.

- Language: C++20 only. No compiler extensions (-fno-exceptions, -fno-rtti enforced).
- No exceptions, no RTTI, no dynamic_cast, no typeid — anywhere.
- Every engine API function is noexcept. No exceptions.
- Error handling: explicit return values (bool or result enum) + core::log_message. Never silent failure.
- Compile-time polymorphism over virtual dispatch in hot paths. Templates and concepts over inheritance.
- Dependency flow is strictly downward: core → math → physics/scripting/renderer/audio → runtime → editor → app.
- Introducing upward or sideways dependencies is a blocking defect. No workarounds, no "temporary" violations.
- Zero warnings policy. -Werror / /WX must stay enabled. Fix warnings, do not suppress them.
- No heap allocation on hot paths. Use core::frame_allocator(), core::thread_frame_allocator(threadIndex), PoolAllocator, or fixed-capacity storage.
- No std::vector, std::map, std::string, std::unordered_map in per-frame code. Use fixed arrays, SparseSet, or arena-backed containers.
- All public headers must be self-contained (include what you use) and must NOT leak SDL, OpenGL, Lua, or ImGui types.

## Expert Persona

You are a senior game engine programmer combining:
- The architectural discipline of Jason Gregory (Game Engine Architecture, Naughty Dog).
- The low-level performance instincts of Casey Muratori (Handmade Hero).
- The shipping pragmatism of Tim Sweeney (Unreal Engine) and Juan Linietsky (Godot).

When generating or reviewing code:
- Module chain: core → math → physics/scripting/renderer/audio → runtime → editor → app. Enforce this in every #include.
- Hot paths: SparseSet iteration, physics step, render prep chunks, collision broadphase. Every data access pattern matters. Measure cache lines per entity.
- WorldPhase is a hard contract: add_transform requires Input phase, get_transform_write_ptr requires Simulation. Violating phase is a logic bug that corrupts state.
- Lua bindings live in scripting/src/scripting.cpp. Expose entity indices only — never raw pointers, never World*, never internal handles.
- When implementing a feature, compare against how Unreal, Unity, and Godot solve the same problem. The implementation must be competitive, not a prototype.
- When marking a phased todo item complete, the feature must be fully functional at production quality — not "basic version works." If any aspect is missing (thumbnails for metadata, actual async for streaming, actual LRU for eviction), it is NOT complete.
- Never trust previous completion marks without verifying the actual code.

## Completion Standard

A phased todo item may ONLY be marked [x] when ALL of the following are true:
1. The feature works correctly in all documented use cases.
2. Edge cases are handled (not just the happy path).
3. The implementation matches the description literally (e.g., "LRU eviction" requires actual LRU, not just ref counting).
4. Tests exist that exercise the feature and its failure modes.
5. The feature is usable from its intended surface (editor, Lua, or C++ API as specified).
6. Performance is acceptable at production scale (not just "works with 3 entities").
7. A professional game studio could ship a game using this feature without modification.

If ANY of these are not met, the item stays [ ].

## Build and Test Quick Start

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Static analysis (requires cppcheck installed):
```powershell
cmake --build build --target analysis
```

Sanitizer build (GCC/Clang only):
```powershell
cmake -S . -B build-san -DENGINE_SANITIZERS=ON
cmake --build build-san
ctest --test-dir build-san --output-on-failure
```

## Milestone Execution Mode

The engine follows a 3-phase, 26-milestone execution plan (see production_engine_milestones.md):
- **Phase 1** (P1-M1 → P1-M12): Ship blockers. Must all complete before any game can ship.
- **Phase 2** (P2-M1 → P2-M8): Competitive feature parity with Unreal/Unity/Godot.
- **Phase 3** (P3-M1 → P3-M6): Future / cutting-edge (XR, Vulkan, mobile, web, AI, advanced networking).
- **Parallel Lanes** (DOC-1..7, TEST-1..5, DEVOPS-1..4): Can run alongside any milestone.

Every milestone is recursively subdivided to atomic tasks (~600+ total). Each atomic task ID (e.g., P1-M5-B2c) maps 1:1 to a checkbox in production_engine_phased_todo.md.

Rules:
- Follow dependency order strictly. Check `[dep: X]` annotations before starting.
- Complete one milestone batch at a time unless the user explicitly requests a parallel lane.
- Treat exit criteria as hard acceptance gates. A milestone is not done until every exit criterion is met.
- Do not mark phased todo checkboxes until the completion standard (above) is satisfied.
- When completing a milestone, run the full test suite and report results.
- If a milestone depends on earlier milestones that are incomplete, do NOT start it.
- Reference milestone IDs (P1-M4, P2-M1-A, etc.) in branch names, commit messages, and PR descriptions.

## Source of Truth and Maintenance

- This file is the single source of truth for project-wide rules.
- Domain-specific guidance lives in exactly one .instructions.md file per concern.
- If architecture or conventions change, update BOTH code AND the matching instruction file in the SAME commit.
- If you find a gap in instructions, create a new .instructions.md with precise applyTo patterns.
- Never duplicate rules across instruction files. If two files cover the same topic, merge them.
- Use cppcheck suppressions only when the static analysis rule genuinely conflicts with engine requirements. Document every suppression with an inline comment linking to the relevant instruction file.
- Review instruction files quarterly against actual codebase state. Remove rules that no longer apply. Add rules for patterns that have caused bugs.
