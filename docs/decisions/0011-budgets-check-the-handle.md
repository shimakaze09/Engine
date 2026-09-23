# 0011 — An input budget is enforced on the handle that is read

**Date:** 2026-09-18. Decided by the maintainer agent on the owner's
delegation; the owner may override.

**Status:** `load_sound` conforms. `play_music` does not yet: it checks
the size by path and then opens the file again by path.

## Context

`play_music` checks a file's size from its metadata and then opens the
resolved OS path separately, so a file that grows between the check and the
open streams past the budget. The question was posed as a choice between
building a bounded VFS for the audio backend and demoting the cap to
advisory.

That was a false binary. The sibling function already solved this: the
audio decode budget bounds the read `load_sound` performs **on the handle
it reads from**, not on a path it looked up earlier.

## Decision

**A budget is checked against the handle the code will actually read, never
against a path or a metadata record consulted beforehand.**

`play_music` opens the file itself and streams from that handle, applying
the cap to it, the way `load_sound` already does. No new VFS layer, and the
cap stays enforced rather than advisory.

## Rationale

- A check on a path and a read on a separately-resolved path are two
  different objects. Any budget expressed that way is advisory whether or
  not it says so.
- The pattern already exists in the same file for the same class of input,
  so this is consistency with a landed fix rather than new machinery.
- Demoting the cap to advisory would leave a stated budget the code does
  not enforce, which is the documentation-diverges-from-behavior pattern
  that decision 0008 exists to stop.
- The v0.x trust model means this is a robustness budget, not a security
  boundary, so a full bounded VFS would be disproportionate. The cheap fix
  is also the correct one here.

## Consequences

- Applies to every budget, not just this one: decoded-PCM caps, file-size
  caps, asset-size caps, and any future quota. A reviewer checks *what
  object was measured*, not whether a check exists.
- Where a budget genuinely cannot be enforced on the read handle, it is
  documented as advisory **and** the reason is recorded here — it does not
  stay silently advisory.
