# C++ Project Review Report

## 1. Review Scope

Reviewed the complete first-party Engine repository under `app/`, `core/`, `math/`, `physics/`, `renderer/`, `audio/`, `scripting/`, `runtime/`, `editor/`, `tools/`, `assets/`, and `tests/`. The review covered C++20 sources and headers, CMake and helper scripts, GLSL, all shipped Lua modules and demo behavior, existing uncommitted work, and new regression coverage.

The audit combined manual data-flow and lifetime review, high-risk pattern searches, full compilation, the repository clang-tidy analysis target, exact targeted regressions, the complete headless and GPU test suites, determinism tests, performance baselines, CMake validation, and both source-comment audits. SHA-pinned third-party implementation code and generated `build/` output were excluded from manual source review.

## 2. Executive Summary

The review confirmed and repaired 22 correctness, lifetime, determinism, validation, renderer, tooling, editor, and demo-script defects. The most serious issues were stale generation-blind handles, leaked Lua registry references, non-transactional hot reload, a deferred shader that exceeded NVIDIA uniform limits, stale joint IDs mutating replacement joints, and silent asset-graph corruption.

No known Critical, High, Medium, or Low defect from this review remains open. Full verification passes. One hardware-validation gap remains informational: the deferred path passes the available GPU smoke test, but the original NVIDIA C6020 failure should still be confirmed on an NVIDIA CI runner.

## 3. Critical Findings

No Critical finding was identified.

## 4. High Findings

### ENG-H01 — Entity script instances could target recycled entities

- **ID:** ENG-H01
- **Severity:** High
- **Category:** Lifetime / stale handles
- **Location:** `scripting/src/entity_script_bindings.cpp`, `runtime/src/world.cpp`
- **Problem:** Script-module state tracked only entity indices in several lifecycle paths, allowing a destroyed entity's state to collide with a later entity reusing the slot. Registry references and pending-start state were also not released consistently.
- **Why it matters:** A stale script could run against or mutate an unrelated replacement entity, while repeated reloads leaked Lua registry objects.
- **Recommended fix:** Track the full entity handle, own every registry reference explicitly, release references on every exit path, and remove script components during entity destruction.
- **Example/fix:** Entity module lookup, begin/end dispatch, fault state, reload state, and pending starts now require exact generation matches; lifecycle regressions recycle slots and verify no stale callback fires.

### ENG-H02 — Lua timers reused stale identifiers and registry references

- **ID:** ENG-H02
- **Severity:** High
- **Category:** Lifetime / resource ownership
- **Location:** `scripting/src/timer_bindings.cpp`
- **Problem:** Timer callbacks were identified without a generation-bearing external handle and replacement/self-cancel paths could retain or release the wrong Lua reference.
- **Why it matters:** A stale timer ID could cancel a new timer occupying the same slot, or invoke the wrong callback after reuse.
- **Recommended fix:** Encode and validate timer generations and define one ownership transfer/release point for callback references.
- **Example/fix:** Timer creation, cancellation, replacement, dispatch, and shutdown now preserve exact-generation ownership; regressions cover self-cancel, stale cancellation, replacement, and shutdown.

### ENG-H03 — Global Lua hot reload was non-transactional

- **ID:** ENG-H03
- **Severity:** High
- **Category:** State integrity / hot reload
- **Location:** `scripting/src/scripting.cpp`
- **Problem:** A runtime error during global-script reload could leave overwritten, deleted, or newly introduced globals in a partially applied state.
- **Why it matters:** A failed developer reload silently corrupted the running game state and made recovery dependent on the failed chunk's execution order.
- **Recommended fix:** Compile before mutation, snapshot top-level globals, and rollback all additions, deletions, and replacements on runtime failure.
- **Example/fix:** Reload now performs a shallow global snapshot and exact rollback, preserves the prior script on compile failure, and clears watched scripts at shutdown; integration tests cover failure and recovery.

### ENG-H04 — Physics joint IDs could mutate replacement joints

