// Implements world behavior for the Engine runtime world.

#include "engine/runtime/world.h"

#include "engine/core/hash.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/math/transform.h"
#include "engine/runtime/reflect_types.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace engine::runtime {

namespace {

constexpr std::uint8_t kNameSlotEmpty = 0U;
constexpr std::uint8_t kNameSlotOccupied = 1U;
constexpr std::uint8_t kNameSlotTombstone = 2U;

/// Handles world transform from local.
WorldTransform world_transform_from_local(const Transform &local) noexcept {
  WorldTransform world{};
  world.position = local.position;
  world.rotation = local.rotation;
  world.scale = local.scale;
  world.matrix = math::compose_trs(world.position, world.rotation, world.scale);
  return world;
}

/// Formats and logs one world component-API failure.
void log_component_error(const char *label, const char *reason) noexcept {
  char message[128] = {};
  std::snprintf(message, sizeof(message), "%s %s", label, reason);
  core::log_message(core::LogLevel::Error, "world", message);
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
bool World::get_component_checked(const Set &set, Entity entity,
                                  Component *out,
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
  ensure_runtime_reflection_registered();
  m_entityGenerations.fill(0U);
  m_entityPersistentIds.fill(kInvalidPersistentId);
  m_movementAuthorities.fill(MovementAuthority::None);
  m_persistentIndex.clear();
  m_entityAlive.fill(false);
  m_aliveEntityCount = 0U;
  m_transforms.clear();
  m_worldTransforms.clear();
  m_transformNodes.fill(TransformNode{});
  m_transformActiveIndices.fill(0U);
  m_transformActiveCount = 0U;
  m_transformRoots.fill(0U);
  m_transformQueueIndices.fill(0U);
  m_transformQueueInheritedDirty.fill(false);
  m_rigidBodies.clear();
  m_colliders.clear();
  m_meshComponents.clear();
  m_nameComponents.clear();
  m_nameLookupHashes.fill(0U);
  m_nameLookupEntityIndices.fill(0U);
  m_nameLookupState.fill(kNameSlotEmpty);
  m_scriptComponents.clear();
  m_reflectionProbes.clear();
  m_foliagePatches.clear();
}

Entity World::create_entity() noexcept {
  return create_entity_with_persistent_id(kInvalidPersistentId);
}

Entity
World::create_entity_with_persistent_id(PersistentId persistentId) noexcept {
  if (!is_mutation_phase()) {
    core::log_message(core::LogLevel::Error, "world",
                      "create_entity requires Input phase");
    return kInvalidEntity;
  }

  if ((persistentId != kInvalidPersistentId) &&
      (find_persistent_index(persistentId) != 0U)) {
    return kInvalidEntity;
  }

  std::uint32_t index = 0U;
  if (m_freeEntityCount > 0U) {
    --m_freeEntityCount;
    index = m_freeEntityIndices[m_freeEntityCount];
  } else {
    if (m_nextEntityIndex > static_cast<std::uint32_t>(kMaxEntities)) {
      return kInvalidEntity;
    }

    index = m_nextEntityIndex;
    ++m_nextEntityIndex;
  }

  if (m_entityGenerations[index] == 0U) {
    m_entityGenerations[index] = 1U;
  }

  if (persistentId == kInvalidPersistentId) {
    const std::uint32_t startCandidate = m_nextPersistentId;
    do {
      if (m_nextPersistentId == kInvalidPersistentId) {
        ++m_nextPersistentId;
      }

      if (find_persistent_index(m_nextPersistentId) == 0U) {
        persistentId = m_nextPersistentId;
        ++m_nextPersistentId;
        if (m_nextPersistentId == kInvalidPersistentId) {
          ++m_nextPersistentId;
        }
        break;
      }

      ++m_nextPersistentId;
      if (m_nextPersistentId == kInvalidPersistentId) {
        ++m_nextPersistentId;
      }
    } while (m_nextPersistentId != startCandidate);

    if (persistentId == kInvalidPersistentId) {
      return kInvalidEntity;
    }
  }

  m_entityAlive[index] = true;
  m_entityPersistentIds[index] = persistentId;
  m_movementAuthorities[index] = MovementAuthority::None;
  if (!insert_persistent_index(persistentId, index)) {
    m_entityAlive[index] = false;
    m_entityPersistentIds[index] = kInvalidPersistentId;
    m_movementAuthorities[index] = MovementAuthority::None;
    if (m_freeEntityCount < m_freeEntityIndices.size()) {
      m_freeEntityIndices[m_freeEntityCount] = index;
      ++m_freeEntityCount;
    }
    return kInvalidEntity;
  }
  ++m_aliveEntityCount;
  m_entityBeginPlayFired[index] = false;
  ++m_beginPlayPendingCount;
  return Entity{index, m_entityGenerations[index]};
}

bool World::destroy_entity_immediate(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return false;
  }

  NameComponent removedName{};
  const bool hadName = m_nameComponents.get(entity, &removedName);

  m_cameraManager.on_entity_destroyed(entity);

  physics::remove_shape_payloads(m_physicsContext, entity);
  static_cast<void>(m_transforms.remove(entity));
  static_cast<void>(m_worldTransforms.remove(entity));
  static_cast<void>(m_rigidBodies.remove(entity));
  static_cast<void>(m_colliders.remove(entity));
  static_cast<void>(m_meshComponents.remove(entity));
  static_cast<void>(m_nameComponents.remove(entity));
  static_cast<void>(m_lightComponents.remove(entity));
  static_cast<void>(m_pointLights.remove(entity));
  static_cast<void>(m_spotLights.remove(entity));
  static_cast<void>(m_reflectionProbes.remove(entity));
  static_cast<void>(m_foliagePatches.remove(entity));
  static_cast<void>(m_springArms.remove(entity));

  const std::uint32_t index = entity.index;
  erase_persistent_index(m_entityPersistentIds[index]);
  m_entityAlive[index] = false;
  if (!m_entityBeginPlayFired[index] && (m_beginPlayPendingCount > 0U)) {
    --m_beginPlayPendingCount;
  }
  m_entityBeginPlayFired[index] = false;
  m_entityPersistentIds[index] = kInvalidPersistentId;
  m_movementAuthorities[index] = MovementAuthority::None;
  reset_transform_cache(index);
  if (m_aliveEntityCount > 0U) {
    --m_aliveEntityCount;
  }

  ++m_entityGenerations[index];
  if (m_entityGenerations[index] == 0U) {
    m_entityGenerations[index] = 1U;
  }

  if (m_freeEntityCount < m_freeEntityIndices.size()) {
    m_freeEntityIndices[m_freeEntityCount] = index;
    ++m_freeEntityCount;
  }

  if (hadName && (removedName.name[0] != '\0')) {
    name_lookup_erase(core::fnv1a_32(removedName.name), index);
  }

  return true;
}

bool World::queue_deferred_destroy(Entity entity) noexcept {
  for (std::size_t i = 0U; i < m_pendingDestroyCount; ++i) {
    if (m_pendingDestroyEntities[i] == entity) {
      return true;
    }
  }

  if (m_pendingDestroyCount >= m_pendingDestroyEntities.size()) {
    return false;
  }

  m_pendingDestroyEntities[m_pendingDestroyCount] = entity;
  ++m_pendingDestroyCount;
  return true;
}

void World::flush_deferred_destroys() noexcept {
  if (m_pendingDestroyCount == 0U) {
    return;
  }

  for (std::size_t i = 0U; i < m_pendingDestroyCount; ++i) {
    const Entity entity = m_pendingDestroyEntities[i];
    if (is_valid_entity(entity)) {
      static_cast<void>(destroy_entity_immediate(entity));
    }
  }

  m_pendingDestroyCount = 0U;
}

bool World::destroy_entity(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    core::log_message(core::LogLevel::Error, "world",
                      "destroy_entity requires a live entity");
    return false;
  }

