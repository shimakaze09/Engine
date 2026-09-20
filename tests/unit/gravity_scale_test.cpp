// Pins the authored gravity scale on a rigid body: the physics step
// applies the world's gravity times the scale, the scene and prefab
// formats carry it, and documents from before it existed migrate: a
// dynamic body whose authored acceleration exactly cancelled the world's
// gravity reads as gravity scale 0 with no acceleration, against the
// gravity the scene authored (or the default a prefab relied on), while
// any other acceleration is kept and a current document is never
// reinterpreted.

#include <cmath>
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

using engine::runtime::Entity;
using engine::runtime::RigidBody;
using engine::runtime::World;

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

/// The rigid body of the one entity a loaded scene holds.
bool load_scene_body(const char *json, RigidBody *outBody) noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return false;
  }
  if (!engine::runtime::load_scene(*world, json, std::strlen(json))) {
    return false;
  }
  const Entity entity = world->find_entity_by_persistent_id(7U);
  return (entity != engine::runtime::kInvalidEntity) &&
         world->get_rigid_body(entity, outBody);
}

constexpr const char *kBodyHead =
    "{\"persistentId\":7,\"components\":{\"Transform\":{\"position\":[0,1,0],"
    "\"rotation\":[0,0,0,1],\"scale\":[1,1,1],\"parentId\":0},"
    "\"RigidBody\":{\"velocity\":[0,0,0],\"acceleration\":";
constexpr const char *kBodyTail =
    ",\"angularVelocity\":[0,0,0],\"inverseMass\":1,"
    "\"inverseInertia\":[1,1,1],\"inertiaAuthored\":true,\"sleeping\":false";

/// A one-body scene of `version`, with the given acceleration, an
/// optional gravity root field and optional extra body fields.
const char *scene(char *buffer, std::size_t capacity, int version,
                  const char *acceleration, const char *gravityField,
                  const char *extraBodyFields) noexcept {
  std::snprintf(buffer, capacity, "{\"version\":%d,%s\"entities\":[%s%s%s%s}}}]}",
                version, gravityField, kBodyHead, acceleration, kBodyTail,
                extraBodyFields);
  return buffer;
}

void test_migration() noexcept {
  char json[1024] = {};
  RigidBody body{};

  check(load_scene_body(scene(json, sizeof(json), 4, "[0,9.8,0]", "", ""),
                        &body) &&
            (body.gravityScale == 0.0F) && (body.acceleration.x == 0.0F) &&
            (body.acceleration.y == 0.0F) && (body.acceleration.z == 0.0F),
        "a v4 body cancelling the default gravity reads as gravity scale 0");

  check(load_scene_body(scene(json, sizeof(json), 4, "[0,9.80000019,0]", "",
                              ""),
                        &body) &&
            (body.gravityScale == 0.0F) && (body.acceleration.y == 0.0F),
        "the float the editor wrote for 9.8 cancels too");

  check(load_scene_body(scene(json, sizeof(json), 4, "[0,5,0]",
                              "\"gravity\":[0,-5,0],", ""),
                        &body) &&
            (body.gravityScale == 0.0F) && (body.acceleration.y == 0.0F),
        "a v4 body is migrated against the gravity its scene authored");

  check(load_scene_body(scene(json, sizeof(json), 4, "[0,9.8,0]",
                              "\"gravity\":[0,-5,0],", ""),
                        &body) &&
            (body.gravityScale == 1.0F) && (body.acceleration.y == 9.8F),
        "a v4 acceleration that is not the scene's gravity is kept");

  check(load_scene_body(scene(json, sizeof(json), 4, "[0,3,0]", "", ""),
                        &body) &&
            (body.gravityScale == 1.0F) && (body.acceleration.y == 3.0F),
        "a v4 body with another acceleration keeps it and full gravity");

  check(load_scene_body(scene(json, sizeof(json), 4, "[1,9.8,0]", "", ""),
                        &body) &&
            (body.gravityScale == 1.0F) && (body.acceleration.x == 1.0F),
        "a v4 body cancelling gravity on one axis only is kept");

  check(load_scene_body(scene(json, sizeof(json), 3, "[0,9.8,0]", "", ""),
                        &body) &&
            (body.gravityScale == 0.0F) && (body.acceleration.y == 0.0F),
        "a v3 body cancelling gravity migrates as well");

  check(load_scene_body(scene(json, sizeof(json), 5, "[0,9.8,0]", "", ""),
                        &body) &&
            (body.gravityScale == 1.0F) && (body.acceleration.y == 9.8F),
        "a v5 body with a cancelling acceleration is never reinterpreted");

  check(load_scene_body(scene(json, sizeof(json), 5, "[0,0,0]", "",
                              ",\"gravityScale\":0.25"),
                        &body) &&
            (body.gravityScale == 0.25F),
        "a v5 body reads its gravity scale");

  check(load_scene_body(scene(json, sizeof(json), 5, "[0,0,0]", "", ""),
                        &body) &&
            (body.gravityScale == 1.0F),
        "a v5 body without the field feels full gravity");

  check(!load_scene_body(scene(json, sizeof(json), 5, "[0,0,0]", "",
                               ",\"gravityScale\":\"none\""),
                         &body),
        "a malformed gravity scale refuses the document");
}