- **ID:** ENG-H04
- **Severity:** High
- **Category:** Stale handles / input validation
- **Location:** `physics/src/joint_handle.h`, `physics/src/physics.cpp`, `physics/src/constraint_solver.cpp`
- **Problem:** Public joint IDs were raw slot indices. Removed or endpoint-stale joints could be reused, after which an old ID could remove or change the replacement. Typed creation accepted invalid endpoints and non-finite parameters.
- **Why it matters:** Gameplay code holding a stale ID could corrupt an unrelated constraint and feed NaN/invalid axes into the deterministic solver.
- **Recommended fix:** Use bounded generation-bearing opaque IDs, retire stale slots by advancing generation, fully reset reused slots, and validate every typed parameter before allocation.
- **Example/fix:** All creation/removal/limit paths now resolve exact generations; axes are normalized, invalid finite/range cases fail without allocation, and tests prove stale IDs cannot alter replacements.

### ENG-H05 — Deferred lighting exceeded uniform limits and misread spot tiles

- **ID:** ENG-H05
- **Severity:** High
- **Category:** Renderer correctness / portability
- **Location:** `assets/shaders/deferred_lighting.frag`, `renderer/src/light_culling.cpp`, `renderer/src/command_buffer*.cpp`
- **Problem:** Large per-light uniform arrays exceeded NVIDIA fragment uniform registers, disabling deferred rendering. The shader also used a spot-light tile offset inconsistent with the CPU packing layout.
- **Why it matters:** Deferred rendering silently fell back on affected hardware, and deferred spot lights sampled outside the packed tile row.
- **Recommended fix:** Move per-light payloads to a compact indexed GPU resource and share exact packing constants between CPU tests and shader layout.
- **Example/fix:** Light data now uses an R32F texture uploaded beside tile data; exact CPU packing tests cover point/spot offsets, and the GPU smoke suite passes on the available GL context.

### ENG-H06 — Texture upload formats and GL unpack state were incorrect

- **ID:** ENG-H06
- **Severity:** High
- **Category:** Renderer data corruption / state leakage
- **Location:** `renderer/src/render_device_gl.cpp`, `renderer/src/gl_texture_upload_layout.h`
- **Problem:** One- and two-channel LDR/HDR images could select incorrect external/internal formats, tightly packed odd-width rows assumed the default alignment, and uploads did not consistently restore the caller's unpack state.
- **Why it matters:** Texture rows could be misread, RG textures could be interpreted incorrectly, and one upload could contaminate later GL operations.
- **Recommended fix:** Derive formats from channel count and data width, scope `GL_UNPACK_ALIGNMENT`, validate dimensions, and restore prior state after every upload.
- **Example/fix:** A private upload-layout helper now covers 1–4 channel LDR/HDR formats and alignment 1/2/4/8; exact unit tests verify format selection and state restoration.

### ENG-H07 — DAP unknown-command decoding could read beyond the payload

- **ID:** ENG-H07
- **Severity:** High
- **Category:** Bounds safety / protocol handling
- **Location:** `scripting/src/dap_server.cpp`, `tests/integration/dap_test.cpp`
- **Problem:** Unknown command names were consumed from bounded JSON text as though they were NUL-terminated C strings.
- **Why it matters:** A crafted or truncated debugger request could overread adjacent receive-buffer data and echo an incorrect command.
- **Recommended fix:** Copy decoded strings into a bounded local buffer and terminate them explicitly before formatting a response.
- **Example/fix:** Unknown commands now use a bounded, NUL-terminated decode; an integration test asserts the exact echoed command.

## 5. Medium Findings

### ENG-M01 — Physics queries accepted degenerate and non-finite inputs

