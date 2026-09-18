# 0005 — Declarations are documented by complexity

**Date:** 2026-09-09.

## Context

A blanket "document every declaration" rule produced tautologies — a
comment restating the function name above every private helper — which cost
attention and drifted out of date without ever carrying information.

## Decision

Documentation obligation scales with what the reader cannot infer:

- Public API in `include/` headers documents purpose, ownership, failure
  behavior and threading.
- Private and self-evident declarations carry no comment.
- Every file carries a real file-level purpose comment, mechanically
  enforced.
- Body comments are reserved for non-obvious invariants, ordering, units,
  ownership or external constraints, and explain why rather than what.

## Consequences

- Filler patterns are rejected outright by the comment quality gate rather
  than allowlisted.
- The standard itself lives in the `comment` skill, loaded when comments
  are being written, rather than as a standing document. See
  [0009](0009-net-negative-documentation.md).
