---
description: "Use when editing CMake files, module interfaces, or cross-module dependencies. Covers allowed dependency directions, architecture boundaries, and link-time enforcement."
name: "Module Boundaries"
applyTo: "CMakeLists.txt, **/CMakeLists.txt"
---
# Module Boundaries

## Dependency DAG (Strictly Enforced)

```
core (zero dependencies)
  ↓
math (depends on: core)
  ↓
physics     (depends on: core, math)
scripting   (depends on: core, math)
renderer    (depends on: core, math)
audio       (depends on: core, math)
  ↓
runtime (depends on: core, math, physics, scripting, renderer, audio)
  ↓
editor (depends on: core, math, runtime, renderer)
  ↓
app (depends on: everything)
```

## Absolute Rules

- Dependencies flow DOWNWARD ONLY. No upward references. No sideways references.
- `core` has ZERO dependencies on any other engine module.
- `math` depends ONLY on `core`.
- `physics`, `scripting`, `renderer`, `audio` depend on `core` and `math` only. They NEVER depend on each other.
- `runtime` may depend on all middle-layer modules. It is the integration layer.
- `editor` may depend on `runtime` and below. It never depends on `app`.
- `app` is the top. It depends on everything.
- Introducing an upward or sideways dependency is a BLOCKING DEFECT. Not a warning, not a TODO — a defect that must be fixed before merge.

## CMake Enforcement

- Every module has its own CMakeLists.txt with explicit `target_link_libraries`.
- When adding a new dependency, verify it does not create a cycle by tracing the full path.
- Use `PRIVATE` linking for implementation dependencies. Use `PUBLIC` only when the dependency appears in public headers.
- After any dependency change: rebuild from clean and run full test suite.

## Interface Discipline

- Keep interfaces minimal. Every public function must justify its existence.
- No god objects. No "Manager" classes that own everything.
- No hidden side effects. If a function mutates global state, its name and documentation must say so.
- Document hot-path complexity, memory ownership, and thread-safety for every major API.
- Prefer free functions in namespaces over classes with only static methods.

## Cross-Module Communication

- Event bus (core) is the approved mechanism for decoupled communication.
- Direct function calls across module boundaries must go through public API headers.
- Never pass internal handles, raw pointers to internals, or implementation types across boundaries.
- Entity indices (uint32_t) are the universal cross-module handle.

## Legacy Violation Rule

- If you encounter an existing dependency cycle, do NOT expand it.
- File a defect, add a TODO comment with the issue reference, and refactor toward correct layering.
- Never accept "it was already broken" as justification for making it worse.
