// Declares the navigation mesh (Unity's NavMesh, Godot's NavigationMesh):
// the walkable surfaces of a level baked into convex polygons joined by
// portals, and the queries an agent plans with. The bake samples the level
// column by column through a caller-supplied sampler (the runtime supplies
// one over the physics world's static colliders), keeps the surfaces an
// agent of the settings' radius and height can stand on and climb between,
// and merges them into axis-aligned rectangles. A path is found with A*
// over the rectangles and straightened through their portals with a
// funnel. The bake and every query run in a fixed order with deterministic
// arithmetic, so the same level and settings give the same mesh and paths
// on every platform and at any worker count. A baked mesh is stored as a
// .navmesh document, written and read here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "engine/math/vec3.h"

namespace engine::navigation {

/// What an agent is and where to bake. Lengths are metres.
struct NavBakeSettings final {
  /// The region baked; surfaces outside it are not part of the mesh.
  math::Vec3 boundsMin{};
  math::Vec3 boundsMax{};
  /// The side of one sampled column; smaller is finer and slower.
  float cellSize = 0.25F;
  /// How far an agent keeps from walls and edges.
  float agentRadius = 0.4F;
  /// The headroom an agent needs above a surface.
  float agentHeight = 1.8F;
  /// The highest step an agent walks up or down between neighbours.
  float maxClimb = 0.4F;
  /// The steepest surface an agent stands on, in degrees.
  float maxSlopeDegrees = 45.0F;
};

/// The most columns one bake samples: a 1024 by 1024 grid.
inline constexpr std::size_t kMaxNavColumns = 1024U * 1024U;
/// The most surfaces a column keeps, top first, so an agent can walk on a
/// floor, a bridge over it and a roof above both.
inline constexpr std::size_t kMaxNavLayers = 4U;
/// The longest side, in columns, of one merged polygon.
inline constexpr std::int32_t kMaxNavRectSide = 32;

/// True when `settings` can be baked: finite, positive sizes, a slope
/// from 0 to 89 degrees, bounds with max above min, and no more than
/// kMaxNavColumns columns.
bool nav_bake_settings_are_valid(const NavBakeSettings &settings) noexcept;

/// One surface found in a column: its height, the up component of its
/// unit normal, and the free space above it up to the next solid (or a
/// large value when nothing is above).
struct NavSurface final {
  float y = 0.0F;
  float normalY = 1.0F;
  float clearance = 0.0F;
};

/// Fills `out` with up to `capacity` surfaces of the column at (x, z),
/// highest first, and returns how many. The bake calls it once per column
/// in row order.
using NavColumnSampler = std::size_t (*)(void *context, float x, float z,
                                         NavSurface *out,
                                         std::size_t capacity) noexcept;

/// A convex polygon of the mesh: the columns [x0, x1) by [z0, z1).
struct NavRect final {
  std::int32_t x0 = 0;
  std::int32_t z0 = 0;
  std::int32_t x1 = 0;
  std::int32_t z1 = 0;
  /// Its portals are portals()[firstPortal, firstPortal + portalCount).
  std::uint32_t firstPortal = 0U;
  std::uint32_t portalCount = 0U;
};

/// A shared stretch of edge between two polygons, in world XZ.
struct NavPortal final {
  std::uint32_t neighbor = 0U;
  float ax = 0.0F;
  float az = 0.0F;
  float bx = 0.0F;
  float bz = 0.0F;
};

/// A baked navigation mesh. Empty until bake_nav_mesh fills it.
class NavMesh final {
public:
  NavMesh() noexcept = default;
  NavMesh(const NavMesh &) = delete;
  NavMesh &operator=(const NavMesh &) = delete;
  NavMesh(NavMesh &&) noexcept = default;
  NavMesh &operator=(NavMesh &&) noexcept = default;
  ~NavMesh() = default;

  /// True once a bake has filled it.
  bool empty() const noexcept { return m_rectCount == 0U; }
  const NavBakeSettings &settings() const noexcept { return m_settings; }
  std::int32_t columns_x() const noexcept { return m_columnsX; }
  std::int32_t columns_z() const noexcept { return m_columnsZ; }
  std::size_t rect_count() const noexcept { return m_rectCount; }
  const NavRect *rects() const noexcept { return m_rects.get(); }
  std::size_t portal_count() const noexcept { return m_portalCount; }
  const NavPortal *portals() const noexcept { return m_portals.get(); }

  /// The walkable surfaces of a column, highest first: how many there are
  /// (0 outside the grid), each one's height and the polygon it belongs to.
  std::size_t surface_count(std::int32_t x, std::int32_t z) const noexcept;
  float surface_height(std::int32_t x, std::int32_t z,
                       std::size_t layer) const noexcept;
  std::uint32_t surface_rect(std::int32_t x, std::int32_t z,
                             std::size_t layer) const noexcept;

