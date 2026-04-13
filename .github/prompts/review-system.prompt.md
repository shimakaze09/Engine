---
description: 'Deep code review of a game engine subsystem with production quality gates'
---
Review the attached code as a senior engine programmer would at a studio shipping this engine.

## Pre-Review Verification

Before reviewing, verify the actual codebase state:
- Read the file(s) being reviewed.
- Check what the instruction files say about this module.
- Do NOT trust comments claiming features are complete. Verify by reading the implementation.

## Review Checklist (In Order)

1. **Phase contract** — Does any mutation (add_*/remove_*) happen outside WorldPhase::Input? Does get_transform_write_ptr get called outside Simulation? Check every world access.

2. **Dependency direction** — Does this code reach upward (renderer including runtime, scripting including editor)? Trace every #include against: core -> math -> physics/scripting/renderer/audio -> runtime -> editor -> app.

3. **Allocation on hot path** — Any heap allocation per frame? Check for: std::vector, std::string, std::map, std::unordered_map, new, std::make_shared, std::make_unique, implicit copies of containers. Frame allocator or SparseSet must be used instead.

4. **SparseSet hot loop** — If iterating component_data(), are hot and cold fields separated? Is there a linear scan that should be O(1)? Check find_mesh_asset_slot and similar patterns.

5. **noexcept and error handling** — All engine API functions must be noexcept. Errors go to core::log_message. Never thrown, never silently swallowed. Every failure path must log.

6. **Lua boundary** — If this touches scripting, are raw pointers exposed to Lua? Only entity indices (uint32_t) cross the boundary. Check every lua_push* call.

7. **API misuse prevention** — Can this be called incorrectly at compile time? Make misuse a compile error where possible. Check for implicit conversions, ambiguous overloads.

8. **Thread safety** — If accessed from multiple threads, are there races? Check: shared mutable state, atomics without proper ordering, lock elision.

9. **Buffer overflows** — Check all fixed-size arrays (char arrays, fixed buffers, SparseSet caps). Are bounds checked before writes? Can user input exceed buffer size?

10. **Resource leaks** — Check GPU resources, Lua references, file handles, allocations. Are all cleanup paths covered, including error paths?

## Output Format

For each issue found:
- **Problem**: What is wrong.
- **Risk**: What can go wrong in production.
- **Fix**: Exact code change needed.
- **Severity**: Blocking (must fix before merge) / Warning (should fix) / Note (improvement).

No praise. No filler. Only defects and fixes.
