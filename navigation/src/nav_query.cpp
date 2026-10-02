// Implements the navigation queries: snapping a point onto the mesh, A*
// over the mesh's rectangles from one to another, and the funnel that
// straightens the route through the portals into a path of corners.

#include "engine/navigation/nav_mesh.h"

#include <cmath>
#include <new>

#include "engine/core/logging.h"

namespace engine::navigation {

namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFU;
constexpr std::uint8_t kUnseen = 0U;
constexpr std::uint8_t kOpen = 1U;
constexpr std::uint8_t kClosed = 2U;
/// How many columns around a point nearest_point searches.
constexpr std::int32_t kSnapColumns = 2;

/// A point in the XZ plane the funnel works in.
struct Point2 final {
  float x = 0.0F;
  float z = 0.0F;
};

bool same(const Point2 &a, const Point2 &b) noexcept {
  return (a.x == b.x) && (a.z == b.z);
}

/// Twice the signed area of (a, b, c) in XZ, Recast's dtTriArea2D: positive
/// when c is to the right of a->b in the funnel's sense (+x of a step
/// along +z).
float area2(const Point2 &a, const Point2 &b, const Point2 &c) noexcept {
  const float abx = b.x - a.x;
  const float abz = b.z - a.z;
  const float acx = c.x - a.x;
  const float acz = c.z - a.z;
  return (acx * abz) - (abx * acz);
}

float distance_xz(float ax, float az, float bx, float bz) noexcept {
  const float dx = bx - ax;
  const float dz = bz - az;
  return std::sqrt((dx * dx) + (dz * dz));
}

Point2 rect_center(const NavMesh &mesh, const NavRect &rect) noexcept {
  const NavBakeSettings &s = mesh.settings();
  return Point2{s.boundsMin.x +
                    (static_cast<float>(rect.x0 + rect.x1) * 0.5F * s.cellSize),
                s.boundsMin.z + (static_cast<float>(rect.z0 + rect.z1) * 0.5F *
                                 s.cellSize)};
}

/// The open list: a binary min-heap of (estimate, rectangle) entries, ties
/// to the lower rectangle, so the order never depends on how the heap was
/// filled. A rectangle whose estimate improves is pushed again; the stale
/// entry is skipped when it surfaces.
struct Heap final {
  std::uint32_t *items = nullptr;
  float *keys = nullptr;
  std::size_t size = 0U;

  bool before(std::size_t a, std::size_t b) const noexcept {
    return (keys[a] < keys[b]) ||
           ((keys[a] == keys[b]) && (items[a] < items[b]));
  }
  void swap_at(std::size_t a, std::size_t b) noexcept {
    const std::uint32_t item = items[a];
    items[a] = items[b];
    items[b] = item;
    const float key = keys[a];
    keys[a] = keys[b];
    keys[b] = key;
  }
  void push(std::uint32_t item, float key) noexcept {
    std::size_t at = size++;
    items[at] = item;
    keys[at] = key;
    while (at > 0U) {
      const std::size_t parent = (at - 1U) / 2U;
      if (!before(at, parent)) {
        break;
      }
      swap_at(at, parent);
      at = parent;
    }
  }
  std::uint32_t pop() noexcept {
    const std::uint32_t top = items[0];
    --size;
    items[0] = items[size];
    keys[0] = keys[size];
    std::size_t at = 0U;
    for (;;) {
      const std::size_t left = (at * 2U) + 1U;
      const std::size_t right = left + 1U;
      std::size_t best = at;
      if ((left < size) && before(left, best)) {
        best = left;
      }
      if ((right < size) && before(right, best)) {
        best = right;
      }
      if (best == at) {
        break;
      }
      swap_at(at, best);
      at = best;
    }
    return top;
  }
};

} // namespace

