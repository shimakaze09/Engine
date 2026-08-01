// Implements World component storage: the checked add/remove/get
// helpers, movement authority, and every component type's accessor
// surface (transforms, bodies, colliders, meshes, lights, probes,
// captures, scripts, names, foliage, spring arms).

#include "engine/runtime/world.h"

#include "engine/core/hash.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/math/transform.h"
#include "engine/physics/physics.h"
#include "engine/physics/primitive_hulls.h"
#include "engine/runtime/reflect_types.h"
#include "world_internal.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine::runtime {

namespace {

/// True when every component of the vector is finite.
bool finite_vec3(const math::Vec3 &value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

/// True when every component of the quaternion is finite.
bool finite_quat(const math::Quat &value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z) && std::isfinite(value.w);
}

/// Ingress validation (audit H-06): rejects non-finite transform fields so
/// NaN can never enter propagation, physics, or rendering.
bool validate_transform_ingress(const Transform &transform) noexcept {
  return finite_vec3(transform.position) && finite_quat(transform.rotation) &&
         finite_vec3(transform.scale);
}

/// Ingress validation (audit H-06): rigid body fields must be finite and
/// the inverse mass/inertia non-negative.
bool validate_rigid_body_ingress(const RigidBody &rigidBody) noexcept {
  return finite_vec3(rigidBody.velocity) &&
         finite_vec3(rigidBody.acceleration) &&
         finite_vec3(rigidBody.angularVelocity) &&
         std::isfinite(rigidBody.inverseMass) &&
         (rigidBody.inverseMass >= 0.0F) &&
         std::isfinite(rigidBody.inverseInertia) &&
         (rigidBody.inverseInertia >= 0.0F);
}

/// Ingress validation (audit H-06): collider geometry must be finite with
/// strictly positive extents, and material terms finite and non-negative.
bool validate_collider_ingress(const Collider &collider) noexcept {
  return finite_vec3(collider.localPosition) &&
         finite_quat(collider.localRotation) &&
         finite_vec3(collider.halfExtents) && (collider.halfExtents.x > 0.0F) &&
         (collider.halfExtents.y > 0.0F) && (collider.halfExtents.z > 0.0F) &&
         std::isfinite(collider.restitution) &&
         (collider.restitution >= 0.0F) &&
         std::isfinite(collider.staticFriction) &&
         (collider.staticFriction >= 0.0F) &&
         std::isfinite(collider.dynamicFriction) &&
         (collider.dynamicFriction >= 0.0F) &&
         std::isfinite(collider.density) && (collider.density >= 0.0F);
}

// Rebuilds the canonical primitive hull recorded in Collider::hullSource so
// every collider install path (scene/prefab load, world copy, editor undo,
// script spawn) restores the payload the component cannot carry itself. On
// failure the component stays as authored and narrow phase falls back to box
// behavior — loudly, never silently.
void install_provenance_hull(physics::PhysicsContext &context, Entity entity,
                             const Collider &collider) noexcept {
  if ((collider.shape != ColliderShape::ConvexHull) ||
      (collider.hullSource == HullSource::None)) {
    return;
  }

  physics::ConvexHullData hull{};
  bool built = false;
  switch (collider.hullSource) {
  case HullSource::Cylinder:
    built = physics::build_cylinder_hull(&hull);
    break;
  case HullSource::Pyramid:
    built = physics::build_pyramid_hull(&hull);
    break;
  default:
    break;
  }

  if (!built || !physics::set_convex_hull_data(context, entity, hull)) {
    char message[128] = {};
    std::snprintf(message, sizeof(message),
                  "convex hull rebuild failed for entity %u (source %u) — "
                  "collider falls back to box behavior",
                  entity.index,
                  static_cast<unsigned>(collider.hullSource));
    core::log_message(core::LogLevel::Warning, "world", message);
  }
}

} // namespace


