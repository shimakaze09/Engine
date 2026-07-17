---
name: next-finding
description: 查看 Engine REVIEW_FINDINGS.md 修复进度，选出下一个要修的问题并按流程开工。
---

# Pick and execute the next review finding

1. Read `REVIEW_FINDINGS.md`. Report counts: open `[ ]`, in-progress `[~]`,
   fixed `[x]` per section (P0/P1/S/A/C).

2. Selection order (do not skip ahead without user approval):
   P0 bugs (B*) → P1 perf (P*) → duplication (S*) → architecture (A*) → comments (C1).
   Within a tier, prefer: smallest blast radius first; items that unblock other
   items (e.g. S1 fixes B6; S4 coordinates with P2/P5) get priority over
   standalone ones of equal size.

3. Before editing:
   - Mark the item `[~]` in REVIEW_FINDINGS.md.
   - Read the affected files fully (not just the cited lines).
   - Check for existing tests covering the area; note gaps.

4. Fix rules:
   - One finding = one focused commit (or a small series with one concern each).
   - Follow AGENTS.md constraints (no exceptions/RTTI, noexcept, no hot-path
     heap alloc, downward deps only).
   - Shared utilities go in `core`; migrate ALL duplicate call sites in the
     same series.
   - Write the regression test alongside the fix, not after.

5. Verify with the `verify-engine` skill, then mark `[x]` with a one-line note
   of how it was verified, and commit (message references the finding ID,
   e.g. "Fix B1: mask job handle generation to encoded width").