bool NavQuery::init(const NavMesh &mesh) noexcept {
  const std::size_t rects = (mesh.rect_count() > 0U) ? mesh.rect_count() : 1U;
  // A rectangle enters the open list once per portal into it at most, so
  // the heap holds one entry per portal plus the start.
  const std::size_t heap = mesh.portal_count() + 1U;
  m_cost.reset(new (std::nothrow) float[rects]());
  m_parent.reset(new (std::nothrow) std::uint32_t[rects]());
  m_parentPortal.reset(new (std::nothrow) std::uint32_t[rects]());
  m_state.reset(new (std::nothrow) std::uint8_t[rects]());
  m_route.reset(new (std::nothrow) std::uint32_t[rects]());
  m_routePortals.reset(new (std::nothrow) std::uint32_t[rects]());
  m_heap.reset(new (std::nothrow) std::uint32_t[heap]());
  m_heapKeys.reset(new (std::nothrow) float[heap]());
  if ((m_cost == nullptr) || (m_heapKeys == nullptr) || (m_parent == nullptr) ||
      (m_parentPortal == nullptr) || (m_state == nullptr) ||
      (m_route == nullptr) || (m_routePortals == nullptr) ||
      (m_heap == nullptr)) {
    core::log_message(core::LogLevel::Error, "navigation",
                      "navigation query not ready: there is not enough "
                      "memory for its scratch");
    m_mesh = nullptr;
    m_capacity = 0U;
    return false;
  }
  m_mesh = &mesh;
  m_capacity = rects;
  return true;
}

bool NavQuery::locate(const math::Vec3 &point, Located *out) const noexcept {
  if ((m_mesh == nullptr) || m_mesh->empty() || !std::isfinite(point.x) ||
      !std::isfinite(point.y) || !std::isfinite(point.z)) {
    return false;
  }
  const NavBakeSettings &s = m_mesh->settings();
  const float fx = std::floor((point.x - s.boundsMin.x) / s.cellSize);
  const float fz = std::floor((point.z - s.boundsMin.z) / s.cellSize);
  const float limit = static_cast<float>(kMaxNavColumns);
  if ((fx < -limit) || (fx > limit) || (fz < -limit) || (fz > limit)) {
    return false;
  }
  const auto cx = static_cast<std::int32_t>(fx);
  const auto cz = static_cast<std::int32_t>(fz);
  bool found = false;
  float bestDistance = 0.0F;
  for (std::int32_t z = cz - kSnapColumns; z <= cz + kSnapColumns; ++z) {
    for (std::int32_t x = cx - kSnapColumns; x <= cx + kSnapColumns; ++x) {
      const std::size_t count = m_mesh->surface_count(x, z);
      if (count == 0U) {
        continue;
      }
      // The point itself when it is over this column, else the column's
      // nearest point to it.
      const float lowX = s.boundsMin.x + (static_cast<float>(x) * s.cellSize);
      const float lowZ = s.boundsMin.z + (static_cast<float>(z) * s.cellSize);
      const float px = std::fmin(std::fmax(point.x, lowX), lowX + s.cellSize);
      const float pz = std::fmin(std::fmax(point.z, lowZ), lowZ + s.cellSize);
      for (std::size_t layer = 0U; layer < count; ++layer) {
        const float y = m_mesh->surface_height(x, z, layer);
        const float dy = y - point.y;
        if (std::fabs(dy) > s.agentHeight) {
          continue;
        }
        const float dx = px - point.x;
        const float dz = pz - point.z;
        const float d = (dx * dx) + (dy * dy) + (dz * dz);
        if (!found || (d < bestDistance)) {
          found = true;
          bestDistance = d;
          out->x = x;
          out->z = z;
          out->layer = layer;
          out->point = math::Vec3(px, y, pz);
        }
      }
    }
  }
  return found;
}

bool NavQuery::nearest_point(const math::Vec3 &point,
                             math::Vec3 *outPoint) const noexcept {
  Located located{};
  if ((outPoint == nullptr) || !locate(point, &located)) {
    return false;
  }
  *outPoint = located.point;
  return true;
}

