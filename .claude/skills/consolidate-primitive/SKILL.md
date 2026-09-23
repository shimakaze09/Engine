---
name: consolidate-primitive
description: >
  How to merge several hand-written copies of one primitive into a single
  owned implementation, and the evidence that closes such a change. Load it
  whenever you notice the same concept implemented more than once — a
  handle or slot table, a hash, a string copy, a ring, a file reader, a
  path check, an argument validator, a capacity policy — and whenever a
  task asks to deduplicate, unify, extract into core, or remove a
  duplicate implementation. Load it before fixing one instance of a defect
  that exists in several copies of the same code.
---

# Consolidating a primitive

Many defects here are one missing primitive showing symptoms at every
site that invented its own. Fixing one site leaves the others broken and
adds a local workaround the eventual consolidation must migrate.

## Similar is a signal; identical is the test

Similar code is a reason to investigate, not a verdict. Signals: the same
magic number, bit layout or loop in two files; two files that differ only
by a type name; a finding whose text applies verbatim elsewhere; one event
(capacity reached, input malformed, handle stale) with different behavior
in different modules.

Two implementations are one primitive only when their owner, lifetime,
identity, threading, failure and persistence semantics are compatible.
Shared mechanics never force shared policy: two tables with the same
shape but different lifetimes stay two tables. A local bug in one place
is fixed in place.

## Procedure

1. **Inventory first, exhaustively.** Find every implementation before
   designing anything. Grep for the shared vocabulary, the magic numbers,
   and the structural pattern. Write the list down in the change
   description. An inventory that misses a site produces a seventh copy.
2. **Pick the owner.** Shared primitives live in `core`; the generic asset
   layer's own vocabulary lives in `content`. Never in a mid-tier
   subsystem — siblings cannot depend on each other.
3. **Design for the union of real requirements.** Take the widest bit
   layout, the strictest validation, the explicit failure result. Where
   two copies disagree on behavior, the correct behavior wins; record the
   disagreement and what you chose, because one of the two call sites is
   changing behavior and needs a test.
4. **Migrate every call site in the same series.** If a full migration is
   genuinely too broad for one change, migrate a coherent subset, and the
   remaining sites stay listed in an open linked issue naming each one.
   Never leave the migration implicit.
5. **Delete the originals.** This is the step that makes the change worth
   doing. A consolidation that adds a primitive and leaves the copies in
   place has made the codebase worse.
6. **Add the gate that prevents copy N+1.** Prefer the compiler, a type,
   a schema or a rule in `tools/check_duplicate_primitives.py` over a
   bespoke checker; a new script is
   the last resort. If no mechanical check is possible, say so and name
   what a reviewer must look for.

## Evidence that closes it

A consolidation is **not** closed by a red-on-base regression. It is
usually behavior-preserving, so demanding one pushes the work back toward
instance fixes. Its evidence is:

- **Net deletion is evidence, not the objective.** Report lines added and
  removed; a positive net needs an explanation, and the replaced
  implementations are deleted, not kept beside the new one.
- **Zero remaining implementations.** State the count found in step 1 and
  the count remaining. The target is zero; anything else names the
  remaining sites and their tracking issue.
- **Every migrated call site's tests pass**, with assertions unchanged
  where behavior was preserved.
- **Behavior changes are called out individually**, each with its own
  test. A call site whose behavior the union design corrects has a
  red-on-base test for that correction.
- **The gate fails on a reintroduced copy.** Show it: add a copy locally,
  watch the gate reject it, revert.
