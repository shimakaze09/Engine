---
description: 'Expose an engine feature to Lua with full validation, error handling, and documentation'
---
Expose the following feature to Lua: ${input:featureDescription}

## Pre-Implementation Verification

Before writing any binding:
1. Read scripting/src/scripting.cpp to understand the existing binding pattern.
2. Identify the helpers: read_entity_index(), read_entity(), push_vec3(), etc.
3. Verify the feature you are exposing exists and works correctly in C++ first.
4. Check the global pointers: g_world, g_services, g_physics_world, g_audio_system.
5. Read scripting-lua.instructions.md for full binding rules.

## Binding Implementation

Write a static C function:
```cpp
static int lua_engine_${input:functionName}(lua_State* L) noexcept
{
    // 1. Validate argument count
    // 2. Read and validate each argument (entity index, numbers, strings)
    // 3. Perform the operation via g_world / g_services / etc.
    // 4. Push return values or nil on failure
    // 5. Return the number of values pushed
}
```

## Mandatory Rules

1. **Entity access**: Use read_entity_index() or read_entity(). NEVER accept raw pointers, World*, or internal handles from Lua.
2. **Null checks**: Check g_world != nullptr, g_services != nullptr before use. Push nil and return on null.
3. **Input validation**: Validate every argument type with luaL_checkinteger, luaL_checknumber, luaL_checkstring. Push nil and return on type mismatch.
4. **Error messages**: Use luaL_error() or push error strings that include the function name and what went wrong.
5. **Return values**: Always push something. Never leave the stack dirty. Return the number of pushed values.
6. **Registration**: Add to register_engine_bindings() with: `lua_setfield(L, -2, "${input:functionName}");`
7. **Naming**: snake_case, reads like natural English. Example: `engine.get_half_extents`, `engine.set_position`.
8. **Thread safety**: Lua bindings run on the main thread only. Do not access job system internals.
9. **No raw pointers**: Never push lightuserdata pointing to engine internals. Entity indices only.
10. **noexcept**: The binding function must be noexcept. Lua errors go through luaL_error, never C++ exceptions.

## Documentation

Add a comment block above the function:
```cpp
// Lua: engine.${input:functionName}(entity_id, ...)
// Description: <what game authors see>
// Args: entity_id (integer), ...
// Returns: <what is pushed on success>, nil on failure
```

## Testing

- Add a Lua script test in tests/ that exercises the new binding.
- Test: valid input, invalid entity, wrong argument types, nil arguments.
- Verify the Lua stack is balanced after each call.

## Do NOT

- Expose internal handles, component pointers, or World*.
- Skip null checks on globals.
- Leave the Lua stack unbalanced.
- Use C++ exceptions in binding code.
- Access non-thread-safe data without documenting assumptions.

A non-programmer reading only the Lua API should understand what this function does.
