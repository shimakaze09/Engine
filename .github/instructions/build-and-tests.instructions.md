---
description: "Use when editing CMake files, tests, CI configuration, or build automation. Covers canonical commands, quality bar, test expectations, and CI pipeline rules."
name: "Build and Test Rules"
applyTo: "CMakeLists.txt, **/CMakeLists.txt, tests/**"
---
# Build and Test Rules

Every rule below is a hard gate. Violations are blocking defects.

## Canonical Build Commands

```powershell
# Standard build
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure

# Static analysis (requires cppcheck)
cmake --build build --target analysis

# Sanitizer build (GCC/Clang only)
cmake -S . -B build-san -DENGINE_SANITIZERS=ON
cmake --build build-san
ctest --test-dir build-san --output-on-failure
```

## Quality Bar

- **Zero warnings.** -Werror (GCC/Clang) and /WX (MSVC) stay enabled. Fix warnings. Never suppress.
- Every new source file must compile without warnings on GCC, Clang, and MSVC.
- Every new build target must have CTest coverage.
- Builds must be deterministic for identical inputs and compiler versions.

## Test Organization

```
tests/
  unit/           — Fast, isolated, no GL context. One file per module feature.
  integration/    — Cross-module tests, vertical slices, scheduler stress.
  smoke/          — Minimal startup/shutdown tests.
  benchmark/      — Performance regression tests.
```

## Test Rules

### When to add tests
- Every behavior change in: math, ECS/runtime, physics, renderer/mesh loading, reflection, scripting.
- Every new component type, Lua binding, render pass, or subsystem.
- Every bug fix (regression test preventing recurrence).

### Hard requirements
- Tests requiring a GL context must be excluded from headless CTest runs. Document this in the file header.
- Tests must be deterministic. No random seeds without explicit seed logging.
- Tests must clean up after themselves. No leaked state between test functions.
- Stack-allocating large objects (World, command buffers) in tests is forbidden on Windows. Use heap allocation with `new (std::nothrow)` and null checks.

### Naming
- File: `tests/unit/<module>_<feature>_test.cpp`
- Function: `bool test_<behavior_under_test>() noexcept`
- Names describe behavior, not function names.

## CI Pipeline Rules

- Required lanes: Windows MSVC, Linux GCC, Linux Clang.
- Each lane: build → test → static analysis.
- Sanitizer lane: GCC/Clang with ASAN + UBSAN.
- All lanes must pass before merge. No exceptions.
- Performance regression gate: ECS stress test must stay under threshold.