void test_round_trip() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    g_tests.fail("allocate the world");
    return;
  }
  const Entity entity = world->create_scene_object();
  RigidBody body{};
  body.inverseMass = 2.0F;
  body.gravityScale = 0.5F;
  body.inertiaAuthored = true;
  check((entity != engine::runtime::kInvalidEntity) &&
            world->add_rigid_body(entity, body),
        "the body is added");
  char buffer[8192] = {};
  std::size_t size = 0U;
  check(engine::runtime::save_scene(*world, buffer, sizeof(buffer), &size),
        "the scene saves");
  check(std::strstr(buffer, "\"gravityScale\":0.5") != nullptr,
        "the saved scene carries the gravity scale");
  check(std::strstr(buffer, "\"version\":5") != nullptr,
        "the saved scene is revision 5");

  std::unique_ptr<World> reloaded(new (std::nothrow) World());
  RigidBody back{};
  check((reloaded != nullptr) &&
            engine::runtime::load_scene(*reloaded, buffer, size) &&
            reloaded->get_rigid_body(
                reloaded->find_entity_by_persistent_id(
                    world->persistent_id(entity)),
                &back) &&
            (back.gravityScale == 0.5F),
        "the gravity scale round-trips exactly");
}

constexpr const char *kPrefabPath = "gravity_scale_test_prefab.json";

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

void test_prefab_migration() noexcept {
  const char *legacy =
      "{\"version\":3,\"components\":{\"Transform\":{\"position\":"
      "[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1],\"parentId\":0},"
      "\"RigidBody\":{\"velocity\":[0,0,0],\"acceleration\":[0,9.8,0],"
      "\"angularVelocity\":[0,0,0],\"inverseMass\":1,\"inverseInertia\":"
      "[1,1,1],\"inertiaAuthored\":true,\"sleeping\":false}}}";
  std::unique_ptr<World> world(new (std::nothrow) World());
  if ((world == nullptr) || !write_file(kPrefabPath, legacy)) {
    g_tests.fail("prepare the prefab");
    return;
  }
  const Entity entity = engine::runtime::instantiate_prefab(*world, kPrefabPath);
  RigidBody body{};
  check((entity != engine::runtime::kInvalidEntity) &&
            world->get_rigid_body(entity, &body) &&
            (body.gravityScale == 0.0F) && (body.acceleration.y == 0.0F),
        "a v3 prefab body cancelling the default gravity reads as scale 0");
  static_cast<void>(std::remove(kPrefabPath));
}

/// One fixed step from rest: the velocity gained is the world's gravity
/// times the scale times dt, exactly, since every operand is exact.
void test_physics_step() noexcept {
  const float scales[] = {1.0F, 0.0F, 0.5F, -1.0F};
  for (float scale : scales) {
    std::unique_ptr<World> world(new (std::nothrow) World());
    if (world == nullptr) {
      g_tests.fail("allocate the world");
      return;
    }
    world->end_frame_phase();
    const Entity entity = world->create_entity();
    engine::runtime::Transform transform{};
    transform.position = engine::math::Vec3(0.0F, 10.0F, 0.0F);
    RigidBody body{};
    body.inverseMass = 1.0F;
    body.inertiaAuthored = true;
    body.gravityScale = scale;
    if (!world->add_transform(entity, transform) ||
        !world->add_rigid_body(entity, body)) {
      g_tests.fail("build the body");
      return;
    }
    engine::runtime::set_gravity(*world, 0.0F, -8.0F, 0.0F);
    world->begin_update_phase();
    check(engine::runtime::step_physics(*world, 0.25F), "the step runs");
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
    RigidBody after{};
    check(world->get_rigid_body(entity, &after) &&
              (after.velocity.y == (-8.0F * scale * 0.25F)) &&
              (after.velocity.x == 0.0F) && (after.velocity.z == 0.0F),
          "one step from rest gains gravity times the scale times dt");
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  static_cast<void>(engine::core::initialize_logging());
  test_migration();
  test_round_trip();
  test_prefab_migration();
  test_physics_step();
  engine::core::shutdown_logging();
  return g_tests.finish("gravity scale tests");
}
