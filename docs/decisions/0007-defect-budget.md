# 0007 — A defect budget replaces open-ended auditing

**Date:** 2026-09-18.

## Context

The open-finding queue was a function of audit frequency, not of defect
injection rate. Batch audits filed dozens of findings in minutes, and any
sufficiently large C++ codebase will yield another batch on the next pass —
indefinitely, on any engine. Treating that output as a backlog to empty
guaranteed the experience of never progressing, regardless of how much was
actually fixed.

The findings also arrived undifferentiated: a feature that does not work at
all sat in the same pile as a comment describing deleted behavior.

Meanwhile the one bounded, self-prioritizing source of defects — running
the engine and using the editor — had been removed from the loop.
Defects that a human would hit within five minutes of opening the editor
were being discovered by code reading instead.

## Decision

1. **Budget, not zero.** P0 and P1 stay at zero. P2 stays under an
   owner-set ceiling. P3 is unbounded and not audited.
2. **No broad audit campaign while P0/P1 are non-empty.** Audits produce
   volume, not information, once the systemic classes are already known.
3. **Running the product is a required source of defects.** The acceptance
   demo is executed, by a human, as a gate — the defects it surfaces enter
   the queue ahead of anything found by reading code.
4. **A P3 never justifies a change of its own.** Fix it inside a change
   already touching those files, or close it as `wont-fix`.

## Consequences

- The tracker stops being a measure of quality and becomes a work queue.
- Closing a stale P3 is progress, not a concession.
- Severity tiers now carry different evidence burdens; see the
  `close-finding` skill.
- Audits keep their value for a *specific* question ("is this subsystem's
  capacity handling correct?"). They lose it as a recurring sweep.
