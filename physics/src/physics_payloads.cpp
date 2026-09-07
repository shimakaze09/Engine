// Implements physics cvar registration, PhysicsContext lifetime, and the
// World-owned collider shape payloads (convex hulls and heightfields).

#include "engine/physics/physics.h"

#include <algorithm>
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
  if (!cvar_exists("physics.contact_relaxation_iterations")) {
    ok = core::cvar_register_int(
             "physics.contact_relaxation_iterations", 4,
             "Extra outer passes over the contact-manifold cache per step, "
             "propagating corrections through contact chains so resting "
             "stacks converge and sleep (0 disables)") &&
         ok;
  }
  return ok;
}

void refresh_step_cvar_cache(PhysicsContext &context) noexcept {
  context.ccdThresholdCvar = core::cvar_get_float("physics.ccd_threshold", 2.0F);
  context.solverIterationsCvar = core::cvar_get_int("physics.solver_iterations");
  context.blockedWarnStepsCvar =
      core::cvar_get_float("physics.blocked_warn_steps", 30.0F);
  context.contactRelaxationIterationsCvar =
      core::cvar_get_int("physics.contact_relaxation_iterations", 4);
}

PhysicsContext::PhysicsContext() noexcept
    : shapeStore(new (std::nothrow) PhysicsShapeStore()) {}

// Out-of-line where ResolveScratch is complete (#170); moves transfer the
// scratch with the context, the destructor frees it with the World.
PhysicsContext::PhysicsContext(PhysicsContext &&other) noexcept = default;
PhysicsContext &
PhysicsContext::operator=(PhysicsContext &&other) noexcept = default;
PhysicsContext::~PhysicsContext() = default;

PhysicsContext::PhysicsContext(const PhysicsContext &other) noexcept
    : PhysicsContext() {
  *this = other;
}

