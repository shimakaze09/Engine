// Declares cvar types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

// CVar (Console Variable) system.
// Provides named, typed, runtime-mutable configuration variables.
// Variables are registered once at startup with a default value; subsequent
// get/set calls look up by name.  All functions are noexcept and safe to
// call from any thread; the storage is process-global and needs no prior
// initialize_cvars() call.  initialize_cvars/shutdown_cvars reset the
// registry to empty and exist for owners of the full lifecycle (tests,
// process teardown) — resetting while cvars are registered discards them.
//
// Two access paths share one table:
//
// - By name: every call takes the registry mutex and scans the table for
//   the name.  This is the console's path and the right one for cold code.
// - By handle: cvar_find resolves a name once (one lock, one scan) into a
//   CVarHandle; scalar reads through the handle take no lock and touch no
//   string, so a hot path (the renderer flush, the streaming pump, the
//   frame pipeline) reads its tuning knobs per frame without contending
//   with console writes.  Live tuning is preserved: a set by name is
//   visible to the next handle read.  CVarRef bundles a name with its
//   handle and re-resolves after a registry reset, which is the form hot
//   paths hold.

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

/// Resolved reference to one registered cvar: the table slot plus the
/// registry generation it was resolved against. A handle outlives its
/// registry generation harmlessly: reads through it return the fallback
/// once shutdown_cvars/initialize_cvars has reset the table, so a stale
/// handle can never alias a cvar registered later in the same slot.
struct CVarHandle final {
  static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFU;
  std::uint32_t index = kInvalidIndex;
  std::uint32_t generation = 0U;

  /// True when the handle was resolved (it may still be stale).
  bool resolved() const noexcept { return index != kInvalidIndex; }
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
// Used by the console command "set <name> <value>".  The whole token must
// parse: bool accepts exactly 1/true/0/false, int/float reject trailing
// text, out-of-range magnitudes, and non-finite floats, and strings longer
// than the stored capacity are rejected rather than truncated.  Failures
// log a diagnostic with the cvar name and input and leave the value as-is.
bool cvar_set_from_string(const char *name, const char *valueStr) noexcept;

// Enumerate all registered CVars.  Returns the number of entries written.
std::size_t cvar_get_all(CVarInfo *out, std::size_t maxEntries) noexcept;

// ---- handle access ----

/// Resolves a name into a handle with one lock and one table scan; an
/// unresolved handle (unknown name) reads as its fallback everywhere.
CVarHandle cvar_find(const char *name) noexcept;
/// True while the handle refers to a live registration: resolved, and the
/// registry has not been reset since. Lock-free.
bool cvar_handle_live(CVarHandle handle) noexcept;

// Scalar reads through a handle take no lock and do no name scan; a stale,
// unresolved, or type-mismatched handle returns the fallback.
bool cvar_get_bool(CVarHandle handle, bool fallback = false) noexcept;
/// Current int value through a handle, or fallback; lock-free.
int cvar_get_int(CVarHandle handle, int fallback = 0) noexcept;
/// Current float value through a handle, or fallback; lock-free.
float cvar_get_float(CVarHandle handle, float fallback = 0.0F) noexcept;
/// Current string value through a handle, or fallback. String storage is
/// not lock-free: this takes the registry mutex but skips the name scan.
/// Pair with cvar_change_stamp to read the string only when it changed.
const char *cvar_get_string(CVarHandle handle,
                            const char *fallback = "") noexcept;
/// Stamp that changes on every successful set of the cvar (by name or from
/// the console) and never repeats within a process: the registry
/// generation in the high 32 bits, the cvar's own set counter (1 right
/// after registration) in the low 32. 0 for a stale or unresolved handle.
/// Lock-free, so a per-frame consumer can compare it against the stamp it
/// last acted on and skip the locked string read while nothing changed.
std::uint64_t cvar_change_stamp(CVarHandle handle) noexcept;

/// Number of by-name table scans performed since the registry was last
/// reset (registration, name lookups, name getters/setters). A diagnostic
/// for tests and profiles: a hot path that holds handles adds nothing to
/// it after its first frame.
std::size_t cvar_name_lookup_count() noexcept;

/// A cvar read site that holds its own handle: resolves the name on first
/// use and again whenever the registry has been reset since, so every
/// steady-state read is the lock-free handle path. A null name never
/// resolves and always yields the fallback. Not synchronized: one CVarRef
/// belongs to one thread (or to a caller that serializes its use), which is
/// the case for the frame-owned consumers it exists for.
class CVarRef final {
public:
  explicit constexpr CVarRef(const char *name) noexcept : m_name(name) {}

  /// Bool value or fallback; lock-free once resolved.
  bool get_bool(bool fallback = false) const noexcept;
  /// Int value or fallback; lock-free once resolved.
  int get_int(int fallback = 0) const noexcept;
  /// Float value or fallback; lock-free once resolved.
  float get_float(float fallback = 0.0F) const noexcept;
  /// String value or fallback; takes the registry mutex, skips the scan.
  const char *get_string(const char *fallback = "") const noexcept;
  /// The cvar's change stamp (see cvar_change_stamp); lock-free.
  std::uint64_t change_stamp() const noexcept;
  /// The bound name (may be null).
  const char *name() const noexcept { return m_name; }

private:
  /// The live handle, re-resolving after a registry reset. An unknown
  /// name re-scans on every call, which is the by-name cost it had before.
  CVarHandle handle() const noexcept;

  const char *m_name;
  // The cached resolution is an implementation detail of a logically
  // read-only reference, so const holders (a const backend state) can read.
  mutable CVarHandle m_handle{};
};

} // namespace engine::core
