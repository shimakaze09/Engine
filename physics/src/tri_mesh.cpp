// Implements the triangle mesh: building it (dropping triangles with no
// area, then a median-split BVH whose layout depends only on the input),
// its counted reference, and the box, ray and closest-point queries the
// narrow phase and the scene queries run against it.

#include "engine/physics/tri_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/mem_tracker.h"

namespace engine::physics {

struct TriMeshBuilder final {
  static TriMeshRef adopt(TriMeshData *data) noexcept {
    return TriMeshRef(data);
  }
};

namespace {

/// Deeper than a median split of kMaxTriangles ever goes; bounds the
/// traversal stacks.
constexpr std::size_t kMaxBvhDepth = 64U;

bool finite(const math::Vec3 &value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

float axis_of(const math::Vec3 &value, int axis) noexcept {
  return (axis == 0) ? value.x : ((axis == 1) ? value.y : value.z);
}

math::AABB empty_box() noexcept {
  return math::AABB{math::Vec3(INFINITY, INFINITY, INFINITY),
                    math::Vec3(-INFINITY, -INFINITY, -INFINITY)};
}

void grow(math::AABB &box, const math::Vec3 &point) noexcept {
  box.min =
      math::Vec3(std::min(box.min.x, point.x), std::min(box.min.y, point.y),
                 std::min(box.min.z, point.z));
  box.max =
      math::Vec3(std::max(box.max.x, point.x), std::max(box.max.y, point.y),
                 std::max(box.max.z, point.z));
}

bool boxes_meet(const math::AABB &a, const math::AABB &b) noexcept {
  return (a.min.x <= b.max.x) && (a.max.x >= b.min.x) && (a.min.y <= b.max.y) &&
         (a.max.y >= b.min.y) && (a.min.z <= b.max.z) && (a.max.z >= b.min.z);
}

/// One triangle being sorted into the BVH: its source order breaks ties,
/// and its centroid (times three, to keep the comparison free of a
/// division) decides its side of a split.
struct BuildEntry final {
  TriMeshTriangle triangle{};
  math::Vec3 centroid3{};
  std::uint32_t source = 0U;
};

struct BuildState final {
  BuildEntry *entries = nullptr;
  const math::Vec3 *vertices = nullptr;
  TriMeshNode *nodes = nullptr;
  std::size_t nodeCount = 0U;
};

math::AABB entry_bounds(const BuildState &state, std::size_t first,
                        std::size_t count) noexcept {
  math::AABB box = empty_box();
  for (std::size_t i = first; i < (first + count); ++i) {
    for (const std::uint32_t corner : state.entries[i].triangle.v) {
      grow(box, state.vertices[corner]);
    }
  }
  return box;
}

/// Builds the subtree over entries [first, first + count) at node
/// `nodeIndex`. A split puts the lower half by centroid, ties broken by
/// source order, on the left: a strict total order, so the halves hold the
/// same triangles whatever the standard library's selection does.
void build_node(BuildState &state, std::size_t nodeIndex, std::size_t first,
                std::size_t count, std::size_t depth) noexcept {
  TriMeshNode &node = state.nodes[nodeIndex];
  node.bounds = entry_bounds(state, first, count);
  BuildEntry *begin = state.entries + first;
  if ((count <= TriMeshData::kLeafTriangles) || (depth + 1U >= kMaxBvhDepth)) {
    // A leaf's triangles in source order, so its layout is fixed too.
    std::sort(begin, begin + count,
              [](const BuildEntry &a, const BuildEntry &b) {
                return a.source < b.source;
              });
    node.first = static_cast<std::uint32_t>(first);
    node.count = static_cast<std::uint32_t>(count);
    return;
  }
  math::AABB centroids = empty_box();
  for (std::size_t i = 0U; i < count; ++i) {
    grow(centroids, begin[i].centroid3);
  }
  const math::Vec3 extent = math::sub(centroids.max, centroids.min);
  int axis = 0;
  if (extent.y > axis_of(extent, axis)) {
    axis = 1;
  }
  if (extent.z > axis_of(extent, axis)) {
    axis = 2;
  }
  const std::size_t half = count / 2U;
  std::nth_element(begin, begin + half, begin + count,
                   [axis](const BuildEntry &a, const BuildEntry &b) {
                     const float ka = axis_of(a.centroid3, axis);
                     const float kb = axis_of(b.centroid3, axis);
                     return (ka < kb) || ((ka == kb) && (a.source < b.source));
                   });
  const std::size_t left = state.nodeCount++;
  build_node(state, left, first, half, depth + 1U);
  const std::size_t right = state.nodeCount++;
  build_node(state, right, first + half, count - half, depth + 1U);
  // The left child is always the next node; record the right one.
  node.first = static_cast<std::uint32_t>(right);
  node.count = 0U;
}

/// Frees a mesh's arrays and tells the tracker.
void free_arrays(math::Vec3 *vertices, std::size_t vertexCount,
                 TriMeshTriangle *triangles, std::size_t triangleCount,
                 TriMeshNode *nodes, std::size_t nodeCapacity) noexcept {
  delete[] vertices;
  delete[] triangles;
  delete[] nodes;
  core::mem_tracker_free(core::MemTag::Physics,
                         (vertexCount * sizeof(math::Vec3)) +
                             (triangleCount * sizeof(TriMeshTriangle)) +
                             (nodeCapacity * sizeof(TriMeshNode)));
}

} // namespace

TriMeshData::~TriMeshData() {
  free_arrays(vertices_, vertexCount_, triangles_, triangleCount_, nodes_,
              nodeCapacity_);
}

TriMeshRef::TriMeshRef(TriMeshData *adopted) noexcept : data_(adopted) {
  if (data_ != nullptr) {
    data_->references_.fetch_add(1U, std::memory_order_relaxed);
  }
}

TriMeshRef::~TriMeshRef() { reset(); }

TriMeshRef::TriMeshRef(const TriMeshRef &other) noexcept : data_(other.data_) {
  if (data_ != nullptr) {
    data_->references_.fetch_add(1U, std::memory_order_relaxed);
  }
}

TriMeshRef::TriMeshRef(TriMeshRef &&other) noexcept : data_(other.data_) {
  other.data_ = nullptr;
}

TriMeshRef &TriMeshRef::operator=(const TriMeshRef &other) noexcept {
  if (this != &other) {
    TriMeshRef copy(other);
    reset();
    data_ = copy.data_;
    copy.data_ = nullptr;
  }
  return *this;
}

TriMeshRef &TriMeshRef::operator=(TriMeshRef &&other) noexcept {
  if (this != &other) {
    reset();
    data_ = other.data_;
    other.data_ = nullptr;
  }
  return *this;
}

void TriMeshRef::reset() noexcept {
  if (data_ == nullptr) {
    return;
  }
  // Acquire-release, so the last owner sees every other owner's reads
  // complete before it frees the arrays.
  if (data_->references_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
    delete data_;
  }
  data_ = nullptr;
}

const char *tri_mesh_build_result_text(TriMeshBuildResult result) noexcept {
  switch (result) {
  case TriMeshBuildResult::Ok:
    return "built";
  case TriMeshBuildResult::Empty:
    return "the mesh has no triangles";
  case TriMeshBuildResult::TooManyVertices:
    return "the mesh has more vertices than a collision mesh holds";
  case TriMeshBuildResult::TooManyTriangles:
    return "the mesh has more triangles than a collision mesh holds; split "
           "it into several colliders";
  case TriMeshBuildResult::IndexOutOfRange:
    return "a triangle names a vertex the mesh does not have";
  case TriMeshBuildResult::NonFinite:
    return "a vertex is not a finite position";
  case TriMeshBuildResult::AllDegenerate:
    return "every triangle has no area";
  case TriMeshBuildResult::OutOfMemory:
    return "out of memory";
  }
  return "unknown result";
}

TriMeshBuildResult build_tri_mesh(const math::Vec3 *vertices,
                                  std::size_t vertexCount,
                                  const std::uint32_t *indices,
                                  std::size_t indexCount,
                                  TriMeshRef *outMesh) noexcept {
  if ((vertices == nullptr) || (indices == nullptr) || (outMesh == nullptr) ||
      (indexCount < 3U) || ((indexCount % 3U) != 0U) || (vertexCount == 0U)) {
    return TriMeshBuildResult::Empty;
  }
  if (vertexCount > TriMeshData::kMaxVertices) {
    return TriMeshBuildResult::TooManyVertices;
  }
  const std::size_t sourceTriangles = indexCount / 3U;
  if (sourceTriangles > TriMeshData::kMaxTriangles) {
    return TriMeshBuildResult::TooManyTriangles;
  }
  for (std::size_t i = 0U; i < vertexCount; ++i) {
    if (!finite(vertices[i])) {
      return TriMeshBuildResult::NonFinite;
    }
  }
  for (std::size_t i = 0U; i < indexCount; ++i) {
    if (indices[i] >= vertexCount) {
      return TriMeshBuildResult::IndexOutOfRange;
    }
  }

  std::unique_ptr<BuildEntry[]> entries(new (std::nothrow)
                                            BuildEntry[sourceTriangles]);
  if (entries == nullptr) {
    return TriMeshBuildResult::OutOfMemory;
  }
  // A triangle is dropped when its area is negligible at the mesh's own
  // scale: twice its area against the square of its longest edge.
  std::size_t kept = 0U;
  for (std::size_t t = 0U; t < sourceTriangles; ++t) {
    const std::uint32_t i0 = indices[(t * 3U) + 0U];
    const std::uint32_t i1 = indices[(t * 3U) + 1U];
    const std::uint32_t i2 = indices[(t * 3U) + 2U];
    const math::Vec3 &a = vertices[i0];
    const math::Vec3 &b = vertices[i1];
    const math::Vec3 &c = vertices[i2];
    const math::Vec3 ab = math::sub(b, a);
    const math::Vec3 ac = math::sub(c, a);
    const math::Vec3 bc = math::sub(c, b);
    const float twiceArea = math::length(math::cross(ab, ac));
    const float longest =
        std::max(math::length_sq(ab),
                 std::max(math::length_sq(ac), math::length_sq(bc)));
    constexpr float kMinShapeRatio = 1.0e-6F;
    if (!(twiceArea > (kMinShapeRatio * longest)) || !(longest > 0.0F)) {
      continue;
    }
    BuildEntry &entry = entries[kept++];
    entry.triangle.v[0] = i0;
    entry.triangle.v[1] = i1;
    entry.triangle.v[2] = i2;
    entry.centroid3 = math::add(math::add(a, b), c);
    entry.source = static_cast<std::uint32_t>(t);
  }
  if (kept == 0U) {
    return TriMeshBuildResult::AllDegenerate;
  }

  // Every leaf holds at least one triangle and an interior node has two
  // children, so a tree over `kept` triangles has fewer than 2 * kept nodes.
  const std::size_t nodeCapacity = 2U * kept;
  auto *data = new (std::nothrow) TriMeshData();
  auto *ownedVertices = new (std::nothrow) math::Vec3[vertexCount];
  auto *ownedTriangles = new (std::nothrow) TriMeshTriangle[kept];
  auto *nodes = new (std::nothrow) TriMeshNode[nodeCapacity];
  core::mem_tracker_alloc(core::MemTag::Physics,
                          (vertexCount * sizeof(math::Vec3)) +
                              (kept * sizeof(TriMeshTriangle)) +
                              (nodeCapacity * sizeof(TriMeshNode)));
  if ((data == nullptr) || (ownedVertices == nullptr) ||
      (ownedTriangles == nullptr) || (nodes == nullptr)) {
    delete data;
    free_arrays(ownedVertices, vertexCount, ownedTriangles, kept, nodes,
                nodeCapacity);
    return TriMeshBuildResult::OutOfMemory;
  }
  std::memcpy(ownedVertices, vertices, vertexCount * sizeof(math::Vec3));

  BuildState state{};
  state.entries = entries.get();
  state.vertices = ownedVertices;
  state.nodes = nodes;
  state.nodeCount = 1U;
  build_node(state, 0U, 0U, kept, 0U);
  for (std::size_t i = 0U; i < kept; ++i) {
    ownedTriangles[i] = entries[i].triangle;
  }

  data->vertices_ = ownedVertices;
  data->vertexCount_ = vertexCount;
  data->triangles_ = ownedTriangles;
  data->triangleCount_ = kept;
  data->nodes_ = nodes;
  data->nodeCount_ = state.nodeCount;
  data->nodeCapacity_ = nodeCapacity;
  data->localBounds_ = nodes[0].bounds;
  data->droppedDegenerate_ = sourceTriangles - kept;
  *outMesh = TriMeshBuilder::adopt(data);
  return TriMeshBuildResult::Ok;
}

std::size_t collect_tri_mesh_triangles(const TriMeshData &mesh,
                                       const math::AABB &localBox,
                                       std::uint32_t *outTriangles,
                                       std::size_t maxTriangles) noexcept {
  if ((mesh.triangle_count() == 0U) ||
      !boxes_meet(mesh.local_bounds(), localBox)) {
    return 0U;
  }
  std::uint32_t stack[kMaxBvhDepth * 2U] = {};
  std::size_t top = 0U;
  stack[top++] = 0U;
  std::size_t found = 0U;
  // Left before right, and leaves hold contiguous ascending ranges with the
  // left range first, so triangles come out in ascending order.
  while (top > 0U) {
    const TriMeshNode &node = mesh.nodes()[stack[--top]];
    if (!boxes_meet(node.bounds, localBox)) {
      continue;
    }
    if (node.count > 0U) {
      for (std::uint32_t i = node.first; i < (node.first + node.count); ++i) {
        math::AABB triangleBox = empty_box();
        grow(triangleBox, mesh.corner(i, 0U));
        grow(triangleBox, mesh.corner(i, 1U));
        grow(triangleBox, mesh.corner(i, 2U));
        if (!boxes_meet(triangleBox, localBox)) {
          continue;
        }
        if ((outTriangles != nullptr) && (found < maxTriangles)) {
          outTriangles[found] = i;
        }
        ++found;
      }
      continue;
    }
    const std::uint32_t self = static_cast<std::uint32_t>(&node - mesh.nodes());
    stack[top++] = node.first;
    stack[top++] = self + 1U;
  }
  return found;
}

bool ray_triangle_intersect(const math::Vec3 &origin,
                            const math::Vec3 &direction, const math::Vec3 &a,
                            const math::Vec3 &b, const math::Vec3 &c,
                            float maxT, float *outT) noexcept {
  const math::Vec3 edgeAB = math::sub(b, a);
  const math::Vec3 edgeAC = math::sub(c, a);
  const math::Vec3 crossDirection = math::cross(direction, edgeAC);
  const float determinant = math::dot(edgeAB, crossDirection);
  if (std::fabs(determinant) <= 1.0e-10F) {
    return false;
  }
  const float inverseDeterminant = 1.0F / determinant;
  const math::Vec3 originOffset = math::sub(origin, a);
  const float u = math::dot(originOffset, crossDirection) * inverseDeterminant;
  if ((u < 0.0F) || (u > 1.0F)) {
    return false;
  }
  const math::Vec3 crossOffset = math::cross(originOffset, edgeAB);
  const float v = math::dot(direction, crossOffset) * inverseDeterminant;
  if ((v < 0.0F) || ((u + v) > 1.0F)) {
    return false;
  }
  const float candidate = math::dot(edgeAC, crossOffset) * inverseDeterminant;
  if ((candidate < 0.0F) || (candidate > maxT)) {
    return false;
  }
  if (outT != nullptr) {
    *outT = candidate;
  }
  return true;
}

namespace {

/// Whether the ray meets `box` at a distance within [0, maxT].
bool ray_meets_box(const math::Vec3 &origin, const math::Vec3 &inverse,
                   const math::AABB &box, float maxT) noexcept {
  float near = 0.0F;
  float far = maxT;
  for (int axis = 0; axis < 3; ++axis) {
    const float o = axis_of(origin, axis);
    const float inv = axis_of(inverse, axis);
    float t0 = (axis_of(box.min, axis) - o) * inv;
    float t1 = (axis_of(box.max, axis) - o) * inv;
    if (std::isnan(t0) || std::isnan(t1)) {
      // A zero direction component with the origin on a slab plane.
      if ((o < axis_of(box.min, axis)) || (o > axis_of(box.max, axis))) {
        return false;
      }
      continue;
    }
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    near = std::max(near, t0);
    far = std::min(far, t1);
    if (near > far) {
      return false;
    }
  }
  return true;
}

} // namespace

bool raycast_tri_mesh(const TriMeshData &mesh, const math::Vec3 &origin,
                      const math::Vec3 &direction, float maxT,
                      TriMeshRayHit *outHit) noexcept {
  if ((mesh.triangle_count() == 0U) || !finite(origin) || !finite(direction) ||
      !(maxT >= 0.0F) || (math::length_sq(direction) <= 0.0F)) {
    return false;
  }
  const math::Vec3 inverse(1.0F / direction.x, 1.0F / direction.y,
                           1.0F / direction.z);
  std::uint32_t stack[kMaxBvhDepth * 2U] = {};
  std::size_t top = 0U;
  stack[top++] = 0U;
  bool hit = false;
  float bestT = maxT;
  std::uint32_t bestTriangle = 0U;
  while (top > 0U) {
    const TriMeshNode &node = mesh.nodes()[stack[--top]];
    if (!ray_meets_box(origin, inverse, node.bounds, bestT)) {
      continue;
    }
    if (node.count > 0U) {
      for (std::uint32_t i = node.first; i < (node.first + node.count); ++i) {
        float t = 0.0F;
        if (!ray_triangle_intersect(origin, direction, mesh.corner(i, 0U),
                                    mesh.corner(i, 1U), mesh.corner(i, 2U),
                                    bestT, &t)) {
          continue;
        }
        if (!hit || (t < bestT) || ((t == bestT) && (i < bestTriangle))) {
          hit = true;
          bestT = t;
          bestTriangle = i;
        }
      }
      continue;
    }
    const std::uint32_t self = static_cast<std::uint32_t>(&node - mesh.nodes());
    stack[top++] = node.first;
    stack[top++] = self + 1U;
  }
  if (!hit) {
    return false;
  }
  if (outHit != nullptr) {
    const math::Vec3 &a = mesh.corner(bestTriangle, 0U);
    math::Vec3 normal = math::normalize(
        math::cross(math::sub(mesh.corner(bestTriangle, 1U), a),
                    math::sub(mesh.corner(bestTriangle, 2U), a)));
    if (math::dot(normal, direction) > 0.0F) {
      normal = math::mul(normal, -1.0F);
    }
    outHit->t = bestT;
    outHit->triangle = bestTriangle;
    outHit->normal = normal;
  }
  return true;
}

math::Vec3 closest_point_on_triangle(const math::Vec3 &p, const math::Vec3 &a,
                                     const math::Vec3 &b,
                                     const math::Vec3 &c) noexcept {
  // Voronoi region projection (Christer Ericson, Real-Time Collision
  // Detection, 5.1.5).
  const math::Vec3 ab = math::sub(b, a);
  const math::Vec3 ac = math::sub(c, a);
  const math::Vec3 ap = math::sub(p, a);
  const float d1 = math::dot(ab, ap);
  const float d2 = math::dot(ac, ap);
  if (d1 <= 0.0F && d2 <= 0.0F) {
    return a;
  }
  const math::Vec3 bp = math::sub(p, b);
  const float d3 = math::dot(ab, bp);
  const float d4 = math::dot(ac, bp);
  if (d3 >= 0.0F && d4 <= d3) {
    return b;
  }
  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
    const float v = d1 / (d1 - d3);
    return math::add(a, math::mul(ab, v));
  }
  const math::Vec3 cp = math::sub(p, c);
  const float d5 = math::dot(ab, cp);
  const float d6 = math::dot(ac, cp);
  if (d6 >= 0.0F && d5 <= d6) {
    return c;
  }
  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
    const float w = d2 / (d2 - d6);
    return math::add(a, math::mul(ac, w));
  }
  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
    const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return math::add(b, math::mul(math::sub(c, b), w));
  }
  const float denom = 1.0F / (va + vb + vc);
  const float v2 = vb * denom;
  const float w2 = vc * denom;
  return math::add(a, math::add(math::mul(ab, v2), math::mul(ac, w2)));
}

math::Vec3
closest_point_on_tri_mesh_triangle(const TriMeshData &mesh,
                                   std::uint32_t triangle,
                                   const math::Vec3 &point) noexcept {
  return closest_point_on_triangle(point, mesh.corner(triangle, 0U),
                                   mesh.corner(triangle, 1U),
                                   mesh.corner(triangle, 2U));
}

} // namespace engine::physics
