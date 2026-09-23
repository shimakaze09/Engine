# 0012 — The defect budget is a flow rule, not a ceiling

**Date:** 2026-09-18. Supersedes the "owner-set ceiling" placeholder in
[0007](0007-defect-budget.md). Decided by the maintainer agent on the
owner's delegation; the owner may override the numbers, but the reasoning
against a hard count should be read first.

## Context

Decision 0007 left a numeric ceiling on P2 to be set later. Counting the
tracker before setting it produced the actual distribution:

| | Open |
| --- | --- |
| P0 | 4 |
| P1 | 27 |
| P2 | 38 |
| P3 | 26 |
| P4 | ~46 |

Three things follow immediately.

First, 0007's "P0 and P1 stay at zero" was not a description of a
near-term state — it is violated 31 times today. A rule that is false on
the day it is written teaches readers to skim rules.

Second, a P2 ceiling is the wrong dial while P0 and P1 are non-empty: it
would not change which work gets picked.

Third, and most important: **a hard count creates pressure to
under-classify.** The 2026-09-16 audit filed four acceptance-breaking
defects and five violations of stated invariants as "Severity: Low",
because ten unrelated rows inherited one batch label. Attaching
consequences to a severity count reproduces exactly that incentive, this
time deliberately. A ceiling would make the defect we just spent a triage
pass removing into the rational thing to do.

## Decision

No hard ceiling on any severity. The budget is expressed as flow, plus one
promotion rule that does the "miss nothing" work.

**Flow**

- **P0 is stop-the-line.** While any P0 is open, it is the work. No feature
  work, no consolidation, no audit.
- **P1 may not grow.** The count at the end of a month must be at or below
  the count at its start. Baseline on this date: **27**.
- **P2 may not grow.** Same rule. Baseline on this date: **38**.
  *(Both superseded by [0014](0014-severity-is-impact.md): the counts are
  health signals, reported and triaged, never an optimization target.)*
- **P3 is unbounded and not audited**, as 0007 states.

**Promotion — this is the half that catches what a label missed**

- **Observation outranks classification.** Anything seen happening during
  real use of the editor or a build is at least P2, whatever it was filed
  as and whoever filed it. Reality is a better classifier than a reviewer's
  guess about reachability, and it costs nothing to apply.
- **Severity is assigned per row, never per batch.** A grouped issue whose
  rows differ in severity is split before it is filed.
- **A budget never justifies a downgrade.** If the flow rule would be
  breached, the response is to fix something or to let the month close over
  budget and say so. Relabelling to stay inside a number is prohibited, and
  a severity that moves down needs the same evidence as any other contract
  change.

## Rationale

The flow rule serves steady pace: it never forces a drain sprint, only a
requirement that the backlog not outrun the fixing. A count would either
sit above today's number — blessing an unhealthy state — or below it,
forcing a stop that would be gamed.

"Miss nothing" and "steady pace" cannot both be hard constraints, because
the finding rate from reading code is unbounded — any sufficiently large
C++ codebase yields another batch on the next pass, indefinitely. What is
achievable is *missing nothing that matters*, and the mechanism for that is
the promotion rule, not the size of the queue. Severity assigned per row,
plus observation overriding classification, would have caught all eight
mislabelled batches; no ceiling would have caught any of them.

## Consequences

- The immediate work is the four P0s. Nothing else is in budget until they
  are closed.
- The P1 baseline of 27 includes the foundation parents, which are large.
  Expect the month-over-month rule to bind there first; that is the
  intended pressure.
- A month that closes over budget is reported, not hidden, and the next
  month's baseline does not ratchet up to absorb it — the baseline stays at
  the lower of the two.
  *(Superseded by [0014](0014-severity-is-impact.md) with the budgets
  above: there is no baseline to hold.)*
- These baselines are re-read from the tracker, not maintained by hand in
  this document.