  // During Simulation, defer so EndPlay callbacks fire before removal.
  if (m_phase == WorldPhase::Simulation) {
    return queue_deferred_destroy(entity);
  }

  // Immediate destruction is allowed during Input, BeginPlay, and EndPlay.
  if (m_phase != WorldPhase::Input && m_phase != WorldPhase::BeginPlay &&
      m_phase != WorldPhase::EndPlay) {
    return false;
  }

  return destroy_entity_immediate(entity);
}

bool World::is_alive(Entity entity) const noexcept {
  return is_valid_entity(entity);
}

Entity World::find_entity_by_index(std::uint32_t index) const noexcept {
  if ((index == 0U) || (index > static_cast<std::uint32_t>(kMaxEntities))) {
    return kInvalidEntity;
  }

  if (!m_entityAlive[index]) {
    return kInvalidEntity;
  }

  return Entity{index, m_entityGenerations[index]};
}

Entity
World::find_entity_by_persistent_id(PersistentId persistentId) const noexcept {
  if (persistentId == kInvalidPersistentId) {
    return kInvalidEntity;
  }

  const std::uint32_t index = find_persistent_index(persistentId);
  if ((index == 0U) || (index > static_cast<std::uint32_t>(kMaxEntities))) {
    return kInvalidEntity;
  }

  if (!m_entityAlive[index] || (m_entityPersistentIds[index] != persistentId)) {
    return kInvalidEntity;
  }

  return Entity{index, m_entityGenerations[index]};
}

PersistentId World::persistent_id(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return kInvalidPersistentId;
  }

  return m_entityPersistentIds[entity.index];
}

std::size_t World::alive_entity_count() const noexcept {
  return m_aliveEntityCount;
}

