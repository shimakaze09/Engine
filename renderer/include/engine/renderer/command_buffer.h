#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/math/mat4.h"
#include "engine/math/vec3.h"
#include "engine/renderer/material.h"

namespace engine::renderer {

struct MeshHandle final {
  std::uint32_t id = 0U;

  friend constexpr bool operator==(const MeshHandle &,
                                   const MeshHandle &) = default;
};

inline constexpr MeshHandle kInvalidMeshHandle{};

// Instancing-ready sort key.
// Bit layout (MSB→LSB):
//   transparent:1 | shader:7 | texture:20 | mesh:20 | depth:16
// Opaque (transparent=0) sorts front-to-back (smaller depth first).
// Transparent (transparent=1) sorts back-to-front (larger depth first).
struct DrawKey final {
  std::uint64_t value = 0U;
};

struct DrawCommand final {
  DrawKey sortKey{};
  std::uint32_t entity = 0U;
  MeshHandle mesh = kInvalidMeshHandle;
  math::Mat4 modelMatrix = math::Mat4();
  Material material{};
};

struct CommandBufferView final {
  const DrawCommand *data = nullptr;
  std::uint32_t count = 0U;
};

class CommandBufferBuilder final {
public:
  static constexpr std::size_t kMaxDrawCommands = 8192U;

  void reset() noexcept;
  bool submit(const DrawCommand &command) noexcept;
  bool append_from(const CommandBufferBuilder &other) noexcept;
  void sort_by_key() noexcept;
  std::size_t command_count() const noexcept;
  CommandBufferView view() const noexcept;

private:
  std::array<DrawCommand, kMaxDrawCommands> m_commands{};
  std::size_t m_commandCount = 0U;
};

struct GpuMeshRegistry;

// Scene light data collected from ECS each frame.
static constexpr std::size_t kMaxDirectionalLights = 4U;
static constexpr std::size_t kMaxPointLights = 8U;

struct DirectionalLightData final {
  math::Vec3 direction{};
  math::Vec3 color{};
  float intensity = 0.0F;
};

struct PointLightData final {
  math::Vec3 position{};
  math::Vec3 color{};
  float intensity = 0.0F;
};

struct SceneLightData final {
  std::array<DirectionalLightData, kMaxDirectionalLights> directionalLights{};
  std::size_t directionalLightCount = 0U;
  std::array<PointLightData, kMaxPointLights> pointLights{};
  std::size_t pointLightCount = 0U;
};

void flush_renderer(CommandBufferView commandBufferView,
                    const GpuMeshRegistry *registry,
                    float timeSeconds,
                    const SceneLightData &lights) noexcept;
void shutdown_renderer() noexcept;

} // namespace engine::renderer
