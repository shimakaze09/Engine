// Implements the null render device backend (#196): every entry succeeds
// without touching GL — creation returns monotonically increasing nonzero
// handles, updates/binds/draws are no-ops, queries report ready with zero
// timestamps. It deliberately models no stale-handle detection or
// resource state (that is the GL backend's slot-table job); its only
// contract is that pipeline initialization and the frame stages complete
// headlessly so backend-independent runtime behavior (FatalFrame
// propagation, stage ordering) is testable on every CI lane.

#include "render_device_null.h"

#include <cstdint>

namespace engine::renderer {

namespace {

std::uint32_t g_nextHandle = 0U;

/// Next nonzero handle value (0 is every handle type's invalid value).
std::uint32_t next_handle() noexcept {
  if (++g_nextHandle == 0U) {
    ++g_nextHandle;
  }
  return g_nextHandle;
}

DeviceBufferHandle null_create_buffer(const BufferDesc &) noexcept {
  return DeviceBufferHandle{next_handle()};
}
void null_update_buffer(DeviceBufferHandle, const void *,
                        std::ptrdiff_t) noexcept {}
void null_update_buffer_range(DeviceBufferHandle, const void *,
                              std::ptrdiff_t) noexcept {}
void null_destroy_buffer(DeviceBufferHandle) noexcept {}
void null_bind_uniform_buffer_slot(std::uint32_t,
                                   DeviceBufferHandle) noexcept {}

DeviceTextureHandle null_create_texture(const TextureDesc &) noexcept {
  return DeviceTextureHandle{next_handle()};
}
void null_update_texture(DeviceTextureHandle, const void *, std::int32_t,
                         std::int32_t) noexcept {}
void null_destroy_texture(DeviceTextureHandle) noexcept {}
void null_bind_texture_slot(std::uint32_t, DeviceTextureHandle) noexcept {}

DeviceProgramHandle null_create_program(const char *, const char *) noexcept {
  return DeviceProgramHandle{next_handle()};
}
DeviceProgramHandle null_create_program_binary(const void *, std::ptrdiff_t,
                                               const void *,
                                               std::ptrdiff_t) noexcept {
  return DeviceProgramHandle{next_handle()};
}
const char *null_cooked_program_profile() noexcept { return "glsl"; }
DeviceProgramHandle null_create_program_binary_introspected(
    const void *, std::ptrdiff_t, const void *, std::ptrdiff_t,
    const void *, std::ptrdiff_t, const void *, std::ptrdiff_t) noexcept {
  return DeviceProgramHandle{next_handle()};
}
void null_destroy_program(DeviceProgramHandle) noexcept {}
void null_bind_program(DeviceProgramHandle) noexcept {}

ShaderParam null_shader_param(DeviceProgramHandle, const char *) noexcept {
  // Every name resolves: set_param_* through the token is a defined no-op,
  // and callers that treat a missing uniform as fatal stay satisfied.
  return ShaderParam{0};
}
void null_set_param_mat4(ShaderParam, const float *) noexcept {}
void null_set_param_mat3(ShaderParam, const float *) noexcept {}
void null_set_param_f32(ShaderParam, float) noexcept {}
void null_set_param_i32(ShaderParam, std::int32_t) noexcept {}
void null_set_param_vec2(ShaderParam, const float *) noexcept {}
void null_set_param_vec3(ShaderParam, const float *) noexcept {}
void null_set_param_vec4(ShaderParam, const float *) noexcept {}
void null_set_param_vec4_array(ShaderParam, const float *,
                               std::int32_t) noexcept {}
void null_set_param_mat4_array(ShaderParam, const float *,
                               std::int32_t) noexcept {}
bool null_bind_program_uniform_block(DeviceProgramHandle, const char *,
                                     std::uint32_t) noexcept {
  return true;
}

DeviceGeometryHandle null_create_geometry(const GeometryDesc &) noexcept {
  return DeviceGeometryHandle{next_handle()};
}
void null_destroy_geometry(DeviceGeometryHandle) noexcept {}
bool null_set_geometry_instance_stream(DeviceGeometryHandle,
                                       DeviceBufferHandle,
                                       const VertexLayout &) noexcept {
  return true;
}
void null_draw(DeviceGeometryHandle, PrimitiveTopology, std::int32_t,
               std::int32_t) noexcept {}
void null_draw_indexed(DeviceGeometryHandle, std::int32_t) noexcept {}
void null_draw_indexed_instanced(DeviceGeometryHandle, std::int32_t,
                                 std::int32_t) noexcept {}

RenderTargetHandle
null_create_render_target(const RenderTargetDesc &) noexcept {
  return RenderTargetHandle{next_handle()};
}
void null_destroy_render_target(RenderTargetHandle) noexcept {}
void null_bind_render_target(RenderTargetHandle) noexcept {}
void null_copy_depth(RenderTargetHandle, RenderTargetHandle, std::int32_t,
                     std::int32_t) noexcept {}

void null_apply_render_state(const RenderState &) noexcept {}
void null_set_viewport(std::int32_t, std::int32_t, std::int32_t,
                       std::int32_t) noexcept {}
void null_clear(ClearFlags, float, float, float, float) noexcept {}

DeviceQueryHandle null_create_timestamp_query() noexcept {
  return DeviceQueryHandle{next_handle()};
}
void null_destroy_timestamp_query(DeviceQueryHandle) noexcept {}
void null_write_timestamp(DeviceQueryHandle) noexcept {}
bool null_timestamp_ready(DeviceQueryHandle) noexcept { return true; }
std::uint64_t null_timestamp_value(DeviceQueryHandle) noexcept { return 0U; }

std::uint64_t null_native_texture_id(DeviceTextureHandle) noexcept {
  return 0U;
}

DeviceDebugStats null_debug_stats() noexcept { return DeviceDebugStats{}; }

} // namespace

void fill_null_render_device(RenderDevice *device) noexcept {
  if (device == nullptr) {
    return;
  }
  *device = RenderDevice{};
  g_nextHandle = 0U;

  device->caps.instancing = true;
  device->caps.uniformBlocks = true;
  device->caps.timestampQueries = true;
  device->caps.cookedPrograms = true;
  // Generous so headless tests exercise the full pass list.
  device->caps.maxTextureSamplers = 32U;

  device->create_buffer = &null_create_buffer;
  device->update_buffer = &null_update_buffer;
  device->update_buffer_range = &null_update_buffer_range;
  device->destroy_buffer = &null_destroy_buffer;
  device->bind_uniform_buffer_slot = &null_bind_uniform_buffer_slot;
  device->create_texture = &null_create_texture;
  device->update_texture = &null_update_texture;
  device->destroy_texture = &null_destroy_texture;
  device->bind_texture_slot = &null_bind_texture_slot;
  device->create_program = &null_create_program;
  device->create_program_binary = &null_create_program_binary;
  device->create_program_binary_introspected =
      &null_create_program_binary_introspected;
  device->cooked_program_profile = &null_cooked_program_profile;
  device->destroy_program = &null_destroy_program;
  device->bind_program = &null_bind_program;
  device->shader_param = &null_shader_param;
  device->set_param_mat4 = &null_set_param_mat4;
  device->set_param_mat3 = &null_set_param_mat3;
  device->set_param_f32 = &null_set_param_f32;
  device->set_param_i32 = &null_set_param_i32;
  device->set_param_vec2 = &null_set_param_vec2;
  device->set_param_vec3 = &null_set_param_vec3;
  device->set_param_vec4 = &null_set_param_vec4;
  device->set_param_vec4_array = &null_set_param_vec4_array;
  device->set_param_mat4_array = &null_set_param_mat4_array;
  device->bind_program_uniform_block = &null_bind_program_uniform_block;
  device->create_geometry = &null_create_geometry;
  device->destroy_geometry = &null_destroy_geometry;
  device->set_geometry_instance_stream = &null_set_geometry_instance_stream;
  device->draw = &null_draw;
  device->draw_indexed = &null_draw_indexed;
  device->draw_indexed_instanced = &null_draw_indexed_instanced;
  device->create_render_target = &null_create_render_target;
  device->destroy_render_target = &null_destroy_render_target;
  device->bind_render_target = &null_bind_render_target;
  device->copy_depth = &null_copy_depth;
  device->apply_render_state = &null_apply_render_state;
  device->set_viewport = &null_set_viewport;
  device->clear = &null_clear;
  device->create_timestamp_query = &null_create_timestamp_query;
  device->destroy_timestamp_query = &null_destroy_timestamp_query;
  device->write_timestamp = &null_write_timestamp;
  device->timestamp_ready = &null_timestamp_ready;
  device->timestamp_value = &null_timestamp_value;
  device->native_texture_id = &null_native_texture_id;
  device->debug_stats = &null_debug_stats;
}

} // namespace engine::renderer
