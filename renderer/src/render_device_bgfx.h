// Declares the bgfx render device backend's bring-up hooks. The backend
// fills the engine RenderDevice table over bgfx running single-threaded,
// windowed or on the Noop renderer; initialize/shutdown/render_device stay
// the shared entry points in render_device.h.

#pragma once

namespace engine::renderer {

/// Advances the bgfx frame: submits everything recorded since the last
/// call and resets the per-frame view allocation. Test and bring-up hook;
/// a no-op before initialization and for the null backend.
void render_device_bgfx_frame() noexcept;

/// Asks for the presented back buffer to be written to path as a TGA by
/// the next render_device_bgfx_frame, through bgfx's readback. The file
/// appears a frame or two later, once bgfx hands the pixels back, so a
/// caller runs further frames and then looks for it. The path is copied;
/// one that does not fit, or a null one, is refused and nothing is
/// requested. This is how a gpu-labelled test reads pixels: the engine
/// has no other readback.
bool render_device_bgfx_request_screenshot(const char *path) noexcept;

} // namespace engine::renderer
