# 0019 — The simulation owns its time and its randomness

**Date:** 2026-09-22. Owner decision, resolving the two items
[#415](https://github.com/shimakaze09/Engine/issues/415) marked as needing
it.

## Context

The fixed step is correct — one `kFixedDeltaSeconds`, exact for physics,
transforms and animation. What surrounds it is not a contract.

Two gaps were left open because either answer was defensible and the
wrong one is expensive to reverse.

**Randomness.** There is no engine random stream. Lua's `math.random` is
opened unrestricted and seeded from `time(NULL)`, so a script that uses
it produces a different run every time and no two machines agree. Lua's
string-hash seed is also unpinned, so `pairs()` over string keys varies
per process. A deterministic engine whose scripting layer is
nondeterministic is not deterministic; it only looks that way until a
script calls the wrong function.

**Timer cadence.** Timers tick once per rendered frame with the summed
delta, while animation evaluates once per fixed step. So a timer's firing
depends on the frame rate: the same script on a 144 Hz machine and a
30 Hz machine fires callbacks at different simulation times, and a frame
that runs several catch-up steps collapses them into one tick. Coroutine
`wait_frames` counts rendered frames for the same reason.

Both choices were becoming more expensive with every script written
against the current behaviour.

## Decision

1. **`math.random` and `math.randomseed` route to the engine stream.**
   Not removed from the sandbox: a beginner reaching for `math.random`
   should get a working, deterministic random number rather than a nil
   error, which is the same reasoning that put the deterministic scalar
   set behind `math.sin` and friends. `engine.random()`,
   `engine.random_int()` and `engine.set_seed()` are the explicit names;
   `math.random` becomes an alias for the first two. The stream is
   World-owned, seeded at run start and at scene load, and its state is
   part of what a state hash covers.

2. **`luai_makeseed` is pinned to a constant** for `engine_lua`, so
   `pairs()` over string keys enumerates in the same order in every
   process. Table iteration order is not a language guarantee, but it
   *is* observable, and a script that iterates a table to build simulation
   state must not depend on the process.

3. **Timers tick once per fixed step.** Callbacks are queued as they come
   due and dispatched once per frame, so a callback still runs on the
   main thread between frames and cannot re-enter the step, while *when*
   it comes due is a simulation time rather than a frame rate.
   `wait_frames` is re-specified against `tickIndex` and keeps its name.

4. **This is a behaviour change with a version, not a silent
   reinterpretation.** Timer firing times move. Any content that depended
   on the old per-frame cadence changes behaviour, so the change carries
   before-and-after tests naming both cadences, and the state-hash
   baselines are expected to move once, deliberately, in the commit that
   lands it.

## Consequences

Replay, and later networking, become possible without redesigning time or
randomness: a run is reproducible from a seed plus an input sequence.

A script that calls `math.random` expecting operating-system entropy no
longer gets it. For a game engine that is the right trade — an author who
genuinely wants unpredictability can seed from a clock explicitly — but it
is a real behaviour change for anything already written.

The per-frame timer semantics are gone rather than documented as the
contract, which was the alternative this record rejects. Documenting the
frame-rate dependence would have made every future gameplay timer a
portability question.

## Alternatives rejected

**Remove `math.random` from the sandbox.** Honest, and it forces authors
to the engine names, but it turns a working call into a nil-index error
for the exact beginner the engine is meant to serve, and every tutorial
written against Lua stops working.

**Document per-frame timer cadence as the contract.** Cheaper today, and
it leaves the frame rate in the semantics of every timer anyone writes
afterwards. Rejected as the more expensive of the two, later.

**Per-entity random streams.** More useful for rollback, much more
machinery; out of scope here and not foreclosed by this record.
