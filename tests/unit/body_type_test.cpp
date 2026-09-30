// Pins the rigid-body type (#980): the World's ingress rules, the scene
// and prefab formats, and the integration step. A body is Dynamic unless
// authored otherwise; the type is written only when it is not Dynamic, so
// every document saved before types existed reads and saves unchanged; an
// unknown type is refused whole. A Kinematic body keeps its authored mass
// and inertia but the simulation treats it as immovable, integrates its
// velocities exactly with no gravity, and never lets it sleep; a Static
// body holds no velocity at all.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/core/logging.h"
#include "engine/physics/physics.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

using engine::math::BodyType;
using engine::runtime::Entity;
using engine::runtime::RigidBody;
using engine::runtime::World;

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

constexpr const char *kBodyHead =
    "{\"version\":6,\"entities\":[{\"persistentId\":7,\"components\":{"
    "\"Transform\":{\"position\":[0,1,0],\"rotation\":[0,0,0,1],"
    "\"scale\":[1,1,1],\"parentId\":0},\"RigidBody\":{";
constexpr const char *kBodyTail =
    "\"velocity\":[1,0,0],\"acceleration\":[0,0,0],"
    "\"angularVelocity\":[0,0,0],\"inverseMass\":2,"
    "\"inverseInertia\":[1,1,1],\"inertiaAuthored\":true,"
    "\"sleeping\":false,\"gravityScale\":1}}}]}";

/// Loads a one-body scene whose RigidBody starts with `typeField` (empty,
/// or a `"bodyType":...,` member) into a fresh World.
bool load_body(const char *typeField, RigidBody *outBody) noexcept {
  char json[1024] = {};
  std::snprintf(json, sizeof(json), "%s%s%s", kBodyHead, typeField, kBodyTail);
  std::unique_ptr<World> world(new (std::nothrow) World());
  if ((world == nullptr) ||
      !engine::runtime::load_scene(*world, json, std::strlen(json))) {
    return false;
  }
  const Entity entity = world->find_entity_by_persistent_id(7U);
  return (entity != engine::runtime::kInvalidEntity) &&
         world->get_rigid_body(entity, outBody);
}

void test_scene_reads() noexcept {
  RigidBody body{};
  check(load_body("", &body) && (body.bodyType == 0U) &&
            (engine::math::body_type(body) == BodyType::Dynamic),
        "a body saved before types existed reads as Dynamic");
  check(load_body("\"bodyType\":1,", &body) &&
            (engine::math::body_type(body) == BodyType::Kinematic) &&
            (body.inverseMass == 2.0F) && (body.velocity.x == 1.0F),
        "a kinematic body keeps its authored mass and velocity");
  check(load_body("\"bodyType\":2,", &body) &&
            (engine::math::body_type(body) == BodyType::Static) &&
            (body.velocity.x == 0.0F),
        "a static body loads with no velocity");
  check(!load_body("\"bodyType\":3,", &body),
        "an unknown body type refuses the document");
  check(!load_body("\"bodyType\":\"kinematic\",", &body),
        "a body type that is not a number refuses the document");
}

/// Saves a one-body World and returns the text in `buffer`.
bool save_one_body(std::uint32_t type, char *buffer, std::size_t capacity,
                   std::size_t *outSize,
                   engine::core::PersistentId *outId) noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return false;
  }
  const Entity entity = world->create_scene_object();
  RigidBody body{};
  body.bodyType = type;
  body.inverseMass = 0.5F;
  body.inertiaAuthored = true;
  if ((entity == engine::runtime::kInvalidEntity) ||
      !world->add_rigid_body(entity, body)) {
    return false;
  }
  *outId = world->persistent_id(entity);
  return engine::runtime::save_scene(*world, buffer, capacity, outSize);
}

