// Implements procedural built-in mesh builders for bootstrap content and
// script-spawned primitives.

#include "engine/renderer/mesh_primitives.h"

#include <cmath>
#include <cstdint>

#include "engine/math/vec3.h"

namespace engine::renderer {

// ---------------------------------------------------------------------------
// Procedural mesh builders
// Vertex layout: 6 floats per vertex — (px, py, pz, nx, ny, nz).
// All meshes use CCW winding for outward-facing normals.
// ---------------------------------------------------------------------------

bool build_plane_mesh(GpuMesh *outMesh) noexcept {
  // clang-format off
  static constexpr float kVerts[] = {
    -5.0F, 0.5F, -5.0F,  0.0F, 1.0F, 0.0F,
    -5.0F, 0.5F,  5.0F,  0.0F, 1.0F, 0.0F,
     5.0F, 0.5F,  5.0F,  0.0F, 1.0F, 0.0F,
     5.0F, 0.5F, -5.0F,  0.0F, 1.0F, 0.0F,
  };
  static constexpr std::uint32_t kIdx[] = { 0, 1, 2,  0, 2, 3 };
  // clang-format on
  return build_gpu_mesh_from_data(kVerts, 4U, kIdx, 6U, false,
                                            outMesh);
}

// Unit cube: 4 unique verts per face so normals stay hard-edged.
bool build_cube_mesh(GpuMesh *outMesh) noexcept {
  // clang-format off
  static constexpr float kVerts[] = {
    // +Y face (top), normal (0,+1,0)  — CCW from above
    -0.5F, 0.5F, -0.5F,  0.0F, 1.0F, 0.0F,
    -0.5F, 0.5F,  0.5F,  0.0F, 1.0F, 0.0F,
     0.5F, 0.5F,  0.5F,  0.0F, 1.0F, 0.0F,
     0.5F, 0.5F, -0.5F,  0.0F, 1.0F, 0.0F,
    // -Y face (bottom), normal (0,-1,0)
    -0.5F,-0.5F, -0.5F,  0.0F,-1.0F, 0.0F,
     0.5F,-0.5F, -0.5F,  0.0F,-1.0F, 0.0F,
     0.5F,-0.5F,  0.5F,  0.0F,-1.0F, 0.0F,
    -0.5F,-0.5F,  0.5F,  0.0F,-1.0F, 0.0F,
    // +Z face (front), normal (0,0,+1)
    -0.5F,-0.5F,  0.5F,  0.0F, 0.0F, 1.0F,
     0.5F,-0.5F,  0.5F,  0.0F, 0.0F, 1.0F,
     0.5F, 0.5F,  0.5F,  0.0F, 0.0F, 1.0F,
    -0.5F, 0.5F,  0.5F,  0.0F, 0.0F, 1.0F,
    // -Z face (back), normal (0,0,-1)
     0.5F,-0.5F, -0.5F,  0.0F, 0.0F,-1.0F,
    -0.5F,-0.5F, -0.5F,  0.0F, 0.0F,-1.0F,
    -0.5F, 0.5F, -0.5F,  0.0F, 0.0F,-1.0F,
     0.5F, 0.5F, -0.5F,  0.0F, 0.0F,-1.0F,
    // +X face (right), normal (+1,0,0)
     0.5F,-0.5F,  0.5F,  1.0F, 0.0F, 0.0F,
     0.5F,-0.5F, -0.5F,  1.0F, 0.0F, 0.0F,
     0.5F, 0.5F, -0.5F,  1.0F, 0.0F, 0.0F,
     0.5F, 0.5F,  0.5F,  1.0F, 0.0F, 0.0F,
    // -X face (left), normal (-1,0,0)
    -0.5F,-0.5F, -0.5F, -1.0F, 0.0F, 0.0F,
    -0.5F,-0.5F,  0.5F, -1.0F, 0.0F, 0.0F,
    -0.5F, 0.5F,  0.5F, -1.0F, 0.0F, 0.0F,
    -0.5F, 0.5F, -0.5F, -1.0F, 0.0F, 0.0F,
  };
  static constexpr std::uint32_t kIdx[] = {
     0, 1, 2,  0, 2, 3,   // +Y
     4, 5, 6,  4, 6, 7,   // -Y
     8, 9,10,  8,10,11,   // +Z
    12,13,14, 12,14,15,   // -Z
    16,17,18, 16,18,19,   // +X
    20,21,22, 20,22,23,   // -X
  };
  // clang-format on
  return build_gpu_mesh_from_data(kVerts, 24U, kIdx, 36U, false,
                                            outMesh);
}

// UV sphere: rings share verts along the seam column for simple indexing.
bool build_sphere_mesh(GpuMesh *outMesh) noexcept {
  constexpr int kStacks = 12;
  constexpr int kSlices = 24;
  constexpr float kRadius = 0.5F;
  constexpr int kVCount = (kStacks + 1) * (kSlices + 1);
  constexpr int kICount = kStacks * kSlices * 6;

  float verts[kVCount * 6]{};
  std::uint32_t idx[kICount]{};

  int vi = 0;
  for (int i = 0; i <= kStacks; ++i) {
    const float theta =
        static_cast<float>(i) * 3.14159265359F / static_cast<float>(kStacks);
    const float sinT = std::sin(theta);
    const float cosT = std::cos(theta);
    for (int j = 0; j <= kSlices; ++j) {
      const float phi = static_cast<float>(j) * 2.0F * 3.14159265359F /
                        static_cast<float>(kSlices);
      const float nx = sinT * std::cos(phi);
      const float ny = cosT;
      const float nz = sinT * std::sin(phi);
      verts[vi++] = nx * kRadius;
      verts[vi++] = ny * kRadius;
      verts[vi++] = nz * kRadius;
      verts[vi++] = nx;
      verts[vi++] = ny;
      verts[vi++] = nz;
    }
  }

  int ii = 0;
  for (int i = 0; i < kStacks; ++i) {
    for (int j = 0; j < kSlices; ++j) {
      const std::uint32_t a = static_cast<std::uint32_t>(i * (kSlices + 1) + j);
      const std::uint32_t b = a + static_cast<std::uint32_t>(kSlices + 1);
      const std::uint32_t c = b + 1U;
      const std::uint32_t d = a + 1U;
      idx[ii++] = a;
      idx[ii++] = c;
      idx[ii++] = b;
      idx[ii++] = a;
      idx[ii++] = d;
      idx[ii++] = c;
    }
  }

  return build_gpu_mesh_from_data(
      verts, static_cast<std::uint32_t>(kVCount), idx,
      static_cast<std::uint32_t>(kICount), false, outMesh);
}

// Cylinder: separate ring and cap verts so side/cap normals stay distinct.
bool build_cylinder_mesh(GpuMesh *outMesh) noexcept {
  constexpr int kSlices = 24;
  constexpr float kRadius = 0.5F;
  constexpr float kHalfH = 0.5F;
  constexpr float kPI = 3.14159265359F;

  constexpr int kSideVerts = 2 * (kSlices + 1);
  constexpr int kCapVerts = kSlices + 1;
  constexpr int kTotalVerts = kSideVerts + 2 * kCapVerts;
  constexpr int kTotalIdx = kSlices * 6 + 2 * kSlices * 3;

  float verts[kTotalVerts * 6]{};
  std::uint32_t idx[kTotalIdx]{};

  int vi = 0;
  const auto pushV = [&](float px, float py, float pz, float nx, float ny,
                         float nz) {
    verts[vi++] = px;
    verts[vi++] = py;
    verts[vi++] = pz;
    verts[vi++] = nx;
    verts[vi++] = ny;
    verts[vi++] = nz;
  };

  // Bottom ring
  for (int j = 0; j <= kSlices; ++j) {
    const float phi =
        static_cast<float>(j) * 2.0F * kPI / static_cast<float>(kSlices);
    const float nx = std::cos(phi);
    const float nz = std::sin(phi);
    pushV(nx * kRadius, -kHalfH, nz * kRadius, nx, 0.0F, nz);
  }
  // Top ring
  for (int j = 0; j <= kSlices; ++j) {
    const float phi =
        static_cast<float>(j) * 2.0F * kPI / static_cast<float>(kSlices);
    const float nx = std::cos(phi);
    const float nz = std::sin(phi);
    pushV(nx * kRadius, kHalfH, nz * kRadius, nx, 0.0F, nz);
  }

  // Top cap verts
  const std::uint32_t topBase = static_cast<std::uint32_t>(kSideVerts);
  pushV(0.0F, kHalfH, 0.0F, 0.0F, 1.0F, 0.0F);
  for (int j = 0; j < kSlices; ++j) {
    const float phi =
        static_cast<float>(j) * 2.0F * kPI / static_cast<float>(kSlices);
    pushV(std::cos(phi) * kRadius, kHalfH, std::sin(phi) * kRadius, 0.0F, 1.0F,
          0.0F);
  }

  // Bottom cap verts
  const std::uint32_t botBase = topBase + static_cast<std::uint32_t>(kCapVerts);
  pushV(0.0F, -kHalfH, 0.0F, 0.0F, -1.0F, 0.0F);
  for (int j = 0; j < kSlices; ++j) {
    const float phi =
        static_cast<float>(j) * 2.0F * kPI / static_cast<float>(kSlices);
    pushV(std::cos(phi) * kRadius, -kHalfH, std::sin(phi) * kRadius, 0.0F,
          -1.0F, 0.0F);
  }

  int ii = 0;
  // Side indices
  for (int j = 0; j < kSlices; ++j) {
    const std::uint32_t a = static_cast<std::uint32_t>(j);
    const std::uint32_t b = static_cast<std::uint32_t>(j + 1);
    const std::uint32_t c = static_cast<std::uint32_t>(kSlices + 1 + j);
    const std::uint32_t d = static_cast<std::uint32_t>(kSlices + 1 + j + 1);
    idx[ii++] = a;
    idx[ii++] = c;
    idx[ii++] = b;
    idx[ii++] = b;
    idx[ii++] = c;
    idx[ii++] = d;
  }
  // Top cap indices (CCW from above)
  for (int j = 0; j < kSlices; ++j) {
    const std::uint32_t centre = topBase;
    const std::uint32_t r0 = topBase + 1U + static_cast<std::uint32_t>(j);
    const std::uint32_t r1 =
        topBase + 1U + static_cast<std::uint32_t>((j + 1) % kSlices);
    idx[ii++] = centre;
    idx[ii++] = r1;
    idx[ii++] = r0;
  }
  // Bottom cap indices (CCW from below)
  for (int j = 0; j < kSlices; ++j) {
    const std::uint32_t centre = botBase;
    const std::uint32_t r0 = botBase + 1U + static_cast<std::uint32_t>(j);
    const std::uint32_t r1 =
        botBase + 1U + static_cast<std::uint32_t>((j + 1) % kSlices);
    idx[ii++] = centre;
    idx[ii++] = r0;
    idx[ii++] = r1;
  }

  return build_gpu_mesh_from_data(
      verts, static_cast<std::uint32_t>(kTotalVerts), idx,
      static_cast<std::uint32_t>(kTotalIdx), false, outMesh);
}

// Capsule: two hemisphere stacks offset by the half body height.
bool build_capsule_mesh(GpuMesh *outMesh) noexcept {
  constexpr int kHemiStacks = 8;
  constexpr int kSlices = 16;
  constexpr float kRadius = 0.5F;
  constexpr float kHalfBodyH = 0.5F;
  constexpr float kPI = 3.14159265359F;

  constexpr int kRows = kHemiStacks * 2 + 1;
  constexpr int kVCount = kRows * (kSlices + 1);
  constexpr int kICount = (kRows - 1) * kSlices * 6;

  float verts[kVCount * 6]{};
  std::uint32_t idx[kICount]{};

  int vi = 0;
  for (int i = 0; i < kRows; ++i) {
    float theta = 0.0F;
    float yOffset = 0.0F;
    if (i <= kHemiStacks) {
      theta = static_cast<float>(i) * (kPI * 0.5F) /
              static_cast<float>(kHemiStacks);
      yOffset = kHalfBodyH;
    } else {
      theta = (kPI * 0.5F) + static_cast<float>(i - kHemiStacks) *
                                 (kPI * 0.5F) / static_cast<float>(kHemiStacks);
      yOffset = -kHalfBodyH;
    }
    const float sinT = std::sin(theta);
    const float cosT = std::cos(theta);
    for (int j = 0; j <= kSlices; ++j) {
      const float phi =
          static_cast<float>(j) * 2.0F * kPI / static_cast<float>(kSlices);
      const float nx = sinT * std::cos(phi);
      const float ny = cosT;
      const float nz = sinT * std::sin(phi);
      verts[vi++] = nx * kRadius;
      verts[vi++] = ny * kRadius + yOffset;
      verts[vi++] = nz * kRadius;
      verts[vi++] = nx;
      verts[vi++] = ny;
      verts[vi++] = nz;
    }
  }

  int ii = 0;
  for (int i = 0; i < kRows - 1; ++i) {
    for (int j = 0; j < kSlices; ++j) {
      const std::uint32_t a = static_cast<std::uint32_t>(i * (kSlices + 1) + j);
      const std::uint32_t b = a + static_cast<std::uint32_t>(kSlices + 1);
      const std::uint32_t c = b + 1U;
      const std::uint32_t d = a + 1U;
      idx[ii++] = a;
      idx[ii++] = c;
      idx[ii++] = b;
      idx[ii++] = a;
      idx[ii++] = d;
      idx[ii++] = c;
    }
  }

  return build_gpu_mesh_from_data(
      verts, static_cast<std::uint32_t>(kVCount), idx,
      static_cast<std::uint32_t>(kICount), false, outMesh);
}

bool build_pyramid_mesh(GpuMesh *outMesh) noexcept {
  constexpr float kBaseZBack = -0.288675F;
  constexpr float kBaseZFront = 0.577350F;
  const math::Vec3 apex(0.0F, 0.5F, 0.0F);
  const math::Vec3 b0(-0.5F, -0.5F, kBaseZBack);
  const math::Vec3 b1(0.5F, -0.5F, kBaseZBack);
  const math::Vec3 b2(0.0F, -0.5F, kBaseZFront);

  float verts[72]{};
  int vi = 0;
  const auto addFace = [&](const math::Vec3 &p0, const math::Vec3 &p1,
                           const math::Vec3 &p2) {
    const math::Vec3 normal =
        math::normalize(math::cross(math::sub(p1, p0), math::sub(p2, p0)));
    for (const math::Vec3 &p : {p0, p1, p2}) {
      verts[vi++] = p.x;
      verts[vi++] = p.y;
      verts[vi++] = p.z;
      verts[vi++] = normal.x;
      verts[vi++] = normal.y;
      verts[vi++] = normal.z;
    }
  };

  addFace(b0, b2, b1);
  addFace(b0, b1, apex);
  addFace(b1, b2, apex);
  addFace(b2, b0, apex);

  return build_gpu_mesh_from_data(verts, 12U, nullptr, 0U, false, outMesh);
}

} // namespace engine::renderer
