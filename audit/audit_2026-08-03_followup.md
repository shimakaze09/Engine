# Engine Audit Follow-up — 2026-08-03

Revision audited: `main` @ 893ade4. Companion to `audit/cpp23_engine_audit_report.md`
(2026-08-01); this report reconciles that audit's findings against what actually
merged, reviews open PRs #51 and #52, verifies the build/test contract locally,
and records new findings from three fresh adversarial sweeps (concurrency/lifetime,
serialization/data safety, renderer/physics/scripting). Method: local Linux
build + headless ctest, PR-diff review at head revisions, and code-verified
spot checks on main for every Critical/High closure claim.

## Executive summary

- Local Linux (gcc) build is warning-clean; headless tests pass 111/112. The one
  failure, `engine_integration_ecs_stress_50k`, is itself a finding (N-15): it
  asserts a 24 ms wall-clock threshold inside a functional integration test,
  which the CLAUDE.md hard rule forbids.
- All five Criticals from the 2026-08-01 audit are verified fixed on main.
- **PR #50's closure table misnumbers two findings**: its "H-07" row is actually
  H-02 (entity pool) and its "H-08" row is actually H-01 (serialization gaps).
  The report's real **H-07 (broad phase/CCD) and H-08 (contact generation) have
  no fix commits anywhere** and must be re-opened in the tracker.
- PR #51: approve after two fixes (one new boundary defect in `AtomicFileWriter`,
  plus closure-table relabeling). PR #52: request changes (one hard-rule
  regression: `vector::resize` OOM termination on a `noexcept` streaming path).
- Fresh sweeps found 4 new P0-severity and ~12 new P1-severity defects on main,
  concentrated in the asset packer's write safety, Lua panic paths, and two
  concurrency races that survived the C-02 fix.

## 1. Build/test verification (local, Linux, gcc via ccache)

- `cmake --build build --parallel`: clean, zero warnings.
- `ctest -LE gpu`: 111/112 passed.
- FAIL: `engine_integration_ecs_stress_50k` — `FAIL: simulation 63.2ms exceeded
  24.00ms threshold` (`tests/integration/ecs_stress_50k.cpp:21`,
  `constexpr double kMaxSimMs = 24.0`). Wall-clock assertion in a functional
  test; flaky by construction on unoptimized/slow builds. See N-15.
- Comment audits: both pass (513 files, 0 quality findings).
- Mechanical hard-rule sweeps: no `.value()` misuse, no RTTI/exceptions/
  dynamic_cast, zero TODO/FIXME markers, `std::abort()` only on the documented
  Fatal log path.

## 2. Closure ledger — 2026-08-01 findings vs. main @ 893ade4

Verified-in-code statuses (full detail per finding in section 2.1):

| Status | Findings |
|---|---|
| FIXED-ON-MAIN (code-verified) | C-01, C-02, C-03, C-04, C-05*, H-03, H-04, H-05, H-10, H-22 |
| PARTIAL on main | H-01, H-02, H-06, H-09, H-11, H-12, H-17, L-04, L-08 |
| PENDING in open PR #51 | H-14, H-18, H-19 (partial), H-20 (partial), H-21 |
| PENDING in open PR #52 | H-02 remainder, H-09 remainder, H-11 remainder, H-17 remainder |
| **OPEN — no fix anywhere** | **H-07, H-08**, H-13, H-15, H-16, M-01..M-29 (all), L-01/02/03/05/06/07 |

\* C-05 core fix landed (`core::atomic_write_file` used by scene/prefab/save
writes), but only 2 call sites use it; the remaining truncating writers are the
subject of new findings N-01/N-02/N-05..N-07 below.

### 2.1 The H-07/H-08 misnumbering (action required)

PR #50's scope table rows read "H-07 entity pool recycling — Fixed" and
"H-08 serialization gaps — Partially fixed". In the audit report, H-07 is
*broad phase/CCD collision loss* and H-08 is *contact generation/impulse gaps*;
entity-pool recycling is H-02 (commit bfde506 itself says "Audit H-02") and
serialization gaps is H-01 (016f106). `git log` over `physics/src/ccd.cpp`,
`contact_clip.cpp`, `contact_resolution.cpp` confirms no post-audit fix beyond
the H-06 ingress validation (0409423). Anyone reading PR #50's table would
believe H-07/H-08 are closed. **Re-open both explicitly.** Their entire original
scope remains: int32 quantization, broad-phase cell-loop overflow, node-cap
silent drops, order-dependent speculative pairs, dense-index CCD snapshots
(H-07); coincident spheres, sphere-in-AABB normals, heightfield AABB
degradation, sweep false positives, angular effective mass, dropped callbacks
(H-08).

