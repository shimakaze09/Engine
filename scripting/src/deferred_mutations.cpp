// Owns deferred world-mutation queueing for the scripting module.

#include "deferred_mutations.h"

#include "engine/core/logging.h"
#include "engine/scripting/runtime_services.h"
#include "engine/scripting/scripting.h"
#include "entity_script_bindings.h"
#include "reload_transaction.h"
#include "runtime_binding.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace engine::scripting {
namespace {

/// Enumerates deferred mutation type values used by the engine.
enum class DeferredMutationType : std::uint8_t {
  DestroyEntity,
  SetTransform,
  AddRigidBody,
  AddCollider,
  AddMeshComponent,
  AddNameComponent,
  AddLightComponent,
  RemoveLightComponent,
  AddScriptComponent,
  RemoveScriptComponent,
  AddPointLightComponent,
  RemovePointLightComponent,
  AddSpotLightComponent,
  RemoveSpotLightComponent,
  AddSpringArm,
  AddCameraComponent,
  RemoveCameraComponent,
};

struct DeferredMutation final {
  DeferredMutationType type = DeferredMutationType::DestroyEntity;
  runtime::Entity entity{};
  runtime::Transform transform{};
  runtime::RigidBody rigidBody{};
  runtime::Collider collider{};
  runtime::MeshComponent meshComponent{};
  runtime::NameComponent nameComponent{};
  runtime::LightComponent lightComponent{};
  runtime::ScriptComponent scriptComponent{};
  runtime::PointLightComponent pointLightComponent{};
  runtime::SpotLightComponent spotLightComponent{};
  math::SpringArmComponent springArm{};
  math::CameraComponent cameraComponent{};
  runtime::MovementAuthority movementAuthority =
      runtime::MovementAuthority::None;
  bool setMovementAuthority = false;
  // World content epoch at queue time. A scene replacement assigns a fresh
  // World over the live one and restarts entity generations, so the new
  // scene's entity at the same index carries the same {index, generation}
  // and is_alive alone cannot tell it from the queued target; the epoch
  // is the identity that changes.
  std::uint32_t contentEpoch = 0U;
};

constexpr std::size_t kMaxDeferredMutations = 2048U;
DeferredMutation g_deferredMutations[kMaxDeferredMutations]{};
std::size_t g_deferredMutationCount = 0U;
std::mutex g_deferredMutationMutex{};

/// Content epoch of the bound World, or 0 when no World is bound.
std::uint32_t bound_world_content_epoch() noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  return runtime_bound() ? binding.services->content_epoch(binding.world) : 0U;
}

/// Queues one deferred mutation for the next safe flush point, stamped
/// with the bound World's current content epoch. The queue state is
/// mutex-guarded; flush releases the lock around each apply so on_end_play
/// callbacks queueing further mutations cannot deadlock.
bool queue_deferred_mutation(const DeferredMutation &mutation) noexcept {
  const std::uint32_t contentEpoch = bound_world_content_epoch();
  std::lock_guard<std::mutex> lock(g_deferredMutationMutex);
  if (g_deferredMutationCount >= kMaxDeferredMutations) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "deferred mutation queue overflow");
    return false;
  }

  g_deferredMutations[g_deferredMutationCount] = mutation;
  g_deferredMutations[g_deferredMutationCount].contentEpoch = contentEpoch;
  ++g_deferredMutationCount;
  return true;
}

/// Returns whether a deferred mutation still targets the same live entity.
bool is_deferred_entity_current(runtime::World *world,
                                runtime::Entity entity) noexcept {
  return (world != nullptr) && runtime_bound() &&
         runtime_binding().services->is_alive(world, entity);
}

/// Pending-read outcome: nothing queued, a queued snapshot, or a queued
/// removal/destruction that invalidates the component.
enum class PendingRead : std::uint8_t { None, Value, Removed };

