// Verifies the pyramid primitive's CPU vertex data: every face winds CCW
// around its stored normal and every normal points away from the solid, so
// back-face culling shows the outside surfaces (regression: the pyramid
// used to render inside-out).

#include "engine/math/vec3.h"
#include "engine/renderer/mesh_primitives.h"

#include <cstddef>
#include <cstdio>

namespace {

constexpr std::size_t kFloatCount = 72U;
constexpr std::size_t kVertexCount = 12U;
constexpr std::size_t kFaceCount = 4U;

/// Loads the packed position of vertex `index` (6 floats per vertex).
engine::math::Vec3 position_at(const float *verts,
                               std::size_t index) noexcept {
  return {verts[(index * 6U) + 0U], verts[(index * 6U) + 1U],
          verts[(index * 6U) + 2U]};
}

/// Loads the packed stored normal of vertex `index`.
engine::math::Vec3 normal_at(const float *verts, std::size_t index) noexcept {
  return {verts[(index * 6U) + 3U], verts[(index * 6U) + 4U],
          verts[(index * 6U) + 5U]};
}

/// Component dot product without relying on additional math helpers.
float dot3(const engine::math::Vec3 &a, const engine::math::Vec3 &b) noexcept {
  return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

/// Every face's winding normal must match its stored normals exactly and
/// must point away from the solid's centroid.
int check_pyramid_faces_wind_outward() noexcept {
  float verts[kFloatCount] = {};
  if (engine::renderer::fill_pyramid_vertices(verts, kFloatCount) !=
      kFloatCount) {
    return 1;
  }

  engine::math::Vec3 centroid{};
  for (std::size_t i = 0U; i < kVertexCount; ++i) {
    const engine::math::Vec3 p = position_at(verts, i);
    centroid.x += p.x / static_cast<float>(kVertexCount);
    centroid.y += p.y / static_cast<float>(kVertexCount);
    centroid.z += p.z / static_cast<float>(kVertexCount);
  }

  for (std::size_t face = 0U; face < kFaceCount; ++face) {
    const std::size_t base = face * 3U;
    const engine::math::Vec3 p0 = position_at(verts, base + 0U);
    const engine::math::Vec3 p1 = position_at(verts, base + 1U);
    const engine::math::Vec3 p2 = position_at(verts, base + 2U);
    const engine::math::Vec3 winding = engine::math::normalize(
        engine::math::cross(engine::math::sub(p1, p0),
                            engine::math::sub(p2, p0)));

    for (std::size_t corner = 0U; corner < 3U; ++corner) {
      const engine::math::Vec3 stored = normal_at(verts, base + corner);
      if ((stored.x != winding.x) || (stored.y != winding.y) ||
          (stored.z != winding.z)) {
        return 2;
      }
    }

    const engine::math::Vec3 faceCenter(
        (p0.x + p1.x + p2.x) / 3.0F, (p0.y + p1.y + p2.y) / 3.0F,
        (p0.z + p1.z + p2.z) / 3.0F);
    const engine::math::Vec3 outward = engine::math::sub(faceCenter, centroid);
    if (dot3(winding, outward) <= 0.0F) {
      return 3;
    }
  }

  return 0;
}

/// The base must face straight down and every side face must tilt upward
/// toward the apex.
int check_pyramid_base_down_sides_up() noexcept {
  float verts[kFloatCount] = {};
  if (engine::renderer::fill_pyramid_vertices(verts, kFloatCount) !=
      kFloatCount) {
    return 10;
  }

  const engine::math::Vec3 baseNormal = normal_at(verts, 0U);
  if ((baseNormal.x != 0.0F) || (baseNormal.y >= 0.0F) ||
      (baseNormal.z != 0.0F)) {
    return 11;
  }

  for (std::size_t face = 1U; face < kFaceCount; ++face) {
    const engine::math::Vec3 sideNormal = normal_at(verts, face * 3U);
    if (sideNormal.y <= 0.0F) {
      return 12;
    }
  }

  return 0;
}

/// The fill must reject null output and short capacities.
int check_pyramid_fill_capacity_guard() noexcept {
  float verts[kFloatCount] = {};
  if (engine::renderer::fill_pyramid_vertices(nullptr, kFloatCount) != 0U) {
    return 20;
  }
  if (engine::renderer::fill_pyramid_vertices(verts, kFloatCount - 1U) != 0U) {
    return 21;
  }
  return 0;
}

} // namespace

int main() {
  int result = check_pyramid_faces_wind_outward();
  if (result != 0) {
    std::fprintf(stderr, "mesh_primitives_test failed: %d\n", result);
    return result;
  }

  result = check_pyramid_base_down_sides_up();
  if (result != 0) {
    std::fprintf(stderr, "mesh_primitives_test failed: %d\n", result);
    return result;
  }

  result = check_pyramid_fill_capacity_guard();
  if (result != 0) {
    std::fprintf(stderr, "mesh_primitives_test failed: %d\n", result);
    return result;
  }

  std::printf("mesh_primitives_test: all tests passed\n");
  return 0;
}
