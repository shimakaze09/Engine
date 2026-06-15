# C++ Project Review Report

## 1. Review Scope

Project reviewed: `D:\dev\Engine`
Files reviewed: C++/CMake/test files summarized in section 12, including top-level `CMakeLists.txt`, `cmake/EngineHelpers.cmake`, module CMake files, test CMake files, core/math/physics/renderer/runtime/scripting/audio/editor/tools source and public headers, and matching unit/integration tests with particular attention to ownership, handles, parser input, asset loading, build configuration, lifecycle, and shutdown paths.
Files ignored: `.git/`, `.cache/`, generated build outputs except for verification metadata under `build/` and `build-release/`, assets, binaries, and third-party dependency source under build `_deps`.
Build system: CMake 3.28 project with module helper functions, C++20, Visual Studio build tree verified.
C++ standard: C++20 (`CMAKE_CXX_STANDARD 20`, extensions off).
Build verification: Successfully configured a fresh Visual Studio tree at `build-vs` with generator `Visual Studio 18 2026`, MSVC 19.51.36247.0, and Windows SDK 10.0.26100.0. Targeted Debug builds of core, math, physics, renderer, runtime, scripting, audio, editor-adjacent, and tool test targets completed successfully. After completing the review fixes, the GCC `-Werror=format-truncation` follow-up, and the headless foundation-test follow-up, `cmake --build build --config Debug --parallel` passed.
Test verification: `ctest --test-dir build-vs -C Debug -R "engine_unit_(vfs|async_streaming|pool_allocator)$"` passed 3/3 tests. `engine_unit_input_map` passed. The focused core subset `engine_unit_(touch_input|profiler|cvar_console|event_bus|input|platform_paths)` passed 6/6 tests. The focused math/physics subset `engine_unit_(math|physics|physics_world_view|collision_layer|joints|physics_query|ccd|speculative_contacts|determinism)` passed 9/9 tests. The focused renderer/runtime/scripting/audio/tool subset `engine_unit_(asset_database|asset_manager|texture_loader|mesh_loader|gpu_profiler|shader_system|audio|scripting|scene_serializer|runtime_world|prefab|runtime_service_registry|dependency_graph|lru_cache|light_culling|command_buffer|shadow_map|post_process_stack|engine_pipeline)` passed 19/19 tests. The focused integration subset `engine_integration_(lifecycle|lua_lifecycle|timer|camera|coroutine|sandbox|async_load|streaming_budget|hotreload|dap|asset_dep_load)` passed 11/11 tests after building the targets. After completing the review fixes, the GCC `-Werror=format-truncation` follow-up, and the headless foundation-test follow-up, the full Debug CTest suite passed 82/82 tests. Several findings remain because the current tests do not cover the edge cases called out below.

Fix progress in current working tree: `CPP-HIGH-001`, `CPP-HIGH-002`, `CPP-HIGH-003`, `CPP-HIGH-004`, `CPP-HIGH-006`, `CPP-HIGH-007`, `CPP-HIGH-009`, and `CPP-HIGH-011` have source/test fixes applied; targeted Debug builds and focused CTest runs for `engine_unit_vfs`, `engine_unit_async_streaming`, `engine_unit_input_map`, `engine_unit_cvar_console`, `engine_unit_pool_allocator`, `engine_unit_pass_resources`, and `engine_unit_scripting` passed. `CPP-HIGH-005` and `CPP-MED-005` have source/test fixes applied; the targeted Debug build and focused CTest run for `engine_unit_import_settings` passed. `CPP-HIGH-008` has source/test fixes applied; the targeted Debug build and focused CTest run for `engine_unit_physics` passed. `CPP-HIGH-010` has source/test fixes applied; the targeted Debug build and focused CTest run for `engine_unit_asset_database` passed. `CPP-MED-001` has a CMake fix applied; regenerating/building `engine_unit_pass_resources` no longer emits the previous missing-analyzer warning, and its focused CTest run passed. `CPP-MED-004` has source/test fixes applied; the targeted Debug build and focused CTest run for `engine_unit_touch_input` passed. `CPP-MED-006` has source/test fixes applied; the targeted Debug build and focused CTest run for `engine_unit_profiler` passed. `CPP-MED-007` and `CPP-MED-008` have source/test fixes applied, including the same static-registration bug for `physics.ccd_threshold`; the targeted Debug build and focused CTest run for `engine_unit_physics` passed. `CPP-MED-009` has source/test fixes applied; the targeted Debug build for `engine_unit_audio`, `engine_unit_texture_loader`, and `engine_unit_texture_loader_handles` passed, and the focused CTest run passed 3/3. `CPP-MED-010` has source/test fixes applied; the targeted Debug build for `engine_unit_command_history` and `engine_editor` passed, and the focused CTest run for `engine_unit_command_history` passed. `CPP-MED-011` has source/test fixes applied; the targeted Debug build for `engine_unit_dependency_graph`, `engine_unit_deterministic_cook`, and `asset_packer` passed, and the focused CTest run for `engine_unit_(dependency_graph|deterministic_cook)` passed 2/2. `CPP-MED-012` has a CMake fix applied; `cmake -DEMSDK=D:/dev/Engine -P cmake\toolchains\emscripten.cmake` confirmed the missing-shell fallback path, and a quick targeted Debug build/CTest run for `engine_unit_command_history` passed. `CPP-MED-013` has a source fix applied; the targeted Debug build for `asset_packer` passed, and the rebuilt executable's usage path was smoke-run successfully. `CPP-MED-014` has source/test fixes applied; the targeted Debug build and focused CTest run for `engine_integration_dap` passed. `CPP-MED-015` has source/test fixes applied; the targeted Debug build and focused CTest run for `engine_unit_(prefab|scene_serializer)` passed 2/2. `CPP-MED-016` has source/test fixes applied; the targeted Debug build and focused CTest run for `engine_unit_texture_loader` passed. `CPP-MED-017` has source/test fixes applied; the targeted Debug build for `engine_unit_scripting` and scripting-adjacent integration targets passed, and the focused CTest run for `engine_(unit_scripting|integration_game_mode|integration_camera|integration_timer)` passed 4/4. `CPP-MED-018` has source/test fixes applied; the targeted Debug build for `engine_unit_scripting` and `engine_integration_game_mode` passed, and the focused CTest run for `engine_(unit_scripting|integration_game_mode)` passed 2/2. `CPP-MED-019` has source/test fixes applied; the targeted Debug build for `engine_integration_timer` and `engine_unit_scripting` passed, and the focused CTest run for `engine_(integration_timer|unit_scripting)` passed 2/2. `CPP-MED-020` has source/test fixes applied; the targeted Debug build for `engine_integration_camera` and `engine_unit_scene_serializer` passed, and the focused CTest run for `engine_(integration_camera|unit_scene_serializer)` passed 2/2. `CPP-MED-021` has source/test fixes applied; the targeted Debug build and focused CTest run for `engine_unit_runtime_service_registry` passed. `CPP-MED-022` has a source fix applied; the targeted Debug build for `engine_editor` and `engine_editor_app` passed, and the available editor-adjacent `engine_unit_command_history` CTest passed. `CPP-LOW-001` has a CMake test-label/headless-platform fix applied; regenerating/building `engine_unit_foundation` passed, the focused CTest run passed, and the full Debug CTest suite passed 82/82 after the foundation test stopped requiring a GL context. `CPP-LOW-002` was addressed by the allocator lifetime fix; `PoolAllocator` comments now describe constructed object lifetime and caller responsibilities, and `engine_unit_pool_allocator` passed.

