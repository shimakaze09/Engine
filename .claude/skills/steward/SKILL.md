---
name: steward
description: >
  How to drive an Engine pull request to a mergeable state: what to do on
  a CI failure, a review comment, or a merge conflict, in what order, and
  what never counts as done. Load it when handling any pull request event,
  when watching or babysitting a pull request, when CI goes red, when a
  reviewer or review bot comments, and before selecting which piece of work
  to take next.
---

# Stewarding a pull request

A pull request you opened is yours to drive to green. Red CI or a merge
conflict is work now, whatever the review state — a red or conflicted head
is never "waiting on review".

## Never end an event having done nothing

On a pull request you opened, each event ends in one of exactly two
states: a pushed fix, or a comment naming precisely what is failing and
why it is not yours. There is no third option, and replying to your user
is not a stopping point.

Two things are always safe to skip: an event echoing your own comment, and
an event duplicating one you already handled.

## Order of work

1. **Merge conflict.** Merge the base branch in and resolve it. Regenerate
   lockfiles and generated files with the repo's tooling, never by hand.
   Never rewrite history on a branch you did not create — no rebase,
   amend or force-push. Ask only when both sides changed the same logic
   and either choice loses behavior.

2. **CI red.** First rule out a failure that is not this change's: an
   error naming a subsystem the diff does not touch that reproduces
   identically once, or a check red on the base branch too. If a fix for
   it exists anywhere, port it here now and push — waiting for another
   pull request to merge is still waiting. Standing down on such a failure
   is never silent: one comment naming the check, why it is not this
   change's, and what you ported.

   Everything else is yours to root-cause.

   **"Flake" is not a root cause.** Re-run a job only to confirm the case
   above, or when it died before any test body ran. At most once. A second
   failure is real.

   **Never** skip, disable or quarantine a test to get green. **Never**
   push an empty commit or close and reopen to kick CI.

3. **Review comments.** Implement and push small, local asks — nits,
   renames, an added test, a one-function refactor — and lint-bot fixes.
   A larger ask (multi-file refactor, API change, open-ended design
   feedback) on a pull request you did not open gets a reply with your
   proposal; the author decides. Can't tell if it is small? Treat it as
   large.

   A review bot's finding is a bug report: verify it and fix it. Repeated
   findings across your pushes mean fix the root cause, not stop.

## A failing test is never normalized

If a test fails for an environment reason, fix it or make it skip with a
stated reason, in this change. Never carry a known failure forward as a
sentence in a pull request body. Once "N-1 of N pass, and the one failure
also fails on main" becomes template text, the suite has stopped being a
signal and the next real failure will be invisible.

## Before you push

One validated push beats three speculative ones.

- Run the repo's fast checks directly (see the `verify` skill for the
  tiers that apply to what you touched).
- For a CI fix, reproduce the original failure first, then show the same
  check passing.
- Re-read your own diff adversarially: what would make CI reject this?
- Keep the fix minimal — what the failure or comment needs, no more. Do
  not widen the pull request on your own.

A push that would reset an approval is an accepted cost of getting to
green; it never justifies holding a fix.

## Done

A pull request is done when CI is green on the current head, there is no
merge conflict, and no review thread is waiting on you. Until then keep it
watched. Stop the moment the author or your user says stop.
