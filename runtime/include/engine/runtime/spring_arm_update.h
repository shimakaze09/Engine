// Declares spring arm update types and APIs for the Engine runtime world.

#pragma once

namespace engine::runtime {

class World;

/// Iterate all SpringArmComponents, compute camera boom positions from the
/// composed world transforms (with a sphere-sweep collision clamp when
/// enabled), and push resulting cameras to the world's CameraManager.
/// Call once per fixed simulation step, after simulation for that step.
void update_spring_arm_cameras(World &world, float dt) noexcept;

} // namespace engine::runtime