Combined verification after auditing completed fixes: a single focused Debug build of `engine_unit_vfs`, `engine_unit_async_streaming`, `engine_unit_input_map`, `engine_unit_cvar_console`, `engine_unit_pool_allocator`, `engine_unit_import_settings`, `engine_unit_physics`, `engine_unit_asset_database`, `engine_unit_pass_resources`, `engine_unit_scripting`, `engine_unit_touch_input`, and `engine_unit_profiler` passed. The matching focused CTest regex passed 12/12 tests. The command-history ownership fix was additionally verified with a targeted Debug build of `engine_unit_command_history` and `engine_editor`, followed by a focused CTest run that passed. The asset-packer JSON escaping fix was additionally verified with a targeted Debug build of `engine_unit_dependency_graph`, `engine_unit_deterministic_cook`, and `asset_packer`, followed by a focused CTest regex that passed 2/2 tests. The Emscripten shell-file guard was verified by running the toolchain script with `EMSDK` set and by a quick targeted VS build/CTest smoke. The checksum parsing fix was additionally verified with a targeted Debug build of `asset_packer` and a smoke run of its usage path. The DAP Content-Length parser fix was additionally verified with a targeted Debug build and focused CTest run of `engine_integration_dap`. The prefab/scene atomic-load fix was additionally verified with targeted Debug builds and a focused CTest run of `engine_unit_prefab` and `engine_unit_scene_serializer`. The stale audio/texture handle generation fix was additionally verified with targeted Debug builds of `engine_unit_audio`, `engine_unit_texture_loader`, and `engine_unit_texture_loader_handles`, followed by a focused CTest run that passed 3/3 tests. The oversized texture input-size fix was additionally verified with a targeted Debug build and focused CTest run of `engine_unit_texture_loader`. The Lua entity generation fix was additionally verified with a targeted Debug build of `engine_unit_scripting`, targeted builds of `engine_integration_game_mode`, `engine_integration_camera`, and `engine_integration_timer`, and a focused CTest run that passed 4/4 tests. The player-controller generation fix was additionally verified with a targeted Debug build and focused CTest run of `engine_unit_scripting` and `engine_integration_game_mode`. The stale timer ID fix was additionally verified with a targeted Debug build and focused CTest run of `engine_integration_timer` and `engine_unit_scripting`. The camera owner generation fix was additionally verified with a targeted Debug build and focused CTest run of `engine_integration_camera` and `engine_unit_scene_serializer`. The service-registration rollback fix was additionally verified with a targeted Debug build and focused CTest run of `engine_unit_runtime_service_registry`. The editor thumbnail loader fix was additionally verified with targeted Debug builds of `engine_editor` and `engine_editor_app`, plus the available editor-adjacent `engine_unit_command_history` CTest; no headless thumbnail-loader test harness exists yet. The foundation test label/headless-platform fix was additionally verified by regenerating/building `engine_unit_foundation`, running its focused CTest, and running the full Debug CTest suite. The stale generated PCH sidecars in the local `build` tree were removed and the full Debug build plus a focused core/renderer CTest subset passed. The GCC `-Werror=format-truncation` console-help follow-up was verified with a targeted Debug build of `engine_core`. After completing the current fix set, `cmake --build build --config Debug --parallel` passed and the full Debug CTest suite passed 82/82 tests.

## 2. Executive Summary

Overall status: The project has a strong CMake/test structure and strict compiler settings, but several concrete correctness and robustness issues should be fixed before treating the engine as production-ready.
Main risks: VFS path traversal outside mounted roots, async asset streaming queue exhaustion after completed requests, allocator object-lifetime undefined behavior for non-trivial types, public APIs that can dereference null names, unchecked physics and asset metadata payloads, renderer/editor resource leaks on partial or cache-full creation paths, scripting shutdown leaving touch/gesture callbacks tied to a closed Lua state, stale Lua entity/player-controller/timer/camera-owner handles, partial service registrations on init failure, malformed debug/asset input edge cases, unsafe asset-tool output/checksum handling, incomplete Web toolchain assets, and documented thread-safety guarantees that are not implemented in the console/CVar systems.
Most important fixes: Canonicalize and constrain VFS paths, add a release/retire mechanism for completed streaming requests, make `PoolAllocator` construct/destroy objects or restrict its type contract, add missing null guards, validate physics and renderer metadata payloads, clean up partial GPU resource creation, unregister Lua-backed callbacks during shutdown, and either implement or remove the console/CVar thread-safety contract.

## 3. Critical Issues

No critical issues were confirmed in this review pass.

## 4. High Severity Issues

### CPP-HIGH-001
Severity: High
Category: Security / path handling
Location: `core/src/vfs.cpp:65`, `core/src/vfs.cpp:105`
Problem: `resolve()` normalizes slashes and joins the mounted OS directory with the virtual-path remainder, but it does not reject `..`, absolute-path fragments, drive-like paths, or canonicalize the final path back under the mount root.
Why it matters: A caller can resolve paths such as `assets/../outside.txt` through a mounted `assets` prefix and escape the intended virtual root. The same resolver is used by read, write, exists, mtime, and public path resolution APIs, so this affects both reads and writes.
Recommended fix: Parse virtual paths into components, reject `.`/`..` traversal and absolute remainders, then canonicalize or lexically normalize the final path and verify it remains under the mounted root before opening files.
Example fix: Add a validation step before line 114 that rejects any remainder component equal to `..`, and for filesystem-backed platforms use `std::filesystem::weakly_canonical(root / remainder)` plus a root-prefix check before returning the resolved path.

### CPP-HIGH-002
Severity: High
Category: Resource management / async asset loading
Location: `renderer/include/engine/renderer/asset_streaming.h:35`, `renderer/include/engine/renderer/asset_streaming.h:120`, `renderer/src/asset_streaming.cpp:288`, `renderer/src/asset_streaming.cpp:463`
Problem: Terminal `Ready` or `Failed` requests are not automatically released even though the update path comments say they are cleaned when the handle goes out of scope. `LoadHandle` is only an index and has no destructor; the only non-shutdown path that clears `occupied` is `cancel_load()`, which is documented as canceling a pending request rather than releasing a completed one.
Why it matters: Callers that follow the documented handle-out-of-scope behavior leave terminal slots occupied. After enough distinct completed or failed loads, `find_free_request()` can run out of free slots and future `load_asset_async()` calls fail with "queue full" even though no work is pending.
Recommended fix: Add and document an explicit terminal-handle release path, make `LoadHandle` own/release its slot, or document and rename the existing clear path so completed handles are intentionally retired. Decrement `queue->count` when a terminal slot is released.
Example fix: Implement `release_load()` that locks the queue, verifies the handle is terminal, assigns `LoadRequest{}` to the slot, decrements `count`, and notifies `stateChanged`; add a test that completes `kMaxRequests + 1` sequential loads while retiring each terminal handle.

### CPP-HIGH-003
Severity: High
Category: Undefined behavior / object lifetime
Location: `core/include/engine/core/pool_allocator.h:31`
Problem: `PoolAllocator<T>::allocate()` returns a `T*` into raw byte storage without constructing a `T`, and `deallocate()` overwrites storage without calling `T`'s destructor.
Why it matters: This is undefined behavior for non-implicit-lifetime or non-trivially destructible types and can leak resources or crash when callers expect constructors/destructors to run. The current tests only use trivial pointer-sized structs, so this contract hole is not covered.
Recommended fix: Either restrict the allocator with `static_assert(std::is_trivially_default_constructible_v<T> && std::is_trivially_destructible_v<T>)` and document that it returns raw slots for trivial types, or provide `create()`/`destroy()` APIs using placement new and `std::destroy_at`.
Example fix: Include `<type_traits>` and `<memory>`, then add `template<class... Args> T* create(Args&&... args)` that placement-constructs in an allocated slot and `destroy(T*)` that calls `std::destroy_at(ptr)` before returning the slot.

### CPP-HIGH-004
Severity: High
Category: Robustness / null pointer handling
Location: `core/src/input_map.cpp:376`, `scripting/src/scripting.cpp:1538`
Problem: `is_mapped_action_pressed(const char *name)` does not guard against `name == nullptr` before calling `std::strcmp(g_mappedActions[i].name, name)`. The function returns false for null only when no action slots are occupied; once an action is registered, a null name can reach `strcmp`. The Lua binding passes `lua_tostring(state, 1)` directly, so a missing or non-string script argument can provide that null pointer.
Why it matters: The public C++ API and script API can crash or hit undefined behavior on invalid input. `tests/unit/input_map_test.cpp:515` expects `is_mapped_action_pressed(nullptr)` to return false, but the test runs before registering any action, so it misses the failing state.
Recommended fix: Mirror `is_mapped_action_down()` and call `find_mapped_action(name)`, or add an immediate null check at the start of `is_mapped_action_pressed()`.
Example fix: `if (name == nullptr) { return false; }` before the loop at line 378, plus a regression test that registers an action and then calls `is_mapped_action_pressed(nullptr)`.

### CPP-HIGH-005
Severity: High
Category: Robustness / parser recursion
Location: `core/src/json.cpp:149`, `core/src/json.cpp:153`, `core/src/json.cpp:203`, `core/src/json.cpp:269`, `core/src/json.cpp:280`
Problem: The JSON parser is an unbounded recursive-descent parser. `parse_value()` calls `parse_object()`/`parse_array()`, and those functions call `parse_value()` for every nested value without a depth counter or maximum nesting limit.
Why it matters: Deeply nested JSON in scene, prefab, input-map, or asset-packer inputs can exhaust the C++ call stack and crash the process instead of returning a parse error. These files are treated as external data by multiple subsystems.
Recommended fix: Add a depth parameter to `parse_value()`, `parse_array()`, and `parse_object()`, reject inputs beyond a documented maximum, and expose that failure as an ordinary parse failure.
Example fix: Use `constexpr std::uint32_t kMaxJsonDepth = 128U`; increment depth before parsing child arrays/objects and return false once the limit is reached.

### CPP-HIGH-006
Severity: High
Category: Thread safety / API contract
Location: `core/include/engine/core/cvar.h:11`, `core/src/cvar.cpp:36`, `core/include/engine/core/console.h:11`, `core/src/console.cpp:38`
Problem: The CVar and console headers state that the APIs are safe to call from any thread after initialization, but the implementations mutate shared globals such as `g_entries`, `g_count`, `g_commands`, `g_commandCount`, and the console output ring without a mutex or atomic synchronization.
Why it matters: Concurrent registration, reads, writes, command execution, or console output can data-race, which is undefined behavior in C++. The current headers invite exactly that usage.
Recommended fix: Either protect each system's shared state with a small mutex or remove the thread-safe guarantee and document single-thread ownership. If console callbacks can call back into CVar/console APIs, avoid holding locks while invoking user callbacks or use a careful two-phase design.
Example fix: Add an internal mutex around registry and ring-buffer access, copy callback targets/arguments while locked, release the lock, then invoke the command callback.