void test_round_trip() noexcept {
  char buffer[8192] = {};
  std::size_t size = 0U;
  engine::core::PersistentId id = 0U;
  check(save_one_body(0U, buffer, sizeof(buffer), &size, &id) &&
            (std::strstr(buffer, "bodyType") == nullptr) &&
            (std::strstr(buffer, "\"version\":6") != nullptr),
        "a dynamic body is saved exactly as before: no type key, revision 6");

  const std::uint32_t types[] = {1U, 2U};
  for (std::uint32_t type : types) {
    char expected[32] = {};
    std::snprintf(expected, sizeof(expected), "\"bodyType\":%u", type);
    check(save_one_body(type, buffer, sizeof(buffer), &size, &id) &&
              (std::strstr(buffer, expected) != nullptr),
          "a non-dynamic body writes its type");
    std::unique_ptr<World> reloaded(new (std::nothrow) World());
    RigidBody back{};
    check((reloaded != nullptr) &&
              engine::runtime::load_scene(*reloaded, buffer, size) &&
              reloaded->get_rigid_body(
                  reloaded->find_entity_by_persistent_id(id), &back) &&
              (back.bodyType == type) && (back.inverseMass == 0.5F),
          "the body type and the authored mass round-trip exactly");
  }
}

constexpr const char *kPrefabPath = "body_type_test_prefab.json";

bool write_file(const char *path, const char *text) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(text);
  const bool ok = std::fwrite(text, 1U, length, file) == length;
  return (std::fclose(file) == 0) && ok;
}

void test_prefab() noexcept {
  const char *prefab =
      "{\"version\":5,\"components\":{\"Transform\":{\"position\":"
      "[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1],\"parentId\":0},"
      "\"RigidBody\":{\"bodyType\":1,\"velocity\":[0,0,0],"
      "\"acceleration\":[0,0,0],\"angularVelocity\":[0,0,0],"
      "\"inverseMass\":1,\"inverseInertia\":[1,1,1],"
      "\"inertiaAuthored\":true,\"sleeping\":false,\"gravityScale\":1}}}";
  std::unique_ptr<World> world(new (std::nothrow) World());
  if ((world == nullptr) || !write_file(kPrefabPath, prefab)) {
    g_tests.fail("prepare the prefab");
    return;
  }
  const Entity entity =
      engine::runtime::instantiate_prefab(*world, kPrefabPath);
  RigidBody body{};
  check((entity != engine::runtime::kInvalidEntity) &&
            world->get_rigid_body(entity, &body) &&
            (engine::math::body_type(body) == BodyType::Kinematic),
        "a prefab body reads its type");
  static_cast<void>(std::remove(kPrefabPath));
}

void test_world_ingress() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    g_tests.fail("allocate the world");
    return;
  }
  const Entity entity = world->create_scene_object();
  RigidBody body{};
  body.inverseMass = 3.0F;
  check(world->add_rigid_body(entity, body), "a dynamic body is added");

  RigidBody unknown = body;
  unknown.bodyType = engine::math::kBodyTypeCount;
  RigidBody kept{};
  check(!world->add_rigid_body(entity, unknown) &&
            world->get_rigid_body(entity, &kept) && (kept.bodyType == 0U) &&
            (kept.inverseMass == 3.0F),
        "an unknown type is refused and the stored body is unchanged");

  RigidBody fixed = body;
  fixed.bodyType = static_cast<std::uint32_t>(BodyType::Static);
  fixed.velocity = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  fixed.angularVelocity = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  fixed.acceleration = engine::math::Vec3(0.0F, 4.0F, 0.0F);
  check(world->add_rigid_body(entity, fixed) &&
            world->get_rigid_body(entity, &kept) && (kept.velocity.y == 0.0F) &&
            (kept.angularVelocity.y == 0.0F) && (kept.acceleration.y == 0.0F) &&
            (kept.inverseMass == 3.0F),
        "a static body stores no motion and keeps its authored mass");

  // Kinematic bodies integrate in world space, so like dynamic ones they
  // must be roots; a static one may hang anywhere.
  const Entity parent = world->create_scene_object();
  engine::runtime::Transform child{};
  child.parentId = world->persistent_id(parent);
  const Entity driven = world->create_scene_object();
  RigidBody kinematic{};
  kinematic.bodyType = static_cast<std::uint32_t>(BodyType::Kinematic);
  kinematic.inverseMass = 0.0F;
  check(world->add_rigid_body(driven, kinematic) &&
            !world->add_transform(driven, child),
        "a kinematic body cannot be parented, even with zero mass");
  const Entity anchor = world->create_scene_object();
  RigidBody staticBody{};
  staticBody.bodyType = static_cast<std::uint32_t>(BodyType::Static);
  check(world->add_rigid_body(anchor, staticBody) &&
            world->add_transform(anchor, child),
        "a static body can be parented");

  RigidBody sleeper = kinematic;
  sleeper.sleeping = true;
  sleeper.sleepFrameCount = 60U;
  check(world->add_rigid_body(driven, sleeper) &&
            world->get_rigid_body(driven, &kept) && !kept.sleeping &&
            (kept.sleepFrameCount == 0U),
        "a kinematic body is never stored asleep");
}

