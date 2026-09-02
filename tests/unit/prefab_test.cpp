// Verifies prefab test behavior for the Engine test suite.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/physics/physics.h"
#include "engine/physics/primitive_hulls.h"
#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/world.h"

namespace {

constexpr const char *kPrefabPath = "prefab_test_temp.json";
constexpr const char *kPrefabSourceName = "Prefab \"Source\" \\ Name";
constexpr const char *kPrefabScriptPath =
    "assets\\scripts\\prefab \"source\".lua";

void remove_prefab_file() noexcept {
  static_cast<void>(std::remove(kPrefabPath));
}

bool nearly_equal(float lhs, float rhs) noexcept {
  const float diff = lhs - rhs;
  return (diff < 0.0001F) && (diff > -0.0001F);
}

engine::math::Quat collider_test_rotation(std::size_t index) noexcept {
  switch (index) {
  case 1U:
    return engine::math::Quat(1.0F, 0.0F, 0.0F, 0.0F);
  case 2U:
    return engine::math::Quat(0.0F, 1.0F, 0.0F, 0.0F);
  case 3U:
    return engine::math::Quat(0.0F, 0.0F, 1.0F, 0.0F);
  case 4U:
    return engine::math::Quat(0.0F, 0.0F, 0.0F, -1.0F);
  default:
    return engine::math::Quat();
  }
}

bool collider_pose_equals(const engine::runtime::Collider &collider,
                          const engine::math::Vec3 &position,
                          const engine::math::Quat &rotation) noexcept {
  return (collider.localPosition.x == position.x) &&
         (collider.localPosition.y == position.y) &&
         (collider.localPosition.z == position.z) &&
         (collider.localRotation.x == rotation.x) &&
         (collider.localRotation.y == rotation.y) &&
         (collider.localRotation.z == rotation.z) &&
         (collider.localRotation.w == rotation.w);
}

bool write_prefab_text(const char *text) noexcept {
  if (text == nullptr) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kPrefabPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kPrefabPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }

  const std::size_t size = std::strlen(text);
  const std::size_t written = std::fwrite(text, 1U, size, file);
  std::fclose(file);
  return written == size;
}

int verify_instantiate_rejects_malformed_component() {
  remove_prefab_file();
  constexpr const char *kMalformedPrefab =
      "{\"version\":1,\"components\":{\"Transform\":{\"position\":\"bad\"}}}";
  if (!write_prefab_text(kMalformedPrefab)) {
    remove_prefab_file();
    return 32;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    remove_prefab_file();
    return 40;
  }
  const engine::runtime::Entity entity =
      engine::runtime::instantiate_prefab(*world, kPrefabPath);
  remove_prefab_file();
  if (entity != engine::runtime::kInvalidEntity) {
    return 33;
  }
  if (world->alive_entity_count() != 0U) {
    return 34;
  }
  return 0;
}

/// Regression for #332: instantiate_prefab must gate on the document's
/// schema revision. Covers the revisions this build accepts, the future
/// and malformed values it must refuse, and the requirement that a refusal
/// creates nothing -- the version is read before the instance exists.
int verify_instantiate_validates_schema_version() {
  struct VersionCase final {
    const char *json;
    bool accepted;
  };

  // Revision 1 is the only one this build writes. A document omitting the
  // key reads as that revision, so hand-authored prefabs still load.
  constexpr VersionCase kCases[] = {
      {"{\"version\":1,\"components\":{}}", true},
      {"{\"components\":{}}", true},
      {"{\"version\":2,\"components\":{}}", false},
      {"{\"version\":999,\"components\":{}}", false},
      {"{\"version\":0,\"components\":{}}", false},
      {"{\"version\":-1,\"components\":{}}", false},
      {"{\"version\":1.5,\"components\":{}}", false},
      {"{\"version\":4294967296,\"components\":{}}", false},
      {"{\"version\":\"1\",\"components\":{}}", false},
      {"{\"version\":true,\"components\":{}}", false},
      {"{\"version\":null,\"components\":{}}", false},
      {"{\"version\":[1],\"components\":{}}", false},
  };

  for (const VersionCase &testCase : kCases) {
    remove_prefab_file();
    if (!write_prefab_text(testCase.json)) {
      remove_prefab_file();
      return 200;
    }

    std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                      engine::runtime::World());
    if (world == nullptr) {
      remove_prefab_file();
      return 201;
    }

    const std::size_t aliveBefore = world->alive_entity_count();
    const engine::runtime::Entity entity =
        engine::runtime::instantiate_prefab(*world, kPrefabPath);
    remove_prefab_file();

    const bool accepted = (entity != engine::runtime::kInvalidEntity);
    if (accepted != testCase.accepted) {
      std::fprintf(stderr, "FAIL: prefab %s was %s: %s\n",
                   testCase.accepted ? "rejected" : "accepted",
                   testCase.accepted ? "expected to load" : "expected refused",
                   testCase.json);
      return testCase.accepted ? 202 : 203;
    }

    const std::size_t expectedAlive =
        aliveBefore + (testCase.accepted ? 1U : 0U);
    if (world->alive_entity_count() != expectedAlive) {
      std::fprintf(stderr, "FAIL: prefab left the world changed: %s\n",
                   testCase.json);
      return 204;
    }
  }

  return 0;
}