- **ID:** ENG-M01
- **Severity:** Medium
- **Category:** Numerical correctness / determinism
- **Location:** `physics/src/physics_query.cpp`, `physics/include/engine/physics/physics_query.h`
- **Problem:** Ray/sweep directions were not consistently normalized, zero directions could reach division, and non-finite ranges/radii were not rejected. `raycast_all` kept the first capacity-sized set instead of the nearest set.
- **Why it matters:** Results depended on direction magnitude, could produce NaN, and overflow behavior depended on world iteration order.
- **Recommended fix:** Validate finite positive ranges, normalize nonzero directions, retain the nearest bounded results, and sort deterministically.
- **Example/fix:** Query entry points now enforce those rules; unit tests cover degenerate/non-finite input, normalization, capacity overflow, and exact ordering.

### ENG-M02 — Camera shake noise contained signed overflow

- **ID:** ENG-M02
- **Severity:** Medium
- **Category:** Undefined behavior / determinism
- **Location:** `runtime/src/camera_manager.cpp`, `tests/integration/camera_test.cpp`
- **Problem:** Integer noise mixing used signed overflow and could convert out-of-range floating-point values to integers for extreme time inputs.
- **Why it matters:** The operation was undefined in C++ and could diverge across compilers or optimization levels.
- **Recommended fix:** Perform mixing in defined unsigned arithmetic and bound time conversion before integer use.
- **Example/fix:** Shake noise now uses deterministic unsigned mixing and safe extreme-time reduction; integration coverage repeats extreme inputs exactly.

### ENG-M03 — Mesh files could reference vertices outside the loaded buffer

- **ID:** ENG-M03
- **Severity:** Medium
- **Category:** Asset validation / memory safety
- **Location:** `renderer/src/mesh_loader.cpp`, `tests/unit/mesh_loader_test.cpp`
- **Problem:** Several mesh loading paths accepted indices greater than or equal to the vertex count.
- **Why it matters:** Later GPU or CPU consumers could fetch outside the uploaded vertex data.
- **Recommended fix:** Validate every index after size/stride validation and before registration or upload.
- **Example/fix:** All file and memory paths reject out-of-range indices; regression fixtures cover the invalid boundary.

### ENG-M04 — Material registration could leave partial database state

- **ID:** ENG-M04
- **Severity:** Medium
- **Category:** Transactionality / capacity handling
- **Location:** `renderer/src/material_loader.cpp`, `tests/unit/material_asset_test.cpp`
- **Problem:** Material and metadata tables were mutated sequentially without preflighting both capacities or checking every dependency insertion.
- **Why it matters:** A full companion table could leave a material record without matching metadata, or vice versa.
- **Recommended fix:** Preflight all required slots and make every insertion result explicit before publishing either record.
- **Example/fix:** Both tables are checked before mutation, dependency insertion is validated, and full-capacity tests assert no partial companion record.

### ENG-M05 — Texture handle generation could escape its encoded field

- **ID:** ENG-M05
- **Severity:** Medium
- **Category:** Handle encoding / stale resources
- **Location:** `renderer/src/texture_handle_codec.h`, `renderer/src/texture_loader.cpp`
- **Problem:** Generation increments used the full integer range even though handles reserve low bits for the slot.
- **Why it matters:** After wrap, generation bits could overlap or truncate, allowing stale handles to alias live textures.
- **Recommended fix:** Define the slot/generation codec once and wrap inside the encoded generation mask while reserving zero.
- **Example/fix:** Loader encode/decode/reset paths share the bounded codec; tests cover the exact wrap boundary.

### ENG-M06 — Editor history stored generation-blind entity targets

- **ID:** ENG-M06
- **Severity:** Medium
- **Category:** Editor correctness / stale handles
- **Location:** `editor/src/editor_commands.cpp`, `editor/src/editor_commands.h`
- **Problem:** Undo/redo commands restored edits by entity index without requiring the original generation.
- **Why it matters:** Undo after entity destruction and slot reuse could mutate an unrelated entity.
- **Recommended fix:** Persist and validate the complete entity handle in every command.
- **Example/fix:** Commands now target exact handles; unit tests recycle an entity and prove undo/redo ignores the replacement.

### ENG-M07 — Touch callbacks captured coroutine states

