// Declares cvar types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

// CVar (Console Variable) system.
// Provides named, typed, runtime-mutable configuration variables.
// Variables are registered once at startup with a default value; subsequent
// get/set calls look up by name.  All functions are noexcept and safe to call
// from any thread after initialize_cvars().

namespace engine::core {

/// Enumerates cvar type values used by the engine.
enum class CVarType : std::uint8_t {
  Bool = 0,
  Int = 1,
  Float = 2,
  String = 3,
};

/// Registered cvar: name, type, current/default values, description.
struct CVarInfo final {
  const char *name = nullptr;
  const char *description = nullptr;
  CVarType type = CVarType::Bool;
};

/// Initializes the owning system for cvars.
bool initialize_cvars() noexcept;
/// Shuts down the owning system for cvars.
void shutdown_cvars() noexcept;

// Registration — returns false if already registered or capacity exceeded.
bool cvar_register_bool(const char *name, bool defaultValue,
                        const char *description) noexcept;
/// Registers an int cvar; false on duplicate name or full registry.
bool cvar_register_int(const char *name, int defaultValue,
                       const char *description) noexcept;
/// Registers a float cvar; false on duplicate name or full registry.
bool cvar_register_float(const char *name, float defaultValue,
                         const char *description) noexcept;
/// Registers a string cvar; false on duplicate name or full registry.
bool cvar_register_string(const char *name, const char *defaultValue,
                          const char *description) noexcept;

// Getters — return fallback when the name is not found.
bool cvar_get_bool(const char *name, bool fallback = false) noexcept;
/// Current int value, or fallback when the cvar is unknown.
int cvar_get_int(const char *name, int fallback = 0) noexcept;
/// Current float value, or fallback when the cvar is unknown.
float cvar_get_float(const char *name, float fallback = 0.0F) noexcept;
/// Current string value, or fallback when the cvar is unknown.
const char *cvar_get_string(const char *name,
                            const char *fallback = "") noexcept;

// Setters — return false when the name is not found or type mismatches.
bool cvar_set_bool(const char *name, bool value) noexcept;
/// Sets an int cvar; false when unknown or type-mismatched.
bool cvar_set_int(const char *name, int value) noexcept;
/// Sets a float cvar; false when unknown or type-mismatched.
bool cvar_set_float(const char *name, float value) noexcept;
/// Sets a string cvar; false when unknown or type-mismatched.
bool cvar_set_string(const char *name, const char *value) noexcept;

// Set from a string literal, parsing according to the registered type.
// Used by the console command "set <name> <value>".
bool cvar_set_from_string(const char *name, const char *valueStr) noexcept;

// Enumerate all registered CVars.  Returns the number of entries written.
std::size_t cvar_get_all(CVarInfo *out, std::size_t maxEntries) noexcept;

} // namespace engine::core
