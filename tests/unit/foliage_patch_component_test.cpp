#include <array>
#include <cmath>
#include <memory>
#include <new>

#include "engine/core/json.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

bool nearly_equal(float lhs, float rhs) noexcept {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

engine::runtime::FoliagePatchComponent make_test_foliage() noexcept {
  engine::runtime::FoliagePatchComponent foliage{};
  foliage.meshAssetIds[0] = 11U;
  foliage.meshAssetIds[1] = 22U;
  foliage.meshAssetIds[2] = 33U;
  foliage.instanceCount = 3U;
  foliage.density = 2.25F;
  foliage.albedo = engine::math::Vec3(0.2F, 0.7F, 0.3F);
  foliage.roughness = 0.91F;
  foliage.metallic = 0.05F;
  foliage.opacity = 0.85F;
  foliage.windStrength = 0.35F;
  foliage.windFrequency = 1.75F;

  foliage.instances[0].offset = engine::math::Vec3(-1.0F, 0.0F, 0.5F);
  foliage.instances[0].scale = 0.5F;
  foliage.instances[0].phase = 0.25F;
  foliage.instances[0].lodIndex = 0U;

  foliage.instances[1].offset = engine::math::Vec3(0.5F, 0.0F, 1.0F);
  foliage.instances[1].scale = 0.75F;
  foliage.instances[1].phase = 0.75F;
  foliage.instances[1].lodIndex = 1U;

  foliage.instances[2].offset = engine::math::Vec3(1.5F, 0.0F, -0.5F);
  foliage.instances[2].scale = 1.1F;
  foliage.instances[2].phase = 1.25F;
  foliage.instances[2].lodIndex = 2U;
  return foliage;
}

int verify_foliage_crud() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 2;
  }

  const engine::runtime::FoliagePatchComponent foliage = make_test_foliage();
  if (!world->add_foliage_patch_component(entity, foliage)) {
    return 3;
  }
  if (!world->has_foliage_patch_component(entity)) {
    return 4;
  }
  if (world->foliage_patch_count() != 1U) {
    return 5;
  }
  if (world->foliage_patch_entity_at(0U) != entity) {
    return 6;
  }

  engine::runtime::FoliagePatchComponent readBack{};
  if (!world->get_foliage_patch_component(entity, &readBack)) {
    return 7;
  }
  if ((readBack.meshAssetIds[1] != 22U) || (readBack.instanceCount != 3U) ||
      !nearly_equal(readBack.density, 2.25F) ||
      !nearly_equal(readBack.instances[2].phase, 1.25F) ||
      (readBack.instances[2].lodIndex != 2U)) {
    return 8;
  }

  int visited = 0;
  world->for_each<engine::runtime::FoliagePatchComponent>(
      [&](engine::runtime::Entity,
          const engine::runtime::FoliagePatchComponent &component) noexcept {
        if (nearly_equal(component.windStrength, 0.35F)) {
          ++visited;
        }
      });
  if (visited != 1) {
    return 9;
  }

  engine::runtime::FoliagePatchComponent *mutableFoliage =
      world->get_foliage_patch_component_ptr(entity);
  if (mutableFoliage == nullptr) {
    return 10;
  }
  mutableFoliage->density = 3.0F;
  if (!world->get_foliage_patch_component(entity, &readBack) ||
      !nearly_equal(readBack.density, 3.0F)) {
    return 11;
  }

  if (!world->destroy_entity(entity)) {
    return 12;
  }
  if (world->has_foliage_patch_component(entity) ||
      (world->foliage_patch_count() != 0U)) {
    return 13;
  }

  return 0;
}

int verify_foliage_scene_round_trip() {
  std::unique_ptr<engine::runtime::World> source(new (std::nothrow)
                                                     engine::runtime::World());
  if (source == nullptr) {
    return 20;
  }

  const engine::runtime::Entity entity = source->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 21;
  }

  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(4.0F, 0.0F, -2.0F);
  if (!source->add_transform(entity, transform)) {
    return 22;
  }

  if (!source->add_foliage_patch_component(entity, make_test_foliage())) {
    return 23;
  }

  std::array<char, engine::core::JsonWriter::kBufferBytes> buffer{};
  std::size_t size = 0U;
  if (!engine::runtime::save_scene(*source, buffer.data(), buffer.size(),
                                   &size)) {
    return 24;
  }

  std::unique_ptr<engine::runtime::World> loaded(new (std::nothrow)
                                                     engine::runtime::World());
  if (loaded == nullptr) {
    return 25;
  }
  if (!engine::runtime::load_scene(*loaded, buffer.data(), size)) {
    return 26;
  }
  if (loaded->foliage_patch_count() != 1U) {
    return 27;
  }

  const engine::runtime::Entity loadedEntity =
      loaded->foliage_patch_entity_at(0U);
  engine::runtime::FoliagePatchComponent foliage{};
  if (!loaded->get_foliage_patch_component(loadedEntity, &foliage)) {
    return 28;
  }

  if ((foliage.meshAssetIds[0] != 11U) ||
      (foliage.meshAssetIds[2] != 33U) || (foliage.instanceCount != 3U) ||
      !nearly_equal(foliage.albedo.y, 0.7F) ||
      !nearly_equal(foliage.roughness, 0.91F) ||
      !nearly_equal(foliage.windFrequency, 1.75F) ||
      !nearly_equal(foliage.instances[1].offset.z, 1.0F) ||
      !nearly_equal(foliage.instances[1].scale, 0.75F) ||
      (foliage.instances[1].lodIndex != 1U)) {
    return 29;
  }

  return 0;
}

} // namespace

int main() {
  int result = verify_foliage_crud();
  if (result != 0) {
    return result;
  }
  return verify_foliage_scene_round_trip();
}
