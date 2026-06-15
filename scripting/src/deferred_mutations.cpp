// Owns deferred world-mutation queueing for the scripting module.

#include "deferred_mutations.h"

#include "engine/core/logging.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/scripting/scripting.h"
#include "runtime_binding.h"

#include <cstddef>
#include <cstdint>

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
};

/// Stores deferred mutation data used by the engine.
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
  runtime::MovementAuthority movementAuthority =
      runtime::MovementAuthority::None;
  bool setMovementAuthority = false;
};

constexpr std::size_t kMaxDeferredMutations = 2048U;
DeferredMutation g_deferredMutations[kMaxDeferredMutations]{};
std::size_t g_deferredMutationCount = 0U;

/// Queues one deferred mutation for the next safe flush point.
bool queue_deferred_mutation(const DeferredMutation &mutation) noexcept {
  if (g_deferredMutationCount >= kMaxDeferredMutations) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "deferred mutation queue overflow");
    return false;
  }

  g_deferredMutations[g_deferredMutationCount] = mutation;
  ++g_deferredMutationCount;
  return true;
}

/// Returns whether a deferred mutation still targets the same live entity.
bool is_deferred_entity_current(runtime::World *world,
                                runtime::Entity entity) noexcept {
  return (world != nullptr) && world->is_alive(entity);
}

} // namespace

/// Returns whether script-driven world mutations may run immediately.
bool can_apply_mutations_now() noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  return (binding.world != nullptr) && (binding.services != nullptr) &&
         (binding.services->get_current_phase(binding.world) ==
          runtime::WorldPhase::Input);
}

/// Applies or queues entity destruction based on the current World phase.
bool apply_or_queue_destroy_entity(runtime::Entity entity) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->destroy_entity_op(binding.world, entity.index);
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
        binding.services->add_transform_op(binding.world, entity.index,
                                           transform);
    if (!transformUpdated) {
      return false;
    }
    return !setAuthority ||
           binding.services->set_movement_authority_op(binding.world,
                                                       entity.index, authority);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::SetTransform;
  mutation.entity = entity;
  mutation.transform = transform;
  mutation.setMovementAuthority = setAuthority;
  mutation.movementAuthority = authority;
  return queue_deferred_mutation(mutation);
}

/// Applies or queues a rigid body update based on the current World phase.
bool apply_or_queue_rigid_body(runtime::Entity entity,
                               const runtime::RigidBody &rigidBody) noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return binding.services->add_rigid_body_op(binding.world, entity.index,
                                               rigidBody);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddRigidBody;
  mutation.entity = entity;
  mutation.rigidBody = rigidBody;
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
    return binding.services->add_collider_op(binding.world, entity.index,
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
    return binding.services->add_mesh_component_op(binding.world, entity.index,
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
    return binding.services->add_name_component_op(binding.world, entity.index,
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
    return binding.services->add_light_component_op(binding.world, entity.index,
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
                                                       entity.index);
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
                                                     entity.index, component);
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
                                                        entity.index);
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
    return binding.world->add_point_light_component(entity, component);
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
    return binding.world->remove_point_light_component(entity);
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
    return binding.world->add_spot_light_component(entity, component);
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
    return binding.world->remove_spot_light_component(entity);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::RemoveSpotLightComponent;
  mutation.entity = entity;
  return queue_deferred_mutation(mutation);
}

/// Flushes queued work to the backing runtime system for deferred mutations.
void flush_deferred_mutations() noexcept {
  const ScriptingRuntimeBinding &binding = runtime_binding();
  if ((binding.world == nullptr) || (binding.services == nullptr) ||
      (g_deferredMutationCount == 0U) || !can_apply_mutations_now()) {
    return;
  }

  const std::size_t count = g_deferredMutationCount;
  g_deferredMutationCount = 0U;
  for (std::size_t i = 0U; i < count; ++i) {
    const DeferredMutation &mutation = g_deferredMutations[i];
    switch (mutation.type) {
    case DeferredMutationType::DestroyEntity:
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(
            binding.services->destroy_entity_op(binding.world,
                                                mutation.entity.index));
      }
      break;
    case DeferredMutationType::SetTransform: {
      if (!is_deferred_entity_current(binding.world, mutation.entity)) {
        break;
      }
      const bool transformUpdated = binding.services->add_transform_op(
          binding.world, mutation.entity.index, mutation.transform);
      if (transformUpdated && mutation.setMovementAuthority) {
        static_cast<void>(binding.services->set_movement_authority_op(
            binding.world, mutation.entity.index, mutation.movementAuthority));
      }
      break;
    }
    case DeferredMutationType::AddRigidBody:
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(binding.services->add_rigid_body_op(
            binding.world, mutation.entity.index, mutation.rigidBody));
      }
      break;
    case DeferredMutationType::AddCollider:
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(binding.services->add_collider_op(
            binding.world, mutation.entity.index, mutation.collider));
      }
      break;
    case DeferredMutationType::AddMeshComponent:
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(binding.services->add_mesh_component_op(
            binding.world, mutation.entity.index, mutation.meshComponent));
      }
      break;
    case DeferredMutationType::AddNameComponent:
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(binding.services->add_name_component_op(
            binding.world, mutation.entity.index, mutation.nameComponent));
      }
      break;
    case DeferredMutationType::AddLightComponent:
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(binding.services->add_light_component_op(
            binding.world, mutation.entity.index, mutation.lightComponent));
      }
      break;
    case DeferredMutationType::RemoveLightComponent:
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(binding.services->remove_light_component_op(
            binding.world, mutation.entity.index));
      }
      break;
    case DeferredMutationType::AddScriptComponent:
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(binding.services->add_script_component_op(
            binding.world, mutation.entity.index, mutation.scriptComponent));
      }
      break;
    case DeferredMutationType::RemoveScriptComponent:
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(binding.services->remove_script_component_op(
            binding.world, mutation.entity.index));
      }
      break;
    case DeferredMutationType::AddPointLightComponent: {
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(binding.world->add_point_light_component(
            mutation.entity, mutation.pointLightComponent));
      }
      break;
    }
    case DeferredMutationType::RemovePointLightComponent: {
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(
            binding.world->remove_point_light_component(mutation.entity));
      }
      break;
    }
    case DeferredMutationType::AddSpotLightComponent: {
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(binding.world->add_spot_light_component(
            mutation.entity, mutation.spotLightComponent));
      }
      break;
    }
    case DeferredMutationType::RemoveSpotLightComponent: {
      if (is_deferred_entity_current(binding.world, mutation.entity)) {
        static_cast<void>(
            binding.world->remove_spot_light_component(mutation.entity));
      }
      break;
    }
    }
  }
}

/// Clears queued deferred mutations without applying them.
void clear_deferred_mutations() noexcept { g_deferredMutationCount = 0U; }

} // namespace engine::scripting
