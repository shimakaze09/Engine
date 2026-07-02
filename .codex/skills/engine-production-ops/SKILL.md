---
name: engine-production-ops
description: Work on the Engine repository's production operations, platform packaging, project workflow, compatibility migration, commandlet, content validation, release signing, diagnostics, safe mode, compliance, and hardware QA tasks. Use when implementing or planning TODO items under P1-M12 or P1-M13, or when a request mentions ship readiness, packaging, commandlets, project manifests, migrations, crash diagnostics, release artifacts, or production workflow.
---

# Engine Production Ops

## Workflow

Start in `D:\dev\Engine` unless the user points to another checkout.

Read first:

- `AGENTS.md`
- `README.md`
- `TODO.md`
- Root `CMakeLists.txt`
- Relevant module `CMakeLists.txt` files for `app`, `core`, `runtime`, `editor`, `tools`, and `tests`
- Existing tests for platform paths, asset cooking, scene/prefab serialization, smoke startup, and CI helper scripts

Run `git status --short` before edits and preserve unrelated user changes.

## Production Rules

Keep project workflow and release automation layered above runtime systems:

- Commandlet entry should live at the app/editor boundary, with reusable implementation in runtime, tools, or editor modules as appropriate.
- Project manifests, cooked-output paths, and package metadata must not require editing engine source for a game project.
- Versioned data migrations must be deterministic and idempotent, with old-format fixtures under tests.
- Content validation must return explicit status objects or errors; do not fail only by logging.
- Release and diagnostic systems must default to local files and no network traffic unless the user explicitly enables upload behavior.
- Do not add large third-party packaging, telemetry, crash, or compression dependencies without confirmation.

## Implementation Checklist

For compatibility and migrations:

- Add schema versions to scenes, prefabs, saves, project manifests, and asset metadata.
- Build one-version migration steps with fixture tests.
- Keep serialization format changes covered by regression tests.

For project workflow and commandlets:

- Define `.engineproject` loading and validation first.
- Add headless commandlet flow for `cook`, `validate`, `package`, `smoke`, and `generate-docs`.
- Keep editor UI optional; CI workflows must run without opening windows.

For release readiness:

- Produce platform-specific cooked asset manifests with hashes and source provenance.
- Package license notices, checksums, symbols, and release metadata.
- Add safe-mode and settings recovery paths before broad platform packaging.

## Verification

Prefer focused tests before broad CTest:

- Unit tests for manifest parsing/defaults, migrations, validation rules, settings recovery, and platform paths.
- Integration or smoke tests for commandlets and packaged/cooked startup.
- CI helper script tests for deterministic reports and failure modes.

When code is unchanged, validate docs/skills only. When behavior changes, run targeted CTest patterns and explain any skipped full-suite verification.
