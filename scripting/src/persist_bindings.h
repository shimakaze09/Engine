// Declares private Lua persist table bindings for the scripting module.

#pragma once

struct lua_State;

namespace engine::scripting {

/// Lua binding: Lua engine.persist(key, value).
int lua_engine_persist(lua_State *state) noexcept;
/// Lua binding: Lua engine.restore(key).
int lua_engine_restore(lua_State *state) noexcept;

/// Releases the Lua persist table registry ref.
void clear_persist_bindings(lua_State *state) noexcept;

/// Lua binding: engine.save_data(table) -> bool. Serializes a flat table
/// (string keys of at most 127 bytes; number/string/bool values) to the
/// running project's single JSON save slot. A document past
/// kMaxGameSaveBytes, a value it cannot write, or a save held because it
/// did not load returns false with an Error logged and the previous save
/// unchanged. The document carries the format version.
int lua_engine_save_data(lua_State *state) noexcept;

/// Lua binding: engine.load_data() -> table, "ok" | nil, status. The status
/// is "absent" (no save yet), "corrupt" (it does not parse, or an entry is
/// malformed), "unsupported" (a newer build wrote it) or "unreadable" (the
/// file could not be read). Every status but "absent" is logged with its
/// reason and holds the save: engine.save_data refuses until
/// engine.discard_save(), so a game that starts fresh cannot overwrite
/// what may be the only copy of the player's progress.
int lua_engine_load_data(lua_State *state) noexcept;

/// Lua binding: engine.discard_save() -> bool. Moves the save aside to
/// save.json.discarded-<n> (never deleting it) and lets the next
/// engine.save_data start a new file; false, logged, when it cannot.
int lua_engine_discard_save(lua_State *state) noexcept;

} // namespace engine::scripting
