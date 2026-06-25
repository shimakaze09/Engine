// Implements private renderer command buffer context state.

#include "command_buffer_context.h"

namespace engine::renderer {

RendererContext &renderer_context() noexcept {
  static RendererContext context{};
  return context;
}

BackendState &backend_state() noexcept {
  return renderer_context().backend;
}

void reset_renderer_public_state() noexcept {
  renderer_context().activeCamera = CameraState{};
  renderer_context().sceneViewportWidth = 0;
  renderer_context().sceneViewportHeight = 0;
  renderer_context().lastFrameStats = RendererFrameStats{};
  renderer_context().fxaaAppliedThisFrame = false;
  renderer_context().activeSkyboxTexture = kInvalidTextureHandle;
}

void reset_backend_on_failure() noexcept {
  BackendState &backend = backend_state();
  backend = BackendState{};
  backend.failed = true;
}

} // namespace engine::renderer
