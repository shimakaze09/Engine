#include "engine/engine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <new>

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#if __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL2 headers not found"
#endif

#include "engine/audio/audio.h"
#include "engine/core/bootstrap.h"
#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/input.h"
#include "engine/core/job_system.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/profiler.h"
#include "engine/core/vfs.h"
#include "engine/math/transform.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/shader_system.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/render_prep_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace engine {

namespace {

constexpr std::size_t kFrameAllocatorBytes = 1024U * 1024U;
constexpr double kFixedDeltaSeconds = 1.0 / 60.0;
constexpr std::size_t kChunkSize = 256U;
constexpr std::size_t kMaxUpdateStepsPerFrame = 8U;
constexpr std::size_t kMaxChunkJobs = 1024U;
constexpr std::size_t kMaxPhaseJobs = kMaxUpdateStepsPerFrame * 2U + 4U;
constexpr std::uint32_t kSliceDiagnosticsPeriodFrames = 60U;
constexpr const char *kMainScriptPath = "assets/main.lua";

// Runtime binaries are launched from multiple working directories (repo root,
// build root, and nested test folders), so keep a short fallback search list.
constexpr std::array<const char *, 4U> kMeshAssetPathCandidates = {
    "assets/triangle.mesh",
    "../assets/triangle.mesh",
    "../../assets/triangle.mesh",
    "../../../assets/triangle.mesh",
};

struct UpdateChunkJobData final {
  runtime::World *world = nullptr;
  std::size_t startIndex = 0U;
  std::size_t count = 0U;
  float deltaSeconds = 0.0F;
};

struct PhysicsChunkJobData final {
  runtime::World *world = nullptr;
  std::size_t startIndex = 0U;
  std::size_t count = 0U;
  float deltaSeconds = 0.0F;
  std::atomic<bool> *frameGraphFailed = nullptr;
};

struct WorldPhaseJobData final {
  runtime::World *world = nullptr;
};

struct ResolveCollisionsJobData final {
  runtime::World *world = nullptr;
  std::atomic<bool> *frameGraphFailed = nullptr;
};

struct FrameContext final {
  runtime::RenderPrepPipelineContext renderPrepPipeline{};
  std::array<UpdateChunkJobData, kMaxChunkJobs> updateJobData{};
  std::array<core::JobHandle, kMaxChunkJobs> updateJobHandles{};
  std::array<PhysicsChunkJobData, kMaxChunkJobs> physicsJobData{};
  std::array<core::JobHandle, kMaxChunkJobs> physicsJobHandles{};
  std::array<WorldPhaseJobData, kMaxPhaseJobs> phaseJobData{};
  ResolveCollisionsJobData resolveCollisionsJobData{};
  std::atomic<bool> frameGraphFailed = false;
};

std::unique_ptr<FrameContext> g_frameContext;

bool file_exists(const char *path) noexcept {
  if (path == nullptr) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return false;
  }

  std::fclose(file);
  return true;
}

