# CLAUDE.md — Engine

The contributor contract: the rules a change must satisfy. It holds rules
only. Architecture lives in `docs/architecture.md`, product direction in
`docs/vision.md`, decisions in `docs/decisions/`, open work on the GitHub
tracker, and current behavior in the code. When this document conflicts
with the code or the tests, report and resolve the mismatch rather than
assuming the document is right.

C++23 game engine built from scratch: SDL3, bgfx (deferred+forward,
PBR/IBL), fixed-capacity ECS, CPU-deterministic physics, Lua 5.4, miniaudio,
ImGui editor. Game authors work through Lua and the editor; engine
contributors work in C++ under the rules below.

## Load the skill for what you are doing

These carry the procedures. Load them without being asked — the trigger is
the work, not a request.

| Doing this | Load |
| --- | --- |
| Claiming a change works; writing a PR's evidence | `verify` |
| Touching the renderer, shaders or post stack | `verify` (CI cannot verify this tier) |
| Writing or editing comments; adding a file | `comment` |
| Working a tracked issue; deciding if a fix is done | `close-finding` |
| Seeing one concept implemented twice | `consolidate-primitive` |
| Adding or changing anything serialized | `serialization` |
| Handling a PR event; CI red; choosing work | `steward` |

## Rule labels

- **[CI]** — a named mechanical gate rejects a violation.
- **[REVIEW]** — demonstrated in the change and checked by a reviewer.
  There is currently no independent human reviewer on most changes, so in
  practice this means an adversarial self-check plus the owner's spot
  checks. Treat a [REVIEW] rule as weakly enforced, and converting one to
  [CI] as always-welcome work.
- **[OWNER]** — requires explicit project-owner approval.

Calling a rule enforced without one of these three is prohibited.

## Hard rules

- **[CI]** C++23 only. No exceptions, no RTTI, no `dynamic_cast`/`typeid`
  (`/EHs-c- /GR-`, `_HAS_EXCEPTIONS=0`; `/W4 /WX` or `-Werror`). Language
  features must compile on every CI lane (AppleClang is the laggard).
  New APIs prefer `std::expected<T, E>`; never call `.value()` — with
  exceptions disabled it terminates (`tools/check_error_handling.py`). Use
  `has_value()`/`operator*`/`error()`.
- **[REVIEW]** Public real-time and leaf runtime APIs are `noexcept` only
  when every operation they invoke is proven non-throwing. A recoverable
  `noexcept` path must not call allocation, filesystem, or thread-creation
  operations that can terminate under the no-exception build. Cold
  initialization, editor, tool and filesystem work uses staged RAII
  transactions, explicit error results and rollback. No silent failure and
  no process termination for a recoverable error.
- **[REVIEW]** No heap allocation on hot paths: ECS iteration, transform
  propagation, physics stepping, render prep, command buffers, streaming,
  input, jobs. Fixed-size preallocated storage; no unordered containers,
  locks or virtual dispatch there without a profile and a budget.
- **[CI]** Dependency flow is strictly downward, with no cycles or sideways
  edges (`tools/check_module_deps.py`; the graph and its tracked
  exceptions are in `docs/architecture.md`).
