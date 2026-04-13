---
description: "Use when editing C++ source or headers in this engine. Covers naming, noexcept policy, allocation rules, error handling, include hygiene, and code review gates."
name: "C++ Core Style"
applyTo: "**/*.h, **/*.hpp, **/*.c, **/*.cc, **/*.cpp, **/*.inl, **/*.ipp"
---
# C++ Core Style

## Language Standard

- C++20 only. No compiler extensions.
- Compile with -std=c++20 (GCC/Clang) or /std:c++20 (MSVC).
- Never use __attribute__, __declspec, or pragma-based extensions unless guarded by platform macros in core/include/engine/core/platform.h.

## Prohibited Features

These are hard errors, not style preferences:

- **No exceptions.** Compile with -fno-exceptions. Never use throw, try, or catch.
- **No RTTI.** Compile with -fno-rtti. Never use dynamic_cast, typeid, or std::any.
- **No virtual dispatch in hot paths.** Use templates, concepts, or function pointers.
- **No std::function in per-frame code.** Use function pointers or template callbacks.
- **No global constructors with side effects.** Use lazy initialization or explicit init functions.

## Noexcept Policy

- Every engine-facing public function is `noexcept`. No exceptions.
- Every function that can fail returns `bool` (true = success) or a typed result enum.
- On failure: log via `core::log_message(LogLevel, channel, message)` and return the error value.
- Never silently succeed. Never silently fail. Every error path must log.
- Assertions (`ENGINE_ASSERT`) are for programmer errors in debug builds only.

## Naming Conventions

| Element | Convention | Example |
|---------|-----------|---------|
| Types (struct, class, enum) | PascalCase | `RigidBody`, `AssetId`, `WorldPhase` |
| Functions | snake_case | `add_transform()`, `flush_renderer()` |
| Variables (local, parameter) | camelCase | `entityCount`, `deltaTime` |
| Member variables | m_prefix + camelCase | `m_entityCount`, `m_freeList` |
| Constants (constexpr, const) | k prefix + PascalCase | `kMaxEntities`, `kChunkSize` |
| Globals (avoid unless justified) | g_ prefix + camelCase | `g_state`, `g_world` |
| Macros (reflection and platform only) | ALL_CAPS | `REFLECT_TYPE`, `ENGINE_ASSERT` |
| Enum values | PascalCase | `WorldPhase::Input`, `LogLevel::Error` |
| Namespaces | lowercase | `engine::core`, `engine::runtime` |
| File names | snake_case | `job_system.cpp`, `sparse_set.h` |

## Memory and Allocation

### Hot-Path Rules (per-frame code)
- **No heap allocation.** Zero calls to new, malloc, or anything that calls them.
- **No std::vector, std::string, std::map, std::unordered_map.** These allocate on the heap.
- Use `core::frame_allocator()` for per-frame scratch data.
- Use `core::thread_frame_allocator(threadIndex)` for per-thread per-frame data.
- Use `core::PoolAllocator` for fixed-size reusable blocks.
- Use fixed-capacity arrays (C arrays or std::array) with explicit size tracking.
- Use SparseSet for component storage.

### Cold-Path Rules (initialization, loading, editor)
- Heap allocation is acceptable but must use `new (std::nothrow)` with null checks.
- Never assume allocation succeeds. Always check the return value.
- Prefer RAII-like patterns (allocate in init, free in shutdown).
- Document ownership: who allocates, who frees, when is it freed.

## Include Hygiene

- Every header must be self-contained. It must compile alone.
- Prefer forward declarations over includes in headers.
- Never include SDL, OpenGL, Lua, or ImGui headers in engine-facing public headers.
- These are implementation details confined to .cpp files behind opaque boundaries.
- Order includes: own header first, then engine headers (alphabetical), then system headers.
- Use `#pragma once` for include guards.

## Error Handling Patterns

```cpp
// CORRECT: explicit failure with logging
bool load_mesh(const char* path, GpuMesh* out) noexcept {
    if (!path || !out) {
        core::log_message(core::LogLevel::Error, "mesh", "null argument");
        return false;
    }
    if (error_condition) {
        core::log_message(core::LogLevel::Error, "mesh", "parse failed");
        return false;
    }
    return true;
}

// WRONG: silent failure (no log)
bool load_mesh(const char* path, GpuMesh* out) noexcept {
    if (!path) return false;
}

// WRONG: exception (prohibited)
GpuMesh load_mesh(const char* path) {
    throw std::runtime_error("bad mesh");
}
```

## Code Size and Complexity

- Keep functions under 80 lines. Extract helpers if a function grows.
- Keep files under 2000 lines. Split into multiple files if necessary.
- Maximum nesting depth: 4 levels. Extract a function if deeper.
- Prefer early returns over deep nesting.

## Thread Safety

- Document thread safety for every public function.
- Use `alignas(64)` for cache-line separation of per-thread data.
- Atomics: use std::atomic with explicit memory ordering. Comment relaxed ordering.
- Never hold a lock while calling into another module.

## Const Correctness

- Every non-mutated parameter is const.
- Every non-mutating member function is const.
- Every pointer-to-read-only data is const.
