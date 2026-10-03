// Implements the .navmesh document: the writer that encodes a baked mesh
// little endian with a trailing checksum, and the reader that refuses a
// document whose magic, version, checksum, settings or any index or range
// does not hold, so a damaged file never becomes a mesh.

#include "engine/navigation/nav_mesh.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "engine/core/hash.h"
#include "engine/core/logging.h"

namespace engine::navigation {

namespace {

constexpr const char *kLogChannel = "navigation";
/// "NAVM" read as a little-endian 32-bit word.
constexpr std::uint32_t kMagic = 0x4D56414EU;
constexpr std::uint32_t kNoRect = 0xFFFFFFFFU;
constexpr std::size_t kSettingsFloats = 11U;

std::uint64_t checksum(const std::uint8_t *data, std::size_t size) noexcept {
  std::uint64_t hash = core::kFnv1a64Offset;
  for (std::size_t i = 0U; i < size; ++i) {
    hash = core::fnv1a_64_append(hash, data[i]);
  }
  return hash;
}

/// Appends little-endian words to a buffer sized up front.
struct Writer final {
  std::uint8_t *data = nullptr;
  std::size_t at = 0U;

  void u8(std::uint8_t value) noexcept { data[at++] = value; }
  void u32(std::uint32_t value) noexcept {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
      data[at++] = static_cast<std::uint8_t>((value >> shift) & 0xFFU);
    }
  }
  void u64(std::uint64_t value) noexcept {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
      data[at++] = static_cast<std::uint8_t>((value >> shift) & 0xFFU);
    }
  }
  void i32(std::int32_t value) noexcept {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    u32(bits);
  }
  void f32(float value) noexcept {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    u32(bits);
  }
};

/// Reads little-endian words, failing once anything would read past the
/// end.
struct Reader final {
  const std::uint8_t *data = nullptr;
  std::size_t size = 0U;
  std::size_t at = 0U;
  bool ok = true;

  bool has(std::size_t bytes) noexcept {
    ok = ok && (bytes <= (size - at));
    return ok;
  }
  std::uint8_t u8() noexcept { return has(1U) ? data[at++] : 0U; }
  std::uint32_t u32() noexcept {
    if (!has(4U)) {
      return 0U;
    }
    std::uint32_t value = 0U;
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
      value |= static_cast<std::uint32_t>(data[at++]) << shift;
    }
    return value;
  }
  std::uint64_t u64() noexcept {
    if (!has(8U)) {
      return 0U;
    }
    std::uint64_t value = 0U;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
      value |= static_cast<std::uint64_t>(data[at++]) << shift;
    }
    return value;
  }
  std::int32_t i32() noexcept {
    const std::uint32_t bits = u32();
    std::int32_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
  float f32() noexcept {
    const std::uint32_t bits = u32();
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
};

bool refuse(const char *reason) noexcept {
  char message[200] = {};
  std::snprintf(message, sizeof(message), "navigation mesh not read: %s",
                reason);
  core::log_message(core::LogLevel::Error, kLogChannel, message);
  return false;
}

} // namespace

