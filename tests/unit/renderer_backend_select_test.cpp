// The render device's API choice per platform (#312 item 7): what "auto"
// means on each platform, that a named request wins wherever a window
// exists, and that nothing but Noop is chosen without one. The choice is
// a pure function of the platform's capabilities, so every platform's row
// runs on any host.

#include "renderer_backend_select.h"

#include <cstdio>
#include <initializer_list>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

using engine::core::PlatformCaps;
using engine::core::PlatformId;
using engine::renderer::RendererBackendChoice;
using engine::renderer::select_renderer_backend;

PlatformCaps caps_for(PlatformId id, bool hasWindow) {
  PlatformCaps caps{};
  caps.id = id;
  caps.hasWindow = hasWindow;
  return caps;
}

} // namespace

/// Runs this executable or test program.
int main() {
  constexpr PlatformId kPlatforms[] = {PlatformId::Windows, PlatformId::Linux,
                                       PlatformId::MacOS, PlatformId::Web};

  // "auto": Vulkan on Windows, the library's pick elsewhere. A null or
  // unrecognized request reads as "auto".
  for (const char *request :
       {"auto", static_cast<const char *>(nullptr), "not-an-api"}) {
    CHECK(select_renderer_backend(caps_for(PlatformId::Windows, true),
                                  request) == RendererBackendChoice::Vulkan,
          "auto on Windows is Vulkan");
    for (const PlatformId id :
         {PlatformId::Linux, PlatformId::MacOS, PlatformId::Web}) {
      CHECK(select_renderer_backend(caps_for(id, true), request) ==
                RendererBackendChoice::LibraryDefault,
            "auto off Windows leaves the choice to the library");
    }
  }

  // A named API wins on every platform that has a window.
  struct Named final {
    const char *name;
    RendererBackendChoice choice;
  };
  constexpr Named kNamed[] = {
      {"noop", RendererBackendChoice::Noop},
      {"vulkan", RendererBackendChoice::Vulkan},
      {"opengl", RendererBackendChoice::OpenGL},
      {"gles", RendererBackendChoice::OpenGLES},
      {"metal", RendererBackendChoice::Metal},
      {"d3d11", RendererBackendChoice::Direct3D11},
      {"d3d12", RendererBackendChoice::Direct3D12},
  };
  for (const PlatformId id : kPlatforms) {
    for (const Named &named : kNamed) {
      CHECK(select_renderer_backend(caps_for(id, true), named.name) ==
                named.choice,
            "a named API is chosen as named");
    }
  }

  // Without a window only Noop can present, whatever was asked for.
  for (const PlatformId id : kPlatforms) {
    CHECK(select_renderer_backend(caps_for(id, false), "auto") ==
              RendererBackendChoice::Noop,
          "no window chooses Noop for auto");
    for (const Named &named : kNamed) {
      CHECK(select_renderer_backend(caps_for(id, false), named.name) ==
                RendererBackendChoice::Noop,
            "no window chooses Noop for a named API");
    }
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("renderer_backend_select_test passed");
  return 0;
}
