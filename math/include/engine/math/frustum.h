// Declares the one view-frustum primitive every system culls and picks
// with: planes extracted from a view-projection, conservative box, swept
// box and sphere tests, sub-rectangle projections (light tiles, a marquee),
// and unprojection of clip-space points and the frustum's corners. Both
// clip depth conventions are handled: [-1, 1] on GL, [0, 1] on D3D, Metal
// and Vulkan. Defined inline, like the rest of engine_math, so culling
// loops stay inlinable.

#pragma once

#include <cmath>

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"

namespace engine::math {

/// The plane dot(normal, p) + d = 0. Frustum planes face inward with a
/// unit normal, so plane_distance is a signed distance in world units,
/// positive inside.
struct Plane final {
  Vec3 normal = Vec3(0.0F, 0.0F, 0.0F);
  float d = 0.0F;
};

/// Six inward planes, in the order left, right, bottom, top, near, far.
struct Frustum final {
  Plane planes[6] = {};
};

/// The clip-space depth of the near plane: 0 under the zero-to-one
/// convention, -1 under GL's. The far plane is at 1 under both.
constexpr float clip_near_depth(bool depthZeroToOne) noexcept {
  return depthZeroToOne ? 0.0F : -1.0F;
}

/// Signed distance from `point` to `plane`, positive on the side its
/// normal faces.
inline float plane_distance(const Plane &plane, const Vec3 &point) noexcept {
  return dot(plane.normal, point) + plane.d;
}

/// Extracts the frustum of the column-major `viewProjection` (Gribb and
/// Hartmann): each plane is the w row plus or minus the x, y or z row.
/// The near plane follows the projection's convention: GL clips at
/// z = -w (the w row plus the z row), the zero-to-one APIs at z = 0 (the
/// z row alone). Planes are normalized; a degenerate plane (a normal
/// shorter than 1e-6, only from a singular matrix) is left as extracted.
inline Frustum frustum_from_view_projection(const Mat4 &viewProjection,
                                            bool depthZeroToOne) noexcept {
  const Vec4 &c0 = viewProjection.columns[0];
  const Vec4 &c1 = viewProjection.columns[1];
  const Vec4 &c2 = viewProjection.columns[2];
  const Vec4 &c3 = viewProjection.columns[3];
  const Vec4 rowX(c0.x, c1.x, c2.x, c3.x);
  const Vec4 rowY(c0.y, c1.y, c2.y, c3.y);
  const Vec4 rowZ(c0.z, c1.z, c2.z, c3.z);
  const Vec4 rowW(c0.w, c1.w, c2.w, c3.w);
  const auto combine = [](const Vec4 &w, const Vec4 &row, float sign) noexcept {
    return Vec4(w.x + (sign * row.x), w.y + (sign * row.y),
                w.z + (sign * row.z), w.w + (sign * row.w));
  };
  const Vec4 raw[6] = {
      combine(rowW, rowX, 1.0F),
      combine(rowW, rowX, -1.0F),
      combine(rowW, rowY, 1.0F),
      combine(rowW, rowY, -1.0F),
      depthZeroToOne ? rowZ : combine(rowW, rowZ, 1.0F),
      combine(rowW, rowZ, -1.0F),
  };
  Frustum frustum{};
  for (int i = 0; i < 6; ++i) {
    const Vec4 &p = raw[i];
    const float length = std::sqrt((p.x * p.x) + (p.y * p.y) + (p.z * p.z));
    const float scale = (length > 1.0e-6F) ? (1.0F / length) : 1.0F;
    frustum.planes[i].normal = Vec3(p.x * scale, p.y * scale, p.z * scale);
    frustum.planes[i].d = p.w * scale;
  }
  return frustum;
}

/// True when the box (center, half extents) lies wholly outside one of
/// the planes. Conservative: a box outside the frustum only past a corner
/// or an edge is not excluded, which costs a draw, never a missing one.
inline bool frustum_excludes_box(const Frustum &frustum, const Vec3 &center,
                                 const Vec3 &half) noexcept {
  for (const Plane &plane : frustum.planes) {
    // The corner furthest along the normal: when even it is behind the
    // plane, the whole box is.
    const Vec3 corner(center.x + ((plane.normal.x >= 0.0F) ? half.x : -half.x),
                      center.y + ((plane.normal.y >= 0.0F) ? half.y : -half.y),
                      center.z + ((plane.normal.z >= 0.0F) ? half.z : -half.z));
    if (plane_distance(plane, corner) < 0.0F) {
      return true;
    }
  }
  return false;
}

/// True when the box swept along `sweep` (a direction scaled by the sweep
/// length) lies wholly outside one of the planes: the box's furthest
/// corner plus the sweep's reach toward the plane still falls behind it.
/// Conservative as frustum_excludes_box is.
inline bool frustum_excludes_swept_box(const Frustum &frustum,
                                       const Vec3 &center, const Vec3 &half,
                                       const Vec3 &sweep) noexcept {
  for (const Plane &plane : frustum.planes) {
    const Vec3 corner(center.x + ((plane.normal.x >= 0.0F) ? half.x : -half.x),
                      center.y + ((plane.normal.y >= 0.0F) ? half.y : -half.y),
                      center.z + ((plane.normal.z >= 0.0F) ? half.z : -half.z));
    const float along = dot(plane.normal, sweep);
    const float reach = (along > 0.0F) ? along : 0.0F;
    if ((plane_distance(plane, corner) + reach) < 0.0F) {
      return true;
    }
  }
  return false;
}

/// True when the sphere lies wholly outside one of the planes.
/// Conservative as frustum_excludes_box is.
inline bool frustum_excludes_sphere(const Frustum &frustum, const Vec3 &center,
                                    float radius) noexcept {
  for (const Plane &plane : frustum.planes) {
    if (plane_distance(plane, center) < -radius) {
      return true;
    }
  }
  return false;
}

/// The projection that maps the NDC rectangle [minX, maxX] x [minY, maxY]
/// of `projection` onto the whole [-1, 1] square, so its frustum is the
/// part of the view behind that rectangle (a light tile, a marquee). Depth
/// is untouched, so either convention carries through. The rectangle must
/// have positive width and height.
inline Mat4 sub_rect_projection(const Mat4 &projection, float minX, float minY,
                                float maxX, float maxY) noexcept {
  const float scaleX = 2.0F / (maxX - minX);
  const float scaleY = 2.0F / (maxY - minY);
  Mat4 rect = identity();
  rect.columns[0].x = scaleX;
  rect.columns[1].y = scaleY;
  rect.columns[3].x = -(maxX + minX) / (maxX - minX);
  rect.columns[3].y = -(maxY + minY) / (maxY - minY);
  return mul(rect, projection);
}

/// The world point at normalized device coordinates `ndc` under
/// `inverseViewProjection`. False when the point has no finite image (a
/// homogeneous w within 1e-12 of zero, only from a degenerate matrix).
inline bool unproject_ndc(const Mat4 &inverseViewProjection, const Vec3 &ndc,
                          Vec3 *out) noexcept {
  const Vec4 world =
      mul(inverseViewProjection, Vec4(ndc.x, ndc.y, ndc.z, 1.0F));
  if (!(std::fabs(world.w) >= 1.0e-12F)) {
    return false;
  }
  const float invW = 1.0F / world.w;
  *out = Vec3(world.x * invW, world.y * invW, world.z * invW);
  return true;
}

/// The frustum's eight world-space corners under `inverseViewProjection`:
/// the near quad, then the far quad, each bottom-left, bottom-right,
/// top-right, top-left. False, with `out` unspecified, when a corner has
/// no finite image.
inline bool frustum_corners(const Mat4 &inverseViewProjection,
                            bool depthZeroToOne, Vec3 (&out)[8]) noexcept {
  constexpr float kQuad[4][2] = {
      {-1.0F, -1.0F}, {1.0F, -1.0F}, {1.0F, 1.0F}, {-1.0F, 1.0F}};
  const float depths[2] = {clip_near_depth(depthZeroToOne), 1.0F};
  for (int face = 0; face < 2; ++face) {
    for (int corner = 0; corner < 4; ++corner) {
      if (!unproject_ndc(inverseViewProjection,
                         Vec3(kQuad[corner][0], kQuad[corner][1], depths[face]),
                         &out[(face * 4) + corner])) {
        return false;
      }
    }
  }
  return true;
}

} // namespace engine::math
