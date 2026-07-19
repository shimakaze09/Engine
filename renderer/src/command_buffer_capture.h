// Declares private scene-capture (render-to-texture) target helpers.

#pragma once

#include <cstddef>

#include "command_buffer_context.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

/// Creates or resizes the GL target backing capture slot `slot`; false when
/// the slot index is out of range or resource creation fails (logged).
bool ensure_scene_capture_target(BackendState &backend,
                                 const RenderDevice *dev, std::size_t slot,
                                 int width, int height) noexcept;

/// Destroys every scene-capture GL target owned by the backend.
void destroy_scene_capture_targets(BackendState &backend,
                                   const RenderDevice *dev) noexcept;

} // namespace engine::renderer