- **ID:** ENG-M07
- **Severity:** Medium
- **Category:** Lua state lifetime
- **Location:** `scripting/src/touch_bindings.cpp`, `tests/unit/scripting_test.cpp`
- **Problem:** Callback registration retained whichever `lua_State*` invoked it, including a coroutine stack.
- **Why it matters:** The coroutine could finish while input dispatch later used its stale state.
- **Recommended fix:** Resolve and store the main Lua state for registry ownership and callback dispatch.
- **Example/fix:** Touch and gesture registration now canonicalize to the main state; a coroutine-registration regression dispatches after coroutine completion.

### ENG-M08 — Editor GL shutdown ran without acquiring the render context

- **ID:** ENG-M08
- **Severity:** Medium
- **Category:** Resource teardown / platform integration
- **Location:** `runtime/src/engine.cpp`, `runtime/include/engine/runtime/editor_bridge.h`, `tests/smoke/CMakeLists.txt`
- **Problem:** Editor teardown could destroy ImGui GL resources after the context was no longer current.
- **Why it matters:** Shutdown behavior was driver-dependent and could leak or crash.
- **Recommended fix:** Make context ownership explicit around editor shutdown and ensure the smoke executable links editor bridge registration.
- **Example/fix:** Runtime acquires/releases the render context around editor shutdown; the whole-archived GPU smoke path exercises the bridge.

### ENG-M09 — Dependency graph corruption and invalid edges were silent

- **ID:** ENG-M09
- **Severity:** Medium
- **Category:** Tooling data integrity
- **Location:** `tools/asset_packer/dependency_graph.cpp`, `tools/asset_packer/main.cpp`
- **Problem:** The graph lacked a required schema version, load accepted invalid/self/cyclic edges through direct insertions, a corrupt existing graph was treated as first use, and graph write failure still returned success.
- **Why it matters:** Incremental cooking could silently lose dependency information or accept a cycle.
- **Recommended fix:** Version the format, parse transactionally, route every edge through validation, distinguish missing from malformed files, and fail the command when persistence fails.
- **Example/fix:** Schema version 1 and transactional parsing are enforced; CLI and unit regressions reject corrupt, self, invalid, and cyclic graphs.

### ENG-M10 — Thumbnail cooking could sample outside the image and report corrupt input as success

- **ID:** ENG-M10
- **Severity:** Medium
- **Category:** Tooling bounds safety / error reporting
- **Location:** `tools/asset_packer/thumbnail_resample.cpp`, `tools/asset_packer/main.cpp`
- **Problem:** Upscale sampling derived negative interpolation weights after clamping only indices, corrupt PNG/JPEG input returned success, metadata short reads were accepted, and intermediate index multiplication used signed arithmetic.
- **Why it matters:** Edge pixels were wrong, damaged source assets passed automation, and extreme dimensions could trigger undefined signed overflow.
- **Recommended fix:** Clamp sample coordinates before weights, propagate decode failure, require complete reads, and calculate byte/index offsets in `size_t`.
- **Example/fix:** A deterministic resampler and corrupt-input CLI regressions now cover boundaries and failure; all thumbnail buffer arithmetic is size-safe.

### ENG-M11 — Demo Lua setup could become permanently partial

- **ID:** ENG-M11
- **Severity:** Medium
- **Category:** Shipped script correctness
- **Location:** `assets/main.lua`, `assets/scripts/player.lua`, `assets/lib/utils.lua`
- **Problem:** Demo hooks used inconsistent lifecycle conventions, setup marked itself complete before all entities were configured, failures leaked partial entities, diagonal movement was faster, jump grounding could hit self, and clamp mishandled reversed bounds.
- **Why it matters:** The shipped first-run experience could duplicate, stall, or behave inconsistently after hot reload and capacity failures.
- **Recommended fix:** Use canonical module hooks, persist reload state, make setup transactional, normalize movement, filter ray hits, and define reversed-bound behavior.
- **Example/fix:** Every shipped Lua module loads in the lifecycle harness; exact tests cover idempotence, rollback/retry, diagonal speed, grounding, jump, reload state, and utility outputs.

