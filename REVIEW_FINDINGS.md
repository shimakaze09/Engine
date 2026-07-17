# Code Review Findings — Fix Tracker

Full-codebase review performed 2026-07-17 (all 8 modules read/sampled; ~54k LOC).
This file is the working tracker for the production-hardening campaign. Update the
status column as fixes land. Keep entries until verified fixed (build + tests + review).

Status codes: `[ ]` open · `[~]` in progress · `[x]` fixed+verified · `[-]` rejected/won't fix

## P0 — Bugs (correctness / runtime failure)

- `[x]` **B1: Job handle generation truncation → engine self-terminates after ~73 min @60FPS.**
  *Fixed 2026-07-17: generation counter now wraps within the 19 encodable bits
  (`kGenerationMask`) + static_assert on index width. Regression test
  `engine_unit_job_handle_generation` cycles 2^19+64 graphs and verifies handles
  still validate/dispatch; confirmed the test fails against the pre-fix code.*
  `core/src/job_system.cpp` — `encode_handle_id()` keeps only 19 bits of generation
  (`generation << 13`), but `is_valid_handle_locked()` compares full 32-bit
  `node.generation` against the truncated decode. `m_generation` increments ~2×/frame
  (reset in both `begin_graph` and `end_graph`). At generation ≥ 2^19 every new handle
  fails validation → `stage_frame_graph` sets `graphFailed` → `running = false` →
  engine exits with "job graph assembly failed".
  Fix: mask stored/compared generation to 19 bits consistently. Add soak-style unit
  test that forces `m_generation` past 2^19.

- `[x]` **B2: Audio handle generation truncation (same class as B1).**
  *Fixed 2026-07-17: `next_sound_generation` wraps within 23 encodable bits +
  static_assert on slot width. No soak test (would need 8.4M reload cycles);
  same pattern as the tested B1 fix.*
  `audio/src/audio.cpp:79` — `(generation << 9) | slotToken`, 23-bit decode vs 32-bit
  stored. Needs ~8.4M reload cycles to trigger; fix alongside B1 for consistency.

- `[x]` **B3: `World::add_transform` leaves half-added state on failure.**
  *Fixed 2026-07-17: fresh insert rolls back the local transform when the world
  transform insert fails (overwrites are untouched). Verified by full suite.*
  `runtime/src/world.cpp` — if `m_transforms.add` succeeds and `m_worldTransforms.add`
  fails, returns false but local transform stays. Roll back on failure.

- `[x]` **B4: `SparseSet` ignores entity generation — stale-handle aliasing footgun.**
  *Fixed 2026-07-17: lookups now reject generation-mismatched handles when the
  entity type carries a generation (`HasGenerationMember` concept); `add` under
  a newer generation adopts the slot. New `engine_unit_sparse_set` covers stale
  get/remove rejection, slot adoption, range guards, capacity. Junk comments in
  sparse_set.h cleaned as part of the rewrite.*
  `core/include/engine/core/sparse_set.h` — keyed on `entity.index` only. Safe today
  because `World` pre-validates, but it is a public core API. Either validate
  generation internally or document the contract prominently + add debug assert.

- `[x]` **B5: Twin APIs with opposite count semantics.**
  *Fixed 2026-07-17: `update_transforms_range` now rejects out-of-bounds ranges
  like its three sibling range APIs, and delegates validation to
  `get_transform_update_range` (removed the duplicated guard block). All
  callers verified to pass exact counts — no behavioral impact.*

