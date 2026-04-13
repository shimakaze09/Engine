---
description: 'Audit a module for compliance with engine architecture, coding standards, and production quality'
---
Audit the module at ${input:modulePath} for production compliance.

## Audit Checklist

Examine every source and header file in the module. Report ALL violations.

### 1. Dependency Direction
- Trace every `#include` and `target_link_libraries` entry.
- Verify against the DAG: core → math → physics/scripting/renderer/audio → runtime → editor → app.
- Every upward or sideways dependency is a BLOCKING defect.

### 2. Public API Surface
- Every public function is `noexcept`.
- Every function that can fail returns `bool` or a result enum.
- Every failure path logs via `core::log_message`. No silent failures.
- No SDL, OpenGL, Lua, or ImGui types in public headers.

### 3. Hot-Path Allocation
- Scan per-frame functions for: `new`, `malloc`, `std::vector`, `std::string`, `std::map`, `std::make_shared`, `push_back`, `emplace_back`.
- Every instance in per-frame code is a BLOCKING defect.

### 4. Thread Safety
- Shared mutable state has proper synchronization.
- Per-thread data uses `alignas(64)`.
- Atomics use explicit memory ordering.

### 5. Error Handling
- Every `if (error)` path logs AND returns an error value.
- No exception usage. No RTTI. Flag either as BLOCKING.

### 6. Test Coverage
- Every public function has a test.
- Failure paths are tested.
- Edge cases tested: empty input, max capacity, invalid handles.

### 7. Naming and Style
- Types: PascalCase. Functions: snake_case. Members: m_prefix.
- Constants: kPrefix. Files: snake_case.

## Output

| # | Category | Severity | File:Line | Description | Fix |
|---|----------|----------|-----------|-------------|-----|

Severity:
- **BLOCKING**: Must fix. Architectural violation, crash risk, or data corruption.
- **WARNING**: Should fix. Style violation, missing test, degraded quality.
- **NOTE**: Minor improvement.

No praise. No filler. Only defects and fixes.
