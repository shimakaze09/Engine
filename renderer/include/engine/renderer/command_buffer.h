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

struct DrawCommand final {
  std::uint32_t entity = 0U;
  MeshHandle mesh = kInvalidMeshHandle;
  math::Mat4 modelMatrix = math::Mat4();
  // Copied per draw; keep this lean to avoid command buffer bloat.
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
  void sort_by_entity() noexcept;
  std::size_t command_count() const noexcept;
  CommandBufferView view() const noexcept;

private:
  std::array<DrawCommand, kMaxDrawCommands> m_commands{};
  std::size_t m_commandCount = 0U;
};

struct GpuMeshRegistry;

void flush_renderer(CommandBufferView commandBufferView,
                    const GpuMeshRegistry *registry,
                    float timeSeconds) noexcept;
void shutdown_renderer() noexcept;

} // namespace engine::renderer
