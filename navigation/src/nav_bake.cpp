// Implements the navigation mesh bake: sampling the level column by
// column, keeping the surfaces an agent can stand on, linking neighbours
// an agent can step between, eroding by the agent's radius (a chamfer
// distance, as Recast's), merging the
// rest into rectangles and finding the portals between them; plus the
// mesh's accessors and content hash.

#include "engine/navigation/nav_mesh.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "engine/core/hash.h"
#include "engine/core/logging.h"
#include "engine/math/scalar.h"

namespace engine::navigation {

namespace {

constexpr const char *kLogChannel = "navigation";
constexpr std::uint8_t kNoLink = 0xFFU;
constexpr std::uint32_t kNoRect = 0xFFFFFFFFU;
/// How many surfaces one sample may report before walkability is judged.
constexpr std::size_t kMaxSampledSurfaces = 16U;

/// The four neighbour directions: +x, -x, +z, -z.
constexpr std::int32_t kDirX[4] = {1, -1, 0, 0};
constexpr std::int32_t kDirZ[4] = {0, 0, 1, -1};

bool finite_positive(float value) noexcept {
  return std::isfinite(value) && (value > 0.0F);
}

/// Columns spanning [low, high] at `cell`, rounded up.
std::int64_t column_span(float low, float high, float cell) noexcept {
  return static_cast<std::int64_t>(std::ceil((high - low) / cell));
}

/// The surfaces of the grid while the bake works on them: per column a
/// count and per column-layer a height, with links to the neighbouring
/// surface an agent can step to in each direction.
struct Surfaces final {
  std::int32_t columnsX = 0;
  std::int32_t columnsZ = 0;
  std::unique_ptr<std::uint8_t[]> counts;
  std::unique_ptr<float[]> heights;
  std::unique_ptr<std::uint8_t[]> links;

