#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace engine::runtime {

// Persistent cross-scene data: score, inventory, checkpoints, etc.
// Separate from World lifetime — survives scene transitions.
// Key-value store supporting both numeric and string values.
struct GameState final {
  static constexpr std::size_t kMaxEntries = 128U;
  static constexpr std::size_t kMaxKeyLength = 32U;
  static constexpr std::size_t kMaxStringLength = 64U;

  struct Entry final {
    char key[kMaxKeyLength] = {};
    float numericValue = 0.0F;
    char stringValue[kMaxStringLength] = {};
    bool isNumeric = true;
  };

  std::array<Entry, kMaxEntries> entries{};
  std::size_t entryCount = 0U;

  // Store a numeric value. Overwrites if key exists. Returns false if full.
  bool set_number(const char *key, float value) noexcept {
    if ((key == nullptr) || (key[0] == '\0')) {
      return false;
    }
    Entry *existing = find_entry(key);
    if (existing != nullptr) {
      existing->numericValue = value;
      existing->isNumeric = true;
      existing->stringValue[0] = '\0';
      return true;
    }
    if (entryCount >= kMaxEntries) {
      return false;
    }
    Entry &e = entries[entryCount];
    std::snprintf(e.key, kMaxKeyLength, "%s", key);
    e.numericValue = value;
    e.isNumeric = true;
    e.stringValue[0] = '\0';
    ++entryCount;
    return true;
  }

  // Store a string value. Overwrites if key exists. Returns false if full.
  bool set_string(const char *key, const char *value) noexcept {
    if ((key == nullptr) || (key[0] == '\0')) {
      return false;
    }
    Entry *existing = find_entry(key);
    if (existing != nullptr) {
      existing->isNumeric = false;
      existing->numericValue = 0.0F;
      std::snprintf(existing->stringValue, kMaxStringLength, "%s",
                    value ? value : "");
      return true;
    }
    if (entryCount >= kMaxEntries) {
      return false;
    }
    Entry &e = entries[entryCount];
    std::snprintf(e.key, kMaxKeyLength, "%s", key);
    e.isNumeric = false;
    e.numericValue = 0.0F;
    std::snprintf(e.stringValue, kMaxStringLength, "%s", value ? value : "");
    ++entryCount;
    return true;
  }

  // Get a numeric value. Returns 0.0 if not found or not numeric.
  float get_number(const char *key) const noexcept {
    const Entry *e = find_entry(key);
    if ((e == nullptr) || !e->isNumeric) {
      return 0.0F;
    }
    return e->numericValue;
  }

  // Get a string value. Returns nullptr if not found or not a string.
  const char *get_string(const char *key) const noexcept {
    const Entry *e = find_entry(key);
    if ((e == nullptr) || e->isNumeric) {
      return nullptr;
    }
    return e->stringValue;
  }

  // Check if a key exists.
  bool has(const char *key) const noexcept {
    return find_entry(key) != nullptr;
  }

  // Check if a key holds a numeric value.
  bool is_number(const char *key) const noexcept {
    const Entry *e = find_entry(key);
    return (e != nullptr) && e->isNumeric;
  }

  // Remove a key. Returns true if found.
  bool remove(const char *key) noexcept {
    if ((key == nullptr) || (key[0] == '\0')) {
      return false;
    }
    for (std::size_t i = 0U; i < entryCount; ++i) {
      if (std::strcmp(entries[i].key, key) == 0) {
        entries[i] = entries[entryCount - 1U];
        entries[entryCount - 1U] = {};
        --entryCount;
        return true;
      }
    }
    return false;
  }

  // Clear all entries.
  void clear() noexcept {
    for (std::size_t i = 0U; i < entryCount; ++i) {
      entries[i] = {};
    }
    entryCount = 0U;
  }

private:
  Entry *find_entry(const char *key) noexcept {
    if ((key == nullptr) || (key[0] == '\0')) {
      return nullptr;
    }
    for (std::size_t i = 0U; i < entryCount; ++i) {
      if (std::strcmp(entries[i].key, key) == 0) {
        return &entries[i];
      }
    }
    return nullptr;
  }

  const Entry *find_entry(const char *key) const noexcept {
    if ((key == nullptr) || (key[0] == '\0')) {
      return nullptr;
    }
    for (std::size_t i = 0U; i < entryCount; ++i) {
      if (std::strcmp(entries[i].key, key) == 0) {
        return &entries[i];
      }
    }
    return nullptr;
  }
};

} // namespace engine::runtime