### CPP-HIGH-007
Severity: High
Category: Robustness / null pointer handling
Location: `core/src/cvar.cpp:41`, `core/src/cvar.cpp:163`, `core/src/cvar.cpp:200`, `core/src/cvar.cpp:230`
Problem: `find_cvar(const char *name)` passes `name` directly to `std::strcmp()`. Registration and `cvar_set_from_string()` guard null names, but the typed getters and setters call `find_cvar()` directly, and `cvar_set_string()` only checks `value`.
Why it matters: Public CVar calls such as `cvar_get_bool(nullptr, fallback)` or `cvar_set_bool(nullptr, true)` can crash once any CVar is registered. The header says getters return fallbacks when the name is not found and setters return false when the name is not found, so null should be handled as a not-found name.
Recommended fix: Add `if (name == nullptr) return -1;` at the start of `find_cvar()` and add regression tests for null names after at least one CVar is registered.
Example fix: Update `find_cvar()` at line 41 to reject null before iterating, then test every typed getter/setter with a null name.

### CPP-HIGH-008
Severity: High
Category: Memory safety / physics payload validation
Location: `physics/src/physics.cpp:161`, `physics/src/physics.cpp:179`, `physics/src/physics.cpp:405`, `physics/src/physics.cpp:420`, `physics/src/physics.cpp:1215`, `physics/src/convex_hull.cpp:703`
Problem: `set_convex_hull_data()` and `set_heightfield_data()` copy caller-provided payloads without validating counts or dimensions. Later code trusts `ConvexHullData::vertexCount`/`planeCount` and `HeightfieldData::rows`/`columns` for array indexing and `rows - 2` / `columns - 2` calculations.
Why it matters: A malformed or partially initialized shape payload can drive out-of-bounds reads from fixed arrays, size_t underflow in heightfield bounds, or invalid collision/raycast results. The runtime bridge forwards payloads directly by entity index, so this is reachable from engine-facing APIs.
Recommended fix: Validate shape payloads at setter boundaries. Reject convex hulls with zero or over-capacity counts, reject heightfields with rows/columns outside `[2, kMaxResolution]`, reject `rows * columns > kMaxSamples`, and reject non-positive spacing.
Example fix: Add `validate_convex_hull()` and `validate_heightfield()` helpers called before lines 163 and 181; return false without storing invalid payloads and add tests with zero, one, oversized, and max-sized heightfields/hulls.

### CPP-HIGH-009
Severity: High
Category: Resource management / renderer initialization
Location: `renderer/src/pass_resources.cpp:213`, `renderer/src/pass_resources.cpp:221`, `renderer/src/pass_resources.cpp:229`, `renderer/src/pass_resources.cpp:237`, `renderer/src/pass_resources.cpp:249`, `renderer/src/pass_resources.cpp:258`, `renderer/src/pass_resources.cpp:272`, `renderer/src/pass_resources.cpp:279`, `renderer/src/pass_resources.cpp:286`, `renderer/src/pass_resources.cpp:293`, `renderer/src/pass_resources.cpp:318`, `renderer/src/pass_resources.cpp:327`, `renderer/src/pass_resources.cpp:350`
Problem: `create_gpu_resources()` writes new texture/FBO handles directly into global `g_state` and returns false on later failures without destroying resources already created in the same attempt. `initialize_pass_resources()` then returns false with `initialized == false`, and `shutdown_pass_resources()` immediately returns when not initialized, so partially created GPU handles are leaked. `resize_pass_resources()` destroys the old resources first, then has the same leak/stale-state problem if recreating the new set fails.
Why it matters: A transient GPU allocation/FBO failure can leak textures/framebuffers and leave the renderer pass-resource state partially populated but not owned by the shutdown path. Resize failure can also leave the renderer disabled with stale resource IDs.
Recommended fix: Make resource creation transactional. Build into a temporary state object, clean up all temporary handles on any failure, and only commit to `g_state` after the full set succeeds.
Example fix: Replace direct writes to `g_state` with a local `PassResourceState next`; on every failure call `destroy_gpu_resources(next)` or a small helper that accepts the state to destroy, then assign `g_state = next` only after line 303 succeeds.

### CPP-HIGH-010
Severity: High
Category: Memory safety / asset metadata validation
Location: `renderer/src/asset_database.cpp:551`, `renderer/src/asset_database.cpp:563`, `renderer/include/engine/renderer/asset_metadata.h:55`, `renderer/include/engine/renderer/asset_metadata.h:58`, `renderer/include/engine/renderer/asset_metadata.h:71`, `renderer/src/asset_database.cpp:675`, `renderer/src/asset_database.cpp:740`
Problem: `register_asset_metadata()` copies caller-provided `AssetMetadata` directly into the database without validating `tagCount` or `dependencyCount` against the fixed `tags` and `dependencies` arrays. Later helpers iterate those counts and index the fixed arrays.
Why it matters: Invalid metadata registered through the public API can cause out-of-bounds reads in `asset_metadata_has_tag()`, `get_dependencies()`, or dependency-recursive loading. This is especially risky because asset metadata is derived from external asset pipeline data.
Recommended fix: Reject or clamp metadata with `tagCount > AssetMetadata::kMaxTags` or `dependencyCount > AssetMetadata::kMaxDependencies` at registration time, and validate individual tag strings if metadata can come from external files.
Example fix: Add validation before line 563 and return false for over-capacity counts; add regression tests that intentionally submit counts of `kMaxTags + 1` and `kMaxDependencies + 1`.

### CPP-HIGH-011
Severity: High
Category: Lifecycle / scripting callback ownership
Location: `scripting/src/scripting.cpp:1609`, `scripting/src/scripting.cpp:1618`, `scripting/src/scripting.cpp:1649`, `scripting/src/scripting.cpp:1655`, `scripting/src/scripting.cpp:1674`, `scripting/src/scripting.cpp:1729`, `scripting/src/scripting.cpp:1736`, `scripting/src/scripting.cpp:5366`, `scripting/src/scripting.cpp:5397`, `core/src/touch_input.cpp:520`, `core/src/touch_input.cpp:555`
Problem: Lua touch and gesture callbacks store registry refs and a `lua_State *` in scripting globals, and register C callbacks with core touch input. `shutdown_scripting()` releases many Lua refs before `lua_close()`, but it does not unregister touch/gesture callbacks, unref `g_touchCallbackRef`/`g_gestureCallbackRefs`, or set `g_touchLuaState` back to null.
Why it matters: After scripting shutdown or play-stop reinitialization, a later touch/gesture event can call back into a closed Lua state. Re-registering callbacks can also accumulate duplicate core callback slots because registration does not deduplicate existing function/userData pairs.
Recommended fix: During scripting shutdown, unregister `lua_touch_handler` and each registered `lua_gesture_handler`, unref all touch/gesture Lua refs while the state is still alive, reset refs to `LUA_NOREF`, and set `g_touchLuaState = nullptr`.
Example fix: Add a `clear_touch_gesture_callbacks()` helper called before line 5397 that uses `core::unregister_touch_callback(&lua_touch_handler, nullptr)` and `core::unregister_gesture_callback(type, &lua_gesture_handler, nullptr)` for each type.

## 5. Medium Severity Issues

### CPP-MED-001
Severity: Medium
Category: Build reliability
Location: `CMakeLists.txt:191`, `CMakeLists.txt:227`
Problem: The `analysis` target intentionally fails when neither `cppcheck` nor `clang-tidy` is installed. The fresh Visual Studio configure emitted "No static analyzer found; 'analysis' target will fail."
Why it matters: A named build target that always fails can surprise developers and CI jobs that build common targets or expect analysis to be available after configure. It also means static review is not currently automated on this machine.
Recommended fix: Install and document one supported analyzer for the Visual Studio lane, or gate the failing target behind an explicit option while still warning during configure.
Example fix: Install `cppcheck` or expose a preset that sets `CMAKE_PROGRAM_PATH` to a known `clang-tidy`/`cppcheck` location before configuring.

### CPP-MED-002
Severity: Medium
Category: Build / dependency reproducibility
Location: `CMakeLists.txt:46`, `CMakeLists.txt:51`, `CMakeLists.txt:61`, `audio/CMakeLists.txt:16`
Problem: Required dependencies such as SDL2 and miniaudio are fetched from Git during configure when not already installed.
Why it matters: Fresh configure is network-dependent and can fail in offline, restricted, or hermetic build environments. It also makes review and CI reproducibility depend on external service availability even though tags are pinned.
Recommended fix: Support a documented offline dependency path via package manager presets, vendored source mirrors, or a dependency cache. Keep `FetchContent` as a convenience path, but make CI and release builds use reproducible dependency inputs.
Example fix: Add a `CMakePresets.json` or README section for `CMAKE_PREFIX_PATH`/vcpkg/conan dependency resolution, and gate `FetchContent` behind an option such as `ENGINE_ALLOW_FETCHCONTENT`.

