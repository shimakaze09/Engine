// Implements cvar behavior for the Engine core engine. Textual values are
// parsed as full tokens with range and finiteness checks: trailing garbage,
// overflow, non-finite floats, and unrecognized boolean words are rejected
// with a diagnostic naming the variable and input, and the stored value
// stays unchanged.
//
// Storage is one fixed table under one mutex. The by-name API locks and
// scans it. The handle API reads scalar values from per-entry atomics
// without the lock: a handle is {slot, registry generation}, the generation
// advances on every table reset, and an entry's type and scalar bits are
// atomics so a reader racing a reset or a set observes either the old or
// the new value, never a torn one. Strings stay under the lock; a
// per-entry change stamp lets a handle holder skip the locked read while
// nothing changed.

#include "engine/core/cvar.h"

#include "engine/core/platform.h"

#include "engine/core/logging.h"

#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <system_error>

namespace engine::core {

namespace {

constexpr std::size_t kMaxCVars = 256U;
constexpr std::size_t kMaxNameLen = 64U;
constexpr std::size_t kMaxDescLen = 128U;
constexpr std::size_t kMaxStringValLen = 64U;

/// One table slot. Fields the handle path reads without the lock (type,
/// scalarBits, serial) are atomics; the rest is written and read under
/// g_mutex only. Bool/int/float share scalarBits as their bit pattern so
/// one 32-bit atomic serves every scalar type.
struct CVarEntry final {
  char name[kMaxNameLen] = {};
  char desc[kMaxDescLen] = {};
  std::atomic<std::uint8_t> type{static_cast<std::uint8_t>(CVarType::Bool)};
  std::atomic<std::uint32_t> scalarBits{0U};
  std::atomic<std::uint32_t> serial{0U};
  char str[kMaxStringValLen] = {};
  bool used = false;