int verify_instantiate_rolls_back_on_component_add_failure() {
  remove_prefab_file();
  constexpr const char *kPointLightPrefab =
      "{\"version\":1,\"components\":{\"PointLightComponent\":{\"radius\":3}}}";
  if (!write_prefab_text(kPointLightPrefab)) {
    remove_prefab_file();
    return 35;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    remove_prefab_file();
    return 41;
  }
  for (std::size_t i = 0U; i < engine::runtime::World::kMaxPointLightComponents;
       ++i) {
    const engine::runtime::Entity entity = world->create_entity();
    if (entity == engine::runtime::kInvalidEntity) {
      remove_prefab_file();
      return 36;
    }
    engine::runtime::PointLightComponent pointLight{};
    if (!world->add_point_light_component(entity, pointLight)) {
      remove_prefab_file();
      return 37;
    }
  }

  const std::size_t aliveBefore = world->alive_entity_count();
  const engine::runtime::Entity entity =
      engine::runtime::instantiate_prefab(*world, kPrefabPath);
  remove_prefab_file();
  if (entity != engine::runtime::kInvalidEntity) {
    return 38;
  }
  if (world->alive_entity_count() != aliveBefore) {
    return 39;
  }
  return 0;
}

/// Verifies all collider shapes/local poses plus legacy and invalid prefabs.
int verify_collider_prefab_round_trip() {
  constexpr engine::runtime::ColliderShape kShapes[] = {
      engine::runtime::ColliderShape::AABB,
      engine::runtime::ColliderShape::Sphere,
      engine::runtime::ColliderShape::Capsule,
      engine::runtime::ColliderShape::ConvexHull,
      engine::runtime::ColliderShape::Heightfield};
  remove_prefab_file();
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 100;
  }
  for (std::size_t i = 0U; i < 5U; ++i) {
    const engine::runtime::Entity source = world->create_scene_object();
    if (source == engine::runtime::kInvalidEntity) {
      return 101;
    }
    engine::runtime::Collider collider{};
    collider.shape = kShapes[i];
    collider.localPosition = engine::math::Vec3(static_cast<float>(i + 1U),
                                                -static_cast<float>(i + 2U),
                                                static_cast<float>(i + 3U));
    collider.localRotation = collider_test_rotation(i);
    if (!world->add_collider(source, collider) ||
        !engine::runtime::save_prefab(*world, source, kPrefabPath)) {
      return 102;
    }
    const engine::runtime::Entity instance =
        engine::runtime::instantiate_prefab(*world, kPrefabPath);
    engine::runtime::Collider loaded{};
    if ((instance == engine::runtime::kInvalidEntity) ||
        !world->get_collider(instance, &loaded) ||
        (loaded.shape != kShapes[i]) ||
        !collider_pose_equals(loaded, collider.localPosition,
                              collider.localRotation)) {
      return 103;
    }
  }

  constexpr const char *kLegacyPrefab =
      "{\"version\":1,\"components\":{\"Collider\":{\"halfExtents\":[1,2,3]}}}";
  if (!write_prefab_text(kLegacyPrefab)) {
    return 104;
  }
  const engine::runtime::Entity legacy =
      engine::runtime::instantiate_prefab(*world, kPrefabPath);
  engine::runtime::Collider legacyCollider{};
  if ((legacy == engine::runtime::kInvalidEntity) ||
      !world->get_collider(legacy, &legacyCollider) ||
      (legacyCollider.shape != engine::runtime::ColliderShape::AABB) ||
      !collider_pose_equals(legacyCollider, engine::math::Vec3(),
                            engine::math::Quat())) {
    return 105;
  }

  constexpr const char *kInvalidPrefab =
      "{\"version\":1,\"components\":{\"Collider\":{\"shape\":5}}}";
  if (!write_prefab_text(kInvalidPrefab)) {
    return 106;
  }
  const std::size_t aliveBefore = world->alive_entity_count();
  if ((engine::runtime::instantiate_prefab(*world, kPrefabPath) !=
       engine::runtime::kInvalidEntity) ||
      (world->alive_entity_count() != aliveBefore)) {
    return 107;
  }
  return 0;
}

