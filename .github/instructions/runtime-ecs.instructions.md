---
description: "Use when editing runtime world or ECS code, physics stepping, reflection registration, scene serialization, or entity lifecycle."
name: "Runtime ECS Rules"
applyTo: "runtime/**, physics/**, tests/integration/**"
---
# Runtime ECS Rules

Every rule below is a hard gate. Violations are blocking defects.

## World Phase Contract

The World operates in strict phases. Calling an operation in the wrong phase is a logic bug that corrupts double-buffered state.

| Phase | Allowed | Forbidden |
|-------|---------|-----------|
| Idle | Entity create/destroy, component add/remove, scene load/save, editor mutations | Physics writes, render submission |
| Input | add_transform, read input, queue deferred mutations | Direct component removal |
| Simulation | get_transform_write_ptr, physics step, collision resolve | Entity creation, component add |
| TransformPropagation | Parent→child cascade, dirty flag clear | Any mutation |
| RenderSubmission | Read transforms for render commands, frustum cull | Any mutation |
| Render | GPU submission (renderer owns this) | Any world access |

Hard rules:
- `add_transform()` → WorldPhase::Input. No other phase.
- `get_transform_write_ptr()` → WorldPhase::Simulation. No other phase.
- Entity create/destroy → WorldPhase::Idle. No other phase.
- Editor mutations → `world_is_editable()` AND WorldPhase::Idle. Both conditions.

## SparseSet Storage

- Components live in `SparseSet<Entity, Component, kMaxEntities, kMaxComponents>`.
- Dense array = cache-friendly iteration. Sparse array = O(1) lookup.
- Double-buffered transforms: `writeStateIndex` for simulation, `readStateIndex` for render.
- Never mix read/write indices in a single operation.

## Entity Handles

- Entity = index (uint32_t) + generation (uint32_t).
- Generation increments on reuse. Stale handles are detectable.
- Free list recycles indices.
- Validate every handle before use: `is_valid_entity()`. No exceptions.

## Adding a Component Type — Mandatory Checklist

Every new component type requires ALL ten deliverables. Skip none.

1. **POD struct** in `world.h` — no constructors, no virtuals, no owning pointers, no std::string. Default initializers on every field.
2. **SparseSet typedef + member** in World.
3. **Capacity constant**: `static constexpr std::size_t kMaxXxx = N;`
4. **CRUD methods**: `add_X`, `remove_X`, `get_X` — all noexcept, validate entity + phase, return false and log on failure. Never assert. Never crash.
5. **Reflection** in `reflect_types.cpp` via REFLECT_TYPE/REFLECT_FIELD/REFLECT_END.
6. **Scene serialization** in `scene_serializer.cpp` — both save_scene and load_scene.
7. **Prefab serialization** in `prefab_serializer.cpp` — both save_prefab and instantiate_prefab.
8. **Lua bindings** in `scripting/src/scripting.cpp` — entity index only, never raw pointers.
9. **Editor inspector** support via reflection.
10. **Unit test** in `runtime_world_test.cpp` — add, get, remove, double-add, invalid entity, wrong phase.

## Scene Serialization

- Stage load_scene into a temporary World. Commit only after full parse succeeds.
- On failure: log and return false. Never mutate the live world on partial parse.
- Do not cache parser navigation pointers across calls.
- Persistent IDs must survive save/load roundtrip. Test this.
- JSON output must be deterministic for identical input.

## Physics Integration

- Gravity applied per-step, not at component creation.
- `resolve_collisions` runs post-chunk — it reads and writes global transform state.
- Collision callbacks dispatch via entity indices, never pointers.
- Physics step runs during WorldPhase::Simulation only.