template <typename Set, typename Component>
bool World::add_component_checked(Set &set, Entity entity,
                                  const Component &component,
                                  const char *label) noexcept {
  if (!is_mutation_phase()) {
    log_component_error(label, "requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    log_component_error(label, "requires a live entity");
    return false;
  }

  return set.add(entity, component);
}

template <typename Set>
bool World::remove_component_checked(Set &set, Entity entity,
                                     const char *label) noexcept {
  if (!is_mutation_phase()) {
    log_component_error(label, "requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    log_component_error(label, "requires a live entity");
    return false;
  }

  return set.remove(entity);
}

template <typename Set, typename Component>
bool World::get_component_checked(const Set &set, Entity entity, Component *out,
                                  const char *label) const noexcept {
  if (out == nullptr) {
    return false;
  }

  if (!is_valid_entity(entity)) {
    log_component_error(label, "on stale or dead entity");
    return false;
  }

  return set.get(entity, out);
}

bool World::check_component_mutation(Entity entity,
                                     const char *label) noexcept {
  if (!is_mutation_phase()) {
    log_component_error(label, "requires Input phase");
    return false;
  }

  if (!is_valid_entity(entity)) {
    log_component_error(label, "requires a live entity");
    return false;
  }

  return true;
}

World::World() noexcept {
  // Every member carries a correct default initializer (arrays zero-init,
  // SparseSets and the persistent-id table self-initialize); only the
  // reflection registry needs explicit setup.
  ensure_runtime_reflection_registered();
}

bool World::add_transform(Entity entity, const Transform &transform) noexcept {
  if (!check_component_mutation(entity, "add_transform")) {
    return false;
  }

  if (!validate_transform_ingress(transform)) {
    core::log_message(core::LogLevel::Error, "world",
                      "add_transform rejected non-finite fields");
    return false;
  }

  const RigidBody *body = m_rigidBodies.get_ptr(entity);
  if ((body != nullptr) && (body->inverseMass > 0.0F) &&
      (transform.parentId != kInvalidPersistentId)) {
    core::log_message(core::LogLevel::Error, "world",
                      "dynamic rigid bodies must be transform roots");
    return false;
  }

  const bool hadTransform = m_transforms.contains(entity);
  if (!m_transforms.add(entity, transform)) {
    return false;
  }

  const WorldTransform world = world_transform_from_local(transform);
  m_transformNodes[entity.index].cacheValid = false;
  if (!m_worldTransforms.add(entity, world)) {
    // Keep the two sets consistent: a fresh insert that cannot get its world
    // transform must not leave a local transform behind.
    if (!hadTransform) {
      static_cast<void>(m_transforms.remove(entity));
    }
    return false;
  }
  return true;
}

bool World::remove_transform(Entity entity) noexcept {
  if (!check_component_mutation(entity, "remove_transform")) {
    return false;
  }

  const bool removed = m_transforms.remove(entity);
  static_cast<void>(m_worldTransforms.remove(entity));
  reset_transform_cache(entity.index);
  return removed;
}

bool World::get_transform(Entity entity,
                          Transform *outTransform) const noexcept {
  if (outTransform == nullptr) {
    return false;
  }

  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "get_transform on stale or dead entity");
    return false;
  }

  return m_transforms.get(entity, outTransform, m_readStateIndex);
}

bool World::get_physics_transform(
    Entity entity, physics::PhysicsTransform *outTransform) const noexcept {
  return build_physics_transform(entity, m_readStateIndex, outTransform);
}

const Transform *World::get_transform_read_ptr(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_transforms.get_ptr(entity, m_readStateIndex);
}

bool World::set_movement_authority(Entity entity,
                                   MovementAuthority authority) noexcept {
  if (!check_component_mutation(entity, "set_movement_authority")) {
    return false;
  }

  m_movementAuthorities[entity.index] = authority;
  return true;
}

MovementAuthority World::movement_authority(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return MovementAuthority::None;
  }

  return m_movementAuthorities[entity.index];
}

bool World::add_rigid_body(Entity entity, const RigidBody &rigidBody) noexcept {
  if (!check_component_mutation(entity, "add_rigid_body")) {
    return false;
  }

  if (!validate_rigid_body_ingress(rigidBody)) {
    core::log_message(
        core::LogLevel::Error, "world",
        "add_rigid_body rejected non-finite or negative fields");
    return false;
  }

  const Transform *transform = m_transforms.get_ptr(entity, m_readStateIndex);
  if ((rigidBody.inverseMass > 0.0F) && (transform != nullptr) &&
      (transform->parentId != kInvalidPersistentId)) {
    core::log_message(core::LogLevel::Error, "world",
                      "dynamic rigid bodies must be transform roots");
    return false;
  }

  return m_rigidBodies.add(entity, rigidBody);
}

