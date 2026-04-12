#include "engine/physics/constraint_solver.h"

#include "engine/math/vec3.h"

#include <cmath>
#include <cstddef>

namespace engine::physics {

namespace {

ContactManifold g_manifolds[kMaxContactManifolds]{};
std::size_t g_manifoldCount = 0U;

// Find an existing manifold for this entity pair, or return nullptr.
ContactManifold *find_manifold(std::uint32_t entityIndexA,
                               std::uint32_t entityIndexB) noexcept {
  for (std::size_t i = 0U; i < g_manifoldCount; ++i) {
    if (((g_manifolds[i].entityIndexA == entityIndexA) &&
         (g_manifolds[i].entityIndexB == entityIndexB)) ||
        ((g_manifolds[i].entityIndexA == entityIndexB) &&
         (g_manifolds[i].entityIndexB == entityIndexA))) {
      return &g_manifolds[i];
    }
  }
  return nullptr;
}

// Allocate a new manifold slot, or evict the oldest if full.
ContactManifold *allocate_manifold() noexcept {
  if (g_manifoldCount < kMaxContactManifolds) {
    ContactManifold *m = &g_manifolds[g_manifoldCount];
    ++g_manifoldCount;
    return m;
  }

  // All slots full — find the oldest (lowest lastFrameUsed).
  std::size_t oldestIdx = 0U;
  std::uint32_t oldestFrame = g_manifolds[0U].lastFrameUsed;
  for (std::size_t i = 1U; i < g_manifoldCount; ++i) {
    if (g_manifolds[i].lastFrameUsed < oldestFrame) {
      oldestFrame = g_manifolds[i].lastFrameUsed;
      oldestIdx = i;
    }
  }

  g_manifolds[oldestIdx] = ContactManifold{};
  return &g_manifolds[oldestIdx];
}

// Feature-ID based contact matching threshold.
constexpr float kContactMatchDistSq = 0.01F; // 0.1 units

// Find the best matching existing contact by feature ID or proximity.
std::size_t find_matching_contact(const ContactManifold &m,
                                  const math::Vec3 &pointOnA,
                                  std::uint32_t featureId) noexcept {
  // First try matching by feature ID.
  for (std::size_t i = 0U; i < m.contactCount; ++i) {
    if (m.contacts[i].featureId == featureId && featureId != 0U) {
      return i;
    }
  }

  // Fall back to proximity.
  float bestDistSq = kContactMatchDistSq;
  std::size_t bestIdx = m.contactCount; // sentinel = no match
  for (std::size_t i = 0U; i < m.contactCount; ++i) {
    const math::Vec3 diff = math::sub(m.contacts[i].pointOnA, pointOnA);
    const float distSq = math::dot(diff, diff);
    if (distSq < bestDistSq) {
      bestDistSq = distSq;
      bestIdx = i;
    }
  }
  return bestIdx;
}

struct ExtendedManifold final {
  ManifoldContact contacts[ContactManifold::kMaxContacts + 1U]{};
  std::size_t contactCount = 0U;
};

// Manifold reduction: keep at most 4 contacts that maximize contact area.
// Algorithm: keep deepest, then pick 3 more maximizing spread.
void reduce_manifold(ExtendedManifold &em, ContactManifold &m) noexcept {
  if (em.contactCount <= ContactManifold::kMaxContacts) {
    for (std::size_t i = 0U; i < em.contactCount; ++i) {
      m.contacts[i] = em.contacts[i];
    }
    m.contactCount = em.contactCount;
    return;
  }

  ManifoldContact kept[ContactManifold::kMaxContacts]{};
  bool used[ContactManifold::kMaxContacts + 2U] = {};
  const std::size_t n = em.contactCount;

  // 1. Pick deepest penetration.
  std::size_t deepestIdx = 0U;
  float deepestPen = em.contacts[0U].penetration;
  for (std::size_t i = 1U; i < n; ++i) {
    if (em.contacts[i].penetration > deepestPen) {
      deepestPen = em.contacts[i].penetration;
      deepestIdx = i;
    }
  }
  kept[0U] = em.contacts[deepestIdx];
  used[deepestIdx] = true;

  // 2. Pick farthest from first.
  float maxDistSq = -1.0F;
  std::size_t secondIdx = 0U;
  for (std::size_t i = 0U; i < n; ++i) {
    if (used[i]) {
      continue;
    }
    const math::Vec3 d =
        math::sub(em.contacts[i].pointOnA, kept[0U].pointOnA);
    const float distSq = math::dot(d, d);
    if (distSq > maxDistSq) {
      maxDistSq = distSq;
      secondIdx = i;
    }
  }
  kept[1U] = em.contacts[secondIdx];
  used[secondIdx] = true;

  // 3. Pick point forming largest triangle area with first two.
  float maxArea = -1.0F;
  std::size_t thirdIdx = 0U;
  const math::Vec3 edge01 = math::sub(kept[1U].pointOnA, kept[0U].pointOnA);
  for (std::size_t i = 0U; i < n; ++i) {
    if (used[i]) {
      continue;
    }
    const math::Vec3 edge0i =
        math::sub(em.contacts[i].pointOnA, kept[0U].pointOnA);
    const math::Vec3 crossVec = math::cross(edge01, edge0i);
    const float area = math::dot(crossVec, crossVec);
    if (area > maxArea) {
      maxArea = area;
      thirdIdx = i;
    }
  }
  if (n > 2U && !used[thirdIdx]) {
    kept[2U] = em.contacts[thirdIdx];
    used[thirdIdx] = true;
  }

  // 4. Pick point forming largest quadrilateral area with first three.
  float maxArea2 = -1.0F;
  std::size_t fourthIdx = 0U;
  for (std::size_t i = 0U; i < n; ++i) {
    if (used[i]) {
      continue;
    }
    const math::Vec3 e0 =
        math::sub(em.contacts[i].pointOnA, kept[0U].pointOnA);
    const math::Vec3 e1 =
        math::sub(em.contacts[i].pointOnA, kept[1U].pointOnA);
    const math::Vec3 crossVec = math::cross(e0, e1);
    const float area = math::dot(crossVec, crossVec);
    if (area > maxArea2) {
      maxArea2 = area;
      fourthIdx = i;
    }
  }
  if (n > 3U && !used[fourthIdx]) {
    kept[3U] = em.contacts[fourthIdx];
  }

  // Copy back.
  for (std::size_t i = 0U; i < ContactManifold::kMaxContacts; ++i) {
    m.contacts[i] = kept[i];
  }
  m.contactCount = ContactManifold::kMaxContacts;
}

} // namespace

std::size_t manifold_add_contact(std::uint32_t entityIndexA,
                                 std::uint32_t entityIndexB,
                                 const math::Vec3 &pointOnA,
                                 const math::Vec3 &pointOnB,
                                 const math::Vec3 &normal, float penetration,
                                 std::uint32_t featureId,
                                 std::uint32_t frameNumber) noexcept {
  ContactManifold *m = find_manifold(entityIndexA, entityIndexB);
  if (m == nullptr) {
    m = allocate_manifold();
    m->entityIndexA = entityIndexA;
    m->entityIndexB = entityIndexB;
    m->contactCount = 0U;
  }
  m->lastFrameUsed = frameNumber;

  // Try to match an existing contact.
  const std::size_t matchIdx = find_matching_contact(*m, pointOnA, featureId);
  if (matchIdx < m->contactCount) {
    // Update existing contact, preserving accumulated impulse.
    ManifoldContact &c = m->contacts[matchIdx];
    c.pointOnA = pointOnA;
    c.pointOnB = pointOnB;
    c.normal = normal;
    c.penetration = penetration;
    c.featureId = featureId;
  } else if (m->contactCount < ContactManifold::kMaxContacts) {
    // Add new contact.
    ManifoldContact &c = m->contacts[m->contactCount];
    c.pointOnA = pointOnA;
    c.pointOnB = pointOnB;
    c.normal = normal;
    c.penetration = penetration;
    c.accumulatedNormalImpulse = 0.0F;
    c.featureId = featureId;
    ++m->contactCount;
  } else {
    // Manifold full — copy to extended buffer, add new, reduce back to 4.
    ExtendedManifold em{};
    for (std::size_t i = 0U; i < m->contactCount; ++i) {
      em.contacts[i] = m->contacts[i];
    }
    em.contactCount = m->contactCount;
    ManifoldContact &c = em.contacts[em.contactCount];
    c.pointOnA = pointOnA;
    c.pointOnB = pointOnB;
    c.normal = normal;
    c.penetration = penetration;
    c.accumulatedNormalImpulse = 0.0F;
    c.featureId = featureId;
    ++em.contactCount;
    reduce_manifold(em, *m);
  }

  // Return manifold index.
  return static_cast<std::size_t>(m - g_manifolds);
}

void manifold_evict_stale(std::uint32_t frameNumber) noexcept {
  std::size_t writeIdx = 0U;
  for (std::size_t i = 0U; i < g_manifoldCount; ++i) {
    if (g_manifolds[i].lastFrameUsed >= frameNumber) {
      if (writeIdx != i) {
        g_manifolds[writeIdx] = g_manifolds[i];
      }
      ++writeIdx;
    }
  }
  g_manifoldCount = writeIdx;
}

std::size_t manifold_count() noexcept { return g_manifoldCount; }

void manifold_reset() noexcept {
  for (std::size_t i = 0U; i < g_manifoldCount; ++i) {
    g_manifolds[i] = ContactManifold{};
  }
  g_manifoldCount = 0U;
}

const ContactManifold *manifold_get(std::size_t index) noexcept {
  if (index >= g_manifoldCount) {
    return nullptr;
  }
  return &g_manifolds[index];
}

} // namespace engine::physics
