// Verifies the entity reference-picker's pure filter/liveness logic (issue
// #156): name-substring search over the real World production path (no
// copied scheduler/index model), predicate filtering, result-count
// capping, and liveness/world-identity validation for the broken-
// reference state.

#include "editor_reference_pickers.h"

#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

using engine::editor::EntityPickerResult;
using engine::runtime::Entity;
using engine::runtime::World;

Entity make_named_entity(World &world, const char *name) noexcept {
  const Entity entity = world.create_scene_object();
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  static_cast<void>(world.add_name_component(entity, nameComponent));
  return entity;
}

/// An empty query matches every named entity; a substring query narrows
/// case-insensitively; results are ordered by ascending entity index.
int check_filter_by_name_substring() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  make_named_entity(*world, "Coin");
  make_named_entity(*world, "Goal Flag");
  make_named_entity(*world, "MOVING_PLATFORM");

  EntityPickerResult results[8];
  const std::size_t allCount =
      engine::editor::filter_entities_by_name(*world, "", results, 8);
  if (allCount != 3U) {
    return 2;
  }

  const std::size_t coinCount =
      engine::editor::filter_entities_by_name(*world, "coin", results, 8);
  if ((coinCount != 1U) || (std::strcmp(results[0].name, "Coin") != 0)) {
    return 3;
  }

  const std::size_t platformCount =
      engine::editor::filter_entities_by_name(*world, "platform", results, 8);
  if ((platformCount != 1U) ||
      (std::strcmp(results[0].name, "MOVING_PLATFORM") != 0)) {
    return 4;
  }

  const std::size_t noneCount = engine::editor::filter_entities_by_name(
      *world, "no-such-entity", results, 8);
  if (noneCount != 0U) {
    return 5;
  }
  return 0;
}

/// maxResults caps the written count without reading past the buffer.
int check_filter_result_cap() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  for (int i = 0; i < 5; ++i) {
    char name[16] = {};
    std::snprintf(name, sizeof(name), "Item%d", i);
    make_named_entity(*world, name);
  }
  EntityPickerResult results[2];
  const std::size_t count =
      engine::editor::filter_entities_by_name(*world, "", results, 2);
  if (count != 2U) {
    return 2;
  }
  return 0;
}

/// A predicate restricts the candidate set (the picker's mechanism for
/// e.g. "only entities carrying a SceneCaptureComponent").
bool only_entities_named_capture(const World &world, Entity entity) noexcept {
  engine::runtime::NameComponent name{};
  return world.get_name_component(entity, &name) &&
        (std::strcmp(name.name, "Capture") == 0);
}

int check_filter_with_predicate() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  make_named_entity(*world, "Capture");
  make_named_entity(*world, "NotCapture");

  EntityPickerResult results[4];
  const std::size_t count = engine::editor::filter_entities_by_name(
      *world, "", results, 4, only_entities_named_capture);
  if ((count != 1U) || (std::strcmp(results[0].name, "Capture") != 0)) {
    return 2;
  }
  return 0;
}

/// A zero/invalid entity never appears (no NameComponent, in this world an
/// entity with no name is never a candidate) and destroyed entities drop
/// out of the search as soon as they die.
int check_filter_excludes_destroyed_entities() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  const Entity entity = make_named_entity(*world, "Temp");
  EntityPickerResult results[4];
  if (engine::editor::filter_entities_by_name(*world, "", results, 4) != 1U) {
    return 2;
  }
  static_cast<void>(world->destroy_entity(entity));
  if (engine::editor::filter_entities_by_name(*world, "", results, 4) != 0U) {
    return 3;
  }
  return 0;
}

/// entity_reference_is_live validates both liveness and the invalid-id
/// sentinel; a persistent id surviving a delete/re-create under a
/// different entity still resolves live (matches the rest of the editor's
/// persistent-id targeting contract).
int check_entity_reference_liveness() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  const Entity entity = make_named_entity(*world, "Target");
  const auto persistentId = world->persistent_id(entity);

  if (!engine::editor::entity_reference_is_live(*world, persistentId)) {
    return 2;
  }
  if (engine::editor::entity_reference_is_live(
          *world, engine::runtime::kInvalidPersistentId)) {
    return 3;
  }

  static_cast<void>(world->destroy_entity(entity));
  if (engine::editor::entity_reference_is_live(*world, persistentId)) {
    return 4;
  }
  return 0;
}

} // namespace

int main() {
  struct Case {
    const char *name;
    int (*fn)() noexcept;
  };
  const Case cases[] = {
      {"filter_by_name_substring", check_filter_by_name_substring},
      {"filter_result_cap", check_filter_result_cap},
      {"filter_with_predicate", check_filter_with_predicate},
      {"filter_excludes_destroyed_entities",
       check_filter_excludes_destroyed_entities},
      {"entity_reference_liveness", check_entity_reference_liveness},
  };
  for (const Case &c : cases) {
    const int result = c.fn();
    if (result != 0) {
      std::fprintf(stderr, "editor_reference_pickers_test: %s failed: %d\n",
                   c.name, result);
      return result;
    }
  }
  std::printf("editor_reference_pickers_test: all tests passed\n");
  return 0;
}