bool World::remove_rigid_body(Entity entity) noexcept {
  return remove_component_checked(m_rigidBodies, entity, "remove_rigid_body");
}

bool World::get_rigid_body(Entity entity,
                           RigidBody *outRigidBody) const noexcept {
  return get_component_checked(m_rigidBodies, entity, outRigidBody,
                               "get_rigid_body");
}

bool World::get_rigid_body_range(std::size_t startIndex, std::size_t count,
                                 const Entity **outEntities,
                                 RigidBody **outBodies) noexcept {
  if ((outEntities == nullptr) || (outBodies == nullptr)) {
    return false;
  }

  const std::size_t bodyCount = m_rigidBodies.count();
  if ((startIndex > bodyCount) || (count > (bodyCount - startIndex))) {
    return false;
  }

  const Entity *entities = m_rigidBodies.entity_data();
  RigidBody *bodies = m_rigidBodies.component_data();
  if ((entities == nullptr) || (bodies == nullptr)) {
    return false;
  }

  *outEntities = entities + startIndex;
  *outBodies = bodies + startIndex;
  return true;
}

Entity World::find_rigid_body_owner(Entity colliderEntity,
                                    std::size_t stateIndex) const noexcept {
  if ((stateIndex >= kStateBufferCount) || !is_valid_entity(colliderEntity)) {
    return kInvalidEntity;
  }

  Entity current = colliderEntity;
  for (std::size_t depth = 0U; depth < kMaxEntities; ++depth) {
    if (m_rigidBodies.get_ptr(current) != nullptr) {
      return current;
    }

    const Transform *local = m_transforms.get_ptr(current, stateIndex);
    if ((local == nullptr) || (local->parentId == kInvalidPersistentId)) {
      return kInvalidEntity;
    }

    const std::uint32_t parentIndex = find_persistent_index(local->parentId);
    if ((parentIndex == 0U) || (parentIndex == current.index) ||
        !m_entityAlive[parentIndex]) {
      return kInvalidEntity;
    }
    current = Entity{parentIndex, m_entityGenerations[parentIndex]};
  }

  return kInvalidEntity;
}

Entity World::rigid_body_owner(Entity colliderEntity) const noexcept {
  return find_rigid_body_owner(colliderEntity, m_readStateIndex);
}

Entity
World::rigid_body_owner(Entity colliderEntity,
                        const SimulationAccessToken &token) const noexcept {
  if (!token.valid() || (m_phase != WorldPhase::Simulation)) {
    return kInvalidEntity;
  }
  return find_rigid_body_owner(colliderEntity, m_writeStateIndex);
}

bool World::add_collider(Entity entity, const Collider &collider) noexcept {
  if (!validate_collider_ingress(collider)) {
    core::log_message(
        core::LogLevel::Error, "world",
        "add_collider rejected non-finite or non-positive fields");
    return false;
  }

  if (!add_component_checked(m_colliders, entity, collider, "add_collider")) {
    return false;
  }

  install_provenance_hull(m_physicsContext, entity, collider);
  return true;
}

bool World::remove_collider(Entity entity) noexcept {
  if (!check_component_mutation(entity, "remove_collider")) {
    return false;
  }

  const bool removed = m_colliders.remove(entity);
  if (removed) {
    physics::remove_shape_payloads(m_physicsContext, entity);
  }
  return removed;
}

bool World::get_collider(Entity entity, Collider *outCollider) const noexcept {
  return get_component_checked(m_colliders, entity, outCollider,
                               "get_collider");
}

bool World::add_mesh_component(Entity entity,
                               const MeshComponent &component) noexcept {
  return add_component_checked(m_meshComponents, entity, component,
                               "add_mesh_component");
}

bool World::remove_mesh_component(Entity entity) noexcept {
  return remove_component_checked(m_meshComponents, entity,
                                  "remove_mesh_component");
}

