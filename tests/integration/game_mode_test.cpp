#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/runtime/game_mode.h"
#include "engine/runtime/game_state.h"
#include "engine/runtime/player_controller.h"
#include "engine/runtime/world.h"

static int g_testsPassed = 0;
static int g_testsFailed = 0;

static void check(bool condition, const char *name) noexcept {
  if (condition) {
    ++g_testsPassed;
  } else {
    ++g_testsFailed;
    std::fprintf(stderr, "FAIL: %s\n", name);
  }
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

static bool test_player_controller_array() noexcept {
  engine::runtime::PlayerControllerArray pca;

  check(pca.set_controlled_entity(0, 42U), "set player 0 entity 42");
  check(pca.get_controlled_entity(0) == 42U, "get player 0");
  check(pca.controllers[0].active, "player 0 active");

  check(pca.set_controlled_entity(3, 99U), "set player 3 entity 99");
  check(pca.get_controlled_entity(3) == 99U, "get player 3");

  // Out of range
  check(!pca.set_controlled_entity(4, 1U), "set player 4 OOB");
  check(pca.get_controlled_entity(4) == 0U, "get player 4 OOB");

  // Entity destroyed
  pca.on_entity_destroyed(42U);
  check(pca.get_controlled_entity(0) == 0U, "player 0 cleared on destroy");
  check(!pca.controllers[0].active, "player 0 inactive after destroy");
  check(pca.get_controlled_entity(3) == 99U, "player 3 unaffected");

  // Reset
  pca.reset();
  check(pca.get_controlled_entity(0) == 0U, "reset player 0");
  check(pca.get_controlled_entity(3) == 0U, "reset player 3");
  return true;
}

static bool test_game_state_persists_across_worlds() noexcept {
  // GameState is separate from World — verify it survives World
  // reconstruction.
  engine::runtime::GameState gs;
  gs.set_number("level", 5.0F);
  gs.set_string("name", "player1");

  // Simulate world reset (GameState is independent).
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

int main() {
  test_game_mode_owned_by_world();
  test_game_mode_state_transitions();
  test_game_mode_invalid_transitions();
  test_game_mode_rules();
  test_game_mode_reset();
  test_game_state_numbers();
  test_game_state_strings();
  test_game_state_remove_and_clear();
  test_player_controller_array();
  test_game_state_persists_across_worlds();

  std::fprintf(stdout, "GameMode/GameState tests: %d passed, %d failed\n",
               g_testsPassed, g_testsFailed);
  return (g_testsFailed == 0) ? 0 : 1;
}