/// Finds the newest queued mutation affecting entity's component of the
/// given snapshot type; a queued destroy (or matching remove) wins over
/// older snapshots so setters cannot resurrect the component.
/// Entries queued against an earlier content epoch belong to a replaced
/// scene and are invisible here, exactly as the flush will drop them.
PendingRead find_pending_snapshot(runtime::Entity entity,
                                  DeferredMutationType snapshotType,
                                  DeferredMutationType removeType,
                                  bool hasRemoveType,
                                  DeferredMutation *out) noexcept {
  const std::uint32_t liveEpoch = bound_world_content_epoch();
  std::lock_guard<std::mutex> lock(g_deferredMutationMutex);
  for (std::size_t i = g_deferredMutationCount; i > 0U; --i) {
    const DeferredMutation &mutation = g_deferredMutations[i - 1U];
    if (!(mutation.entity == entity) || (mutation.contentEpoch != liveEpoch)) {
      continue;
    }
    if ((mutation.type == DeferredMutationType::DestroyEntity) ||
        (hasRemoveType && (mutation.type == removeType))) {
      return PendingRead::Removed;
    }
    if (mutation.type == snapshotType) {
      *out = mutation;
      return PendingRead::Value;
    }
  }
  return PendingRead::None;
}

} // namespace

/// Reads the entity's transform through any pending queued write.
bool latest_transform(runtime::Entity entity,
                      runtime::Transform *outTransform) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (!runtime_bound() || (outTransform == nullptr)) {
    return false;
  }
  DeferredMutation pending{};
  switch (find_pending_snapshot(entity, DeferredMutationType::SetTransform,
                                DeferredMutationType::SetTransform, false,
                                &pending)) {
  case PendingRead::Value:
    *outTransform = pending.transform;
    return true;
  case PendingRead::Removed:
    return false;
  case PendingRead::None:
    break;
  }
  return binding.services->get_transform_op(binding.world, entity, outTransform);
}

/// Reads the entity's rigid body through any pending queued write.
bool latest_rigid_body(runtime::Entity entity,
                       runtime::RigidBody *outRigidBody) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (!runtime_bound() || (outRigidBody == nullptr)) {
    return false;
  }
  DeferredMutation pending{};
  switch (find_pending_snapshot(entity, DeferredMutationType::AddRigidBody,
                                DeferredMutationType::AddRigidBody, false,
                                &pending)) {
  case PendingRead::Value:
    *outRigidBody = pending.rigidBody;
    return true;
  case PendingRead::Removed:
    return false;
  case PendingRead::None:
    break;
  }
  return binding.services->get_rigid_body_op(binding.world, entity, outRigidBody);
}

/// Reads the entity's collider through any pending queued write.
bool latest_collider(runtime::Entity entity,
                     runtime::Collider *outCollider) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (!runtime_bound() || (outCollider == nullptr)) {
    return false;
  }
  DeferredMutation pending{};
  switch (find_pending_snapshot(entity, DeferredMutationType::AddCollider,
                                DeferredMutationType::AddCollider, false,
                                &pending)) {
  case PendingRead::Value:
    *outCollider = pending.collider;
    return true;
  case PendingRead::Removed:
    return false;
  case PendingRead::None:
    break;
  }
  return binding.services->get_collider_op(binding.world, entity, outCollider);
}

/// Reads the entity's mesh component through any pending queued write.
bool latest_mesh_component(runtime::Entity entity,
                           runtime::MeshComponent *outComponent) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (!runtime_bound() || (outComponent == nullptr)) {
    return false;
  }
  DeferredMutation pending{};
  switch (find_pending_snapshot(entity,
                                DeferredMutationType::AddMeshComponent,
                                DeferredMutationType::AddMeshComponent, false,
                                &pending)) {
  case PendingRead::Value:
    *outComponent = pending.meshComponent;
    return true;
  case PendingRead::Removed:
    return false;
  case PendingRead::None:
    break;
  }
  return binding.services->get_mesh_component_op(binding.world, entity, outComponent);
}