bool World::add_transform(Entity entity, const Transform &transform) noexcept {
  if (!check_component_mutation(entity, "add_transform")) {
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

const Transform *World::get_transform_read_ptr(Entity entity) const noexcept {
  if (!is_valid_entity(entity)) {
    return nullptr;
  }

  return m_transforms.get_ptr(entity, m_readStateIndex);
}

World::SimulationAccessToken World::simulation_access_token() const noexcept {
  return make_token(m_phase == WorldPhase::Simulation);
}

Transform *
World::get_transform_write_ptr(Entity entity,
                               const SimulationAccessToken &token) noexcept {
  if (!token.valid() || !is_valid_entity(entity) ||
      (m_phase != WorldPhase::Simulation)) {
    return nullptr;
  }

  return m_transforms.get_ptr(entity, m_writeStateIndex);
}

const WorldTransform *
World::get_world_transform_read_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_worldTransforms, entity);
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
  return add_component_checked(m_rigidBodies, entity, rigidBody, "add_rigid_body");
}

bool World::remove_rigid_body(Entity entity) noexcept {
  return remove_component_checked(m_rigidBodies, entity, "remove_rigid_body");
}

bool World::get_rigid_body(Entity entity,
                           RigidBody *outRigidBody) const noexcept {
  return get_component_checked(m_rigidBodies, entity, outRigidBody, "get_rigid_body");
}

bool World::add_collider(Entity entity, const Collider &collider) noexcept {
  return add_component_checked(m_colliders, entity, collider, "add_collider");
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
  return get_component_checked(m_colliders, entity, outCollider, "get_collider");
}

bool World::add_mesh_component(Entity entity,
                               const MeshComponent &component) noexcept {
  return add_component_checked(m_meshComponents, entity, component, "add_mesh_component");
}

bool World::remove_mesh_component(Entity entity) noexcept {
  return remove_component_checked(m_meshComponents, entity, "remove_mesh_component");
}

bool World::get_mesh_component(Entity entity,
                               MeshComponent *outComponent) const noexcept {
  return get_component_checked(m_meshComponents, entity, outComponent, "get_mesh_component");
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
  return remove_component_checked(m_foliagePatches, entity, "remove_foliage_patch_component");
}

bool World::get_foliage_patch_component(
    Entity entity, FoliagePatchComponent *outComponent) const noexcept {
  return get_component_checked(m_foliagePatches, entity, outComponent, "get_foliage_patch_component");
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
  return get_component_checked(m_nameComponents, entity, outComponent, "get_name_component");
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
  return add_component_checked(m_lightComponents, entity, component, "add_light_component");
}

bool World::remove_light_component(Entity entity) noexcept {
  return remove_component_checked(m_lightComponents, entity, "remove_light_component");
}

bool World::get_light_component(Entity entity,
                                LightComponent *outComponent) const noexcept {
  return get_component_checked(m_lightComponents, entity, outComponent, "get_light_component");
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
  return add_component_checked(m_pointLights, entity, component, "add_point_light_component");
}

bool World::remove_point_light_component(Entity entity) noexcept {
  return remove_component_checked(m_pointLights, entity, "remove_point_light_component");
}

bool World::get_point_light_component(
    Entity entity, PointLightComponent *outComponent) const noexcept {
  return get_component_checked(m_pointLights, entity, outComponent, "get_point_light_component");
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
  return add_component_checked(m_spotLights, entity, component, "add_spot_light_component");
}

bool World::remove_spot_light_component(Entity entity) noexcept {
  return remove_component_checked(m_spotLights, entity, "remove_spot_light_component");
}

bool World::get_spot_light_component(
    Entity entity, SpotLightComponent *outComponent) const noexcept {
  return get_component_checked(m_spotLights, entity, outComponent, "get_spot_light_component");
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
  return add_component_checked(m_reflectionProbes, entity, component, "add_reflection_probe_component");
}

bool World::remove_reflection_probe_component(Entity entity) noexcept {
  return remove_component_checked(m_reflectionProbes, entity, "remove_reflection_probe_component");
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
  return remove_component_checked(m_scriptComponents, entity, "remove_script_component");
}

bool World::get_script_component(Entity entity,
                                 ScriptComponent *outComponent) const noexcept {
  return get_component_checked(m_scriptComponents, entity, outComponent, "get_script_component");
}

ScriptComponent *World::get_script_component_ptr(Entity entity) noexcept {
  return get_component_ptr_checked(m_scriptComponents, entity);
}

const ScriptComponent *
World::get_script_component_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_scriptComponents, entity);
}

bool World::add_spring_arm(Entity entity,
                           const SpringArmComponent &component) noexcept {
  return add_component_checked(m_springArms, entity, component, "add_spring_arm");
}

bool World::remove_spring_arm(Entity entity) noexcept {
  return remove_component_checked(m_springArms, entity, "remove_spring_arm");
}

