// Implements physics cvar registration, PhysicsContext lifetime, and the
// World-owned collider shape payloads (convex hulls and heightfields).

#include "engine/physics/physics.h"

#include <cmath>
#include <cstddef>
#include <cstring>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/physics/physics_context.h"
#include "physics_internal.h"

namespace engine::physics {

namespace {

bool cvar_exists(const char *name) noexcept {
  if (name == nullptr) {
    return false;
  }

  core::CVarInfo infos[256] = {};
  const std::size_t count = core::cvar_get_all(infos, 256U);
  for (std::size_t i = 0U; i < count; ++i) {
    if ((infos[i].name != nullptr) && (std::strcmp(infos[i].name, name) == 0)) {
      return true;
    }
  }
  return false;
}

} // namespace

bool register_physics_cvars() noexcept {
  bool ok = true;
  if (!cvar_exists("physics.solver_iterations")) {
    ok = core::cvar_register_int("physics.solver_iterations", 8,
                                 "Number of constraint solver iterations") &&
         ok;
  }
  if (!cvar_exists("physics.ccd_threshold")) {
    ok = core::cvar_register_float(
             "physics.ccd_threshold", 2.0F,
             "Minimum velocity magnitude (m/s) to trigger CCD") &&
         ok;
  }
  if (!cvar_exists("physics.blocked_warn_steps")) {
    ok = core::cvar_register_float(
             "physics.blocked_warn_steps", 30.0F,
             "Consecutive blocked steps before warning that a "
             "velocity-driven body is not moving (0 disables)") &&
         ok;
  }
  return ok;
}

PhysicsContext::PhysicsContext() noexcept
    : shapeStore(new (std::nothrow) PhysicsShapeStore()) {}

PhysicsContext::PhysicsContext(const PhysicsContext &other) noexcept
    : PhysicsContext() {
  *this = other;
}

PhysicsContext &
PhysicsContext::operator=(const PhysicsContext &other) noexcept {
  if (this == &other) {
    return *this;
  }

  gravity = other.gravity;
  joints = other.joints;
  jointCount = other.jointCount;
  collisionPairData = other.collisionPairData;
  collisionPairCount = other.collisionPairCount;
  collisionDispatch = other.collisionDispatch;
  pairHashKeys = other.pairHashKeys;
  pairHashStamps = other.pairHashStamps;
  pairHashGeneration = other.pairHashGeneration;
  testedStamps = other.testedStamps;
  testedGeneration = other.testedGeneration;
  ccdColliderCount = other.ccdColliderCount;
  ccdHasCompoundColliders = other.ccdHasCompoundColliders;

  if (other.shapeStore == nullptr) {
    shapeStore.reset();
  } else {
    if (shapeStore == nullptr) {
      shapeStore.reset(new (std::nothrow) PhysicsShapeStore());
    }
    if (shapeStore != nullptr) {
      *shapeStore = *other.shapeStore;
    }
  }

  return *this;
}

/// Finds the matching object or resource for hull data.
ConvexHullData *find_hull_data(PhysicsContext &context,
                               Entity entity) noexcept {
  PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return nullptr;
  }
  for (std::size_t i = 0U; i < store->convexHullCount; ++i) {
    if (store->convexHullEntity[i] == entity) {
      return &store->convexHullData[i];
    }
  }
  return nullptr;
}

/// Finds the matching object or resource for hull data.
const ConvexHullData *find_hull_data(const PhysicsContext &context,
                                     Entity entity) noexcept {
  const PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return nullptr;
  }
  for (std::size_t i = 0U; i < store->convexHullCount; ++i) {
    if (store->convexHullEntity[i] == entity) {
      return &store->convexHullData[i];
    }
  }
  return nullptr;
}

ConvexHullData *allocate_hull_data(PhysicsContext &context,
                                   Entity entity) noexcept {
  ConvexHullData *existing = find_hull_data(context, entity);
  if (existing != nullptr) {
    return existing;
  }
  PhysicsShapeStore *store = context.shapeStore.get();
  if ((store == nullptr) || (store->convexHullCount >= kMaxConvexHulls)) {
    return nullptr;
  }
  store->convexHullEntity[store->convexHullCount] = entity;
  store->convexHullData[store->convexHullCount] = ConvexHullData{};
  return &store->convexHullData[store->convexHullCount++];
}

/// Finds the matching object or resource for heightfield data.
HeightfieldData *find_heightfield_data(PhysicsContext &context,
                                       Entity entity) noexcept {
  PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return nullptr;
  }
  for (std::size_t i = 0U; i < store->heightfieldCount; ++i) {
    if (store->heightfieldEntity[i] == entity) {
      return &store->heightfieldData[i];
    }
  }
  return nullptr;
}

