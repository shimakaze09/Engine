#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace engine::runtime {

// Describes the current game mode — rules, state machine, and player limits.
// Owned by World. One GameMode per World instance.
struct GameMode final {
  enum class State : std::uint8_t {
    WaitingToStart = 0,
    InProgress,
    Paused,
    Ended,
  };

  static constexpr std::size_t kMaxRules = 32U;
  static constexpr std::size_t kMaxKeyLength = 32U;
  static constexpr std::size_t kMaxValueLength = 64U;
  static constexpr std::size_t kMaxNameLength = 64U;

  struct Rule final {
    char key[kMaxKeyLength] = {};
    char value[kMaxValueLength] = {};
  };

  char name[kMaxNameLength] = "default";
  State state = State::WaitingToStart;
  std::uint32_t maxPlayers = 1U;

  std::array<Rule, kMaxRules> rules{};
  std::size_t ruleCount = 0U;

  // Set a rule key-value pair. Overwrites if key exists. Returns false if full.
  bool set_rule(const char *key, const char *value) noexcept {
    if ((key == nullptr) || (key[0] == '\0')) {
      return false;
    }
    // Overwrite existing.
    for (std::size_t i = 0U; i < ruleCount; ++i) {
      if (std::strcmp(rules[i].key, key) == 0) {
        std::snprintf(rules[i].value, kMaxValueLength, "%s",
                      value ? value : "");
        return true;
      }
    }
    if (ruleCount >= kMaxRules) {
      return false;
    }
    std::snprintf(rules[ruleCount].key, kMaxKeyLength, "%s", key);
    std::snprintf(rules[ruleCount].value, kMaxValueLength, "%s",
                  value ? value : "");
    ++ruleCount;
    return true;
  }

  // Get a rule value. Returns nullptr if not found.
  const char *get_rule(const char *key) const noexcept {
    if ((key == nullptr) || (key[0] == '\0')) {
      return nullptr;
    }
    for (std::size_t i = 0U; i < ruleCount; ++i) {
      if (std::strcmp(rules[i].key, key) == 0) {
        return rules[i].value;
      }
    }
    return nullptr;
  }

  // Reset to default state.
  void reset() noexcept {
    std::snprintf(name, kMaxNameLength, "%s", "default");
    state = State::WaitingToStart;
    maxPlayers = 1U;
    ruleCount = 0U;
    rules = {};
  }

  // Transition to InProgress. Returns false if not in WaitingToStart or Paused.
  bool start() noexcept {
    if ((state != State::WaitingToStart) && (state != State::Paused)) {
      return false;
    }
    state = State::InProgress;
    return true;
  }

  // Transition to Paused. Returns false if not InProgress.
  bool pause() noexcept {
    if (state != State::InProgress) {
      return false;
    }
    state = State::Paused;
    return true;
  }

  // Transition to Ended. Returns false if already Ended.
  bool end() noexcept {
    if (state == State::Ended) {
      return false;
    }
    state = State::Ended;
    return true;
  }
};

} // namespace engine::runtime
