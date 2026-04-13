---
description: "Use when editing Lua bindings, gameplay scripts, or the scripting runtime. Covers Lua C API safety, binding patterns, and API surface rules."
name: "Lua Scripting Rules"
applyTo: "scripting/**, assets/**/*.lua"
---
# Lua Scripting Rules

Every rule below is a hard gate. Violations are blocking defects.

## Runtime

- Lua 5.4 via C API. Single global state (`g_state`).
- Standard libraries loaded: base, coroutine, table, string, math, utf8.
- Excluded: io, os, debug, package. No file system access from scripts.

## Safety Rules

1. Execute ALL Lua calls through `lua_pcall`. Never allow `longjmp` across noexcept boundaries.
2. Validate argument types at the start of every binding function.
3. Validate `g_world != nullptr` and entity handle before any world mutation.
4. Return nil or false on failure. Never crash the Lua state.
5. Log errors with file + line via `luaL_traceback()`.
6. Expose ONLY the `engine` table as the user-facing API surface.
7. Entity handles cross the boundary as uint32_t indices. Never expose: raw pointers, World*, internal handles, SparseSet references.
8. Every binding function is `noexcept`. Lua errors go through `luaL_error`, never C++ exceptions.
9. Always push a return value. Never leave the stack dirty. Return the count of pushed values.

## Binding Pattern

Every binding follows this exact structure. No variations.

```cpp
// Lua: engine.feature_name(entity_id, ...)
// Returns: <result> on success, nil on failure
static int lua_engine_feature_name(lua_State* state) noexcept {
    // 1. Validate argument count
    if (lua_gettop(state) < expectedArgs) {
        lua_pushnil(state);
        return 1;
    }
    // 2. Extract arguments with type checking
    const auto entityIdx = read_entity_index(state, 1);
    // 3. Validate world and entity
    if (!g_world || !g_world->is_valid_entity(entityIdx)) {
        lua_pushnil(state);
        return 1;
    }
    // 4. Perform operation
    // 5. Push result
    lua_pushboolean(state, 1);
    return 1;
}
```

Registration:
```cpp
lua_pushcfunction(state, &lua_engine_feature_name);
lua_setfield(state, -2, "feature_name");
```

## Naming

- Lua-facing names: snake_case, natural English.
- Prefix with verb: `get_`, `set_`, `is_`, `has_`, `add_`, `remove_`, `spawn_`, `destroy_`.
- Examples: `engine.get_position(entity)`, `engine.set_velocity(entity, x, y, z)`.

## API Design Principle

If a game author must understand C++ internals to use the API, the API is wrong. Every function must be self-explanatory from its name and arguments alone.
