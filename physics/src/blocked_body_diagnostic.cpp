// Implements the blocked-body warning diagnostic: velocity-driven bodies
// whose achieved displacement persistently falls far short of their
// commanded speed get one rate-limited log warning per blocking episode,
// naming the blocking partner when the step recorded a contact pair. The
// diagnostic only reads simulation state and never feeds back into it.

#include "blocked_body_diagnostic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_context.h"

namespace engine::physics {

namespace {

// Commanded speeds below this floor are ignored: gravity alone gives a
// supported resting body ~g*dt (~0.16 m/s at 60 Hz) of commanded-but-undone
// velocity every step, which must not read as "blocked".
constexpr float kBlockedSpeedFloor = 0.25F;

// A step counts as blocked when achieved displacement per second falls
// below this fraction of the commanded speed.
constexpr float kBlockedAchievedFraction = 0.25F;

// Episode counters saturate at 255, so thresholds clamp safely below it.
constexpr int kMaxWarnThreshold = 250;

/// Consecutive-blocked-step threshold from physics.blocked_warn_steps;
/// 0 disables the diagnostic.
int blocked_warn_step_threshold() noexcept {
  const float value = core::cvar_get_float("physics.blocked_warn_steps", 30.0F);
  if (!(value > 0.0F) || !std::isfinite(value)) {
    return 0;
  }
  return std::clamp(static_cast<int>(value), 1, kMaxWarnThreshold);
}

/// First recorded contact-pair partner index for the entity this step, or
/// 0 when the step recorded no pair for it (speculative-only or CCD-clamped
/// contacts record none).
std::uint32_t find_blocking_partner(const PhysicsContext &ctx,
                                    std::uint32_t entityIndex) noexcept {
  const std::size_t elementCount = ctx.collisionPairCount * 2U;
  for (std::size_t i = 0U; i < elementCount; ++i) {
    if (ctx.collisionPairData[i] == entityIndex) {
      return ctx.collisionPairData[i ^ 1U];
    }
  }
  return 0U;
}

} // namespace

void capture_blocked_body_commands(PhysicsWorldView &world) noexcept {
  PhysicsShapeStore *store = world.physics_context().shapeStore.get();
  if ((store == nullptr) || (blocked_warn_step_threshold() <= 0)) {
    return;
  }

  const std::size_t count =
      std::min(world.rigid_body_count(), store->blockedCommandedSpeeds.size());
  const Entity *entities = nullptr;
  RigidBody *bodies = nullptr;
  if ((count == 0U) ||
      !world.get_rigid_body_range(0U, count, &entities, &bodies)) {
    return;
  }

  for (std::size_t i = 0U; i < count; ++i) {
    const RigidBody &body = bodies[i];
    const bool eligible =
        (body.inverseMass > 0.0F) && !body.sleeping &&
        (world.movement_authority(entities[i]) != MovementAuthority::Script);
    store->blockedCommandedSpeeds[i] =
        eligible ? engine::math::length(body.velocity) : -1.0F;
  }
}

void report_blocked_bodies(PhysicsWorldView &world,
                           float deltaSeconds) noexcept {
  const int threshold = blocked_warn_step_threshold();
  PhysicsContext &physicsCtx = world.physics_context();
  PhysicsShapeStore *store = physicsCtx.shapeStore.get();
  if ((store == nullptr) || (threshold <= 0) || (deltaSeconds <= 0.0F)) {
    return;
  }

  const auto simToken = world.simulation_access_token();
  const std::size_t count =
      std::min(world.rigid_body_count(), store->blockedCommandedSpeeds.size());
  const Entity *entities = nullptr;
  RigidBody *bodies = nullptr;
  if ((count == 0U) ||
      !world.get_rigid_body_range(0U, count, &entities, &bodies)) {
    return;
  }

  for (std::size_t i = 0U; i < count; ++i) {
    const Entity entity = entities[i];
    const std::uint32_t entityIndex = entity.index;
    if ((entityIndex == 0U) ||
        (entityIndex >= store->blockedStepCounts.size())) {
      continue;
    }
    std::uint8_t &blockedSteps = store->blockedStepCounts[entityIndex];

    const float commandedSpeed = store->blockedCommandedSpeeds[i];
    if (commandedSpeed < kBlockedSpeedFloor) {
      blockedSteps = 0U;
      continue;
    }

    Transform readTransform{};
    const Transform *writeTransform =
        world.get_transform_write_ptr(entity, simToken);
    if (!world.get_transform(entity, &readTransform) ||
        (writeTransform == nullptr)) {
      blockedSteps = 0U;
      continue;
    }

    const float achievedPerSecond =
        engine::math::length(engine::math::sub(writeTransform->position,
                                               readTransform.position)) /
        deltaSeconds;
    if (achievedPerSecond >= (kBlockedAchievedFraction * commandedSpeed)) {
      blockedSteps = 0U;
      continue;
    }

    if (blockedSteps < 255U) {
      ++blockedSteps;
    }
    if (blockedSteps != static_cast<std::uint8_t>(threshold)) {
      continue;
    }

    const std::uint32_t partnerIndex =
        find_blocking_partner(physicsCtx, entityIndex);
    ++store->blockedWarningCount;
    store->blockedLastEntityIndex = entityIndex;
    store->blockedLastBlockerIndex = partnerIndex;

    char message[160];
    if (partnerIndex != 0U) {
      std::snprintf(message, sizeof(message),
                    "velocity-driven body (entity %u) blocked: commanded "
                    "%.2f m/s but moved %.2f m/s for %d steps (touching "
                    "entity %u)",
                    entityIndex, static_cast<double>(commandedSpeed),
                    static_cast<double>(achievedPerSecond), threshold,
                    partnerIndex);
    } else {
      std::snprintf(message, sizeof(message),
                    "velocity-driven body (entity %u) blocked: commanded "
                    "%.2f m/s but moved %.2f m/s for %d steps (no recorded "
                    "contact pair)",
                    entityIndex, static_cast<double>(commandedSpeed),
                    static_cast<double>(achievedPerSecond), threshold);
    }
    core::log_message(core::LogLevel::Warning, "physics", message);
  }
}

BlockedBodyWarningStats
blocked_body_warning_stats(const PhysicsWorldView &world) noexcept {
  BlockedBodyWarningStats stats{};
  const PhysicsShapeStore *store = world.physics_context().shapeStore.get();
  if (store != nullptr) {
    stats.totalWarnings = store->blockedWarningCount;
    stats.lastBlockedEntityIndex = store->blockedLastEntityIndex;
    stats.lastBlockingEntityIndex = store->blockedLastBlockerIndex;
  }
  return stats;
}

} // namespace engine::physics
