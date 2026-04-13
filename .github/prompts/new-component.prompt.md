---
description: 'Add a new ECS component type with full World integration, serialization, Lua binding, and tests'
---
Add a new component named ${input:componentName}Component to the ECS.

## Pre-Implementation Verification

Before writing ANY code:
1. Read `runtime/include/engine/runtime/world.h` — understand Transform, Collider, MeshComponent, LightComponent, AudioSourceComponent, RigidBody patterns.
2. Read `runtime/src/world.cpp` — understand CRUD method implementations and phase checks.
3. Read `runtime/src/reflect_types.cpp` — understand reflection registration.
4. Read `runtime/src/scene_serializer.cpp` — understand JSON serialization pattern.
5. Verify the component does not already exist or overlap with an existing one.

## Required Deliverables (ALL 10 must be completed)

### 1. Component Struct (world.h)
- Location: `runtime/include/engine/runtime/world.h` alongside existing components
- **POD ONLY**: no constructors, no destructors, no virtuals, no owning pointers, no std::string
- Use: float, int32_t, uint32_t, bool, Vec2, Vec3, Vec4, Quat, fixed-size arrays
- Default member initializers for every field
- Keep struct size reasonable (should fit in 1-2 cache lines ideally)

### 2. Capacity Constant (world.h)
```cpp
static constexpr std::size_t kMax${input:componentName}s = 16384U;
```
Use a smaller cap if the component is rare (e.g., cameras, player controllers).

### 3. SparseSet Typedef (world.h)
```cpp
using ${input:componentName}Set = core::SparseSet<Entity, ${input:componentName}Component, kMaxEntities, kMax${input:componentName}s>;
```

### 4. Private Member (world.h)
```cpp
${input:componentName}Set m_${input:componentName}s;
```

### 5. CRUD Methods (world.h declaration + world.cpp implementation)
```cpp
bool add_${input:componentName}(Entity e, const ${input:componentName}Component& c) noexcept;
bool remove_${input:componentName}(Entity e) noexcept;
bool get_${input:componentName}(Entity e, ${input:componentName}Component* out) noexcept;
```
- add/remove: check `is_mutation_phase()`, check `is_valid_entity()`, return false and log on failure
- get: check `is_valid_entity()`, return false if entity has no such component
- NEVER assert, NEVER crash — always return false and log

### 6. Reflection Registration (reflect_types.cpp)
```cpp
REFLECT_TYPE(${input:componentName}Component)
REFLECT_FIELD(fieldName, &${input:componentName}Component::fieldName)
// ... all fields
REFLECT_END()
```

### 7. Scene Serialization (scene_serializer.cpp)
- Add write section in `save_scene()` — serialize all fields to JSON
- Add read section in `load_scene()` — deserialize and call add_${input:componentName}()
- Follow the exact pattern used for MeshComponent or LightComponent

### 8. Prefab Serialization (prefab_serializer.cpp)
- Add to `save_prefab()` and `instantiate_prefab()`
- Follow existing component pattern exactly

### 9. Lua Binding (scripting.cpp)
- Add get/set functions using entity index (NEVER pointers)
- Validate all inputs, push nil on bad input
- Register in `register_engine_bindings()`
- Document with a Lua usage comment

### 10. Unit Tests (runtime_world_test.cpp)
- Test: add component, get it back, verify values match
- Test: remove component, verify get returns false
- Test: double-add same entity (should handle gracefully)
- Test: operations on invalid entity return false
- Test: add in wrong phase returns false (if testable)

## Do NOT
- Make the component non-POD
- Skip any of the 10 deliverables
- Use std::vector, std::map, std::string in the component struct
- Introduce upward module dependencies
- Use heap allocation in CRUD methods