/// Reads the entity's light component through any pending queued write;
/// a queued removal reports the component as absent.
bool latest_light_component(runtime::Entity entity,
                            runtime::LightComponent *outComponent) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (!runtime_bound() || (outComponent == nullptr)) {
    return false;
  }
  DeferredMutation pending{};
  switch (find_pending_snapshot(entity,
                                DeferredMutationType::AddLightComponent,
                                DeferredMutationType::RemoveLightComponent,
                                true, &pending)) {
  case PendingRead::Value:
    *outComponent = pending.lightComponent;
    return true;
  case PendingRead::Removed:
    return false;
  case PendingRead::None:
    break;
  }
  return binding.services->get_light_component_op(binding.world, entity, outComponent);
}

bool latest_point_light_component(
    runtime::Entity entity,
    runtime::PointLightComponent *outComponent) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (!runtime_bound() || (outComponent == nullptr)) {
    return false;
  }
  DeferredMutation pending{};
  switch (find_pending_snapshot(entity,
                                DeferredMutationType::AddPointLightComponent,
                                DeferredMutationType::RemovePointLightComponent,
                                true, &pending)) {
  case PendingRead::Value:
    *outComponent = pending.pointLightComponent;
    return true;
  case PendingRead::Removed:
    return false;
  case PendingRead::None:
    break;
  }
  return binding.services->get_point_light_component_op(binding.world, entity, outComponent);
}

bool latest_spot_light_component(
    runtime::Entity entity,
    runtime::SpotLightComponent *outComponent) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (!runtime_bound() || (outComponent == nullptr)) {
    return false;
  }
  DeferredMutation pending{};
  switch (find_pending_snapshot(entity,
                                DeferredMutationType::AddSpotLightComponent,
                                DeferredMutationType::RemoveSpotLightComponent,
                                true, &pending)) {
  case PendingRead::Value:
    *outComponent = pending.spotLightComponent;
    return true;
  case PendingRead::Removed:
    return false;
  case PendingRead::None:
    break;
  }
  return binding.services->get_spot_light_component_op(binding.world, entity, outComponent);
}

bool latest_spring_arm(runtime::Entity entity,
                       math::SpringArmComponent *outComponent) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (!runtime_bound() || (outComponent == nullptr)) {
    return false;
  }
  DeferredMutation pending{};
  switch (find_pending_snapshot(entity, DeferredMutationType::AddSpringArm,
                                DeferredMutationType::AddSpringArm, false,
                                &pending)) {
  case PendingRead::Value:
    *outComponent = pending.springArm;
    return true;
  case PendingRead::Removed:
    return false;
  case PendingRead::None:
    break;
  }
  return binding.services->get_spring_arm_op(binding.world, entity,
                                             outComponent);
}

bool latest_camera_component(runtime::Entity entity,
                             math::CameraComponent *outComponent) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (!runtime_bound() || (outComponent == nullptr)) {
    return false;
  }
  DeferredMutation pending{};
  switch (find_pending_snapshot(entity,
                                DeferredMutationType::AddCameraComponent,
                                DeferredMutationType::RemoveCameraComponent,
                                true, &pending)) {
  case PendingRead::Value:
    *outComponent = pending.cameraComponent;
    return true;
  case PendingRead::Removed:
    return false;
  case PendingRead::None:
    break;
  }
  return binding.services->get_camera_component_op(binding.world, entity,
                                                   outComponent);
}

bool can_create_entities_now() noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  return runtime_bound() && !in_end_play_dispatch() &&
         binding.services->is_input_phase(binding.world);
}

bool can_apply_mutations_now() noexcept {
  return can_create_entities_now() && !reload_transaction_open();
}

std::size_t deferred_mutation_count() noexcept {
  std::lock_guard<std::mutex> lock(g_deferredMutationMutex);
  return g_deferredMutationCount;
}

void truncate_deferred_mutations(std::size_t count) noexcept {
  std::lock_guard<std::mutex> lock(g_deferredMutationMutex);
  if (count < g_deferredMutationCount) {
    g_deferredMutationCount = count;
  }
}

