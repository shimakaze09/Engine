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

/// Every save binding takes an optional slot name, "default" when absent
/// or nil: a name token (letters, digits, '_', '-' and '.', 1 to 31
/// characters) that ignores case. Any other value is an argument error,
/// never a save to some other slot.

/// Lua binding: engine.save_data(table [, slot]) -> bool. Serializes a
/// flat table (string keys of at most 127 bytes; number/string/bool
/// values) to the slot. A document past the project's save limit, a value
/// it cannot write, or a slot held because it did not load returns false
/// with an Error logged and the previous save unchanged. The document
/// carries the format version.
int lua_engine_save_data(lua_State *state) noexcept;

/// Lua binding: engine.load_data([slot]) -> table, "ok" | nil, status. The
/// status is "absent" (no save yet), "corrupt" (the file is damaged or cut
/// off, it does not parse, or an entry is malformed), "unsupported" (a
/// newer build wrote it) or "unreadable" (the file could not be read).
/// Every status but "absent" is logged with its reason and holds the
/// slot: engine.save_data refuses it until engine.discard_save(slot), so a
/// game that starts fresh cannot overwrite what may be the only copy of
/// the player's progress.
int lua_engine_load_data(lua_State *state) noexcept;

/// Lua binding: engine.discard_save([slot]) -> bool. Moves the slot aside
/// to <slot>.save.discarded-<n> (never deleting it) and lets the next
/// engine.save_data start a new file; false, logged, when it cannot.
int lua_engine_discard_save(lua_State *state) noexcept;

/// Lua binding: engine.list_saves() -> array of {slot, saved_at, bytes,
/// status, legacy}, sorted by slot name. saved_at is seconds since 1970
/// UTC (0 for a save from before slots existed, which lists as "default"
/// with legacy true); status is "ok", or what is wrong with the slot's
/// header ("corrupt", "unsupported", "unreadable"), read without loading
/// the slot.
int lua_engine_list_saves(lua_State *state) noexcept;

/// Lua binding: engine.get_save_limit() -> integer. The largest save, in
/// bytes, engine.save_data writes: the project's setting.
int lua_engine_get_save_limit(lua_State *state) noexcept;

} // namespace engine::scripting
