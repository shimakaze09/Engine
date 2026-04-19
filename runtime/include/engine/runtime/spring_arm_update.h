#pragma once

namespace engine::runtime {

class World;

/// Iterate all SpringArmComponents, compute camera boom positions from entity
/// transforms, and push resulting cameras to the world's CameraManager.
/// Call once per frame after simulation, before render prep.
void update_spring_arm_cameras(World &world, float dt) noexcept;

} // namespace engine::runtime
