# 0008 — No status claim without named evidence

**Date:** 2026-09-18.

## Context

The project maintained a careful status vocabulary — `LANDED` for present
on a date, `VERIFIED` and `PRODUCTION-READY` requiring commit, test
evidence, platform scope and acceptance result. It did not work.

Features were recorded as implemented foundations on the strength of "the
code exists and its unit tests pass". Later audits found, among those
entries, a post-processing stage that rendered every frame and was never
read, shadow types no producer could ever enable, an instanced draw path
that used the wrong matrices for every batch but the last, and shadow
passes consuming the main camera's culled draw list. The precise
vocabulary produced an illusion of precision about something nobody had
measured.

The root cause is that the status label was applied to code, while the
claim it implied was about behavior.

## Decision

1. **Delete the status vocabulary.** No `LANDED`, `VERIFIED` or
   `PRODUCTION-READY` labels anywhere.
2. **A feature is not described as working — in any document, comment,
   pull request or status claim — without a named end-to-end test or a
   dated human observation beside it.** Nothing else counts, and a passing
   unit test is not an end-to-end test.
3. **The roadmap leaves the repository.** Integration state lives on the
   tracker, which was already declared the source of truth for open scope.
   A document that restates the tracker drifts from it.
4. `docs/vision.md` states direction only, never status.

## Consequences

- Feature inventories disappear from documentation. To know whether
  something works, read the test list or run the engine.
- A renderer feature cannot satisfy this rule from CI alone, because no CI
  lane draws a frame. That is a gap in verification, not a reason to relax
  the rule; see the `verify` skill.
  *(Since 2026-09-23 the web lane draws frames on SwiftShader's WebGL2, but
  only fails on errors and checks no image, so the gap stands.)*
- Historical status prose was deleted rather than migrated. Git history
  holds it.
