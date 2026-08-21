// Declares the bgfx render device backend's bring-up hooks (#138 Phase
// B). The backend fills the engine RenderDevice table over bgfx running
// single-threaded on the Noop renderer until the platform window and
// presentation path are ported in Phase D; initialize/shutdown/
// render_device stay the shared entry points in render_device.h.

#pragma once

namespace engine::renderer {

/// Advances the bgfx frame: submits everything recorded since the last
/// call and resets the per-frame view allocation. Test and bring-up hook
/// until Phase D ports the presentation path; defined no-op before
/// initialization and for the null backend.
void render_device_bgfx_frame() noexcept;

} // namespace engine::renderer