bool World::get_spring_arm(Entity entity,
                           SpringArmComponent *outComponent) const noexcept {
  return get_component_checked(m_springArms, entity, outComponent, "get_spring_arm");
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

void World::begin_update_phase() noexcept {
  if (m_phase != WorldPhase::Input) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_update_phase requires Input phase");
    return;
  }

  m_writeStateIndex = (m_readStateIndex + 1U) % kStateBufferCount;
  m_updateSwapPending = true;
  m_phase = WorldPhase::Simulation;
}

void World::begin_update_step() noexcept {
  if (m_phase != WorldPhase::Simulation) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_update_step requires Simulation phase");
    return;
  }
  if (m_updateSwapPending) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_update_step called with pending update");
    return;
  }
  m_updateSwapPending = true;
}

void World::commit_update_phase() noexcept {
  if ((m_phase != WorldPhase::Simulation) || !m_updateSwapPending) {
    core::log_message(core::LogLevel::Error, "world",
                      "commit_update_phase requires active Simulation phase");
    return;
  }

  m_readStateIndex = m_writeStateIndex;
  m_writeStateIndex = (m_readStateIndex + 1U) % kStateBufferCount;
  m_updateSwapPending = false;
}

void World::begin_transform_phase() noexcept {
  if ((m_phase != WorldPhase::Simulation) && (m_phase != WorldPhase::Input)) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_transform_phase requires Input or Simulation");
    return;
  }

  if ((m_phase == WorldPhase::Simulation) && m_updateSwapPending) {
    m_readStateIndex = m_writeStateIndex;
    m_writeStateIndex = (m_readStateIndex + 1U) % kStateBufferCount;
    m_updateSwapPending = false;
  }

  m_phase = WorldPhase::TransformPropagation;
  if (!propagate_world_transforms()) {
    core::log_message(
        core::LogLevel::Warning, "runtime",
        "transform cycle detected; using deterministic root fallback");
  }
}

void World::begin_render_prep_phase() noexcept {
  if ((m_phase == WorldPhase::Simulation) || (m_phase == WorldPhase::Input)) {
    begin_transform_phase();
  }

  if (m_phase != WorldPhase::TransformPropagation) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_render_prep_phase requires transform propagation");
    return;
  }

  m_phase = WorldPhase::RenderSubmission;
}

void World::begin_render_phase() noexcept {
  if (m_phase != WorldPhase::RenderSubmission) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_render_phase requires RenderSubmission phase");
    return;
  }

  m_phase = WorldPhase::Render;
}

void World::end_frame_phase() noexcept {
  if ((m_phase != WorldPhase::Render) &&
      (m_phase != WorldPhase::RenderSubmission) &&
      (m_phase != WorldPhase::TransformPropagation) &&
      (m_phase != WorldPhase::Simulation) && (m_phase != WorldPhase::Input)) {
    core::log_message(core::LogLevel::Error, "world",
                      "end_frame_phase called from invalid phase");
    return;
  }

  flush_deferred_destroys();
  m_phase = WorldPhase::Input;
}

WorldPhase World::current_phase() const noexcept { return m_phase; }

void World::begin_begin_play_phase() noexcept {
  if (m_phase != WorldPhase::Input) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_begin_play_phase requires Input phase");
    return;
  }
  m_phase = WorldPhase::BeginPlay;
}

void World::end_begin_play_phase() noexcept {
  if (m_phase != WorldPhase::BeginPlay) {
    core::log_message(core::LogLevel::Error, "world",
                      "end_begin_play_phase requires BeginPlay phase");
    return;
  }
  m_phase = WorldPhase::Input;
}

void World::mark_begin_play_done(Entity entity) noexcept {
  if (!is_valid_entity(entity)) {
    return;
  }
  if (!m_entityBeginPlayFired[entity.index] &&
      (m_beginPlayPendingCount > 0U)) {
    --m_beginPlayPendingCount;
  }
  m_entityBeginPlayFired[entity.index] = true;
}

void World::begin_end_play_phase() noexcept {
  if ((m_phase != WorldPhase::Render) &&
      (m_phase != WorldPhase::RenderSubmission) &&
      (m_phase != WorldPhase::TransformPropagation) &&
      (m_phase != WorldPhase::Simulation) && (m_phase != WorldPhase::Input)) {
    core::log_message(core::LogLevel::Error, "world",
                      "begin_end_play_phase called from invalid phase");
    return;
  }
  m_phase = WorldPhase::EndPlay;
}

void World::end_end_play_phase() noexcept {
  if (m_phase != WorldPhase::EndPlay) {
    core::log_message(core::LogLevel::Error, "world",
                      "end_end_play_phase requires EndPlay phase");
    return;
  }
  flush_deferred_destroys();
  m_phase = WorldPhase::Input;
}

