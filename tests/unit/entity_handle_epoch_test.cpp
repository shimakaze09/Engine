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

  std::printf("entity_handle_epoch_test: all tests passed\n");
  return 0;
}