/// Applies or queues entity destruction based on the current World phase.
/// Immediate destroys fire on_end_play for the subtree first.
bool apply_or_queue_destroy_entity(runtime::Entity entity) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    dispatch_entity_subtree_end_play(binding.world, entity);
    return binding.services->destroy_entity_op(binding.world, entity);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::DestroyEntity;
  mutation.entity = entity;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues a transform update based on the current World phase.
bool apply_or_queue_transform(runtime::Entity entity,
                              const runtime::Transform &transform,
                              bool setAuthority,
                              runtime::MovementAuthority authority) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    const bool transformUpdated =
        binding.services->add_transform_op(binding.world, entity,
                                           transform);
    if (!transformUpdated) {
      return false;
    }
    return !setAuthority ||
           binding.services->set_movement_authority_op(binding.world,
                                                       entity, authority);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::SetTransform;
  mutation.entity = entity;
  mutation.transform = transform;
  mutation.setMovementAuthority = setAuthority;
  mutation.movementAuthority = authority;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues a rigid body update based on the current World phase;
/// releaseAuthority additionally returns the entity to physics control.
bool apply_or_queue_rigid_body(runtime::Entity entity,
                               const runtime::RigidBody &rigidBody,
                               bool releaseAuthority) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    if (!binding.services->add_rigid_body_op(binding.world, entity,
                                             rigidBody)) {
      return false;
    }
    return !releaseAuthority ||
           binding.services->set_movement_authority_op(
               binding.world, entity, runtime::MovementAuthority::None);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddRigidBody;
  mutation.entity = entity;
  mutation.rigidBody = rigidBody;
  mutation.setMovementAuthority = releaseAuthority;
  mutation.movementAuthority = runtime::MovementAuthority::None;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues a collider update based on the current World phase.
bool apply_or_queue_collider(runtime::Entity entity,
                             const runtime::Collider &collider) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->add_collider_op(binding.world, entity,
                                             collider);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddCollider;
  mutation.entity = entity;
  mutation.collider = collider;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues a mesh component update based on the current World phase.
bool apply_or_queue_mesh_component(
    runtime::Entity entity, const runtime::MeshComponent &component) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->add_mesh_component_op(binding.world, entity,
                                                   component);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddMeshComponent;
  mutation.entity = entity;
  mutation.meshComponent = component;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues a name component update based on the current World phase.
bool apply_or_queue_name_component(
    runtime::Entity entity, const runtime::NameComponent &component) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->add_name_component_op(binding.world, entity,
                                                   component);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddNameComponent;
  mutation.entity = entity;
  mutation.nameComponent = component;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues a light component update based on the current World phase.
bool apply_or_queue_light_component(
    runtime::Entity entity, const runtime::LightComponent &component) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->add_light_component_op(binding.world, entity,
                                                    component);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddLightComponent;
  mutation.entity = entity;
  mutation.lightComponent = component;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues light component removal based on the current World phase.
bool apply_or_queue_remove_light_component(runtime::Entity entity) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->remove_light_component_op(binding.world,
                                                       entity);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::RemoveLightComponent;
  mutation.entity = entity;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues a script component update based on the current World phase.
bool apply_or_queue_script_component(
    runtime::Entity entity,
    const runtime::ScriptComponent &component) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->add_script_component_op(binding.world,
                                                     entity, component);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddScriptComponent;
  mutation.entity = entity;
  mutation.scriptComponent = component;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues script component removal based on the current World phase.
bool apply_or_queue_remove_script_component(runtime::Entity entity) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->remove_script_component_op(binding.world,
                                                        entity);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::RemoveScriptComponent;
  mutation.entity = entity;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues a point light update based on the current World phase.
bool apply_or_queue_point_light_component(
    runtime::Entity entity,
    const runtime::PointLightComponent &component) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->add_point_light_component_op(binding.world,
                                                          entity, component);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddPointLightComponent;
  mutation.entity = entity;
  mutation.pointLightComponent = component;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues point light removal based on the current World phase.
