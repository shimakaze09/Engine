# Decision records

One file per decision, with the date it was made and why. Append-only: a
decision that no longer holds is superseded by a new record that links back
to it, never edited in place.

This is the only place in the repository where dates belong. The rules live
in `CLAUDE.md`, the invariants in `docs/architecture.md`, the direction in
`docs/vision.md`, and open work on the GitHub tracker — none of them carry
history.

| # | Decision | Date |
| --- | --- | --- |
| [0001](0001-bgfx-as-the-rhi.md) | bgfx is the rendering backend | 2026-07-19 |
| [0002](0002-macos-is-a-test-lane.md) | macOS is a build and headless-test lane, not an editor target | 2026-08-09 |
| [0003](0003-script-trust-model.md) | v0.x scripts are author-local and trusted, but VFS-jailed | 2026-08-09 |
| [0004](0004-engine-first.md) | The engine is the product; templates are fixtures | 2026-08-25 |
| [0005](0005-comments-by-complexity.md) | Declarations are documented by complexity | 2026-09-09 |
| [0006](0006-foundation-work-runs-serially.md) | Foundation work runs serially, and overlap never selects work | 2026-09-18 |
| [0007](0007-defect-budget.md) | A defect budget replaces open-ended auditing | 2026-09-18 |
| [0008](0008-evidence-before-status.md) | No status claim without named evidence | 2026-09-18 |
| [0009](0009-net-negative-documentation.md) | Documentation is net-negative | 2026-09-18 |
| [0010](0010-handles-bump-on-release.md) | A generational handle always bumps on release | 2026-09-18 |
| [0011](0011-budgets-check-the-handle.md) | An input budget is enforced on the handle that is read | 2026-09-18 |
| [0012](0012-defect-budget-is-a-flow-rule.md) | The defect budget is a flow rule, not a ceiling | 2026-09-18 |
| [0013](0013-malformed-authored-fields-refuse-the-load.md) | A malformed authored field refuses the load | 2026-09-18 |
| [0014](0014-severity-is-impact.md) | Severity is impact; counts are signals; consolidation needs identity | 2026-09-20 |
| [0015](0015-commercial-anime-engine-on-six-platforms.md) | A commercial anime-game engine: three editor platforms, six shipping targets | 2026-09-21 |
| [0016](0016-a-project-is-the-unit-of-authoring.md) | A project is the unit of authoring and shipping | 2026-09-21 |
| [0017](0017-materials-carry-a-shading-model.md) | A material carries its shading model; passes are chosen per draw | 2026-09-21 |
| [0018](0018-authors-compose-shading.md) | Authors compose shading; the engine ships the pieces and the presets | 2026-09-22 |
| [0019](0019-the-simulation-owns-time-and-randomness.md) | The simulation owns its time and its randomness | 2026-09-22 |
