// Declares private deferred world-mutation helpers for scripting.

#pragma once

#include "engine/scripting/runtime_services.h"

namespace engine::scripting {

/// Returns whether script-driven world mutations may run immediately:
/// the World is in its mutation phase and no hot-reload scope is staging.
bool can_apply_mutations_now() noexcept;
/// Returns whether scripts may create entities now: the phase check alone,
/// because a hot-reload scope records creations instead of staging them.
bool can_create_entities_now() noexcept;
/// Queued mutations not yet flushed; the hot-reload scope truncates back
/// to this on rollback.
std::size_t deferred_mutation_count() noexcept;
/// Drops every queued mutation past `count`, oldest kept.
void truncate_deferred_mutations(std::size_t count) noexcept;

// Read-through component reads: read-modify-write setters must see the
// newest queued snapshot, or later deferred writes clobber earlier ones
// with stale state. A queued destroy/removal reads as absent.

/// Reads the entity's transform through any pending queued write.
bool latest_transform(runtime::Entity entity,
                      runtime::Transform *outTransform) noexcept;

/// Reads the entity's rigid body through any pending queued write.
bool latest_rigid_body(runtime::Entity entity,
                       runtime::RigidBody *outRigidBody) noexcept;

/// Reads the entity's collider through any pending queued write.
bool latest_collider(runtime::Entity entity,
                     runtime::Collider *outCollider) noexcept;

/// Reads the entity's mesh component through any pending queued write.
bool latest_mesh_component(runtime::Entity entity,
                           runtime::MeshComponent *outComponent) noexcept;

/// Reads the entity's light component through any pending queued write.
bool latest_light_component(runtime::Entity entity,
                            runtime::LightComponent *outComponent) noexcept;

/// Reads the entity's point light through any pending queued write, so an
/// add followed by a get or set inside on_begin_play sees the light.
bool latest_point_light_component(
    runtime::Entity entity, runtime::PointLightComponent *outComponent) noexcept;

/// Reads the entity's spot light through any pending queued write.
bool latest_spot_light_component(
    runtime::Entity entity, runtime::SpotLightComponent *outComponent) noexcept;

/// Reads the entity's spring arm through any pending queued write.
bool latest_spring_arm(runtime::Entity entity,
                       math::SpringArmComponent *outComponent) noexcept;

/// Reads the entity's camera component through any pending queued write.
bool latest_camera_component(runtime::Entity entity,
                             math::CameraComponent *outComponent) noexcept;

/// Applies or queues entity destruction based on the current World phase.
bool apply_or_queue_destroy_entity(runtime::Entity entity) noexcept;

/// Applies or queues a transform update based on the current World phase.
bool apply_or_queue_transform(runtime::Entity entity,
                              const runtime::Transform &transform,
                              bool setAuthority,
                              runtime::MovementAuthority authority) noexcept;

/// Applies or queues a rigid body update based on the current World phase.
/// releaseAuthority hands movement authority back to physics — velocity
/// writes pass true so a teleported entity resumes simulating; body
/// configuration writes pass false and leave script movers untouched.
bool apply_or_queue_rigid_body(runtime::Entity entity,
                               const runtime::RigidBody &rigidBody,
                               bool releaseAuthority = false) noexcept;

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

/// Applies or queues a spring arm update based on the current World phase.
bool apply_or_queue_spring_arm(runtime::Entity entity,
                               const math::SpringArmComponent &component) noexcept;

/// Applies or queues a camera component update based on the current World
/// phase.
bool apply_or_queue_camera_component(
    runtime::Entity entity, const math::CameraComponent &component) noexcept;

/// Applies or queues camera component removal based on the current World
/// phase.
bool apply_or_queue_remove_camera_component(runtime::Entity entity) noexcept;

/// Clears queued deferred mutations without applying them.
void clear_deferred_mutations() noexcept;

} // namespace engine::scripting
