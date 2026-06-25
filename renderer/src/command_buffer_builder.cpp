// Implements command buffer builder sorting and batching helpers.

#include "engine/renderer/command_buffer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace engine::renderer {
namespace {

constexpr std::uint64_t kDrawKeyTransparentBit = 1ULL << 63U;
constexpr std::uint64_t kDrawKeyDepthMask = 0xFFFFULL;

/// Compares vec3 values for exact equality.
bool vec3_equal(const math::Vec3 &lhs, const math::Vec3 &rhs) noexcept {
  return (lhs.x == rhs.x) && (lhs.y == rhs.y) && (lhs.z == rhs.z);
}

/// Orders vec3 values lexicographically for stable command sorting.
bool vec3_less(const math::Vec3 &lhs, const math::Vec3 &rhs) noexcept {
  if (lhs.x != rhs.x) {
    return lhs.x < rhs.x;
  }
  if (lhs.y != rhs.y) {
    return lhs.y < rhs.y;
  }
  return lhs.z < rhs.z;
}

/// Compares materials for static mesh instancing compatibility.
bool materials_equal(const Material &lhs, const Material &rhs) noexcept {
  return vec3_equal(lhs.albedo, rhs.albedo) &&
         vec3_equal(lhs.emissive, rhs.emissive) &&
         (lhs.roughness == rhs.roughness) && (lhs.metallic == rhs.metallic) &&
         (lhs.opacity == rhs.opacity) &&
         (lhs.albedoTexture == rhs.albedoTexture) &&
         (lhs.normalTexture == rhs.normalTexture);
}

/// Orders materials for stable opaque command sorting.
bool material_less(const Material &lhs, const Material &rhs) noexcept {
  if (!vec3_equal(lhs.albedo, rhs.albedo)) {
    return vec3_less(lhs.albedo, rhs.albedo);
  }
  if (!vec3_equal(lhs.emissive, rhs.emissive)) {
    return vec3_less(lhs.emissive, rhs.emissive);
  }
  if (lhs.roughness != rhs.roughness) {
    return lhs.roughness < rhs.roughness;
  }
  if (lhs.metallic != rhs.metallic) {
    return lhs.metallic < rhs.metallic;
  }
  if (lhs.opacity != rhs.opacity) {
    return lhs.opacity < rhs.opacity;
  }
  if (lhs.albedoTexture.id != rhs.albedoTexture.id) {
    return lhs.albedoTexture.id < rhs.albedoTexture.id;
  }
  return lhs.normalTexture.id < rhs.normalTexture.id;
}

/// Returns whether two adjacent draw commands can share one instance batch.
bool draw_commands_instance_compatible(const DrawCommand &lhs,
                                       const DrawCommand &rhs) noexcept {
  return (lhs.mesh == rhs.mesh) && materials_equal(lhs.material, rhs.material) &&
         (lhs.foliageWindStrength == rhs.foliageWindStrength) &&
         (lhs.foliageWindFrequency == rhs.foliageWindFrequency);
}

/// Extracts the non-depth state bits from a packed draw key.
std::uint64_t draw_key_state_bits(const DrawCommand &command) noexcept {
  return command.sortKey.value & ~kDrawKeyDepthMask;
}

} // namespace

/// Resets this object back to its reusable empty state.
void CommandBufferBuilder::reset() noexcept { m_commandCount = 0U; }

/// Submits work to the owning buffer or system.
bool CommandBufferBuilder::submit(const DrawCommand &command) noexcept {
  if (m_commandCount >= kMaxDrawCommands) {
    return false;
  }

  m_commands[m_commandCount] = command;
  ++m_commandCount;
  return true;
}

/// Appends all commands from another builder if capacity allows.
bool CommandBufferBuilder::append_from(
    const CommandBufferBuilder &other) noexcept {
  if (other.m_commandCount == 0U) {
    return true;
  }

  if ((m_commandCount + other.m_commandCount) > kMaxDrawCommands) {
    return false;
  }

  std::memcpy(m_commands.data() + m_commandCount, other.m_commands.data(),
              sizeof(DrawCommand) * other.m_commandCount);
  m_commandCount += other.m_commandCount;

  return true;
}

/// Sorts submitted commands by transparency, state, material, and depth.
void CommandBufferBuilder::sort_by_key() noexcept {
  std::sort(m_commands.begin(),
            m_commands.begin() + static_cast<std::ptrdiff_t>(m_commandCount),
            [](const DrawCommand &lhs, const DrawCommand &rhs) {
              const bool lhsTransparent =
                  (lhs.sortKey.value & kDrawKeyTransparentBit) != 0U;
              const bool rhsTransparent =
                  (rhs.sortKey.value & kDrawKeyTransparentBit) != 0U;
              if (lhsTransparent != rhsTransparent) {
                return lhsTransparent < rhsTransparent;
              }
              if (lhsTransparent) {
                return lhs.sortKey.value < rhs.sortKey.value;
              }

              const std::uint64_t lhsState = draw_key_state_bits(lhs);
              const std::uint64_t rhsState = draw_key_state_bits(rhs);
              if (lhsState != rhsState) {
                return lhsState < rhsState;
              }
              if (lhs.mesh.id != rhs.mesh.id) {
                return lhs.mesh.id < rhs.mesh.id;
              }
              if (!materials_equal(lhs.material, rhs.material)) {
                return material_less(lhs.material, rhs.material);
              }
              if (lhs.foliageWindStrength != rhs.foliageWindStrength) {
                return lhs.foliageWindStrength < rhs.foliageWindStrength;
              }
              if (lhs.foliageWindFrequency != rhs.foliageWindFrequency) {
                return lhs.foliageWindFrequency < rhs.foliageWindFrequency;
              }
              return (lhs.sortKey.value & kDrawKeyDepthMask) <
                     (rhs.sortKey.value & kDrawKeyDepthMask);
            });
}

/// Returns the number of submitted commands.
std::size_t CommandBufferBuilder::command_count() const noexcept {
  return m_commandCount;
}

/// Returns a read-only view of the submitted command storage.
CommandBufferView CommandBufferBuilder::view() const noexcept {
  CommandBufferView commandBufferView{};
  commandBufferView.data = m_commands.data();
  commandBufferView.count = static_cast<std::uint32_t>(m_commandCount);
  return commandBufferView;
}

/// Builds the requested runtime data for static mesh batches.
std::size_t build_static_mesh_batches(CommandBufferView commandBufferView,
                                      std::size_t start, std::size_t end,
                                      StaticMeshBatch *batches,
                                      std::size_t batchCapacity) noexcept {
  if ((commandBufferView.data == nullptr) || (batches == nullptr) ||
      (batchCapacity == 0U)) {
    return 0U;
  }

  const std::size_t viewCount = static_cast<std::size_t>(commandBufferView.count);
  if (start > viewCount) {
    start = viewCount;
  }
  if (end > viewCount) {
    end = viewCount;
  }
  if (start >= end) {
    return 0U;
  }

  std::size_t batchCount = 0U;
  std::size_t first = start;
  while (first < end) {
    std::size_t next = first + 1U;
    while ((next < end) &&
           draw_commands_instance_compatible(commandBufferView.data[first],
                                             commandBufferView.data[next])) {
      ++next;
    }

    if (batchCount >= batchCapacity) {
      return batchCount;
    }

    batches[batchCount].first = static_cast<std::uint32_t>(first);
    batches[batchCount].count = static_cast<std::uint32_t>(next - first);
    ++batchCount;
    first = next;
  }

  return batchCount;
}

} // namespace engine::renderer