const char *resolve_mesh_asset_path() noexcept {
  for (const char *candidate : kMeshAssetPathCandidates) {
    if (file_exists(candidate)) {
      return candidate;
    }
  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// Procedural mesh builders
// Vertex layout: 6 floats per vertex — (px, py, pz, nx, ny, nz).
// All meshes use CCW winding for outward-facing normals.
// ---------------------------------------------------------------------------

// Ground plane: flat quad at y=+0.5 (entity of space), 10×10, normal upward.
// When the ground entity sits at world y=−0.5 the visible surface falls at y=0,
// matching the top of a (5,0.5,5) AABB collider.
bool build_plane_mesh(renderer::GpuMesh *outMesh) noexcept {
  // clang-format off
  static constexpr float kVerts[] = {
    -5.0F, 0.5F, -5.0F,  0.0F, 1.0F, 0.0F,
    -5.0F, 0.5F,  5.0F,  0.0F, 1.0F, 0.0F,
     5.0F, 0.5F,  5.0F,  0.0F, 1.0F, 0.0F,
     5.0F, 0.5F, -5.0F,  0.0F, 1.0F, 0.0F,
  };
  static constexpr std::uint32_t kIdx[] = { 0, 1, 2,  0, 2, 3 };
  // clang-format on
  return renderer::build_gpu_mesh_from_data(kVerts, 4U, kIdx, 6U, false,
                                            outMesh);
}

// Unit cube: half-extent 0.5 on all axes.  24 vertices (4 per face), 36 idx.
bool build_cube_mesh(renderer::GpuMesh *outMesh) noexcept {
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
  return renderer::build_gpu_mesh_from_data(kVerts, 24U, kIdx, 36U, false,
                                            outMesh);
}

// UV sphere: radius 0.5, 12 stacks × 24 slices.
// Vertices: (stacks+1)×(slices+1) = 13×25 = 325.  Indices: 12×24×6 = 1728.
bool build_sphere_mesh(renderer::GpuMesh *outMesh) noexcept {
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

  return renderer::build_gpu_mesh_from_data(
      verts, static_cast<std::uint32_t>(kVCount), idx,
      static_cast<std::uint32_t>(kICount), false, outMesh);
}

// Cylinder: radius 0.5, height 1.0, 24 slices.
// Sides: 2 rings × (slices+1) verts.  Top+bottom caps: fan with centre.
bool build_cylinder_mesh(renderer::GpuMesh *outMesh) noexcept {
  constexpr int kSlices = 24;
  constexpr float kRadius = 0.5F;
  constexpr float kHalfH = 0.5F;
  constexpr float kPI = 3.14159265359F;

  // Side verts: (bottom ring) + (top ring) = 2*(slices+1)
  // Each cap: 1 centre + slices rim = slices+1
  // Total verts: 2*(slices+1) + 2*(slices+1)
  constexpr int kSideVerts = 2 * (kSlices + 1);
  constexpr int kCapVerts = kSlices + 1; // centre + rim for ONE cap
  constexpr int kTotalVerts = kSideVerts + 2 * kCapVerts;
  // Side indices: slices quads × 2 tris = slices*6
  // Cap indices: slices tris × 2 caps = 2*slices*3
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

  // --- Side verts ---
  // Bottom ring: verts [0 .. slices]
  for (int j = 0; j <= kSlices; ++j) {
    const float phi =
        static_cast<float>(j) * 2.0F * kPI / static_cast<float>(kSlices);
    const float nx = std::cos(phi);
    const float nz = std::sin(phi);
    pushV(nx * kRadius, -kHalfH, nz * kRadius, nx, 0.0F, nz);
  }
  // Top ring: verts [slices+1 .. 2*slices+1]
  for (int j = 0; j <= kSlices; ++j) {
    const float phi =
        static_cast<float>(j) * 2.0F * kPI / static_cast<float>(kSlices);
    const float nx = std::cos(phi);
    const float nz = std::sin(phi);
    pushV(nx * kRadius, kHalfH, nz * kRadius, nx, 0.0F, nz);
  }

  // --- Top cap verts: centre then rim ---
  // Top cap base index = kSideVerts
  const std::uint32_t topBase = static_cast<std::uint32_t>(kSideVerts);
  pushV(0.0F, kHalfH, 0.0F, 0.0F, 1.0F, 0.0F); // centre
  for (int j = 0; j < kSlices; ++j) {
    const float phi =
        static_cast<float>(j) * 2.0F * kPI / static_cast<float>(kSlices);
    pushV(std::cos(phi) * kRadius, kHalfH, std::sin(phi) * kRadius, 0.0F, 1.0F,
          0.0F);
  }

  // --- Bottom cap verts: centre then rim ---
  const std::uint32_t botBase = topBase + static_cast<std::uint32_t>(kCapVerts);
  pushV(0.0F, -kHalfH, 0.0F, 0.0F, -1.0F, 0.0F); // centre
  for (int j = 0; j < kSlices; ++j) {
    const float phi =
        static_cast<float>(j) * 2.0F * kPI / static_cast<float>(kSlices);
    pushV(std::cos(phi) * kRadius, -kHalfH, std::sin(phi) * kRadius, 0.0F,
          -1.0F, 0.0F);
  }

  int ii = 0;
  // --- Side indices ---
  for (int j = 0; j < kSlices; ++j) {
    const std::uint32_t a = static_cast<std::uint32_t>(j);     // bot j
    const std::uint32_t b = static_cast<std::uint32_t>(j + 1); // bot j+1
    const std::uint32_t c =
        static_cast<std::uint32_t>(kSlices + 1 + j); // top j
    const std::uint32_t d =
        static_cast<std::uint32_t>(kSlices + 1 + j + 1); // top j+1
    idx[ii++] = a;
    idx[ii++] = c;
    idx[ii++] = b;
    idx[ii++] = b;
    idx[ii++] = c;
    idx[ii++] = d;
  }
  // --- Top cap indices (CCW from above) ---
  for (int j = 0; j < kSlices; ++j) {
    const std::uint32_t centre = topBase;
    const std::uint32_t r0 = topBase + 1U + static_cast<std::uint32_t>(j);
    const std::uint32_t r1 =
        topBase + 1U + static_cast<std::uint32_t>((j + 1) % kSlices);
    idx[ii++] = centre;
    idx[ii++] = r1;
    idx[ii++] = r0;
  }
  // --- Bottom cap indices (CCW from below) ---
  for (int j = 0; j < kSlices; ++j) {
    const std::uint32_t centre = botBase;
    const std::uint32_t r0 = botBase + 1U + static_cast<std::uint32_t>(j);
    const std::uint32_t r1 =
        botBase + 1U + static_cast<std::uint32_t>((j + 1) % kSlices);
    idx[ii++] = centre;
    idx[ii++] = r0;
    idx[ii++] = r1;
  }

  return renderer::build_gpu_mesh_from_data(
      verts, static_cast<std::uint32_t>(kTotalVerts), idx,
      static_cast<std::uint32_t>(kTotalIdx), false, outMesh);
}

// Capsule: radius 0.5, total height 2.0 (cylinder body height 1.0 +
// hemispheres). 8 stacks per hemisphere, 16 slices.
bool build_capsule_mesh(renderer::GpuMesh *outMesh) noexcept {
  constexpr int kHemiStacks = 8;
  constexpr int kSlices = 16;
  constexpr float kRadius = 0.5F;
  constexpr float kHalfBodyH = 0.5F; // cylinder centre half-height
  constexpr float kPI = 3.14159265359F;

  // Total rows = kHemiStacks*2 + 1 cylinder band (just top+bottom rings)
  //   top hemisphere: (kHemiStacks+1) rows
  //   bottom hemisphere: (kHemiStacks+1) rows
  //   shared equator row → -1
  constexpr int kRows = kHemiStacks * 2 + 1; // =17 rows
  constexpr int kVCount = kRows * (kSlices + 1);
  constexpr int kICount = (kRows - 1) * kSlices * 6;

  float verts[kVCount * 6]{};
  std::uint32_t idx[kICount]{};

  int vi = 0;
  // Row 0 = north pole (top of capsule, y = +kRadius + kHalfBodyH)
  // Row kHemiStacks = equator (y = +kHalfBodyH)
  // Row kHemiStacks+1 = equator of bottom body (y = -kHalfBodyH)
  // Row kHemiStacks*2 = south pole (y = -kRadius - kHalfBodyH)
  for (int i = 0; i < kRows; ++i) {
    float theta = 0.0F;
    float yOffset = 0.0F;
    if (i <= kHemiStacks) {
      // Top hemisphere: theta 0 → PI/2
      theta = static_cast<float>(i) * (kPI * 0.5F) /
              static_cast<float>(kHemiStacks);
      yOffset = kHalfBodyH;
    } else {
      // Bottom hemisphere: theta PI/2 → PI
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

  return renderer::build_gpu_mesh_from_data(
      verts, static_cast<std::uint32_t>(kVCount), idx,
      static_cast<std::uint32_t>(kICount), false, outMesh);
}

// Triangular pyramid (tetrahedron-like) centered around origin:
// base at y=-0.5, apex at y=+0.5. 4 faces, 12 vertices.
bool build_pyramid_mesh(renderer::GpuMesh *outMesh) noexcept {
  // Equilateral base with side length ~1.0.
  constexpr float kBaseZBack = -0.288675F; // -sqrt(3)/6
  constexpr float kBaseZFront = 0.577350F; //  sqrt(3)/3
  const float apex[3] = {0.0F, 0.5F, 0.0F};
  const float b0[3] = {-0.5F, -0.5F, kBaseZBack};
  const float b1[3] = {0.5F, -0.5F, kBaseZBack};
  const float b2[3] = {0.0F, -0.5F, kBaseZFront};

  const auto cross3 = [](float ax, float ay, float az, float bxv, float byn,
                         float bzv, float *ox, float *oy, float *oz) {
    *ox = ay * bzv - az * byn;
    *oy = az * bxv - ax * bzv;
    *oz = ax * byn - ay * bxv;
  };
  const auto norm3 = [](float *x, float *y, float *z) {
    const float len = std::sqrt(*x * *x + *y * *y + *z * *z);
    if (len > 0.0F) {
      *x /= len;
      *y /= len;
      *z /= len;
    }
  };

  // 4 faces × 3 verts × 6 floats = 72, no index buffer needed
  float verts[72]{};
  int vi = 0;
  const auto addFace = [&](float p0x, float p0y, float p0z, float p1x,
                           float p1y, float p1z, float p2x, float p2y,
                           float p2z) {
    float nx, ny, nz;
    cross3(p1x - p0x, p1y - p0y, p1z - p0z, p2x - p0x, p2y - p0y, p2z - p0z,
           &nx, &ny, &nz);
    norm3(&nx, &ny, &nz);
    verts[vi++] = p0x;
    verts[vi++] = p0y;
    verts[vi++] = p0z;
    verts[vi++] = nx;
    verts[vi++] = ny;
    verts[vi++] = nz;
    verts[vi++] = p1x;
    verts[vi++] = p1y;
    verts[vi++] = p1z;
    verts[vi++] = nx;
    verts[vi++] = ny;
    verts[vi++] = nz;
    verts[vi++] = p2x;
    verts[vi++] = p2y;
    verts[vi++] = p2z;
    verts[vi++] = nx;
    verts[vi++] = ny;
    verts[vi++] = nz;
  };

  // Base (CCW when viewed from below, normal downward).
  addFace(b0[0], b0[1], b0[2], b2[0], b2[1], b2[2], b1[0], b1[1], b1[2]);
  // Sides.
  addFace(b0[0], b0[1], b0[2], b1[0], b1[1], b1[2], apex[0], apex[1], apex[2]);
  addFace(b1[0], b1[1], b1[2], b2[0], b2[1], b2[2], apex[0], apex[1], apex[2]);
  addFace(b2[0], b2[1], b2[2], b0[0], b0[1], b0[2], apex[0], apex[1], apex[2]);

  return renderer::build_gpu_mesh_from_data(verts, 12U, nullptr, 0U, false,
                                            outMesh);
}

// Register a procedurally-built GpuMesh into the mesh registry and asset DB.
// Returns kInvalidAssetId on failure. Uses a fake "builtin://<name>" path so
// the asset manager never tries to load it from disk.
renderer::AssetId register_builtin_mesh(renderer::GpuMeshRegistry *registry,
                                        renderer::AssetDatabase *database,
                                        const renderer::GpuMesh &mesh,
                                        const char *builtinPath) noexcept {
  const std::uint32_t slot = renderer::register_gpu_mesh(registry, mesh);
  if (slot == 0U) {
    return renderer::kInvalidAssetId;
  }
  const renderer::MeshHandle handle{slot};
  const renderer::AssetId id = renderer::make_asset_id_from_path(builtinPath);
  if (id == renderer::kInvalidAssetId) {
    return renderer::kInvalidAssetId;
  }
  if (!renderer::register_mesh_asset(database, id, builtinPath, handle)) {
    return renderer::kInvalidAssetId;
  }
  return id;
}

void mark_graph_failed(std::atomic<bool> *frameGraphFailed) noexcept {
  if (frameGraphFailed != nullptr) {
    frameGraphFailed->store(true, std::memory_order_release);
  }
}

void process_input_events_with_editor() noexcept {
  core::begin_input_frame();

  const runtime::EditorBridge *bridge = runtime::editor_bridge();

  SDL_Event event{};
  while (SDL_PollEvent(&event) != 0) {
    core::input_process_event(&event);

    if ((bridge != nullptr) && (bridge->process_event != nullptr)) {
      bridge->process_event(&event);
    }

    if (event.type == SDL_QUIT) {
      core::request_platform_quit();
      continue;
    }

    const bool keyboardEvent =
        (event.type == SDL_KEYDOWN) || (event.type == SDL_KEYUP) ||
        (event.type == SDL_TEXTINPUT) || (event.type == SDL_TEXTEDITING);
    const bool mouseEvent = (event.type == SDL_MOUSEMOTION) ||
                            (event.type == SDL_MOUSEBUTTONDOWN) ||
                            (event.type == SDL_MOUSEBUTTONUP) ||
                            (event.type == SDL_MOUSEWHEEL);
    const bool captureKeyboard = (bridge != nullptr) &&
                                 (bridge->wants_capture_keyboard != nullptr) &&
                                 bridge->wants_capture_keyboard();
    const bool captureMouse = (bridge != nullptr) &&
                              (bridge->wants_capture_mouse != nullptr) &&
                              bridge->wants_capture_mouse();
    const bool editorCapturesInput =
        (keyboardEvent && captureKeyboard) || (mouseEvent && captureMouse);

    if (editorCapturesInput) {
      continue;
    }

    // Game-level input hooks are added in later phases.
  }

  core::end_input_frame();
}

void update_chunk_job(void *userData) noexcept {
  auto *jobData = static_cast<UpdateChunkJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)) {
    return;
  }

  static_cast<void>(jobData->world->update_transforms_range(
      jobData->startIndex, jobData->count, jobData->deltaSeconds));
}

void physics_chunk_job(void *userData) noexcept {
  auto *jobData = static_cast<PhysicsChunkJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)) {
    return;
  }

  if (!runtime::step_physics_range(*jobData->world, jobData->startIndex,
                                   jobData->count, jobData->deltaSeconds)) {
    mark_graph_failed(jobData->frameGraphFailed);
  }
}

