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

Most defects in this engine are not independent. They are one missing
primitive showing symptoms at every site that had to invent its own. Six
hand-written generational handle tables produce stale-handle defects six
times; twenty-nine independent capacity constants produce a different
behavior at capacity twenty-nine times.

Fixing one site leaves the other sites broken **and** adds a local
workaround that the eventual consolidation must migrate. Instance fixes on
a duplicated primitive increase the work remaining. Consolidate instead.

## When this is the right change

Use this procedure when a defect, a review note, or a finding is one
instance of a concept implemented more than once. Signals:

- The same magic number, bit layout or loop appears in two files.
- Two files differ only by a type name (diff them with the names
  normalized — if they match, they are one primitive).
- A finding's text would apply verbatim to another file.
- The same event (capacity reached, input malformed, handle stale) has
  different behavior in different modules.

Do not use it for a genuinely local bug in one place.

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
6. **Add the gate that prevents copy N+1.** A mechanical check that fails
   on a new hand-written implementation. Without it the copies come back,
   and the next agent has no way to know the primitive exists. If no
   mechanical check is possible, say so explicitly and name what a
   reviewer must look for.

## Evidence that closes it

A consolidation is **not** closed by a red-on-base regression. It is
usually behavior-preserving, so demanding one pushes the work back toward
instance fixes. Its evidence is:

- **Net deletion.** Report lines added and removed. A consolidation whose
  net line count is positive needs an explanation.
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

## Serialization

A consolidation touches many files by nature, so it cannot be scheduled
alongside parallel work that touches the same files. Run it alone: one
consolidation at a time, holding the default branch, with no other
concurrent change in the same modules.

Never defer a consolidation because it overlaps other in-flight work.
Overlap is a scheduling fact, not a reason to substitute an instance fix.
Pause the parallel work instead.
