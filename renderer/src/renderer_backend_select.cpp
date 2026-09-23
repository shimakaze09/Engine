// Maps the platform's capabilities and the r_bgfx_renderer request to the
// GPU API the render device initializes.

#include "renderer_backend_select.h"

#include <cstring>

namespace engine::renderer {

RendererBackendChoice select_renderer_backend(const core::PlatformCaps &caps,
                                              const char *requested) noexcept {
  if (!caps.hasWindow) {
    return RendererBackendChoice::Noop;
  }
  struct Named final {
    const char *name;
    RendererBackendChoice choice;
  };
  static constexpr Named kNamed[] = {
      {"noop", RendererBackendChoice::Noop},
      {"vulkan", RendererBackendChoice::Vulkan},
      {"opengl", RendererBackendChoice::OpenGL},
      // The web export's API family, runnable natively for diagnosis.
      {"gles", RendererBackendChoice::OpenGLES},
      {"metal", RendererBackendChoice::Metal},
      {"d3d11", RendererBackendChoice::Direct3D11},
      {"d3d12", RendererBackendChoice::Direct3D12},
  };
  if (requested != nullptr) {
    for (const Named &named : kNamed) {
      if (std::strcmp(requested, named.name) == 0) {
        return named.choice;
      }
    }
  }
  return (caps.id == core::PlatformId::Windows)
             ? RendererBackendChoice::Vulkan
             : RendererBackendChoice::LibraryDefault;
}

} // namespace engine::renderer