void resolve_collisions_job(void *userData) noexcept {
  auto *jobData = static_cast<ResolveCollisionsJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)) {
    return;
  }

  if (!runtime::resolve_collisions(*jobData->world)) {
    mark_graph_failed(jobData->frameGraphFailed);
  }
}

void commit_update_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->commit_update_phase();
  }
}

void begin_update_step_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->begin_update_step();
  }
}

void begin_render_prep_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->begin_render_prep_phase();
  }
}

void begin_render_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->begin_render_phase();
  }
}

void end_frame_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->end_frame_phase();
  }
}

bool link_dependency(core::JobHandle prerequisite,
                     core::JobHandle dependent) noexcept {
  if (!core::is_valid_handle(prerequisite) ||
      !core::is_valid_handle(dependent)) {
    return false;
  }

  return core::add_dependency(prerequisite, dependent);
}

core::JobHandle submit_world_phase_job(FrameContext *frameContext,
                                       runtime::World *world,
                                       std::size_t *phaseJobCursor,
                                       core::JobFunction function) noexcept {
  if ((frameContext == nullptr) || (world == nullptr) ||
      (phaseJobCursor == nullptr) ||
      (*phaseJobCursor >= frameContext->phaseJobData.size())) {
    return {};
  }

  WorldPhaseJobData &jobData = frameContext->phaseJobData[*phaseJobCursor];
  ++(*phaseJobCursor);
  jobData.world = world;

  core::Job job{};
  job.function = function;
  job.data = &jobData;
  return core::submit(job);
}

enum class LoopPlayState : std::uint8_t { Stopped, Playing, Paused };

LoopPlayState query_editor_play_state() noexcept {
  const runtime::EditorBridge *bridge = runtime::editor_bridge();
  if (bridge == nullptr) {
    return LoopPlayState::Playing;
  }

  if ((bridge->is_playing != nullptr) && bridge->is_playing()) {
    return LoopPlayState::Playing;
  }

  if ((bridge->is_paused != nullptr) && bridge->is_paused()) {
    return LoopPlayState::Paused;
  }

  return LoopPlayState::Stopped;
}

const char *world_phase_to_string(runtime::WorldPhase phase) noexcept {
  switch (phase) {
  case runtime::WorldPhase::Input:
    return "Input";
  case runtime::WorldPhase::Simulation:
    return "Simulation";
  case runtime::WorldPhase::TransformPropagation:
    return "Transform";
  case runtime::WorldPhase::RenderSubmission:
    return "RenderPrep";
  case runtime::WorldPhase::Render:
    return "Render";
  default:
    return "Unknown";
  }
}

bool vec3_has_motion(const math::Vec3 &value) noexcept {
  constexpr float kEpsilon = 0.0001F;
  return (value.x > kEpsilon) || (value.x < -kEpsilon) ||
         (value.y > kEpsilon) || (value.y < -kEpsilon) ||
         (value.z > kEpsilon) || (value.z < -kEpsilon);
}

std::size_t count_moving_rigid_bodies(const runtime::World &world) noexcept {
  std::size_t count = 0U;
  world.for_each<runtime::RigidBody>(
      [&count](runtime::Entity, const runtime::RigidBody &rigidBody) noexcept {
        if (vec3_has_motion(rigidBody.velocity) ||
            vec3_has_motion(rigidBody.acceleration)) {
          ++count;
        }
      });
  return count;
}

std::size_t count_mesh_components(const runtime::World &world) noexcept {
  std::size_t count = 0U;
  world.for_each<runtime::MeshComponent>(
      [&count](runtime::Entity, const runtime::MeshComponent &) noexcept {
        ++count;
      });
  return count;
}

std::size_t
count_ready_mesh_components(const runtime::World &world,
                            const renderer::AssetDatabase *assets) noexcept {
  if (assets == nullptr) {
    return 0U;
  }

  std::size_t count = 0U;
  world.for_each<runtime::MeshComponent>(
      [&count, assets](runtime::Entity,
                       const runtime::MeshComponent &mesh) noexcept {
        if (renderer::mesh_asset_state(assets, mesh.meshAssetId) ==
            renderer::AssetState::Ready) {
          ++count;
        }
      });
  return count;
}