### CPP-MED-003
Severity: Medium
Category: Portability / path limits
Location: `renderer/include/engine/renderer/asset_streaming.h:61`, `renderer/src/asset_streaming.cpp:19`, `renderer/src/asset_database.cpp:75`, `renderer/src/asset_database.cpp:181`, `renderer/src/asset_database.cpp:393`, `renderer/src/asset_manager.cpp:66`, `renderer/src/asset_manager.cpp:132`, `renderer/src/asset_manager.cpp:175`, `renderer/include/engine/renderer/asset_metadata.h:104`, `renderer/src/texture_loader.cpp:56`
Problem: Multiple asset systems store source paths in fixed 260-byte arrays and silently truncate longer paths in helpers such as `write_path()`, `write_source_path()`, `copy_source_path()`, and `write_metadata_path()`.
Why it matters: Long asset paths can be truncated into a different path, causing failed loads, duplicate-looking records, metadata collisions, or misleading diagnostics. This is especially fragile on platforms that support paths longer than the legacy Windows `MAX_PATH` length.
Recommended fix: Return failure when the path does not fit, or store paths in an owned dynamic string type for queued requests and database records.
Example fix: Change each path-copy helper to return `bool`, reject `strlen(src) >= out->size()`, and make callers fail visibly instead of persisting a truncated path.

### CPP-MED-004
Severity: Medium
Category: Input correctness / touch lifecycle
Location: `core/src/touch_input.cpp:324`, `core/src/touch_input.cpp:337`, `core/src/touch_input.cpp:402`, `core/src/touch_input.cpp:412`
Problem: `SDL_FINGERDOWN` always allocates a free active-touch slot and a free timing slot. It does not first check whether the same `fingerId` is already active. `SDL_FINGERUP` uses `find_touch(fingerId)` and clears only the first matching touch.
Why it matters: A duplicate finger-down event for the same finger ID leaves duplicate active state behind; the subsequent finger-up clears one copy and one timing entry, so `touch_active_count()` and gesture state can report a phantom touch.
Recommended fix: On finger-down, first find an existing touch/timing entry for the `fingerId` and update it instead of allocating another slot. Alternatively, make finger-up clear all entries with the matching ID, but avoiding duplicates at insertion is cleaner.
Example fix: Replace the line 324 allocation with `ActiveTouch *slot = find_touch(fingerId); if (slot == nullptr) slot = find_empty_touch();`, and apply the same reuse-before-allocate pattern to `g_touchTimings`.

### CPP-MED-005
Severity: Medium
Category: Robustness / JSON validation
Location: `core/src/json.cpp:64`, `core/src/json.cpp:70`, `core/src/json.cpp:83`
Problem: `parse_string_token()` accepts any escaped character except it performs extra validation only for `\u` hex digits, and it also allows raw control characters inside strings.
Why it matters: Invalid JSON such as `"\q"` or strings containing unescaped control bytes can be accepted as valid engine data. That makes malformed settings, input maps, scenes, prefabs, or asset-packer input harder to diagnose and can diverge from tools that produce or validate standard JSON.
Recommended fix: Validate escape characters against JSON's allowed set (`"`, `\`, `/`, `b`, `f`, `n`, `r`, `t`, `u`) and reject unescaped bytes below `0x20`.
Example fix: After line 64, switch on `*cursor`; only allow the standard escape characters, validate four hex digits for `u`, and return false for any other escape or raw control byte.

### CPP-MED-006
Severity: Medium
Category: Diagnostics / profiler correctness
Location: `core/src/profiler.cpp:79`, `core/src/profiler.cpp:80`, `core/src/profiler.cpp:84`, `core/src/profiler.cpp:116`, `core/src/profiler.cpp:120`
Problem: `ProfileScope` always calls `profiler_end_scope()` in its destructor, but `profiler_begin_scope()` silently returns without pushing a stack entry when the entry buffer is full or the scope depth exceeds `kMaxDepth`.
Why it matters: A dropped scope still pops the previous real scope when the RAII object is destroyed, corrupting parent/child relationships and end times for the rest of the frame's profiling data.
Recommended fix: Make begin-scope return a token or boolean that indicates whether a scope was actually pushed, store it in `ProfileScope`, and only call end-scope for active scopes.
Example fix: Change `profiler_begin_scope()` to return `bool`, add `bool m_active` to `ProfileScope`, and have `~ProfileScope()` call `profiler_end_scope()` only when `m_active` is true.

### CPP-MED-007
Severity: Medium
Category: Physics correctness / timestep handling
Location: `physics/src/physics.cpp:1040`, `physics/src/physics.cpp:1680`, `physics/src/physics.cpp:2006`
Problem: The physics integration APIs accept `deltaSeconds`, and `step_physics_range()` uses it for velocity and position integration, but collision speculation and constraint solving still hard-code `1.0F / 60.0F`.
Why it matters: Calling `step_physics()` with a timestep other than 1/60 seconds produces mixed-timestep behavior: bodies integrate using the caller's delta, while speculative contacts and spring/constraint damping use 1/60. That makes the lower-level physics API frame-rate dependent and can produce different collision/joint behavior outside the fixed pipeline path.
Recommended fix: Pass the active physics timestep through collision resolution and constraint solving, or document that the physics API is fixed-step only and reject non-fixed deltas at the public boundary.
Example fix: Change `resolve_collisions(PhysicsWorldView &world)` to accept `float deltaSeconds`, use that instead of `kSpeculativeDt`, and call `solve_constraints(world, deltaSeconds)` at line 2006.

### CPP-MED-008
Severity: Medium
Category: Configuration / static initialization
Location: `physics/src/constraint_solver.cpp:17`, `physics/src/constraint_solver.cpp:18`, `physics/src/ccd.cpp:30`, `core/src/cvar.cpp:57`
Problem: `physics.solver_iterations` and `physics.ccd_threshold` are registered in namespace-scope static initializers, but `initialize_cvars()` later clears `g_entries` and `g_count`. In the normal initialized engine path, the static registrations are erased.
Why it matters: The intended runtime CVars are not visible to console enumeration after CVar initialization, and `solve_constraints()` / CCD threshold checks fall back to defaults because the registered values no longer exist. Future code may assume changing those physics CVars works when it does not.
Recommended fix: Register engine CVars during an explicit module initialization step that runs after `initialize_cvars()`, or make `initialize_cvars()` preserve pre-registered static entries.
Example fix: Move the registration at line 18 into a `register_physics_cvars()` function called from core/runtime initialization after CVars are initialized, and add a test that enumerates `physics.solver_iterations` after `initialize_cvars()`.

### CPP-MED-009
Severity: Medium
Category: API correctness / stale handles
Location: `audio/include/engine/audio/audio.h:10`, `audio/src/audio.cpp:120`, `audio/src/audio.cpp:169`, `audio/src/audio.cpp:179`, `audio/src/audio.cpp:199`, `renderer/include/engine/renderer/texture_loader.h:10`, `renderer/src/texture_loader.cpp:93`, `renderer/src/texture_loader.cpp:105`, `renderer/src/texture_loader.cpp:534`, `renderer/src/texture_loader.cpp:539`
Problem: `SoundHandle` and `TextureHandle` are slot-only IDs with no generation/version. After unload, the freed slot can be reused by a later load, and stale handles are considered valid if the same slot is active again.
Why it matters: Old handles can operate on unrelated new resources. A stale sound handle can play or stop a newly loaded sound; a stale texture handle can report the GPU ID/HDR/cubemap state of a different texture. Current tests cover invalid IDs but not unload/reuse aliasing.
Recommended fix: Split handles into index + generation, increment the generation on every slot free/reuse, and validate both parts on lookup. Also clear any global state, such as skybox texture selection, when unloading the selected resource.
Example fix: Store `generation` in each slot, return `(generation << indexBits) | index`, and have lookup compare the handle generation to the slot generation before returning a live resource.

### CPP-MED-010
Severity: Medium
Category: Resource ownership / Rule of Five
Location: `editor/include/engine/editor/command_history.h:20`, `editor/include/engine/editor/command_history.h:25`, `editor/include/engine/editor/command_history.h:39`, `editor/src/command_history.cpp:18`, `editor/src/command_history.cpp:30`, `editor/src/command_history.cpp:64`
Problem: `CommandHistory` takes ownership of raw `EditorCommand *` entries and deletes them from `execute()`/`clear()`, but it has no destructor and does not delete copy/move operations. A history destroyed without `clear()` leaks commands; a copied history aliases owned command pointers and can double-delete if either copy is cleared.
Why it matters: The current editor calls `clear()` on some paths, but the class contract itself is unsafe and easy to misuse in tests, tools, or future editor sessions. Ownership should be represented in the type.
Recommended fix: Store `std::unique_ptr<EditorCommand>` in the fixed history or explicitly delete copy operations and add a destructor that calls `clear()`.
Example fix: Add `~CommandHistory() noexcept { clear(); }`, `CommandHistory(const CommandHistory &) = delete;`, and `CommandHistory &operator=(const CommandHistory &) = delete;`, then consider replacing the raw array with `std::array<std::unique_ptr<EditorCommand>, kMaxHistory>`.