## 6. Low Findings

### ENG-L01 — Zero entity capacity passed configure validation

- **ID:** ENG-L01
- **Severity:** Low
- **Category:** Build configuration validation
- **Location:** `CMakeLists.txt`
- **Problem:** `ENGINE_MAX_ENTITIES=0` matched the digits-only validation even though the world requires a positive capacity.
- **Why it matters:** The generated configuration violated core entity-index assumptions and failed later with less useful diagnostics.
- **Recommended fix:** Reject numeric values below one during configure.
- **Example/fix:** CMake now emits a fatal positive-integer error for zero; an isolated clang-cl configure check confirms rejection.

### ENG-L02 — Material tests claimed exactness but used a tolerance

- **ID:** ENG-L02
- **Severity:** Low
- **Category:** Test quality
- **Location:** `tests/unit/material_asset_test.cpp`
- **Problem:** The helper comment said values were exactly round-trippable while assertions allowed a `0.0001` difference.
- **Why it matters:** A parser or inheritance regression could change material data without failing the test.
- **Recommended fix:** Compare the exact parsed/stored float values produced from the same literals.
- **Example/fix:** The helper and all call sites now use exact equality; the full-capacity material regressions remain green.

## 7. Informational Findings

- The complete available GPU suite passes, including editor bootstrap/shutdown and renderer smoke. The historical NVIDIA C6020 reproduction still needs a dedicated NVIDIA runner for hardware-specific confirmation.
- Third-party warnings reported by clang-tidy are suppressed as non-user code; no first-party diagnostic was emitted.
- The repository now contains both `AGENTS.md` and `CLAUDE.md` with synchronized project status, plus the local `verify-engine` skill used for this campaign.

## 8. Build/Tooling Recommendations

1. Move fundamental cache validation such as `ENGINE_MAX_ENTITIES` before expensive dependency configuration where CMake structure permits.
2. Add an NVIDIA GPU lane that compiles/links the deferred shader and records the selected render path.
3. Keep asset-packer CLI failure tests for malformed sources and graph persistence in the normal headless suite.
4. Continue treating clang-tidy's zero first-party findings and both comment audits as merge gates.

## 9. Test Recommendations

1. Add deterministic file-I/O fault injection so short reads and failed atomic writes can be tested without filesystem races.
2. Add offscreen golden-image tests for point/spot deferred lighting when a stable GPU runner is available.
3. Retain exact generation-wrap tests for every externally visible fixed-slot handle type.
4. Keep the shipped Lua modules in integration coverage so demo code cannot drift from the generated binding surface.

## 10. Prioritized Fix List

1. **Completed:** generation-bearing entity script, timer, joint, editor-command, and texture handles.
2. **Completed:** transactional Lua reload, asset database registration, dependency graph parsing/mutation, and demo setup.
3. **Completed:** deferred-light data texture, tile-layout correction, texture format/alignment handling, and context-safe editor teardown.
4. **Completed:** physics-query validation/deterministic truncation, mesh index validation, camera UB removal, DAP bounded decoding, and thumbnail safety.
5. **Follow-up validation:** run deferred shader link and representative lighting captures on NVIDIA hardware.
6. **Follow-up coverage:** add deterministic I/O fault injection and renderer golden images.

## 11. Final Assessment

The reviewed first-party tree is materially safer and more deterministic after the campaign. All confirmed findings were repaired with focused regressions where deterministic reproduction was practical. The final build, 90 headless-safe tests, three determinism tests, three benchmark gates, three GPU-labelled tests, 115-file clang-tidy target, CMake invalid-capacity check, and both comment audits pass.

There are no known open code defects from this review. That result is an evidence-based snapshot, not a guarantee that a production-scale engine contains no undiscovered bug; the remaining risk is concentrated in hardware-specific rendering behavior and failure modes that require injected I/O faults or additional platform runners.