  /// Returns the slot to its unregistered state. Caller holds g_mutex.
  void clear() noexcept {
    std::memset(name, 0, sizeof(name));
    std::memset(desc, 0, sizeof(desc));
    type.store(static_cast<std::uint8_t>(CVarType::Bool),
               std::memory_order_relaxed);
    scalarBits.store(0U, std::memory_order_relaxed);
    serial.store(0U, std::memory_order_relaxed);
    std::memset(str, 0, sizeof(str));
    used = false;
  }
};

bool g_initialized = false;
std::array<CVarEntry, kMaxCVars> g_entries{};
std::size_t g_count = 0U;
std::mutex g_mutex{};
// Advances on every table reset so a handle resolved before the reset
// reads as stale afterwards. Starts at 1 so a default-constructed handle
// (generation 0) never matches a live registry.
std::atomic<std::uint32_t> g_generation{1U};
// By-name scans since the last reset; guarded by g_mutex.
std::size_t g_nameLookups = 0U;

struct CVarInfoSnapshot final {
  char names[kMaxCVars][kMaxNameLen] = {};
  char descriptions[kMaxCVars][kMaxDescLen] = {};
};

thread_local char g_stringResult[kMaxStringValLen] = {};
thread_local CVarInfoSnapshot g_infoSnapshot{};

/// Finds the matching object or resource for cvar. Caller must hold g_mutex.
int find_cvar_unlocked(const char *name) noexcept {
  if (name == nullptr) {
    return -1;
  }

  ++g_nameLookups;
  for (std::size_t i = 0U; i < g_count; ++i) {
    if (g_entries[i].used && std::strcmp(g_entries[i].name, name) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

/// Empties the table and retires every outstanding handle. Caller holds
/// g_mutex.
void reset_table_unlocked() noexcept {
  for (CVarEntry &entry : g_entries) {
    entry.clear();
  }
  g_count = 0U;
  g_nameLookups = 0U;
  g_generation.fetch_add(1U, std::memory_order_acq_rel);
}

CVarType entry_type(const CVarEntry &entry) noexcept {
  return static_cast<CVarType>(entry.type.load(std::memory_order_acquire));
}

// ---- scalar storage: value bits in, value bits out ----

std::uint32_t bits_of(bool value) noexcept { return value ? 1U : 0U; }
std::uint32_t bits_of(int value) noexcept {
  return std::bit_cast<std::uint32_t>(value);
}
std::uint32_t bits_of(float value) noexcept {
  return std::bit_cast<std::uint32_t>(value);
}

/// Publishes a scalar and advances the serial. Caller holds g_mutex, so
/// two writers cannot interleave; the release stores pair with the acquire
/// loads on the lock-free read path.
void store_scalar(CVarEntry &entry, std::uint32_t bits) noexcept {
  entry.scalarBits.store(bits, std::memory_order_release);
  entry.serial.fetch_add(1U, std::memory_order_release);
}

/// Copies a string value and advances the serial. Caller holds g_mutex.
void store_string(CVarEntry &entry, const char *value) noexcept {
  std::snprintf(entry.str, kMaxStringValLen - 1U + 1U, "%s", value);
  entry.str[kMaxStringValLen - 1U] = '\0';
  entry.serial.fetch_add(1U, std::memory_order_release);
}

/// The slot a live handle refers to, or nullptr when the handle is
/// unresolved or predates the last reset. Lock-free.
const CVarEntry *live_entry(CVarHandle handle) noexcept {
  if ((handle.index >= kMaxCVars) ||
      (handle.generation != g_generation.load(std::memory_order_acquire))) {
    return nullptr;
  }
  return &g_entries[handle.index];
}

} // namespace

/// Initializes the owning system for cvars.
bool initialize_cvars() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_initialized) {
    return true;
  }
  reset_table_unlocked();
  g_initialized = true;
  return true;
}

/// Shuts down the owning system for cvars.
void shutdown_cvars() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  reset_table_unlocked();
  g_initialized = false;
}

// ---- registration ----

// Defined with the parse helpers below; applied inside each register so
// the override sees the entry's final type.
void apply_env_override(CVarEntry &entry) noexcept;

namespace {

/// Claims the next free slot for name; nullptr on duplicate or full table.
/// Caller holds g_mutex. The serial starts at 1 so a handle holder that
/// remembers 0 ("never seen") reads the freshly registered value once.
CVarEntry *claim_entry_unlocked(const char *name, const char *description,
                                CVarType type) noexcept {
  if (g_count >= kMaxCVars) {
    return nullptr;
  }
  if (find_cvar_unlocked(name) >= 0) {
    return nullptr;
  }

  CVarEntry &e = g_entries[g_count++];
  std::snprintf(e.name, kMaxNameLen - 1U + 1U, "%s", name);
  std::snprintf(e.desc, kMaxDescLen - 1U + 1U, "%s", description);
  e.type.store(static_cast<std::uint8_t>(type), std::memory_order_release);
  e.serial.store(1U, std::memory_order_release);
  e.used = true;
  return &e;
}

} // namespace

bool cvar_register_bool(const char *name, bool defaultValue,
                        const char *description) noexcept {
  if ((name == nullptr) || (description == nullptr)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  CVarEntry *e = claim_entry_unlocked(name, description, CVarType::Bool);
  if (e == nullptr) {
    return false;
  }
  e->scalarBits.store(bits_of(defaultValue), std::memory_order_release);
  apply_env_override(*e);
  return true;
}

bool cvar_register_int(const char *name, int defaultValue,
                       const char *description) noexcept {
  if ((name == nullptr) || (description == nullptr)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  CVarEntry *e = claim_entry_unlocked(name, description, CVarType::Int);
  if (e == nullptr) {
    return false;
  }
  e->scalarBits.store(bits_of(defaultValue), std::memory_order_release);
  apply_env_override(*e);
  return true;
}

bool cvar_register_float(const char *name, float defaultValue,
                         const char *description) noexcept {
  if ((name == nullptr) || (description == nullptr)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  CVarEntry *e = claim_entry_unlocked(name, description, CVarType::Float);
  if (e == nullptr) {
    return false;
  }
  e->scalarBits.store(bits_of(defaultValue), std::memory_order_release);
  apply_env_override(*e);
  return true;
}

bool cvar_register_string(const char *name, const char *defaultValue,
                          const char *description) noexcept {
  if ((name == nullptr) || (description == nullptr)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  CVarEntry *e = claim_entry_unlocked(name, description, CVarType::String);
  if (e == nullptr) {
    return false;
  }
  if (defaultValue != nullptr) {
    std::snprintf(e->str, kMaxStringValLen - 1U + 1U, "%s", defaultValue);
  }
  apply_env_override(*e);
  return true;
}

// ---- getters ----

bool cvar_get_bool(const char *name, bool fallback) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (entry_type(g_entries[idx]) != CVarType::Bool)) {
    return fallback;
  }
  return g_entries[idx].scalarBits.load(std::memory_order_acquire) != 0U;
}

int cvar_get_int(const char *name, int fallback) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (entry_type(g_entries[idx]) != CVarType::Int)) {
    return fallback;
  }
  return std::bit_cast<int>(
      g_entries[idx].scalarBits.load(std::memory_order_acquire));
}

float cvar_get_float(const char *name, float fallback) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (entry_type(g_entries[idx]) != CVarType::Float)) {
    return fallback;
  }
  return std::bit_cast<float>(
      g_entries[idx].scalarBits.load(std::memory_order_acquire));
}