### CPP-MED-011
Severity: Medium
Category: Tool robustness / JSON output escaping
Location: `tools/asset_packer/main.cpp:707`, `tools/asset_packer/main.cpp:713`, `tools/asset_packer/main.cpp:714`, `tools/asset_packer/main.cpp:731`, `tools/asset_packer/dependency_graph.cpp:433`, `tools/asset_packer/dependency_graph.cpp:451`
Problem: Asset-packer metadata and dependency-graph JSON are written with raw `%s` path insertion and no JSON string escaping.
Why it matters: Paths containing quotes, backslashes that form escapes, control characters, or other special JSON characters can produce invalid or misleading `.meta.json` and graph files. The dependency graph reader is already a minimal parser, so malformed persisted graph data can silently lose paths or edges.
Recommended fix: Use `core::JsonWriter` for these files or add one shared JSON string escape helper used for every path and string value emitted by the tools.
Example fix: Replace the `fprintf()` JSON construction with `JsonWriter::write_string("source", inputPath)`, `write_string("output", outputPath)`, and escaped dependency path writes.

### CPP-MED-012
Severity: Medium
Category: Build portability / Web toolchain
Location: `cmake/toolchains/emscripten.cmake:40`, `cmake/toolchains/emscripten.cmake:41`
Problem: The Emscripten toolchain always adds `--shell-file ${CMAKE_SOURCE_DIR}/app/web/shell.html`, but `app/web/shell.html` is not present in the repository.
Why it matters: Web builds that reach link time will fail because the configured shell file does not exist. This also makes the documented `build-web` flow incomplete for fresh clones.
Recommended fix: Add the expected shell file, make the shell file path optional, or guard the flag behind an existence check with a clear warning.
Example fix: `if(EXISTS "${CMAKE_SOURCE_DIR}/app/web/shell.html") set(CMAKE_EXE_LINKER_FLAGS_INIT "... --shell-file ...") else() set(CMAKE_EXE_LINKER_FLAGS_INIT "-sUSE_SDL=2 -sALLOW_MEMORY_GROWTH=1 -sWASM=1") endif()`.

### CPP-MED-013
Severity: Medium
Category: Tool portability / undefined behavior
Location: `tools/asset_packer/main.cpp:899`, `tools/asset_packer/main.cpp:907`
Problem: `read_thumbnail_checksum()` scans a `%llu` value by casting a `std::uint64_t *` to `unsigned long long *` with `reinterpret_cast`.
Why it matters: `std::uint64_t` is not guaranteed to be the same type as `unsigned long long` on every supported platform. Writing through the wrong pointer type can violate aliasing and object-type rules, and a malformed or missing `outHash` pointer is not guarded before the write.
Recommended fix: Scan into a local `unsigned long long`, check the result, then assign the converted value to `*outHash` only after verifying `outHash != nullptr`.
Example fix: `unsigned long long parsed = 0ULL; const int scanned = std::fscanf(f, "%llu", &parsed); if (scanned == 1 && outHash != nullptr) { *outHash = static_cast<std::uint64_t>(parsed); } return scanned == 1 && outHash != nullptr;`

### CPP-MED-014
Severity: Medium
Category: Robustness / debug protocol parsing
Location: `scripting/src/dap_server.cpp:271`, `scripting/src/dap_server.cpp:276`, `scripting/src/dap_server.cpp:302`
Problem: The DAP `Content-Length` parser accumulates the header value in a signed `int` and multiplies by 10 without overflow checks before casting the result to `std::size_t`.
Why it matters: A malformed or hostile debugger client can send a very large decimal length and trigger signed integer overflow, which is undefined behavior in C++. Even without overflow, lengths larger than the fixed receive buffer are not rejected early, so the connection is only dropped after the buffer fills.
Recommended fix: Parse into `std::size_t`, check `value <= (kRecvBufferSize - headerSize)` or another documented maximum, reject overflow before multiplying, and close or consume invalid frames instead of waiting forever.
Example fix: Before `value = value * 10 + digit`, check `value > (maxLen - digit) / 10`; after parsing the separator, reject `bodyLen > kRecvBufferSize - headerSize`.

### CPP-MED-015
Severity: Medium
Category: Serialization robustness / partial loads
Location: `runtime/src/prefab_serializer.cpp:502`, `runtime/src/prefab_serializer.cpp:524`, `runtime/src/prefab_serializer.cpp:548`, `runtime/src/prefab_serializer.cpp:578`, `runtime/src/prefab_serializer.cpp:621`, `runtime/src/prefab_serializer.cpp:656`, `runtime/src/prefab_serializer.cpp:674`, `runtime/src/prefab_serializer.cpp:701`, `runtime/src/prefab_serializer.cpp:738`, `runtime/src/prefab_serializer.cpp:766`, `runtime/src/scene_serializer.cpp:843`, `runtime/src/scene_serializer.cpp:851`, `runtime/src/scene_serializer.cpp:861`, `runtime/src/scene_serializer.cpp:878`
Problem: Prefab instantiation creates an entity and then ignores many parse and `world.add_*` failures. Scene loading is stricter for most components, but `PointLightComponent` and `SpotLightComponent` still ignore parse failures and ignored `add_*` return values.
Why it matters: Malformed prefab/scene data, full component pools, or invalid component values can produce a "successful" load with missing/default components. For prefabs, the partially created entity remains alive and callers receive its handle, making the failure hard to detect and clean up.
Recommended fix: Treat component parse/add failures consistently. Accumulate success for every requested component, destroy the just-created entity on prefab failure, and return `kInvalidEntity` or a failed scene load when requested components cannot be applied.
Example fix: Replace `static_cast<void>(world.add_mesh_component(entity, mesh));` with `if (!world.add_mesh_component(entity, mesh)) { world.destroy_entity(entity); return kInvalidEntity; }`, and make `read_vec3`/`as_float` failures reject the component instead of silently keeping defaults.

### CPP-MED-016
Severity: Medium
Category: Robustness / asset file size handling
Location: `renderer/src/texture_loader.cpp:365`, `renderer/src/texture_loader.cpp:366`, `renderer/src/texture_loader.cpp:384`, `renderer/src/texture_loader.cpp:461`, `renderer/src/texture_loader.cpp:462`
Problem: Texture loaders read files through VFS into a `std::size_t fileSize`, then pass `static_cast<int>(fileSize)` to stb image APIs without checking that the file size fits in `int`.
Why it matters: A file larger than `INT_MAX` can narrow to a negative or truncated length before reaching stb. That turns oversized external asset input into decoder misuse instead of a clean "asset too large" error.
Recommended fix: Reject texture files whose `fileSize` exceeds `std::numeric_limits<int>::max()` before calling any `stbi_*_from_memory` function.
Example fix: `if (fileSize > static_cast<std::size_t>(std::numeric_limits<int>::max())) { core::vfs_free(fileData); return kInvalidTextureHandle; } const int stbSize = static_cast<int>(fileSize);`

### CPP-MED-017
Severity: Medium
Category: Scripting API correctness / stale handles
Location: `scripting/src/scripting.cpp:276`, `scripting/src/scripting.cpp:309`, `scripting/src/scripting.cpp:650`, `scripting/src/scripting.cpp:658`, `scripting/src/scripting.cpp:664`, `runtime/src/scripting_bridge.cpp:437`, `runtime/src/scripting_bridge.cpp:468`, `runtime/src/scripting_bridge.cpp:573`
Problem: The Lua API exposes entity handles as plain entity indices. `lua_engine_spawn_entity()` returns only `entity.index`, and `read_entity()` validates by resolving the current live entity at that index. The runtime scripting bridge follows the same pattern with `find_entity_by_index(entityIndex)`.
Why it matters: C++ entities carry a generation specifically to prevent stale handles from aliasing recycled slots, but Lua scripts lose that generation. A script that stores an entity index, destroys the entity, and later reuses the old value after the slot is recycled can mutate or destroy an unrelated new entity.
Recommended fix: Expose script entity handles as `{index, generation}` userdata/table values or an opaque integer that encodes both fields. Make all script-facing lookup paths validate both index and generation.
Example fix: Return a Lua entity userdata from `spawn_entity`, store both fields, and replace `find_entity_by_index(entityIndex)` with validation against `Entity{index, generation}`.

