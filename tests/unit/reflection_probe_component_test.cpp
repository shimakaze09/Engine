// Verifies reflection probe component test behavior for the Engine test suite.

#include <cmath>
#include <memory>
#include <new>

#include "engine/runtime/world.h"

namespace {

/// Handles nearly equal.
bool nearly_equal(float lhs, float rhs) noexcept {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

/// Handles verify reflection probe crud.
int verify_reflection_probe_crud() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 2;
  }

  engine::runtime::ReflectionProbeComponent probe{};
  probe.boxExtents = engine::math::Vec3(2.0F, 3.0F, 4.0F);
  probe.radius = 12.0F;
  probe.intensity = 1.5F;
  probe.prefilteredResolution = 256U;
  probe.irradianceResolution = 64U;
  probe.brdfLutResolution = 512U;
  probe.mipLevels = 6U;
  probe.boxProjection = true;
  probe.needsBake = true;

  if (!world->add_reflection_probe_component(entity, probe)) {
    return 3;
  }
  if (!world->has_reflection_probe_component(entity)) {
    return 4;
  }
  if (world->reflection_probe_count() != 1U) {
    return 5;
  }
  if (world->reflection_probe_entity_at(0U) != entity) {
    return 6;
  }

  const engine::runtime::ReflectionProbeComponent *stored =
      world->reflection_probe_at(0U);
  if ((stored == nullptr) || !nearly_equal(stored->boxExtents.y, 3.0F)) {
    return 7;
  }

  engine::runtime::ReflectionProbeComponent readBack{};
  if (!world->get_reflection_probe_component(entity, &readBack)) {
    return 8;
  }
  if (!nearly_equal(readBack.radius, 12.0F) ||
      !nearly_equal(readBack.intensity, 1.5F) ||
      (readBack.prefilteredResolution != 256U) ||
      (readBack.irradianceResolution != 64U) ||
      (readBack.brdfLutResolution != 512U) || (readBack.mipLevels != 6U) ||
      !readBack.boxProjection || !readBack.needsBake) {
    return 9;
  }

  engine::runtime::ReflectionProbeComponent *mutableProbe =
      world->get_reflection_probe_component_ptr(entity);
  if (mutableProbe == nullptr) {
    return 10;
  }
  mutableProbe->needsBake = false;
  if (!world->get_reflection_probe_component(entity, &readBack) ||
      readBack.needsBake) {
    return 11;
  }

  if (!world->remove_reflection_probe_component(entity)) {
    return 12;
  }
  if (world->has_reflection_probe_component(entity) ||
      (world->reflection_probe_count() != 0U)) {
    return 13;
  }
  if (world->get_reflection_probe_component(entity, &readBack)) {
    return 14;
  }

  return 0;
}

/// Handles verify reflection probe query and destroy.
int verify_reflection_probe_query_and_destroy() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 20;
  }

  const engine::runtime::Entity first = world->create_entity();
  const engine::runtime::Entity second = world->create_entity();
  if ((first == engine::runtime::kInvalidEntity) ||
      (second == engine::runtime::kInvalidEntity)) {
    return 21;
  }

  engine::runtime::ReflectionProbeComponent firstProbe{};
  firstProbe.radius = 6.0F;
  if (!world->add_reflection_probe_component(first, firstProbe)) {
    return 22;
  }

  engine::runtime::ReflectionProbeComponent secondProbe{};
  secondProbe.radius = 9.0F;
  if (!world->add_reflection_probe_component(second, secondProbe)) {
    return 23;
  }

  int visited = 0;
  float radiusSum = 0.0F;
  world->for_each<engine::runtime::ReflectionProbeComponent>(
      [&](engine::runtime::Entity,
          const engine::runtime::ReflectionProbeComponent &probe) noexcept {
        ++visited;
        radiusSum += probe.radius;
      });

  if ((visited != 2) || !nearly_equal(radiusSum, 15.0F)) {
    return 24;
  }

  if (!world->destroy_entity(first)) {
    return 25;
  }
  if (world->has_reflection_probe_component(first) ||
      (world->reflection_probe_count() != 1U)) {
    return 26;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = verify_reflection_probe_crud();
  if (result != 0) {
    return result;
  }

  return verify_reflection_probe_query_and_destroy();
}
