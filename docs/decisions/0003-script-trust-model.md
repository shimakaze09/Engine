# 0003 — v0.x scripts are author-local and trusted, but VFS-jailed

**Date:** 2026-08-09.

## Context

The scripting sandbox has instruction and memory caps, but the question of
whether a script is *hostile* input was unsettled, and the answer changes
how much validation every binding needs.

## Decision

In v0.x, scripts are author-local and trusted: the person running them
wrote them or chose to run them. Untrusted-content isolation for shared
creations arrives with the web sandbox, not before.

Every script-reachable filesystem path is nonetheless VFS-jailed as
defence in depth: relative, forward slashes, no drive designators, no
`..`.

## Consequences

- A binding does not need to defend against a malicious script, but it
  does need to defend against a *wrong* one: out-of-range indices,
  non-finite floats, stale handles and capacity overruns are ordinary
  input-validation obligations, not security theatre.
- The path jail is unconditional, so its enforcement does not depend on the
  trust model and will not need revisiting when the model changes.
- When shared creations ship, this record is superseded rather than
  quietly reinterpreted.
