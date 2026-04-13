---
description: "Use when editing asset import, cooking, metadata, streaming, and asset database code. Covers deterministic cooking, dependency tracking, runtime loading, and file format rules."
name: "Asset Pipeline Rules"
applyTo: "assets/**, tools/asset_packer/**, runtime/**, editor/**"
---
# Asset Pipeline Rules

Every rule below is a hard gate. Violations are blocking defects.

## Cook Pipeline

- Cook inputs: source file + platform profile + import settings.
- Cook outputs must be deterministic for identical inputs. Byte-identical rebuilds.
- Sidecar files: `.meta.json` (source hash, format version), `.cookstamp` (dependency hashes).
- Never write to source directories. All cooked output goes to the build asset directory.

## Loading Contract

- Asset load requests from gameplay code must be non-blocking.
- Load requests return a handle. Completion checked via polling or callback.
- Failed loads return explicit error state. Never partial mutable state. Never silent failure.
- Log every load failure with asset path and reason.

## File Format Rules

- Binary formats: document byte layout in a header comment.
- Version every format. Include a magic number and version field in the header.
- Reject unknown versions with an error. Never silently parse unknown data.
- Endianness: store little-endian. Assert platform matches or byte-swap.

## Hot Reload

- Editor detects source file changes via mtime and offers reimport.
- Reimport must not invalidate active world state. Log and degrade on failure.
- When asset B changes, everything depending on B must be flagged for recook.
- Cascading invalidation must terminate. No circular dependencies in the asset graph.

## Identity and Hashing

- Asset identity is path-based with hash as cache key.
- Hash algorithm must be consistent between tool and runtime.
- Collision in hash space is a data corruption bug. Use sufficient bit width.

## Memory

- Every asset cache must have a hard maximum size. No unbounded growth.
- Configure budgets via CVars.
- Document ownership for every allocation: who allocates, who frees, when.
- Zero-ref assets are candidates for eviction. Active refs must never be evicted.
