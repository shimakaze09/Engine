# 0009 — Documentation is net-negative

**Date:** 2026-09-18.

## Context

The contributor contract had reached 1,058 lines and 8,556 words, loaded in
full for every session. Over half of it restated information whose
authoritative source was elsewhere: a prose description of what each module
currently does (the code's job) and a roadmap of feature status (the
tracker's job). It carried 51 dates and 81 issue references while
mechanically forbidding both in code comments — the standard did not
satisfy itself. It needed an internal precedence clause to resolve its own
contradictions.

Alongside it, a 1,997-line commenting standard — 59% the size of the entire
core layer's public API — plus three gates and a 212-line shared allowlist,
together 36% of all gate code, policed comment prose. Roughly one commit in
ten was a comment rewording. All of this while shipped features did not
work.

Every one of those additions was individually reasonable. The failure was
that nothing ever came out.

## Decision

1. **Every new document or skill deletes at least as much prose as it
   adds.** Net documentation change is zero or negative.
2. **Skills cap at eight.** To add one, merge or delete another. The cap is
   arbitrary; its purpose is to force the trade-off that was previously
   never made.
3. **Only procedures with a completion criterion become skills.**
   Principles belong in the hard rules; "how to think about X" belongs
   nowhere.
4. **Never build a skill for an activity you want less of.** A skill lowers
   the cost of what it describes, so it is an accelerator aimed at whatever
   it documents. There is deliberately no audit skill.
5. **Each fact has one home.** Rules in `CLAUDE.md`, invariants in
   `docs/architecture.md`, direction in `docs/vision.md`, decisions and
   dates here, open work on the tracker, current behavior in the code. A
   document never mirrors another, and no gate exists to keep two prose
   documents consistent — the mirror is deleted instead.

## Consequences

- Prose quality moves from merge-time gates to authoring-time guidance.
  The shared allowlist and the documentation-mirror gate are deleted; the
  objective checks (file-level comment presence, filler patterns,
  commented-out code, untracked TODOs, terminating error accessors) stay
  mechanical.
- Historical content was deleted, not migrated. Git history holds it.
- Conditional rules load only when relevant, so the always-on contract can
  stay short without losing them.
