---
description: 'Plan and execute any production milestone by ID with strict completion verification'
---
Plan and execute milestone ${input:milestoneId} from production_engine_milestones.md.

Inputs:
- Milestone ID: ${input:milestoneId}
- Optional parallel lane request: ${input:parallelLane}
- Optional constraints: ${input:constraints}

## Required Process (No Shortcuts)

### Step 1: Dependency Verification
1. Locate the milestone section in production_engine_milestones.md.
2. List ALL prerequisite milestones.
3. For EACH prerequisite: read the actual code and verify it meets the 7-point completion standard.
4. If ANY prerequisite is incomplete: STOP. Report exactly what is missing. Do not proceed.

### Step 2: Scope Mapping
1. Map milestone scope to production_engine_phased_todo.md checkboxes.
2. List every checkbox this milestone should address.
3. For each checkbox, identify: which module(s) are affected, what tests are needed, what Lua/editor exposure is required.

### Step 3: Implementation Planning
1. Propose implementation tasks in dependency order.
2. For each task: identify files to create/modify, expected test coverage, risk level.
3. Identify hot-path impacts and measurement requirements.
4. Get user approval on the plan before implementing.

### Step 4: Implementation
1. Implement one task at a time.
2. After each task: build, run tests, verify no regressions.
3. Add tests for each feature AND its failure modes.
4. Measure performance impact on hot paths.

### Step 5: Verification
1. Build the full project: cmake -S . -B build && cmake --build build
2. Run all tests: ctest --test-dir build --output-on-failure
3. For each checkbox in scope: verify against the 7-point completion standard.
4. Only mark checkboxes [x] if ALL 7 criteria are met.

### Step 6: Report
1. List completed exit criteria with evidence.
2. List any exit criteria NOT met with explanation.
3. List new tests added.
4. List performance measurements (before/after if hot-path work).
5. List remaining work if milestone is partially complete.

## 7-Point Completion Standard (Reminder)

A checkbox may ONLY be marked [x] when ALL are true:
1. Works correctly in all documented use cases.
2. Edge cases handled.
3. Implementation matches description literally.
4. Tests exist for feature and failure modes.
5. Usable from intended surface (editor, Lua, C++ API).
6. Acceptable at production scale.
7. A professional game studio could ship with it.

## Constraints
- Do not start dependent milestone work unless explicitly requested.
- Keep risky additions behind feature flags.
- Do not introduce upward module dependencies.
- Do not mark items complete that are not verified complete.
