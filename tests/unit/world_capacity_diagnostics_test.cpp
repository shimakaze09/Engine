// Pins that the World names capacity exhaustion instead of failing
// silently: a component add refused for want of a slot, an entity created
// past kMaxEntities, and a duplicate persistent id each return their
// failure value and log exactly one error, so a lost write is never mute.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/runtime/world.h"

namespace {

namespace rt = engine::runtime;

int g_failures = 0;
int g_fullErrors = 0;
int g_capacityErrors = 0;
int g_duplicateErrors = 0;

void check(bool condition, const char *name) noexcept {
  if (!condition) {
    std::printf("FAIL: %s\n", name);
    ++g_failures;
  }
}

void count_world_errors(engine::core::LogLevel level, const char *channel,
                        const char *message, void *) noexcept {
  if ((level != engine::core::LogLevel::Error) ||
      (std::strcmp(channel, "world") != 0)) {
    return;
  }
  if (std::strstr(message, "component storage is full") != nullptr) {
    ++g_fullErrors;
  }
  if (std::strstr(message, "entity capacity is full") != nullptr) {
    ++g_capacityErrors;
  }
  if (std::strstr(message, "persistent id already in use") != nullptr) {
    ++g_duplicateErrors;
  }
}

} // namespace

int main() {
  check(engine::core::initialize_logging(), "initialize logging");
  check(engine::core::log_register_sink(&count_world_errors, nullptr),
        "register sink");
  std::unique_ptr<rt::World> world(new (std::nothrow) rt::World());
  if (world == nullptr) {
    return 1;
  }

  // --- A component set at capacity refuses with one logged error ---
  for (std::size_t i = 0U; i < rt::World::kMaxSceneCaptureComponents; ++i) {
    const rt::Entity entity = world->create_scene_object();
    check(world->add_scene_capture_component(entity,
                                             rt::SceneCaptureComponent{}),
          "scene capture adds up to capacity");
  }
  check(g_fullErrors == 0, "adds within capacity log nothing");
  const rt::Entity onePast = world->create_scene_object();
  check(!world->add_scene_capture_component(onePast,
                                            rt::SceneCaptureComponent{}),
        "the add one past capacity is refused");
  check(g_fullErrors == 1, "the refused add logs exactly one error");

  // --- A duplicate persistent id is named ---
  const rt::Entity first = world->create_entity_with_persistent_id(4242U);
  check(first != rt::kInvalidEntity, "first persistent id is taken");
  check(world->create_entity_with_persistent_id(4242U) == rt::kInvalidEntity,
        "the duplicate persistent id is refused");
  check(g_duplicateErrors == 1, "the duplicate logs exactly one error");

  // --- Entity capacity is named ---
  std::size_t created = world->alive_entity_count();
  while (created < rt::World::kMaxEntities) {
    if (world->create_entity() == rt::kInvalidEntity) {
      break;
    }
    ++created;
  }
  check(created == rt::World::kMaxEntities, "the world fills to capacity");
  check(g_capacityErrors == 0, "creates within capacity log nothing");
  check(world->create_entity() == rt::kInvalidEntity,
        "the create one past capacity is refused");
  check(g_capacityErrors == 1, "the refused create logs exactly one error");

  engine::core::log_unregister_sink(&count_world_errors, nullptr);
  engine::core::shutdown_logging();
  if (g_failures != 0) {
    std::printf("world capacity diagnostics: %d failure(s)\n", g_failures);
    return 1;
  }
  return 0;
}
