---
name: serialization
description: >
  The procedure for adding or changing a persistent component, a scene or
  prefab field, or any serialized format in Engine — the registry row, the
  wire key, the codec, the migration, and the tests that must pair with
  it. Load it before adding a component that persists, before renaming or
  removing a serialized field, before changing a scene, prefab, save,
  material or cooked-asset format, and when a serialization test fails.
---

# Serialization

One authoritative registry generates or mechanically validates every
direction: parse, write, copy, reset, migration and codec coverage. A type
missing a row fails to compile rather than silently skipping. Keep it that
way — never add a parallel dispatch path.

## Adding a persistent component

1. **Add the row** to the X-macro table in
   `runtime/src/component_registry.h`. Row order is the serialized key
   order of every format, so appending is the compatible position.
2. **The compile-time cross-check** against `World::PersistentComponentTypes`
   will tell you what else the row needs. Follow it rather than guessing.
3. **Reflect the fields** that are plain data, and give each a wire key.
   `REFLECT_FIELD_KEY` defaults to the member name; declare it explicitly
   whenever the member name might ever change, because the key — not the
   C++ name — is what scenes carry.
4. **Add a codec** for anything reflection cannot express (64-bit asset
   ids, VFS paths, enums, nested arrays), sharing the helpers in
   `serialization_util` rather than writing a new reader.
5. **Runtime-only state is never serialized.** Slots, blend timers,
   current/previous state, resolved handles, joints, timers. Serialize the
   authored intent; rebuild the rest on load.
6. **The Inspector follows from the registry** — the editor generates its
   capture/apply/remove dispatch from the same table, so a new component
   cannot skip the Inspector. Give it display metadata.

## Wire keys and renames

A member rename must not migrate scenes. Declare the old key on the
renamed member and existing files keep loading. A key change *is* a format
change and needs the migration path below.

## Changing a format

- **Bump the schema version and migrate the tree.** The gate is exact: the
  one current revision loads, and every other value — older, newer,
  malformed, absent — is refused. The project is unreleased, so there is
  no dual-read layer to add; a reader that guessed an older revision
  would drop the fields it no longer knows and resave the document as a
  reduction of itself.
- **Prefer no version change at all.** Where a default-valued field can be
  omitted so that unchanged files stay byte-identical, do that — the
  revision stands and the diff is empty.
- **Migrate every authored file in the same change**, with a script you
  keep out of the engine, reproducing exactly what the read path you are
  deleting produced. A file left behind stops loading.
- **Test both directions**: the new form round-trips, and each refused
  shape — the previous revision, the next one, a missing key, a
  wrong-typed value — is refused.
- A behavior change in serialized data (physics semantics, for instance)
  is the same kind of change: bump the revision, migrate the tree, and
  pair before/after tests, never a silent reinterpretation.

## Failure and durability

- **Parse or load failure leaves the destination unchanged.** A scene load
  stages into a replacement World and commits only on success.
- **Identity-bearing fields reject input that does not fit.** A name that
  is hashed or looked up, an asset, script or controller path: refuse it
  with a logged diagnostic and leave the destination unchanged. Never
  truncate — a truncated identity addresses a different thing. Display-only
  fields may truncate with a warning.
- **Malformed is not missing.** An absent optional field takes its
  default; a present-but-malformed field rejects the document. Conflating
  the two silently rewrites authored data.
- **Writes are staged and atomically replaced**, never truncated in place.
  Multi-file output commits as a transaction or a manifest.

## Tests that must pair with the change

Determinism-sensitive by definition, so:

- Round-trip through the production entry point — never a copied
  serializer model.
- Byte-identical output for identical input.
- `-R engine_integration_determinism` and
  `-R engine_unit_component_registry`.
- A refusal test per ingress you made strict, red on base.
- Boundary cases: empty document, one entity, at capacity, one past
  capacity, malformed field, unknown version, truncated file.

See the `verify` skill for the full tier and the `close-finding` skill for
the evidence a fix needs.