float NavQuery::height_at(float worldX, float worldZ, std::uint32_t rect,
                          float fallback) const noexcept {
  const NavRect &r = m_mesh->rects()[rect];
  const NavBakeSettings &s = m_mesh->settings();
  // A corner lies on the rectangle's edge, so the column is clamped into
  // the rectangle before its surface is looked up.
  auto x = static_cast<std::int32_t>(
      std::floor((worldX - s.boundsMin.x) / s.cellSize));
  auto z = static_cast<std::int32_t>(
      std::floor((worldZ - s.boundsMin.z) / s.cellSize));
  x = (x < r.x0) ? r.x0 : ((x >= r.x1) ? (r.x1 - 1) : x);
  z = (z < r.z0) ? r.z0 : ((z >= r.z1) ? (r.z1 - 1) : z);
  for (std::size_t layer = 0U; layer < m_mesh->surface_count(x, z); ++layer) {
    if (m_mesh->surface_rect(x, z, layer) == rect) {
      return m_mesh->surface_height(x, z, layer);
    }
  }
  return fallback;
}

NavPathResult NavQuery::find_path(const math::Vec3 &start,
                                  const math::Vec3 &end, math::Vec3 *out,
                                  std::size_t capacity,
                                  std::size_t *outCount) noexcept {
  if (outCount != nullptr) {
    *outCount = 0U;
  }
  Located from{};
  Located to{};
  if ((out == nullptr) || (outCount == nullptr) || !locate(start, &from) ||
      !locate(end, &to)) {
    return NavPathResult::OffMesh;
  }
  const NavMesh &mesh = *m_mesh;
  const NavRect *rects = mesh.rects();
  const NavPortal *portals = mesh.portals();
  const std::uint32_t first = mesh.surface_rect(from.x, from.z, from.layer);
  const std::uint32_t goal = mesh.surface_rect(to.x, to.z, to.layer);

  // A* over the rectangles, measured between their centres, from the
  // start rectangle until the goal is taken from the open list.
  std::size_t routeLength = 1U;
  m_route[0] = first;
  if (first != goal) {
    for (std::size_t i = 0U; i < mesh.rect_count(); ++i) {
      m_state[i] = kUnseen;
    }
    const Point2 target{to.point.x, to.point.z};
    const auto heuristic = [&](std::uint32_t rect) noexcept {
      const Point2 c = rect_center(mesh, rects[rect]);
      return distance_xz(c.x, c.z, target.x, target.z);
    };
    const Point2 startCenter = rect_center(mesh, rects[first]);
    float *cost = m_cost.get();
    Heap open{m_heap.get(), m_heapKeys.get(), 0U};
    cost[first] =
        distance_xz(from.point.x, from.point.z, startCenter.x, startCenter.z);
    m_parent[first] = kNone;
    m_parentPortal[first] = kNone;
    m_state[first] = kOpen;
    open.push(first, cost[first] + heuristic(first));
    bool reached = false;
    while (open.size > 0U) {
      const std::uint32_t current = open.pop();
      if (m_state[current] == kClosed) {
        continue;
      }
      m_state[current] = kClosed;
      if (current == goal) {
        reached = true;
        break;
      }
      const Point2 here = rect_center(mesh, rects[current]);
      const NavRect &rect = rects[current];
      for (std::uint32_t p = 0U; p < rect.portalCount; ++p) {
        const std::uint32_t portalIndex = rect.firstPortal + p;
        const std::uint32_t next = portals[portalIndex].neighbor;
        if (m_state[next] == kClosed) {
          continue;
        }
        const Point2 there = rect_center(mesh, rects[next]);
        const float g =
            cost[current] + distance_xz(here.x, here.z, there.x, there.z);
        if ((m_state[next] == kOpen) && !(g < cost[next])) {
          continue;
        }
        cost[next] = g;
        m_parent[next] = current;
        m_parentPortal[next] = portalIndex;
        m_state[next] = kOpen;
        if (open.size >= (mesh.portal_count() + 1U)) {
          return NavPathResult::Unreachable;
        }
        open.push(next, g + heuristic(next));
      }
    }
    if (!reached) {
      return NavPathResult::Unreachable;
    }
    // The route, start to goal, and the portal entering each step.
    routeLength = 0U;
    for (std::uint32_t at = goal; at != kNone; at = m_parent[at]) {
      m_route[routeLength++] = at;
    }
    for (std::size_t i = 0U; i < (routeLength / 2U); ++i) {
      const std::uint32_t held = m_route[i];
      m_route[i] = m_route[routeLength - 1U - i];
      m_route[routeLength - 1U - i] = held;
    }
    for (std::size_t i = 1U; i < routeLength; ++i) {
      m_routePortals[i] = m_parentPortal[m_route[i]];
    }
  }

  // The funnel (string pull) through the portals, each oriented left and
  // right as seen walking from one rectangle into the next.
  const Point2 startPoint{from.point.x, from.point.z};
  const Point2 endPoint{to.point.x, to.point.z};
  const std::size_t portalSteps = routeLength + 1U;
  const auto portal_at = [&](std::size_t i, Point2 *left,
                             Point2 *right) noexcept {
    if (i == 0U) {
      *left = startPoint;
      *right = startPoint;
      return;
    }
    if (i == (portalSteps - 1U)) {
      *left = endPoint;
      *right = endPoint;
      return;
    }
    const NavPortal &portal = portals[m_routePortals[i]];
    const Point2 a{portal.ax, portal.az};
    const Point2 b{portal.bx, portal.bz};
    const Point2 c0 = rect_center(mesh, rects[m_route[i - 1U]]);
    const Point2 c1 = rect_center(mesh, rects[m_route[i]]);
    // The endpoint with the larger area against the step is on the
    // right, the side the funnel tightens with area2(...) <= 0.
    const bool aRight = area2(c0, c1, a) > area2(c0, c1, b);
    *right = aRight ? a : b;
    *left = aRight ? b : a;
  };
  std::size_t count = 0U;
  const auto emit = [&](const Point2 &p, float y) noexcept {
    if (count < capacity) {
      out[count] = math::Vec3(p.x, y, p.z);
    }
    ++count;
  };
  // A corner's height comes from the rectangle the funnel step entered.
  const auto corner_height = [&](std::size_t step, const Point2 &p) noexcept {
    const std::size_t routeIndex = (step == 0U) ? 0U : (step - 1U);
    return height_at(p.x, p.z, m_route[routeIndex], from.point.y);
  };
  emit(startPoint, from.point.y);
  Point2 apex = startPoint;
  Point2 left = startPoint;
  Point2 right = startPoint;
  std::size_t apexIndex = 0U;
  std::size_t leftIndex = 0U;
  std::size_t rightIndex = 0U;
  for (std::size_t i = 1U; i < portalSteps; ++i) {
    Point2 nextLeft{};
    Point2 nextRight{};
    portal_at(i, &nextLeft, &nextRight);
    if (area2(apex, right, nextRight) <= 0.0F) {
      if (same(apex, right) || (area2(apex, left, nextRight) > 0.0F)) {
        right = nextRight;
        rightIndex = i;
      } else {
        emit(left, corner_height(leftIndex, left));
        apex = left;
        apexIndex = leftIndex;
        right = apex;
        rightIndex = apexIndex;
        i = apexIndex;
        continue;
      }
    }
    if (area2(apex, left, nextLeft) >= 0.0F) {
      if (same(apex, left) || (area2(apex, right, nextLeft) < 0.0F)) {
        left = nextLeft;
        leftIndex = i;
      } else {
        emit(right, corner_height(rightIndex, right));
        apex = right;
        apexIndex = rightIndex;
        left = apex;
        leftIndex = apexIndex;
        i = apexIndex;
        continue;
      }
    }
  }
  if (!same(apex, endPoint) || (count == 1U)) {
    emit(endPoint, to.point.y);
  }
  if (count > capacity) {
    return NavPathResult::TooLong;
  }
  *outCount = count;
  return NavPathResult::Found;
}

} // namespace engine::navigation
