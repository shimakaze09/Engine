// Verifies game mode test behavior for the Engine test suite.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/runtime/game_mode.h"
#include "engine/runtime/game_state.h"
#include "engine/runtime/player_controller.h"
#include "engine/runtime/world.h"
#include "../test_harness.h"

static engine::tests::TestContext g_tests;

static void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

static bool test_game_mode_owned_by_world() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (!world) {
    check(false, "world allocation");
    return false;
  }
  auto &gm = world->game_mode();
  check(gm.state == engine::runtime::GameMode::State::WaitingToStart,
        "initial state is WaitingToStart");
  check(gm.maxPlayers == 1U, "default maxPlayers is 1");
  check(gm.ruleCount == 0U, "no rules initially");
  return true;
}

static bool test_game_mode_state_transitions() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (!world) {
    check(false, "world allocation");
    return false;
  }
  auto &gm = world->game_mode();

  // WaitingToStart → InProgress
  check(gm.start(), "start from WaitingToStart succeeds");
  check(gm.state == engine::runtime::GameMode::State::InProgress,
        "state is InProgress after start");

  // InProgress → Paused
  check(gm.pause(), "pause from InProgress succeeds");
  check(gm.state == engine::runtime::GameMode::State::Paused,
        "state is Paused after pause");

  // Paused → InProgress (resume)
  check(gm.start(), "start from Paused succeeds (resume)");
  check(gm.state == engine::runtime::GameMode::State::InProgress,
        "state is InProgress after resume");

  // InProgress → Ended
  check(gm.end(), "end from InProgress succeeds");
  check(gm.state == engine::runtime::GameMode::State::Ended,
        "state is Ended after end");

  // Ended → cannot start
  check(!gm.start(), "start from Ended fails");
  check(!gm.pause(), "pause from Ended fails");
  check(!gm.end(), "end from Ended fails (already ended)");
  return true;
}

static bool test_game_mode_invalid_transitions() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (!world) {
    check(false, "world allocation");
    return false;
  }
  auto &gm = world->game_mode();

  // Cannot pause from WaitingToStart
  check(!gm.pause(), "pause from WaitingToStart fails");

  // Can end from WaitingToStart
  check(gm.end(), "end from WaitingToStart succeeds");
  return true;
}

static bool test_game_mode_rules() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (!world) {
    check(false, "world allocation");
    return false;
  }
  auto &gm = world->game_mode();

  check(gm.set_rule("max_score", "100"), "set_rule max_score");
  check(gm.set_rule("time_limit", "300"), "set_rule time_limit");
  check(gm.ruleCount == 2U, "ruleCount is 2");

  const char *v = gm.get_rule("max_score");
  check(v != nullptr, "get_rule max_score non-null");
  check(std::strcmp(v, "100") == 0, "get_rule max_score value");

  // Overwrite
  check(gm.set_rule("max_score", "200"), "overwrite max_score");
  check(gm.ruleCount == 2U, "ruleCount still 2 after overwrite");
  v = gm.get_rule("max_score");
  check(v != nullptr && std::strcmp(v, "200") == 0,
        "overwritten max_score value");

  // Not found
  check(gm.get_rule("nonexistent") == nullptr, "get_rule nonexistent");
  check(gm.get_rule(nullptr) == nullptr, "get_rule null key");
  check(!gm.set_rule(nullptr, "a"), "set_rule null key fails");
  check(!gm.set_rule("", "a"), "set_rule empty key fails");
  return true;
}

static bool test_game_mode_reset() noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (!world) {
    check(false, "world allocation");
    return false;
  }
  auto &gm = world->game_mode();

  std::snprintf(gm.name, sizeof(gm.name), "%s", "deathmatch");
  gm.start();
  gm.set_rule("key", "val");

  gm.reset();
  check(gm.state == engine::runtime::GameMode::State::WaitingToStart,
        "reset state");
  check(std::strcmp(gm.name, "default") == 0, "reset name");
  check(gm.ruleCount == 0U, "reset rules");
  check(gm.maxPlayers == 1U, "reset maxPlayers");
  return true;
}