const char *cvar_get_string(const char *name, const char *fallback) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (entry_type(g_entries[idx]) != CVarType::String)) {
    return fallback;
  }
  std::snprintf(g_stringResult, sizeof(g_stringResult), "%s",
                g_entries[idx].str);
  return g_stringResult;
}

// ---- setters ----

bool cvar_set_bool(const char *name, bool value) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (entry_type(g_entries[idx]) != CVarType::Bool)) {
    return false;
  }
  store_scalar(g_entries[idx], bits_of(value));
  return true;
}

bool cvar_set_int(const char *name, int value) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (entry_type(g_entries[idx]) != CVarType::Int)) {
    return false;
  }
  store_scalar(g_entries[idx], bits_of(value));
  return true;
}

bool cvar_set_float(const char *name, float value) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (entry_type(g_entries[idx]) != CVarType::Float)) {
    return false;
  }
  store_scalar(g_entries[idx], bits_of(value));
  return true;
}

bool cvar_set_string(const char *name, const char *value) noexcept {
  if (value == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (entry_type(g_entries[idx]) != CVarType::String)) {
    return false;
  }
  store_string(g_entries[idx], value);
  return true;
}

// ---- handle access ----

CVarHandle cvar_find(const char *name) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if (idx < 0) {
    return CVarHandle{};
  }
  CVarHandle handle{};
  handle.index = static_cast<std::uint32_t>(idx);
  handle.generation = g_generation.load(std::memory_order_acquire);
  return handle;
}

bool cvar_handle_live(CVarHandle handle) noexcept {
  return live_entry(handle) != nullptr;
}

bool cvar_get_bool(CVarHandle handle, bool fallback) noexcept {
  const CVarEntry *entry = live_entry(handle);
  if ((entry == nullptr) || (entry_type(*entry) != CVarType::Bool)) {
    return fallback;
  }
  return entry->scalarBits.load(std::memory_order_acquire) != 0U;
}

int cvar_get_int(CVarHandle handle, int fallback) noexcept {
  const CVarEntry *entry = live_entry(handle);
  if ((entry == nullptr) || (entry_type(*entry) != CVarType::Int)) {
    return fallback;
  }
  return std::bit_cast<int>(entry->scalarBits.load(std::memory_order_acquire));
}

float cvar_get_float(CVarHandle handle, float fallback) noexcept {
  const CVarEntry *entry = live_entry(handle);
  if ((entry == nullptr) || (entry_type(*entry) != CVarType::Float)) {
    return fallback;
  }
  return std::bit_cast<float>(
      entry->scalarBits.load(std::memory_order_acquire));
}

