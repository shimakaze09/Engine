// Implements the world-scoped persistent contact-manifold cache: pair
// lookup keyed by full Entity identity, contact matching/reduction, and
// frame-stamped eviction (issue #110 moved it off a process-global store).

#include "engine/physics/constraint_solver.h"

#include "engine/math/vec3.h"
#include "engine/physics/physics_context.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::physics {

namespace {

// Returns the store's manifold array, or nullptr when heap-backed storage
// is unavailable.
PhysicsShapeStore *manifold_store(PhysicsContext &context) noexcept {
  return context.shapeStore.get();
}

// Order-independent hash key from the pair's entity indices; lookups verify
// full Entity identity, so index reuse can never alias through the key.
std::uint64_t manifold_pair_key(Entity entityA, Entity entityB) noexcept {
  const std::uint32_t lo = std::min(entityA.index, entityB.index);
  const std::uint32_t hi = std::max(entityA.index, entityB.index);
  return (static_cast<std::uint64_t>(lo) << 32U) |
         static_cast<std::uint64_t>(hi);
}

// Fibonacci-mixed starting bucket for a pair key.
std::size_t manifold_bucket(std::uint64_t key) noexcept {
  return static_cast<std::size_t>((key * 11400714819323198485ULL) %
                                  kManifoldHashBuckets);
}

// Inserts a manifold slot into the first empty probe bucket.
void manifold_index_insert(PhysicsShapeStore &store,
                           std::uint32_t slot) noexcept {
  const ContactManifold &m = store.contactManifolds[slot];
  std::size_t bucket = manifold_bucket(manifold_pair_key(m.entityA, m.entityB));
  for (std::size_t probe = 0U; probe < kManifoldHashBuckets; ++probe) {
    if (store.contactManifoldHash[bucket] == kManifoldSlotEmpty) {
      store.contactManifoldHash[bucket] = slot;
      return;
    }
    bucket = (bucket + 1U) % kManifoldHashBuckets;
  }
}

// Rebuilds the whole pair index from the live manifold set.
void manifold_index_rebuild(PhysicsShapeStore &store) noexcept {
  store.contactManifoldHash.fill(kManifoldSlotEmpty);
  for (std::size_t i = 0U; i < store.contactManifoldCount; ++i) {
    manifold_index_insert(store, static_cast<std::uint32_t>(i));
  }
}

// Find an existing manifold for this entity pair (either order, exact
// index+generation match) through the O(1) index, or return nullptr.
ContactManifold *find_manifold(PhysicsShapeStore &store, Entity entityA,
                               Entity entityB) noexcept {
  std::size_t bucket = manifold_bucket(manifold_pair_key(entityA, entityB));
  for (std::size_t probe = 0U; probe < kManifoldHashBuckets; ++probe) {
    const std::uint32_t slot = store.contactManifoldHash[bucket];
    if (slot == kManifoldSlotEmpty) {
      return nullptr;
    }
    if (slot < store.contactManifoldCount) {
      ContactManifold &m = store.contactManifolds[slot];
      if (((m.entityA == entityA) && (m.entityB == entityB)) ||
          ((m.entityA == entityB) && (m.entityB == entityA))) {
        return &m;
      }
    }
    bucket = (bucket + 1U) % kManifoldHashBuckets;
  }
  return nullptr;
}

// Allocate a new manifold slot, or evict the oldest (lowest lastFrameUsed)
// if full; the caller assigns the pair and must then index the slot (the
// full-replacement path rebuilds so dead keys never accumulate). Returns
// nullptr when every slot is live this frame — those pairs cold-start
// rather than thrashing same-frame evictions, and the saturation stamp
// caps the oldest-scan at once per resolve.
ContactManifold *allocate_manifold(PhysicsShapeStore &store,
                                   std::uint32_t frameNumber,
                                   bool *outNeedsRebuild) noexcept {
  *outNeedsRebuild = false;
  if (store.contactManifoldCount < kMaxContactManifolds) {
    ContactManifold *m = &store.contactManifolds[store.contactManifoldCount];
    ++store.contactManifoldCount;
    return m;
  }
  if (store.contactManifoldSaturatedFrame == frameNumber) {
    return nullptr;
  }

  std::size_t oldestIdx = 0U;
  std::uint32_t oldestFrame = store.contactManifolds[0U].lastFrameUsed;
  for (std::size_t i = 1U; i < store.contactManifoldCount; ++i) {
    if (store.contactManifolds[i].lastFrameUsed < oldestFrame) {
      oldestFrame = store.contactManifolds[i].lastFrameUsed;
      oldestIdx = i;
    }
  }
  if (oldestFrame >= frameNumber) {
    store.contactManifoldSaturatedFrame = frameNumber;
    return nullptr;
  }

  store.contactManifolds[oldestIdx] = ContactManifold{};
  *outNeedsRebuild = true;
  return &store.contactManifolds[oldestIdx];
}

// Feature-ID based contact matching threshold.
constexpr float kContactMatchDistSq = 0.01F; // 0.1 units

// Find the best matching existing contact by feature ID first, then by
// proximity; returns contactCount when nothing matches.
std::size_t find_matching_contact(const ContactManifold &m,
                                  const math::Vec3 &pointOnA,
                                  std::uint32_t featureId) noexcept {
  for (std::size_t i = 0U; i < m.contactCount; ++i) {
    if (m.contacts[i].featureId == featureId && featureId != 0U) {
      return i;
    }
  }

  float bestDistSq = kContactMatchDistSq;
  std::size_t bestIdx = m.contactCount;
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

// Manifold reduction: keep at most 4 contacts that maximize contact area —
// the deepest, the farthest from it, then the largest-triangle and
// largest-quadrilateral additions.
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

  float maxDistSq = -1.0F;
  std::size_t secondIdx = 0U;
  for (std::size_t i = 0U; i < n; ++i) {
    if (used[i]) {
      continue;
    }
    const math::Vec3 d = math::sub(em.contacts[i].pointOnA, kept[0U].pointOnA);
    const float distSq = math::dot(d, d);
    if (distSq > maxDistSq) {
      maxDistSq = distSq;
      secondIdx = i;
    }
  }
  kept[1U] = em.contacts[secondIdx];
  used[secondIdx] = true;

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

  float maxArea2 = -1.0F;
  std::size_t fourthIdx = 0U;
  for (std::size_t i = 0U; i < n; ++i) {
    if (used[i]) {
      continue;
    }
    const math::Vec3 e0 = math::sub(em.contacts[i].pointOnA, kept[0U].pointOnA);
    const math::Vec3 e1 = math::sub(em.contacts[i].pointOnA, kept[1U].pointOnA);
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

  for (std::size_t i = 0U; i < ContactManifold::kMaxContacts; ++i) {
    m.contacts[i] = kept[i];
  }
  m.contactCount = ContactManifold::kMaxContacts;
}

} // namespace

ContactManifold *manifold_acquire(PhysicsContext &context, Entity entityA,
                                  Entity entityB,
                                  std::uint32_t frameNumber) noexcept {
  PhysicsShapeStore *store = manifold_store(context);
  if (store == nullptr) {
    return nullptr;
  }
  ContactManifold *m = find_manifold(*store, entityA, entityB);
  if (m == nullptr) {
    bool needsRebuild = false;
    m = allocate_manifold(*store, frameNumber, &needsRebuild);
    if (m == nullptr) {
      return nullptr;
    }
    m->entityA = entityA;
    m->entityB = entityB;
    m->contactCount = 0U;
    if (needsRebuild) {
      manifold_index_rebuild(*store);
    } else {
      manifold_index_insert(
          *store,
          static_cast<std::uint32_t>(m - store->contactManifolds.data()));
    }
  } else if (!(m->entityA == entityA)) {
    // Dense reorder flipped the pair's perspective: stored points and
    // impulses are mirrored, so restart the manifold in the new order.
    m->entityA = entityA;
    m->entityB = entityB;
    m->contactCount = 0U;
  }
  m->lastFrameUsed = frameNumber;
  return m;
}

std::size_t manifold_add_contact(PhysicsContext &context, Entity entityA,
                                 Entity entityB, const math::Vec3 &pointOnA,
                                 const math::Vec3 &pointOnB,
                                 const math::Vec3 &normal, float penetration,
                                 std::uint32_t featureId,
                                 std::uint32_t frameNumber) noexcept {
  PhysicsShapeStore *store = manifold_store(context);
  if (store == nullptr) {
    return kMaxContactManifolds;
  }
  ContactManifold *m = manifold_acquire(context, entityA, entityB, frameNumber);
  if (m == nullptr) {
    return kMaxContactManifolds;
  }

  const std::size_t matchIdx = find_matching_contact(*m, pointOnA, featureId);
  if (matchIdx < m->contactCount) {
    ManifoldContact &c = m->contacts[matchIdx];
    c.pointOnA = pointOnA;
    c.pointOnB = pointOnB;
    c.normal = normal;
    c.penetration = penetration;
    c.featureId = featureId;
  } else if (m->contactCount < ContactManifold::kMaxContacts) {
    ManifoldContact &c = m->contacts[m->contactCount];
    c.pointOnA = pointOnA;
    c.pointOnB = pointOnB;
    c.normal = normal;
    c.penetration = penetration;
    c.accumulatedNormalImpulse = 0.0F;
    c.featureId = featureId;
    ++m->contactCount;
  } else {
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

  return static_cast<std::size_t>(m - store->contactManifolds.data());
}

void manifold_evict_stale(PhysicsContext &context,
                          std::uint32_t frameNumber) noexcept {
  PhysicsShapeStore *store = manifold_store(context);
  if (store == nullptr) {
    return;
  }
  std::size_t writeIdx = 0U;
  for (std::size_t i = 0U; i < store->contactManifoldCount; ++i) {
    if (store->contactManifolds[i].lastFrameUsed >= frameNumber) {
      if (writeIdx != i) {
        store->contactManifolds[writeIdx] = store->contactManifolds[i];
      }
      ++writeIdx;
    }
  }
  if (writeIdx != store->contactManifoldCount) {
    store->contactManifoldCount = writeIdx;
    manifold_index_rebuild(*store);
  }
}

std::size_t manifold_count(const PhysicsContext &context) noexcept {
  const PhysicsShapeStore *store = context.shapeStore.get();
  return (store != nullptr) ? store->contactManifoldCount : 0U;
}

void manifold_reset(PhysicsContext &context) noexcept {
  PhysicsShapeStore *store = manifold_store(context);
  if (store == nullptr) {
    return;
  }
  for (std::size_t i = 0U; i < store->contactManifoldCount; ++i) {
    store->contactManifolds[i] = ContactManifold{};
  }
  store->contactManifoldCount = 0U;
  store->contactManifoldHash.fill(kManifoldSlotEmpty);
  store->contactManifoldSaturatedFrame = 0U;
}

const ContactManifold *manifold_get(const PhysicsContext &context,
                                    std::size_t index) noexcept {
  const PhysicsShapeStore *store = context.shapeStore.get();
  if ((store == nullptr) || (index >= store->contactManifoldCount)) {
    return nullptr;
  }
  return &store->contactManifolds[index];
}

} // namespace engine::physics
