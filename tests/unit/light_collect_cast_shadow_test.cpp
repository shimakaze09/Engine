// Verifies that an authored castShadow flag on point and spot light
// components reaches the renderer's per-frame light data through the
// production collector (collect_scene_lights), and that the default stays
// off. Until #522 the components had no such field, so the renderer's
// shadow candidate lists were always empty.

#include "engine_frame_collect.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <memory>
#include <new>

int main() {
  using namespace engine::runtime;
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  const Entity caster = world->create_scene_object();
  const Entity plain = world->create_scene_object();
  if ((caster == kInvalidEntity) || (plain == kInvalidEntity)) {
    return 2;
  }
  PointLightComponent castingPoint{};
  castingPoint.castShadow = true;
  castingPoint.radius = 4.0F;
  SpotLightComponent castingSpot{};
  castingSpot.castShadow = true;
  castingSpot.radius = 6.0F;
  if (!world->add_point_light_component(caster, castingPoint) ||
      !world->add_spot_light_component(caster, castingSpot) ||
      !world->add_point_light_component(plain, PointLightComponent{}) ||
      !world->add_spot_light_component(plain, SpotLightComponent{})) {
    return 3;
  }
  world->begin_render_prep_phase();
  const engine::renderer::SceneLightData lights =
      engine::collect_scene_lights(*world);
  world->end_frame_phase();

  if ((lights.pointLightCount != 2U) || (lights.spotLightCount != 2U)) {
    std::fprintf(stderr, "FAIL: collected %zu point and %zu spot lights\n",
                 lights.pointLightCount, lights.spotLightCount);
    return 4;
  }
  std::size_t castingPoints = 0U;
  std::size_t castingSpots = 0U;
  for (std::size_t i = 0U; i < 2U; ++i) {
    // The caster is identifiable by its radius whichever order the
    // collector walked the components in.
    if (lights.pointLights[i].castShadow !=
        (lights.pointLights[i].radius == 4.0F)) {
      std::fprintf(stderr, "FAIL: point light %zu castShadow mismatch\n", i);
      return 5;
    }
    if (lights.spotLights[i].castShadow !=
        (lights.spotLights[i].radius == 6.0F)) {
      std::fprintf(stderr, "FAIL: spot light %zu castShadow mismatch\n", i);
      return 6;
    }
    castingPoints += lights.pointLights[i].castShadow ? 1U : 0U;
    castingSpots += lights.spotLights[i].castShadow ? 1U : 0U;
  }
  if ((castingPoints != 1U) || (castingSpots != 1U)) {
    std::fprintf(stderr, "FAIL: exactly one caster of each kind expected\n");
    return 7;
  }
  std::puts("light_collect_cast_shadow_test passed");
  return 0;
}
