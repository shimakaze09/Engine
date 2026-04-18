#include <cmath>
#include <memory>
#include <new>

#include "engine/runtime/world.h"

namespace {

bool nearly_equal(float a, float b) { return std::fabs(a - b) <= 0.0001F; }

// ---------------------------------------------------------------------------
// PointLightComponent tests
// ---------------------------------------------------------------------------

int verify_point_light_crud() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 200;
  }

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 201;
  }

  engine::runtime::PointLightComponent plc{};
  plc.color = engine::math::Vec3(0.5F, 0.7F, 0.9F);
  plc.intensity = 2.0F;
  plc.radius = 15.0F;

  if (!world->add_point_light_component(entity, plc)) {
    return 202;
  }

  engine::runtime::PointLightComponent readBack{};
  if (!world->get_point_light_component(entity, &readBack)) {
    return 203;
  }

  if (!nearly_equal(readBack.color.x, 0.5F)) {
    return 204;
  }
  if (!nearly_equal(readBack.color.y, 0.7F)) {
    return 205;
  }
  if (!nearly_equal(readBack.color.z, 0.9F)) {
    return 206;
  }
  if (!nearly_equal(readBack.intensity, 2.0F)) {
    return 207;
  }
  if (!nearly_equal(readBack.radius, 15.0F)) {
    return 208;
  }

  if (!world->remove_point_light_component(entity)) {
    return 209;
  }

  // After removal, get should fail.
  engine::runtime::PointLightComponent afterRemove{};
  if (world->get_point_light_component(entity, &afterRemove)) {
    return 210;
  }

  return 0;
}

int verify_point_light_double_add() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 220;
  }

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 221;
  }

  engine::runtime::PointLightComponent plc{};
  plc.intensity = 1.0F;
  if (!world->add_point_light_component(entity, plc)) {
    return 222;
  }

  // SparseSet add overwrites on duplicate — second add should succeed.
  engine::runtime::PointLightComponent plc2{};
  plc2.intensity = 5.0F;
  if (!world->add_point_light_component(entity, plc2)) {
    return 223;
  }

  // Verify the overwrite took effect.
  engine::runtime::PointLightComponent readBack{};
  if (!world->get_point_light_component(entity, &readBack)) {
    return 224;
  }
  if (!nearly_equal(readBack.intensity, 5.0F)) {
    return 225;
  }

  return 0;
}

int verify_point_light_invalid_entity() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 230;
  }

  const engine::runtime::Entity invalid = engine::runtime::kInvalidEntity;
  engine::runtime::PointLightComponent plc{};

  // Add on invalid entity must fail.
  if (world->add_point_light_component(invalid, plc)) {
    return 231;
  }

  // Get on invalid entity must fail.
  engine::runtime::PointLightComponent readBack{};
  if (world->get_point_light_component(invalid, &readBack)) {
    return 232;
  }

  // Remove on invalid entity must fail.
  if (world->remove_point_light_component(invalid)) {
    return 233;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// SpotLightComponent tests
// ---------------------------------------------------------------------------

int verify_spot_light_crud() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 300;
  }

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 301;
  }

  engine::runtime::SpotLightComponent slc{};
  slc.color = engine::math::Vec3(1.0F, 0.5F, 0.0F);
  slc.direction = engine::math::Vec3(0.0F, 0.0F, -1.0F);
  slc.intensity = 3.0F;
  slc.radius = 20.0F;
  slc.innerConeAngle = 0.2F;
  slc.outerConeAngle = 0.4F;

  if (!world->add_spot_light_component(entity, slc)) {
    return 302;
  }

  engine::runtime::SpotLightComponent readBack{};
  if (!world->get_spot_light_component(entity, &readBack)) {
    return 303;
  }

  if (!nearly_equal(readBack.color.x, 1.0F)) {
    return 304;
  }
  if (!nearly_equal(readBack.color.y, 0.5F)) {
    return 305;
  }
  if (!nearly_equal(readBack.color.z, 0.0F)) {
    return 306;
  }
  if (!nearly_equal(readBack.direction.x, 0.0F)) {
    return 307;
  }
  if (!nearly_equal(readBack.direction.y, 0.0F)) {
    return 308;
  }
  if (!nearly_equal(readBack.direction.z, -1.0F)) {
    return 309;
  }
  if (!nearly_equal(readBack.intensity, 3.0F)) {
    return 310;
  }
  if (!nearly_equal(readBack.radius, 20.0F)) {
    return 311;
  }
  if (!nearly_equal(readBack.innerConeAngle, 0.2F)) {
    return 312;
  }
  if (!nearly_equal(readBack.outerConeAngle, 0.4F)) {
    return 313;
  }

  if (!world->remove_spot_light_component(entity)) {
    return 314;
  }

  engine::runtime::SpotLightComponent afterRemove{};
  if (world->get_spot_light_component(entity, &afterRemove)) {
    return 315;
  }

  return 0;
}

int verify_spot_light_double_add() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 320;
  }

  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return 321;
  }

  engine::runtime::SpotLightComponent slc{};
  slc.intensity = 1.0F;
  if (!world->add_spot_light_component(entity, slc)) {
    return 322;
  }

  // SparseSet add overwrites on duplicate — second add should succeed.
  engine::runtime::SpotLightComponent slc2{};
  slc2.intensity = 7.0F;
  if (!world->add_spot_light_component(entity, slc2)) {
    return 323;
  }

  // Verify the overwrite took effect.
  engine::runtime::SpotLightComponent readBack{};
  if (!world->get_spot_light_component(entity, &readBack)) {
    return 324;
  }
  if (!nearly_equal(readBack.intensity, 7.0F)) {
    return 325;
  }

  return 0;
}

int verify_spot_light_invalid_entity() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 330;
  }

  const engine::runtime::Entity invalid = engine::runtime::kInvalidEntity;
  engine::runtime::SpotLightComponent slc{};

  if (world->add_spot_light_component(invalid, slc)) {
    return 331;
  }

  engine::runtime::SpotLightComponent readBack{};
  if (world->get_spot_light_component(invalid, &readBack)) {
    return 332;
  }

  if (world->remove_spot_light_component(invalid)) {
    return 333;
  }

  return 0;
}

} // namespace

int main() {
  int result = verify_point_light_crud();
  if (result != 0) {
    return result;
  }

  result = verify_point_light_double_add();
  if (result != 0) {
    return result;
  }

  result = verify_point_light_invalid_entity();
  if (result != 0) {
    return result;
  }

  result = verify_spot_light_crud();
  if (result != 0) {
    return result;
  }

  result = verify_spot_light_double_add();
  if (result != 0) {
    return result;
  }

  return verify_spot_light_invalid_entity();
}
