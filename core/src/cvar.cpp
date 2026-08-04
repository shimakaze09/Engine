// Implements cvar behavior for the Engine core engine. Textual values are
// parsed as full tokens with range and finiteness checks (audit M-10):
// trailing garbage, overflow, non-finite floats, and unrecognized boolean
// words are rejected with a diagnostic naming the variable and input, and
// the stored value stays unchanged.

#include "engine/core/cvar.h"

#include "engine/core/logging.h"

#include <array>
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

union CVarValue final {
  bool b;
  int i;
  float f;
  char str[kMaxStringValLen];
};

struct CVarEntry final {
  char name[kMaxNameLen] = {};
  char desc[kMaxDescLen] = {};
  CVarType type = CVarType::Bool;
  CVarValue value = {};
  bool used = false;
};

bool g_initialized = false;
std::array<CVarEntry, kMaxCVars> g_entries{};
std::size_t g_count = 0U;
std::mutex g_mutex{};

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

  for (std::size_t i = 0U; i < g_count; ++i) {
    if (g_entries[i].used && std::strcmp(g_entries[i].name, name) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

} // namespace

/// Initializes the owning system for cvars.
bool initialize_cvars() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_initialized) {
    return true;
  }
  g_entries = {};
  g_count = 0U;
  g_initialized = true;
  return true;
}

/// Shuts down the owning system for cvars.
void shutdown_cvars() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_entries = {};
  g_count = 0U;
  g_initialized = false;
}

// ---- registration ----

bool cvar_register_bool(const char *name, bool defaultValue,
                        const char *description) noexcept {
  if ((name == nullptr) || (description == nullptr)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_count >= kMaxCVars) {
    return false;
  }
  if (find_cvar_unlocked(name) >= 0) {
    return false;
  }

  CVarEntry &e = g_entries[g_count++];
  std::snprintf(e.name, kMaxNameLen - 1U + 1U, "%s", name);
  std::snprintf(e.desc, kMaxDescLen - 1U + 1U, "%s", description);
  e.type = CVarType::Bool;
  e.value.b = defaultValue;
  e.used = true;
  return true;
}

bool cvar_register_int(const char *name, int defaultValue,
                       const char *description) noexcept {
  if ((name == nullptr) || (description == nullptr)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_count >= kMaxCVars) {
    return false;
  }
  if (find_cvar_unlocked(name) >= 0) {
    return false;
  }

  CVarEntry &e = g_entries[g_count++];
  std::snprintf(e.name, kMaxNameLen - 1U + 1U, "%s", name);
  std::snprintf(e.desc, kMaxDescLen - 1U + 1U, "%s", description);
  e.type = CVarType::Int;
  e.value.i = defaultValue;
  e.used = true;
  return true;
}

bool cvar_register_float(const char *name, float defaultValue,
                         const char *description) noexcept {
  if ((name == nullptr) || (description == nullptr)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_count >= kMaxCVars) {
    return false;
  }
  if (find_cvar_unlocked(name) >= 0) {
    return false;
  }

  CVarEntry &e = g_entries[g_count++];
  std::snprintf(e.name, kMaxNameLen - 1U + 1U, "%s", name);
  std::snprintf(e.desc, kMaxDescLen - 1U + 1U, "%s", description);
  e.type = CVarType::Float;
  e.value.f = defaultValue;
  e.used = true;
  return true;
}

bool cvar_register_string(const char *name, const char *defaultValue,
                          const char *description) noexcept {
  if ((name == nullptr) || (description == nullptr)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_count >= kMaxCVars) {
    return false;
  }
  if (find_cvar_unlocked(name) >= 0) {
    return false;
  }

  CVarEntry &e = g_entries[g_count++];
  std::snprintf(e.name, kMaxNameLen - 1U + 1U, "%s", name);
  std::snprintf(e.desc, kMaxDescLen - 1U + 1U, "%s", description);
  e.type = CVarType::String;
  if (defaultValue != nullptr) {
    std::snprintf(e.value.str, kMaxStringValLen - 1U + 1U, "%s", defaultValue);
  }
  e.used = true;
  return true;
}

// ---- getters ----

bool cvar_get_bool(const char *name, bool fallback) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::Bool)) {
    return fallback;
  }
  return g_entries[idx].value.b;
}

int cvar_get_int(const char *name, int fallback) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::Int)) {
    return fallback;
  }
  return g_entries[idx].value.i;
}

float cvar_get_float(const char *name, float fallback) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::Float)) {
    return fallback;
  }
  return g_entries[idx].value.f;
}

const char *cvar_get_string(const char *name, const char *fallback) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::String)) {
    return fallback;
  }
  std::snprintf(g_stringResult, sizeof(g_stringResult), "%s",
                g_entries[idx].value.str);
  return g_stringResult;
}

// ---- setters ----

bool cvar_set_bool(const char *name, bool value) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::Bool)) {
    return false;
  }
  g_entries[idx].value.b = value;
  return true;
}

bool cvar_set_int(const char *name, int value) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::Int)) {
    return false;
  }
  g_entries[idx].value.i = value;
  return true;
}

bool cvar_set_float(const char *name, float value) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::Float)) {
    return false;
  }
  g_entries[idx].value.f = value;
  return true;
}

bool cvar_set_string(const char *name, const char *value) noexcept {
  if (value == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const int idx = find_cvar_unlocked(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::String)) {
    return false;
  }
  std::snprintf(g_entries[idx].value.str, kMaxStringValLen - 1U + 1U, "%s",
                value);
  g_entries[idx].value.str[kMaxStringValLen - 1U] = '\0';
  return true;
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
/// and inf/nan spellings.
bool parse_float_token(const char *valueStr, float *outValue) noexcept {
  const char *end = valueStr + std::strlen(valueStr);
  float parsed = 0.0F;
  const auto result = std::from_chars(valueStr, end, parsed);
  if ((result.ec != std::errc{}) || (result.ptr != end) ||
      !std::isfinite(parsed)) {
    return false;
  }
  *outValue = parsed;
  return true;
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
  switch (e.type) {
  case CVarType::Bool: {
    bool parsed = false;
    if (!parse_bool_token(valueStr, &parsed)) {
      log_parse_rejection(name, valueStr, "invalid bool");
      return false;
    }
    e.value.b = parsed;
    return true;
  }
  case CVarType::Int: {
    int parsed = 0;
    if (!parse_int_token(valueStr, &parsed)) {
      log_parse_rejection(name, valueStr, "invalid int");
      return false;
    }
    e.value.i = parsed;
    return true;
  }
  case CVarType::Float: {
    float parsed = 0.0F;
    if (!parse_float_token(valueStr, &parsed)) {
      log_parse_rejection(name, valueStr, "invalid float");
      return false;
    }
    e.value.f = parsed;
    return true;
  }
  case CVarType::String: {
    if (std::strlen(valueStr) >= kMaxStringValLen) {
      log_parse_rejection(name, valueStr, "overlong string");
      return false;
    }
    std::snprintf(e.value.str, kMaxStringValLen - 1U + 1U, "%s", valueStr);
    e.value.str[kMaxStringValLen - 1U] = '\0';
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
    out[written].type = g_entries[i].type;
    ++written;
  }
  return written;
}

} // namespace engine::core
