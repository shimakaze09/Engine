# 0014 — Severity is impact; counts are signals; consolidation needs identity

**Date:** 2026-09-20. Owner decision, recorded after an external review of
the correction program. Supersedes the flow counts in
[0012](0012-defect-budget-is-a-flow-rule.md), the P3 `wont-fix` rule in
[0007](0007-defect-budget.md), and narrows the evidence rule in
[0006](0006-foundation-work-runs-serially.md); amends
[0009](0009-net-negative-documentation.md).

## Decision

1. **Severity is impact.** P0: data loss or corruption, unrecoverable
   project damage, a critical crash. P1: a major correctness or stability
   failure, or an important workflow that cannot reliably complete. P2: a
   user-visible functional defect, or a workflow materially obstructed but
   recoverable. P3: low-impact debt, hygiene, cosmetics. Visibility alone
   sets no severity; a reachable functional defect is not dismissed for
   being hard to reach.
2. *(Superseded by [0020](0020-issues-are-classified-before-they-are-worked.md):
   the class label, not a count, decides the work, and the reported
   signal is the regression count.)* **P1 and P2 counts are health
   signals, not quotas.** A month-over-month
   count is reported and triaged, never the optimization target. P0 stays
   stop-the-line; a consolidation is allowed for a P0 only as its
   owning-layer fix.
3. *(Superseded by [0020](0020-issues-are-classified-before-they-are-worked.md):
   a P3 is `class:deferred` and rides inside a change already touching
   its files; age still never closes it.)* **P3 stands alone only for
   bounded structural value**: deleting a
   substantial obsolete API or dead code, unblocking a migration, removing
   recurring noise, or a cleanup cheaper than carrying. The
   one-month `wont-fix` rule is deleted; age triggers triage or an icebox,
   never a close.
4. **Consolidation requires semantic identity.** Similar code is a signal
   to investigate. Two implementations merge only when owner, lifetime,
   identity, threading, failure and persistence semantics are compatible;
   shared mechanics never force shared policy. Net line count is evidence,
   not the objective; replaced implementations are deleted; a recurrence
   gate prefers the compiler, a type, a schema or an existing gate over a
   bespoke checker.
5. **Foundation serialization stays conservative for now**: foundation and
   consolidation changes run alone. A later record may narrow this to
   "overlapping ownership or migration domains serialize; unrelated
   bounded work proceeds concurrently". File-disjointness never selects
   work.
6. **Documentation.** The net-negative rule and the skill cap of 0009 are
   temporary through the current correction pass. Permanent: one fact has
   one owner, no prose mirrors, no history in always-loaded contracts,
   every document has a unique purpose, stale prose is deleted, and a
   genuinely new subsystem may add a document.