/// EXPECTATION: a SpringArmComponent survives the prefab round trip with
/// every field intact (regression: prefabs silently dropped spring arms).
int verify_spring_arm_prefab_round_trip() {
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 90;
  }
  const Entity entity = world->create_scene_object();
  if (entity == kInvalidEntity) {
    return 91;
  }
  SpringArmComponent springArm{};
  springArm.armLength = 4.5F;
  springArm.currentLength = 3.25F;
  springArm.offset = engine::math::Vec3(0.5F, 1.5F, -0.75F);
  springArm.lagSpeed = 7.0F;
  springArm.collisionRadius = 0.35F;
  springArm.collisionEnabled = true;
  if (!world->add_spring_arm(entity, springArm)) {
    return 92;
  }

  if (!save_prefab(*world, entity, kPrefabPath)) {
    return 93;
  }
  const Entity spawned = instantiate_prefab(*world, kPrefabPath);
  if (spawned == kInvalidEntity) {
    return 94;
  }
  SpringArmComponent loaded{};
  if (!world->get_spring_arm(spawned, &loaded) ||
      (loaded.armLength != 4.5F) || (loaded.currentLength != 3.25F) ||
      (loaded.offset.x != 0.5F) || (loaded.offset.y != 1.5F) ||
      (loaded.offset.z != -0.75F) || (loaded.lagSpeed != 7.0F) ||
      (loaded.collisionRadius != 0.35F) || !loaded.collisionEnabled) {
    return 95;
  }
  return 0;
}

