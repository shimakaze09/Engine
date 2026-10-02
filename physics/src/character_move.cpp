// Implements the kinematic character move: GJK closest points between the
// capsule's core segment and each shape it meets (exact contact normals,
// where the capsule sweep's own normal is only center to center), and the
// collide-and-slide built on them: depenetration, swept slides, step-up,
// steep-slope walls and the ground probe. Everything runs serially over the
// world's colliders, so a move is deterministic for a given world.

#include "engine/physics/character_move.h"

#include <cmath>
#include <cstddef>

#include "engine/physics/convex_hull.h"
#include "engine/physics/physics_query.h"
#include "engine/physics/physics_world_view.h"
#include "physics_internal.h"

namespace engine::physics {

namespace {

// ------------------------------------------------------------------------
// GJK closest points between a segment and a convex shape.

/// One vertex of the Minkowski difference segment - shape, with the two
/// points it came from so the closest points can be recovered.
struct GjkVertex final {
  math::Vec3 w{};
  math::Vec3 onSegment{};
  math::Vec3 onShape{};
};

struct GjkSimplex final {
  GjkVertex vertices[4]{};
  float weights[4]{};
  std::size_t count = 0U;
};

/// Keeps the vertices whose weight is positive, in order.
void compact(GjkSimplex &simplex) noexcept {
  std::size_t kept = 0U;
  for (std::size_t i = 0U; i < simplex.count; ++i) {
    if (simplex.weights[i] > 0.0F) {
      simplex.vertices[kept] = simplex.vertices[i];
      simplex.weights[kept] = simplex.weights[i];
      ++kept;
    }
  }
  simplex.count = kept;
}

/// Weights of the point of segment [a, b] closest to the origin.
void closest_on_segment(const math::Vec3 &a, const math::Vec3 &b, float *wa,
                        float *wb) noexcept {
  const math::Vec3 ab = math::sub(b, a);
  const float lengthSq = math::length_sq(ab);
  float t = (lengthSq > 0.0F) ? (-math::dot(a, ab) / lengthSq) : 0.0F;
  t = std::clamp(t, 0.0F, 1.0F);
  *wa = 1.0F - t;
  *wb = t;
}

/// Weights of the point of triangle abc closest to the origin (Ericson,
/// Real-Time Collision Detection, 5.1.5).
void closest_on_triangle(const math::Vec3 &a, const math::Vec3 &b,
                         const math::Vec3 &c, float weights[3]) noexcept {
  const math::Vec3 ab = math::sub(b, a);
  const math::Vec3 ac = math::sub(c, a);
  const math::Vec3 ap = math::mul(a, -1.0F);
  const float d1 = math::dot(ab, ap);
  const float d2 = math::dot(ac, ap);
  if ((d1 <= 0.0F) && (d2 <= 0.0F)) {
    weights[0] = 1.0F;
    weights[1] = 0.0F;
    weights[2] = 0.0F;
    return;
  }
  const math::Vec3 bp = math::mul(b, -1.0F);
  const float d3 = math::dot(ab, bp);
  const float d4 = math::dot(ac, bp);
  if ((d3 >= 0.0F) && (d4 <= d3)) {
    weights[0] = 0.0F;
    weights[1] = 1.0F;
    weights[2] = 0.0F;
    return;
  }
  const float vc = (d1 * d4) - (d3 * d2);
  if ((vc <= 0.0F) && (d1 >= 0.0F) && (d3 <= 0.0F)) {
    const float v = d1 / (d1 - d3);
    weights[0] = 1.0F - v;
    weights[1] = v;
    weights[2] = 0.0F;
    return;
  }
  const math::Vec3 cp = math::mul(c, -1.0F);
  const float d5 = math::dot(ab, cp);
  const float d6 = math::dot(ac, cp);
  if ((d6 >= 0.0F) && (d5 <= d6)) {
    weights[0] = 0.0F;
    weights[1] = 0.0F;
    weights[2] = 1.0F;
    return;
  }
  const float vb = (d5 * d2) - (d1 * d6);
  if ((vb <= 0.0F) && (d2 >= 0.0F) && (d6 <= 0.0F)) {
    const float w = d2 / (d2 - d6);
    weights[0] = 1.0F - w;
    weights[1] = 0.0F;
    weights[2] = w;
    return;
  }
  const float va = (d3 * d6) - (d5 * d4);
  if ((va <= 0.0F) && ((d4 - d3) >= 0.0F) && ((d5 - d6) >= 0.0F)) {
    const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    weights[0] = 0.0F;
    weights[1] = 1.0F - w;
    weights[2] = w;
    return;
  }
  const float denominator = 1.0F / (va + vb + vc);
  const float v = vb * denominator;
  const float w = vc * denominator;
  weights[0] = 1.0F - v - w;
  weights[1] = v;
  weights[2] = w;
}

math::Vec3 weighted_point(const GjkSimplex &simplex) noexcept {
  math::Vec3 point(0.0F, 0.0F, 0.0F);
  for (std::size_t i = 0U; i < simplex.count; ++i) {
    point =
        math::add(point, math::mul(simplex.vertices[i].w, simplex.weights[i]));
  }
  return point;
}

/// Reduces the simplex to the vertices supporting its point closest to the
/// origin and weights them. True when the origin lies inside it.
bool solve_simplex(GjkSimplex &simplex) noexcept {
  GjkVertex *v = simplex.vertices;
  switch (simplex.count) {
  case 1U:
    simplex.weights[0] = 1.0F;
    return false;
  case 2U:
    closest_on_segment(v[0].w, v[1].w, &simplex.weights[0],
                       &simplex.weights[1]);
    compact(simplex);
    return false;
  case 3U:
    closest_on_triangle(v[0].w, v[1].w, v[2].w, simplex.weights);
    compact(simplex);
    return false;
  default:
    break;
  }
  // A tetrahedron: the origin is inside unless some face separates it from
  // the opposite vertex; the closest such face wins.
  constexpr std::size_t kFaces[4][4] = {
      {0U, 1U, 2U, 3U}, {0U, 2U, 3U, 1U}, {0U, 3U, 1U, 2U}, {1U, 3U, 2U, 0U}};
  bool outside = false;
  float bestDistanceSq = 0.0F;
  GjkSimplex best{};
  for (const auto &face : kFaces) {
    const math::Vec3 &a = v[face[0]].w;
    const math::Vec3 &b = v[face[1]].w;
    const math::Vec3 &c = v[face[2]].w;
    const math::Vec3 &d = v[face[3]].w;
    const math::Vec3 normal = math::cross(math::sub(b, a), math::sub(c, a));
    const float originSide = math::dot(math::mul(a, -1.0F), normal);
    const float vertexSide = math::dot(math::sub(d, a), normal);
    // A flat tetrahedron decides nothing by sign; every face is tried.
    const bool separates = (std::fabs(vertexSide) <= 1.0e-12F) ||
                           ((originSide * vertexSide) < 0.0F);
    if (!separates) {
      continue;
    }
    GjkSimplex candidate{};
    candidate.vertices[0] = v[face[0]];
    candidate.vertices[1] = v[face[1]];
    candidate.vertices[2] = v[face[2]];
    candidate.count = 3U;
    closest_on_triangle(a, b, c, candidate.weights);
    compact(candidate);
    const float distanceSq = math::length_sq(weighted_point(candidate));
    if (!outside || (distanceSq < bestDistanceSq)) {
      outside = true;
      bestDistanceSq = distanceSq;
      best = candidate;
    }
  }
  if (!outside) {
    return true;
  }
  simplex = best;
  return false;
}

/// The segment endpoint farthest along `direction` (the first on a tie).
math::Vec3 segment_support(const math::Vec3 &a, const math::Vec3 &b,
                           const math::Vec3 &direction) noexcept {
  return (math::dot(b, direction) > math::dot(a, direction)) ? b : a;
}

// ------------------------------------------------------------------------
// Character move.

constexpr int kMaxSlides = 4;
constexpr int kMaxDepenetrations = 4;
/// Moves shorter than this are not swept.
constexpr float kMinMove = 1.0e-5F;
/// A sweep hit this close to the start is a surface the capsule touches.
constexpr float kTouchTravel = 1.0e-4F;
/// Moves within this cosine of a touched surface's plane run along it.
constexpr float kParallelCosine = 1.0e-3F;
/// How far past a contact, and from how high, the surface probe looks for
/// the face under it.
constexpr float kSurfaceProbeInset = 0.01F;
constexpr float kSurfaceProbeHeight = 0.05F;
/// The least |cos| between a move and a surface normal used to turn the
/// skin gap along the normal into a back-off along the move.
constexpr float kMinBackOffCosine = 0.05F;

const math::Vec3 kUp(0.0F, 1.0F, 0.0F);

CharacterCapsule moved(const CharacterCapsule &capsule,
                       const math::Vec3 &offset) noexcept {
  return CharacterCapsule{math::add(capsule.bottom, offset),
                          math::add(capsule.top, offset), capsule.radius};
}

/// Whether a collider blocks the character: not a trigger, not its own or
/// one its entity owns, and one the pair rule lets it collide with.
bool blocks(const PhysicsWorldView &world,
            const CharacterMoveSettings &settings, Entity entity,
            const Collider &collider) noexcept {
  if (collider.isTrigger || (entity == settings.self) ||
      ((settings.self != kInvalidEntity) &&
       (world.rigid_body_owner(entity) == settings.self))) {
    return false;
  }
  return colliders_may_collide(settings.selfCollider, collider,
                               world.physics_context().collisionMatrix);
}

/// A contact between the capsule and one shape: the gap between their
/// surfaces (negative while they overlap) and the normal pushing the
/// capsule away, with the closest point on the capsule's core.
struct Contact final {
  Entity entity = kInvalidEntity;
  float separation = 0.0F;
  math::Vec3 normal{};
  math::Vec3 onCore{};
  math::Vec3 onShape{};
};

/// Measures the contact between the capsule and `shape`. A core that
/// reaches inside the shape takes EPA's penetration instead.
bool measure_contact(const CharacterCapsule &capsule,
                     const ColliderWorldGeometry &shape,
                     Contact *out) noexcept {
  SegmentConvexDistance distance{};
  if (!segment_convex_distance(capsule.bottom, capsule.top, shape, &distance)) {
    return false;
  }
  if (!distance.intersecting && (distance.distance > 1.0e-6F)) {
    out->separation = distance.distance - capsule.radius;
    out->normal = math::mul(math::sub(distance.onSegment, distance.onShape),
                            1.0F / distance.distance);
    out->onCore = distance.onSegment;
    out->onShape = distance.onShape;
    return true;
  }
  ColliderWorldGeometry capsuleGeometry{};
  if (!capsule_query_geometry(capsule.bottom, capsule.top, capsule.radius,
                              &capsuleGeometry)) {
    return false;
  }
  const auto support = [](const void *data, const math::Vec3 &,
                          const math::Vec3 &direction) noexcept {
    return collider_support_point(
        *static_cast<const ColliderWorldGeometry *>(data), direction);
  };
  const GjkResult penetration =
      gjk_epa(&capsuleGeometry, capsuleGeometry.center, support, &shape,
              shape.center, support);
  if (!penetration.intersecting ||
      (math::length_sq(penetration.normal) <= 1.0e-12F)) {
    // The core touches the surface exactly; push straight up and out.
    out->separation = -capsule.radius;
    out->normal = kUp;
    out->onCore = distance.onSegment;
    out->onShape = distance.onShape;
    return true;
  }
  out->separation = -penetration.depth;
  out->normal = math::mul(math::normalize(penetration.normal), -1.0F);
  out->onCore = distance.onSegment;
  out->onShape = penetration.contactPoint;
  return true;
}

/// The capsule's world box grown by `margin`.
math::AABB capsule_bounds(const CharacterCapsule &capsule,
                          float margin) noexcept {
  const float reach = capsule.radius + margin;
  const math::Vec3 grow(reach, reach, reach);
  math::AABB box{};
  box.min = math::sub(math::Vec3(std::min(capsule.bottom.x, capsule.top.x),
                                 std::min(capsule.bottom.y, capsule.top.y),
                                 std::min(capsule.bottom.z, capsule.top.z)),
                      grow);
  box.max = math::add(math::Vec3(std::max(capsule.bottom.x, capsule.top.x),
                                 std::max(capsule.bottom.y, capsule.top.y),
                                 std::max(capsule.bottom.z, capsule.top.z)),
                      grow);
  return box;
}

/// The deepest overlap between the capsule and a blocking collider, if any.
bool deepest_overlap(const PhysicsWorldView &world,
                     const CharacterCapsule &capsule,
                     const CharacterMoveSettings &settings,
                     Contact *out) noexcept {
  const std::size_t count = world.collider_count();
  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if ((count == 0U) ||
      !world.get_collider_range(0U, count, &entities, &colliders)) {
    return false;
  }
  const math::AABB bounds = capsule_bounds(capsule, 0.0F);
  bool found = false;
  for (std::size_t i = 0U; i < count; ++i) {
    if (!blocks(world, settings, entities[i], colliders[i])) {
      continue;
    }
    ColliderWorldGeometry shape{};
    if (!world_collider_geometry(world, entities[i], colliders[i], &shape) ||
        !math::aabb_intersects(bounds, shape.worldAabb)) {
      continue;
    }
    Contact contact{};
    if (!measure_contact(capsule, shape, &contact) ||
        (contact.separation >= 0.0F)) {
      continue;
    }
    // Deepest first; equal depths go to the lower entity index, so the
    // order the world stores colliders in never decides.
    if (!found || (contact.separation < out->separation) ||
        ((contact.separation == out->separation) &&
         (entities[i].index < out->entity.index))) {
      contact.entity = entities[i];
      *out = contact;
      found = true;
    }
  }
  return found;
}

/// Pushes the capsule out of what it overlaps; returns the offset applied.
math::Vec3 depenetrate(const PhysicsWorldView &world,
                       const CharacterCapsule &capsule,
                       const CharacterMoveSettings &settings) noexcept {
  math::Vec3 offset(0.0F, 0.0F, 0.0F);
  for (int pass = 0; pass < kMaxDepenetrations; ++pass) {
    Contact contact{};
    if (!deepest_overlap(world, moved(capsule, offset), settings, &contact)) {
      break;
    }
    offset = math::add(
        offset, math::mul(contact.normal, -contact.separation + 1.0e-4F));
  }
  return offset;
}

/// The first blocking collider the capsule meets moving `distance` along
/// unit `direction`, with the exact contact there.
struct SweepContact final {
  float travel = 0.0F;
  Contact contact{};
};

bool sweep(const PhysicsWorldView &world, const CharacterCapsule &capsule,
           const math::Vec3 &direction, float distance,
           const CharacterMoveSettings &settings, SweepContact *out) noexcept {
  if (!(distance > kMinMove)) {
    return false;
  }
  ColliderWorldGeometry query{};
  if (!capsule_query_geometry(capsule.bottom, capsule.top, capsule.radius,
                              &query)) {
    return false;
  }
  const std::size_t count = world.collider_count();
  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if ((count == 0U) ||
      !world.get_collider_range(0U, count, &entities, &colliders)) {
    return false;
  }
  // Everything the capsule passes through lies in the box around its start
  // and end.
  math::AABB swept = capsule_bounds(capsule, 0.0F);
  const math::AABB end =
      capsule_bounds(moved(capsule, math::mul(direction, distance)), 0.0F);
  swept.min = math::Vec3(std::min(swept.min.x, end.min.x),
                         std::min(swept.min.y, end.min.y),
                         std::min(swept.min.z, end.min.z));
  swept.max = math::Vec3(std::max(swept.max.x, end.max.x),
                         std::max(swept.max.y, end.max.y),
                         std::max(swept.max.z, end.max.z));

  bool found = false;
  float bestT = distance;
  Entity bestEntity = kInvalidEntity;
  ColliderWorldGeometry bestShape{};
  for (std::size_t i = 0U; i < count; ++i) {
    if (!blocks(world, settings, entities[i], colliders[i])) {
      continue;
    }
    ColliderWorldGeometry shape{};
    if (!world_collider_geometry(world, entities[i], colliders[i], &shape) ||
        !math::aabb_intersects(swept, shape.worldAabb)) {
      continue;
    }
    float t = 0.0F;
    if (!sweep_convex_geometry(query, direction, distance, shape, &t)) {
      continue;
    }
    // A surface already touched blocks only a move into it: walking along
    // the floor, or away from a wall, is free.
    if (t <= kTouchTravel) {
      Contact touching{};
      if (measure_contact(capsule, shape, &touching) &&
          (math::dot(direction, touching.normal) >= -kParallelCosine)) {
        continue;
      }
    }
    // Earliest first; equal times go to the lower entity index.
    if (!found || (t < bestT) ||
        ((t == bestT) && (entities[i].index < bestEntity.index))) {
      found = true;
      bestT = t;
      bestEntity = entities[i];
      bestShape = shape;
    }
  }
  if (!found) {
    return false;
  }
  Contact contact{};
  if (!measure_contact(moved(capsule, math::mul(direction, bestT)), bestShape,
                       &contact)) {
    return false;
  }
  contact.entity = bestEntity;
  out->travel = bestT;
  out->contact = contact;
  return true;
}

/// How far to travel along `direction` to stop `skin` short of a surface
/// with `normal` first met after `hitTravel`.
float travel_before(float hitTravel, const math::Vec3 &direction,
                    const math::Vec3 &normal, float skin) noexcept {
  const float approach =
      std::max(-math::dot(direction, normal), kMinBackOffCosine);
  return std::max(hitTravel - (skin / approach), 0.0F);
}

/// Where the capsule was touched, from the contact's point on its core.
std::uint32_t contact_flag(const CharacterCapsule &capsule,
                           const Contact &contact) noexcept {
  constexpr float kEndTolerance = 1.0e-4F;
  if ((contact.onCore.y <= capsule.bottom.y + kEndTolerance) &&
      (contact.normal.y > 1.0e-3F)) {
    return kCharacterCollidedBelow;
  }
  if ((contact.onCore.y >= capsule.top.y - kEndTolerance) &&
      (contact.normal.y < -1.0e-3F)) {
    return kCharacterCollidedAbove;
  }
  return kCharacterCollidedSides;
}

bool walkable(const math::Vec3 &normal,
              const CharacterMoveSettings &settings) noexcept {
  return normal.y >= settings.slopeLimitCos;
}

/// The normal of the face a contact lies on. A capsule resting on a
/// ledge's edge touches it with a tilted normal, yet stands on the ledge's
/// top: a short ray straight down onto the touched collider, just past the
/// contact away from the capsule's axis, finds that face, as PhysX's
/// controller judges a step by the touched face rather than the contact.
math::Vec3 surface_normal(const PhysicsWorldView &world,
                          const CharacterCapsule &capsule,
                          const Contact &contact,
                          const CharacterMoveSettings &settings) noexcept {
  if (walkable(contact.normal, settings)) {
    return contact.normal;
  }
  math::Vec3 outward(contact.onShape.x - capsule.bottom.x, 0.0F,
                     contact.onShape.z - capsule.bottom.z);
  const float outwardLengthSq = math::length_sq(outward);
  if (outwardLengthSq > 1.0e-12F) {
    outward =
        math::mul(outward, kSurfaceProbeInset / std::sqrt(outwardLengthSq));
  }
  const math::Vec3 origin = math::add(math::add(contact.onShape, outward),
                                      math::mul(kUp, kSurfaceProbeHeight));
  PhysicsRaycastHit hit{};
  if (raycast(world, origin, math::mul(kUp, -1.0F), 2.0F * kSurfaceProbeHeight,
              &hit, settings.self) &&
      (hit.entity == contact.entity)) {
    return hit.normal;
  }
  return contact.normal;
}

/// Climbs a ledge no higher than the step offset: up, along `horizontal`,
/// then down onto walkable ground. On success `*offset` moves to the top
/// and the horizontal travel made is returned; on failure nothing changes.
bool try_step(const PhysicsWorldView &world, const CharacterCapsule &capsule,
              const CharacterMoveSettings &settings,
              const math::Vec3 &horizontal, math::Vec3 *offset,
              float *outTravel) noexcept {
  const float length = math::length(horizontal);
  if ((settings.stepOffset <= 0.0F) || !(length > kMinMove)) {
    return false;
  }
  const math::Vec3 forward = math::mul(horizontal, 1.0F / length);
  const float skin = settings.skinWidth;

  float climb = settings.stepOffset;
  SweepContact hit{};
  if (sweep(world, moved(capsule, *offset), kUp, settings.stepOffset, settings,
            &hit)) {
    climb = travel_before(hit.travel, kUp, hit.contact.normal, skin);
  }
  if (!(climb > kMinMove)) {
    return false;
  }
  math::Vec3 raised = math::add(*offset, math::mul(kUp, climb));

  float along = length;
  if (sweep(world, moved(capsule, raised), forward, length, settings, &hit)) {
    along = travel_before(hit.travel, forward, hit.contact.normal, skin);
  }
  if (!(along > kMinMove)) {
    return false;
  }
  raised = math::add(raised, math::mul(forward, along));

  const math::Vec3 down = math::mul(kUp, -1.0F);
  if (!sweep(world, moved(capsule, raised), down, climb + (2.0F * skin),
             settings, &hit) ||
      !walkable(
          surface_normal(
              world,
              moved(capsule, math::add(raised, math::mul(down, hit.travel))),
              hit.contact, settings),
          settings)) {
    // Nothing walkable to stand on at the top: not a step.
    return false;
  }
  const float drop = travel_before(hit.travel, down, hit.contact.normal, skin);
  *offset = math::add(raised, math::mul(down, drop));
  *outTravel = along;
  return true;
}

bool finite(const math::Vec3 &value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

} // namespace

bool segment_convex_distance(const math::Vec3 &a, const math::Vec3 &b,
                             const ColliderWorldGeometry &shape,
                             SegmentConvexDistance *out) noexcept {
  if ((out == nullptr) || !finite(a) || !finite(b)) {
    return false;
  }
  *out = SegmentConvexDistance{};
  const auto support = [&](const math::Vec3 &direction) noexcept {
    GjkVertex vertex{};
    vertex.onSegment = segment_support(a, b, direction);
    vertex.onShape = collider_support_point(shape, math::mul(direction, -1.0F));
    vertex.w = math::sub(vertex.onSegment, vertex.onShape);
    return vertex;
  };

  GjkSimplex simplex{};
  simplex.vertices[0] = support(math::sub(shape.center, a));
  simplex.weights[0] = 1.0F;
  simplex.count = 1U;
  math::Vec3 v = simplex.vertices[0].w;
  constexpr int kMaxIterations = 64;
  constexpr float kRelativeTolerance = 1.0e-6F;
  constexpr float kTouching = 1.0e-12F;
  bool intersecting = false;
  for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
    const float vLengthSq = math::length_sq(v);
    if (vLengthSq <= kTouching) {
      intersecting = true;
      break;
    }
    const GjkVertex vertex = support(math::mul(v, -1.0F));
    // No support point gets meaningfully closer: v is the answer.
    if ((vLengthSq - math::dot(v, vertex.w)) <=
        (kRelativeTolerance * vLengthSq)) {
      break;
    }
    bool repeated = false;
    for (std::size_t i = 0U; i < simplex.count; ++i) {
      repeated = repeated ||
                 (math::length_sq(math::sub(simplex.vertices[i].w, vertex.w)) <=
                  kTouching);
    }
    if (repeated || (simplex.count == 4U)) {
      break;
    }
    const GjkSimplex previous = simplex;
    simplex.vertices[simplex.count] = vertex;
    ++simplex.count;
    if (solve_simplex(simplex)) {
      intersecting = true;
      break;
    }
    const math::Vec3 next = weighted_point(simplex);
    // Distance never grows in exact arithmetic; a step that does is
    // rounding, and the previous answer stands with the simplex that gave
    // it, so the closest points still match the distance.
    if (math::length_sq(next) >= vLengthSq) {
      simplex = previous;
      break;
    }
    v = next;
  }