struct MeshAssetStateCounts final {
  std::size_t ready = 0U;
  std::size_t loading = 0U;
  std::size_t failed = 0U;
};

MeshAssetStateCounts
count_mesh_asset_states(const renderer::AssetDatabase *assets) noexcept {
  MeshAssetStateCounts counts{};
  if (assets == nullptr) {
    return counts;
  }

  for (std::size_t i = 0U; i < assets->meshAssets.size(); ++i) {
    if (!assets->occupied[i]) {
      continue;
    }

    switch (assets->meshAssets[i].state) {
    case renderer::AssetState::Ready:
      ++counts.ready;
      break;
    case renderer::AssetState::Loading:
      ++counts.loading;
      break;
    case renderer::AssetState::Failed:
      ++counts.failed;
      break;
    case renderer::AssetState::Unloaded:
      break;
    }
  }

  return counts;
}

} // namespace

bool bootstrap() noexcept {
  if (!core::initialize_core(kFrameAllocatorBytes)) {
    return false;
  }

  static_cast<void>(core::cvar_register_bool(
      "r_showStats", true,
      "Toggle in-game stats and profiling overlays in the editor"));

  // Mount the assets directory relative to CWD so that VFS paths like
  // "assets/shaders/pbr.vert" resolve correctly when running from build/.
  static_cast<void>(core::mount("assets", "assets"));

  const runtime::EditorBridge *bridge = runtime::editor_bridge();
  if ((bridge != nullptr) && (bridge->initialize != nullptr)) {
    if (!core::make_render_context_current()) {
      core::log_message(core::LogLevel::Error, "editor",
                        "failed to acquire OpenGL context for editor init");
      core::shutdown_core();
      return false;
    }

    if (!bridge->initialize(core::get_sdl_window(),
                            core::get_sdl_gl_context())) {
      core::log_message(core::LogLevel::Error, "editor",
                        "failed to initialize editor bridge");
      core::release_render_context();
      core::shutdown_core();
      return false;
    }

    core::release_render_context();
  }

  if (!scripting::initialize_scripting()) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "failed to initialize scripting");
    if ((bridge != nullptr) && (bridge->shutdown != nullptr)) {
      bridge->shutdown();
    }
    core::shutdown_core();
    return false;
  }

  if (!audio::initialize_audio()) {
    core::log_message(core::LogLevel::Error, "audio",
                      "failed to initialize audio");
    scripting::shutdown_scripting();
    if ((bridge != nullptr) && (bridge->shutdown != nullptr)) {
      bridge->shutdown();
    }
    core::shutdown_core();
    return false;
  }

  if (g_frameContext == nullptr) {
    g_frameContext.reset(new (std::nothrow) FrameContext());
    if (g_frameContext == nullptr) {
      core::log_message(core::LogLevel::Error, "engine",
                        "failed to allocate frame context");
      scripting::shutdown_scripting();
      if ((bridge != nullptr) && (bridge->shutdown != nullptr)) {
        bridge->shutdown();
      }
      core::shutdown_core();
      return false;
    }
  }

  core::log_message(core::LogLevel::Info, "engine", "bootstrap complete");
  return true;
}