void World::for_each_transform(TransformVisitor visitor,
                               void *userData) const noexcept {
  if (visitor == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < m_transforms.count(); ++i) {
    visitor(m_transforms.entity_at(i),
            m_transforms.component_at(i, m_readStateIndex), userData);
  }
}

bool World::update_transforms(float deltaSeconds) noexcept {
  return update_transforms_range(0U, m_transforms.count(), deltaSeconds);
}

bool World::update_transforms_range(std::size_t startIndex, std::size_t count,
                                    float deltaSeconds) noexcept {
  static_cast<void>(deltaSeconds);

  // Same contract as the other range APIs: an out-of-bounds range is an
  // error, not a request to clamp.
  const Entity *entities = nullptr;
  const Transform *readState = nullptr;
  Transform *writeState = nullptr;
  if (!get_transform_update_range(startIndex, count, &entities, &readState,
                                  &writeState)) {
    return false;
  }

  for (std::size_t i = 0U; i < count; ++i) {
    writeState[i] = readState[i];
  }

  return true;
}

bool World::get_transform_update_range(
    std::size_t startIndex, std::size_t count, const Entity **outEntities,
    const Transform **outReadTransforms,
    Transform **outWriteTransforms) noexcept {
  if ((outEntities == nullptr) || (outReadTransforms == nullptr) ||
      (outWriteTransforms == nullptr)) {
    return false;
  }

  if (m_phase != WorldPhase::Simulation) {
    return false;
  }

  const std::size_t transformCount = m_transforms.count();
  if (startIndex > transformCount) {
    return false;
  }

  const std::size_t remaining = transformCount - startIndex;
  if (count > remaining) {
    return false;
  }

  const Entity *entities = m_transforms.entity_data();
  const Transform *readState = m_transforms.component_data(m_readStateIndex);
  Transform *writeState = m_transforms.component_data(m_writeStateIndex);
  if ((entities == nullptr) || (readState == nullptr) ||
      (writeState == nullptr)) {
    return false;
  }

  *outEntities = entities + startIndex;
  *outReadTransforms = readState + startIndex;
  *outWriteTransforms = writeState + startIndex;
  return true;
}

bool World::read_transform_range(
    std::size_t startIndex, std::size_t count, const Entity **outEntities,
    const Transform **outTransforms) const noexcept {
  if ((outEntities == nullptr) || (outTransforms == nullptr)) {
    return false;
  }

  if ((m_phase != WorldPhase::RenderSubmission) &&
      (m_phase != WorldPhase::Render)) {
    return false;
  }

  const std::size_t transformCount = m_transforms.count();
  if (startIndex > transformCount) {
    return false;
  }

  const std::size_t remaining = transformCount - startIndex;
  if (count > remaining) {
    return false;
  }

  const Entity *entities = m_transforms.entity_data();
  const Transform *transforms = m_transforms.component_data(m_readStateIndex);
  if ((entities == nullptr) || (transforms == nullptr)) {
    return false;
  }

  *outEntities = entities + startIndex;
  *outTransforms = transforms + startIndex;
  return true;
}

bool World::read_world_transform_range(
    std::size_t startIndex, std::size_t count, const Entity **outEntities,
    const WorldTransform **outTransforms) const noexcept {
  if ((outEntities == nullptr) || (outTransforms == nullptr)) {
    return false;
  }

  if ((m_phase != WorldPhase::RenderSubmission) &&
      (m_phase != WorldPhase::Render)) {
    return false;
  }

  const std::size_t transformCount = m_worldTransforms.count();
  if (startIndex > transformCount) {
    return false;
  }

  const std::size_t remaining = transformCount - startIndex;
  if (count > remaining) {
    return false;
  }

  const Entity *entities = m_worldTransforms.entity_data();
  const WorldTransform *transforms = m_worldTransforms.component_data();
  if ((entities == nullptr) || (transforms == nullptr)) {
    return false;
  }

  *outEntities = entities + startIndex;
  *outTransforms = transforms + startIndex;
  return true;
}

std::size_t World::transform_count() const noexcept {
  return m_transforms.count();
}

std::size_t World::world_transform_count() const noexcept {
  return m_worldTransforms.count();
}

std::size_t World::rigid_body_count() const noexcept {
  return m_rigidBodies.count();
}

std::size_t World::collider_count() const noexcept {
  return m_colliders.count();
}

physics::PhysicsContext &World::physics_context() noexcept {
  return m_physicsContext;
}

const physics::PhysicsContext &World::physics_context() const noexcept {
  return m_physicsContext;
}

RigidBody *World::get_rigid_body_ptr(Entity entity) noexcept {
  return get_component_ptr_checked(m_rigidBodies, entity);
}

const RigidBody *World::get_rigid_body_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_rigidBodies, entity);
}

const Collider *World::get_collider_ptr(Entity entity) const noexcept {
  return get_component_ptr_checked(m_colliders, entity);
}