bool World::get_mesh_component(Entity entity,
                               MeshComponent *outComponent) const noexcept {
  return get_component_checked(m_meshComponents, entity, outComponent,
                               "get_mesh_component");
}

MeshComponent *World::get_mesh_component_ptr(Entity entity) noexcept {
  return get_component_ptr_checked(m_meshComponents, entity);
}

const MeshComponent *
World::get_mesh_component_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_meshComponents, entity);
}

bool World::add_foliage_patch_component(
    Entity entity, const FoliagePatchComponent &component) noexcept {
  if (!check_component_mutation(entity, "add_foliage_patch_component")) {
    return false;
  }

  FoliagePatchComponent safe = component;
  if (safe.instanceCount >
      static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances)) {
    safe.instanceCount =
        static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances);
  }
  for (std::uint32_t i = 0U; i < safe.instanceCount; ++i) {
    if (safe.instances[i].lodIndex >=
        static_cast<std::uint32_t>(FoliagePatchComponent::kMaxLods)) {
      safe.instances[i].lodIndex = 0U;
    }
    if (safe.instances[i].scale <= 0.0F) {
      safe.instances[i].scale = 1.0F;
    }
  }

  return m_foliagePatches.add(entity, safe);
}

bool World::remove_foliage_patch_component(Entity entity) noexcept {
  return remove_component_checked(m_foliagePatches, entity,
                                  "remove_foliage_patch_component");
}

bool World::get_foliage_patch_component(
    Entity entity, FoliagePatchComponent *outComponent) const noexcept {
  return get_component_checked(m_foliagePatches, entity, outComponent,
                               "get_foliage_patch_component");
}

bool World::has_foliage_patch_component(Entity entity) const noexcept {
  return is_valid_entity(entity) && m_foliagePatches.contains(entity);
}

std::size_t World::foliage_patch_count() const noexcept {
  return m_foliagePatches.count();
}

const FoliagePatchComponent *
World::foliage_patch_at(std::size_t index) const noexcept {
  if (index >= m_foliagePatches.count()) {
    return nullptr;
  }
  return &m_foliagePatches.component_at(index);
}

Entity World::foliage_patch_entity_at(std::size_t index) const noexcept {
  if (index >= m_foliagePatches.count()) {
    return Entity{};
  }
  return m_foliagePatches.entity_at(index);
}

FoliagePatchComponent *
World::get_foliage_patch_component_ptr(Entity entity) noexcept {
  return get_component_ptr_checked(m_foliagePatches, entity);
}

const FoliagePatchComponent *
World::get_foliage_patch_component_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_foliagePatches, entity);
}

bool World::add_name_component(Entity entity,
                               const NameComponent &component) noexcept {
  if (!check_component_mutation(entity, "add_name_component")) {
    return false;
  }

  NameComponent safe{};
  core::copy_string(safe.name, sizeof(safe.name), component.name);

  // A re-add overwrites the previous name; drop its lookup entry first.
  NameComponent previous{};
  const bool hadName = m_nameComponents.get(entity, &previous);

  const bool ok = m_nameComponents.add(entity, safe);
  if (ok) {
    if (hadName && (previous.name[0] != '\0') &&
        (std::strcmp(previous.name, safe.name) != 0)) {
      name_lookup_erase(core::fnv1a_32(previous.name), entity.index);
    }
    if (safe.name[0] != '\0') {
      if (!name_lookup_insert(core::fnv1a_32(safe.name), entity.index)) {
        core::log_message(
            core::LogLevel::Warning, "world",
            "name lookup table overflow; name lookup may miss entries");
      }
    }
  }
  return ok;
}

bool World::remove_name_component(Entity entity) noexcept {
  if (!check_component_mutation(entity, "remove_name_component")) {
    return false;
  }

  NameComponent previous{};
  const bool hadName = m_nameComponents.get(entity, &previous);

  const bool ok = m_nameComponents.remove(entity);
  if (ok && hadName && (previous.name[0] != '\0')) {
    name_lookup_erase(core::fnv1a_32(previous.name), entity.index);
  }
  return ok;
}