void run(std::uint32_t maxFrames) noexcept {
  std::unique_ptr<runtime::World> world(new (std::nothrow) runtime::World());
  std::unique_ptr<renderer::CommandBufferBuilder> commandBuffer(
      new (std::nothrow) renderer::CommandBufferBuilder());
  std::unique_ptr<renderer::GpuMeshRegistry> meshRegistry(
      new (std::nothrow) renderer::GpuMeshRegistry());
  std::unique_ptr<renderer::AssetDatabase> assetDatabase(
      new (std::nothrow) renderer::AssetDatabase());
  std::unique_ptr<renderer::AssetManager> assetManager(
      new (std::nothrow) renderer::AssetManager());

  if ((world == nullptr) || (commandBuffer == nullptr) ||
      (meshRegistry == nullptr) || (assetDatabase == nullptr) ||
      (assetManager == nullptr)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to allocate runtime frame state");
    return;
  }
  renderer::clear_asset_database(assetDatabase.get());
  renderer::clear_asset_manager(assetManager.get());

  const runtime::EditorBridge *bridge = runtime::editor_bridge();

  runtime::bind_scripting_runtime(world.get());
  if ((bridge != nullptr) && (bridge->set_world != nullptr)) {
    bridge->set_world(world.get());
  }

  // Route physics collision pairs to the Lua on_collision callback.
  runtime::set_collision_dispatch(*world,
                                  &scripting::dispatch_physics_callbacks);

  const char *bootstrapMeshPath = resolve_mesh_asset_path();
  if (bootstrapMeshPath == nullptr) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to resolve mesh asset path");
    return;
  }

  const renderer::AssetId bootstrapMeshAssetId =
      renderer::make_asset_id_from_file(bootstrapMeshPath);
  scripting::set_default_mesh_asset_id(bootstrapMeshAssetId);
  bool bootstrapMeshLoadOk =
      (bootstrapMeshAssetId != renderer::kInvalidAssetId) &&
      renderer::queue_mesh_load(assetManager.get(), assetDatabase.get(),
                                bootstrapMeshAssetId, bootstrapMeshPath);
  if (bootstrapMeshLoadOk) {
    if (!core::make_render_context_current()) {
      core::log_message(
          core::LogLevel::Error, "engine",
          "failed to acquire OpenGL context for bootstrap mesh upload");
      return;
    }

    bootstrapMeshLoadOk = renderer::update_asset_manager(
        assetManager.get(), assetDatabase.get(), meshRegistry.get(), 8U);
    core::release_render_context();
    bootstrapMeshLoadOk = bootstrapMeshLoadOk &&
                          (renderer::mesh_asset_state(assetDatabase.get(),
                                                      bootstrapMeshAssetId) ==
                           renderer::AssetState::Ready);
  }

  if (!bootstrapMeshLoadOk) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to load bootstrap mesh asset");
    return;
  }

  // --- Create procedural built-in meshes (plane, cube, sphere, cylinder,
  // capsule, pyramid). build_gpu_mesh_from_data requires owning the GL
  // context, so acquire it explicitly for this upload block.
  renderer::AssetId planeMeshAssetId = renderer::kInvalidAssetId;
  renderer::AssetId cubeMeshAssetId = renderer::kInvalidAssetId;
  renderer::AssetId sphereMeshAssetId = renderer::kInvalidAssetId;
  renderer::AssetId cylinderMeshAssetId = renderer::kInvalidAssetId;
  renderer::AssetId capsuleMeshAssetId = renderer::kInvalidAssetId;
  renderer::AssetId pyramidMeshAssetId = renderer::kInvalidAssetId;
  if (!core::make_render_context_current()) {
    core::log_message(
        core::LogLevel::Warning, "engine",
        "failed to acquire OpenGL context for procedural mesh upload");
  } else {
    renderer::GpuMesh m{};
    if (build_plane_mesh(&m)) {
      planeMeshAssetId = register_builtin_mesh(
          meshRegistry.get(), assetDatabase.get(), m, "builtin://plane");
    }
    m = renderer::GpuMesh{};
    if (build_cube_mesh(&m)) {
      cubeMeshAssetId = register_builtin_mesh(
          meshRegistry.get(), assetDatabase.get(), m, "builtin://cube");
    }
    m = renderer::GpuMesh{};
    if (build_sphere_mesh(&m)) {
      sphereMeshAssetId = register_builtin_mesh(
          meshRegistry.get(), assetDatabase.get(), m, "builtin://sphere");
    }
    m = renderer::GpuMesh{};
    if (build_cylinder_mesh(&m)) {
      cylinderMeshAssetId = register_builtin_mesh(
          meshRegistry.get(), assetDatabase.get(), m, "builtin://cylinder");
    }
    m = renderer::GpuMesh{};
    if (build_capsule_mesh(&m)) {
      capsuleMeshAssetId = register_builtin_mesh(
          meshRegistry.get(), assetDatabase.get(), m, "builtin://capsule");
    }
    m = renderer::GpuMesh{};
    if (build_pyramid_mesh(&m)) {
      pyramidMeshAssetId = register_builtin_mesh(
          meshRegistry.get(), assetDatabase.get(), m, "builtin://pyramid");
    }
    core::release_render_context();
  }
  // Default mesh for Lua scripts is the cube; fall back to triangle.mesh.
  scripting::set_default_mesh_asset_id(
      (cubeMeshAssetId != renderer::kInvalidAssetId) ? cubeMeshAssetId
                                                     : bootstrapMeshAssetId);
  scripting::set_builtin_mesh_ids(planeMeshAssetId, cubeMeshAssetId,
                                  sphereMeshAssetId, cylinderMeshAssetId,
                                  capsuleMeshAssetId, pyramidMeshAssetId);

  FrameContext *frameContext = g_frameContext.get();
  if (frameContext == nullptr) {
    core::log_message(core::LogLevel::Error, "engine",
                      "frame context not initialized");
    return;
  }

  const std::size_t frameThreadCount = core::thread_frame_allocator_count();
  if ((frameThreadCount == 0U) ||
      (frameThreadCount >
       frameContext->renderPrepPipeline.localCommandBuffers.size())) {
    core::log_message(core::LogLevel::Error, "engine",
                      "invalid thread allocator count");
    return;
  }

  const runtime::Entity entity = world->create_entity();
  const runtime::Entity stackedEntity = world->create_entity();
  const runtime::Entity groundEntity = world->create_entity();
  const runtime::Entity lightEntity = world->create_entity();
  const runtime::Entity sceneControllerEntity = world->create_entity();
  if ((entity == runtime::kInvalidEntity) ||
      (stackedEntity == runtime::kInvalidEntity) ||
      (groundEntity == runtime::kInvalidEntity) ||
      (lightEntity == runtime::kInvalidEntity) ||
      (sceneControllerEntity == runtime::kInvalidEntity)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to create bootstrap entities");
    return;
  }

  // Assign names to bootstrap entities.
  {
    runtime::NameComponent name{};
    std::snprintf(name.name, sizeof(name.name), "Red Cube");
    static_cast<void>(world->add_name_component(entity, name));
  }
  {
    runtime::NameComponent name{};
    std::snprintf(name.name, sizeof(name.name), "Blue Cube");
    static_cast<void>(world->add_name_component(stackedEntity, name));
  }
  {
    runtime::NameComponent name{};
    std::snprintf(name.name, sizeof(name.name), "Ground");
    static_cast<void>(world->add_name_component(groundEntity, name));
  }
  {
    runtime::NameComponent name{};
    std::snprintf(name.name, sizeof(name.name), "Sun Light");
    static_cast<void>(world->add_name_component(lightEntity, name));
  }
  {
    runtime::NameComponent name{};
    std::snprintf(name.name, sizeof(name.name), "Scene Controller");
    static_cast<void>(world->add_name_component(sceneControllerEntity, name));
  }

  // Add directional light.
  {
    runtime::Transform lightTransform{};
    lightTransform.position = math::Vec3(0.0F, 10.0F, 0.0F);
    static_cast<void>(world->add_transform(lightEntity, lightTransform));

    runtime::LightComponent sunLight{};
    sunLight.type = runtime::LightType::Directional;
    sunLight.color = math::Vec3(1.0F, 0.95F, 0.9F);
    sunLight.direction = math::Vec3(0.4F, -1.0F, 0.6F);
    sunLight.intensity = 1.2F;
    static_cast<void>(world->add_light_component(lightEntity, sunLight));
  }

  runtime::Transform transform{};
  transform.position = math::Vec3(0.0F, 1.5F, 0.0F);
  if (!world->add_transform(entity, transform)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add bootstrap transform");
    return;
  }

  runtime::RigidBody rigidBody{};
  rigidBody.velocity = math::Vec3(0.0F, 0.0F, 0.0F);
  rigidBody.inverseMass = 1.0F;
  if (!world->add_rigid_body(entity, rigidBody)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add bootstrap rigid body");
    return;
  }

  runtime::Collider collider{};
  collider.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  if (!world->add_collider(entity, collider)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add bootstrap collider");
    return;
  }

  runtime::MeshComponent meshComponent{};
  meshComponent.meshAssetId = (cubeMeshAssetId != renderer::kInvalidAssetId)
                                  ? cubeMeshAssetId
                                  : bootstrapMeshAssetId;
  meshComponent.albedo = math::Vec3(0.9F, 0.2F, 0.2F);
  if (!world->add_mesh_component(entity, meshComponent)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add bootstrap mesh component");
    return;
  }

  runtime::Transform stackedTransform{};
  stackedTransform.position = math::Vec3(0.0F, 3.0F, 0.0F);
  if (!world->add_transform(stackedEntity, stackedTransform)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add stacked transform");
    return;
  }

  if (!world->add_rigid_body(stackedEntity, rigidBody)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add stacked rigid body");
    return;
  }

  if (!world->add_collider(stackedEntity, collider)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add stacked collider");
    return;
  }

  runtime::MeshComponent stackedMesh{};
  stackedMesh.meshAssetId = (cubeMeshAssetId != renderer::kInvalidAssetId)
                                ? cubeMeshAssetId
                                : bootstrapMeshAssetId;
  stackedMesh.albedo = math::Vec3(0.2F, 0.4F, 0.9F);
  if (!world->add_mesh_component(stackedEntity, stackedMesh)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add stacked mesh component");
    return;
  }

  runtime::Transform groundTransform{};
  groundTransform.position = math::Vec3(0.0F, -0.5F, 0.0F);
  if (!world->add_transform(groundEntity, groundTransform)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add ground transform");
    return;
  }

  runtime::Collider groundCollider{};
  groundCollider.halfExtents = math::Vec3(5.0F, 0.5F, 5.0F);
  if (!world->add_collider(groundEntity, groundCollider)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add ground collider");
    return;
  }

  runtime::MeshComponent groundMesh{};
  groundMesh.meshAssetId = (planeMeshAssetId != renderer::kInvalidAssetId)
                               ? planeMeshAssetId
                               : bootstrapMeshAssetId;
  groundMesh.albedo = math::Vec3(0.45F, 0.42F, 0.38F);
  if (!world->add_mesh_component(groundEntity, groundMesh)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add ground mesh component");
    return;
  }

  // Attach the scene-setup script to the Scene Controller entity.
  // dispatch_entity_scripts_start() will call M.on_start(self) when Play
  // begins.
  {
    runtime::ScriptComponent sceneScript{};
    std::snprintf(sceneScript.scriptPath, sizeof(sceneScript.scriptPath), "%s",
                  kMainScriptPath);
    static_cast<void>(
        world->add_script_component(sceneControllerEntity, sceneScript));
  }

  using Clock = std::chrono::steady_clock;
  auto previousTick = Clock::now();
  double accumulator = 0.0;
  double simulationTimeSeconds = 0.0;
  std::uint32_t frameIndex = 0;
  bool running = true;
  LoopPlayState previousPlayState = query_editor_play_state();
  std::size_t previousAliveCount = world->alive_entity_count();
  core::reset_engine_stats();

  while (running) {
    core::profiler_begin_frame();
    PROFILE_SCOPE("engine_frame");
    const auto frameStart = Clock::now();

    process_input_events_with_editor();

    const LoopPlayState playState = query_editor_play_state();
    if ((playState == LoopPlayState::Playing) &&
        (previousPlayState == LoopPlayState::Stopped)) {
      scripting::dispatch_entity_scripts_start();
    }

    // Fire BeginPlay for entities that haven't received it yet.
    if (playState == LoopPlayState::Playing) {
      world->begin_begin_play_phase();
      scripting::dispatch_entity_scripts_begin_play(world.get());
      world->end_begin_play_phase();
    }

    if ((playState == LoopPlayState::Stopped) &&
        (previousPlayState != LoopPlayState::Stopped)) {
      scripting::dispatch_entity_scripts_end();
      scripting::clear_entity_script_modules();
      scripting::shutdown_scripting();
      if (!scripting::initialize_scripting()) {
        core::log_message(core::LogLevel::Error, "scripting",
                          "failed to reinitialize scripting on stop");
      } else {
        runtime::bind_scripting_runtime(world.get());
        scripting::set_default_mesh_asset_id(
            (cubeMeshAssetId != renderer::kInvalidAssetId)
                ? cubeMeshAssetId
                : bootstrapMeshAssetId);
        scripting::set_builtin_mesh_ids(planeMeshAssetId, cubeMeshAssetId,
                                        sphereMeshAssetId, cylinderMeshAssetId,
                                        capsuleMeshAssetId, pyramidMeshAssetId);
      }

      accumulator = 0.0;
      previousTick = frameStart;
      simulationTimeSeconds = 0.0;
    }

    const bool isPlaying = (playState == LoopPlayState::Playing);
    const bool isPaused = (playState == LoopPlayState::Paused);
    const bool runPhysics = isPlaying;
    const bool runFrameGraph = !isPaused;

    std::size_t updateStepCount = 0U;
    if (isPlaying) {
      const auto now = Clock::now();
      accumulator += std::chrono::duration<double>(now - previousTick).count();
      previousTick = now;

      const double maxAccumulator =
          static_cast<double>(kMaxUpdateStepsPerFrame) * kFixedDeltaSeconds;
      if (accumulator > maxAccumulator) {
        accumulator = maxAccumulator;
      }

      while ((accumulator >= kFixedDeltaSeconds) &&
             (updateStepCount < kMaxUpdateStepsPerFrame)) {
        accumulator -= kFixedDeltaSeconds;
        ++updateStepCount;
      }

      simulationTimeSeconds +=
          static_cast<double>(updateStepCount) * kFixedDeltaSeconds;
    } else {
      accumulator = 0.0;
      previousTick = frameStart;
    }

    scripting::set_frame_index(frameIndex);

    if (isPlaying && (updateStepCount > 0U)) {
      scripting::tick_timers();
      scripting::tick_coroutines();
      scripting::set_frame_time(static_cast<float>(kFixedDeltaSeconds),
                                static_cast<float>(simulationTimeSeconds));
      scripting::dispatch_entity_scripts_update(
          static_cast<float>(kFixedDeltaSeconds));
    }

    scripting::flush_deferred_mutations();

    bool updatedAssets = true;
    if (!core::make_render_context_current()) {
      core::log_message(
          core::LogLevel::Warning, "assets",
          "skipping asset transitions: OpenGL context unavailable");
    } else {
      updatedAssets = renderer::update_asset_manager(
          assetManager.get(), assetDatabase.get(), meshRegistry.get(), 16U);
      core::release_render_context();
    }

    if (!updatedAssets) {
      core::log_message(core::LogLevel::Warning, "assets",
                        "one or more asset transitions failed this frame");
    }

    renderer::check_shader_reload();

    audio::update_audio();

    double frameMs = 0.0;
    double utilizationPct = 0.0;
    core::JobSystemStats jobStats{};

    if (runFrameGraph) {
      if (!core::begin_frame_graph()) {
        core::log_message(core::LogLevel::Error, "engine",
                          "failed to begin frame graph");
        running = false;
        core::profiler_end_frame();
        continue;
      }

      std::size_t updateJobCursor = 0U;
      std::size_t physicsJobCursor = 0U;
      std::size_t phaseJobCursor = 0U;
      frameContext->frameGraphFailed.store(false, std::memory_order_release);

      const bool hasSimulationSteps = updateStepCount > 0U;
      if (hasSimulationSteps) {
        world->begin_update_phase();
      }

      core::JobHandle previousUpdateCommit{};
      bool graphFailed = false;

      for (std::size_t step = 0U; step < updateStepCount; ++step) {
        core::JobHandle commitHandle =
            submit_world_phase_job(frameContext, world.get(), &phaseJobCursor,
                                   &commit_update_phase_job);
        if (!core::is_valid_handle(commitHandle)) {
          graphFailed = true;
          break;
        }

        if (core::is_valid_handle(previousUpdateCommit) &&
            !link_dependency(previousUpdateCommit, commitHandle)) {
          graphFailed = true;
          break;
        }

        if (step > 0U) {
          core::JobHandle beginStepHandle =
              submit_world_phase_job(frameContext, world.get(), &phaseJobCursor,
                                     &begin_update_step_job);
          if (!core::is_valid_handle(beginStepHandle)) {
            graphFailed = true;
            break;
          }
          if (!link_dependency(previousUpdateCommit, beginStepHandle)) {
            graphFailed = true;
            break;
          }
          if (!link_dependency(beginStepHandle, commitHandle)) {
            graphFailed = true;
            break;
          }
        }

        const std::size_t transformCount = world->transform_count();
        const std::size_t updateJobStart = updateJobCursor;

        for (std::size_t start = 0U; start < transformCount;
             start += kChunkSize) {
          if (updateJobCursor >= frameContext->updateJobData.size()) {
            graphFailed = true;
            break;
          }

          const std::size_t count = ((start + kChunkSize) > transformCount)
                                        ? (transformCount - start)
                                        : kChunkSize;

          UpdateChunkJobData &updateData =
              frameContext->updateJobData[updateJobCursor];
          updateData.world = world.get();
          updateData.startIndex = start;
          updateData.count = count;
          updateData.deltaSeconds = static_cast<float>(kFixedDeltaSeconds);

          core::Job updateJob{};
          updateJob.function = &update_chunk_job;
          updateJob.data = &updateData;
          const core::JobHandle updateHandle = core::submit(updateJob);
          if (!core::is_valid_handle(updateHandle)) {
            graphFailed = true;
            break;
          }

          if (core::is_valid_handle(previousUpdateCommit) &&
              !link_dependency(previousUpdateCommit, updateHandle)) {
            graphFailed = true;
            break;
          }

          if (!link_dependency(updateHandle, commitHandle)) {
            graphFailed = true;
            break;
          }

          frameContext->updateJobHandles[updateJobCursor] = updateHandle;
          ++updateJobCursor;
        }

        if (graphFailed) {
          break;
        }

        if (runPhysics) {
          const std::size_t physicsJobStart = physicsJobCursor;
          std::size_t updateHandleIndex = updateJobStart;
          for (std::size_t start = 0U; start < transformCount;
               start += kChunkSize) {
            if ((physicsJobCursor >= frameContext->physicsJobData.size()) ||
                (updateHandleIndex >= updateJobCursor)) {
              graphFailed = true;
              break;
            }

            const std::size_t count = ((start + kChunkSize) > transformCount)
                                          ? (transformCount - start)
                                          : kChunkSize;

            PhysicsChunkJobData &physicsData =
                frameContext->physicsJobData[physicsJobCursor];
            physicsData.world = world.get();
            physicsData.startIndex = start;
            physicsData.count = count;
            physicsData.deltaSeconds = static_cast<float>(kFixedDeltaSeconds);
            physicsData.frameGraphFailed = &frameContext->frameGraphFailed;

            core::Job physicsJob{};
            physicsJob.function = &physics_chunk_job;
            physicsJob.data = &physicsData;
            const core::JobHandle physicsHandle = core::submit(physicsJob);
            if (!core::is_valid_handle(physicsHandle)) {
              graphFailed = true;
              break;
            }

            if (!link_dependency(
                    frameContext->updateJobHandles[updateHandleIndex],
                    physicsHandle)) {
              graphFailed = true;
              break;
            }

            frameContext->physicsJobHandles[physicsJobCursor] = physicsHandle;
            ++physicsJobCursor;
            ++updateHandleIndex;
          }

          if (graphFailed) {
            break;
          }

          frameContext->resolveCollisionsJobData.world = world.get();
          frameContext->resolveCollisionsJobData.frameGraphFailed =
              &frameContext->frameGraphFailed;
          core::Job resolveJob{};
          resolveJob.function = &resolve_collisions_job;
          resolveJob.data = &frameContext->resolveCollisionsJobData;
          const core::JobHandle resolveHandle = core::submit(resolveJob);
          if (!core::is_valid_handle(resolveHandle)) {
            graphFailed = true;
            break;
          }

          for (std::size_t i = physicsJobStart; i < physicsJobCursor; ++i) {
            if (!link_dependency(frameContext->physicsJobHandles[i],
                                 resolveHandle)) {
              graphFailed = true;
              break;
            }
          }

          if (!graphFailed && !link_dependency(resolveHandle, commitHandle)) {
            graphFailed = true;
            break;
          }
        }

        previousUpdateCommit = commitHandle;
      }

      core::JobHandle renderPrepPhaseHandle =
          submit_world_phase_job(frameContext, world.get(), &phaseJobCursor,
                                 &begin_render_prep_phase_job);
      if (!core::is_valid_handle(renderPrepPhaseHandle)) {
        graphFailed = true;
      }

      if (!graphFailed && core::is_valid_handle(previousUpdateCommit) &&
          !link_dependency(previousUpdateCommit, renderPrepPhaseHandle)) {
        graphFailed = true;
      }

      core::JobHandle renderPhaseHandle = submit_world_phase_job(
          frameContext, world.get(), &phaseJobCursor, &begin_render_phase_job);
      if (!core::is_valid_handle(renderPhaseHandle)) {
        graphFailed = true;
      }

      if (!graphFailed &&
          !link_dependency(renderPrepPhaseHandle, renderPhaseHandle)) {
        graphFailed = true;
      }

      core::JobHandle mergeHandle{};

      if (!graphFailed) {
        int vpW = 1;
        int vpH = 1;
        core::render_drawable_size(&vpW, &vpH);
        const float vpAspect =
            (vpH > 0) ? (static_cast<float>(vpW) / static_cast<float>(vpH))
                      : 1.0F;
        const renderer::CameraState cam = renderer::get_active_camera();
        const math::Mat4 vpMatrix =
            math::mul(math::perspective(cam.fovRadians, vpAspect, cam.nearPlane,
                                        cam.farPlane),
                      math::look_at(cam.position, cam.target, cam.up));

        if (!runtime::enqueue_render_prep_pipeline(
                &frameContext->renderPrepPipeline, world.get(),
                commandBuffer.get(), assetDatabase.get(), meshRegistry.get(),
                renderPrepPhaseHandle, renderPhaseHandle,
                &frameContext->frameGraphFailed, frameThreadCount, kChunkSize,
                vpMatrix, &mergeHandle)) {
          graphFailed = true;
        }
      }

      core::JobHandle endFrameHandle = submit_world_phase_job(
          frameContext, world.get(), &phaseJobCursor, &end_frame_phase_job);
      if (!core::is_valid_handle(endFrameHandle)) {
        graphFailed = true;
      }

      if (!graphFailed && !link_dependency(mergeHandle, endFrameHandle)) {
        graphFailed = true;
      }

      if (graphFailed) {
        core::log_message(core::LogLevel::Error, "engine",
                          "job graph assembly failed");
        running = false;
        static_cast<void>(core::end_frame_graph());
        core::profiler_end_frame();
        continue;
      }

      core::wait(endFrameHandle);
      const bool frameJobsFailed =
          frameContext->frameGraphFailed.load(std::memory_order_acquire);
      if (!core::end_frame_graph()) {
        core::log_message(core::LogLevel::Error, "engine",
                          "failed to end frame graph");
        running = false;
        core::profiler_end_frame();
        continue;
      }

      if (frameJobsFailed) {
        core::log_message(core::LogLevel::Error, "engine",
                          "frame graph job execution failed");
        running = false;
        core::profiler_end_frame();
        continue;
      }

      // Dispatch Lua on_collision callbacks for all pairs recorded this frame.
      if (runPhysics) {
        runtime::dispatch_collision_callbacks(*world);
      }

      // Process EndPlay phase: fire callbacks for pending-destroy entities,
      // then flush them. Phase: Input → EndPlay → Input.
      if (isPlaying) {
        world->begin_end_play_phase();
        scripting::dispatch_entity_scripts_end_play(world.get());
        world->end_end_play_phase();
      }

      // Handle deferred scene operations requested from Lua.
      if (scripting::has_pending_scene_op()) {
        if (scripting::pending_scene_op_is_load()) {
          const char *scenePath = scripting::get_pending_scene_path();
          if (scenePath != nullptr) {
            runtime::load_scene(*world, scenePath);
          }
        }
        // new_scene: destroy all entities by resetting world state.
        if (scripting::pending_scene_op_is_new()) {
          runtime::reset_world(*world);
        }
        scripting::clear_pending_scene_op();
      }

      const auto frameGraphEnd = Clock::now();
      frameMs =
          std::chrono::duration<double, std::milli>(frameGraphEnd - frameStart)
              .count();

      jobStats = core::consume_job_stats();
      const auto frameNs = static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(frameGraphEnd -
                                                               frameStart)
              .count());
      const double totalCapacityNs =
          frameNs * static_cast<double>(frameThreadCount);
      utilizationPct =
          (totalCapacityNs > 0.0)
              ? ((100.0 * static_cast<double>(jobStats.busyNanoseconds)) /
                 totalCapacityNs)
              : 0.0;
    } else {
      frameMs =
          std::chrono::duration<double, std::milli>(Clock::now() - frameStart)
              .count();
      jobStats = core::consume_job_stats();
      utilizationPct = 0.0;
    }

    std::size_t threadFrameBytes = 0U;
    std::size_t threadFrameAllocs = 0U;
    for (std::size_t i = 0U; i < frameThreadCount; ++i) {
      threadFrameBytes += core::thread_frame_allocator_bytes_used(i);
      threadFrameAllocs += core::thread_frame_allocator_allocation_count(i);
    }

    core::log_frame_metrics(
        frameIndex, frameMs,
        core::frame_allocator_bytes_used() + threadFrameBytes,
        core::frame_allocator_allocation_count() + threadFrameAllocs);

    const std::size_t aliveCount = world->alive_entity_count();
    const std::size_t spawnedCount = (aliveCount >= previousAliveCount)
                                         ? (aliveCount - previousAliveCount)
                                         : 0U;
    const std::size_t destroyedCount = (previousAliveCount > aliveCount)
                                           ? (previousAliveCount - aliveCount)
                                           : 0U;

    const MeshAssetStateCounts assetCounts =
        count_mesh_asset_states(assetDatabase.get());

    const bool shouldLogSliceDiagnostics =
        ((frameIndex % kSliceDiagnosticsPeriodFrames) == 0U) ||
        (spawnedCount > 0U) || (destroyedCount > 0U) ||
        (assetCounts.failed > 0U);
    if (shouldLogSliceDiagnostics) {
      const std::size_t movingRigidBodyCount =
          count_moving_rigid_bodies(*world);
      const std::size_t meshComponentCount = count_mesh_components(*world);
      const std::size_t readyMeshComponentCount =
          count_ready_mesh_components(*world, assetDatabase.get());
      const std::size_t pendingAssetRequests =
          renderer::pending_asset_request_count(assetManager.get());

      char diagnostics[640] = {};
      std::snprintf(
          diagnostics, sizeof(diagnostics),
          "frame=%u phase=%s alive=%llu spawned=%llu destroyed=%llu "
          "transforms=%llu worldTransforms=%llu movingBodies=%llu "
          "meshComponents=%llu readyMeshComponents=%llu drawCommands=%llu "
          "assetsReady=%llu assetsLoading=%llu assetsFailed=%llu "
          "assetRequests=%llu updateSteps=%llu",
          frameIndex, world_phase_to_string(world->current_phase()),
          static_cast<unsigned long long>(aliveCount),
          static_cast<unsigned long long>(spawnedCount),
          static_cast<unsigned long long>(destroyedCount),
          static_cast<unsigned long long>(world->transform_count()),
          static_cast<unsigned long long>(world->world_transform_count()),
          static_cast<unsigned long long>(movingRigidBodyCount),
          static_cast<unsigned long long>(meshComponentCount),
          static_cast<unsigned long long>(readyMeshComponentCount),
          static_cast<unsigned long long>(commandBuffer->command_count()),
          static_cast<unsigned long long>(assetCounts.ready),
          static_cast<unsigned long long>(assetCounts.loading),
          static_cast<unsigned long long>(assetCounts.failed),
          static_cast<unsigned long long>(pendingAssetRequests),
          static_cast<unsigned long long>(updateStepCount));
      core::log_message(core::LogLevel::Info, "slice", diagnostics);
    }

    renderer::RendererFrameStats rendererStats{};

    if (!core::make_render_context_current()) {
      core::log_message(core::LogLevel::Error, "editor",
                        "failed to acquire OpenGL context for editor");
      running = false;
    } else {
      if ((bridge != nullptr) && (bridge->new_frame != nullptr)) {
        bridge->new_frame();
      }

      // Collect lights from ECS for the PBR shader.
      renderer::SceneLightData sceneLights{};
      const std::size_t lightCount = world->light_count();
      for (std::size_t li = 0U; li < lightCount; ++li) {
        const runtime::LightComponent *lc = world->light_at(li);
        if (lc == nullptr) {
          continue;
        }

        if (lc->type == runtime::LightType::Directional) {
          if (sceneLights.directionalLightCount <
              renderer::kMaxDirectionalLights) {
            auto &dl =
                sceneLights
                    .directionalLights[sceneLights.directionalLightCount];
            dl.direction = lc->direction;
            dl.color = lc->color;
            dl.intensity = lc->intensity;
            ++sceneLights.directionalLightCount;
          }
        } else if (lc->type == runtime::LightType::Point) {
          if (sceneLights.pointLightCount < renderer::kMaxPointLights) {
            const runtime::Entity pointLightEntity = world->light_entity_at(li);
            const runtime::WorldTransform *wt =
                world->get_world_transform_read_ptr(pointLightEntity);

            auto &pl = sceneLights.pointLights[sceneLights.pointLightCount];
            pl.position =
                (wt != nullptr) ? wt->position : math::Vec3(0.0F, 0.0F, 0.0F);
            pl.color = lc->color;
            pl.intensity = lc->intensity;
            ++sceneLights.pointLightCount;
          }
        }
      }

      renderer::flush_renderer(commandBuffer->view(), meshRegistry.get(),
                               static_cast<float>(simulationTimeSeconds),
                               sceneLights);
      rendererStats = renderer::renderer_get_last_frame_stats();
      if ((bridge != nullptr) && (bridge->render != nullptr)) {
        bridge->render(static_cast<float>(frameMs),
                       static_cast<float>(utilizationPct));
      }
      core::swap_render_buffers();
      core::release_render_context();
    }

    core::EngineStats frameStats{};
    frameStats.frameTimeMs = static_cast<float>(frameMs);
    frameStats.fps =
        (frameMs > 0.0) ? static_cast<float>(1000.0 / frameMs) : 0.0F;
    frameStats.drawCalls = rendererStats.drawCalls;
    frameStats.triCount = rendererStats.triangleCount;
    frameStats.entityCount = aliveCount;
    frameStats.memoryUsedMb = static_cast<float>(
        static_cast<double>(core::process_memory_bytes()) / (1024.0 * 1024.0));
    frameStats.gpuSceneMs = rendererStats.gpuSceneMs;
    frameStats.gpuTonemapMs = rendererStats.gpuTonemapMs;
    frameStats.jobUtilizationPct = static_cast<float>(utilizationPct);
    core::set_engine_stats(frameStats);

    char jobMessage[192] = {};
    std::snprintf(
        jobMessage, sizeof(jobMessage),
        "jobs=%llu busyMs=%.3f utilization=%.2f%% queueContention=%llu",
        static_cast<unsigned long long>(jobStats.jobsExecuted),
        static_cast<double>(jobStats.busyNanoseconds) / 1000000.0,
        utilizationPct,
        static_cast<unsigned long long>(jobStats.queueContentionCount));
    core::log_message(core::LogLevel::Trace, "jobs", jobMessage);

    core::reset_frame_allocator();
    core::reset_thread_frame_allocators();

    previousPlayState = playState;
    previousAliveCount = aliveCount;
    ++frameIndex;
    if ((maxFrames != 0U) && (frameIndex >= maxFrames)) {
      running = false;
    }

    if (!core::is_platform_running()) {
      running = false;
    }

    core::profiler_end_frame();
  }

  if ((bridge != nullptr) && (bridge->set_world != nullptr)) {
    bridge->set_world(nullptr);
  }

  renderer::shutdown_asset_manager(assetManager.get(), assetDatabase.get(),
                                   meshRegistry.get());
}

void shutdown() noexcept {
  core::log_message(core::LogLevel::Info, "engine", "shutdown complete");

  const runtime::EditorBridge *bridge = runtime::editor_bridge();

  if ((bridge != nullptr) && (bridge->shutdown != nullptr)) {
    bridge->shutdown();
  }
  renderer::shutdown_renderer();
  audio::shutdown_audio();
  scripting::shutdown_scripting();
  g_frameContext.reset();
  core::shutdown_core();
}

} // namespace engine