Collider *World::get_collider_ptr(Entity entity) noexcept {
  return get_component_ptr_checked(m_colliders, entity);
}

std::size_t World::query_state_index() const noexcept {
  return m_readStateIndex;
}

bool World::name_lookup_insert(std::uint32_t nameHash,
                               std::uint32_t entityIndex) noexcept {
  if (entityIndex == 0U) {
    return false;
  }

  std::size_t slot = static_cast<std::size_t>(nameHash) %
                     static_cast<std::size_t>(kNameLookupCapacity);
  std::size_t tombstone = kNameLookupCapacity;
  for (std::size_t probe = 0U; probe < kNameLookupCapacity; ++probe) {
    if (m_nameLookupState[slot] == kNameSlotEmpty) {
      const std::size_t writeSlot =
          (tombstone != kNameLookupCapacity) ? tombstone : slot;
      if (writeSlot == tombstone) {
        --m_nameLookupTombstones;
      }
      m_nameLookupState[writeSlot] = kNameSlotOccupied;
      m_nameLookupHashes[writeSlot] = nameHash;
      m_nameLookupEntityIndices[writeSlot] = entityIndex;
      return true;
    }

    if (m_nameLookupState[slot] == kNameSlotTombstone) {
      if (tombstone == kNameLookupCapacity) {
        tombstone = slot;
      }
    } else if ((m_nameLookupHashes[slot] == nameHash) &&
               (m_nameLookupEntityIndices[slot] == entityIndex)) {
      // Same entity re-registered under the same hash; keep its slot.
      return true;
    }
    // Entities sharing a name each keep their own slot so erasing one does
    // not orphan the others; lookups skip entries whose entity died.

    slot = (slot + 1U) % kNameLookupCapacity;
  }

  return false;
}

void World::name_lookup_erase(std::uint32_t nameHash,
                              std::uint32_t entityIndex) noexcept {
  std::size_t slot = static_cast<std::size_t>(nameHash) %
                     static_cast<std::size_t>(kNameLookupCapacity);
  for (std::size_t probe = 0U; probe < kNameLookupCapacity; ++probe) {
    if (m_nameLookupState[slot] == kNameSlotEmpty) {
      return;
    }

    if ((m_nameLookupState[slot] == kNameSlotOccupied) &&
        (m_nameLookupHashes[slot] == nameHash) &&
        (m_nameLookupEntityIndices[slot] == entityIndex)) {
      m_nameLookupState[slot] = kNameSlotTombstone;
      m_nameLookupHashes[slot] = 0U;
      m_nameLookupEntityIndices[slot] = 0U;
      ++m_nameLookupTombstones;

      // Rebuild once tombstones dominate so misses stay cheap: a probe stops
      // at the first empty slot, and churn erodes empty slots over time.
      if (m_nameLookupTombstones > (kNameLookupCapacity / 4U)) {
        rebuild_name_lookup();
      }
      return;
    }

    slot = (slot + 1U) % kNameLookupCapacity;
  }
}

void World::rebuild_name_lookup() noexcept {
  m_nameLookupHashes.fill(0U);
  m_nameLookupEntityIndices.fill(0U);
  m_nameLookupState.fill(kNameSlotEmpty);
  m_nameLookupTombstones = 0U;

  const std::size_t count = m_nameComponents.count();
  for (std::size_t i = 0U; i < count; ++i) {
    const Entity entity = m_nameComponents.entity_at(i);
    if (!is_valid_entity(entity)) {
      continue;
    }

    const NameComponent &nameComponent = m_nameComponents.component_at(i);
    if (nameComponent.name[0] == '\0') {
      continue;
    }

    const std::uint32_t hash = core::fnv1a_32(nameComponent.name);
    if (!name_lookup_insert(hash, entity.index)) {
      core::log_message(
          core::LogLevel::Warning, "world",
          "name lookup table overflow; name lookup may miss entries");
      return;
    }
  }
}

bool World::is_mutation_phase() const noexcept {
  return (m_phase == WorldPhase::Input);
}

bool World::is_valid_entity(Entity entity) const noexcept {
  if ((entity.index == 0U) ||
      (entity.index > static_cast<std::uint32_t>(kMaxEntities))) {
    return false;
  }

  if (!m_entityAlive[entity.index]) {
    return false;
  }

  return m_entityGenerations[entity.index] == entity.generation;
}

bool World::insert_persistent_index(PersistentId persistentId,
                                    std::uint32_t entityIndex) noexcept {
  if ((persistentId == kInvalidPersistentId) || (entityIndex == 0U) ||
      (entityIndex > static_cast<std::uint32_t>(kMaxEntities))) {
    return false;
  }

  return m_persistentIndex.insert(persistentId, entityIndex);
}