bool World::get_name_component(Entity entity,
                               NameComponent *outComponent) const noexcept {
  return get_component_checked(m_nameComponents, entity, outComponent,
                               "get_name_component");
}

NameComponent *World::get_name_component_ptr(Entity entity) noexcept {
  return get_component_ptr_checked(m_nameComponents, entity);
}

const NameComponent *
World::get_name_component_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_nameComponents, entity);
}

Entity World::find_entity_by_name(const char *name) const noexcept {
  if ((name == nullptr) || (name[0] == '\0')) {
    return kInvalidEntity;
  }

  const std::uint32_t nameHash = core::fnv1a_32(name);
  std::size_t slot = static_cast<std::size_t>(nameHash) %
                     static_cast<std::size_t>(kNameLookupCapacity);
  for (std::size_t probe = 0U; probe < kNameLookupCapacity; ++probe) {
    if (m_nameLookupState[slot] == kNameSlotEmpty) {
      return kInvalidEntity;
    }

    if ((m_nameLookupState[slot] == kNameSlotOccupied) &&
        (m_nameLookupHashes[slot] == nameHash)) {
      const Entity candidate =
          find_entity_by_index(m_nameLookupEntityIndices[slot]);
      if (candidate != kInvalidEntity) {
        NameComponent component{};
        if (m_nameComponents.get(candidate, &component) &&
            (std::strcmp(component.name, name) == 0)) {
          return candidate;
        }
      }
    }

    slot = (slot + 1U) % kNameLookupCapacity;
  }

  return kInvalidEntity;
}

bool World::add_light_component(Entity entity,
                                const LightComponent &component) noexcept {
  return add_component_checked(m_lightComponents, entity, component,
                               "add_light_component");
}

bool World::remove_light_component(Entity entity) noexcept {
  return remove_component_checked(m_lightComponents, entity,
                                  "remove_light_component");
}

bool World::get_light_component(Entity entity,
                                LightComponent *outComponent) const noexcept {
  return get_component_checked(m_lightComponents, entity, outComponent,
                               "get_light_component");
}

bool World::has_light_component(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return false;
  }

  return m_lightComponents.contains(entity);
}

std::size_t World::light_count() const noexcept {
  return m_lightComponents.count();
}

const LightComponent *World::light_at(std::size_t index) const noexcept {
  if (index >= m_lightComponents.count()) {
    return nullptr;
  }
  return &m_lightComponents.component_at(index);
}

Entity World::light_entity_at(std::size_t index) const noexcept {
  if (index >= m_lightComponents.count()) {
    return kInvalidEntity;
  }
  return m_lightComponents.entity_at(index);
}

bool World::add_point_light_component(
    Entity entity, const PointLightComponent &component) noexcept {
  return add_component_checked(m_pointLights, entity, component,
                               "add_point_light_component");
}

bool World::remove_point_light_component(Entity entity) noexcept {
  return remove_component_checked(m_pointLights, entity,
                                  "remove_point_light_component");
}

bool World::get_point_light_component(
    Entity entity, PointLightComponent *outComponent) const noexcept {
  return get_component_checked(m_pointLights, entity, outComponent,
                               "get_point_light_component");
}

bool World::has_point_light_component(Entity entity) const noexcept {
  return is_valid_entity(entity) && m_pointLights.contains(entity);
}

std::size_t World::point_light_count() const noexcept {
  return m_pointLights.count();
}

const PointLightComponent *
World::point_light_at(std::size_t index) const noexcept {
  if (index >= m_pointLights.count()) {
    return nullptr;
  }
  return &m_pointLights.component_at(index);
}

Entity World::point_light_entity_at(std::size_t index) const noexcept {
  if (index >= m_pointLights.count()) {
    return Entity{};
  }
  return m_pointLights.entity_at(index);
}

bool World::add_spot_light_component(
    Entity entity, const SpotLightComponent &component) noexcept {
  return add_component_checked(m_spotLights, entity, component,
                               "add_spot_light_component");
}

bool World::remove_spot_light_component(Entity entity) noexcept {
  return remove_component_checked(m_spotLights, entity,
                                  "remove_spot_light_component");
}

