# 0016 — A project is the unit of authoring and shipping

**Date:** 2026-09-21. Owner decision, following the gap inventory that
produced [0015](0015-commercial-anime-engine-on-six-platforms.md).

**Status:** Point 5 is in place for a running game's per-user data: the
save slot and the rebound input map live in a per-project directory
(`core/include/engine/core/project_data.h`), named for now by the mounted
content root because no `.project` document exists yet; its GUID takes
that role once one does. The rest is not yet implemented.

## Context

The engine has no notion of a project. `app/main.cpp` takes no arguments
and calls `bootstrap()` with defaults; `EngineConfig` is seven hard-coded
strings under `assets`; the editor can only be launched from the
repository root and cannot be told what to open. The sample game's scenes
and characters sit in the same `assets/` directory as the engine's own
shaders, fonts, templates and web shell, so deleting the sample deletes
the renderer's shaders. The editor is linked whole-archive into the only
executable, and nothing a game must own — its scene list, startup scene,
collision-layer matrix, audio bus graph, default input map, quality
tiers, tags, locales — has anywhere to live.

The primitives a project system needs already exist: the VFS mounts
several named prefixes, `platform_get_save_dir` resolves a per-user
directory by organisation and application, the JSON serializer carries
schema versions, and `engine_validate` opens scenes by path.

## Decision

1. **A project is a directory with a `.project` document at its root.**
   The document uses the same serializer, schema-version and
   refuse-on-malformed discipline as `.scene`
   ([0013](0013-malformed-authored-fields-refuse-the-load.md)). It owns
   the project's identity, its roots, its scene list and startup scene,
   its build targets, and every per-project table that today is
   hard-coded, missing or misplaced: collision layers and their matrix,
   audio buses, the default input map, quality tiers, tags and layers,
   locales, the main script and its sandbox budgets.
2. **Engine assets and project content are different mounts.**
   Engine-owned assets — shaders, built-in fonts, editor resources, the
   web shell — mount at `engine://`; the project's content mounts at
   `assets://`. The two never share a directory.
3. **The executables take a project path.** The editor opens a project,
   or its project hub when given none; the player runs one.
   `EngineConfig` is derived from the project document. Its hard-coded
   defaults survive only as the fixture for tests that run without a
   project.
4. **The editor and the player are separate executables.** The player
   links `engine_runtime` and nothing from `editor/`. A configuration
   that cannot build the player without the editor is a defect.
5. **State is split by owner.** Project state lives in the project
   directory and is meant to be committed. Per-user state — layout,
   recent projects, preferences — lives under `platform_get_save_dir`.
   Derived state — cooked outputs, thumbnails, caches — lives under a
   project-local cache root that version control ignores.
6. **The bundled sample is a project**, opened by path. It stays in the
   tree and stays runnable ([0004](0004-engine-first.md)); it stops being
   the engine's own directory layout.

## Consequences

- Every feature epic that needs a project-owned setting depends on this
  record; the project document is where those settings are reserved, one
  schema field at a time, rather than as scattered cvars.
- The six content-browser operations stubbed pending asset identity —
  rename, delete, move, duplicate, reimport, find dependencies — become
  project operations once identity and the project root both exist.
- The `assets` literals across content, runtime, renderer and core —
  sixty-five at the time of writing — are each a site this record
  obliges to resolve through a mount.
- Web shipping packages the project's `assets://` mount, not the engine
  tree.