std::uint32_t
World::find_persistent_index(PersistentId persistentId) const noexcept {
  if (persistentId == kInvalidPersistentId) {
    return 0U;
  }

  const std::uint32_t *entityIndex = m_persistentIndex.find(persistentId);
  return (entityIndex != nullptr) ? *entityIndex : 0U;
}

void World::erase_persistent_index(PersistentId persistentId) noexcept {
  if (persistentId == kInvalidPersistentId) {
    return;
  }

  static_cast<void>(m_persistentIndex.erase(persistentId));

  // Entity churn accumulates tombstones; rebuild from the alive arrays once
  // they dominate so lookup misses stay cheap.
  if (m_persistentIndex.tombstone_count() > (kPersistentIndexCapacity / 4U)) {
    rebuild_persistent_index();
  }
}

void World::rebuild_persistent_index() noexcept {
  m_persistentIndex.clear();

  std::size_t visited = 0U;
  for (std::uint32_t index = 1U;
       (index < m_nextEntityIndex) && (visited < m_aliveEntityCount);
       ++index) {
    if (!m_entityAlive[index]) {
      continue;
    }
    ++visited;
    if (m_entityPersistentIds[index] != kInvalidPersistentId) {
      static_cast<void>(
          m_persistentIndex.insert(m_entityPersistentIds[index], index));
    }
  }
}

void World::reset_transform_cache(std::uint32_t entityIndex) noexcept {
  if ((entityIndex == 0U) ||
      (entityIndex > static_cast<std::uint32_t>(kMaxEntities))) {
    return;
  }

  m_transformNodes[entityIndex] = TransformNode{};
}

