// Declares private sky rendering helpers shared by the renderer command
// buffer implementation files (backend init/teardown, frame flush, IBL).
// Split out of command_buffer.cpp (REVIEW_FINDINGS A1).

#pragma once

#include <cstdint>

#include "command_buffer_context.h"
#include "engine/math/mat4.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

/// Enumerates sky model values used by the engine.
enum class SkyModel : std::uint8_t {
  Hosek = 0,
  Preetham = 1,
  Cubemap = 2,
  None = 3,
};

// Skybox cube draw size: 12 triangles, 3 vertices each. The vertex data
// lives in command_buffer_sky.cpp; a static_assert there keeps this count
// in sync with the array.
inline constexpr std::int32_t kSkyboxVertexCount = 36;

/// Maps the r_sky_model cvar string to a SkyModel (default Hosek).
SkyModel selected_sky_model() noexcept;
/// Lazily creates the shared skybox cube VAO/VBO (also used by IBL bakes).
bool create_skybox_geometry(BackendState &backend,
                            const RenderDevice *dev) noexcept;
/// Releases every sky shader and the shared cube geometry.
void destroy_skybox_resources(BackendState &backend) noexcept;
/// Releases the Preetham sky shader and resets its cached uniform locations.
void destroy_preetham_sky_resources(BackendState &backend) noexcept;
/// Releases the Hosek sky shader and resets its cached uniform locations.
void destroy_hosek_sky_resources(BackendState &backend) noexcept;
/// Returns the GPU id of the active cubemap skybox, or 0 when unavailable.
std::uint32_t active_skybox_gpu_texture(const BackendState &backend) noexcept;
/// Draws the cubemap skybox and counts the draw in frameStats.
void draw_skybox(const BackendState &backend, const RenderDevice *dev,
                 const math::Mat4 &viewMat, const math::Mat4 &projMat,
                 std::uint32_t cubemapGpuId,
                 RendererFrameStats &frameStats) noexcept;
/// Draws the Preetham analytic sky; the sun direction comes from the first
/// directional light (with a fixed fallback).
void draw_preetham_sky(const BackendState &backend, const RenderDevice *dev,
                       const math::Mat4 &viewMat, const math::Mat4 &projMat,
                       const SceneLightData &lights,
                       RendererFrameStats &frameStats) noexcept;
/// Draws the Hosek-Wilkie analytic sky; the sun direction comes from the
/// first directional light (with a fixed fallback).
void draw_hosek_sky(const BackendState &backend, const RenderDevice *dev,
                    const math::Mat4 &viewMat, const math::Mat4 &projMat,
                    const SceneLightData &lights,
                    RendererFrameStats &frameStats) noexcept;

} // namespace engine::renderer