### CPP-MED-018
Severity: Medium
Category: Gameplay state correctness / stale handles
Location: `runtime/include/engine/runtime/player_controller.h:20`, `runtime/include/engine/runtime/player_controller.h:40`, `runtime/include/engine/runtime/player_controller.h:53`, `runtime/include/engine/runtime/player_controller.h:77`, `scripting/src/scripting.cpp:1811`, `scripting/src/scripting.cpp:1813`, `scripting/src/scripting.cpp:1848`, `scripting/src/scripting.cpp:5155`, `scripting/src/scripting.cpp:5160`, `runtime/src/scripting_bridge.cpp:568`, `runtime/src/scripting_bridge.cpp:574`
Problem: Player-controller ownership is stored as a bare entity index, and although `PlayerControllerArray::on_entity_destroyed()` exists, the normal scripting/world destroy path calls `World::destroy_entity()` without notifying the controller array. The legacy `g_playerControllerEntities` mirror is also only reset globally, not when an individual controlled entity is destroyed.
Why it matters: Destroying a player-controlled entity leaves `get_player_controller()` returning the old index. If the world later recycles that slot, scripts and debug commands such as `kill_all` can treat an unrelated new entity as the player-controlled entity and skip or target the wrong object.
Recommended fix: Store full generated entity handles for controller mappings, or wire entity-destroy notifications through the same service path that destroys script-visible entities. Keep the legacy mirror and `PlayerControllerArray` in sync on every assignment and destruction.
Example fix: After resolving the entity in `scripting_destroy_entity_op()`, call a scripting/controller cleanup hook before or after `world->destroy_entity(entity)`, and add a regression where a controlled entity is destroyed, an index is recycled, and `get_player_controller()` no longer returns the stale slot.

### CPP-MED-019
Severity: Medium
Category: Timer lifecycle / stale handles
Location: `runtime/include/engine/runtime/timer_manager.h:10`, `runtime/include/engine/runtime/timer_manager.h:89`, `runtime/src/timer_manager.cpp:30`, `runtime/src/timer_manager.cpp:58`, `runtime/src/timer_manager.cpp:66`, `runtime/src/timer_manager.cpp:88`, `runtime/src/timer_manager.cpp:99`, `scripting/src/scripting.cpp:3669`, `scripting/src/scripting.cpp:3693`, `scripting/src/scripting.cpp:3709`
Problem: `TimerId` is just `slotIndex + 1`. When a one-shot fires or a timer is canceled, the slot is cleared and can be reused for a later timer with the same ID. `cancel()` accepts only the slot-derived ID, and Lua exposes that ID through `set_timeout()` / `set_interval()` and `cancel_timer()`.
Why it matters: A stale timer ID retained by script or C++ code can cancel, inspect, or release the Lua registry reference for a different timer that later reused the same slot. That can make unrelated timers disappear or detach their callback unexpectedly.
Recommended fix: Encode a per-slot generation into `TimerId`, or keep an explicit generation array and validate it in `cancel()`, callback ref cleanup, snapshots, and restore paths.
Example fix: Store `{generation, active}` per slot, increment generation whenever a slot is allocated, encode `TimerId` as `(generation << 8) | (slot + 1)`, and reject cancellation when the decoded generation does not match the slot.

### CPP-MED-020
Severity: Medium
Category: Gameplay state correctness / stale owner indices
Location: `runtime/include/engine/runtime/camera_manager.h:23`, `runtime/src/camera_manager.cpp:36`, `runtime/src/camera_manager.cpp:41`, `runtime/src/camera_manager.cpp:58`, `runtime/src/camera_manager.cpp:71`, `runtime/src/camera_manager.cpp:81`, `runtime/src/spring_arm_update.cpp:17`, `runtime/src/spring_arm_update.cpp:59`, `runtime/src/world.cpp:168`, `runtime/src/world.cpp:179`, `runtime/src/scripting_bridge.cpp:49`, `runtime/src/scripting_bridge.cpp:60`
Problem: Camera ownership is keyed by a bare entity index. Spring-arm updates and script calls push cameras with `entity.index`, while `World::destroy_entity_immediate()` removes ECS components but does not pop cameras owned by the destroyed entity.
Why it matters: A destroyed camera owner can leave a stale active camera in the stack. If the entity index is recycled, a new unrelated entity can update or pop the old camera slot because ownership is matched only by index.
Recommended fix: Store full generated entity handles for camera owners, or notify `CameraManager` from the world destruction path so it removes cameras owned by the destroyed entity. Script bridge push/pop should validate the owner entity before updating the manager.
Example fix: Add `CameraManager::on_entity_destroyed(Entity)` or `pop_camera(Entity)` with generation validation, call it from `World::destroy_entity_immediate()` before the entity generation increments, and add a regression that destroys a spring-arm/camera owner and verifies `active_camera()` no longer returns its entry.

### CPP-MED-021
Severity: Medium
Category: Initialization failure cleanup / service lifetime
Location: `runtime/src/service_registry.cpp:26`, `runtime/src/service_registry.cpp:28`, `runtime/src/service_registry.cpp:31`, `runtime/src/service_registry.cpp:50`, `runtime/src/service_registry.cpp:93`, `core/src/service_locator.cpp:18`, `core/src/service_locator.cpp:27`, `runtime/src/engine_pipeline.cpp:1285`, `runtime/src/engine_pipeline.cpp:1288`
Problem: Engine subsystem service registration is not transactional. `register_engine_subsystem_services()` registers many service types into the locator and returns a combined `ok`, but earlier registrations remain if a later `ServiceLocator::register_raw()` fails because the locator is full. `EngineServiceRegistry::register_services()` only sets `m_registered = true` after full success, so the caller's failure cleanup does not remove those partial entries.
Why it matters: A failed runtime initialization can leave stale subsystem pointers in a shared or pre-populated `ServiceLocator`. Later code can resolve services for a world, renderer, or asset database that the failed pipeline has already torn down or never fully initialized.
Recommended fix: Make subsystem registration transactional: preflight capacity, register into a temporary locator, or explicitly unregister the subsystem service set on any failure before returning false.
Example fix: In `EngineServiceRegistry::register_services()`, call `unregister_engine_subsystem_services(*m_locator)` when `register_engine_subsystem_services()` returns false, or rewrite `register_engine_subsystem_services()` to stop at the first failure and roll back the services it already added.

### CPP-MED-022
Severity: Medium
Category: Resource management / editor thumbnail loading
Location: `editor/src/editor.cpp:145`, `editor/src/editor.cpp:201`, `editor/src/editor.cpp:202`, `editor/src/editor.cpp:216`, `editor/src/editor.cpp:229`, `editor/src/editor.cpp:2145`
Problem: `load_thumbnail_texture()` reads a thumbnail file into a `std::vector<unsigned char>` and passes `static_cast<int>(fileData.size())` to `stbi_load_from_memory()` without checking that the size fits in `int`. It then uploads a GL texture and only records it when `thumbnailCount < kMaxThumbnails`; if the cache is full, the function still returns the new texture ID, but `clear_thumbnail_cache()` only deletes recorded cache entries and the caller in `draw_asset_browser_panel()` does not delete the returned texture.
Why it matters: Oversized thumbnail files can be handed to stb with a truncated/negative length. Separately, after 128 cached thumbnails, selecting another asset can create an uncached GL texture every frame and leak it until process exit.
Recommended fix: Reject thumbnail files larger than `INT_MAX` before calling stb, and make cache-full behavior fail before `glGenTextures()` or delete the just-created texture before returning. Alternatively, replace the fixed thumbnail cache with an owning LRU cache that always tracks every returned texture.
Example fix: Add `if (fileData.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return 0U;` before the stb call, and after upload delete `tex` and return `0U` when `thumbnailCount >= kMaxThumbnails`.

## 6. Low Severity Issues

### CPP-LOW-001
Severity: Low
Category: Test classification
Location: `tests/unit/CMakeLists.txt:3`
Problem: `engine_unit_foundation` is registered under unit tests but labeled `gpu`.
Why it matters: GPU-dependent tests inside the unit-test group make fast local unit lanes harder to reason about and can cause headless environments to fail or skip unexpectedly.
Recommended fix: Split GPU smoke coverage into smoke/integration targets or consistently exclude `LABELS gpu` from the default unit lane.
Example fix: Move the executable to `tests/smoke` or add a documented `ctest -LE gpu` command for CPU-only unit verification.

### CPP-LOW-002
Severity: Low
Category: Code clarity / comments
Location: `core/include/engine/core/pool_allocator.h:13`, `core/include/engine/core/pool_allocator.h:15`, `core/include/engine/core/pool_allocator.h:52`
Problem: Several generated-style comments such as "Handles static assert" and "Handles alignas" do not describe the code's real contract.
Why it matters: This allocator has a subtle lifetime contract; low-value comments make it harder for users to notice the important constraints.
Recommended fix: Replace generic comments with contract comments that state whether objects are constructed, whether destructors run, and what caller responsibilities are.
Example fix: Replace the line 30 comment with "Returns unconstructed storage for trivially constructible `T`; caller must not use this for owning/non-trivial types" if the current design is kept.

## 7. Informational Notes

### CPP-INFO-001
Category: Build configuration
Location: `CMakeLists.txt:97`, `CMakeLists.txt:136`, `CMakeLists.txt:142`, `CMakeLists.txt:165`
Observation: The project uses C++20, disables compiler extensions, enables strict warning-as-error flags, disables exceptions/RTTI, and provides an ASAN/UBSAN option for non-MSVC builds.
Suggestion: Keep these settings. Add a documented sanitizer preset and a CI lane that runs it on Clang or GCC.