bool World::propagate_world_transforms() noexcept {
  const std::size_t previousActiveCount = m_transformActiveCount;
  for (std::size_t i = 0U; i < previousActiveCount; ++i) {
    const std::uint32_t index = m_transformActiveIndices[i];
    m_transformQueueIndices[i] = index;

    TransformNode &node = m_transformNodes[index];
    node.parentIndex = 0U;
    node.firstChild = 0U;
    node.lastChild = 0U;
    node.nextSibling = 0U;
    node.traversalState = 0U;
    node.present = false;
    node.localDirty = false;
  }

  m_transformActiveCount = 0U;

  const std::size_t transformCount = m_transforms.count();
  for (std::size_t denseIndex = 0U; denseIndex < transformCount; ++denseIndex) {
    const Entity entity = m_transforms.entity_at(denseIndex);
    if (!is_valid_entity(entity)) {
      continue;
    }

    if (m_transformActiveCount >= m_transformActiveIndices.size()) {
      return false;
    }

    const std::uint32_t index = entity.index;
    m_transformActiveIndices[m_transformActiveCount] = index;
    ++m_transformActiveCount;

    TransformNode &node = m_transformNodes[index];
    node.parentIndex = 0U;
    node.firstChild = 0U;
    node.lastChild = 0U;
    node.nextSibling = 0U;
    node.traversalState = 0U;
    node.present = true;
    node.localDirty = false;

    const Transform &local =
        m_transforms.component_at(denseIndex, m_readStateIndex);

    std::uint32_t parentIndex = 0U;
    if (local.parentId != kInvalidPersistentId) {
      const std::uint32_t resolvedParentIndex =
          find_persistent_index(local.parentId);
      if ((resolvedParentIndex != 0U) && (resolvedParentIndex != index) &&
          m_entityAlive[resolvedParentIndex]) {
        const Entity parentEntity{resolvedParentIndex,
                                  m_entityGenerations[resolvedParentIndex]};
        if (m_transforms.get_ptr(parentEntity, m_readStateIndex) != nullptr) {
          parentIndex = resolvedParentIndex;
        }
      }
    }

    node.parentIndex = parentIndex;

    const bool cacheValid = node.cacheValid;
    const bool localChanged =
        !cacheValid || (local.position.x != node.position.x) ||
        (local.position.y != node.position.y) ||
        (local.position.z != node.position.z) ||
        (local.rotation.x != node.rotation.x) ||
        (local.rotation.y != node.rotation.y) ||
        (local.rotation.z != node.rotation.z) ||
        (local.rotation.w != node.rotation.w) ||
        (local.scale.x != node.scale.x) || (local.scale.y != node.scale.y) ||
        (local.scale.z != node.scale.z);
    const bool parentChanged = !cacheValid ||
                               (node.cachedParentId != local.parentId) ||
                               (node.cachedParentIndex != parentIndex);
    node.localDirty = localChanged || parentChanged;

    node.position = local.position;
    node.rotation = local.rotation;
    node.scale = local.scale;
    node.cachedParentId = local.parentId;
    node.cachedParentIndex = parentIndex;
    node.cacheValid = true;
  }

  // Invalidate cache only for entities that had transforms previously but no
  // longer do, instead of scanning the full entity ID range.
  for (std::size_t i = 0U; i < previousActiveCount; ++i) {
    const std::uint32_t index = m_transformQueueIndices[i];
    if (!m_transformNodes[index].present) {
      reset_transform_cache(index);
    }
  }

  std::size_t rootCount = 0U;
  for (std::size_t i = 0U; i < m_transformActiveCount; ++i) {
    const std::uint32_t index = m_transformActiveIndices[i];
    if (!m_transformNodes[index].present) {
      continue;
    }

    const std::uint32_t parentIndex = m_transformNodes[index].parentIndex;
    if (parentIndex == 0U) {
      if (rootCount >= m_transformRoots.size()) {
        return false;
      }
      m_transformRoots[rootCount] = index;
      ++rootCount;
      continue;
    }

    if (m_transformNodes[parentIndex].firstChild == 0U) {
      m_transformNodes[parentIndex].firstChild = index;
      m_transformNodes[parentIndex].lastChild = index;
      continue;
    }

    const std::uint32_t lastChild = m_transformNodes[parentIndex].lastChild;
    m_transformNodes[lastChild].nextSibling = index;
    m_transformNodes[parentIndex].lastChild = index;
  }

  auto enqueue_node = [this](std::uint32_t entityIndex, bool inheritedDirty,
                             std::size_t *ioQueueTail) noexcept {
    if ((entityIndex == 0U) || (ioQueueTail == nullptr) ||
        (*ioQueueTail >= m_transformQueueIndices.size())) {
      return false;
    }

    if (m_transformNodes[entityIndex].traversalState != 0U) {
      return true;
    }

    m_transformQueueIndices[*ioQueueTail] = entityIndex;
    m_transformQueueInheritedDirty[*ioQueueTail] = inheritedDirty;
    m_transformNodes[entityIndex].traversalState = 1U;
    ++(*ioQueueTail);
    return true;
  };

  std::size_t queueHead = 0U;
  std::size_t queueTail = 0U;
  for (std::size_t i = 0U; i < rootCount; ++i) {
    if (!enqueue_node(m_transformRoots[i], false, &queueTail)) {
      return false;
    }
  }

  auto drain_queue = [this, &queueHead, &queueTail, &enqueue_node]() noexcept {
    while (queueHead < queueTail) {
      const std::uint32_t entityIndex = m_transformQueueIndices[queueHead];
      const bool inheritedDirty = m_transformQueueInheritedDirty[queueHead];
      ++queueHead;

      m_transformNodes[entityIndex].traversalState = 2U;

      const Entity entity{entityIndex, m_entityGenerations[entityIndex]};
      const Transform *local = m_transforms.get_ptr(entity, m_readStateIndex);
      if (local == nullptr) {
        continue;
      }

      const bool hasWorldTransform = m_worldTransforms.contains(entity);
      const bool worldDirty = inheritedDirty ||
                              m_transformNodes[entityIndex].localDirty ||
                              !hasWorldTransform;
      if (worldDirty) {
        WorldTransform world = world_transform_from_local(*local);
        const std::uint32_t parentIndex =
            m_transformNodes[entityIndex].parentIndex;
        if (parentIndex != 0U) {
          const Entity parent{parentIndex, m_entityGenerations[parentIndex]};
          const WorldTransform *parentWorld = m_worldTransforms.get_ptr(parent);
          if (parentWorld != nullptr) {
            const math::Mat4 localMatrix = math::compose_trs(
                local->position, local->rotation, local->scale);
            world.matrix = math::mul(parentWorld->matrix, localMatrix);
            world.position =
                math::Vec3(world.matrix.columns[3].x, world.matrix.columns[3].y,
                           world.matrix.columns[3].z);
            world.rotation = math::normalize(
                math::mul(parentWorld->rotation, local->rotation));
            world.scale = math::Vec3(parentWorld->scale.x * local->scale.x,
                                     parentWorld->scale.y * local->scale.y,
                                     parentWorld->scale.z * local->scale.z);
          }
        }

        static_cast<void>(m_worldTransforms.add(entity, world));
      }

      for (std::uint32_t child = m_transformNodes[entityIndex].firstChild;
           child != 0U; child = m_transformNodes[child].nextSibling) {
        if (!enqueue_node(child, worldDirty, &queueTail)) {
          return false;
        }
      }
    }

    return true;
  };

  if (!drain_queue()) {
    return false;
  }

  bool fullyAcyclic = true;
  for (std::size_t i = 0U; i < m_transformActiveCount; ++i) {
    const std::uint32_t index = m_transformActiveIndices[i];
    if (!m_transformNodes[index].present ||
        (m_transformNodes[index].traversalState != 0U)) {
      continue;
    }

    fullyAcyclic = false;
    m_transformNodes[index].parentIndex = 0U;
    if (!enqueue_node(index, false, &queueTail)) {
      return false;
    }
  }

  if (!drain_queue()) {
    return false;
  }

  return fullyAcyclic;
}

} // namespace engine::runtime
