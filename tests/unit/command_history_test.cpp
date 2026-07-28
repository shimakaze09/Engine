// Verifies editor command history ownership behavior.

#include "editor_commands.h"
#include "editor_transform_util.h"
#include "engine/editor/command_history.h"
#include "engine/math/transform.h"
#include "engine/runtime/world.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

namespace {

int g_liveCommands = 0;
int g_destroyedCommands = 0;
int g_executeCount = 0;
int g_undoCount = 0;
int g_redoCount = 0;

void reset_counts() noexcept {
  g_liveCommands = 0;
  g_destroyedCommands = 0;
  g_executeCount = 0;
  g_undoCount = 0;
  g_redoCount = 0;
}

struct CountingCommand final : engine::editor::EditorCommand {
  CountingCommand() noexcept { ++g_liveCommands; }

  ~CountingCommand() override {
    --g_liveCommands;
    ++g_destroyedCommands;
  }

  void execute() noexcept override { ++g_executeCount; }

  void undo() noexcept override { ++g_undoCount; }

  void redo() noexcept override { ++g_redoCount; }
};

engine::editor::EditorCommand *make_command() noexcept {
  return new (std::nothrow) CountingCommand();
}

/// Returns whether two editor-transform scalar values match tightly.
bool nearly_equal(float lhs, float rhs) noexcept {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

/// Returns whether two transform matrices represent the same affine mapping.
bool matrices_nearly_equal(const engine::math::Mat4 &lhs,
                           const engine::math::Mat4 &rhs) noexcept {
  for (std::size_t column = 0U; column < 4U; ++column) {
    if (!nearly_equal(lhs.columns[column].x, rhs.columns[column].x) ||
        !nearly_equal(lhs.columns[column].y, rhs.columns[column].y) ||
        !nearly_equal(lhs.columns[column].z, rhs.columns[column].z) ||
        !nearly_equal(lhs.columns[column].w, rhs.columns[column].w)) {
      return false;
    }
  }
  return true;
}

/// Verifies a parented gizmo world matrix converts back to local TRS and
/// preserves the selected object's parent identity.
int check_parented_gizmo_world_to_local_conversion() noexcept {
  constexpr float kHalfPi = 1.57079632679F;
  const engine::math::Mat4 parentWorld = engine::math::compose_trs(
      engine::math::Vec3(10.0F, -2.0F, 5.0F),
      engine::math::from_axis_angle(engine::math::Vec3(0.0F, 0.0F, 1.0F),
                                    kHalfPi),
      engine::math::Vec3(2.0F, 2.0F, 2.0F));

  engine::runtime::Transform expected{};
  expected.position = engine::math::Vec3(4.0F, -1.0F, 2.0F);
  expected.rotation = engine::math::from_axis_angle(
      engine::math::Vec3(0.0F, 1.0F, 0.0F), kHalfPi);
  expected.scale = engine::math::Vec3(0.5F, 1.5F, 2.0F);
  expected.parentId = 77U;

  const engine::math::Mat4 expectedLocalMatrix = engine::math::compose_trs(
      expected.position, expected.rotation, expected.scale);
  const engine::math::Mat4 manipulatedWorld =
      engine::math::mul(parentWorld, expectedLocalMatrix);

  engine::runtime::Transform converted{};
  if (!engine::editor::world_matrix_to_local_transform(
          manipulatedWorld, &parentWorld, expected, &converted)) {
    return 1;
  }
  if (converted.parentId != expected.parentId) {
    return 2;
  }

  const engine::math::Mat4 convertedLocalMatrix = engine::math::compose_trs(
      converted.position, converted.rotation, converted.scale);
  return matrices_nearly_equal(convertedLocalMatrix, expectedLocalMatrix) ? 0
                                                                          : 3;
}

int check_destructor_releases_commands() noexcept {
  reset_counts();
  {
    engine::editor::CommandHistory history{};
    history.execute(make_command());
    history.execute(make_command());
    if (g_liveCommands != 2) {
      return 10;
    }
  }

  if ((g_liveCommands != 0) || (g_destroyedCommands != 2)) {
    return 11;
  }
  return 0;
}

int check_redo_entries_are_released() noexcept {
  reset_counts();
  {
    engine::editor::CommandHistory history{};
    history.execute(make_command());
    history.execute(make_command());
    history.undo();
    if (!history.can_redo()) {
      return 20;
    }

    history.execute(make_command());
    if (history.can_redo()) {
      return 21;
    }
    if ((g_liveCommands != 2) || (g_destroyedCommands != 1)) {
      return 22;
    }
  }

  if ((g_liveCommands != 0) || (g_destroyedCommands != 3)) {
    return 23;
  }
  return 0;
}

int check_capacity_eviction_releases_oldest_command() noexcept {
  reset_counts();
  {
    engine::editor::CommandHistory history{};
    for (std::size_t i = 0U;
         i < engine::editor::CommandHistory::kMaxHistory + 1U; ++i) {
      history.execute(make_command());
    }

    if ((g_liveCommands !=
         static_cast<int>(engine::editor::CommandHistory::kMaxHistory)) ||
        (g_destroyedCommands != 1)) {
      return 30;
    }
  }

  if ((g_liveCommands != 0) ||
      (g_destroyedCommands !=
       static_cast<int>(engine::editor::CommandHistory::kMaxHistory + 1U))) {
    return 31;
  }
  return 0;
}

int check_undo_redo_dispatch() noexcept {
  reset_counts();
  engine::editor::CommandHistory history{};
  history.execute(make_command());

  if ((g_executeCount != 1) || !history.can_undo() || history.can_redo()) {
    return 40;
  }

  history.undo();
  if ((g_undoCount != 1) || history.can_undo() || !history.can_redo()) {
    return 41;
  }

  history.redo();
  if ((g_redoCount != 1) || !history.can_undo() || history.can_redo()) {
    return 42;
  }
  return 0;
}

/// Undo and redo must not target a replacement entity that reuses the slot.
int check_component_command_rejects_recycled_entity() noexcept {
  using engine::editor::ComponentEditCommand;
  using engine::editor::ComponentEditSnapshot;
  using engine::editor::ComponentEditType;
  using engine::runtime::Entity;
  using engine::runtime::NameComponent;
  using engine::runtime::World;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 50;
  }

  auto &session = engine::editor::editor_session();
  World *const previousWorld = session.world;
  session.world = world.get();
  const auto finish = [&session, previousWorld](int result) noexcept {
    session.world = previousWorld;
    return result;
  };

  const Entity original = world->create_entity();
  if (original == engine::runtime::kInvalidEntity) {
    return finish(51);
  }

  ComponentEditSnapshot added{};
  std::snprintf(added.name.name, sizeof(added.name.name), "%s", "Original");
  auto *command = new (std::nothrow) ComponentEditCommand();
  if (command == nullptr) {
    return finish(52);
  }
  command->entity = original;
  command->type = ComponentEditType::Name;
  command->beforeExists = false;
  command->afterExists = true;
  command->after = added;

  engine::editor::CommandHistory history{};
  history.execute(command);

  NameComponent current{};
  if (!world->get_name_component(original, &current) ||
      (std::strcmp(current.name, "Original") != 0)) {
    return finish(53);
  }
  if (!world->destroy_entity(original)) {
    return finish(54);
  }

  const Entity replacement = world->create_entity();
  if ((replacement == engine::runtime::kInvalidEntity) ||
      (replacement.index != original.index) ||
      (replacement.generation == original.generation)) {
    return finish(55);
  }

  NameComponent replacementName{};
  std::snprintf(replacementName.name, sizeof(replacementName.name), "%s",
                "Replacement");
  if (!world->add_name_component(replacement, replacementName)) {
    return finish(56);
  }

  history.undo();
  NameComponent afterUndo{};
  if (!world->get_name_component(replacement, &afterUndo) ||
      (std::strcmp(afterUndo.name, "Replacement") != 0)) {
    return finish(57);
  }

  history.redo();
  NameComponent afterRedo{};
  if (!world->get_name_component(replacement, &afterRedo) ||
      (std::strcmp(afterRedo.name, "Replacement") != 0)) {
    return finish(58);
  }

  return finish(0);
}

} // namespace

static_assert(!std::is_copy_constructible_v<engine::editor::CommandHistory>);
static_assert(!std::is_copy_assignable_v<engine::editor::CommandHistory>);
static_assert(!std::is_move_constructible_v<engine::editor::CommandHistory>);
static_assert(!std::is_move_assignable_v<engine::editor::CommandHistory>);

int main() {
  int result = check_parented_gizmo_world_to_local_conversion();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_destructor_releases_commands();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_redo_entries_are_released();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_capacity_eviction_releases_oldest_command();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_undo_redo_dispatch();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_component_command_rejects_recycled_entity();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  std::printf("command_history_test: all tests passed\n");
  return 0;
}