static bool test_game_state_numbers() noexcept {
  engine::runtime::GameState gs;
  check(gs.set_number("score", 42.0F), "set_number score");
  check(gs.set_number("health", 100.0F), "set_number health");
  check(gs.entryCount == 2U, "entryCount 2");

  check(gs.get_number("score") == 42.0F, "get_number score");
  check(gs.get_number("health") == 100.0F, "get_number health");
  check(gs.has("score"), "has score");
  check(gs.is_number("score"), "is_number score");

  // Overwrite
  gs.set_number("score", 99.0F);
  check(gs.get_number("score") == 99.0F, "overwritten score");
  check(gs.entryCount == 2U, "entryCount still 2");
  return true;
}

static bool test_game_state_strings() noexcept {
  engine::runtime::GameState gs;
  check(gs.set_string("checkpoint", "level3_start"), "set_string");
  check(gs.has("checkpoint"), "has checkpoint");
  check(!gs.is_number("checkpoint"), "is_number false for string");

  const char *v = gs.get_string("checkpoint");
  check(v != nullptr && std::strcmp(v, "level3_start") == 0,
        "get_string value");

  // Numeric get on string returns 0
  check(gs.get_number("checkpoint") == 0.0F, "get_number on string key");

  // String get on numeric returns null
  gs.set_number("hp", 50.0F);
  check(gs.get_string("hp") == nullptr, "get_string on numeric key");
  return true;
}

static bool test_game_state_remove_and_clear() noexcept {
  engine::runtime::GameState gs;
  gs.set_number("a", 1.0F);
  gs.set_number("b", 2.0F);
  gs.set_string("c", "hello");

  check(gs.remove("b"), "remove b");
  check(!gs.has("b"), "b gone");
  check(gs.entryCount == 2U, "entryCount 2 after remove");
  check(gs.has("a"), "a still present");
  check(gs.has("c"), "c still present");

  check(!gs.remove("nonexistent"), "remove nonexistent fails");

  gs.clear();
  check(gs.entryCount == 0U, "clear entryCount");
  check(!gs.has("a"), "a gone after clear");
  return true;
}

// Regression for issue #344: keys are identity, so a key that cannot fit its
// fixed slot must be rejected up front — a truncated store would report
// success for an entry the full-string lookups can never reach, and repeated
// writes of such a key would consume slots for unreachable entries.
static bool test_game_state_key_length_boundary() noexcept {
  using engine::runtime::GameState;
  GameState gs;

  // 31 characters + NUL fills the slot exactly; one more cannot fit.
  char maxKey[GameState::kMaxKeyLength] = {};
  std::memset(maxKey, 'k', GameState::kMaxKeyLength - 1U);
  char longKeyA[GameState::kMaxKeyLength + 1U] = {};
  std::memset(longKeyA, 'k', GameState::kMaxKeyLength);
  char longKeyB[GameState::kMaxKeyLength + 1U] = {};
  std::memset(longKeyB, 'k', GameState::kMaxKeyLength);
  longKeyB[GameState::kMaxKeyLength - 1U] = 'B';

  check(gs.set_number(maxKey, 7.0F), "31-char key accepted");
  check(gs.entryCount == 1U, "31-char key stored one entry");
  check(gs.has(maxKey), "31-char key retrievable");
  check(gs.get_number(maxKey) == 7.0F, "31-char key value exact");

  check(!gs.set_number(longKeyA, 1.0F), "32-char numeric key rejected");
  check(!gs.set_string(longKeyA, "v"), "32-char string key rejected");
  check(gs.entryCount == 1U, "rejected keys stored nothing");
  check(!gs.has(longKeyA), "rejected key not present");
  check(gs.get_number(longKeyA) == 0.0F,
        "rejected key reads as absent number");
  check(gs.get_string(longKeyA) == nullptr,
        "rejected key reads as absent string");
  check(!gs.remove(longKeyA), "rejected key cannot be removed");

  // longKeyA and longKeyB share maxKey's 31 characters as their prefix; the
  // rejected writes must not have redirected into the fitting key's slot.
  check(!gs.set_number(longKeyB, 2.0F),
        "distinct shared-prefix over-long key rejected");
  check(gs.entryCount == 1U, "shared-prefix rejections stored nothing");
  check(gs.get_number(maxKey) == 7.0F, "31-char key untouched by rejections");

  // Repeated over-long writes must not consume slots.
  for (int i = 0; i < 3; ++i) {
    static_cast<void>(gs.set_number(longKeyA, static_cast<float>(i)));
  }
  check(gs.entryCount == 1U, "repeated rejected writes consume no slots");
  return true;
}

