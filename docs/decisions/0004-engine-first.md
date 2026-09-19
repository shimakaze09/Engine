# 0004 — The engine is the product; templates are fixtures

**Date:** 2026-08-25.

## Context

Feature work was being driven by what the bundled starter template needed
next, which pulled the roadmap toward content and onboarding while the
engine's own foundations went unfinished. The game is not the product.

## Decision

The engine is the product. The starter templates and sample content are
demoted from deliverables to integration and test fixtures. Template and
script content bugs are frozen: they are fixed only when the root cause is
an engine defect, and then the fix lands in the engine with a
production-path regression, never as a content or script workaround.

Feature work driven by template needs — runtime UI, additional templates,
onboarding — is suspended. The hour-test north star is deferred, not
deleted.

## Consequences

- The active queue is engine hardening: boundary and layering cleanup,
  foundations, and the tech-debt ladder.
- **The templates stay in the tree and stay runnable.** They exercise the
  whole stack at once, and a creation loop nobody runs is a creation loop
  nobody has verified. Demoting them to fixtures means they stop dictating
  priorities — not that they stop being exercised.
- This decision removed the only forcing function that bounded the work
  queue. See [0007](0007-defect-budget.md), which restores one.