bool write_nav_mesh(const NavMesh &mesh, std::unique_ptr<std::uint8_t[]> *out,
                    std::size_t *outSize) noexcept {
  if ((out == nullptr) || (outSize == nullptr)) {
    return false;
  }
  if (mesh.empty()) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "navigation mesh not written: it holds nothing "
                      "walkable");
    return false;
  }
  const std::int32_t cx = mesh.columns_x();
  const std::int32_t cz = mesh.columns_z();
  std::size_t surfaces = 0U;
  for (std::int32_t z = 0; z < cz; ++z) {
    for (std::int32_t x = 0; x < cx; ++x) {
      surfaces += mesh.surface_count(x, z);
    }
  }
  const std::size_t columns =
      static_cast<std::size_t>(cx) * static_cast<std::size_t>(cz);
  const std::size_t size = 8U + (kSettingsFloats * 4U) + 16U + columns +
                           (surfaces * 8U) + (mesh.rect_count() * 24U) +
                           (mesh.portal_count() * 20U) + 8U;
  std::unique_ptr<std::uint8_t[]> buffer(new (std::nothrow) std::uint8_t[size]);
  if (buffer == nullptr) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "navigation mesh not written: there is not enough "
                      "memory");
    return false;
  }
  Writer w{buffer.get(), 0U};
  w.u32(kMagic);
  w.u32(kNavMeshFormatVersion);
  const NavBakeSettings &s = mesh.settings();
  const float settings[kSettingsFloats] = {
      s.boundsMin.x, s.boundsMin.y, s.boundsMin.z,    s.boundsMax.x,
      s.boundsMax.y, s.boundsMax.z, s.cellSize,       s.agentRadius,
      s.agentHeight, s.maxClimb,    s.maxSlopeDegrees};
  for (const float value : settings) {
    w.f32(value);
  }
  w.i32(cx);
  w.i32(cz);
  w.u32(static_cast<std::uint32_t>(mesh.rect_count()));
  w.u32(static_cast<std::uint32_t>(mesh.portal_count()));
  for (std::int32_t z = 0; z < cz; ++z) {
    for (std::int32_t x = 0; x < cx; ++x) {
      w.u8(static_cast<std::uint8_t>(mesh.surface_count(x, z)));
    }
  }
  for (std::int32_t z = 0; z < cz; ++z) {
    for (std::int32_t x = 0; x < cx; ++x) {
      for (std::size_t layer = 0U; layer < mesh.surface_count(x, z); ++layer) {
        w.f32(mesh.surface_height(x, z, layer));
        w.u32(mesh.surface_rect(x, z, layer));
      }
    }
  }
  for (std::size_t i = 0U; i < mesh.rect_count(); ++i) {
    const NavRect &r = mesh.rects()[i];
    w.i32(r.x0);
    w.i32(r.z0);
    w.i32(r.x1);
    w.i32(r.z1);
    w.u32(r.firstPortal);
    w.u32(r.portalCount);
  }
  for (std::size_t i = 0U; i < mesh.portal_count(); ++i) {
    const NavPortal &p = mesh.portals()[i];
    w.u32(p.neighbor);
    w.f32(p.ax);
    w.f32(p.az);
    w.f32(p.bx);
    w.f32(p.bz);
  }
  w.u64(checksum(buffer.get(), w.at));
  *out = std::move(buffer);
  *outSize = size;
  return true;
}