// Capacity boundary for issue #344's exhaustion half: a full table still
// overwrites existing keys, refuses new ones, and refuses over-long keys
// without disturbing stored entries.
static bool test_game_state_capacity_boundary() noexcept {
  using engine::runtime::GameState;
  GameState gs;

  char key[16] = {};
  bool allAccepted = true;
  for (std::size_t i = 0U; i < GameState::kMaxEntries; ++i) {
    std::snprintf(key, sizeof(key), "key_%03zu", i);
    allAccepted = allAccepted && gs.set_number(key, static_cast<float>(i));
  }
  check(allAccepted, "all in-capacity keys accepted");
  check(gs.entryCount == GameState::kMaxEntries, "table full");

  check(!gs.set_number("one_more", 1.0F), "new key rejected when full");
  check(gs.set_number("key_000", 9.0F), "overwrite still works when full");
  check(gs.get_number("key_000") == 9.0F, "overwrite at capacity lands");
  check(gs.entryCount == GameState::kMaxEntries,
        "overwrite at capacity adds no entry");

  char longKey[GameState::kMaxKeyLength + 1U] = {};
  std::memset(longKey, 'q', GameState::kMaxKeyLength);
  check(!gs.set_number(longKey, 1.0F), "over-long key rejected when full");
  check(gs.entryCount == GameState::kMaxEntries,
        "full table unchanged by over-long key");
  return true;
}

// Regression for issue #344 on GameMode: rule keys are identity with the
// same fixed slot, so an unfittable rule key is rejected rather than stored
// truncated and unreachable.
static bool test_game_mode_rule_key_length_boundary() noexcept {
  using engine::runtime::GameMode;
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (!world) {
    check(false, "world allocation");
    return false;
  }
  auto &gm = world->game_mode();

  char maxKey[GameMode::kMaxKeyLength] = {};
  std::memset(maxKey, 'r', GameMode::kMaxKeyLength - 1U);
  char longKeyA[GameMode::kMaxKeyLength + 1U] = {};
  std::memset(longKeyA, 'r', GameMode::kMaxKeyLength);
  char longKeyB[GameMode::kMaxKeyLength + 1U] = {};
  std::memset(longKeyB, 'r', GameMode::kMaxKeyLength);
  longKeyB[GameMode::kMaxKeyLength - 1U] = 'B';

  check(gm.set_rule(maxKey, "fits"), "31-char rule key accepted");
  check(gm.ruleCount == 1U, "31-char rule key stored one rule");
  const char *v = gm.get_rule(maxKey);
  check(v != nullptr && std::strcmp(v, "fits") == 0,
        "31-char rule key retrievable");

  check(!gm.set_rule(longKeyA, "a"), "32-char rule key rejected");
  check(!gm.set_rule(longKeyB, "b"),
        "distinct shared-prefix over-long rule key rejected");
  check(gm.ruleCount == 1U, "rejected rule keys stored nothing");
  check(gm.get_rule(longKeyA) == nullptr, "rejected rule key not present");
  v = gm.get_rule(maxKey);
  check(v != nullptr && std::strcmp(v, "fits") == 0,
        "31-char rule untouched by rejections");

  // Capacity: fill the remaining slots, then a full table refuses new and
  // over-long keys but still overwrites existing ones.
  char key[16] = {};
  bool allAccepted = true;
  for (std::size_t i = gm.ruleCount; i < GameMode::kMaxRules; ++i) {
    std::snprintf(key, sizeof(key), "rule_%02zu", i);
    allAccepted = allAccepted && gm.set_rule(key, "v");
  }
  check(allAccepted, "all in-capacity rule keys accepted");
  check(gm.ruleCount == GameMode::kMaxRules, "rule table full");
  check(!gm.set_rule("one_more", "v"), "new rule key rejected when full");
  check(!gm.set_rule(longKeyA, "v"), "over-long rule key rejected when full");
  check(gm.set_rule(maxKey, "changed"), "overwrite still works when full");
  v = gm.get_rule(maxKey);
  check(v != nullptr && std::strcmp(v, "changed") == 0,
        "overwrite at capacity lands");
  check(gm.ruleCount == GameMode::kMaxRules,
        "full rule table unchanged by rejections");
  return true;
}