  /// The column a world position falls in; false outside the grid.
  bool column_of(float worldX, float worldZ, std::int32_t *outX,
                 std::int32_t *outZ) const noexcept;

  /// A 64-bit FNV-1a over the settings and every array, so two bakes can
  /// be compared exactly.
  std::uint64_t content_hash() const noexcept;

private:
  friend bool bake_nav_mesh(const NavBakeSettings &settings,
                            NavColumnSampler sampler, void *context,
                            NavMesh *out) noexcept;
  friend bool read_nav_mesh(const std::uint8_t *data, std::size_t size,
                            NavMesh *out) noexcept;

  NavBakeSettings m_settings{};
  std::int32_t m_columnsX = 0;
  std::int32_t m_columnsZ = 0;
  /// Per column: how many walkable surfaces it keeps.
  std::unique_ptr<std::uint8_t[]> m_surfaceCounts;
  /// Per column and layer: the surface's height and polygon.
  std::unique_ptr<float[]> m_surfaceHeights;
  std::unique_ptr<std::uint32_t[]> m_surfaceRects;
  std::size_t m_rectCount = 0U;
  std::unique_ptr<NavRect[]> m_rects;
  std::size_t m_portalCount = 0U;
  std::unique_ptr<NavPortal[]> m_portals;
};

/// Bakes the level `sampler` describes into `out`. False, logged, with
/// `out` unchanged, for invalid settings, a null sampler or a failed
/// allocation. A level with nothing walkable bakes an empty mesh.
bool bake_nav_mesh(const NavBakeSettings &settings, NavColumnSampler sampler,
                   void *context, NavMesh *out) noexcept;

/// The .navmesh format's version. A file naming another is refused.
inline constexpr std::uint32_t kNavMeshFormatVersion = 1U;

/// Encodes `mesh` as a .navmesh document: a magic and version, the
/// settings, the grid's surfaces, the polygons and the portals, little
/// endian, then a 64-bit FNV-1a of everything before it. The same mesh
/// always encodes to the same bytes. False, logged, for an empty mesh or a
/// failed allocation.
bool write_nav_mesh(const NavMesh &mesh, std::unique_ptr<std::uint8_t[]> *out,
                    std::size_t *outSize) noexcept;

/// Decodes a .navmesh document into `out`, checking the magic, version,
/// checksum, settings and every index and range, so a damaged or
/// hand-edited file is refused rather than trusted. False, logged, with
/// `out` unchanged, when anything does not hold.
bool read_nav_mesh(const std::uint8_t *data, std::size_t size,
                   NavMesh *out) noexcept;

/// What find_path reports.
enum class NavPathResult : std::uint8_t {
  /// A path from start to end.
  Found,
  /// The start or the end is not near the mesh.
  OffMesh,
  /// Both are on the mesh but no walkable route joins them.
  Unreachable,
  /// The path has more corners than the output holds; the output holds
  /// none of them.
  TooLong,
};

/// The scratch a query needs, sized to one mesh, so a query allocates
/// nothing. One query object serves one caller at a time.
class NavQuery final {
public:
  /// Sizes the scratch for `mesh`; false, logged, on a failed allocation.
  /// The query reads the mesh, which must outlive it and stay unchanged.
  bool init(const NavMesh &mesh) noexcept;

  /// The nearest walkable point to `point`: in its column, or within two
  /// columns around it, on a surface no more than the agent height above
  /// or below. False when there is none.
  bool nearest_point(const math::Vec3 &point,
                     math::Vec3 *outPoint) const noexcept;

  /// Finds the shortest route over the polygons from `start` to `end`,
  /// each snapped onto the mesh by nearest_point, straightened through
  /// the portals. On Found, `out` holds its corners from the snapped start
  /// to the snapped end and *outCount their number.
  NavPathResult find_path(const math::Vec3 &start, const math::Vec3 &end,
                          math::Vec3 *out, std::size_t capacity,
                          std::size_t *outCount) noexcept;

private:
  struct Located final {
    std::int32_t x = 0;
    std::int32_t z = 0;
    std::size_t layer = 0U;
    math::Vec3 point{};
  };

  bool locate(const math::Vec3 &point, Located *out) const noexcept;
  float height_at(float worldX, float worldZ, std::uint32_t rect,
                  float fallback) const noexcept;

  const NavMesh *m_mesh = nullptr;
  std::size_t m_capacity = 0U;
  std::unique_ptr<float[]> m_cost;
  std::unique_ptr<std::uint32_t[]> m_parent;
  std::unique_ptr<std::uint32_t[]> m_parentPortal;
  std::unique_ptr<std::uint32_t[]> m_heap;
  std::unique_ptr<float[]> m_heapKeys;
  std::unique_ptr<std::uint8_t[]> m_state;
  std::unique_ptr<std::uint32_t[]> m_route;
  std::unique_ptr<std::uint32_t[]> m_routePortals;
};

} // namespace engine::navigation