bool read_nav_mesh(const std::uint8_t *data, std::size_t size,
                   NavMesh *out) noexcept {
  if ((data == nullptr) || (out == nullptr)) {
    return false;
  }
  if (size < 16U) {
    return refuse("the file is too short to be a navigation mesh");
  }
  Reader r{data, size - 8U, 0U, true};
  if (r.u32() != kMagic) {
    return refuse("the file is not a navigation mesh");
  }
  const std::uint32_t version = r.u32();
  if (version != kNavMeshFormatVersion) {
    return refuse((version > kNavMeshFormatVersion)
                      ? "a newer build wrote it"
                      : "its format version is not one this build reads");
  }
  Reader tail{data, size, size - 8U, true};
  if (tail.u64() != checksum(data, size - 8U)) {
    return refuse("its checksum does not match, so it was damaged or "
                  "edited after it was written");
  }
  NavBakeSettings settings{};
  float values[kSettingsFloats] = {};
  for (float &value : values) {
    value = r.f32();
  }
  settings.boundsMin = math::Vec3(values[0], values[1], values[2]);
  settings.boundsMax = math::Vec3(values[3], values[4], values[5]);
  settings.cellSize = values[6];
  settings.agentRadius = values[7];
  settings.agentHeight = values[8];
  settings.maxClimb = values[9];
  settings.maxSlopeDegrees = values[10];
  const std::int32_t cx = r.i32();
  const std::int32_t cz = r.i32();
  const std::uint32_t rectCount = r.u32();
  const std::uint32_t portalCount = r.u32();
  if (!r.ok || !nav_bake_settings_are_valid(settings) ||
      (cx != static_cast<std::int32_t>(
                 std::ceil((settings.boundsMax.x - settings.boundsMin.x) /
                           settings.cellSize))) ||
      (cz != static_cast<std::int32_t>(
                 std::ceil((settings.boundsMax.z - settings.boundsMin.z) /
                           settings.cellSize)))) {
    return refuse("its settings or grid size do not hold");
  }
  const std::size_t columns =
      static_cast<std::size_t>(cx) * static_cast<std::size_t>(cz);
  const std::size_t slots = columns * kMaxNavLayers;
  // Every surface, polygon and portal takes bytes, so counts past what the
  // file holds are refused before anything is allocated for them.
  if ((rectCount == 0U) || (rectCount > slots) ||
      (portalCount > (r.size - r.at) / 20U) || !r.has(columns)) {
    return refuse("its polygon or portal counts do not fit the file");
  }
  std::unique_ptr<std::uint8_t[]> counts(new (std::nothrow)
                                             std::uint8_t[columns]());
  std::unique_ptr<float[]> heights(new (std::nothrow) float[slots]());
  std::unique_ptr<std::uint32_t[]> surfaceRects(new (std::nothrow)
                                                    std::uint32_t[slots]);
  std::unique_ptr<NavRect[]> rects(new (std::nothrow) NavRect[rectCount]);
  std::unique_ptr<NavPortal[]> portals(
      new (std::nothrow) NavPortal[(portalCount > 0U) ? portalCount : 1U]);
  if ((counts == nullptr) || (heights == nullptr) ||
      (surfaceRects == nullptr) || (rects == nullptr) || (portals == nullptr)) {
    return refuse("there is not enough memory");
  }
  for (std::size_t i = 0U; i < slots; ++i) {
    surfaceRects[i] = kNoRect;
  }
  for (std::size_t column = 0U; column < columns; ++column) {
    counts[column] = r.u8();
    if (counts[column] > kMaxNavLayers) {
      return refuse("a column holds more surfaces than a mesh keeps");
    }
  }
  for (std::size_t column = 0U; column < columns; ++column) {
    float above = 0.0F;
    for (std::size_t layer = 0U; layer < counts[column]; ++layer) {
      const float height = r.f32();
      const std::uint32_t rect = r.u32();
      if (!r.ok || !std::isfinite(height) || (rect >= rectCount) ||
          ((layer > 0U) && !(height < above))) {
        return refuse("a surface's height, order or polygon does not hold");
      }
      heights[(column * kMaxNavLayers) + layer] = height;
      surfaceRects[(column * kMaxNavLayers) + layer] = rect;
      above = height;
    }
  }
  std::uint32_t nextPortal = 0U;
  for (std::uint32_t i = 0U; i < rectCount; ++i) {
    NavRect &rect = rects[i];
    rect.x0 = r.i32();
    rect.z0 = r.i32();
    rect.x1 = r.i32();
    rect.z1 = r.i32();
    rect.firstPortal = r.u32();
    rect.portalCount = r.u32();
    // Portals are stored polygon by polygon, in order.
    if (!r.ok || (rect.x0 < 0) || (rect.z0 < 0) || (rect.x1 > cx) ||
        (rect.z1 > cz) || !(rect.x0 < rect.x1) || !(rect.z0 < rect.z1) ||
        (rect.firstPortal != nextPortal) ||
        (rect.portalCount > (portalCount - nextPortal))) {
      return refuse("a polygon's bounds or portal range does not hold");
    }
    nextPortal += rect.portalCount;
  }
  if (nextPortal != portalCount) {
    return refuse("the polygons do not account for every portal");
  }
  for (std::uint32_t i = 0U; i < portalCount; ++i) {
    NavPortal &p = portals[i];
    p.neighbor = r.u32();
    p.ax = r.f32();
    p.az = r.f32();
    p.bx = r.f32();
    p.bz = r.f32();
    if (!r.ok || (p.neighbor >= rectCount) || !std::isfinite(p.ax) ||
        !std::isfinite(p.az) || !std::isfinite(p.bx) || !std::isfinite(p.bz)) {
      return refuse("a portal's neighbour or ends do not hold");
    }
  }
  if (!r.ok || (r.at != r.size)) {
    return refuse("its length does not match what it describes");
  }
  out->m_settings = settings;
  out->m_columnsX = cx;
  out->m_columnsZ = cz;
  out->m_surfaceCounts = std::move(counts);
  out->m_surfaceHeights = std::move(heights);
  out->m_surfaceRects = std::move(surfaceRects);
  out->m_rectCount = rectCount;
  out->m_rects = std::move(rects);
  out->m_portalCount = portalCount;
  out->m_portals = std::move(portals);
  return true;
}

} // namespace engine::navigation