- `[x]` **B6: Copy-paste drift in World error messages.**
  *Fixed 2026-07-17 via S1: all component families now share
  `log_component_error` + the checked helpers, so messages are uniform
  ("<label> requires Input phase" / "requires a live entity" / "on stale or
  dead entity"). Note: three light/probe removes previously failed silently on
  dead entities and two gets were silent on stale handles — they now log like
  every other family (intentional unification).*

## P1 — Performance

- `[x]` **P1: Math library fully out-of-line + no LTO.**
  *Fixed 2026-07-17: entire math module moved inline into headers (`constexpr`
  where possible, SSE2 paths preserved via new `math_detail.h`); `math/src/*.cpp`
  deleted; `engine_math` converted to header-only INTERFACE lib via new
  `engine_add_header_library` helper; cppcheck objectIndex suppression retargeted
  to ray.h. Junk comments in all math headers rewritten as real docs.
  Measured (Release, 5 runs): physics step 8.61 ms → 6.34 ms (-26%) at 1000
  bodies; ECS iterate unchanged (no math calls). FP determinism preserved by
  global `/fp:strict` (`-ffp-contract=off`) flags — determinism +
  thread-count-determinism tests pass. 83/83 headless tests green.
  IPO/LTO deliberately not enabled (unnecessary now; avoids CI link-time cost).*

- `[x]` **P2: `rebuild_name_lookup()` on every name add/remove/named-entity destroy → O(n²).**
  *Fixed 2026-07-17: incremental `name_lookup_insert`/`name_lookup_erase` with
  tombstones; rebuild now only fires when tombstones exceed capacity/4 (keeps
  probe chains short after churn). Semantics change from the old dedupe:
  entities sharing a name each keep their own slot, so destroying one twin no
  longer orphans the other (was a latent rebuild-order dependency). New
  `engine_unit_world_name_lookup` covers add/rename/remove/destroy/duplicates
  plus 65k-cycle churn across the rebuild threshold. 84/84 headless green.*

- `[x]` **P3: Job workers poll on 1 ms `wait_for` timeouts; `notify` races with waits.**
  *Fixed 2026-07-17: workers now block on `m_workAvailable` with a predicate
  under `m_queueMutex` (the mutex every push holds), so wakeups cannot be lost
  and idle workers burn zero CPU; `m_sleepMutex` removed; shutdown takes an
  empty `m_queueMutex` section before notify to close the store/wait race.
  Main-thread wait paths deliberately keep the 1 ms timed poll: they must also
  wake to steal newly readied jobs (signalled on the other CV) and are the
  only executor when workerCount==0 — documented inline. Verified: 84/84
  headless green; scheduler stress + thread-count determinism 5×5 runs pass.*

- `[x]` **P4: BeginPlay phase entered + scanned every playing frame.**
  *Fixed 2026-07-17: World tracks `begin_play_pending_count` (maintained by
  create/destroy/mark_begin_play_done); `for_each_needs_begin_play` early-outs
  and the pipeline skips the whole BeginPlay phase when the count is zero.
  Counter transitions unit-tested in `engine_unit_runtime_world` (double-mark,
  destroy-before-fire, index recycling). 84/84 headless green; lifecycle and
  Lua-lifecycle integration tests confirm dispatch semantics unchanged.*

- `[x]` **P5: Asset database slots never reclaimed (fills monotonically).**
  *Fixed 2026-07-17: mesh slots gained tombstones (`meshTombstoned[]`) and a
  public `unregister_mesh_asset` (refused while refCount > 0 or a GPU mesh is
  live), so probe chains survive deletion and slots are reusable. Probe logic
  centralized in `find/claim_mesh_asset_record_slot`; asset_manager.cpp's
  duplicated slot finders deleted in the process. Forced-collision chain
  reclamation covered in the asset database test. Texture/metadata tables can
  adopt the same pattern when reclamation is needed there.*

## P2 — Duplication (structure)

- `[x]` **S1: World per-component boilerplate (~1,200 duplicated lines).**
  *Fixed 2026-07-17: private template helpers `add_component_checked` /
  `remove_component_checked` / `get_component_checked` /
  `get_component_ptr_checked` + `check_component_mutation` for mutators with
  extra logic. 52 wrapper bodies collapsed to one-line dispatches; world.cpp
  2,045 → 1,730 lines and — more importantly — the guard/log logic exists
  exactly once. Public API unchanged. Fixes B6. 84/84 headless green.
  Remaining (tracked by S6/S7): adding a component type still touches the
  serializers and copy_world_contents lists.*

- `[x]` **S2: Five bounded-string-copy implementations.**
  *Fixed 2026-07-17: new `core/include/engine/core/string_util.h` with one
  `core::copy_string` (null-safe, truncating, always terminated); all five
  sites migrated — world.cpp and scripting.cpp use it directly,
  shader_system/texture_loader keep thin path wrappers over it,
  asset_manager keeps its zero-fill-then-copy for byte-comparable records.*

- `[x]` **S3: Duplicated FNV-1a implementations (found five, not two).**
  *Fixed 2026-07-17: new `core/include/engine/core/hash.h` (32/64-bit
  constants, byte-append, string hash, and the word-wise `append_u64` cache-key
  variant). Migrated: event_bus (32), world name lookup (32 — `hash_name_string`
  member deleted), asset_database (64, path canonicalization + content hash),
  shader_system (64, variant keys), command_buffer_math (64, shadow cache
  keys). Exact hash algorithms preserved — asset-hash and deterministic-cook
  tests confirm ids unchanged. 84/84 headless green.*

- `[x]` **S4: Three hand-rolled open-addressing hash tables with divergent semantics.**
  *Fixed 2026-07-17 (scoped): new `core::FixedHashTable<Key,Value,Capacity>`
  (tombstone deletion, owner-driven rebuild signal) with its own unit test;
  World's persistent-id index migrated onto it (~110 lines -> 3 wrappers, plus
  a tombstone-threshold rebuild the old code lacked). Deliberately NOT
  migrated: the name lookup (multiset semantics — entities sharing a name each
  own an entry) and the asset database (records-in-slots is a public struct
  layout iterated by other modules); both documented at their definitions.
  Forcing them into the map template would cost more than the duplication.*

