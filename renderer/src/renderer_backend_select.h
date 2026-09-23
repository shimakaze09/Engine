// Chooses the GPU API the render device initializes, from the platform's
// capabilities and the r_bgfx_renderer request. Pure, so each platform's
// choice is testable on any host.

#pragma once

#include "engine/core/platform.h"

#include <cstdint>

namespace engine::renderer {

enum class RendererBackendChoice : std::uint8_t {
  /// The backend library picks the platform's best API.
  LibraryDefault,
  Noop,
  Vulkan,
  OpenGL,
  OpenGLES,
  Metal,
  Direct3D11,
  Direct3D12,
};

/// The API for `requested` ("auto", "noop", "vulkan", "opengl", "gles",
/// "metal", "d3d11", "d3d12"; null or anything else reads as "auto") on a
/// platform with `caps`. With no window only Noop can present. "auto" is
/// Vulkan on Windows, the proven backend for the canonical spirv cook: the
/// D3D backends stay explicit opt-ins until verified on hardware, an owner
/// call to flip. Elsewhere "auto" leaves the choice to the library.
RendererBackendChoice select_renderer_backend(const core::PlatformCaps &caps,
                                              const char *requested) noexcept;

} // namespace engine::renderer