/// Finds the matching object or resource for heightfield data.
const HeightfieldData *find_heightfield_data(const PhysicsContext &context,
                                             Entity entity) noexcept {
  const PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return nullptr;
  }
  for (std::size_t i = 0U; i < store->heightfieldCount; ++i) {
    if (store->heightfieldEntity[i] == entity) {
      return &store->heightfieldData[i];
    }
  }
  return nullptr;
}

HeightfieldData *allocate_heightfield_data(PhysicsContext &context,
                                           Entity entity) noexcept {
  HeightfieldData *existing = find_heightfield_data(context, entity);
  if (existing != nullptr) {
    return existing;
  }
  PhysicsShapeStore *store = context.shapeStore.get();
  if ((store == nullptr) || (store->heightfieldCount >= kMaxHeightfields)) {
    return nullptr;
  }
  store->heightfieldEntity[store->heightfieldCount] = entity;
  store->heightfieldData[store->heightfieldCount] = HeightfieldData{};
  return &store->heightfieldData[store->heightfieldCount++];
}

void remove_hull_data(PhysicsContext &context, Entity entity) noexcept {
  PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return;
  }
  for (std::size_t i = 0U; i < store->convexHullCount; ++i) {
    if (store->convexHullEntity[i] == entity) {
      const std::size_t last = store->convexHullCount - 1U;
      if (i != last) {
        store->convexHullData[i] = store->convexHullData[last];
        store->convexHullEntity[i] = store->convexHullEntity[last];
      }
      store->convexHullData[last] = ConvexHullData{};
      store->convexHullEntity[last] = kInvalidEntity;
      --store->convexHullCount;
      return;
    }
  }
}

void remove_heightfield_data(PhysicsContext &context, Entity entity) noexcept {
  PhysicsShapeStore *store = context.shapeStore.get();
  if (store == nullptr) {
    return;
  }
  for (std::size_t i = 0U; i < store->heightfieldCount; ++i) {
    if (store->heightfieldEntity[i] == entity) {
      const std::size_t last = store->heightfieldCount - 1U;
      if (i != last) {
        store->heightfieldData[i] = store->heightfieldData[last];
        store->heightfieldEntity[i] = store->heightfieldEntity[last];
      }
      store->heightfieldData[last] = HeightfieldData{};
      store->heightfieldEntity[last] = kInvalidEntity;
      --store->heightfieldCount;
      return;
    }
  }
}

bool validate_convex_hull_data(const ConvexHullData &hull) noexcept {
  return (hull.vertexCount > 0U) &&
         (hull.vertexCount <= ConvexHullData::kMaxVertices) &&
         (hull.planeCount > 0U) &&
         (hull.planeCount <= ConvexHullData::kMaxPlanes);
}

bool validate_heightfield_data(const HeightfieldData &heightfield) noexcept {
  if ((heightfield.rows < 2U) || (heightfield.columns < 2U) ||
      (heightfield.rows > HeightfieldData::kMaxResolution) ||
      (heightfield.columns > HeightfieldData::kMaxResolution) ||
      (heightfield.spacingX <= 0.0F) || (heightfield.spacingZ <= 0.0F)) {
    return false;
  }

  return heightfield.rows <=
         (HeightfieldData::kMaxSamples / heightfield.columns);
}

// Public accessors used by the runtime bridge.
bool set_convex_hull_data(PhysicsContext &context, Entity entity,
                          const ConvexHullData &hull) noexcept {
  if (!validate_convex_hull_data(hull)) {
    return false;
  }

  ConvexHullData *slot = allocate_hull_data(context, entity);
  if (slot == nullptr) {
    return false;
  }
  *slot = hull;
  return true;
}

const ConvexHullData *get_convex_hull_data(const PhysicsContext &context,
                                           Entity entity) noexcept {
  return find_hull_data(context, entity);
}

void remove_shape_payloads(PhysicsContext &context, Entity entity) noexcept {
  remove_hull_data(context, entity);
  remove_heightfield_data(context, entity);
}

/// Sets the requested value for heightfield data impl.
bool set_heightfield_data(PhysicsContext &context, Entity entity,
                          const HeightfieldData &hf) noexcept {
  if (!validate_heightfield_data(hf)) {
    return false;
  }

  HeightfieldData *slot = allocate_heightfield_data(context, entity);
  if (slot == nullptr) {
    return false;
  }
  *slot = hf;
  return true;
}

const HeightfieldData *get_heightfield_data(const PhysicsContext &context,
                                            Entity entity) noexcept {
  return find_heightfield_data(context, entity);
}

const ConvexHullData *get_hull_data_ptr(const PhysicsContext &context,
                                        Entity entity) noexcept {
  return find_hull_data(context, entity);
}


} // namespace engine::physics
