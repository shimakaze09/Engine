// Declares private deferred world-mutation helpers for scripting.

#pragma once

#include "engine/runtime/world.h"

namespace engine::scripting {

/// Returns whether script-driven world mutations may run immediately.
bool can_apply_mutations_now() noexcept;

/// Applies or queues entity destruction based on the current World phase.
bool apply_or_queue_destroy_entity(runtime::Entity entity) noexcept;

/// Applies or queues a transform update based on the current World phase.
bool apply_or_queue_transform(runtime::Entity entity,
                              const runtime::Transform &transform,
                              bool setAuthority,
                              runtime::MovementAuthority authority) noexcept;

/// Applies or queues a rigid body update based on the current World phase.
bool apply_or_queue_rigid_body(runtime::Entity entity,
                               const runtime::RigidBody &rigidBody) noexcept;

/// Applies or queues a collider update based on the current World phase.
bool apply_or_queue_collider(runtime::Entity entity,
                             const runtime::Collider &collider) noexcept;

/// Applies or queues a mesh component update based on the current World phase.
bool apply_or_queue_mesh_component(
    runtime::Entity entity, const runtime::MeshComponent &component) noexcept;

/// Applies or queues a name component update based on the current World phase.
bool apply_or_queue_name_component(
    runtime::Entity entity, const runtime::NameComponent &component) noexcept;

/// Applies or queues a light component update based on the current World phase.
bool apply_or_queue_light_component(
    runtime::Entity entity, const runtime::LightComponent &component) noexcept;

/// Applies or queues light component removal based on the current World phase.
bool apply_or_queue_remove_light_component(runtime::Entity entity) noexcept;

/// Applies or queues a script component update based on the current World phase.
bool apply_or_queue_script_component(
    runtime::Entity entity,
    const runtime::ScriptComponent &component) noexcept;

/// Applies or queues script component removal based on the current World phase.
bool apply_or_queue_remove_script_component(runtime::Entity entity) noexcept;

/// Applies or queues a point light update based on the current World phase.
bool apply_or_queue_point_light_component(
    runtime::Entity entity,
    const runtime::PointLightComponent &component) noexcept;

/// Applies or queues point light removal based on the current World phase.
bool apply_or_queue_remove_point_light_component(
    runtime::Entity entity) noexcept;

/// Applies or queues a spot light update based on the current World phase.
bool apply_or_queue_spot_light_component(
    runtime::Entity entity,
    const runtime::SpotLightComponent &component) noexcept;

/// Applies or queues spot light removal based on the current World phase.
bool apply_or_queue_remove_spot_light_component(
    runtime::Entity entity) noexcept;

/// Clears queued deferred mutations without applying them.
void clear_deferred_mutations() noexcept;

} // namespace engine::scripting
