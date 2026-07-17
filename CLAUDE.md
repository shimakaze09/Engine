# CLAUDE.md — Engine

Quick reference for Claude Code sessions. `AGENTS.md` is the full contributor
rulebook and wins on conflict. `TODO.md` is the production-gap tracker.
`REVIEW_FINDINGS.md` is the active bug/structure fix tracker — check it before
starting work; update its status markers in the same commit as a fix.

## What this is

C++20 game engine built from scratch: SDL2 window/input, OpenGL renderer
(deferred+forward, PBR/IBL), fixed-capacity ECS (65,536 entities, double-buffered
transforms), CPU-deterministic physics, Lua 5.4 scripting, miniaudio, ImGui editor.
Goal: production level.

## Hard rules (enforced; see AGENTS.md for full list)

- C++20 only. No exceptions, no RTTI, no `dynamic_cast`/`typeid`.
- Engine APIs `noexcept`; explicit return values + logged failure paths.
- No heap allocation on hot paths; fixed-size/preallocated storage.
- Dependency flow strictly downward:
  `app → editor → runtime → renderer/physics/scripting/audio → core/math`.
- Public headers must not leak SDL/OpenGL/Lua/ImGui/ImGuizmo types.
- Every file needs a file-level purpose comment (CI-audited by
  `tools/check_source_comments.py`). Comments must be REAL — no filler like
  `/// Handles foo.`; `tools/check_comment_quality.py` flags junk patterns.
- Changes to math/ECS/physics/renderer/scripting behavior require tests.
- Test strictness rule: assert the tested behavior EXACTLY (no loose
  tolerances); never assert wall-clock timing/throughput in functional tests.
  Only dedicated `engine_bench_*` tests hold performance thresholds.
- Determinism-sensitive areas (world, serialization, physics, render-prep, Lua
  API): pair changes with determinism tests.

## Build / test (Windows, clang-cl + Ninja; build/ dir already configured)

```powershell
cmake --build build --parallel                    # build
ctest --test-dir build --output-on-failure        # all tests
ctest --test-dir build --output-on-failure -R engine_unit_
ctest --test-dir build --output-on-failure -R engine_integration_
ctest --test-dir build --output-on-failure -LE gpu   # headless-safe
python tools/check_source_comments.py             # comment presence audit
python tools/check_comment_quality.py             # comment junk audit
```

Reconfigure (only if cache is broken):
```powershell
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Debug
```

## Layout cheat sheet

- `core/` logging, cvars, input, VFS, job system, event bus, allocators, SparseSet,
  FixedHashTable, FNV-1a hash, bounded string copy
- `math/` Vec/Mat/Quat/Transform — header-only INTERFACE lib, all functions inline
  (SSE2 paths in `math_detail.h`); there is no `math/src/`
- `physics/` bodies, colliders, CCD, solver, queries, joints; `PhysicsWorldView`
  interface keeps physics independent of runtime
- `renderer/` assets/streaming, shaders, command buffers, GL backend, post stack
- `scripting/` Lua runtime + 17 binding modules in `src/*.h` (private headers)
- `runtime/` engine bootstrap, `EnginePipeline` (13 stages), `World` ECS,
  serializers, bridges
- `editor/` ImGui editor, cameras, command history
- `tests/` unit / integration / smoke(gpu) / benchmark

## Working conventions for this campaign

- One finding = one focused commit (or small series). No drive-by rewrites.
- Fix order: P0 bugs → P1 perf → S* duplication → A* architecture → C1 comments.
- After each fix: build, run headless-safe tests, update REVIEW_FINDINGS.md.
- Private headers in `src/` are the established pattern for module-internal APIs
  — keep using it; do not move them into `include/`.
- When adding shared utilities, put them in `core` and migrate ALL duplicate
  call sites in the same series (don't leave a 6th copy).
