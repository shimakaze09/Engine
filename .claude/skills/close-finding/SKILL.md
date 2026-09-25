---
name: close-finding
description: >
  The evidence a finding, bug or tech-debt item needs before it can be
  called closed, tiered by severity so a P3 hygiene item does not carry a
  P0's proof burden. Load it when starting work on a tracked issue, when
  writing a pull request that references one, when deciding whether a fix
  is complete, and when judging whether a partial fix may merge. Load it
  before writing any pull request's scope table.
---

# Closing a finding

The proof burden scales with severity. Applying a P0's burden to every
item is why prose cleanups get regression suites while real defects wait,
so match the tier. The tiers are the severity scale in `CLAUDE.md`
(decision 0014); this skill sets only what each tier's close requires.

## P0 / P1 — full contract

- **A red-on-base regression.** It fails on the base revision and passes
  on the fixed one. Record both results and the exact test name. No
  exceptions: without it you have not demonstrated the defect existed.
- **Production entry point.** The test drives the real wiring. A copied
  model of a scheduler, serializer, parser or state machine is
  supplementary and never proves the production path.
- **Boundary coverage** where applicable: zero work, one item, many,
  capacity, one past capacity, malformed input, partial failure,
  cancellation, concurrent access, repeated lifecycle transitions.
- **Every evidence location in the original scope is addressed.** A finding
  listing five sites is not closed by fixing one. A partial fix may merge,
  but the unresolved scope moves to its own linked issue before the
  original closes — never into a pull request body or a closed issue's
  comments.
- **Fault injection** for a data-loss fix: write, flush, sync, close,
  rename, parse, restore, rollback.
- **Explicit happens-before** for a concurrency fix, stated per branch
  including empty ranges, disabled subsystems and submission failure.
  TSAN supports the claim; it does not establish the relation.

## P2 — reduced

- A test that would have caught it, red on base where the change is a
  correctness fix.
- Boundary cases for the specific boundary involved, not the full matrix.
- No new linked issue required unless scope is genuinely left open.

## P3 — fix inline

- A P3 is `class:deferred` unless it is observed in normal use. It is
  fixed inside a change that already touches those files, with the
  existing tests passing: no regression suite, no scope table.
- Age triggers triage, never a close.

## Consolidations are a different shape

A change that merges duplicate implementations of one primitive is closed
by net deletion and a recurrence gate, not by a red-on-base regression.
Use the `consolidate-primitive` skill for its evidence rules.

## Writing the scope table

Only for P0/P1/P2. State, per referenced finding, one of:

- **Fixed** — with the test that proves it.
- **Partially fixed** — with what remains and the linked issue holding it.
- **Deferred** — with why, and the issue.
- **Pre-existing** — present before this change, not addressed here.

Never write "all", "never", "complete", "production-ready" or "closed"
beyond what the tests demonstrate. A reviewer rejects a claim broader than
its evidence, and a contradicting audit reopens it immediately.
