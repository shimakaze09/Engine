---
description: 'Scaffold a new engine subsystem with strict compliance to module boundaries and coding standards'
---
Scaffold a new engine subsystem named ${input:systemName} in the correct module layer.

## Pre-Implementation Checklist

1. Determine the correct module: core / math / physics / scripting / renderer / audio / runtime / editor.
2. Verify the new system does NOT introduce upward dependencies.
3. Check if a similar system already exists (search the codebase first).

## File Structure

- Header: `<module>/include/engine/<module>/${input:systemName}.h`
- Implementation: `<module>/src/${input:systemName}.cpp`
- Test: `tests/unit/${input:systemName}_test.cpp`

## Header Requirements

```cpp
#pragma once
// Include only what is needed. Prefer forward declarations.
// Never include SDL, OpenGL, Lua, or ImGui in public headers.

namespace engine::<module> {

// Initialize the subsystem. Called once at startup.
// Returns false and logs on failure.
bool initialize_${input:systemName}() noexcept;

// Shut down the subsystem. Called once at exit.
// Must release all resources. Safe to call even if init failed.
void shutdown_${input:systemName}() noexcept;

// Per-frame update. Called from main loop.
// Must not heap-allocate. Must not exceed frame budget.
void update_${input:systemName}(float deltaTime) noexcept;

} // namespace engine::<module>
```

## Implementation Requirements

- Internal state goes in an anonymous namespace as plain POD structs.
- Follow the pattern in audio/src/audio.cpp or core/src/vfs.cpp.
- No std::vector or std::map in the hot path. Use fixed arrays or SparseSet.
- Every public function is noexcept, returns bool on failure, logs errors.
- Document thread safety for every public function.

## CMake Integration

- Add the .cpp to the module's CMakeLists.txt source list.
- Add test file to tests/unit/CMakeLists.txt.
- Verify no new upward dependencies are introduced.

## Test Requirements

- Create `tests/unit/${input:systemName}_test.cpp`.
- Test initialization, normal operation, edge cases, and failure paths.
- Tests must be deterministic and not require GL context (unless documented).
- Register in tests/unit/CMakeLists.txt.

## Lua Binding (If Applicable)

- If game authors need this system, add Lua bindings in scripting/src/scripting.cpp.
- Expose only high-level operations. Never expose raw pointers or internals.
- Follow the binding pattern in scripting-lua.instructions.md.

## Do NOT

- Write placeholder comments. Write real code with correct types.
- Skip error handling. Every failure path logs.
- Heap-allocate in per-frame code.
- Introduce upward module dependencies.
