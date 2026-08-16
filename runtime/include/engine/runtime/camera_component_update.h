// Declares the update pass that publishes authored CameraComponents into
// the World's CameraManager priority stack, and the authoring-time query
// that resolves which authored camera currently wins CameraManager's
// priority selection.

#pragma once

#include <cstdint>

#include "engine/core/entity.h"

namespace engine::runtime {

class World;

/// Publishes every active, non-SpringArm-owned CameraComponent's derived
/// pose (from the entity's world transform) into the World's CameraManager
/// priority stack for this frame. Entities that also carry a
/// SpringArmComponent are skipped here: update_spring_arm_cameras already
/// publishes their pose and reads the same CameraComponent for its lens and
/// priority/blend settings, so the two systems never race to push the same
/// owner. Call once per frame after update_spring_arm_cameras.
void update_persistent_cameras(World &world, float dt) noexcept;

/// Resolves which authored CameraComponent would currently win
/// CameraManager's priority selection (the active, highest-priority one, in
/// World::for_each dense order on ties -- the same tie-break
/// CameraManager::active_camera applies once cameras are actually pushed).
/// Unlike querying World::camera_manager() directly, this works whether or
/// not the pipeline has run this frame (CameraManager is only populated
/// while playing), so editor authoring UI can show selection/conflict state
/// at edit time too. Returns kInvalidEntity when no CameraComponent is
/// active. Writes the number of *other* active cameras tied with the
/// winner's priority to *outTieCount when non-null (0 when unique or none).
core::Entity find_authored_active_camera(const World &world,
                                         std::uint32_t *outTieCount) noexcept;

} // namespace engine::runtime
