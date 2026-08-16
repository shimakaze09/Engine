// Cross-validates every persistent-component registry row through the
// production serializers (audit N-16): each component type round-trips a
// non-default value through save_scene/load_scene (staged world + commit
// copy + count invariant included) and save_prefab/instantiate_prefab, and
// the reloaded component is compared field-by-field against the original.
// The rows expand from ENGINE_PERSISTENT_COMPONENT_TABLE, so adding a
// registry row without a make_test_value/components_equal overload here
// fails to compile. Serialized floats round-trip exactly (%.9g), so every
// comparison is exact.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "component_registry.h"
#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

using namespace engine::runtime;

namespace {

constexpr const char *kPrefabPath = "component_registry_prefab_temp.json";
constexpr PersistentId kParentPersistentId = 7001U;
constexpr PersistentId kSubjectPersistentId = 7002U;
constexpr std::size_t kSceneBufferSize = 64U * 1024U;

void remove_prefab_file() noexcept {
  static_cast<void>(std::remove(kPrefabPath));
}

/// Writes raw prefab JSON for the legacy/compatibility cases.
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

/// Exact Vec3 comparison (serialized floats round-trip exactly).
bool vec3_equal(const engine::math::Vec3 &a,
                const engine::math::Vec3 &b) noexcept {
  return (a.x == b.x) && (a.y == b.y) && (a.z == b.z);
}

/// Exact Quat comparison (serialized floats round-trip exactly).
bool quat_equal(const engine::math::Quat &a,
                const engine::math::Quat &b) noexcept {
  return (a.x == b.x) && (a.y == b.y) && (a.z == b.z) && (a.w == b.w);
}

// --- Per-row non-default test values and persisted-field comparators -------
// One overload pair per registry row; every value differs from the field's
// default so a dropped field cannot pass unnoticed. Comparators cover
// exactly the field set the codecs persist.

void make_test_value(Transform *out) noexcept {
  out->position = engine::math::Vec3(1.5F, -2.25F, 3.75F);
  out->rotation = engine::math::Quat(0.0F, 1.0F, 0.0F, 0.0F);
  out->scale = engine::math::Vec3(2.0F, 0.5F, 1.25F);
  out->parentId = kParentPersistentId;
}

bool components_equal(const Transform &a, const Transform &b) noexcept {
  return vec3_equal(a.position, b.position) &&
         quat_equal(a.rotation, b.rotation) && vec3_equal(a.scale, b.scale) &&
         (a.parentId == b.parentId);
}

void make_test_value(RigidBody *out) noexcept {
  out->velocity = engine::math::Vec3(1.25F, -2.5F, 3.5F);
  out->acceleration = engine::math::Vec3(0.5F, 9.75F, -1.5F);
  out->angularVelocity = engine::math::Vec3(0.25F, 0.5F, -0.75F);
  out->inverseMass = 0.5F;
  out->inverseInertia = 0.25F;
  out->sleeping = true;
}

bool components_equal(const RigidBody &a, const RigidBody &b) noexcept {
  return vec3_equal(a.velocity, b.velocity) &&
         vec3_equal(a.acceleration, b.acceleration) &&
         vec3_equal(a.angularVelocity, b.angularVelocity) &&
         (a.inverseMass == b.inverseMass) &&
         (a.inverseInertia == b.inverseInertia) &&
         (a.sleeping == b.sleeping);
}

void make_test_value(Collider *out) noexcept {
  out->shape = ColliderShape::Sphere;
  out->hullSource = HullSource::None;
  out->localPosition = engine::math::Vec3(0.25F, 0.5F, -0.75F);
  out->localRotation = engine::math::Quat(0.0F, 0.0F, 1.0F, 0.0F);
  out->halfExtents = engine::math::Vec3(0.75F, 1.5F, 2.25F);
  out->restitution = 0.625F;
  out->staticFriction = 0.75F;
  out->dynamicFriction = 0.125F;
  out->density = 2.5F;
  out->collisionLayer = 3U;
  out->collisionMask = 5U;
}

bool components_equal(const Collider &a, const Collider &b) noexcept {
  return (a.shape == b.shape) && (a.hullSource == b.hullSource) &&
         vec3_equal(a.localPosition, b.localPosition) &&
         quat_equal(a.localRotation, b.localRotation) &&
         vec3_equal(a.halfExtents, b.halfExtents) &&
         (a.restitution == b.restitution) &&
         (a.staticFriction == b.staticFriction) &&
         (a.dynamicFriction == b.dynamicFriction) &&
         (a.density == b.density) && (a.collisionLayer == b.collisionLayer) &&
         (a.collisionMask == b.collisionMask);
}

void make_test_value(MeshComponent *out) noexcept {
  out->meshAssetId = 0x123456789ABCDEFULL;
  out->materialAssetId = 77ULL;
  out->albedo = engine::math::Vec3(0.25F, 0.5F, 0.75F);
  out->roughness = 0.375F;
  out->metallic = 0.625F;
  out->opacity = 0.5F;
  out->sceneCaptureSourceId = 9U;
}

bool components_equal(const MeshComponent &a,
                      const MeshComponent &b) noexcept {
  return (a.meshAssetId == b.meshAssetId) &&
         (a.materialAssetId == b.materialAssetId) &&
         vec3_equal(a.albedo, b.albedo) && (a.roughness == b.roughness) &&
         (a.metallic == b.metallic) && (a.opacity == b.opacity) &&
         (a.sceneCaptureSourceId == b.sceneCaptureSourceId);
}

void make_test_value(NameComponent *out) noexcept {
  std::snprintf(out->name, sizeof(out->name), "%s", "Registry \"Subject\"");
}

bool components_equal(const NameComponent &a,
                      const NameComponent &b) noexcept {
  return std::strcmp(a.name, b.name) == 0;
}

void make_test_value(LightComponent *out) noexcept {
  out->color = engine::math::Vec3(0.75F, 0.5F, 0.25F);
  out->direction = engine::math::Vec3(1.0F, 0.25F, -0.5F);
  out->intensity = 2.5F;
  out->type = LightType::Point;
}

bool components_equal(const LightComponent &a,
                      const LightComponent &b) noexcept {
  return vec3_equal(a.color, b.color) && vec3_equal(a.direction, b.direction) &&
         (a.intensity == b.intensity) && (a.type == b.type);
}

void make_test_value(ScriptComponent *out) noexcept {
  std::snprintf(out->scriptPath, sizeof(out->scriptPath), "%s",
                "assets/scripts/registry_subject.lua");
}

bool components_equal(const ScriptComponent &a,
                      const ScriptComponent &b) noexcept {
  return std::strcmp(a.scriptPath, b.scriptPath) == 0;
}

void make_test_value(SpringArmComponent *out) noexcept {
  out->armLength = 4.5F;
  out->currentLength = 3.25F;
  out->offset = engine::math::Vec3(0.5F, 1.5F, -0.75F);
  out->lagSpeed = 7.5F;
  out->collisionRadius = 0.375F;
  out->collisionEnabled = false;
}

bool components_equal(const SpringArmComponent &a,
                      const SpringArmComponent &b) noexcept {
  return (a.armLength == b.armLength) &&
         (a.currentLength == b.currentLength) &&
         vec3_equal(a.offset, b.offset) && (a.lagSpeed == b.lagSpeed) &&
         (a.collisionRadius == b.collisionRadius) &&
         (a.collisionEnabled == b.collisionEnabled);
}

void make_test_value(PointLightComponent *out) noexcept {
  out->color = engine::math::Vec3(0.25F, 0.5F, 0.75F);
  out->intensity = 3.5F;
  out->radius = 12.5F;
}

bool components_equal(const PointLightComponent &a,
                      const PointLightComponent &b) noexcept {
  return vec3_equal(a.color, b.color) && (a.intensity == b.intensity) &&
         (a.radius == b.radius);
}

void make_test_value(SpotLightComponent *out) noexcept {
  out->color = engine::math::Vec3(0.5F, 0.25F, 0.75F);
  out->direction = engine::math::Vec3(0.25F, -0.5F, 0.75F);
  out->intensity = 4.5F;
  out->radius = 15.25F;
  out->innerConeAngle = 0.25F;
  out->outerConeAngle = 0.75F;
}

bool components_equal(const SpotLightComponent &a,
                      const SpotLightComponent &b) noexcept {
  return vec3_equal(a.color, b.color) && vec3_equal(a.direction, b.direction) &&
         (a.intensity == b.intensity) && (a.radius == b.radius) &&
         (a.innerConeAngle == b.innerConeAngle) &&
         (a.outerConeAngle == b.outerConeAngle);
}

void make_test_value(ReflectionProbeComponent *out) noexcept {
  out->boxExtents = engine::math::Vec3(1.5F, 2.5F, 3.5F);
  out->radius = 20.5F;
  out->intensity = 1.25F;
  out->prefilteredResolution = 64U;
  out->irradianceResolution = 16U;
  out->brdfLutResolution = 128U;
  out->mipLevels = 4U;
  out->boxProjection = true;
  out->needsBake = false;
}

bool components_equal(const ReflectionProbeComponent &a,
                      const ReflectionProbeComponent &b) noexcept {
  return vec3_equal(a.boxExtents, b.boxExtents) && (a.radius == b.radius) &&
         (a.intensity == b.intensity) &&
         (a.prefilteredResolution == b.prefilteredResolution) &&
         (a.irradianceResolution == b.irradianceResolution) &&
         (a.brdfLutResolution == b.brdfLutResolution) &&
         (a.mipLevels == b.mipLevels) &&
         (a.boxProjection == b.boxProjection) &&
         (a.needsBake == b.needsBake);
}

void make_test_value(SceneCaptureComponent *out) noexcept {
  out->width = 320U;
  out->height = 240U;
  out->fovRadians = 1.25F;
  out->nearPlane = 0.25F;
  out->farPlane = 64.5F;
  out->enabled = false;
}

bool components_equal(const SceneCaptureComponent &a,
                      const SceneCaptureComponent &b) noexcept {
  return (a.width == b.width) && (a.height == b.height) &&
         (a.fovRadians == b.fovRadians) && (a.nearPlane == b.nearPlane) &&
         (a.farPlane == b.farPlane) && (a.enabled == b.enabled);
}

void make_test_value(FoliagePatchComponent *out) noexcept {
  out->meshAssetIds[0] = 101ULL;
  out->meshAssetIds[1] = 202ULL;
  out->instanceCount = 2U;
  out->density = 1.75F;
  out->albedo = engine::math::Vec3(0.125F, 0.75F, 0.375F);
  out->roughness = 0.25F;
  out->metallic = 0.5F;
  out->opacity = 0.75F;
  out->windStrength = 0.375F;
  out->windFrequency = 2.5F;
  out->instances[0].offset = engine::math::Vec3(-0.5F, 0.25F, 0.75F);
  out->instances[0].scale = 0.625F;
  out->instances[0].phase = 0.25F;
  out->instances[0].lodIndex = 1U;
  out->instances[1].offset = engine::math::Vec3(0.75F, -0.25F, -0.5F);
  out->instances[1].scale = 0.875F;
  out->instances[1].phase = 1.25F;
  out->instances[1].lodIndex = 2U;
}

bool components_equal(const FoliagePatchComponent &a,
                      const FoliagePatchComponent &b) noexcept {
  if ((a.instanceCount != b.instanceCount) || (a.density != b.density) ||
      !vec3_equal(a.albedo, b.albedo) || (a.roughness != b.roughness) ||
      (a.metallic != b.metallic) || (a.opacity != b.opacity) ||
      (a.windStrength != b.windStrength) ||
      (a.windFrequency != b.windFrequency)) {
    return false;
  }
  for (std::size_t i = 0U; i < FoliagePatchComponent::kMaxLods; ++i) {
    if (a.meshAssetIds[i] != b.meshAssetIds[i]) {
      return false;
    }
  }
  for (std::uint32_t i = 0U; i < a.instanceCount; ++i) {
    if (!vec3_equal(a.instances[i].offset, b.instances[i].offset) ||
        (a.instances[i].scale != b.instances[i].scale) ||
        (a.instances[i].phase != b.instances[i].phase) ||
        (a.instances[i].lodIndex != b.instances[i].lodIndex)) {
      return false;
    }
  }
  return true;
}

void make_test_value(AnimationComponent *out) noexcept {
  std::snprintf(out->controllerPath, sizeof(out->controllerPath), "%s",
                "assets/anim/registry_controller.json");
}

bool components_equal(const AnimationComponent &a,
                      const AnimationComponent &b) noexcept {
  return std::strcmp(a.controllerPath, b.controllerPath) == 0;
}

void make_test_value(CameraComponent *out) noexcept {
  out->projection = static_cast<std::uint32_t>(CameraProjection::Orthographic);
  out->fovRadians = 0.95F;
  out->orthographicSize = 4.25F;
  out->nearPlane = 0.15F;
  out->farPlane = 175.0F;
  out->priority = 3.5F;
  out->blendSpeed = 7.5F;
  out->active = false;
}

bool components_equal(const CameraComponent &a,
                      const CameraComponent &b) noexcept {
  return (a.projection == b.projection) && (a.fovRadians == b.fovRadians) &&
         (a.orthographicSize == b.orthographicSize) &&
         (a.nearPlane == b.nearPlane) && (a.farPlane == b.farPlane) &&
         (a.priority == b.priority) && (a.blendSpeed == b.blendSpeed) &&
         (a.active == b.active);
}

/// Round-trips one registry row's component through both production
/// serializers and compares the reloaded value field-by-field. Returns 0 on
/// success or a stage code identifying the first failing step.
template <typename Component>
int verify_registry_round_trip(
    bool (World::*getComponent)(Entity, Component *) const noexcept,
    bool (World::*addComponent)(Entity, const Component &) noexcept) {
  remove_prefab_file();

  std::unique_ptr<World> source(new (std::nothrow) World());
  if (source == nullptr) {
    return 1;
  }
  const Entity parent =
      source->create_scene_object_with_persistent_id(kParentPersistentId);
  const Entity subject =
      source->create_scene_object_with_persistent_id(kSubjectPersistentId);
  if ((parent == kInvalidEntity) || (subject == kInvalidEntity)) {
    return 2;
  }

  Component value{};
  make_test_value(&value);
  if (!((*source).*addComponent)(subject, value)) {
    return 3;
  }

  std::unique_ptr<char[]> buffer(new (std::nothrow) char[kSceneBufferSize]);
  if (buffer == nullptr) {
    return 4;
  }
  std::size_t sceneSize = 0U;
  if (!save_scene(*source, buffer.get(), kSceneBufferSize, &sceneSize)) {
    return 5;
  }

  std::unique_ptr<World> sceneLoaded(new (std::nothrow) World());
  if ((sceneLoaded == nullptr) ||
      !load_scene(*sceneLoaded, buffer.get(), sceneSize)) {
    return 6;
  }
  const Entity sceneSubject =
      sceneLoaded->find_entity_by_persistent_id(kSubjectPersistentId);
  Component sceneValue{};
  if ((sceneSubject == kInvalidEntity) ||
      !((*sceneLoaded).*getComponent)(sceneSubject, &sceneValue)) {
    return 7;
  }
  if (!components_equal(value, sceneValue)) {
    return 8;
  }

  if (!save_prefab(*source, subject, kPrefabPath)) {
    return 9;
  }
  std::unique_ptr<World> target(new (std::nothrow) World());
  if (target == nullptr) {
    remove_prefab_file();
    return 10;
  }
  if (target->create_scene_object_with_persistent_id(kParentPersistentId) ==
      kInvalidEntity) {
    remove_prefab_file();
    return 11;
  }
  const Entity instance = instantiate_prefab(*target, kPrefabPath);
  remove_prefab_file();
  if (instance == kInvalidEntity) {
    return 12;
  }
  Component prefabValue{};
  if (!((*target).*getComponent)(instance, &prefabValue)) {
    return 13;
  }
  if (!components_equal(value, prefabValue)) {
    return 14;
  }

  return 0;
}

/// EXPECTATION: a prefab Transform without parentId (every prefab written
/// before the field existed) still loads, defaulting to no parent — the
/// compatible-widening contract for the parentId addition.
int verify_prefab_transform_without_parent_id() {
  constexpr const char *kLegacyPrefab =
      "{\"version\":1,\"components\":{\"Transform\":{"
      "\"position\":[1.5,2.5,3.5],\"rotation\":[0,0,0,1],"
      "\"scale\":[1,1,1]}}}";
  remove_prefab_file();
  if (!write_prefab_text(kLegacyPrefab)) {
    return 61;
  }

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    remove_prefab_file();
    return 62;
  }
  const Entity entity = instantiate_prefab(*world, kPrefabPath);
  remove_prefab_file();
  if (entity == kInvalidEntity) {
    return 63;
  }
  Transform transform{};
  if (!world->get_transform(entity, &transform)) {
    return 64;
  }
  if ((transform.position.x != 1.5F) ||
      (transform.parentId != kInvalidPersistentId)) {
    return 65;
  }
  return 0;
}