- `[x]` **S5: Scene vs prefab serializer helper duplication.**
  *Fixed 2026-07-17: new module-internal `runtime/src/serialization_util.{h,cpp}`
  (src-header pattern) owns file IO, vec2/3/4 + quat array read/write, and the
  foliage-patch read/write pair; both serializers migrated. Semantics unified
  on the STRICT variants: reads reject present-but-malformed fields (scene
  foliage reads were lenient) and float arrays must have exact element counts
  (prefab reads tolerated oversized arrays) — intentional, matching the
  error-handling policy; both writers always emitted exact shapes so
  well-formed files are unaffected. New scene test pins malformed-foliage
  rejection. 85/85 headless green; comment ratchet 1706 → 1702.*

- `[x]` **S6: `copy_world_contents` is another hand-maintained 13-component list.**
  *Fixed 2026-07-17: `copy_component<T>` template takes the World get/add
  member-pointer pair, so the guard/copy body exists once and each component
  type is a single declarative line in the fold (12 blocks × 7 lines → 12
  lines). Pure refactor — same pairs, same order, same failure semantics;
  exercised by every load_scene round-trip test. 85/85 headless green.*

- `[x]` **S7: Split-brained serialization: 4 components via reflection, rest hand-written JSON.**
  *Fixed 2026-07-17: PointLight and SpotLight migrated onto the reflection
  path (their reflect_types.cpp descriptors already matched the hand-written
  JSON field names and order, so the format is byte-identical); the two hand
  readers deleted. Descriptor plumbing deduplicated into
  `SceneComponentDescriptors` + `find_scene_descriptors` (was two 5-lookup
  blocks + a 7-ref parameter list). Remaining hand-written types are now
  documented in scene_serializer.cpp with concrete reasons: MeshComponent
  (64-bit id — no Uint64 field kind — plus legacy "meshId" fallback),
  LightComponent (enum clamp on load), Foliage/Name/Script (array/string
  fields, zero-field descriptors documented in reflect_types.cpp). New exact
  round-trip test for point/spot through the reflected path; the existing
  malformed-reject tests pin strictness unchanged. 85/85 headless green;
  determinism + thread-count determinism pass.*

- `[x]` **S8: `build_pyramid_mesh` local `cross3`/`norm3` lambdas duplicate `math::`.**
  *Fixed 2026-07-17 with A2: pyramid face normals now use
  `math::cross`/`math::normalize` over `math::Vec3`.*

- `[x]` **S9: `texture_loader.cpp` defines a local `Vec3`/`normalize` duplicating
  `engine_math`.**
  *Fixed 2026-07-17: local Vec3 + normalize deleted, `math::Vec3`/
  `math::normalize` used instead (`engine_unit_texture_loader_handles` test
  target gained the missing `engine_math` dep). Local scalar `clamp_int`/
  `clamp_float`/`wrap_index` kept — math has no scalar equivalents. 84/84.*

## P3 — Architecture / file organization

- `[x]` **A1: `renderer/src/command_buffer.cpp` is a 4,163-line god file.**
  *Fixed 2026-07-17: split along the suggested seams using the private
  src-header pattern — `command_buffer_sky.{h,cpp}` (skybox + Preetham/Hosek
  + shared cube geometry, 341), `command_buffer_ibl.{h,cpp}` (prefilter/
  irradiance/BRDF LUT + public bake entry points, 446),
  `command_buffer_post_resources.{h,cpp}` (bloom/luminance mip chains +
  SSAO data, 178), `command_buffer_flush.cpp` (flush_renderer + its
  upload/bind helpers, 2,063), leaving command_buffer.cpp at 1,303
  (initialize_backend, destroy, public state API). `initialize_backend`
  exported via command_buffer_context.h. Pure code motion — the split
  script verified every original non-blank line landed exactly once.
  85/85 headless green; GPU paths are covered by the zero-diff motion +
  `engine_unit_command_buffer` shutdown-state test (headless CI cannot
  execute GL). Comment ratchet unchanged at 1702 — an interim 1666 reading
  was an artifact: the checker scans git-tracked files only, and the moved
  filler comments sat in the not-yet-added new TUs. Residual: `flush_renderer`
  itself is still one ~1,570-line function — function-level staging of the
  flush is opportunistic follow-up under A3's spirit, not tracked here.*