/// Issue #253: the prefab format carried only AnimationComponent's
/// controller path, so the authored `playing`/`playbackSpeed` reverted to
/// their defaults on instantiate. Pins the round trip, the legacy bare-path
/// shape the format has always written, and the format's standing rule that
/// an empty controller path is rejected.
int verify_animation_prefab_round_trip() {
  using namespace engine::runtime;

  constexpr const char *kControllerPath = "assets/character.animctrl.json";

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 96;
  }
  const Entity entity = world->create_scene_object();
  if (entity == kInvalidEntity) {
    return 97;
  }
  AnimationComponent animation{};
  std::snprintf(animation.controllerPath, sizeof(animation.controllerPath),
                "%s", kControllerPath);
  animation.playing = false;
  animation.playbackSpeed = 0.5F;
  if (!world->add_animation_component(entity, animation)) {
    return 98;
  }

  if (!save_prefab(*world, entity, kPrefabPath)) {
    return 99;
  }
  const Entity spawned = instantiate_prefab(*world, kPrefabPath);
  if (spawned == kInvalidEntity) {
    return 100;
  }
  AnimationComponent loaded{};
  if (!world->get_animation_component(spawned, &loaded)) {
    return 101;
  }
  // Exact: authored serialized data, not computed floats.
  if ((std::strcmp(loaded.controllerPath, kControllerPath) != 0) ||
      loaded.playing || (loaded.playbackSpeed != 0.5F)) {
    return 102;
  }

  // The bare path stays readable, supplying defaults for the fields it
  // cannot express -- every prefab authored before the object shape.
  if (!write_prefab_text("{\"version\":1,\"components\":{"
                         "\"AnimationComponent\":"
                         "\"assets/character.animctrl.json\"}}")) {
    return 103;
  }
  const Entity legacy = instantiate_prefab(*world, kPrefabPath);
  if (legacy == kInvalidEntity) {
    return 104;
  }
  AnimationComponent legacyAnimation{};
  const AnimationComponent defaults{};
  if (!world->get_animation_component(legacy, &legacyAnimation) ||
      (std::strcmp(legacyAnimation.controllerPath, kControllerPath) != 0) ||
      (legacyAnimation.playing != defaults.playing) ||
      (legacyAnimation.playbackSpeed != defaults.playbackSpeed)) {
    return 105;
  }

  // Unchanged rule: the prefab format requires a non-empty controller path,
  // in the object shape as well as the string.
  if (!write_prefab_text("{\"version\":1,\"components\":{"
                         "\"AnimationComponent\":{\"controllerPath\":\"\"}}}")) {
    return 106;
  }
  if (instantiate_prefab(*world, kPrefabPath) != kInvalidEntity) {
    return 107;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
/// #208: save_prefab must refuse (not silently drop) payloads the prefab
/// format cannot represent — provenance-free custom hulls and heightfield
/// samples — leaving the destination untouched, while builder-provenance
/// hulls rebuild on instantiate and stay savable.
int verify_prefab_save_refuses_unserializable_payloads() {
  remove_prefab_file();
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 300;
  }

  const engine::runtime::Entity hullEntity = world->create_scene_object();
  const engine::runtime::Entity heightfieldEntity =
      world->create_scene_object();
  if ((hullEntity == engine::runtime::kInvalidEntity) ||
      (heightfieldEntity == engine::runtime::kInvalidEntity)) {
    return 301;
  }

  engine::physics::ConvexHullData hull{};
  if (!engine::physics::build_cylinder_hull(&hull)) {
    return 302;
  }
  engine::runtime::Collider hullCollider{};
  hullCollider.shape = engine::runtime::ColliderShape::ConvexHull;
  hullCollider.hullSource = engine::math::HullSource::None;
  if (!world->add_collider(hullEntity, hullCollider) ||
      !engine::physics::set_convex_hull_data(world->physics_context(),
                                             hullEntity, hull)) {
    return 303;
  }
  if (engine::runtime::save_prefab(*world, hullEntity, kPrefabPath)) {
    return 304; // custom hull payload must refuse the save
  }
  {
    // The refused save must not have created the destination file.
    std::FILE *file = nullptr;
#ifdef _WIN32
    if (fopen_s(&file, kPrefabPath, "rb") != 0) {
      file = nullptr;
    }
#else
    file = std::fopen(kPrefabPath, "rb");
#endif
    if (file != nullptr) {
      static_cast<void>(std::fclose(file));
      return 305;
    }
  }
  if (!world->remove_collider(hullEntity)) {
    return 306;
  }
  hullCollider.hullSource = engine::math::HullSource::Cylinder;
  if (!world->add_collider(hullEntity, hullCollider) ||
      !engine::runtime::save_prefab(*world, hullEntity, kPrefabPath)) {
    return 307; // provenance-backed hull payload must stay savable
  }
  remove_prefab_file();

  engine::runtime::Collider heightfieldCollider{};
  heightfieldCollider.shape = engine::runtime::ColliderShape::Heightfield;
  if (!world->add_collider(heightfieldEntity, heightfieldCollider)) {
    return 308;
  }
  if (!engine::runtime::save_prefab(*world, heightfieldEntity, kPrefabPath)) {
    return 309; // descriptor-only heightfield keeps its round trip
  }
  remove_prefab_file();
  engine::physics::HeightfieldData heightfield{};
  heightfield.rows = 2U;
  heightfield.columns = 2U;
  if (!engine::physics::set_heightfield_data(world->physics_context(),
                                             heightfieldEntity, heightfield)) {
    return 310;
  }
  if (engine::runtime::save_prefab(*world, heightfieldEntity, kPrefabPath)) {
    return 311; // heightfield payload must refuse the save
  }

  return 0;
}


