// Verifies the entity-handle world-epoch boundary (audit C-03): a handle
// retained across a whole-world replacement must be rejected by every
// decode path even when the replacement world reuses the same entity
// index and generation, while fresh handles from the new contents keep
// working.

#include "engine/core/json.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/bindable_api.h"
#include "entity_handle_value.h"
#include "runtime_binding.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

/// EXPECTATION: after load_scene replaces the bound world's contents, a
/// handle from the old contents fails decode/liveness even though the new
/// world holds an entity with the same index and generation; a handle
/// encoded from the new contents succeeds; reset_world invalidates again.
int check_handle_rejected_after_world_replacement() {
  using namespace engine::runtime;
  using namespace engine::scripting;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  runtime_binding().world = world.get();
  const auto finish = [](int result) noexcept {
    runtime_binding().world = nullptr;
    return result;
  };

  const Entity original = world->create_scene_object();
  if (original == kInvalidEntity) {
    return finish(2);
  }
  NameComponent name{};
  std::snprintf(name.name, sizeof(name.name), "%s", "Marker");
  if (!world->add_name_component(original, name)) {
    return finish(3);
  }

  std::uint64_t retainedHandle = 0ULL;
  if (!encode_entity_handle_value(original, &retainedHandle) ||
      !bindable_is_alive(retainedHandle)) {
    return finish(4);
  }

  std::unique_ptr<std::array<char, engine::core::JsonWriter::kBufferBytes>>
      buffer(new (std::nothrow)
                 std::array<char, engine::core::JsonWriter::kBufferBytes>());
  if (buffer == nullptr) {
    return finish(5);
  }
  std::size_t size = 0U;
  if (!save_scene(*world, buffer->data(), buffer->size(), &size)) {
    return finish(6);
  }
  if (!load_scene(*world, buffer->data(), size)) {
    return finish(7);
  }

  const Entity reloaded = world->find_entity_by_name("Marker");
  if (reloaded == kInvalidEntity) {
    return finish(8);
  }
  if ((reloaded.index != original.index) ||
      (reloaded.generation != original.generation)) {
    return finish(9);
  }

  if (bindable_is_alive(retainedHandle)) {
    return finish(10);
  }
  Entity decoded{};
  if (decode_entity_handle_value(retainedHandle, &decoded)) {
    return finish(11);
  }

  std::uint64_t freshHandle = 0ULL;
  if (!encode_entity_handle_value(reloaded, &freshHandle) ||
      (freshHandle == retainedHandle) ||
      !bindable_is_alive(freshHandle)) {
    return finish(12);
  }

  reset_world(*world);
  if (bindable_is_alive(freshHandle)) {
    return finish(13);
  }

  return finish(0);
}

/// EXPECTATION: encoding fails cleanly for invalid entities and for a
/// generation beyond the handle's bit budget.
int check_handle_encoding_bounds() {
  using namespace engine::runtime;
  using namespace engine::scripting;

  std::uint64_t handle = 0ULL;
  if (encode_entity_handle_value(kInvalidEntity, &handle)) {
    return 20;
  }
  Entity oversizedGeneration{1U, (1U << 26U) + 2U};
  if (encode_entity_handle_value(oversizedGeneration, &handle)) {
    return 21;
  }
  return 0;
}

/// EXPECTATION: encoding requires the entity to be alive in the bound
/// world — dead entities and unbound worlds produce no handle.
int check_encode_requires_live_entity() {
  using namespace engine::runtime;
  using namespace engine::scripting;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 30;
  }
  runtime_binding().world = world.get();
  const auto finish = [](int result) noexcept {
    runtime_binding().world = nullptr;
    return result;
  };

  const Entity entity = world->create_scene_object();
  if (entity == kInvalidEntity) {
    return finish(31);
  }
  std::uint64_t handle = 0ULL;
  if (!encode_entity_handle_value(entity, &handle)) {
    return finish(32);
  }
  if (!world->destroy_entity(entity)) {
    return finish(33);
  }
  if (encode_entity_handle_value(entity, &handle)) {
    return finish(34);
  }

  runtime_binding().world = nullptr;
  const Entity fresh{1U, 1U};
  if (encode_entity_handle_value(fresh, &handle)) {
    return finish(35);
  }
  return finish(0);
}

/// EXPECTATION: the 17-bit epoch field wraps after 131072 replacements —
/// a handle exactly one wrap old aliases again. This pins the documented
/// bound of the stale-handle guarantee (an exhaustion warning fires when
/// the raw epoch first exceeds the field).
int check_epoch_wrap_documented_alias() {
  using namespace engine::runtime;
  using namespace engine::scripting;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 40;
  }
  runtime_binding().world = world.get();
  const auto finish = [](int result) noexcept {
    runtime_binding().world = nullptr;
    return result;
  };

  const Entity entity = world->create_scene_object();
  if (entity == kInvalidEntity) {
    return finish(41);
  }
  std::uint64_t epochZeroHandle = 0ULL;
  if (!encode_entity_handle_value(entity, &epochZeroHandle)) {
    return finish(42);
  }

  world->mark_content_replaced(0U);
  Entity decoded{};
  if (decode_entity_handle_value(epochZeroHandle, &decoded)) {
    return finish(43);
  }

  world->mark_content_replaced((1U << 17U) - 1U);
  if (!decode_entity_handle_value(epochZeroHandle, &decoded) ||
      (decoded.index != entity.index)) {
    return finish(44);
  }
  return finish(0);
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_handle_rejected_after_world_replacement();
  if (result != 0) {
    std::fprintf(stderr, "entity_handle_epoch_test failed: %d\n", result);
    return result;
  }

  result = check_handle_encoding_bounds();
  if (result != 0) {
    std::fprintf(stderr, "entity_handle_epoch_test failed: %d\n", result);
    return result;
  }

  result = check_encode_requires_live_entity();
  if (result != 0) {
    std::fprintf(stderr, "entity_handle_epoch_test failed: %d\n", result);
    return result;
  }

  result = check_epoch_wrap_documented_alias();
  if (result != 0) {
    std::fprintf(stderr, "entity_handle_epoch_test failed: %d\n", result);
    return result;
  }

  std::printf("entity_handle_epoch_test: all tests passed\n");
  return 0;
}
