# 0006 — Foundation work runs serially, and overlap never selects work

**Date:** 2026-09-18.

## Context

Running many concurrent agent sessions against one repository made
file-disjointness an implicit selection criterion: a change was taken
because no other in-flight change touched its files, and a correct fix was
deferred because it collided with an open pull request's files.

The effect was structural. A fix that spans modules — merging duplicate
implementations of one primitive, unifying a capacity policy, introducing a
failure vocabulary — collides with concurrent work *by construction*, so it
was never selected. Months of commits closed instance-level findings while
the foundations that generated those findings went untouched, and each
instance fix added a local workaround that the eventual consolidation would
have to migrate. Fixes were increasing the work remaining.

A shrinking shared allowlist made this worse: it required every fix to edit
one common file, so every change conflicted with every other change.

## Decision

1. **Overlap with in-flight work is never a reason to select, defer or
   substitute a change.** Select by severity and by which layer owns the
   defect.
2. **Foundation and consolidation changes run alone**, holding the default
   branch, with concurrent work in the same modules paused.
3. **Shared shrinking allowlists are not a gate mechanism** for open-ended
   classes of finding. They work only where the set is finite and closed
   (dependency pins qualified; comment prose did not).
4. Parallel throughput is subordinate to structural progress. One
   consolidation that deletes five implementations outranks twenty
   instance fixes.

## Consequences

- Commit volume will fall. That is the intended trade.
- A consolidation's evidence is net deletion and a recurrence gate, not a
  red-on-base regression — demanding one pushes work back toward instance
  fixes. See the `consolidate-primitive` skill.
- Agents must be told this explicitly, because the incentive to pick the
  non-overlapping task is structural and does not announce itself.