### CPP-INFO-002
Category: Test coverage
Location: `tests/unit/vfs_test.cpp:50`, `tests/unit/async_streaming_test.cpp:78`, `tests/unit/pool_allocator_test.cpp:20`
Observation: There are many unit/integration tests, but the tests around the findings do not cover VFS traversal rejection, completed streaming request release/reuse, non-trivial allocator types, duplicate touch finger-downs, JSON nesting limits, invalid JSON escapes/control characters, CVar null names, CVar/console thread-safety behavior, profiler overflow/depth balancing, invalid physics shape payloads, renderer partial pass-resource creation failure, invalid asset metadata counts, Lua touch/gesture callback cleanup across shutdown/reinitialize, stale audio/texture/Lua entity/player-controller/timer/camera-owner handle reuse, partial service-registry registration rollback, command-history ownership semantics, asset-tool JSON escaping, asset-tool checksum parsing, DAP malformed `Content-Length`, oversized texture files, prefab partial-load cleanup, non-1/60 physics timesteps, or post-initialization physics CVar registration.
Suggestion: Add targeted regression tests for each finding before or with the fixes.

## 8. Build and Tooling Recommendations

Recommended compiler flags: Existing MSVC `/W4 /WX /permissive- /GR- /EHs-c-` and GCC/Clang `-Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti` are strong. Consider adding `-Wconversion -Wshadow` on GCC/Clang after triaging fallout.
Recommended sanitizer checks: Enable `ENGINE_SANITIZERS=ON` in a Clang or GCC CI lane. Add MSVC-compatible AddressSanitizer configuration if Windows sanitizer coverage is desired.
Recommended static analysis: Keep the `analysis` target. Run clang-tidy/cppcheck in CI using `compile_commands.json`, and fail on new high-confidence warnings.
Recommended formatter: Add or document clang-format settings and a formatting check target if not already enforced elsewhere.
Recommended CI checks: Fresh Visual Studio configure from an empty build directory, build all default targets, `ctest -LE gpu` CPU lane, GPU/smoke lane where hardware is available, a separate Clang/GCC sanitizer lane, and static analysis lane.

## 9. Test Recommendations

Missing tests: Add VFS tests that reject `assets/../x`, `assets/sub/../../x`, absolute remainders, and platform-specific drive-like paths. Add async streaming tests that process more than `kMaxRequests` unique assets over time and prove terminal requests are releasable/reusable. Add allocator tests using a non-trivial type with constructor/destructor counters if construction support is intended. Add an input-map test that registers at least one action before calling `is_mapped_action_pressed(nullptr)`. Add CVar null-name tests after registration, JSON invalid escape/deep nesting tests, touch duplicate-finger tests, profiler max-depth/max-entry tests, invalid physics hull/heightfield payload tests, renderer pass-resource partial-failure tests with a failing mock `RenderDevice`, invalid asset metadata count tests, stale audio/texture/Lua entity/player-controller/timer/camera-owner handle reuse tests, service-registry rollback tests with a nearly full locator, command-history destructor/copy tests, editor thumbnail oversized-file/cache-full tests, asset-packer checksum sidecar parsing tests, malformed DAP `Content-Length` tests, oversized texture file tests, prefab/scene component-pool exhaustion tests, variable-delta physics tests, a post-`initialize_cvars()` test for `physics.solver_iterations`, and a configure/link validation lane for the Emscripten toolchain.
Boundary tests: Long asset paths at 259, 260, and over 260 bytes across asset streaming, database, manager, metadata, and texture systems; VFS maximum resolved path length; mount prefix edge cases with trailing slashes and mixed separators.
Failure-path tests: Build/test behavior from a clean tree without network access; missing static analyzer tools; streaming load failure followed by slot reuse; scripting shutdown/reinitialize followed by a touch or gesture event; asset-packer JSON output for paths containing quotes and control characters; asset-packer thumbnail checksum sidecars containing malformed, missing, and maximum `uint64_t` values; prefab loads where requested component pools are already full; texture and editor thumbnail files larger than `INT_MAX`; editor thumbnail cache-full behavior; DAP frames with overflowing or over-buffer `Content-Length` values.
Regression tests: One test per finding: VFS traversal stays inside mount root, completed streaming requests do not exhaust the queue, `PoolAllocator` either rejects non-trivial types at compile time or constructs/destroys them correctly, null input/CVar names stay safe after mappings/variables exist, JSON rejects malformed/deep input, duplicate touch down/up leaves no phantom touches, profiler overflow does not unbalance the scope stack, invalid physics/asset metadata payloads are rejected, Lua touch/gesture callbacks are unregistered on shutdown, stale resource handles cannot alias new resources, Lua entity, player-controller, timer, and camera-owner handles reject recycled generations, failed service registration rolls back partial entries, asset-packer checksum parsing avoids pointer punning, DAP length parsing rejects overflow, texture and editor thumbnail loading reject oversized files before stb, editor thumbnail cache-full behavior does not leak GL textures, prefab/scene loading fails cleanly instead of leaving partial components, and non-1/60 physics deltas produce documented behavior.

## 10. Prioritized Fix List

1. Fix: Reject/canonicalize VFS traversal.
   Location: `core/src/vfs.cpp:65`
   Reason: Prevents reads/writes outside mounted roots.
   Severity: High

2. Fix: Add a lifecycle/release path for completed asset streaming requests.
   Location: `renderer/src/asset_streaming.cpp:463`
   Reason: Prevents queue exhaustion after normal completed loads.
   Severity: High

3. Fix: Define and enforce `PoolAllocator` object-lifetime semantics.
   Location: `core/include/engine/core/pool_allocator.h:31`
   Reason: Prevents undefined behavior for non-trivial types.
   Severity: High

4. Fix: Guard `is_mapped_action_pressed()` against null names.
   Location: `core/src/input_map.cpp:376`
   Reason: Prevents public API and Lua calls from passing null into `strcmp`.
   Severity: High

5. Fix: Install/configure a static analyzer for the Visual Studio lane.
   Location: `CMakeLists.txt:191`
   Reason: Makes the existing `analysis` target usable instead of intentionally failing.
   Severity: Medium

6. Fix: Implement or remove the CVar/console thread-safety guarantee.
   Location: `core/include/engine/core/cvar.h:11`
   Reason: Prevents documented multi-threaded API use from causing data races.
   Severity: High

7. Fix: Add null-name handling to CVar lookup.
   Location: `core/src/cvar.cpp:41`
   Reason: Prevents public getters/setters from passing null to `strcmp`.
   Severity: High

8. Fix: Add a JSON nesting limit.
   Location: `core/src/json.cpp:269`
   Reason: Prevents malformed external data from exhausting the call stack.
   Severity: High

9. Fix: Validate physics convex hull and heightfield payloads before storing them.
   Location: `physics/src/physics.cpp:161`
   Reason: Prevents fixed-array out-of-bounds reads and heightfield dimension underflow.
   Severity: High

10. Fix: Reject or dynamically store long streaming asset paths.
   Location: `renderer/src/asset_streaming.cpp:19`
   Reason: Avoids silent path truncation and hard-to-debug load failures.
   Severity: Medium

11. Fix: Clean up partial pass-resource creation failures.
    Location: `renderer/src/pass_resources.cpp:213`
    Reason: Prevents GPU texture/FBO leaks and stale renderer state after allocation/FBO failures.
    Severity: High

12. Fix: Validate public asset metadata counts.
    Location: `renderer/src/asset_database.cpp:551`
    Reason: Prevents out-of-bounds reads from fixed metadata arrays.
    Severity: High

13. Fix: Unregister Lua-backed touch/gesture callbacks during scripting shutdown.
    Location: `scripting/src/scripting.cpp:5366`
    Reason: Prevents callbacks from using a closed Lua state after play stop or scripting reinitialization.
    Severity: High

14. Fix: Thread the active physics timestep into collision speculation and constraints.
    Location: `physics/src/physics.cpp:2006`
    Reason: Prevents mixed-delta simulation behavior outside the fixed 1/60 pipeline path.
    Severity: Medium

15. Fix: Provide or guard the Web shell file.
    Location: `cmake/toolchains/emscripten.cmake:41`
    Reason: Prevents fresh Web builds from failing at link time on a missing `app/web/shell.html`.
    Severity: Medium

16. Fix: Remove pointer punning from asset-packer checksum parsing.
    Location: `tools/asset_packer/main.cpp:907`
    Reason: Avoids non-portable writes through an incompatible pointer type.
    Severity: Medium

17. Fix: Harden DAP `Content-Length` parsing.
    Location: `scripting/src/dap_server.cpp:271`
    Reason: Prevents malformed debugger input from causing signed overflow or receive-buffer stalls.
    Severity: Medium

18. Fix: Make prefab and point/spot scene component loads fail atomically.
    Location: `runtime/src/prefab_serializer.cpp:524`
    Reason: Prevents successful-looking loads with missing/default components or leaked partial entities.
    Severity: Medium

19. Fix: Reject texture files too large for stb's `int` length API.
    Location: `renderer/src/texture_loader.cpp:365`
    Reason: Prevents oversized texture assets from narrowing the decoder input length.
    Severity: Medium

20. Fix: Reject oversized editor thumbnails and avoid cache-full GL texture leaks.
    Location: `editor/src/editor.cpp:201`
    Reason: Prevents truncated stb input lengths and leaked uncached thumbnail textures.
    Severity: Medium

