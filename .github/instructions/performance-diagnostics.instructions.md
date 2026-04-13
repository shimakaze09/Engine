---
description: "Use when editing profiling, diagnostics, tracing, benchmark gates, and hot-path performance-sensitive code. Covers measurement-first optimization, regression control, and hot-path constraints."
name: "Performance and Diagnostics Rules"
applyTo: "core/**, runtime/**, renderer/**, physics/**, audio/**, tests/**"
---
# Performance and Diagnostics Rules

Every rule below is a hard gate. Violations are blocking defects.

## Measurement-First Optimization

- Collect baseline numbers BEFORE changing hot-path code. No exceptions.
- Report: CPU time (ns), memory footprint (bytes), frame budget impact (%).
- Include GPU metrics (draw calls, triangle count, texture memory) where applicable.
- "I think it's faster" is not evidence. Measure or do not claim.

## Hot-Path Constraints

### Zero Allocation
- No heap allocations in per-frame code. Not one. Not "just this once."
- Use frame_allocator, pool_allocator, or fixed storage.
- If allocation is unavoidable, it happens at initialization, never per-frame.

### Data Locality
- SOA over AOS in tight loops.
- Minimize pointer chasing. Prefer index-based access into flat arrays.
- SparseSet dense arrays are the canonical hot-path storage.
- Separate hot fields from cold fields. If a loop touches 3 of 8 fields, split the struct.

### O(1) Lookups
- No linear scans in per-frame code. Hash tables, direct index, or SparseSet lookup.

### Branch Predictability
- Avoid unpredictable branches in dense iteration loops.
- Sort data by type/category before iteration where possible.
- Use branchless techniques for simple conditionals in tight loops.

### False Sharing
- Per-thread counters and stats: `alignas(64)`. Always.
- Atomics in hot paths must be justified with a comment explaining why lock-free is necessary.

### Job Granularity
- kChunkSize determines parallelism. Profile different values per workload. Document the choice.
- Target: items_per_chunk × cycles_per_item ≈ 10K–100K cycles.

## Diagnostics

- New subsystems must expose: initialization time, per-frame time, memory footprint.
- Profiling instrumentation is debug-build only. Must not affect shipping builds.
- Use conditional compilation (`#ifndef NDEBUG` or equivalent) for debug-only code.

## Regression Gates

- Benchmark thresholds must be explicit and versioned in test code.
- When CI exists: benchmark regression blocks merge.
- After touching a known bottleneck, add or update the relevant benchmark test.