const char *cvar_get_string(CVarHandle handle, const char *fallback) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  // Re-checked under the lock: a reset between the caller's liveness check
  // and this lock would otherwise hand back a recycled slot's string.
  const CVarEntry *entry = live_entry(handle);
  if ((entry == nullptr) || !entry->used ||
      (entry_type(*entry) != CVarType::String)) {
    return fallback;
  }
  std::snprintf(g_stringResult, sizeof(g_stringResult), "%s", entry->str);
  return g_stringResult;
}

std::uint64_t cvar_change_stamp(CVarHandle handle) noexcept {
  const CVarEntry *entry = live_entry(handle);
  if (entry == nullptr) {
    return 0U;
  }
  return (static_cast<std::uint64_t>(handle.generation) << 32U) |
         entry->serial.load(std::memory_order_acquire);
}

std::size_t cvar_name_lookup_count() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_nameLookups;
}

CVarHandle CVarRef::handle() const noexcept {
  if ((m_name != nullptr) && !cvar_handle_live(m_handle)) {
    m_handle = cvar_find(m_name);
  }
  return m_handle;
}

bool CVarRef::get_bool(bool fallback) const noexcept {
  return cvar_get_bool(handle(), fallback);
}

int CVarRef::get_int(int fallback) const noexcept {
  return cvar_get_int(handle(), fallback);
}

float CVarRef::get_float(float fallback) const noexcept {
  return cvar_get_float(handle(), fallback);
}

const char *CVarRef::get_string(const char *fallback) const noexcept {
  return cvar_get_string(handle(), fallback);
}

std::uint64_t CVarRef::change_stamp() const noexcept {
  return cvar_change_stamp(handle());
}

// ---- textual parsing ----

/// Logs one set_from_string rejection with the variable name and raw input.
void log_parse_rejection(const char *name, const char *valueStr,
                         const char *reason) noexcept {
  char msg[192] = {};
  std::snprintf(msg, sizeof(msg), "set '%.63s' rejected: %s value '%.63s'",
                name, reason, valueStr);
  log_message(LogLevel::Error, "cvar", msg);
}

/// Parses exactly "1"/"true" or "0"/"false"; anything else is rejected.
bool parse_bool_token(const char *valueStr, bool *outValue) noexcept {
  if ((std::strcmp(valueStr, "1") == 0) ||
      (std::strcmp(valueStr, "true") == 0)) {
    *outValue = true;
    return true;
  }
  if ((std::strcmp(valueStr, "0") == 0) ||
      (std::strcmp(valueStr, "false") == 0)) {
    *outValue = false;
    return true;
  }
  return false;
}

/// Parses a whole-token base-10 int, rejecting trailing text and overflow.
bool parse_int_token(const char *valueStr, int *outValue) noexcept {
  const char *end = valueStr + std::strlen(valueStr);
  int parsed = 0;
  const auto result = std::from_chars(valueStr, end, parsed, 10);
  if ((result.ec != std::errc{}) || (result.ptr != end)) {
    return false;
  }
  *outValue = parsed;
  return true;
}

/// Parses a whole-token finite float, rejecting trailing text, overflow,
/// and inf/nan spellings. strtof instead of std::from_chars because
/// AppleClang's libc++ still deletes the floating-point overload.
bool parse_float_token(const char *valueStr, float *outValue) noexcept {
  if ((valueStr[0] == '\0') || (std::isspace(
          static_cast<unsigned char>(valueStr[0])) != 0)) {
    return false;
  }
  errno = 0;
  char *parseEnd = nullptr;
  const float parsed = std::strtof(valueStr, &parseEnd);
  if ((parseEnd != valueStr + std::strlen(valueStr)) || (errno == ERANGE) ||
      !std::isfinite(parsed)) {
    return false;
  }
  *outValue = parsed;
  return true;
}