21. Fix: Preserve entity generation in script-facing entity handles.
    Location: `scripting/src/scripting.cpp:650`
    Reason: Prevents old Lua entity values from aliasing newly recycled entity slots.
    Severity: Medium

22. Fix: Clear or generation-validate player-controller entity mappings on destruction.
    Location: `scripting/src/scripting.cpp:1813`
    Reason: Prevents stale controller slots from aliasing recycled entity indices.
    Severity: Medium

23. Fix: Add generation validation to timer IDs.
    Location: `runtime/src/timer_manager.cpp:66`
    Reason: Prevents stale timer IDs from canceling or detaching callbacks from later timers in the same slot.
    Severity: Medium

24. Fix: Remove or generation-validate cameras owned by destroyed entities.
    Location: `runtime/src/camera_manager.cpp:41`
    Reason: Prevents stale camera owners from keeping or mutating camera slots after entity destruction/reuse.
    Severity: Medium

25. Fix: Roll back partial service-locator registrations on subsystem registration failure.
    Location: `runtime/src/service_registry.cpp:28`
    Reason: Prevents failed initialization from leaving stale subsystem pointers in the locator.
    Severity: Medium

## 11. Final Assessment

The project appears actively hardened and has a broad test suite, but it needs targeted fixes before it is safe to treat as production-stable. Fix VFS traversal, asset streaming request lifetime, physics and asset metadata payload validation, renderer partial-failure cleanup, and scripting callback shutdown first because they affect file access boundaries, runtime correctness, memory safety, and resource lifetime. Then tighten allocator, input/CVar, parser, profiler, handle-generation, Lua/player-controller/timer/camera-owner handles, service-registration rollback, malformed DAP/texture/editor-thumbnail/prefab input handling, timestep, asset-tool output/checksum parsing, Web toolchain, and documented thread-safety contracts, and codify the now-working Visual Studio build/test lane so these fixes can be verified continuously.

## 12. Per-File Review Appendix

This appendix records the file-by-file pass behind the findings. "No new issue" means no additional concrete issue beyond the numbered findings above was confirmed in that file during this pass.

Inventory checked with `rg --files` during review; exact file coverage is summarized by module below. Numeric line counts are intentionally omitted because they can go stale while the workspace changes.

Build files:
- `CMakeLists.txt`: strict C++20/warning configuration is solid; `analysis` target failure without analyzers is `CPP-MED-001`; FetchContent network dependency is `CPP-MED-002`.
- `cmake/EngineHelpers.cmake`: reviewed helper target/test setup; no new issue beyond test labeling noted in `CPP-LOW-001`.
- `cmake/toolchains/android.cmake`: stub/delegation behavior reviewed; no new issue.
- `cmake/toolchains/emscripten.cmake`: missing `app/web/shell.html` is `CPP-MED-012`.
- `cmake/toolchains/ios.cmake`: Xcode/iOS stub reviewed; no new issue.
- Module/test `CMakeLists.txt` files under `app`, `audio`, `core`, `editor`, `math`, `physics`, `renderer`, `runtime`, `scripting`, `tests`, and `tools`: reviewed for source coverage, public deps, and target wiring; audio FetchContent contributes to `CPP-MED-002`.

App and audio:
- `app/main.cpp`: bootstrap/run/shutdown sequence reviewed; no new issue.
- `audio/include/engine/audio/audio.h`, `audio/src/audio.cpp`: slot-only sound handles are `CPP-MED-009`; miniaudio FetchContent is `CPP-MED-002`; no additional issue.

Core:
- `core/include/engine/core/*.h`, `core/src/*.cpp`: reviewed allocator, bootstrap, console, CVar, debug draw, stats, event bus, input, input map, job system, JSON, linear allocator, logging, memory tracker, platform, profiler, reflection, service locator, sparse set, touch input, and VFS surfaces. Findings are `CPP-HIGH-001`, `CPP-HIGH-003`, `CPP-HIGH-004`, `CPP-HIGH-005`, `CPP-HIGH-006`, `CPP-HIGH-007`, `CPP-MED-004`, `CPP-MED-005`, `CPP-MED-006`, and `CPP-LOW-002`; no further concrete core issue was confirmed.
- `core/src/pch.h`: include aggregation only; no new issue.

Math:
- `math/include/engine/math/*.h`, `math/src/*.cpp`: reviewed vector, matrix, quaternion, transform, AABB, ray, sphere, and component type math. Unit tests cover basic behavior; no new concrete issue confirmed.

Physics:
- `physics/include/engine/physics/*.h`, `physics/src/*.cpp`, `physics/src/joints/*.cpp`, `physics/src/joints/joint_solvers.h`: reviewed collider payloads, world view bridge, CCD, contact manifold, convex hull, joints, queries, constraint solver, and main physics step. Findings are `CPP-HIGH-008`, `CPP-MED-007`, and `CPP-MED-008`; no additional concrete physics issue was confirmed.

Renderer:
- `renderer/include/engine/renderer/*.h`, `renderer/src/asset_database.cpp`, `asset_manager.cpp`, `asset_streaming.cpp`, `command_buffer.cpp`, `gpu_profiler.cpp`, `light_culling.cpp`, `lru_cache.cpp`, `mesh_loader.cpp`, `pass_resources.cpp`, `post_process_stack.cpp`, `render_device_gl.cpp`, `shader_system.cpp`, `shadow_map.cpp`, and `texture_loader.cpp`: reviewed asset database/manager/streaming, metadata, command buffer, GL render device, shader hot reload, mesh/texture loaders, pass resources, GPU profiler, light culling, LRU cache, post-process stack, and shadow mapping. Findings are `CPP-HIGH-002`, `CPP-HIGH-009`, `CPP-HIGH-010`, `CPP-MED-003`, `CPP-MED-009`, and `CPP-MED-016`; no additional concrete renderer issue was confirmed.
- `renderer/src/pch.h`: include aggregation only; no new issue.

Runtime:
- `runtime/include/engine/*.h`, `runtime/include/engine/runtime/*.h`, `runtime/src/camera_manager.cpp`, `editor_bridge.cpp`, `engine.cpp`, `engine_pipeline.cpp`, `physics_bridge.cpp`, `prefab_serializer.cpp`, `reflect_types.cpp`, `render_prep_pipeline.cpp`, `scene_serializer.cpp`, `scripting_bridge.cpp`, `service_registry.cpp`, `spring_arm_update.cpp`, `timer_manager.cpp`, and `world.cpp`: reviewed lifecycle, frame graph, service registration, ECS world/entity lifetime, serializer staging, prefab loading, camera manager, physics bridge, scripting bridge, timers, render prep, player-controller state, and editor bridge. JSON parser findings apply to serializer inputs; physics payload findings apply through runtime bridge; prefab/scene partial load behavior is `CPP-MED-015`; script bridge index-only entity lookup contributes to `CPP-MED-017`; player-controller index-only cleanup is `CPP-MED-018`; timer slot-only IDs are `CPP-MED-019`; camera owner index cleanup is `CPP-MED-020`; service registration rollback is `CPP-MED-021`; no additional concrete runtime issue was confirmed.

Scripting:
- `scripting/include/engine/scripting/*.h`, `scripting/src/dap_server.cpp`, `deferred_mutations.cpp`, `runtime_binding.cpp`, and `scripting.cpp`: reviewed Lua initialization/shutdown, bindings, deferred mutations, timers, coroutines, module loading, hot reload, DAP transport, touch/gesture callbacks, player-controller/timer/camera bindings, and runtime service bridge. Findings are `CPP-HIGH-004` through the input map binding, `CPP-HIGH-011`, `CPP-MED-014`, `CPP-MED-017`, `CPP-MED-018`, `CPP-MED-019`, and `CPP-MED-020`; DAP also relies on the JSON parser covered by `CPP-HIGH-005`/`CPP-MED-005`; no additional concrete scripting issue was confirmed.

Editor:
- `editor/include/engine/editor/*.h`, `editor/src/command_history.cpp`, `debug_camera.cpp`, `editor_camera.cpp`, and `editor.cpp`: reviewed editor API, command history, debug/orbit cameras, editor bridge usage, thumbnail loading, and command execution paths. Command history ownership is `CPP-MED-010`; editor thumbnail loading is `CPP-MED-022`; no additional concrete editor issue was confirmed.

Tools:
- `tools/asset_packer/main.cpp`, `tools/asset_packer/dependency_graph.cpp`, `tools/asset_packer/dependency_graph.h`, `tools/CMakeLists.txt`, and `tools/asset_packer/CMakeLists.txt`: reviewed glTF extraction, mesh writing, cook stamps, dependency graph, thumbnail generation, hull cooking, import settings, and tool CMake wiring. JSON string escaping is `CPP-MED-011`; checksum pointer punning is `CPP-MED-013`; no additional concrete tool issue was confirmed.

Tests:
- Unit, integration, smoke, and benchmark CMake/test files under `tests/`: reviewed for coverage and target naming. Gaps are summarized in section 9; `engine_unit_foundation` GPU label is `CPP-LOW-001`.
