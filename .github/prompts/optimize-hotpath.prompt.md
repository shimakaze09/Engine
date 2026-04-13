---
description: 'Profile-guided optimization of a hot path with cache analysis, allocation audit, and benchmark verification'
---
Analyze and optimize the specified hot-path function for production-grade performance.

## Context

This engine dispatches parallel chunk jobs over SparseSet data.
Known hot paths (verify current state by reading the actual code):
- `physics::step_physics_range` — iterates transforms and rigid bodies
- `render_prep_chunk_job` — iterates WorldTransforms and MeshComponents
- `resolve_collisions` — spatial hash over colliders
- `find_mesh_asset_slot` in asset_database.cpp — O(n) linear scan called per entity per frame (KNOWN BUG)

## Analysis Framework (Apply ALL, report findings for EACH)

### 1. Cache Pressure
- Which structs are touched per loop iteration?
- Estimated cache lines consumed per entity (struct size / 64)?
- Are hot fields scattered across cold data (AOS problem)?
- Would SOA or hot/cold split reduce cache footprint?

### 2. Algorithmic Complexity
- Any O(n) or O(n^2) lookups that should be O(1)?
- Any linear searches over arrays that should be hash-map or index lookups?
- Any redundant work that could be hoisted out of the loop?

### 3. Allocation Audit
- Any heap allocation inside the loop (new, malloc, std::vector resize, push_back)?
- Could these use core::frame_allocator() or core::thread_frame_allocator(threadIndex)?
- Any temporary std::string construction?

### 4. Branch Predictability
- Branches inside tight loops that are data-dependent and unpredictable?
- Could data be pre-sorted to make branches predictable?
- Could branchless techniques (conditional moves, arithmetic masks) help?

### 5. False Sharing
- Any atomics or counters in adjacent cache lines accessed by different threads?
- Check for the alignas(64) pattern used in ThreadStats in job_system.cpp.
- Any shared mutable state between job chunks?

### 6. Job Granularity
- Is kChunkSize appropriate for the work per iteration?
- Too small = scheduling overhead dominates. Too large = load imbalance.
- Measure: items_per_chunk * cycles_per_item should be ~10K-100K cycles.

### 7. SIMD / Vectorization Opportunity
- Any Vec3/Vec4 math that could benefit from explicit SIMD or compiler-friendly layout?
- Are loop bounds known at compile time (enables unrolling)?

## Output Requirements

1. **Findings table**: One row per analysis category, severity (critical/moderate/low), description.
2. **Rewritten function**: Complete replacement with inline comments explaining each change.
3. **Preserved behavior**: The rewritten function must be a drop-in replacement. Same signature, same semantics.
4. **Benchmark expectation**: State the expected speedup rationale (e.g., "reduces cache lines from 4 to 1 per entity").

## Rules

- Do NOT guess at performance. Read the actual struct sizes and data access patterns.
- Do NOT optimize cold paths. If this function runs < 1000 times per frame, it is not hot.
- Do NOT change public API signatures without explicit approval.
- Do NOT introduce platform-specific intrinsics unless gated behind a platform macro.
- All changes must remain noexcept with no heap allocation.