/// Boot-time override: ENGINE_CVAR_<name, dots as underscores> in the
/// environment replaces a cvar's default at registration. The app has
/// no command line, so headless runs, CI, and diagnostics need a
/// launch-time channel; a value that fails the same token parsing as
/// cvar_set_from_string is rejected with the standard diagnostic.
void apply_env_override(CVarEntry &entry) noexcept {
  char envName[kMaxNameLen + 16U] = {};
  std::size_t out = 0U;
  for (const char *c = "ENGINE_CVAR_"; *c != '\0'; ++c) {
    envName[out++] = *c;
  }
  for (const char *c = entry.name; *c != '\0'; ++c) {
    envName[out++] = (*c == '.') ? '_' : *c;
  }
  envName[out] = '\0';
  // GetEnvironmentVariableA under the hood on Windows — std::getenv is
  // a -Werror deprecation there.
  const char *value = non_empty_env(envName);
  if (value == nullptr) {
    return;
  }
  bool ok = false;
  switch (entry_type(entry)) {
  case CVarType::Bool: {
    bool parsed = false;
    ok = parse_bool_token(value, &parsed);
    if (ok) {
      entry.scalarBits.store(bits_of(parsed), std::memory_order_release);
    }
    break;
  }
  case CVarType::Int: {
    int parsed = 0;
    ok = parse_int_token(value, &parsed);
    if (ok) {
      entry.scalarBits.store(bits_of(parsed), std::memory_order_release);
    }
    break;
  }
  case CVarType::Float: {
    float parsed = 0.0F;
    ok = parse_float_token(value, &parsed);
    if (ok) {
      entry.scalarBits.store(bits_of(parsed), std::memory_order_release);
    }
    break;
  }
  case CVarType::String:
    if (std::strlen(value) < kMaxStringValLen) {
      std::snprintf(entry.str, kMaxStringValLen, "%s", value);
      ok = true;
    }
    break;
  }
  if (!ok) {
    log_parse_rejection(entry.name, value, "environment override");
    return;
  }
  char msg[160] = {};
  std::snprintf(msg, sizeof(msg), "environment override: %.63s = %.63s",
                entry.name, value);
  log_message(LogLevel::Info, "cvar", msg);
}

bool cvar_set_from_string(const char *name, const char *valueStr) noexcept {
  if ((name == nullptr) || (valueStr == nullptr)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if (idx < 0) {
    log_parse_rejection(name, valueStr, "unknown cvar for");
    return false;
  }

  CVarEntry &e = g_entries[idx];
  switch (entry_type(e)) {
  case CVarType::Bool: {
    bool parsed = false;
    if (!parse_bool_token(valueStr, &parsed)) {
      log_parse_rejection(name, valueStr, "invalid bool");
      return false;
    }
    store_scalar(e, bits_of(parsed));
    return true;
  }
  case CVarType::Int: {
    int parsed = 0;
    if (!parse_int_token(valueStr, &parsed)) {
      log_parse_rejection(name, valueStr, "invalid int");
      return false;
    }
    store_scalar(e, bits_of(parsed));
    return true;
  }
  case CVarType::Float: {
    float parsed = 0.0F;
    if (!parse_float_token(valueStr, &parsed)) {
      log_parse_rejection(name, valueStr, "invalid float");
      return false;
    }
    store_scalar(e, bits_of(parsed));
    return true;
  }
  case CVarType::String: {
    if (std::strlen(valueStr) >= kMaxStringValLen) {
      log_parse_rejection(name, valueStr, "overlong string");
      return false;
    }
    store_string(e, valueStr);
    return true;
  }
  default:
    return false;
  }
}

std::size_t cvar_get_all(CVarInfo *out, std::size_t maxEntries) noexcept {
  if (out == nullptr) {
    return 0U;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  std::size_t written = 0U;
  for (std::size_t i = 0U; (i < g_count) && (written < maxEntries); ++i) {
    if (!g_entries[i].used) {
      continue;
    }
    std::snprintf(g_infoSnapshot.names[written],
                  sizeof(g_infoSnapshot.names[written]), "%s",
                  g_entries[i].name);
    std::snprintf(g_infoSnapshot.descriptions[written],
                  sizeof(g_infoSnapshot.descriptions[written]), "%s",
                  g_entries[i].desc);
    out[written].name = g_infoSnapshot.names[written];
    out[written].description = g_infoSnapshot.descriptions[written];
    out[written].type = entry_type(g_entries[i]);
    ++written;
  }
  return written;
}

} // namespace engine::core