/// Overlong authored names must reject prefab instantiation whole (issue
/// #387): a name past NameComponent's capacity fails the load and leaves
/// the world without a partial entity, while a capacity-sized name
/// instantiates byte-exact.
int verify_overlong_prefab_name_rejected() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 400;
  }

  // 32 'n's: one byte past the 31-char capacity.
  const char *overlong =
      "{\"version\":1,\"components\":{\"Transform\":{},"
      "\"NameComponent\":{\"name\":"
      "\"nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn\"}}}";
  {
    std::FILE *file = nullptr;
#ifdef _WIN32
    if (fopen_s(&file, kPrefabPath, "wb") != 0) {
      file = nullptr;
    }
#else
    file = std::fopen(kPrefabPath, "wb");
#endif
    if (file == nullptr) {
      return 401;
    }
    const bool wrote =
        std::fwrite(overlong, 1U, std::strlen(overlong), file) ==
        std::strlen(overlong);
    std::fclose(file);
    if (!wrote) {
      return 402;
    }
  }

  const std::size_t before = world->alive_entity_count();
  if (engine::runtime::instantiate_prefab(*world, kPrefabPath) !=
      engine::runtime::kInvalidEntity) {
    return 403;
  }
  if (world->alive_entity_count() != before) {
    return 404;
  }

  // Boundary: exactly 31 characters instantiates with the name intact.
  const char *boundary =
      "{\"version\":1,\"components\":{\"Transform\":{},"
      "\"NameComponent\":{\"name\":"
      "\"nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn\"}}}";
  {
    std::FILE *file = nullptr;
#ifdef _WIN32
    if (fopen_s(&file, kPrefabPath, "wb") != 0) {
      file = nullptr;
    }
#else
    file = std::fopen(kPrefabPath, "wb");
#endif
    if (file == nullptr) {
      return 405;
    }
    const bool wrote =
        std::fwrite(boundary, 1U, std::strlen(boundary), file) ==
        std::strlen(boundary);
    std::fclose(file);
    if (!wrote) {
      return 406;
    }
  }

  const engine::runtime::Entity instantiated =
      engine::runtime::instantiate_prefab(*world, kPrefabPath);
  if (instantiated == engine::runtime::kInvalidEntity) {
    return 407;
  }
  engine::runtime::NameComponent name{};
  if (!world->get_name_component(instantiated, &name) ||
      (std::strlen(name.name) !=
       engine::runtime::NameComponent::kMaxNameLength)) {
    return 408;
  }
  return 0;
}

/// Reads the whole prefab file; empty on failure.
std::size_t read_prefab_text(char *buffer, std::size_t capacity) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kPrefabPath, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kPrefabPath, "rb");
#endif
  if (file == nullptr) {
    return 0U;
  }
  const std::size_t read = std::fread(buffer, 1U, capacity - 1U, file);
  std::fclose(file);
  buffer[read] = '\0';
  return read;
}

/// A prefab captured from a child entity must not carry its parent link
/// (issue #412): the saved Transform row is a root, and an instance never
/// adopts a parentId from the document even when the target scene owns an
/// entity under that persistent id. Also pins that a root source stays a
/// root and that the parent's own persistent id is untouched.
int verify_child_prefab_instantiates_as_root() {
  using namespace engine::runtime;

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 500;
  }
  const Entity parent = world->create_scene_object();
  const Entity child = world->create_scene_object();
  if ((parent == kInvalidEntity) || (child == kInvalidEntity)) {
    return 501;
  }
  const engine::core::PersistentId parentId = world->persistent_id(parent);
  Transform childTransform{};
  if ((parentId == engine::core::kInvalidPersistentId) ||
      !world->get_transform(child, &childTransform)) {
    return 502;
  }
  childTransform.position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  childTransform.parentId = parentId;
  if (!world->add_transform(child, childTransform)) {
    return 503;
  }

  remove_prefab_file();
  if (!save_prefab(*world, child, kPrefabPath)) {
    return 504;
  }
  // The document itself must not name the source parent: exactly the root
  // value is written.
  static char text[8192] = {};
  if (read_prefab_text(text, sizeof(text)) == 0U) {
    return 505;
  }
  char foreign[48] = {};
  std::snprintf(foreign, sizeof(foreign), "\"parentId\":%u",
                static_cast<unsigned>(parentId));
  if ((std::strstr(text, "\"parentId\":0") == nullptr) ||
      (std::strstr(text, foreign) != nullptr)) {
    return 506;
  }

  // Instantiate into the scene that still owns the source parent's id: the
  // instance is a root, with the rest of the Transform intact.
  const Entity instance = instantiate_prefab(*world, kPrefabPath);
  Transform instanceTransform{};
  if ((instance == kInvalidEntity) ||
      !world->get_transform(instance, &instanceTransform)) {
    return 507;
  }
  if ((instanceTransform.parentId != engine::core::kInvalidPersistentId) ||
      (instanceTransform.position.x != 1.0F) ||
      (instanceTransform.position.y != 2.0F) ||
      (instanceTransform.position.z != 3.0F)) {
    return 508;
  }

  // A hand-authored (or pre-fix) document carrying a parentId that names a
  // live entity here is normalized to a root rather than re-parented.
  char authored[160] = {};
  std::snprintf(authored, sizeof(authored),
                "{\"version\":1,\"components\":{\"Transform\":{"
                "\"position\":[4,5,6],\"parentId\":%u}}}",
                static_cast<unsigned>(parentId));
  if (!write_prefab_text(authored)) {
    return 509;
  }
  const Entity normalized = instantiate_prefab(*world, kPrefabPath);
  Transform normalizedTransform{};
  if ((normalized == kInvalidEntity) ||
      !world->get_transform(normalized, &normalizedTransform) ||
      (normalizedTransform.parentId != engine::core::kInvalidPersistentId) ||
      (normalizedTransform.position.x != 4.0F)) {
    return 510;
  }

  // Boundary: a root source stays a root, and the parent entity keeps its
  // own persistent id through every instantiate above.
  remove_prefab_file();
  if (!save_prefab(*world, parent, kPrefabPath)) {
    return 511;
  }
  const Entity rootInstance = instantiate_prefab(*world, kPrefabPath);
  Transform rootTransform{};
  if ((rootInstance == kInvalidEntity) ||
      !world->get_transform(rootInstance, &rootTransform) ||
      (rootTransform.parentId != engine::core::kInvalidPersistentId) ||
      (world->persistent_id(parent) != parentId)) {
    return 512;
  }
  return 0;
}

