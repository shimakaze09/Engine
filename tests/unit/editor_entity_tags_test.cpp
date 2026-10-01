// Verifies the Inspector's Tags row logic: a typed tag is added, a repeat
// ignoring case changes nothing, and an invalid tag or a full set is
// refused with a reason and the set unchanged; through the command
// history the first tag adds the Tags component, a further tag edits it,
// removing the last tag removes it, and each step undoes and redoes
// exactly.

#include "editor_commands.h"
#include "editor_entity_tags.h"
#include "editor_session.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

using engine::runtime::Entity;
using engine::runtime::TagSetComponent;
using engine::runtime::World;

int g_failures = 0;

void check(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

/// Binds a fresh world to the editor session; restores on destruction.
struct SessionWorldScope final {
  World *previousWorld = nullptr;

  explicit SessionWorldScope(World *world) noexcept {
    previousWorld = engine::editor::editor_session().world;
    engine::editor::editor_session().world = world;
  }

  ~SessionWorldScope() noexcept {
    engine::editor::inspector_abandon_pending_edit();
    engine::editor::editor_session().commandHistory.clear();
    engine::editor::editor_session().world = previousWorld;
  }
};

/// Number of tags the entity carries, or -1 without a Tags component.
int tag_count(const World &world, Entity entity) noexcept {
  const TagSetComponent *tags = world.get_tag_set_component_ptr(entity);
  return (tags != nullptr) ? static_cast<int>(tags->count) : -1;
}

void check_typed_tag_rule() {
  TagSetComponent current{};
  TagSetComponent next{};
  check((engine::editor::apply_typed_tag(current, "coin", &next) == nullptr) &&
            (next.count == 1U),
        "typed: a tag is added");
  current = next;
  check((engine::editor::apply_typed_tag(current, "COIN", &next) == nullptr) &&
            (next.count == 1U),
        "typed: a repeat ignoring case is no change and no error");
  check((engine::editor::apply_typed_tag(current, "two words", &next) !=
         nullptr) &&
            (next.count == 1U),
        "typed: an invalid tag is refused with a reason");
  for (int i = 0; i < 7; ++i) {
    char name[8] = {};
    std::snprintf(name, sizeof(name), "t%d", i);
    static_cast<void>(engine::editor::apply_typed_tag(current, name, &next));
    current = next;
  }
  check((engine::editor::apply_typed_tag(current, "ninth", &next) != nullptr) &&
            (next.count == TagSetComponent::kMaxTags),
        "typed: a full set refuses another tag with a reason");
}

void check_commands() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  check(world != nullptr, "commands: world");
  if (world == nullptr) {
    return;
  }
  world->end_frame_phase();
  SessionWorldScope scope(world.get());
  auto &history = engine::editor::editor_session().commandHistory;
  const Entity entity = world->create_scene_object();

  TagSetComponent one{};
  static_cast<void>(engine::runtime::tag_set_add(&one, "coin"));
  engine::editor::commit_entity_tags(entity, nullptr, one);
  check(tag_count(*world, entity) == 1, "commands: the first tag adds Tags");

  TagSetComponent two = one;
  static_cast<void>(engine::runtime::tag_set_add(&two, "gold"));
  engine::editor::commit_entity_tags(entity, &one, two);
  check(tag_count(*world, entity) == 2, "commands: a second tag edits Tags");

  TagSetComponent none{};
  engine::editor::commit_entity_tags(entity, &two, none);
  check(tag_count(*world, entity) == -1,
        "commands: removing the last tag removes Tags");

  history.undo();
  check(tag_count(*world, entity) == 2, "commands: undo restores the two tags");
  history.undo();
  check(tag_count(*world, entity) == 1, "commands: undo restores one tag");
  history.undo();
  check(tag_count(*world, entity) == -1,
        "commands: undo of the first tag removes Tags");
  history.redo();
  history.redo();
  const TagSetComponent *tags = world->get_tag_set_component_ptr(entity);
  check((tags != nullptr) && (tags->count == 2U) &&
            (std::strcmp(tags->tags[1], "gold") == 0),
        "commands: redo brings the tags back in order");
}

} // namespace

int main() {
  check_typed_tag_rule();
  check_commands();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d editor tag check(s) failed\n", g_failures);
    return 1;
  }
  return 0;
}