/// EXPECTATION: the prefab reader honors the legacy "meshId" key exactly
/// like the scene reader does (codec parity for pre-asset-id content).
int verify_prefab_legacy_mesh_id() {
  constexpr const char *kLegacyPrefab =
      "{\"version\":1,\"components\":{\"MeshComponent\":{\"meshId\":4242}}}";
  remove_prefab_file();
  if (!write_prefab_text(kLegacyPrefab)) {
    return 71;
  }

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    remove_prefab_file();
    return 72;
  }
  const Entity entity = instantiate_prefab(*world, kPrefabPath);
  remove_prefab_file();
  if (entity == kInvalidEntity) {
    return 73;
  }
  MeshComponent mesh{};
  if (!world->get_mesh_component(entity, &mesh)) {
    return 74;
  }
  if (mesh.meshAssetId != 4242ULL) {
    return 75;
  }
  return 0;
}

} // namespace

// Count tripwire: bumping this is an intentional act that accompanies a new
// registry row, a World::PersistentComponentTypes entry, and the test-value/
// comparator overloads above.
static_assert(engine::runtime::kPersistentComponentTypeCount == 15U,
              "new persistent component type: extend the registry table, the "
              "World type list, and this suite's overloads together");

/// Runs the per-row registry round trips and the prefab compatibility cases.
int main() {
  int rowBase = 100;
  int stage = 0;
#define ENGINE_PCR_RUN_ROW(Type, Key, GetFn, AddFn, RemoveFn)                  \
  stage = verify_registry_round_trip<Type>(&World::GetFn, &World::AddFn);      \
  if (stage != 0) {                                                            \
    std::printf("component_registry: %s round trip failed at stage %d\n",      \
                #Type, stage);                                                 \
    remove_prefab_file();                                                      \
    return rowBase + stage;                                                    \
  }                                                                            \
  rowBase += 20;
  ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_PCR_RUN_ROW)
#undef ENGINE_PCR_RUN_ROW

  int result = verify_prefab_transform_without_parent_id();
  if (result != 0) {
    return result;
  }

  result = verify_prefab_legacy_mesh_id();
  if (result != 0) {
    return result;
  }

  return 0;
}
