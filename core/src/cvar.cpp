#include "engine/core/cvar.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>


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

int find_cvar(const char *name) noexcept {
  for (std::size_t i = 0U; i < g_count; ++i) {
    if (g_entries[i].used && std::strcmp(g_entries[i].name, name) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

} // namespace

bool initialize_cvars() noexcept {
  if (g_initialized) {
    return true;
  }
  g_entries = {};
  g_count = 0U;
  g_initialized = true;
  return true;
}

void shutdown_cvars() noexcept {
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
  if (g_count >= kMaxCVars) {
    return false;
  }
  if (find_cvar(name) >= 0) {
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
  if (g_count >= kMaxCVars) {
    return false;
  }
  if (find_cvar(name) >= 0) {
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
  if (g_count >= kMaxCVars) {
    return false;
  }
  if (find_cvar(name) >= 0) {
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
  if (g_count >= kMaxCVars) {
    return false;
  }
  if (find_cvar(name) >= 0) {
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
  const int idx = find_cvar(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::Bool)) {
    return fallback;
  }
  return g_entries[idx].value.b;
}

int cvar_get_int(const char *name, int fallback) noexcept {
  const int idx = find_cvar(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::Int)) {
    return fallback;
  }
  return g_entries[idx].value.i;
}

float cvar_get_float(const char *name, float fallback) noexcept {
  const int idx = find_cvar(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::Float)) {
    return fallback;
  }
  return g_entries[idx].value.f;
}

const char *cvar_get_string(const char *name, const char *fallback) noexcept {
  const int idx = find_cvar(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::String)) {
    return fallback;
  }
  return g_entries[idx].value.str;
}

// ---- setters ----

bool cvar_set_bool(const char *name, bool value) noexcept {
  const int idx = find_cvar(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::Bool)) {
    return false;
  }
  g_entries[idx].value.b = value;
  return true;
}

bool cvar_set_int(const char *name, int value) noexcept {
  const int idx = find_cvar(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::Int)) {
    return false;
  }
  g_entries[idx].value.i = value;
  return true;
}

bool cvar_set_float(const char *name, float value) noexcept {
  const int idx = find_cvar(name);
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
  const int idx = find_cvar(name);
  if ((idx < 0) || (g_entries[idx].type != CVarType::String)) {
    return false;
  }
  std::snprintf(g_entries[idx].value.str, kMaxStringValLen - 1U + 1U, "%s",
                value);
  g_entries[idx].value.str[kMaxStringValLen - 1U] = '\0';
  return true;
}

bool cvar_set_from_string(const char *name, const char *valueStr) noexcept {
  if ((name == nullptr) || (valueStr == nullptr)) {
    return false;
  }
  const int idx = find_cvar(name);
  if (idx < 0) {
    return false;
  }

  CVarEntry &e = g_entries[idx];
  switch (e.type) {
  case CVarType::Bool:
    e.value.b =
        (std::strcmp(valueStr, "1") == 0 || std::strcmp(valueStr, "true") == 0);
    return true;
  case CVarType::Int: {
    char *end = nullptr;
    const long parsed = std::strtol(valueStr, &end, 10);
    if ((end == nullptr) || (end == valueStr)) {
      return false;
    }
    e.value.i = static_cast<int>(parsed);
    return true;
  }
  case CVarType::Float: {
    char *end = nullptr;
    const float parsed = std::strtof(valueStr, &end);
    if ((end == nullptr) || (end == valueStr)) {
      return false;
    }
    e.value.f = parsed;
    return true;
  }
  case CVarType::String:
    std::snprintf(e.value.str, kMaxStringValLen - 1U + 1U, "%s", valueStr);
    e.value.str[kMaxStringValLen - 1U] = '\0';
    return true;
  default:
    return false;
  }
}

std::size_t cvar_get_all(CVarInfo *out, std::size_t maxEntries) noexcept {
  if (out == nullptr) {
    return 0U;
  }
  std::size_t written = 0U;
  for (std::size_t i = 0U; (i < g_count) && (written < maxEntries); ++i) {
    if (!g_entries[i].used) {
      continue;
    }
    out[written].name = g_entries[i].name;
    out[written].description = g_entries[i].desc;
    out[written].type = g_entries[i].type;
    ++written;
  }
  return written;
}

} // namespace engine::core
