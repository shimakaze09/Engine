# 0020 — Every issue is classified blocker, deferred or feature, and worked in that order

**Date:** 2026-09-25. Owner decision. Supersedes point 2 and the
stand-alone P3 rule in point 3 of
[0014](0014-severity-is-impact.md).

## Context

Two weeks of fixing closed 155 issues. About two thirds of them came from
code-reading audits, not from using the engine. Nothing on the tracker
said which open items a person using the engine would actually meet.
Every finding therefore read as equally urgent, each audit refilled the
queue, and the work felt like a spiral that never converged. The
regressions that fixes did cause clustered in the layers CI could not
see.

## Decision

1. **Every open issue carries exactly one class label:**
   - `class:blocker`: data loss or corruption, a crash, a leak or
     unbounded growth in normal operation, a regression, or any defect a
     normal user of a claimed workflow hits, whoever found it;
   - `class:deferred`: real, but off the normal path;
   - `class:feature`: a capability not yet claimed, UI/UX included.
2. **Work order.**
   - Blockers go first: by severity, then foundation layer first.
   - Deferred items ride only inside a change already touching their
     files, and a deferred item observed in normal use becomes a blocker.
   - Features start when no blocker is open.
3. **Audits continue.** A finding is classified when it is filed. A new
   blocker takes its place by severity and never jumps the queue for being
   new.
4. **Where problems come from is recorded.** One `found:*` label per issue
   (`use`, `audit` or `regression`). The count of `found:regression` is
   the health signal reported to the owner.
5. **A demo is an instrument, not a goal.** Sample scenes and slice
   playthroughs exist to catch regressions, and run in CI wherever CI can
   run them.

The rule itself lives in CLAUDE.md. The issue forms ask for the class, and
`.github/workflows/issue-triage.yml` labels an unclassified issue
`needs-triage`.