bool World::get_spot_light_component(
    Entity entity, SpotLightComponent *outComponent) const noexcept {
  return get_component_checked(m_spotLights, entity, outComponent,
                               "get_spot_light_component");
}

bool World::has_spot_light_component(Entity entity) const noexcept {
  return is_valid_entity(entity) && m_spotLights.contains(entity);
}

std::size_t World::spot_light_count() const noexcept {
  return m_spotLights.count();
}

const SpotLightComponent *
World::spot_light_at(std::size_t index) const noexcept {
  if (index >= m_spotLights.count()) {
    return nullptr;
  }
  return &m_spotLights.component_at(index);
}

Entity World::spot_light_entity_at(std::size_t index) const noexcept {
  if (index >= m_spotLights.count()) {
    return Entity{};
  }
  return m_spotLights.entity_at(index);
}

bool World::add_reflection_probe_component(
    Entity entity, const ReflectionProbeComponent &component) noexcept {
  return add_component_checked(m_reflectionProbes, entity, component,
                               "add_reflection_probe_component");
}

bool World::remove_reflection_probe_component(Entity entity) noexcept {
  return remove_component_checked(m_reflectionProbes, entity,
                                  "remove_reflection_probe_component");
}

bool World::get_reflection_probe_component(
    Entity entity, ReflectionProbeComponent *outComponent) const noexcept {
  if ((outComponent == nullptr) || !is_valid_entity(entity)) {
    return false;
  }
  const ReflectionProbeComponent *ptr = m_reflectionProbes.get_ptr(entity);
  if (ptr == nullptr) {
    return false;
  }
  *outComponent = *ptr;
  return true;
}

bool World::has_reflection_probe_component(Entity entity) const noexcept {
  return is_valid_entity(entity) && m_reflectionProbes.contains(entity);
}

std::size_t World::reflection_probe_count() const noexcept {
  return m_reflectionProbes.count();
}

const ReflectionProbeComponent *
World::reflection_probe_at(std::size_t index) const noexcept {
  if (index >= m_reflectionProbes.count()) {
    return nullptr;
  }
  return &m_reflectionProbes.component_at(index);
}

Entity World::reflection_probe_entity_at(std::size_t index) const noexcept {
  if (index >= m_reflectionProbes.count()) {
    return Entity{};
  }
  return m_reflectionProbes.entity_at(index);
}

ReflectionProbeComponent *
World::get_reflection_probe_component_ptr(Entity entity) noexcept {
  return get_component_ptr_checked(m_reflectionProbes, entity);
}

const ReflectionProbeComponent *
World::get_reflection_probe_component_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_reflectionProbes, entity);
}

bool World::add_scene_capture_component(
    Entity entity, const SceneCaptureComponent &component) noexcept {
  return add_component_checked(m_sceneCaptures, entity, component,
                               "add_scene_capture_component");
}

bool World::remove_scene_capture_component(Entity entity) noexcept {
  return remove_component_checked(m_sceneCaptures, entity,
                                  "remove_scene_capture_component");
}

bool World::get_scene_capture_component(
    Entity entity, SceneCaptureComponent *outComponent) const noexcept {
  return get_component_checked(m_sceneCaptures, entity, outComponent,
                               "get_scene_capture_component");
}

bool World::has_scene_capture_component(Entity entity) const noexcept {
  return is_valid_entity(entity) && m_sceneCaptures.contains(entity);
}

std::size_t World::scene_capture_count() const noexcept {
  return m_sceneCaptures.count();
}

const SceneCaptureComponent *
World::scene_capture_at(std::size_t index) const noexcept {
  if (index >= m_sceneCaptures.count()) {
    return nullptr;
  }
  return &m_sceneCaptures.component_at(index);
}

Entity World::scene_capture_entity_at(std::size_t index) const noexcept {
  if (index >= m_sceneCaptures.count()) {
    return Entity{};
  }
  return m_sceneCaptures.entity_at(index);
}

