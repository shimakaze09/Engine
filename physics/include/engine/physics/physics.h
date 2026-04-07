#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

namespace engine::physics {

struct RayHit final {
  runtime::Entity entity = runtime::kInvalidEntity;
  float distance = 0.0F;
  math::Vec3 point = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Vec3 normal = math::Vec3(0.0F, 0.0F, 0.0F);
};

bool step_physics(runtime::World &world, float deltaSeconds) noexcept;
bool step_physics_range(runtime::World &world, std::size_t startIndex,
                        std::size_t count, float deltaSeconds) noexcept;
bool resolve_collisions(runtime::World &world) noexcept;

void set_gravity(float x, float y, float z) noexcept;
math::Vec3 get_gravity() noexcept;

// Raycasting ------------------------------------------------------------------
// Cast a ray and return the closest hit. Returns true if any collider is hit.
bool raycast(const runtime::World &world, const math::Vec3 &origin,
             const math::Vec3 &direction, float maxDistance, RayHit *outHit,
             runtime::Entity skipEntity = runtime::kInvalidEntity) noexcept;

// Cast a ray and return all hits up to maxHits, sorted by distance.
// Returns the number of hits written to outHits.
std::size_t raycast_all(const runtime::World &world, const math::Vec3 &origin,
                        const math::Vec3 &direction, float maxDistance,
                        RayHit *outHits, std::size_t maxHits) noexcept;

// Joints / Constraints --------------------------------------------------------
using JointId = std::uint32_t;
constexpr JointId kInvalidJointId = 0xFFFFFFFFU;

enum class JointType : std::uint8_t { Distance = 0, Hinge = 1 };

struct JointDesc final {
  runtime::Entity entityA = runtime::kInvalidEntity;
  runtime::Entity entityB = runtime::kInvalidEntity;
  JointType type = JointType::Distance;
  float distance = 1.0F;
};

// Add a distance joint between two entities. Returns the joint id.
JointId add_joint(const JointDesc &desc) noexcept;

// Remove a joint by id.
void remove_joint(JointId id) noexcept;

// Solve all active joints. Called automatically by resolve_collisions.
void solve_joints(runtime::World &world) noexcept;

// Sleeping / deactivation -----------------------------------------------------
// Wake a sleeping rigid body so it resumes simulation.
void wake_body(runtime::World &world, runtime::Entity entity) noexcept;

// Query whether a rigid body is currently sleeping.
bool is_sleeping(const runtime::World &world, runtime::Entity entity) noexcept;

// Collision callbacks ---------------------------------------------------------
// pairData points to an array of [entityIndexA, entityIndexB, ...] uint32
// pairs. pairCount is the number of pairs (not the element count).
using CollisionDispatchFn = void (*)(const std::uint32_t *pairs,
                                     std::size_t pairCount) noexcept;

// Register a callback to receive collision pair data each frame.
void set_collision_dispatch(CollisionDispatchFn fn) noexcept;

// Dispatch all recorded collision pairs and reset the pair buffer.
// Call once per frame on the main thread after the physics step completes.
void dispatch_collision_callbacks() noexcept;

} // namespace engine::physics
