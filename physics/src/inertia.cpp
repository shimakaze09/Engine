// Implements the rigid-body mass-property helpers: analytic inertia of the
// collider shapes about their own centroid, the parallel-axis and rotation
// terms that place them in the body frame, and the world-space application
// of the resulting diagonal inverse tensor.

#include "engine/physics/inertia.h"

#include "engine/physics/physics.h"

#include <cmath>

namespace engine::physics {

namespace {

constexpr float kPi = 3.14159265358979323846F;
// A unit-mass inertia below the square of this extent is a degenerate
// shape on that axis; the inverse is then zero (no rotation) rather than
// unbounded.
constexpr float kMinExtent = 1.0e-4F;

/// Unit-mass inertia of the collider about its own centroid, on its own
/// axes, and its volume for the mass split. Hulls and heightfields take
/// the box of their half extents.
void shape_unit_inertia(const math::Collider &collider, math::Vec3 *outInertia,
                        float *outVolume) noexcept {
  const float hx = std::fabs(collider.halfExtents.x);
  const float hy = std::fabs(collider.halfExtents.y);
  const float hz = std::fabs(collider.halfExtents.z);
  switch (collider.shape) {
  case math::ColliderShape::Sphere: {
    const float r = hx;
    const float rr = r * r;
    *outInertia = math::Vec3(0.4F * rr, 0.4F * rr, 0.4F * rr);
    *outVolume = (4.0F / 3.0F) * kPi * rr * r;
    return;
  }
  case math::ColliderShape::Capsule: {
    // Radius hx, cylinder half height hy along the local Y axis, mass split
    // between the cylinder and the two hemispheres by volume.
    const float r = hx;
    const float h = hy;
    const float rr = r * r;
    const float cylinderVolume = kPi * rr * (2.0F * h);
    const float sphereVolume = (4.0F / 3.0F) * kPi * rr * r;
    const float volume = cylinderVolume + sphereVolume;
    if (volume <= 0.0F) {
      *outInertia = math::Vec3(0.0F, 0.0F, 0.0F);
      *outVolume = 0.0F;
      return;
    }
    const float cylinderMass = cylinderVolume / volume;
    const float hemisphereMass = 0.5F * sphereVolume / volume;
    const float axial =
        (0.5F * cylinderMass * rr) + (2.0F * hemisphereMass * 0.4F * rr);
    // Each hemisphere: its own tensor about its centroid, then the parallel
    // axis from that centroid (3r/8 above the flat face) to the capsule
    // center.
    const float hemisphereArm = h + (0.375F * r);
    const float hemispherePerp =
        ((83.0F / 320.0F) * hemisphereMass * rr) +
        (hemisphereMass * hemisphereArm * hemisphereArm);
    const float perpendicular =
        (cylinderMass * ((0.25F * rr) + (h * h / 3.0F))) +
        (2.0F * hemispherePerp);
    *outInertia = math::Vec3(perpendicular, axial, perpendicular);
    *outVolume = volume;
    return;
  }
  case math::ColliderShape::AABB:
  case math::ColliderShape::ConvexHull:
  case math::ColliderShape::Heightfield:
  default: {
    *outInertia = math::Vec3(((hy * hy) + (hz * hz)) / 3.0F,
                             ((hx * hx) + (hz * hz)) / 3.0F,
                             ((hx * hx) + (hy * hy)) / 3.0F);
    *outVolume = 8.0F * hx * hy * hz;
    return;
  }
  }
}

/// Diagonal of R * diag(inertia) * R^T: each body axis gathers the shape
/// axes by the squared rotation-matrix entries.
math::Vec3 rotate_diagonal(const math::Vec3 &inertia,
                           const math::Quat &rotation) noexcept {
  const math::Quat q = math::normalize(rotation);
  const math::Vec3 ex = math::rotate_vector(math::Vec3(1.0F, 0.0F, 0.0F), q);
  const math::Vec3 ey = math::rotate_vector(math::Vec3(0.0F, 1.0F, 0.0F), q);
  const math::Vec3 ez = math::rotate_vector(math::Vec3(0.0F, 0.0F, 1.0F), q);
  return math::Vec3(
      (ex.x * ex.x * inertia.x) + (ey.x * ey.x * inertia.y) +
          (ez.x * ez.x * inertia.z),
      (ex.y * ex.y * inertia.x) + (ey.y * ey.y * inertia.y) +
          (ez.y * ez.y * inertia.z),
      (ex.z * ex.z * inertia.x) + (ey.z * ey.z * inertia.y) +
          (ez.z * ez.z * inertia.z));
}

} // namespace

void accumulate_collider_inertia(InertiaAccumulator *accumulator,
                                 const math::Collider &collider,
                                 const math::Vec3 &offset,
                                 const math::Quat &rotation) noexcept {
  if (accumulator == nullptr) {
    return;
  }
  math::Vec3 unitInertia{};
  float volume = 0.0F;
  shape_unit_inertia(collider, &unitInertia, &volume);
  const float density =
      (std::isfinite(collider.density) && (collider.density > 0.0F))
          ? collider.density
          : 1.0F;
  const float weight = volume * density;
  if (!(weight > 0.0F) || !std::isfinite(weight)) {
    return;
  }
  const math::Quat placement =
      math::mul(math::normalize(rotation), collider.localRotation);
  const math::Vec3 placed = rotate_diagonal(unitInertia, placement);
  const math::Vec3 d = math::add(
      offset, math::rotate_vector(collider.localPosition,
                                  math::normalize(rotation)));
  // Parallel axis: the diagonal picks up the squared distance to the other
  // two axes.
  const math::Vec3 shifted(placed.x + (d.y * d.y) + (d.z * d.z),
                           placed.y + (d.x * d.x) + (d.z * d.z),
                           placed.z + (d.x * d.x) + (d.y * d.y));
  accumulator->inertia =
      math::add(accumulator->inertia, math::mul(shifted, weight));
  accumulator->weightSum += weight;
}

math::Vec3 finish_inverse_inertia(const InertiaAccumulator &accumulator,
                                  float inverseMass) noexcept {
  if (!(inverseMass > 0.0F) || !std::isfinite(inverseMass) ||
      !(accumulator.weightSum > 0.0F)) {
    return math::Vec3(0.0F, 0.0F, 0.0F);
  }
  // The accumulator holds weight-scaled unit inertias; dividing by the
  // weight sum gives the tensor of a unit-mass body, and the body's inverse
  // mass scales its inverse.
  const float weightSum = accumulator.weightSum;
  const auto invert = [inverseMass, weightSum](float inertia) noexcept -> float {
    const float unitInertia = inertia / weightSum;
    if (!(unitInertia > kMinExtent * kMinExtent) ||
        !std::isfinite(unitInertia)) {
      return 0.0F;
    }
    const float inverse = inverseMass / unitInertia;
    return (inverse > kMaxInverseInertia) ? kMaxInverseInertia : inverse;
  };
  return math::Vec3(invert(accumulator.inertia.x),
                    invert(accumulator.inertia.y),
                    invert(accumulator.inertia.z));
}

math::Vec3 inverse_inertia_for_collider(const math::Collider &collider,
                                        float inverseMass) noexcept {
  InertiaAccumulator accumulator{};
  accumulate_collider_inertia(&accumulator, collider,
                              math::Vec3(0.0F, 0.0F, 0.0F), math::Quat());
  return finish_inverse_inertia(accumulator, inverseMass);
}

math::Vec3 apply_inverse_inertia(const math::Vec3 &inverseInertia,
                                 const math::Quat &rotation,
                                 const math::Vec3 &v) noexcept {
  const math::Vec3 local = math::rotate_vector(v, math::conjugate(rotation));
  const math::Vec3 scaled(local.x * inverseInertia.x,
                          local.y * inverseInertia.y,
                          local.z * inverseInertia.z);
  return math::rotate_vector(scaled, rotation);
}

float angular_effective_inverse_mass(const math::Vec3 &inverseInertia,
                                     const math::Quat &rotation,
                                     const math::Vec3 &lever,
                                     const math::Vec3 &direction) noexcept {
  if (!math::has_rotational_dof(inverseInertia)) {
    return 0.0F;
  }
  const math::Vec3 arm = math::cross(lever, direction);
  return math::dot(arm, apply_inverse_inertia(inverseInertia, rotation, arm));
}

void inverse_inertia_world(const math::Vec3 &inverseInertia,
                           const math::Quat &rotation,
                           float out[3][3]) noexcept {
  // Columns of R are the rotated body axes; R diag R^T sums the outer
  // products of those columns weighted by the axis inverse inertia.
  const math::Vec3 axes[3] = {
      math::rotate_vector(math::Vec3(1.0F, 0.0F, 0.0F), rotation),
      math::rotate_vector(math::Vec3(0.0F, 1.0F, 0.0F), rotation),
      math::rotate_vector(math::Vec3(0.0F, 0.0F, 1.0F), rotation)};
  const float weights[3] = {inverseInertia.x, inverseInertia.y,
                            inverseInertia.z};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      out[row][col] = 0.0F;
    }
  }
  for (int k = 0; k < 3; ++k) {
    const float a[3] = {axes[k].x, axes[k].y, axes[k].z};
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        out[row][col] += weights[k] * a[row] * a[col];
      }
    }
  }
}

} // namespace engine::physics