std::int32_t
World::scene_capture_slot_for_entity(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return -1;
  }

  std::int32_t enabledSlot = 0;
  for (std::size_t i = 0U; i < m_sceneCaptures.count(); ++i) {
    const SceneCaptureComponent &capture = m_sceneCaptures.component_at(i);
    if (!capture.enabled) {
      continue;
    }
    if (m_sceneCaptures.entity_at(i) == entity) {
      return enabledSlot;
    }
    ++enabledSlot;
  }
  return -1;
}

SceneCaptureComponent *
World::get_scene_capture_component_ptr(Entity entity) noexcept {
  return get_component_ptr_checked(m_sceneCaptures, entity);
}

const SceneCaptureComponent *
World::get_scene_capture_component_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_sceneCaptures, entity);
}

bool World::add_script_component(Entity entity,
                                 const ScriptComponent &component) noexcept {
  if (!check_component_mutation(entity, "add_script_component")) {
    return false;
  }

  ScriptComponent safe{};
  core::copy_string(safe.scriptPath, sizeof(safe.scriptPath),
                    component.scriptPath);

  return m_scriptComponents.add(entity, safe);
}

bool World::remove_script_component(Entity entity) noexcept {
  return remove_component_checked(m_scriptComponents, entity,
                                  "remove_script_component");
}

bool World::get_script_component(Entity entity,
                                 ScriptComponent *outComponent) const noexcept {
  return get_component_checked(m_scriptComponents, entity, outComponent,
                               "get_script_component");
}

ScriptComponent *World::get_script_component_ptr(Entity entity) noexcept {
  return get_component_ptr_checked(m_scriptComponents, entity);
}

const ScriptComponent *
World::get_script_component_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_scriptComponents, entity);
}

bool World::add_animation_component(
    Entity entity, const AnimationComponent &component) noexcept {
  if (!check_component_mutation(entity, "add_animation_component")) {
    return false;
  }

  AnimationComponent safe = component;
  core::copy_string(safe.controllerPath, sizeof(safe.controllerPath),
                    component.controllerPath);

  return m_animationComponents.add(entity, safe);
}

bool World::remove_animation_component(Entity entity) noexcept {
  return remove_component_checked(m_animationComponents, entity,
                                  "remove_animation_component");
}

bool World::get_animation_component(
    Entity entity, AnimationComponent *outComponent) const noexcept {
  return get_component_checked(m_animationComponents, entity, outComponent,
                               "get_animation_component");
}

AnimationComponent *World::get_animation_component_ptr(
    Entity entity) noexcept {
  return get_component_ptr_checked(m_animationComponents, entity);
}

const AnimationComponent *
World::get_animation_component_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_animationComponents, entity);
}

bool World::add_spring_arm(Entity entity,
                           const SpringArmComponent &component) noexcept {
  return add_component_checked(m_springArms, entity, component,
                               "add_spring_arm");
}

bool World::remove_spring_arm(Entity entity) noexcept {
  return remove_component_checked(m_springArms, entity, "remove_spring_arm");
}

bool World::get_spring_arm(Entity entity,
                           SpringArmComponent *outComponent) const noexcept {
  return get_component_checked(m_springArms, entity, outComponent,
                               "get_spring_arm");
}

bool World::has_spring_arm(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return false;
  }
  return m_springArms.contains(entity);
}

SpringArmComponent *World::get_spring_arm_ptr(Entity entity) noexcept {
  return get_component_ptr_checked(m_springArms, entity);
}

const SpringArmComponent *
World::get_spring_arm_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_springArms, entity);
}

bool World::get_collider_range(std::size_t startIndex, std::size_t count,
                               const Entity **outEntities,
                               const Collider **outColliders) const noexcept {
  if ((outEntities == nullptr) || (outColliders == nullptr)) {
    return false;
  }

  const std::size_t colliderCount = m_colliders.count();
  if (startIndex > colliderCount) {
    return false;
  }

  const std::size_t remaining = colliderCount - startIndex;
  if (count > remaining) {
    return false;
  }

  const Entity *entities = m_colliders.entity_data();
  const Collider *colliders = m_colliders.component_data();
  if ((entities == nullptr) || (colliders == nullptr)) {
    return false;
  }

  *outEntities = entities + startIndex;
  *outColliders = colliders + startIndex;
  return true;
}

} // namespace engine::runtime