  math::Vec3 onSegment(0.0F, 0.0F, 0.0F);
  math::Vec3 onShape(0.0F, 0.0F, 0.0F);
  float weightSum = 0.0F;
  for (std::size_t i = 0U; i < simplex.count; ++i) {
    onSegment = math::add(onSegment, math::mul(simplex.vertices[i].onSegment,
                                               simplex.weights[i]));
    onShape = math::add(
        onShape, math::mul(simplex.vertices[i].onShape, simplex.weights[i]));
    weightSum += simplex.weights[i];
  }
  if (weightSum > 0.0F) {
    onSegment = math::mul(onSegment, 1.0F / weightSum);
    onShape = math::mul(onShape, 1.0F / weightSum);
  }
  out->intersecting = intersecting;
  out->onSegment = onSegment;
  out->onShape = intersecting ? onSegment : onShape;
  out->distance = intersecting ? 0.0F : std::sqrt(math::length_sq(v));
  return true;
}

bool move_character(const PhysicsWorldView &world,
                    const CharacterCapsule &capsule,
                    const math::Vec3 &displacement,
                    const CharacterMoveSettings &settings,
                    CharacterMoveResult *out) noexcept {
  if (out == nullptr) {
    return false;
  }
  *out = CharacterMoveResult{};
  if (!finite(capsule.bottom) || !finite(capsule.top) ||
      !finite(displacement) || !std::isfinite(capsule.radius) ||
      !(capsule.radius > 0.0F) || !std::isfinite(settings.skinWidth) ||
      !std::isfinite(settings.stepOffset) ||
      !std::isfinite(settings.slopeLimitCos)) {
    return false;
  }
  const float skin = settings.skinWidth;
  math::Vec3 offset = depenetrate(world, capsule, settings);
  std::uint32_t flags = 0U;
  bool grounded = false;

  math::Vec3 remaining = displacement;
  math::Vec3 previousNormal{};
  bool havePrevious = false;
  for (int slide = 0; slide < kMaxSlides; ++slide) {
    const float length = math::length(remaining);
    if (!(length > kMinMove)) {
      break;
    }
    const math::Vec3 direction = math::mul(remaining, 1.0F / length);
    // One skin further than the move, so a surface the move would end
    // closer to than the skin is met, and the gap kept.
    SweepContact hit{};
    if (!sweep(world, moved(capsule, offset), direction, length + skin,
               settings, &hit)) {
      offset = math::add(offset, remaining);
      remaining = math::Vec3(0.0F, 0.0F, 0.0F);
      break;
    }
    const math::Vec3 normal = hit.contact.normal;
    const float travel =
        std::min(travel_before(hit.travel, direction, normal, skin), length);
    offset = math::add(offset, math::mul(direction, travel));
    remaining = math::mul(direction, length - travel);
    const std::uint32_t flag = contact_flag(
        moved(capsule, math::mul(direction, hit.travel)), hit.contact);
    flags |= flag;
    const bool standing = settings.wasGrounded || grounded;
    if (walkable(normal, settings)) {
      grounded = grounded || (flag == kCharacterCollidedBelow);
    } else if (standing && (flag != kCharacterCollidedAbove)) {
      // A ledge low enough to step onto is climbed, not slid along.
      const float contactHeight =
          hit.contact.onShape.y -
          (capsule.bottom.y + offset.y - capsule.radius);
      const math::Vec3 horizontal(remaining.x, 0.0F, remaining.z);
      float stepped = 0.0F;
      if ((contactHeight <= settings.stepOffset + skin) &&
          try_step(world, capsule, settings, horizontal, &offset, &stepped)) {
        const float horizontalLength = math::length(horizontal);
        remaining = (horizontalLength > 0.0F)
                        ? math::mul(horizontal, (horizontalLength - stepped) /
                                                    horizontalLength)
                        : math::Vec3(0.0F, 0.0F, 0.0F);
        grounded = true;
        continue;
      }
    }

    // A slope too steep to walk is a wall to a character standing on the
    // ground: it slides along it but never up it.
    math::Vec3 slideNormal = normal;
    if (!walkable(normal, settings) && (settings.wasGrounded || grounded) &&
        (normal.y > 0.0F)) {
      const math::Vec3 flat(normal.x, 0.0F, normal.z);
      if (math::length_sq(flat) > 1.0e-8F) {
        slideNormal = math::normalize(flat);
      }
    }
    const float into = math::dot(remaining, slideNormal);
    if (into < 0.0F) {
      remaining = math::sub(remaining, math::mul(slideNormal, into));
    }
    // Between two surfaces the only way on is along their crease.
    if (havePrevious && (math::dot(remaining, previousNormal) < 0.0F)) {
      const math::Vec3 crease = math::cross(previousNormal, slideNormal);
      const float creaseLengthSq = math::length_sq(crease);
      if (creaseLengthSq > 1.0e-8F) {
        const math::Vec3 axis =
            math::mul(crease, 1.0F / std::sqrt(creaseLengthSq));
        remaining = math::mul(axis, math::dot(remaining, axis));
      } else {
        remaining = math::Vec3(0.0F, 0.0F, 0.0F);
      }
    }
    previousNormal = slideNormal;
    havePrevious = true;
  }

  // Ground: a walkable surface within the skin below, or within the step
  // offset for a character that was on the ground and is not moving up,
  // which then stays on it (Godot's floor snap).
  const bool snap = settings.wasGrounded && (displacement.y <= 0.0F);
  const float probe = (snap ? settings.stepOffset : 0.0F) + (2.0F * skin);
  const math::Vec3 down = math::mul(kUp, -1.0F);
  SweepContact below{};
  math::Vec3 groundNormal{};
  if ((displacement.y <= 0.0F) &&
      sweep(world, moved(capsule, offset), down, probe, settings, &below) &&
      walkable(
          groundNormal = surface_normal(
              world,
              moved(capsule, math::add(offset, math::mul(down, below.travel))),
              below.contact, settings),
          settings)) {
    // Down to the skin gap above the ground, or up to it when closer.
    const float drop = below.travel - (skin / std::max(below.contact.normal.y,
                                                       kMinBackOffCosine));
    if (snap || (drop <= skin)) {
      offset = math::add(offset, math::mul(down, drop));
      grounded = true;
      flags |= kCharacterCollidedBelow;
      out->ground = below.contact.entity;
      out->groundNormal = groundNormal;
    }
  }
  offset =
      math::add(offset, depenetrate(world, moved(capsule, offset), settings));

  out->translation = offset;
  out->flags = flags;
  out->grounded = grounded;
  if (!grounded) {
    out->ground = kInvalidEntity;
    out->groundNormal = math::Vec3(0.0F, 0.0F, 0.0F);
  }
  return true;
}

} // namespace engine::physics