- `[x]` **A2: Procedural mesh builders live in `runtime/src/engine_pipeline.cpp` (~350 lines).**
  *Fixed 2026-07-17: moved to `renderer/mesh_primitives.{h,cpp}` with a public
  header; behavior-preserving. The plane's y=+0.5 offset is kept and now
  documented (aligns with the unit cube's top face) — change it deliberately
  if a ground-level plane is wanted.*

- `[ ]` **A3: `editor/src/editor.cpp` (2,682 lines) and `scripting/src/scripting.cpp`
  (3,456 lines) monoliths.** Lower priority than A1; split opportunistically when
  touching those areas.

- `[x]` **A4: `resolve_collisions` in `physics/src/physics.cpp` is ~1,090 lines.**
  *Fixed 2026-07-17: eight per-shape-pair functions (heightfield, convex/GJK,
  capsule×{capsule,sphere,AABB}, sphere×sphere, AABB×{sphere,AABB}) take a
  `PairContext` computed once per broadphase candidate; the shared
  record/wake/static-skip prologue (8 copies, 3 of them inlined clones of
  `maybe_wake_pair`) is now one `record_pair_and_wake`. resolve_collisions
  is ~230 lines of broadphase + dispatch. Math moved verbatim — 85/85
  headless green, determinism + thread-count determinism pass 3×, and A/B
  Release benchmark shows no cost (median 6.351 → 6.352 ms @1000 bodies;
  baseline gate 13.80).*

- `[x]` **A5: Dead/vestigial code.**
  *Fixed 2026-07-17: WorldPhase aliases removed (5 call sites migrated to
  canonical names); World() ctor reduced to reflection registration (all
  members carry correct default initializers); the name-lookup tombstone
  branch became live code with P2.*

- `[x]` **A6: PCH only in core+renderer; scripting/runtime/physics equally heavy.**
  *Fixed 2026-07-17: src/pch.h added to all three via the existing PCH helper
  arg. scripting precompiles the Lua C API (12/18 TUs; still banned from
  public headers), physics precompiles <cmath>+Vec3 (no runtime headers —
  PhysicsWorldView boundary), runtime precompiles logging+world.h (9/15 TUs
  already included it). Measured: clean rebuild of the three modules
  16.3 s → 13.9 s (-15%). 85/85 headless green.*

- `[x]` **A7: Script hot-reload watches exactly one file (`g_watchedPath`).**
  *Fixed 2026-07-17: 16-entry watch table with path dedup and logged overflow;
  check_script_reload polls every entry. Registering a new watch no longer
  drops the previous one.*

## P4 — Comment noise

- `[ ]` **C1: ~1,300 machine-generated filler comments; some factually wrong.**
  Patterns: `/// Handles <name>.` tautologies; wrong docs (`normalize()` documented
  as "Clamps and fills settings into a safe runtime range"); doc comments inside
  function bodies, on `private:`, on ctor init-lists (`/// Handles x.`).
  Fix: `tools/check_comment_quality.py` (added by this campaign) detects the
  patterns; clean up mechanically, then wire the checker into CI so they can't
  come back. `tools/check_source_comments.py` enforces presence only — the two
  checks are complementary.
  *Ratchet: 1878 findings at campaign start → 1856 after sparse_set.h cleanup
  → 1720 after math header rewrite → 1713 after S2/S3 → 1711 after S1/S9
  → 1706 after A2/S4/P5/A5/A7 (2026-07-17). Total must only go down.*

## Verification protocol (per fix)

1. `cmake --build build --parallel` — zero warnings introduced.
2. `ctest --test-dir build --output-on-failure` (headless-safe subset minimum:
   `-R "engine_unit_|engine_integration_"` excluding `gpu` label).
3. Determinism-sensitive changes (ECS/world/physics/render-prep/serialization):
   run `engine_integration_determinism` + `engine_integration_thread_count_determinism`.
4. Perf-sensitive changes: run relevant `engine_bench_*` and compare against
   `tests/benchmark/perf_baseline.json`.
5. New behavior requires a new or extended test (per AGENTS.md).
6. Update this file's status marker in the same commit as the fix.