- **[REVIEW]** Public headers are self-contained and never leak SDL, bgfx,
  Lua, ImGui or ImGuizmo types. bgfx stays inside renderer implementation
  (plus the editor's sanctioned ImGui backend), Lua inside scripting
  implementation, editor-only behavior in `editor/` behind explicit
  bridges.
- **[CI][REVIEW]** Comments follow the `comment` skill. Every source and
  header carries a real file-level purpose comment
  (`tools/check_source_comments.py`); filler, commented-out code and
  untracked TODOs are rejected (`tools/check_comment_quality.py`).
- **[CI][REVIEW]** Changes to math, ECS, physics, renderer or scripting
  behavior require tests. Determinism-sensitive areas — world,
  serialization, physics, render prep, Lua API — pair with determinism
  tests.
- **[CI][REVIEW]** Tests assert the semantic contract at the strictest
  valid precision. Integer state, serialized data and promised hashes are
  exact. Floating-point tests carry justified tolerances plus invariants;
  an arbitrary loose tolerance is a defect. Functional tests never assert
  wall-clock timing (`tools/check_test_timing.py`); only `engine_bench_*`
  holds performance thresholds.
- **[OWNER]** Existing behavioral tests are contracts, not append-only
  relics. They change only when the old contract is defective or a
  migration is approved, and the change proves the new contract and
  preserves a legacy mode where content compatibility requires it. Never
  weaken, skip, disable or delete a test to make a change pass.
- **[REVIEW]** One responsibility per translation unit; ~1,000 lines is the
  review trigger for engine sources. Split growing tests into focused
  suites, preserving test names.
- **[OWNER]** No new third-party dependencies, and never ones requiring
  exceptions or RTTI in engine code.
- **[CI]** Third-party code is content-addressed: every FetchContent git
  dependency carries a 40-hex commit `GIT_TAG` (a `URL` download an
  `URL_HASH`), and every remote GitHub Actions `uses:` pins a commit SHA
  (`tools/check_dependency_pins.py`).
- **[REVIEW]** Beginner-friendly APIs never justify incorrect internal
  semantics. Simplicity comes from presets, defaults, validation,
  diagnostics, undo and progressive disclosure. Standard physics names —
  hinge, slider, ball socket, fixed — implement their standard degrees of
  freedom; a simpler constraint takes a different public name.
- **[REVIEW]** Authored user data is never written by truncating the final
  destination. Scenes, prefabs, saves, project and editor settings, input
  maps, metadata and cooked outputs use staged sibling writes with checked
  write/flush/sync/close and atomic replacement. Multi-file outputs commit
  as a transaction or a manifest. A failed load, migration or save
  preserves the previous valid state.
- **[REVIEW]** An identity-bearing field — a name that is hashed or looked
  up, an asset, script or controller path — rejects input that does not fit
  whole, with a logged diagnostic and the destination unchanged. A
  display-only field may truncate with a warning; log and scratch buffers
  may truncate silently.
- **[REVIEW]** No feature is described as working — in a document, a
  comment, a PR, or a status claim — without a named end-to-end test or a
  dated human observation beside it. "Verified", "landed" and
  "production-ready" name the commit, the evidence, the platform and the
  date, or they are not written.
- **[OWNER]** Severity is impact: **P0** data loss or corruption,
  unrecoverable project damage, a critical crash; **P1** a major
  correctness or stability failure, or an important workflow that cannot
  reliably complete; **P2** a user-visible functional defect, or a
  workflow materially obstructed but recoverable; **P3** low-impact debt,
  hygiene, cosmetics. Visibility alone sets no severity, and a reachable
  functional defect is never dismissed for being hard to reach. **P0 is
  stop-the-line** — while one is open it is the work, and a consolidation
  is allowed for it only as the owning-layer fix. **P1 and P2 counts are
  health signals, not quotas**: a rising count is reported and triaged,
  never optimized for. **P3 is not audited**; it is normally fixed inside
  a change already touching its files, and stands alone only for bounded
  structural value (deleting a substantial obsolete API or dead code,
  unblocking a migration, removing recurring noise, a cleanup cheaper than
  carrying). Age triggers triage or an icebox, never a close. Do not open a
  broad audit campaign while P0 or P1 is non-empty. Severity is assigned
  per row, never per batch; **anything observed happening in real use is
  at least P2** whatever it was filed as; a budget never justifies a
  downgrade. Zero open findings is not a reachable state for an engine.

## Working conventions

- Derive every change from the product goal and the architecture
  invariants, not from what a demo or a symptom needs today. A fix
  addresses the root cause at the layer that owns it; a symptom-level patch
  at the nearest convenient layer is rejected even when it works.
- **Never select or defer work by which files are free of other in-flight
  work.** That criterion excludes every structural fix, because structural
  fixes touch many files. Select by severity and by owning layer; pause
  concurrent work when it overlaps. Foundation and consolidation changes
  run alone, holding the default branch.
- Small focused changes, one concern per commit, no drive-by rewrites.
  Concise imperative commit messages. A consolidation is one concern even
  when it spans modules.
- `git status` before editing; never overwrite uncommitted work that is not
  yours; never delete source files or hide build failures; do not commit
  unless asked.
- Findings, bugs and tech debt are queued as GitHub issues through
  `.github/ISSUE_TEMPLATE/`. The tracker is the source of truth for open
  scope — never a document, a PR body or a closed issue's comments.
  Residual scope from a partial fix gets its own linked issue.
- Prefer `bool`+log, small status objects, or optional-like returns;
  assertions only for programmer errors.
- **One fact, one owner.** No document mirrors another, no history in an
  always-loaded contract, every document has a unique purpose, stale prose
  is deleted. A genuinely new subsystem may add a document. Through the
  current correction pass a new skill or document still deletes at least
  as much prose as it adds and skills cap at eight; only procedures with a
  completion criterion become skills.

## Build and test

`README.md` has prerequisites, presets and how to run the app. The `verify`
skill has what to run to prove a change — load it rather than assembling
commands from memory.

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure -LE gpu
```

`build/compile_commands.json` is the clangd source of truth; never
hand-edit it and never commit `build/`.
