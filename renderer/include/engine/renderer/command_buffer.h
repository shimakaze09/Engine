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

// Field order is cache-conscious: sort key, hot per-draw identity, and
// material are first. modelMatrix is appended last because it is only
// read once per draw call after the mesh/material state has been set.
struct DrawCommand final {
  DrawKey sortKey{};
  std::uint32_t entity = 0U;
  MeshHandle mesh = kInvalidMeshHandle;
  Material material{};
  math::Mat4 modelMatrix = math::Mat4();
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
static constexpr std::size_t kMaxPointLights = 128U;
static constexpr std::size_t kMaxSpotLights = 64U;

struct DirectionalLightData final {
  math::Vec3 direction{};
  math::Vec3 color{};
  float intensity = 0.0F;
};

struct PointLightData final {
  math::Vec3 position{};
  math::Vec3 color{};
  float intensity = 0.0F;
  float radius = 10.0F;
};

struct SpotLightData final {
  math::Vec3 position{};
  math::Vec3 direction{};
  math::Vec3 color{};
  float intensity = 0.0F;
  float radius = 10.0F;
  float innerConeAngle = 0.3491F; // ~20 degrees
  float outerConeAngle = 0.5236F; // ~30 degrees
};

struct SceneLightData final {
  std::array<DirectionalLightData, kMaxDirectionalLights> directionalLights{};
  std::size_t directionalLightCount = 0U;
  std::array<PointLightData, kMaxPointLights> pointLights{};
  std::size_t pointLightCount = 0U;
  std::array<SpotLightData, kMaxSpotLights> spotLights{};
  std::size_t spotLightCount = 0U;
};

struct RendererFrameStats final {
  std::uint32_t drawCalls = 0U;
  std::uint64_t triangleCount = 0U;
  float gpuSceneMs = 0.0F;
  float gpuTonemapMs = 0.0F;
  float gpuGBufferMs = 0.0F;
  float gpuDeferredLightMs = 0.0F;
  float gpuBloomMs = 0.0F;
  float gpuSsaoMs = 0.0F;
};

void flush_renderer(CommandBufferView commandBufferView,
                    const GpuMeshRegistry *registry, float timeSeconds,
                    const SceneLightData &lights) noexcept;
void shutdown_renderer() noexcept;

// Optional scene viewport override from editor UI. When set to positive
// values, flush_renderer uses this size for projection and pass resources
// instead of the full SDL drawable size.
void set_scene_viewport_size(int width, int height) noexcept;

/// Returns the GPU texture ID of the tonemapped scene (final color).
/// Valid after the first flush_renderer call. Returns 0 if not yet available.
std::uint32_t get_scene_viewport_texture() noexcept;
RendererFrameStats renderer_get_last_frame_stats() noexcept;

} // namespace engine::renderer