int main() {
  remove_prefab_file();

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 1;
  }

  const engine::runtime::Entity src = world->create_entity();
  if (src == engine::runtime::kInvalidEntity) {
    return 2;
  }

  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  transform.scale = engine::math::Vec3(2.0F, 2.0F, 2.0F);
  if (!world->add_transform(src, transform)) {
    return 3;
  }

  engine::runtime::RigidBody rigidBody{};
  rigidBody.inverseMass = 0.5F;
  if (!world->add_rigid_body(src, rigidBody)) {
    return 4;
  }

  engine::runtime::Collider collider{};
  collider.halfExtents = engine::math::Vec3(0.5F, 1.0F, 0.5F);
  collider.restitution = 0.7F;
  if (!world->add_collider(src, collider)) {
    return 5;
  }

  engine::runtime::NameComponent nameComp{};
  std::snprintf(nameComp.name, sizeof(nameComp.name), "%s", kPrefabSourceName);
  if (!world->add_name_component(src, nameComp)) {
    return 6;
  }

  engine::runtime::ScriptComponent scriptComp{};
  std::snprintf(scriptComp.scriptPath, sizeof(scriptComp.scriptPath), "%s",
                kPrefabScriptPath);
  if (!world->add_script_component(src, scriptComp)) {
    return 32;
  }

  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = 42U;
  mesh.albedo = engine::math::Vec3(0.8F, 0.2F, 0.4F);
  mesh.roughness = 0.6F;
  mesh.metallic = 0.1F;
  mesh.opacity = 0.9F;
  mesh.sceneCaptureSourceId = 6U;
  if (!world->add_mesh_component(src, mesh)) {
    return 7;
  }

  engine::runtime::LightComponent light{};
  light.color = engine::math::Vec3(1.0F, 0.8F, 0.6F);
  light.intensity = 2.5F;
  light.type = engine::runtime::LightType::Point;
  if (!world->add_light_component(src, light)) {
    return 8;
  }

  engine::runtime::ReflectionProbeComponent reflectionProbe{};
  reflectionProbe.boxExtents = engine::math::Vec3(4.0F, 5.0F, 6.0F);
  reflectionProbe.radius = 14.0F;
  reflectionProbe.intensity = 0.75F;
  reflectionProbe.prefilteredResolution = 256U;
  reflectionProbe.irradianceResolution = 64U;
  reflectionProbe.mipLevels = 6U;
  reflectionProbe.boxProjection = true;
  reflectionProbe.needsBake = false;
  if (!world->add_reflection_probe_component(src, reflectionProbe)) {
    return 26;
  }

  engine::runtime::SceneCaptureComponent sceneCapture{};
  sceneCapture.width = 400U;
  sceneCapture.height = 300U;
  sceneCapture.fovRadians = 1.2F;
  sceneCapture.nearPlane = 0.2F;
  sceneCapture.farPlane = 80.0F;
  sceneCapture.enabled = false;
  if (!world->add_scene_capture_component(src, sceneCapture)) {
    return 45;
  }

  engine::runtime::FoliagePatchComponent foliage{};
  foliage.meshAssetIds[0] = 77U;
  foliage.meshAssetIds[1] = 88U;
  foliage.instanceCount = 2U;
  foliage.density = 1.75F;
  foliage.albedo = engine::math::Vec3(0.2F, 0.8F, 0.25F);
  foliage.windStrength = 0.3F;
  foliage.windFrequency = 1.4F;
  foliage.instances[0].offset = engine::math::Vec3(-0.5F, 0.0F, 0.25F);
  foliage.instances[0].scale = 0.6F;
  foliage.instances[0].phase = 0.2F;
  foliage.instances[0].lodIndex = 0U;
  foliage.instances[1].offset = engine::math::Vec3(0.75F, 0.0F, -0.5F);
  foliage.instances[1].scale = 0.9F;
  foliage.instances[1].phase = 1.2F;
  foliage.instances[1].lodIndex = 1U;
  if (!world->add_foliage_patch_component(src, foliage)) {
    return 29;
  }

  if (!engine::runtime::save_prefab(*world, src, kPrefabPath)) {
    remove_prefab_file();
    return 9;
  }

  // Instantiate into the same world (creates a new entity).
  const engine::runtime::Entity inst =
      engine::runtime::instantiate_prefab(*world, kPrefabPath);
  if (inst == engine::runtime::kInvalidEntity) {
    remove_prefab_file();
    return 10;
  }

  engine::runtime::Transform instTransform{};
  if (!world->get_transform(inst, &instTransform)) {
    remove_prefab_file();
    return 11;
  }
  if (!nearly_equal(instTransform.position.x, 1.0F) ||
      !nearly_equal(instTransform.position.y, 2.0F) ||
      !nearly_equal(instTransform.position.z, 3.0F)) {
    remove_prefab_file();
    return 12;
  }
  if (!nearly_equal(instTransform.scale.x, 2.0F) ||
      !nearly_equal(instTransform.scale.y, 2.0F) ||
      !nearly_equal(instTransform.scale.z, 2.0F)) {
    remove_prefab_file();
    return 13;
  }

  engine::runtime::RigidBody instRb{};
  if (!world->get_rigid_body(inst, &instRb)) {
    remove_prefab_file();
    return 14;
  }
  if (!nearly_equal(instRb.inverseMass, 0.5F)) {
    remove_prefab_file();
    return 15;
  }

  engine::runtime::Collider instCol{};
  if (!world->get_collider(inst, &instCol)) {
    remove_prefab_file();
    return 16;
  }
  if (!nearly_equal(instCol.halfExtents.y, 1.0F) ||
      !nearly_equal(instCol.restitution, 0.7F)) {
    remove_prefab_file();
    return 17;
  }

  engine::runtime::NameComponent instName{};
  if (!world->get_name_component(inst, &instName)) {
    remove_prefab_file();
    return 18;
  }
  if (std::strcmp(instName.name, kPrefabSourceName) != 0) {
    remove_prefab_file();
    return 19;
  }

  engine::runtime::ScriptComponent instScript{};
  if (!world->get_script_component(inst, &instScript)) {
    remove_prefab_file();
    return 42;
  }
  if (std::strcmp(instScript.scriptPath, kPrefabScriptPath) != 0) {
    remove_prefab_file();
    return 43;
  }

  engine::runtime::MeshComponent instMesh{};
  if (!world->get_mesh_component(inst, &instMesh)) {
    remove_prefab_file();
    return 20;
  }
  if (instMesh.meshAssetId != 42U || !nearly_equal(instMesh.albedo.x, 0.8F) ||
      !nearly_equal(instMesh.roughness, 0.6F) ||
      !nearly_equal(instMesh.metallic, 0.1F) ||
      !nearly_equal(instMesh.opacity, 0.9F) ||
      (instMesh.sceneCaptureSourceId != 6U)) {
    remove_prefab_file();
    return 21;
  }

  engine::runtime::LightComponent instLight{};
  if (!world->get_light_component(inst, &instLight)) {
    remove_prefab_file();
    return 22;
  }
  if (!nearly_equal(instLight.color.x, 1.0F) ||
      !nearly_equal(instLight.color.y, 0.8F) ||
      !nearly_equal(instLight.color.z, 0.6F) ||
      !nearly_equal(instLight.intensity, 2.5F) ||
      instLight.type != engine::runtime::LightType::Point) {
    remove_prefab_file();
    return 23;
  }

  engine::runtime::ReflectionProbeComponent instProbe{};
  if (!world->get_reflection_probe_component(inst, &instProbe)) {
    remove_prefab_file();
    return 27;
  }
  if (!nearly_equal(instProbe.boxExtents.y, 5.0F) ||
      !nearly_equal(instProbe.radius, 14.0F) ||
      !nearly_equal(instProbe.intensity, 0.75F) ||
      (instProbe.prefilteredResolution != 256U) ||
      (instProbe.irradianceResolution != 64U) || (instProbe.mipLevels != 6U) ||
      !instProbe.boxProjection || instProbe.needsBake) {
    remove_prefab_file();
    return 28;
  }

  engine::runtime::SceneCaptureComponent instCapture{};
  if (!world->get_scene_capture_component(inst, &instCapture)) {
    remove_prefab_file();
    return 46;
  }
  if ((instCapture.width != 400U) || (instCapture.height != 300U) ||
      !nearly_equal(instCapture.fovRadians, 1.2F) ||
      !nearly_equal(instCapture.nearPlane, 0.2F) ||
      !nearly_equal(instCapture.farPlane, 80.0F) || instCapture.enabled) {
    remove_prefab_file();
    return 47;
  }

  engine::runtime::FoliagePatchComponent instFoliage{};
  if (!world->get_foliage_patch_component(inst, &instFoliage)) {
    remove_prefab_file();
    return 30;
  }
  if ((instFoliage.meshAssetIds[0] != 77U) ||
      (instFoliage.meshAssetIds[1] != 88U) ||
      (instFoliage.instanceCount != 2U) ||
      !nearly_equal(instFoliage.density, 1.75F) ||
      !nearly_equal(instFoliage.albedo.y, 0.8F) ||
      !nearly_equal(instFoliage.windStrength, 0.3F) ||
      !nearly_equal(instFoliage.windFrequency, 1.4F) ||
      !nearly_equal(instFoliage.instances[1].offset.x, 0.75F) ||
      !nearly_equal(instFoliage.instances[1].scale, 0.9F) ||
      (instFoliage.instances[1].lodIndex != 1U)) {
    remove_prefab_file();
    return 31;
  }

  if (engine::runtime::save_prefab(*world, engine::runtime::kInvalidEntity,
                                   kPrefabPath)) {
    remove_prefab_file();
    return 24;
  }
  if (engine::runtime::instantiate_prefab(*world, nullptr) !=
      engine::runtime::kInvalidEntity) {
    remove_prefab_file();
    return 25;
  }

  int result = verify_instantiate_rejects_malformed_component();
  if (result != 0) {
    remove_prefab_file();
    return result;
  }

  result = verify_instantiate_validates_schema_version();
  if (result != 0) {
    remove_prefab_file();
    return result;
  }

  result = verify_instantiate_rolls_back_on_component_add_failure();
  if (result != 0) {
    remove_prefab_file();
    return result;
  }

  result = verify_collider_prefab_round_trip();
  if (result != 0) {
    remove_prefab_file();
    return result;
  }

  result = verify_spring_arm_prefab_round_trip();
  if (result != 0) {
    remove_prefab_file();
    return result;
  }

  result = verify_animation_prefab_round_trip();
  if (result != 0) {
    remove_prefab_file();
    return result;
  }

  result = verify_overlong_prefab_name_rejected();
  if (result != 0) {
    remove_prefab_file();
    return result;
  }

  result = verify_prefab_save_refuses_unserializable_payloads();
  if (result != 0) {
    remove_prefab_file();
    return result;
  }

  result = verify_child_prefab_instantiates_as_root();
  if (result != 0) {
    remove_prefab_file();
    return result;
  }

  remove_prefab_file();
  return 0;
}
