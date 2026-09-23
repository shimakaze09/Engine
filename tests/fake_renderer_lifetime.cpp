// The renderer-lifetime seam for suites that compile mesh_loader.cpp on its
// own, without command_buffer.cpp and the lifetime latch it owns: the
// device the fake render device publishes, created on demand as the real
// acquire_render_device does while the renderer is open.

#include "fake_render_device.h"

namespace engine::renderer {

// Declared in command_buffer.h, which these suites do not include; the
// signature must match it.
const RenderDevice *acquire_render_device() noexcept;

const RenderDevice *acquire_render_device() noexcept {
  return initialize_render_device() ? render_device() : nullptr;
}

} // namespace engine::renderer