PhysicsContext &
PhysicsContext::operator=(const PhysicsContext &other) noexcept {
  if (this == &other) {
    return *this;
  }

  // collisionDispatch is deliberately not copied: it is run-tier state the
  // engine installs on the live World once per run, not world content. A
  // scene commit assigns a freshly staged World over the live one, and a
  // staged World never carries a dispatch, so copying it here would silently
  // detach every collision callback on each scene load or editor Stop
  // restore. The destination keeps whatever dispatch its owner installed.
  gravity = other.gravity;
  jointCount = other.jointCount;
  collisionPairData = other.collisionPairData;
  collisionPairCount = other.collisionPairCount;
  frameCollisionPairData = other.frameCollisionPairData;
  frameCollisionPairCount = other.frameCollisionPairCount;
  frameCollisionPairDropCount = other.frameCollisionPairDropCount;
  pairHashKeys = other.pairHashKeys;
  pairHashStamps = other.pairHashStamps;
  pairHashGeneration = other.pairHashGeneration;
  testedGeneration = other.testedGeneration;
  ccdColliderCount = other.ccdColliderCount;
  ccdHasCompoundColliders = other.ccdHasCompoundColliders;
  ccdSnapshotDirty = other.ccdSnapshotDirty;
  solverFrameNumber = other.solverFrameNumber;
  broadphaseOverflowActive = other.broadphaseOverflowActive;
  broadphaseOverflowEpisodes = other.broadphaseOverflowEpisodes;
  collisionPairDropCount = other.collisionPairDropCount;
  collisionPairOverflowActive = other.collisionPairOverflowActive;
  collisionPairOverflowEpisodes = other.collisionPairOverflowEpisodes;
  ccdThresholdCvar = other.ccdThresholdCvar;
  blockedWarnStepsCvar = other.blockedWarnStepsCvar;
  solverIterationsCvar = other.solverIterationsCvar;
  contactRelaxationIterationsCvar = other.contactRelaxationIterationsCvar;

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

namespace {

bool vec3_is_finite(const math::Vec3 &value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

/// Squared-length slack for a plane normal to count as unit length. A
/// float normalize lands within a few ulp (about 5e-7) of 1; this admits
/// hand-authored normals normalized in float precision and rejects a zero
/// or scaled normal by three orders of magnitude.
constexpr float kUnitNormalToleranceSq = 1.0e-3F;

/// Slack, relative to the plane offset's magnitude, for a hull vertex to
/// count as on or behind a face plane. The builder places face vertices
/// on their plane to float precision and keeps every other vertex inside
/// by its 1e-6 visibility margin, so this only rejects planes that do not
/// actually bound the vertex set.
constexpr float kPlaneContainmentTolerance = 1.0e-3F;

} // namespace

/// The payload contract every consumer (support mapping, face-plane ray
/// tests, contact clipping) relies on: counts inside the fixed tables,
/// every active vertex and plane finite, every plane normal unit length
/// with every active vertex on or behind it, and finite non-negative local
/// bounds. A payload failing any of it is refused whole, before it can
/// replace the previous valid one.
bool validate_convex_hull_data(const ConvexHullData &hull) noexcept {
  if ((hull.vertexCount == 0U) ||
      (hull.vertexCount > ConvexHullData::kMaxVertices) ||
      (hull.planeCount == 0U) || (hull.planeCount > ConvexHullData::kMaxPlanes)) {
    return false;
  }

  for (std::size_t index = 0U; index < hull.vertexCount; ++index) {
    if (!vec3_is_finite(hull.vertices[index])) {
      return false;
    }
  }

  for (std::size_t planeIndex = 0U; planeIndex < hull.planeCount;
       ++planeIndex) {
    const ConvexHullData::Plane &plane = hull.planes[planeIndex];
    if (!vec3_is_finite(plane.normal) || !std::isfinite(plane.distance)) {
      return false;
    }
    const float lengthSq = math::dot(plane.normal, plane.normal);
    if (std::fabs(lengthSq - 1.0F) > kUnitNormalToleranceSq) {
      return false;
    }
    const float slack =
        kPlaneContainmentTolerance * std::max(1.0F, std::fabs(plane.distance));
    for (std::size_t index = 0U; index < hull.vertexCount; ++index) {
      if ((math::dot(plane.normal, hull.vertices[index]) - plane.distance) >
          slack) {
        return false;
      }
    }
  }

  if (!vec3_is_finite(hull.localCenter) ||
      !vec3_is_finite(hull.localHalfExtents) ||
      (hull.localHalfExtents.x < 0.0F) || (hull.localHalfExtents.y < 0.0F) ||
      (hull.localHalfExtents.z < 0.0F)) {
    return false;
  }
  return true;
}

/// The heightfield contract: grid dimensions inside the fixed sample
/// table, finite positive spacing (a NaN spacing passes an ordering
/// comparison, so finiteness is checked explicitly), finite ordered
/// minY/maxY, and every active sample finite. Refused whole, before it
/// can replace the previous valid payload.
bool validate_heightfield_data(const HeightfieldData &heightfield) noexcept {
  if ((heightfield.rows < 2U) || (heightfield.columns < 2U) ||
      (heightfield.rows > HeightfieldData::kMaxResolution) ||
      (heightfield.columns > HeightfieldData::kMaxResolution)) {
    return false;
  }
  if (!std::isfinite(heightfield.spacingX) ||
      !std::isfinite(heightfield.spacingZ) ||
      !(heightfield.spacingX > 0.0F) || !(heightfield.spacingZ > 0.0F)) {
    return false;
  }
  if (!std::isfinite(heightfield.minY) || !std::isfinite(heightfield.maxY) ||
      (heightfield.minY > heightfield.maxY)) {
    return false;
  }
  if (heightfield.rows > (HeightfieldData::kMaxSamples / heightfield.columns)) {
    return false;
  }

  const std::size_t sampleCount = heightfield.rows * heightfield.columns;
  for (std::size_t index = 0U; index < sampleCount; ++index) {
    if (!std::isfinite(heightfield.heights[index])) {
      return false;
    }
  }
  return true;
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

void prune_incompatible_shape_payloads(PhysicsContext &context, Entity entity,
                                       ColliderShape shape) noexcept {
  if (shape != ColliderShape::ConvexHull) {
    remove_hull_data(context, entity);
  }
  if (shape != ColliderShape::Heightfield) {
    remove_heightfield_data(context, entity);
  }
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
