---
description: "Use when planning or implementing production_engine_milestones.md batches. Enforces milestone-by-milestone delivery, dependency order, completion standard, and definition-of-done validation."
name: "Milestone Execution Protocol"
applyTo: "**/*.h, **/*.hpp, **/*.cpp, **/*.c, **/CMakeLists.txt, tests/**, .github/prompts/*.prompt.md"
---
# Milestone Execution Protocol

## Cardinal Rule

NEVER mark a phased todo item as complete unless the feature is fully production-ready. "Basic version works" is NOT complete. "Infrastructure exists" is NOT complete. The feature must be shippable by a professional game studio without modification.

## Delivery Model

- Implement exactly one milestone batch at a time unless the user explicitly asks for a parallel lane.
- Respect milestone dependencies from production_engine_milestones.md BEFORE starting.
- If a dependency is incomplete, STOP. Report the blocker. Do not start.
- Keep change scope bounded to the selected milestone's includes and exit criteria.

## Atomic Task ID Format

The milestones doc uses a 5-level hierarchy. Reference leaf-level IDs in branch names and commit messages:
- **Phase**: P1, P2, P3
- **Milestone**: P1-M1, P1-M2, ..., P3-M6
- **Sub-milestone**: P1-M1-A, P1-M1-B, ...
- **Work Package**: P1-M1-A1, P1-M1-A2, ...
- **Atomic Task**: P1-M1-A1a, P1-M1-A1b, ... (leaf level — single-session deliverable)

Each leaf task has `[files: ...]` and optional `[test: ...]` annotations in the milestones doc.

## Dependency Verification

Before starting any milestone, verify:
1. All listed prerequisite milestones are complete.
2. "Complete" means all exit criteria met, not just "partially done."
3. If unsure, audit the prerequisite milestone's actual code before proceeding.

## Vertical Slice Requirement

- Prefer end-to-end slices over isolated subsystem edits.
- A completed milestone must include runtime, data flow, and tooling touchpoints.
- A milestone that compiles but cannot be exercised by tests or a repro scene is NOT complete.
- A milestone that works in C++ but has no Lua/editor exposure (when required) is NOT complete.

## Completion Standard (7-Point Gate)

A phased todo item may ONLY be marked [x] when ALL of the following are true:

1. **Correct functionality**: Works in ALL documented use cases, not just the happy path.
2. **Edge case handling**: Error states, boundary conditions, and failure modes are handled.
3. **Literal match**: Implementation matches the description literally. "LRU eviction" = actual LRU. "Async streaming" = actual async. "DAP debugger" = actual DAP protocol.
4. **Test coverage**: Tests exercise the feature AND its failure modes.
5. **Surface exposure**: Feature is usable from its intended surface (editor, Lua, C++ API).
6. **Production scale**: Performance is acceptable at production scale, not just "works with 3 entities."
7. **Ship quality**: A professional game studio could ship with this feature unmodified.

If ANY criterion is not met, the item stays [ ]. No exceptions. No "close enough."

## Regression Discipline

- Protect hot paths from hidden allocations and O(n^2) behavior.
- Prefer feature flags for risky rendering, platform, or networking features.
- Keep fallback paths available when introducing expensive features.
- Run the full test suite after every milestone completion.

## Milestone Bookkeeping

- Reference milestone IDs in plans, branch names, and descriptions (e.g., P1-M4).
- On completion, summarize: what exit criteria were met, what tests were added, what remains.
- Update production_engine_phased_todo.md checkboxes ONLY after passing the completion standard.

## Anti-Patterns (Blocking Defects)

- Marking items [x] based on partial implementation.
- Claiming "infrastructure exists" as completion for "feature works."
- Starting a milestone before its dependencies are verified complete.
- Skipping test coverage because "it works manually."
- Accepting "good enough for now" as production quality.