  std::size_t column(std::int32_t x, std::int32_t z) const noexcept {
    return (static_cast<std::size_t>(z) * static_cast<std::size_t>(columnsX)) +
           static_cast<std::size_t>(x);
  }
  std::size_t slot(std::size_t column, std::size_t layer) const noexcept {
    return (column * kMaxNavLayers) + layer;
  }
  bool inside(std::int32_t x, std::int32_t z) const noexcept {
    return (x >= 0) && (z >= 0) && (x < columnsX) && (z < columnsZ);
  }
  /// The layer of the neighbour in direction `d` linked from (x, z,
  /// layer), or kNoLink.
  std::uint8_t link(std::int32_t x, std::int32_t z, std::size_t layer,
                    int d) const noexcept {
    return links[(slot(column(x, z), layer) * 4U) +
                 static_cast<std::size_t>(d)];
  }
};

/// The layer of column (x, z) closest in height to `y` within `maxClimb`,
/// lowest layer index on a tie; kNoLink when none is.
std::uint8_t closest_layer(const Surfaces &s, std::int32_t x, std::int32_t z,
                           float y, float maxClimb) noexcept {
  const std::size_t column = s.column(x, z);
  std::uint8_t best = kNoLink;
  float bestGap = 0.0F;
  for (std::size_t layer = 0U; layer < s.counts[column]; ++layer) {
    const float gap = std::fabs(s.heights[s.slot(column, layer)] - y);
    if ((gap <= maxClimb) && ((best == kNoLink) || (gap < bestGap))) {
      best = static_cast<std::uint8_t>(layer);
      bestGap = gap;
    }
  }
  return best;
}

/// Links every surface to the neighbour an agent steps to in each
/// direction. A link holds only when each side picks the other, so the
/// graph is symmetric.
void link_surfaces(Surfaces *s, float maxClimb) noexcept {
  for (std::int32_t z = 0; z < s->columnsZ; ++z) {
    for (std::int32_t x = 0; x < s->columnsX; ++x) {
      const std::size_t column = s->column(x, z);
      for (std::size_t layer = 0U; layer < kMaxNavLayers; ++layer) {
        for (int d = 0; d < 4; ++d) {
          s->links[(s->slot(column, layer) * 4U) +
                   static_cast<std::size_t>(d)] = kNoLink;
        }
        if (layer >= s->counts[column]) {
          continue;
        }
        const float y = s->heights[s->slot(column, layer)];
        for (int d = 0; d < 4; ++d) {
          const std::int32_t nx = x + kDirX[d];
          const std::int32_t nz = z + kDirZ[d];
          if (!s->inside(nx, nz)) {
            continue;
          }
          const std::uint8_t there = closest_layer(*s, nx, nz, y, maxClimb);
          if (there == kNoLink) {
            continue;
          }
          const float back = s->heights[s->slot(s->column(nx, nz), there)];
          if (closest_layer(*s, x, z, back, maxClimb) != layer) {
            continue;
          }
          s->links[(s->slot(column, layer) * 4U) +
                   static_cast<std::size_t>(d)] = there;
        }
      }
    }
  }
}

/// Removes the surfaces nearer an edge than `radiusColumns` columns. A
/// surface missing a neighbour in any direction is an edge, and the
/// distance from the edges is a two-pass chamfer over the links, as
/// Recast erodes: 2 per straight step and 3 per diagonal one, so an
/// outside corner is eroded as far as a straight edge rather than the
/// half as far a straight-steps-only distance leaves it.
bool erode(Surfaces *s, std::int32_t radiusColumns, float maxClimb) noexcept {
  if (radiusColumns <= 0) {
    return true;
  }
  const std::size_t slots = static_cast<std::size_t>(s->columnsX) *
                            static_cast<std::size_t>(s->columnsZ) *
                            kMaxNavLayers;
  std::unique_ptr<std::uint16_t[]> distance(new (std::nothrow)
                                                std::uint16_t[slots]);
  if (distance == nullptr) {
    return false;
  }
  constexpr std::uint16_t kFar = 0xFFFFU;
  for (std::int32_t z = 0; z < s->columnsZ; ++z) {
    for (std::int32_t x = 0; x < s->columnsX; ++x) {
      const std::size_t column = s->column(x, z);
      for (std::size_t layer = 0U; layer < kMaxNavLayers; ++layer) {
        bool edge = false;
        for (int d = 0; (d < 4) && (layer < s->counts[column]); ++d) {
          edge = edge || (s->link(x, z, layer, d) == kNoLink);
        }
        distance[s->slot(column, layer)] = edge ? 0U : kFar;
      }
    }
  }
  // Relaxes (x, z, layer) from the neighbour reached by `straight`, and
  // from that neighbour's neighbour reached by `turn` (the diagonal).
  const auto relax = [&](std::int32_t x, std::int32_t z, std::size_t layer,
                         int straight, int turn) noexcept {
    std::uint16_t &here = distance[s->slot(s->column(x, z), layer)];
    const std::uint8_t a = s->link(x, z, layer, straight);
    if (a == kNoLink) {
      return;
    }
    const std::int32_t ax = x + kDirX[straight];
    const std::int32_t az = z + kDirZ[straight];
    const std::uint16_t viaA = distance[s->slot(s->column(ax, az), a)];
    if ((viaA != kFar) && (viaA + 2U < here)) {
      here = static_cast<std::uint16_t>(viaA + 2U);
    }
    const std::uint8_t b = s->link(ax, az, a, turn);
    if (b == kNoLink) {
      return;
    }
    const std::uint16_t viaB =
        distance[s->slot(s->column(ax + kDirX[turn], az + kDirZ[turn]), b)];
    if ((viaB != kFar) && (viaB + 3U < here)) {
      here = static_cast<std::uint16_t>(viaB + 3U);
    }
  };
  // Forward over rows, then backward, each pass reading the neighbours
  // it has already finished.
  for (std::int32_t z = 0; z < s->columnsZ; ++z) {
    for (std::int32_t x = 0; x < s->columnsX; ++x) {
      for (std::size_t layer = 0U; layer < s->counts[s->column(x, z)];
           ++layer) {
        relax(x, z, layer, 1, 3);
        relax(x, z, layer, 3, 0);
      }
    }
  }
  for (std::int32_t z = s->columnsZ - 1; z >= 0; --z) {
    for (std::int32_t x = s->columnsX - 1; x >= 0; --x) {
      for (std::size_t layer = 0U; layer < s->counts[s->column(x, z)];
           ++layer) {
        relax(x, z, layer, 0, 2);
        relax(x, z, layer, 2, 1);
      }
    }
  }
  // Keep the surfaces far enough from every edge, in their order.
  const auto threshold = static_cast<std::uint32_t>(radiusColumns) * 2U;
  for (std::int32_t z = 0; z < s->columnsZ; ++z) {
    for (std::int32_t x = 0; x < s->columnsX; ++x) {
      const std::size_t column = s->column(x, z);
      std::size_t kept = 0U;
      for (std::size_t layer = 0U; layer < s->counts[column]; ++layer) {
        const std::size_t at = s->slot(column, layer);
        if (distance[at] >= threshold) {
          s->heights[s->slot(column, kept)] = s->heights[at];
          ++kept;
        }
      }
      s->counts[column] = static_cast<std::uint8_t>(kept);
    }
  }
  link_surfaces(s, maxClimb);
  return true;
}

void log_failure(const char *reason) noexcept {
  char message[200] = {};
  std::snprintf(message, sizeof(message), "navigation mesh not baked: %s",
                reason);
  core::log_message(core::LogLevel::Error, kLogChannel, message);
}

} // namespace

bool nav_bake_settings_are_valid(const NavBakeSettings &s) noexcept {
  const math::Vec3 &lo = s.boundsMin;
  const math::Vec3 &hi = s.boundsMax;
  if (!std::isfinite(lo.x) || !std::isfinite(lo.y) || !std::isfinite(lo.z) ||
      !std::isfinite(hi.x) || !std::isfinite(hi.y) || !std::isfinite(hi.z) ||
      !(hi.x > lo.x) || !(hi.y > lo.y) || !(hi.z > lo.z) ||
      !finite_positive(s.cellSize) || !finite_positive(s.agentHeight) ||
      !std::isfinite(s.agentRadius) || (s.agentRadius < 0.0F) ||
      !std::isfinite(s.maxClimb) || (s.maxClimb < 0.0F) ||
      !std::isfinite(s.maxSlopeDegrees) || (s.maxSlopeDegrees < 0.0F) ||
      (s.maxSlopeDegrees > 89.0F)) {
    return false;
  }
  const std::int64_t columnsX = column_span(lo.x, hi.x, s.cellSize);
  const std::int64_t columnsZ = column_span(lo.z, hi.z, s.cellSize);
  return (columnsX >= 1) && (columnsZ >= 1) &&
         (columnsX <= static_cast<std::int64_t>(kMaxNavColumns)) &&
         (columnsZ <= static_cast<std::int64_t>(kMaxNavColumns)) &&
         ((columnsX * columnsZ) <= static_cast<std::int64_t>(kMaxNavColumns));
}

bool bake_nav_mesh(const NavBakeSettings &settings, NavColumnSampler sampler,
                   void *context, NavMesh *out) noexcept {
  if (out == nullptr) {
    return false;
  }
  if (sampler == nullptr) {
    log_failure("there is no level to sample");
    return false;
  }
  if (!nav_bake_settings_are_valid(settings)) {
    log_failure("the settings are not finite and positive, the slope is not "
                "0 to 89 degrees, or the bounds hold more than 1048576 "
                "columns");
    return false;
  }
  Surfaces s{};
  s.columnsX = static_cast<std::int32_t>(column_span(
      settings.boundsMin.x, settings.boundsMax.x, settings.cellSize));
  s.columnsZ = static_cast<std::int32_t>(column_span(
      settings.boundsMin.z, settings.boundsMax.z, settings.cellSize));
  const std::size_t columns = static_cast<std::size_t>(s.columnsX) *
                              static_cast<std::size_t>(s.columnsZ);
  const std::size_t slots = columns * kMaxNavLayers;
  s.counts.reset(new (std::nothrow) std::uint8_t[columns]());
  s.heights.reset(new (std::nothrow) float[slots]());
  s.links.reset(new (std::nothrow) std::uint8_t[slots * 4U]());
  if ((s.counts == nullptr) || (s.heights == nullptr) || (s.links == nullptr)) {
    log_failure("there is not enough memory for the grid");
    return false;
  }

  // Sample each column at its centre and keep the surfaces an agent can
  // stand on, highest first.
  const float minNormalY =
      math::det_cos(settings.maxSlopeDegrees * (math::kDetPi / 180.0F));
  NavSurface sampled[kMaxSampledSurfaces] = {};
  for (std::int32_t z = 0; z < s.columnsZ; ++z) {
    for (std::int32_t x = 0; x < s.columnsX; ++x) {
      const float wx = settings.boundsMin.x +
                       ((static_cast<float>(x) + 0.5F) * settings.cellSize);
      const float wz = settings.boundsMin.z +
                       ((static_cast<float>(z) + 0.5F) * settings.cellSize);
      std::size_t found =
          sampler(context, wx, wz, sampled, kMaxSampledSurfaces);
      found = (found < kMaxSampledSurfaces) ? found : kMaxSampledSurfaces;
      // Highest first whatever order the sampler gave, by insertion so
      // equal heights keep their order.
      for (std::size_t i = 1U; i < found; ++i) {
        const NavSurface held = sampled[i];
        std::size_t j = i;
        while ((j > 0U) && (sampled[j - 1U].y < held.y)) {
          sampled[j] = sampled[j - 1U];
          --j;
        }
        sampled[j] = held;
      }
      const std::size_t column = s.column(x, z);
      std::size_t kept = 0U;
      for (std::size_t i = 0U; (i < found) && (kept < kMaxNavLayers); ++i) {
        const NavSurface &surface = sampled[i];
        if (!std::isfinite(surface.y) || !std::isfinite(surface.normalY) ||
            (surface.y < settings.boundsMin.y) ||
            (surface.y > settings.boundsMax.y) ||
            (surface.normalY < minNormalY) ||
            !(surface.clearance >= settings.agentHeight)) {
          continue;
        }
        s.heights[s.slot(column, kept)] = surface.y;
        ++kept;
      }
      s.counts[column] = static_cast<std::uint8_t>(kept);
    }
  }

  link_surfaces(&s, settings.maxClimb);
  const auto radiusColumns = static_cast<std::int32_t>(
      std::ceil((settings.agentRadius / settings.cellSize) - 1.0e-4F));
  if (!erode(&s, radiusColumns, settings.maxClimb)) {
    log_failure("there is not enough memory to erode the grid");
    return false;
  }

  std::size_t surfaceTotal = 0U;
  for (std::size_t column = 0U; column < columns; ++column) {
    surfaceTotal += s.counts[column];
  }
  std::unique_ptr<std::uint32_t[]> rectOf(new (std::nothrow)
                                              std::uint32_t[slots]);
  std::unique_ptr<NavRect[]> rects(
      new (std::nothrow) NavRect[(surfaceTotal > 0U) ? surfaceTotal : 1U]);
  if ((rectOf == nullptr) || (rects == nullptr)) {
    log_failure("there is not enough memory for the polygons");
    return false;
  }
  for (std::size_t i = 0U; i < slots; ++i) {
    rectOf[i] = kNoRect;
  }

  // Merge the surfaces into rectangles: from each unclaimed surface in row
  // order, grow along +x while the next surface is linked and unclaimed,
  // then along +z while a whole row above links to the row below and
  // along itself.
  std::size_t rectCount = 0U;
  std::uint8_t rowLayers[kMaxNavRectSide][kMaxNavRectSide] = {};
  for (std::int32_t z = 0; z < s.columnsZ; ++z) {
    for (std::int32_t x = 0; x < s.columnsX; ++x) {
      for (std::size_t layer = 0U; layer < s.counts[s.column(x, z)]; ++layer) {
        if (rectOf[s.slot(s.column(x, z), layer)] != kNoRect) {
          continue;
        }
        std::int32_t width = 1;
        rowLayers[0][0] = static_cast<std::uint8_t>(layer);
        while ((width < kMaxNavRectSide) && ((x + width) < s.columnsX)) {
          const std::uint8_t next =
              s.link(x + width - 1, z, rowLayers[0][width - 1], 0);
          if ((next == kNoLink) ||
              (rectOf[s.slot(s.column(x + width, z), next)] != kNoRect)) {
            break;
          }
          rowLayers[0][width] = next;
          ++width;
        }
        std::int32_t depth = 1;
        while ((depth < kMaxNavRectSide) && ((z + depth) < s.columnsZ)) {
          bool rowFits = true;
          for (std::int32_t i = 0; rowFits && (i < width); ++i) {
            const std::uint8_t up =
                s.link(x + i, z + depth - 1, rowLayers[depth - 1][i], 2);
            rowFits =
                (up != kNoLink) &&
                (rectOf[s.slot(s.column(x + i, z + depth), up)] == kNoRect) &&
                ((i == 0) || (s.link(x + i - 1, z + depth,
                                     rowLayers[depth][i - 1], 0) == up));
            rowLayers[depth][i] = up;
          }
          if (!rowFits) {
            break;
          }
          ++depth;
        }
        const auto id = static_cast<std::uint32_t>(rectCount);
        for (std::int32_t j = 0; j < depth; ++j) {
          for (std::int32_t i = 0; i < width; ++i) {
            rectOf[s.slot(s.column(x + i, z + j), rowLayers[j][i])] = id;
          }
        }
        NavRect &rect = rects[rectCount++];
        rect.x0 = x;
        rect.z0 = z;
        rect.x1 = x + width;
        rect.z1 = z + depth;
      }
    }
  }

  // Portals: each rectangle walks its four edges in a fixed order and
  // joins the runs of linked surfaces that lead into the same neighbour.
  const auto layer_of = [&](std::int32_t cx, std::int32_t cz,
                            std::uint32_t rect) noexcept -> std::uint8_t {
    const std::size_t column = s.column(cx, cz);
    for (std::size_t layer = 0U; layer < s.counts[column]; ++layer) {
      if (rectOf[s.slot(column, layer)] == rect) {
        return static_cast<std::uint8_t>(layer);
      }
    }
    return kNoLink;
  };
  const auto neighbor_rect = [&](std::int32_t cx, std::int32_t cz,
                                 std::uint32_t rect,
                                 int d) noexcept -> std::uint32_t {
    const std::uint8_t layer = layer_of(cx, cz, rect);
    if (layer == kNoLink) {
      return kNoRect;
    }
    const std::uint8_t there = s.link(cx, cz, layer, d);
    if (there == kNoLink) {
      return kNoRect;
    }
    const std::uint32_t other =
        rectOf[s.slot(s.column(cx + kDirX[d], cz + kDirZ[d]), there)];
    return (other == rect) ? kNoRect : other;
  };
  // Walks every edge run of `rect`, calling `emit(neighbor, run start,
  // run end, d)` once per run; the run's cells are [start, end) along the
  // edge.
  const auto walk_edges = [&](std::uint32_t id, auto &&emit) noexcept {
    const NavRect &rect = rects[id];
    for (int d = 0; d < 4; ++d) {
      const bool alongZ = (d < 2);
      const std::int32_t first = alongZ ? rect.z0 : rect.x0;
      const std::int32_t last = alongZ ? rect.z1 : rect.x1;
      const std::int32_t fixed = (d == 0)   ? (rect.x1 - 1)
                                 : (d == 1) ? rect.x0
                                 : (d == 2) ? (rect.z1 - 1)
                                            : rect.z0;
      std::int32_t runStart = first;
      std::uint32_t runRect = kNoRect;
      for (std::int32_t i = first; i <= last; ++i) {
        const std::uint32_t here =
            (i < last) ? (alongZ ? neighbor_rect(fixed, i, id, d)
                                 : neighbor_rect(i, fixed, id, d))
                       : kNoRect;
        if (here == runRect) {
          continue;
        }
        if (runRect != kNoRect) {
          emit(runRect, runStart, i, d);
        }
        runRect = here;
        runStart = i;
      }
    }
  };
  std::size_t portalTotal = 0U;
  for (std::size_t id = 0U; id < rectCount; ++id) {
    walk_edges(static_cast<std::uint32_t>(id),
               [&](std::uint32_t, std::int32_t, std::int32_t, int) noexcept {
                 ++portalTotal;
               });
  }
  std::unique_ptr<NavPortal[]> portals(
      new (std::nothrow) NavPortal[(portalTotal > 0U) ? portalTotal : 1U]);
  std::unique_ptr<NavRect[]> finalRects(
      new (std::nothrow) NavRect[(rectCount > 0U) ? rectCount : 1U]);
  std::unique_ptr<std::uint8_t[]> counts(new (std::nothrow)
                                             std::uint8_t[columns]());
  std::unique_ptr<float[]> heights(new (std::nothrow) float[slots]());
  std::unique_ptr<std::uint32_t[]> surfaceRects(new (std::nothrow)
                                                    std::uint32_t[slots]);
  if ((portals == nullptr) || (finalRects == nullptr) || (counts == nullptr) ||
      (heights == nullptr) || (surfaceRects == nullptr)) {
    log_failure("there is not enough memory for the portals");
    return false;
  }
  const float cell = settings.cellSize;
  const math::Vec3 &origin = settings.boundsMin;
  std::size_t portalCount = 0U;
  for (std::size_t id = 0U; id < rectCount; ++id) {
    finalRects[id] = rects[id];
    finalRects[id].firstPortal = static_cast<std::uint32_t>(portalCount);
    walk_edges(static_cast<std::uint32_t>(id), [&](std::uint32_t neighbor,
                                                   std::int32_t from,
                                                   std::int32_t to,
                                                   int d) noexcept {
      const NavRect &rect = rects[id];
      NavPortal &portal = portals[portalCount++];
      portal.neighbor = neighbor;
      if (d < 2) {
        const float edgeX =
            origin.x +
            (static_cast<float>((d == 0) ? rect.x1 : rect.x0) * cell);
        portal.ax = edgeX;
        portal.bx = edgeX;
        portal.az = origin.z + (static_cast<float>(from) * cell);
        portal.bz = origin.z + (static_cast<float>(to) * cell);
      } else {
        const float edgeZ =
            origin.z +
            (static_cast<float>((d == 2) ? rect.z1 : rect.z0) * cell);
        portal.az = edgeZ;
        portal.bz = edgeZ;
        portal.ax = origin.x + (static_cast<float>(from) * cell);
        portal.bx = origin.x + (static_cast<float>(to) * cell);
      }
    });
    finalRects[id].portalCount =
        static_cast<std::uint32_t>(portalCount - finalRects[id].firstPortal);
  }
  for (std::size_t column = 0U; column < columns; ++column) {
    counts[column] = s.counts[column];
  }
  for (std::size_t i = 0U; i < slots; ++i) {
    heights[i] = s.heights[i];
    surfaceRects[i] = rectOf[i];
  }

  out->m_settings = settings;
  out->m_columnsX = s.columnsX;
  out->m_columnsZ = s.columnsZ;
  out->m_surfaceCounts = std::move(counts);
  out->m_surfaceHeights = std::move(heights);
  out->m_surfaceRects = std::move(surfaceRects);
  out->m_rectCount = rectCount;
  out->m_rects = std::move(finalRects);
  out->m_portalCount = portalCount;
  out->m_portals = std::move(portals);
  return true;
}

std::size_t NavMesh::surface_count(std::int32_t x,
                                   std::int32_t z) const noexcept {
  if ((m_surfaceCounts == nullptr) || (x < 0) || (z < 0) || (x >= m_columnsX) ||
      (z >= m_columnsZ)) {
    return 0U;
  }
  return m_surfaceCounts[(static_cast<std::size_t>(z) *
                          static_cast<std::size_t>(m_columnsX)) +
                         static_cast<std::size_t>(x)];
}

float NavMesh::surface_height(std::int32_t x, std::int32_t z,
                              std::size_t layer) const noexcept {
  if (layer >= surface_count(x, z)) {
    return 0.0F;
  }
  const std::size_t column =
      (static_cast<std::size_t>(z) * static_cast<std::size_t>(m_columnsX)) +
      static_cast<std::size_t>(x);
  return m_surfaceHeights[(column * kMaxNavLayers) + layer];
}

std::uint32_t NavMesh::surface_rect(std::int32_t x, std::int32_t z,
                                    std::size_t layer) const noexcept {
  if (layer >= surface_count(x, z)) {
    return kNoRect;
  }
  const std::size_t column =
      (static_cast<std::size_t>(z) * static_cast<std::size_t>(m_columnsX)) +
      static_cast<std::size_t>(x);
  return m_surfaceRects[(column * kMaxNavLayers) + layer];
}

bool NavMesh::column_of(float worldX, float worldZ, std::int32_t *outX,
                        std::int32_t *outZ) const noexcept {
  if ((m_columnsX <= 0) || !std::isfinite(worldX) || !std::isfinite(worldZ)) {
    return false;
  }
  const float fx =
      std::floor((worldX - m_settings.boundsMin.x) / m_settings.cellSize);
  const float fz =
      std::floor((worldZ - m_settings.boundsMin.z) / m_settings.cellSize);
  if ((fx < 0.0F) || (fz < 0.0F) || (fx >= static_cast<float>(m_columnsX)) ||
      (fz >= static_cast<float>(m_columnsZ))) {
    return false;
  }
  *outX = static_cast<std::int32_t>(fx);
  *outZ = static_cast<std::int32_t>(fz);
  return true;
}

std::uint64_t NavMesh::content_hash() const noexcept {
  std::uint64_t hash = core::kFnv1a64Offset;
  const auto mix = [&hash](const void *data, std::size_t size) noexcept {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    for (std::size_t i = 0U; i < size; ++i) {
      hash = core::fnv1a_64_append(hash, bytes[i]);
    }
  };
  const float settingsValues[] = {
      m_settings.boundsMin.x, m_settings.boundsMin.y,    m_settings.boundsMin.z,
      m_settings.boundsMax.x, m_settings.boundsMax.y,    m_settings.boundsMax.z,
      m_settings.cellSize,    m_settings.agentRadius,    m_settings.agentHeight,
      m_settings.maxClimb,    m_settings.maxSlopeDegrees};
  mix(settingsValues, sizeof(settingsValues));
  mix(&m_columnsX, sizeof(m_columnsX));
  mix(&m_columnsZ, sizeof(m_columnsZ));
  const std::size_t columns = static_cast<std::size_t>(m_columnsX) *
                              static_cast<std::size_t>(m_columnsZ);
  if (m_surfaceCounts != nullptr) {
    mix(m_surfaceCounts.get(), columns);
    for (std::size_t column = 0U; column < columns; ++column) {
      const std::size_t count = m_surfaceCounts[column];
      mix(&m_surfaceHeights[column * kMaxNavLayers], count * sizeof(float));
      mix(&m_surfaceRects[column * kMaxNavLayers],
          count * sizeof(std::uint32_t));
    }
  }
  for (std::size_t i = 0U; i < m_rectCount; ++i) {
    const NavRect &r = m_rects[i];
    const std::int32_t fields[] = {r.x0, r.z0, r.x1, r.z1};
    mix(fields, sizeof(fields));
    mix(&r.firstPortal, sizeof(r.firstPortal));
    mix(&r.portalCount, sizeof(r.portalCount));
  }
  for (std::size_t i = 0U; i < m_portalCount; ++i) {
    const NavPortal &p = m_portals[i];
    mix(&p.neighbor, sizeof(p.neighbor));
    const float ends[] = {p.ax, p.az, p.bx, p.bz};
    mix(ends, sizeof(ends));
  }
  return hash;
}

} // namespace engine::navigation