## 3. Open PR verdicts

### PR #51 (P1 tranche 2: H-21, H-20, H-14/18/19) — approve after two changes

Closure verdicts: H-21 closed; H-18 closed; H-14 partially closed (honestly
labeled); H-19 partially closed (honestly labeled); H-20 partially closed but
**mislabeled "Fixed"** — the platform-tag slice is deferred, the stamp-as-commit
marker still permits transiently mixed outputs, and a mesh that degenerates to
hull-less keeps serving a stale `.hull` under a fresh stamp.

New defect that must be fixed pre-merge:

- **`AtomicFileWriter::begin` can delete the destination on temp-path
  truncation** (`core/src/atomic_file.cpp`, PR head ~59-66). When the
  destination fits the 1023-char buffer but the `"%s.new.%lu.%u"` temp name
  truncates, `begin` fails without clearing `m_tempPath`; for a 1023-char
  destination the truncated temp path equals the destination, so the
  destructor's `std::filesystem::remove(m_tempPath)` deletes the authored file
  a *failed* begin promised to leave untouched. One-line fix + boundary test.
- Relabel the H-20/H-14/H-19 closure rows per the closure contract ("Partially
  fixed", no "every output" claims); "survivor assert" → "survivor error log".

Also record (inherited, non-blocking): no parent-directory fsync after rename;
scene saves via `serialization_util.cpp:36` remain on the buffer-based helper —
enumerate remaining truncating writers as an open migration item now that
`AtomicFileWriter` exists.

### PR #52 (PR #50 review blockers, items 4-7, 10) — request changes

Items 5, 6, 7, 10 verified correct with credible red-on-base production-path
regressions. Blocker:

- **Item 4 regresses a hard rule**: replacing the H-11 `new (std::nothrow)`
  buffers with `std::vector::resize` in `noexcept`
  `load_mesh_data_from_file` (`renderer/src/mesh_loader.cpp`) converts a
  handled OOM (asset marked Failed, game continues) into process termination
  under the no-exception build, on the asset-streaming worker path (meshes up
  to ~76 MB per the loader caps). Restore nothrow allocation semantics behind
  an owning type. Nits: delete `CpuMeshData` copy ops; pooled-entity slot
  erosion after deferred destroy is safer than base but undocumented.

## 4. New findings (fresh sweeps, main @ 893ade4)

### P0 — memory safety / self-perpetuating data corruption / process abort

- **N-01 Cooked outputs truncate in place and a surviving cook stamp makes the
  corruption permanent.** `tools/asset_packer/mesh_cook.cpp:214` opens the
  final `.mesh` with `"wb"` (no stage/rename/fsync; success-path `fclose`
  unchecked at `:261`); `anim_cook.cpp:18` same for `.skel`/`.anim`. Power loss
  can leave a truncated output while `.cookstamp` survives; `should_repack`
  (`cook_stamp.cpp:389`) then reports "up-to-date, skipped" forever.
- **N-02 Project-wide `asset_deps.json` truncated in place**
  (`tools/asset_packer/dependency_graph.cpp:24`). Interruption destroys every
  asset's dependency edges; next cook exits 15 until the file is hand-deleted,
  which silently disables texture-change invalidation.
- **N-03 Hostile glTF → OOB heap write; `cgltf_validate` is never called.**
  `mesh_cook.cpp:108`: `vertexCount * strideFloats` in `size_t` can wrap
  (count 2^60 × stride 16 ≡ 0), producing a zero-length vector that the write
  loop at `:117-167` then overruns. `cgltf_accessor_read_float` bounds checks
  live only in `cgltf_validate`, absent from the entire tools tree. Same
  exposure in `skeleton_import.cpp:180` and `animation_import.cpp`.
- **N-04 Unprotected `lua_getglobal`/`lua_getfield` in engine→Lua dispatch can
  abort the process.** `scripting/src/collision_bindings.cpp:94`,
  `animation_bindings.cpp:135`, `entity_script_bindings.cpp:163,337,386`,
  `scripting.cpp:520,548`: the lookup (which can run `__index` metamethods or
  allocate) executes outside any `lua_pcall`; a raised error reaches Lua's
  `panic()` → `abort()`. Also reachable without malice: `LUA_ERRMEM` from the
  sandbox allocator inside the unprotected `lua_newtable`
  (`touch_bindings.cpp:69,106`) or inside `luaL_traceback` in the error logger
  itself (`binding_util.cpp:49`). No `lua_atpanic` handler exists anywhere.

### P1 — correctness, races, data loss

- **N-05 User input bindings truncated in place** (`core/src/input_map.cpp:127,
  167-179`; `fclose` unchecked): kill mid-write → all rebindings lost at next
  boot. Violates the input-maps clause of the write-safety rule. (The
  file-local helper is also named `write_text_file` — likely why migration
  missed it.)
- **N-06 Sandboxed Lua can truncate an arbitrary OS path.**
  `scripting/src/input_bindings.cpp:294` passes a caller-supplied path to the
  truncating saver: `engine.save_input_config("scene.json")` destroys the
  authored scene. Concrete instance of H-15's missing trust boundary.
- **N-07 Editor `.meta.json` truncated in place on every changed frame**
  (`editor/src/editor_panels_assets.cpp:278`; note PR #51 replaces this path —
  verify it fully supersedes before closing).
- **N-08 CCD snapshot fallback reintroduces the C-02-class race.**
  `physics/src/ccd.cpp:233-242` (and `physics_step.cpp:104-125`) read live
  `RigidBody::velocity` of foreign bodies whenever the resolve snapshot is
  unusable — first step after play start/scene load, or any step after a
  collider add/remove — while parallel physics chunks write it
  (`physics_step.cpp:66-67`). Torn read → nondeterministic TOI; breaks the
  determinism and thread-count-independence invariants; TSAN-visible on a
  >256-transform scene with a fast mover (the Island Hopper falling rock hits
  the no-snapshot first step directly).
- **N-09 Unsynchronized LRU touch from parallel render-prep jobs.**
  `renderer/src/asset_database.cpp:395` plain-stores `lastAccessFrame` from
  every `render_prep_chunk_job` (`render_prep_pipeline.cpp:197-198,311-312`);
  two chunks sharing a mesh race. UB now, tearing on 32-bit web targets; feeds
  eviction. Fix: relaxed atomic or single-threaded touch at merge.
- **N-10 Bloom/luminance failure is recorded as success, then renders into
  framebuffer 0.** `renderer/src/command_buffer_post_resources.cpp:74-80,
  121-127` ignore create failures but set `bloomAllocatedWidth/Height`, so the
  retry gate never re-runs; `flush_post_chain` then binds FBO 0 and draws
  threshold/downsample passes into the back buffer at mip viewport size (or
  produces `GL_INVALID_FRAMEBUFFER_OPERATION` via an attachment-less FBO,
  since `gl_create_framebuffer(0,0)` skips its completeness check,
  `render_device_gl.cpp:1085`). The H-12 fix did not reach these chains.
- **N-11 Spring joint stiffness scales with solver iteration count.**
  `physics/src/joints/spring_joint.cpp:51-63` applies an integrated force with
  full-step dt every Gauss-Seidel pass (`constraint_solver.cpp:417-476`):
  8 iterations ≈ 8× authored stiffness; the solver-quality cvar silently
  retunes every spring; warm start compounds it. Existing test tolerance
  (`dist < 5.5`) cannot see it.
- **N-12 Spring joint loses damping/velocity response with a body-less
  endpoint** (`spring_joint.cpp:44-49,58-64` gate on both bodies non-null;
  `add_spring_joint` only requires transforms). Springing a dynamic body to an
  anchor entity never settles. Correct pattern exists in
  `joint_projection.cpp:133-147`.
- **N-13 Lua instruction cap is catchable and self-refilling.**
  The cap raises via `luaL_error` (`debug_bindings.cpp:188-192`) — `pcall`
  swallows it and `luaG_traceexec` refills the count before the hook; new
  coroutines start with a fresh budget. `while true do pcall(...) end` or a
  coroutine loop wedges `stage_scripting` (frame loop, editor UI, PIE stop)
  forever. Distinct from M-20 (memory cap).
- **N-14 Script dispatch iterates a swap-and-pop set under synchronous
  destroys and holds `scriptPath` into the moved slot across re-entrant Lua.**
  `world.h:1171-1173` + `sparse_set.h:80-103`; in `WorldPhase::Input`
  `engine.destroy_entity` mutates immediately, so
  `dispatch_entity_scripts_update` (`entity_script_bindings.cpp:579-594`) can
  skip the swapped-in entity's tick and cache a different script's module
  under the wrong key (path read at `:218-309` after `on_save_state` pcall).
- **N-15 `ecs_stress_50k` asserts wall-clock in a functional test**
  (`tests/integration/ecs_stress_50k.cpp:21`, 24 ms threshold). Violates the
  hard rule reserving thresholds for `engine_bench_*`; fails locally on
  unoptimized builds (observed 63 ms). Move the threshold to a bench test or
  convert the assertion to invariant checks.
- **N-16 Prefab codec has drifted from the scene codec; no authoritative
  registry.** Components are described in five independent places;
  `prefab_serializer.cpp:44-51` drops `Transform::parentId` (scene path
  persists it via reflection) and omits `RigidBody::sleeping`; the scene
  path's legacy `"meshId"` fallback is absent from prefabs. Nothing validates
  the codecs against each other (`world_component_counts_match` covers only
  the scene commit copy). This is the architecture-invariant registry work
  PR #50 queued — it is already producing silent data loss.
- **N-17 Quadratic JSON array access makes scene load O(n²).**
  `core/src/json.cpp:1010` re-parses elements 0..i per call;
  `deserialize_scene_entities` calls it per entity. Measured (-O2): 2k
  entities = 1.9 s, 8k = 31 s. Hits every `load_scene`, Lua `load_scene`, and
  the editor Stop-play restore.
- **N-18 Cook stamp committed before hull/thumbnail/dep-graph outputs, whose
  failures are discarded** (`tools/asset_packer/main.cpp:438-455`;
  `write_cook_stamp` returns true unconditionally, `cook_stamp.cpp:296-308`);
  `.skel`/`.anim` commit before the mesh with no cross-file transaction
  (`main.cpp:337-344`) — an interrupted cook leaves a new-generation skeleton
  beside an old-generation skinned mesh with mismatched joint indices and no
  way to detect it. `should_repack` only checks the `.mesh`, so deleted
  sidecars are never regenerated.

### P2 (summary — details in the sweep reports)

- Catch-up chunk-job budget is cumulative across steps: 4 catch-up steps at
  full entity capacity exhaust `kMaxChunkJobs` → engine exits mid-Simulation
  (`engine_pipeline.cpp:140,930-932,1119-1125`).
- Global cvar mutex acquired per body inside parallel physics chunks
  (`ccd.cpp:118-121`, `blocked_body_diagnostic.cpp:39`,
  `constraint_solver.cpp:357`) — hot-path lock, hard-rule violation.
- Job-system `completed` store can land after graph reset
  (`job_system.cpp:617-619`); reorder store before decrement.
- Streaming budget blind to non-streamed meshes: only one `set_mesh_asset_size`
  caller, so builtin/primitive meshes are size-0 and un-evictable
  (`asset_database.cpp:339`) — open half of M-28.
- `load_scene` peaks at ~3× `sizeof(World)` (~168 MB measured) including a
  redundant verify-copy world (`scene_serializer.cpp:1159-1241`).
- JSON `copy_string` silently truncates long paths and returns true
  (`json.cpp:1256-1367`); 16 MB `JsonWriter` ceiling caps savable scenes at
  ~28k entities, under ECS capacity (`json.h:26`).
- `atomic_write_file`: no parent-dir fsync; `fs::path` heap allocation inside
  `noexcept`; empty files rejected (`atomic_file.cpp:43,79-83`).
- Double `debug_draw_tick()` halves debug-draw lifetimes
  (`command_buffer_flush_forward.cpp:334-335`, split regression).
- `GpuMeshRegistry` slots carry no generation → stale `MeshHandle` aliasing;
  directional shadow cache can keep a shadow of replaced geometry
  (`mesh_loader.h:26-29`, `command_buffer_math.cpp:72-82`).
- Deferred point-shadow slots read stale when the light family empties
  (`command_buffer_flush_shadows.cpp:296-301` vs. forward path's check).
- Capture texture handles leak on uninitialized shutdown
  (`command_buffer.cpp:427`).
- Hinge twist limits near ±π act as a turnstile (atan2 wrap,
  `hinge_joint.cpp:47-48,78-117`); H-06 ingress accepts restitution > 1,
  dynamicFriction > staticFriction, unbounded inverseInertia
  (`world_components.cpp:54-71`); CCD applies restitution-1 reflection
  regardless of material (`physics_step.cpp:133-139`).
- Sandbox toggled on post-init never installs the capping allocator
  (`scripting.cpp:429-431,763-766`); several callbacks skip
  `refresh_lua_hook()`; `engine.persist(key)` with no value deletes the key;
  hot-reload failure retries (incl. `on_save_state` pcalls) every frame;
  DAP session wedges on full buffer without header (`dap_server.cpp:341-343`).
- `MeshComponent` has no ingress validation; NaN opacity from Lua reaches
  `build_draw_sort_key` where `static_cast<uint16_t>(NaN * 65535)` is UB
  (`render_prep_pipeline.cpp:74,88-95`).
- Input-map load accepts `{}` and wipes all bindings, returning true
  (`input_map.cpp:676-679`).
- `vfs_write_binary` truncates in place (test-only callers today,
  `vfs.cpp:369`); imgui.ini persisted by ImGui's own truncating write into the
  CWD (`editor.cpp:226-228`).
- `cvar_get_string` returns a shared static read after lock release
  (`cvar.cpp:205-214`) — latent trap.

### P3 — build/tooling/checklist gaps

- No `CMakePresets.json`; compile flags applied globally
  (`add_compile_options`) rather than per-target.
- GCC/Clang warning set lacks `-Wconversion -Wsign-conversion -Wshadow
  -Wnon-virtual-dtor -Wdouble-promotion`; MSVC lacks `/Zc:preprocessor
  /Zc:__cplusplus`.
- No `.clang-format` and no CI formatting gate; `.clang-tidy` enables only six
  checks (L-08 remainder); cppcheck still runs as C++20 excluding tests/tools.
- No fuzzing lane, no MSan lane, no packaging smoke test, no license/dependency
  inventory check (deps are SHA-pinned — good).
- No `docs/` directory; architecture/threading/ownership docs live only in
  CLAUDE.md.

## 5. Clean areas confirmed this pass

Scene commit/rollback staging; `mesh_loader.cpp` hostile-header validation
(exemplary); `.skel`/`.anim` loader bounds math; JSON parser core (depth cap,
overflow, surrogates); `save_data.cpp`; joint_projection math and creation
validation; contact_clip bounds; entity-handle liveness (all ~70 bindings
route through `read_entity`); generated-binding stack discipline; shader
hot-reload epoch coverage post-aff3ab5; GL create/link error paths; streaming
queue synchronization and shutdown order; light-culling packing vs. shader
layout; VFS read-side traversal defenses; audio thread boundaries; render-prep
double buffering.

## 6. Recommended order of work

1. Merge-blockers on the open PRs: N-fix in #51 (`m_tempPath` clear + boundary
   test, relabel closure rows), N-fix in #52 (restore nothrow mesh allocation).
2. Re-open H-07 and H-08 in the tracker (masked by the PR #50 numbering
   collision); they are the largest unaddressed correctness scope in physics.
3. P0s N-01..N-04: adopt `AtomicFileWriter` across the packer (lands with #51),
   call `cgltf_validate` + checked arithmetic in all importers, wrap engine→Lua
   entry lookups in protected calls and install `lua_atpanic`.
4. Races N-08/N-09 (both small, both determinism-relevant before the web/RHI
   port widens the platform matrix).
5. N-16 (single persistent-component registry) before the next component type
   is added; N-17 (JSON array cursor) before any template ships scenes larger
   than the demo.
6. The M-01..M-29 backlog remains open and untouched; schedule as P2 tranches.