bool apply_or_queue_remove_point_light_component(
    runtime::Entity entity) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->remove_point_light_component_op(binding.world,
                                                             entity);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::RemovePointLightComponent;
  mutation.entity = entity;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues a spot light update based on the current World phase.
bool apply_or_queue_spot_light_component(
    runtime::Entity entity,
    const runtime::SpotLightComponent &component) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->add_spot_light_component_op(binding.world,
                                                         entity, component);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddSpotLightComponent;
  mutation.entity = entity;
  mutation.spotLightComponent = component;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues spot light removal based on the current World phase.
bool apply_or_queue_remove_spot_light_component(
    runtime::Entity entity) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->remove_spot_light_component_op(binding.world,
                                                            entity);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::RemoveSpotLightComponent;
  mutation.entity = entity;
  return queue_deferred_mutation(mutation);
}

bool apply_or_queue_spring_arm(runtime::Entity entity,
                               const math::SpringArmComponent &component) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (!runtime_bound()) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->add_spring_arm_op(binding.world, entity,
                                               component);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddSpringArm;
  mutation.entity = entity;
  mutation.springArm = component;
  return queue_deferred_mutation(mutation);
}

bool apply_or_queue_camera_component(
    runtime::Entity entity, const math::CameraComponent &component) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (!runtime_bound()) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->add_camera_component_op(binding.world, entity,
                                                     component);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddCameraComponent;
  mutation.entity = entity;
  mutation.cameraComponent = component;
  return queue_deferred_mutation(mutation);
}

bool apply_or_queue_remove_camera_component(runtime::Entity entity) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if (!runtime_bound()) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->remove_camera_component_op(binding.world, entity);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::RemoveCameraComponent;
  mutation.entity = entity;
  return queue_deferred_mutation(mutation);
}