void test_simulated_mass() noexcept {
  RigidBody body{};
  body.inverseMass = 2.0F;
  body.inverseInertia = engine::math::Vec3(3.0F, 3.0F, 3.0F);
  check((engine::math::simulated_inverse_mass(body) == 2.0F) &&
            (engine::math::simulated_inverse_inertia(body).x == 3.0F) &&
            engine::math::body_moves(body),
        "a dynamic body simulates with its own mass and inertia");
  body.bodyType = static_cast<std::uint32_t>(BodyType::Kinematic);
  check((engine::math::simulated_inverse_mass(body) == 0.0F) &&
            (engine::math::simulated_inverse_inertia(body).x == 0.0F) &&
            engine::math::body_moves(body),
        "a kinematic body simulates as immovable and still moves");
  body.bodyType = static_cast<std::uint32_t>(BodyType::Static);
  check((engine::math::simulated_inverse_mass(body) == 0.0F) &&
            !engine::math::body_moves(body),
        "a static body simulates as immovable and never moves");
}

/// One step of `dt` from the given body under gravity (0,-8,0); returns
/// the stepped transform and body.
bool step_once(const RigidBody &start, float dt,
               engine::runtime::Transform *outTransform,
               RigidBody *outBody) noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return false;
  }
  world->end_frame_phase();
  const Entity entity = world->create_entity();
  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(0.0F, 10.0F, 0.0F);
  if (!world->add_transform(entity, transform) ||
      !world->add_rigid_body(entity, start)) {
    return false;
  }
  engine::runtime::set_gravity(*world, 0.0F, -8.0F, 0.0F);
  world->begin_update_phase();
  const bool stepped = engine::runtime::step_physics(*world, dt);
  world->commit_update_phase();
  world->begin_render_prep_phase();
  world->end_frame_phase();
  return stepped && world->get_transform(entity, outTransform) &&
         world->get_rigid_body(entity, outBody);
}

void test_step() noexcept {
  // Every operand is a power of two, so the advance is exact.
  RigidBody kinematic{};
  kinematic.bodyType = static_cast<std::uint32_t>(BodyType::Kinematic);
  kinematic.inverseMass = 1.0F;
  kinematic.inertiaAuthored = true;
  kinematic.velocity = engine::math::Vec3(2.0F, 0.0F, -4.0F);
  kinematic.angularVelocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  engine::runtime::Transform after{};
  RigidBody body{};
  check(step_once(kinematic, 0.25F, &after, &body) &&
            (after.position.x == 0.5F) && (after.position.y == 10.0F) &&
            (after.position.z == -1.0F) && (body.velocity.x == 2.0F) &&
            (body.velocity.y == 0.0F),
        "a kinematic body advances by its velocity exactly, with no gravity");

  // A kinematic body with no rotational inertia of its own (it is
  // immovable) still turns at its angular velocity.
  kinematic.velocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  kinematic.angularVelocity = engine::math::Vec3(0.0F, 2.0F, 0.0F);
  kinematic.inverseInertia = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  check(step_once(kinematic, 0.25F, &after, &body) &&
            (after.rotation.y > 0.2F) && (body.angularVelocity.y == 2.0F),
        "a kinematic body turns at its angular velocity, undamped");

  RigidBody fixed{};
  fixed.bodyType = static_cast<std::uint32_t>(BodyType::Static);
  fixed.inverseMass = 1.0F;
  check(step_once(fixed, 0.25F, &after, &body) && (after.position.y == 10.0F) &&
            (body.velocity.y == 0.0F),
        "a static body with mass stays put under gravity");
}

} // namespace

/// Runs this executable or test program.
int main() {
  static_cast<void>(engine::core::initialize_logging());
  test_scene_reads();
  test_round_trip();
  test_prefab();
  test_world_ingress();
  test_simulated_mass();
  test_step();
  engine::core::shutdown_logging();
  return g_tests.finish("engine_unit_body_type");
}
