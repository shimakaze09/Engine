// Implements World::state_hash: one fold over the subset of simulation
// state the determinism scenarios compare, in dense storage order, so every
// determinism test and the CI cross-platform compare read the same
// observable instead of each hashing its own subset.

#include "engine/runtime/world.h"

#include <cstring>

#include "engine/core/hash.h"

namespace engine::runtime {
namespace {

/// Word-wise FNV-1a accumulator over the exact bits of each field, so a
/// one-ulp float difference changes the hash.
struct StateHasher final {
  std::uint64_t hash = core::kFnv1a64Offset;

  void u32(std::uint32_t value) noexcept {
    hash = core::fnv1a_64_append_u64(hash, value);
  }
  void f32(float value) noexcept {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    u32(bits);
  }
  void vec3(const math::Vec3 &value) noexcept {
    f32(value.x);
    f32(value.y);
    f32(value.z);
  }
  void quat(const math::Quat &value) noexcept {
    f32(value.x);
    f32(value.y);
    f32(value.z);
    f32(value.w);
  }
  void entity(Entity value) noexcept {
    u32(value.index);
    u32(value.generation);
  }
};

} // namespace

std::uint64_t World::state_hash() const noexcept {
  StateHasher h{};

  // Every section starts with its count so an empty section and a missing
  // one never fold to the same value.
  h.u32(static_cast<std::uint32_t>(m_aliveEntityCount));
  for_each_alive([&h, this](Entity entity) noexcept {
    h.entity(entity);
    h.u32(m_entityPersistentIds[entity.index]);
  });

  const std::size_t stateIndex = query_state_index();
  h.u32(static_cast<std::uint32_t>(m_transforms.count()));
  for (std::size_t i = 0U; i < m_transforms.count(); ++i) {
    const Transform &transform = m_transforms.component_at(i, stateIndex);
    h.entity(m_transforms.entity_at(i));
    h.vec3(transform.position);
    h.quat(transform.rotation);
    h.vec3(transform.scale);
    h.u32(transform.parentId);
  }

  h.u32(static_cast<std::uint32_t>(m_rigidBodies.count()));
  for (std::size_t i = 0U; i < m_rigidBodies.count(); ++i) {
    const RigidBody &body = m_rigidBodies.component_at(i);
    h.entity(m_rigidBodies.entity_at(i));
    h.vec3(body.velocity);
    h.vec3(body.angularVelocity);
    h.u32(body.sleepFrameCount);
    h.u32(body.sleeping ? 1U : 0U);
  }

  const physics::PhysicsContext &physics = m_physicsContext;
  h.vec3(physics.gravity);
  h.u32(static_cast<std::uint32_t>(physics.collisionPairCount));
  for (std::size_t i = 0U; i < physics.collisionPairCount * 2U; ++i) {
    h.entity(physics.collisionPairData[i]);
  }

  h.u32(static_cast<std::uint32_t>(m_timerManager.active_count()));
  h.f32(m_timerManager.elapsed_seconds());
  for (std::size_t slot = 0U; slot < TimerManager::kMaxTimers; ++slot) {
    const TimerManager::Entry &entry = m_timerManager.entry_at(slot);
    if (!entry.active) {
      continue;
    }
    h.u32(static_cast<std::uint32_t>(slot));
    h.f32(entry.fireAt);
    h.f32(entry.interval);
    h.u32(entry.repeat ? 1U : 0U);
  }

  h.u32(static_cast<std::uint32_t>(m_animationComponents.count()));
  for (std::size_t i = 0U; i < m_animationComponents.count(); ++i) {
    const AnimationComponent &animation =
        m_animationComponents.component_at(i);
    h.entity(m_animationComponents.entity_at(i));
    h.u32(animation.playing ? 1U : 0U);
    h.f32(animation.playbackSpeed);
    h.u32(animation.currentState);
    h.u32(animation.previousState);
    h.f32(animation.stateTime);
    h.f32(animation.previousStateTime);
    h.f32(animation.blendRemaining);
    h.f32(animation.blendDuration);
    h.u32(animation.paramCount);
    for (std::uint32_t p = 0U; p < animation.paramCount &&
                               p < AnimationComponent::kMaxParams; ++p) {
      h.u32(animation.params[p].nameHash);
      h.f32(animation.params[p].value);
    }
  }

  return h.hash;
}

} // namespace engine::runtime