/// Flushes queued mutations against a snapshot of the current count:
/// applying a destroy runs on_end_play, which may queue new mutations —
/// those append past the snapshot and survive into the next flush. Each
/// mutation is copied out under the queue lock and applied unlocked so
/// re-entrant queueing cannot deadlock. Apply failures, mutations dropped
/// because their target entity died, and mutations queued before a scene
/// replacement (whose target index may now belong to an unrelated entity
/// of the new scene) are counted and reported in one summary log instead
/// of being silently discarded.
void flush_deferred_mutations() noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr) ||
      !can_apply_mutations_now()) {
    return;
  }

  std::size_t count = 0U;
  {
    std::lock_guard<std::mutex> lock(g_deferredMutationMutex);
    count = g_deferredMutationCount;
  }
  if (count == 0U) {
    return;
  }

  const std::uint32_t liveEpoch = binding.services->content_epoch(binding.world);
  std::size_t failedApplies = 0U;
  std::size_t deadTargets = 0U;
  std::size_t staleEpochTargets = 0U;
  const auto note = [&failedApplies](bool applied) noexcept {
    if (!applied) {
      ++failedApplies;
    }
  };

  for (std::size_t i = 0U; i < count; ++i) {
    DeferredMutation mutation{};
    {
      std::lock_guard<std::mutex> lock(g_deferredMutationMutex);
      mutation = g_deferredMutations[i];
    }
    if (mutation.contentEpoch != liveEpoch) {
      ++staleEpochTargets;
      continue;
    }
    if (!is_deferred_entity_current(binding.world, mutation.entity)) {
      ++deadTargets;
      continue;
    }
    switch (mutation.type) {
    case DeferredMutationType::DestroyEntity:
      dispatch_entity_subtree_end_play(binding.world, mutation.entity);
      note(binding.services->destroy_entity_op(binding.world,
                                               mutation.entity));
      break;
    case DeferredMutationType::SetTransform: {
      const bool transformUpdated = binding.services->add_transform_op(
          binding.world, mutation.entity, mutation.transform);
      note(transformUpdated);
      if (transformUpdated && mutation.setMovementAuthority) {
        note(binding.services->set_movement_authority_op(
            binding.world, mutation.entity, mutation.movementAuthority));
      }
      break;
    }
    case DeferredMutationType::AddRigidBody: {
      const bool bodyUpdated = binding.services->add_rigid_body_op(
          binding.world, mutation.entity, mutation.rigidBody);
      note(bodyUpdated);
      if (bodyUpdated && mutation.setMovementAuthority) {
        note(binding.services->set_movement_authority_op(
            binding.world, mutation.entity, mutation.movementAuthority));
      }
      break;
    }
    case DeferredMutationType::AddCollider:
      note(binding.services->add_collider_op(binding.world,
                                             mutation.entity,
                                             mutation.collider));
      break;
    case DeferredMutationType::AddMeshComponent:
      note(binding.services->add_mesh_component_op(binding.world,
                                                   mutation.entity,
                                                   mutation.meshComponent));
      break;
    case DeferredMutationType::AddNameComponent:
      note(binding.services->add_name_component_op(binding.world,
                                                   mutation.entity,
                                                   mutation.nameComponent));
      break;
    case DeferredMutationType::AddLightComponent:
      note(binding.services->add_light_component_op(binding.world,
                                                    mutation.entity,
                                                    mutation.lightComponent));
      break;
    case DeferredMutationType::RemoveLightComponent:
      note(binding.services->remove_light_component_op(binding.world,
                                                       mutation.entity));
      break;
    case DeferredMutationType::AddScriptComponent:
      note(binding.services->add_script_component_op(binding.world,
                                                     mutation.entity,
                                                     mutation.scriptComponent));
      break;
    case DeferredMutationType::RemoveScriptComponent:
      note(binding.services->remove_script_component_op(
          binding.world, mutation.entity));
      break;
    case DeferredMutationType::AddPointLightComponent:
      note(binding.services->add_point_light_component_op(
          binding.world, mutation.entity, mutation.pointLightComponent));
      break;
    case DeferredMutationType::RemovePointLightComponent:
      note(binding.services->remove_point_light_component_op(
          binding.world, mutation.entity));
      break;
    case DeferredMutationType::AddSpotLightComponent:
      note(binding.services->add_spot_light_component_op(
          binding.world, mutation.entity, mutation.spotLightComponent));
      break;
    case DeferredMutationType::RemoveSpotLightComponent:
      note(binding.services->remove_spot_light_component_op(
          binding.world, mutation.entity));
      break;
    case DeferredMutationType::AddSpringArm:
      note(binding.services->add_spring_arm_op(binding.world, mutation.entity,
                                               mutation.springArm));
      break;
    case DeferredMutationType::AddCameraComponent:
      note(binding.services->add_camera_component_op(
          binding.world, mutation.entity, mutation.cameraComponent));
      break;
    case DeferredMutationType::RemoveCameraComponent:
      note(binding.services->remove_camera_component_op(binding.world,
                                                        mutation.entity));
      break;
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_deferredMutationMutex);
    const std::size_t appended = g_deferredMutationCount - count;
    for (std::size_t i = 0U; i < appended; ++i) {
      g_deferredMutations[i] = g_deferredMutations[count + i];
    }
    g_deferredMutationCount = appended;
  }

  if ((failedApplies > 0U) || (deadTargets > 0U) ||
      (staleEpochTargets > 0U)) {
    char buffer[192] = {};
    std::snprintf(buffer, sizeof(buffer),
                  "deferred mutation flush: %zu applies failed, %zu targets "
                  "already destroyed, %zu queued before a scene replacement "
                  "and dropped",
                  failedApplies, deadTargets, staleEpochTargets);
    core::log_message(core::LogLevel::Warning, "scripting", buffer);
  }
}

/// Clears queued deferred mutations without applying them.
void clear_deferred_mutations() noexcept {
  std::lock_guard<std::mutex> lock(g_deferredMutationMutex);
  g_deferredMutationCount = 0U;
}

} // namespace engine::scripting