static bool test_player_controller_array() noexcept {
  engine::runtime::PlayerControllerArray pca;
  constexpr engine::runtime::Entity kEntityA{42U, 1U};
  constexpr engine::runtime::Entity kEntityARecycled{42U, 2U};
  constexpr engine::runtime::Entity kEntityB{99U, 1U};

  check(pca.set_controlled_entity(0, kEntityA), "set player 0 entity 42");
  check(pca.get_controlled_entity(0) == kEntityA, "get player 0");
  check(pca.get_controlled_entity_index(0) == 42U, "get player 0 index");
  check(pca.controllers[0].active, "player 0 active");

  check(pca.set_controlled_entity(3, kEntityB), "set player 3 entity 99");
  check(pca.get_controlled_entity(3) == kEntityB, "get player 3");

  // Out of range
  check(!pca.set_controlled_entity(4, kEntityA), "set player 4 OOB");
  check(pca.get_controlled_entity(4) == engine::runtime::kInvalidEntity,
        "get player 4 OOB");

  // Recycled entity indices must not clear a controller for another generation.
  pca.on_entity_destroyed(kEntityARecycled);
  check(pca.get_controlled_entity(0) == kEntityA,
        "player 0 ignores recycled generation");

  pca.on_entity_destroyed(kEntityA);
  check(pca.get_controlled_entity(0) == engine::runtime::kInvalidEntity,
        "player 0 cleared on destroy");
  check(!pca.controllers[0].active, "player 0 inactive after destroy");
  check(pca.get_controlled_entity(3) == kEntityB, "player 3 unaffected");

  pca.reset();
  check(pca.get_controlled_entity(0) == engine::runtime::kInvalidEntity,
        "reset player 0");
  check(pca.get_controlled_entity(3) == engine::runtime::kInvalidEntity,
        "reset player 3");
  return true;
}

static bool test_game_state_persists_across_worlds() noexcept {
  // GameState is separate from World — verify it survives World
  // reconstruction.
  engine::runtime::GameState gs;
  gs.set_number("level", 5.0F);
  gs.set_string("name", "player1");

  {
    std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                      engine::runtime::World());
    if (world) {
      world->game_mode().start();
    }
  } // World destroyed

  // GameState must still have data.
  check(gs.get_number("level") == 5.0F, "level persists");
  check(gs.get_string("name") != nullptr &&
            std::strcmp(gs.get_string("name"), "player1") == 0,
        "name persists");
  return true;
}

/// Runs this executable or test program.
int main() {
  test_game_mode_owned_by_world();
  test_game_mode_state_transitions();
  test_game_mode_invalid_transitions();
  test_game_mode_rules();
  test_game_mode_reset();
  test_game_state_numbers();
  test_game_state_strings();
  test_game_state_remove_and_clear();
  test_game_state_key_length_boundary();
  test_game_state_capacity_boundary();
  test_game_mode_rule_key_length_boundary();
  test_player_controller_array();
  test_game_state_persists_across_worlds();

  return g_tests.finish("GameMode/GameState tests");
}
