// Declares the null render device backend (#196): a RenderDevice function
// table whose entries succeed without any GL so pipeline initialization
// and the frame stages run on headless CI lanes. Test/CI-only, selected
// via the r_null_device cvar in initialize_render_device.

#pragma once

#include "engine/renderer/render_device.h"

namespace engine::renderer {

/// Fills `device` (table + caps) with the null backend.
void fill_null_render_device(RenderDevice *device) noexcept;

} // namespace engine::renderer
