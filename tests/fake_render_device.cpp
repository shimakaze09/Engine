// Implements the shared fake render device and the device seams the
// renderer TUs under test link against (see fake_render_device.h).

#include "fake_render_device.h"

namespace engine::tests {

namespace {

renderer::RenderDevice g_device{};
FakeDeviceLog g_log{};

std::size_t index_of(FakeKind kind) noexcept {
  return static_cast<std::size_t>(kind);
}

} // namespace

renderer::RenderDevice &fake_device() noexcept { return g_device; }

FakeDeviceLog &fake_log() noexcept { return g_log; }

void reset_fake_device() noexcept {
  g_device = renderer::RenderDevice{};
  g_log = FakeDeviceLog{};
}

std::uint32_t fake_create(FakeKind kind) noexcept {
  ++g_log.creates[index_of(kind)];
  ++g_log.createCalls;
  const bool refused = ((g_log.failKinds & fake_kind_bit(kind)) != 0U) ||
                       ((g_log.failCreateCall != 0U) &&
                        (g_log.createCalls == g_log.failCreateCall));
  if (refused) {
    return 0U;
  }
  ++g_log.alive[index_of(kind)];
  return g_log.nextHandle++;
}

void fake_destroy(FakeKind kind, std::uint32_t value) noexcept {
  ++g_log.destroys[index_of(kind)];
  if (value != 0U) {
    --g_log.alive[index_of(kind)];
  }
}

int fake_alive(FakeKind kind) noexcept { return g_log.alive[index_of(kind)]; }

int fake_creates(FakeKind kind) noexcept {
  return g_log.creates[index_of(kind)];
}

int fake_destroys(FakeKind kind) noexcept {
  return g_log.destroys[index_of(kind)];
}

namespace fake {

renderer::DeviceBufferHandle
create_buffer(const renderer::BufferDesc &) noexcept {
  return renderer::DeviceBufferHandle{fake_create(FakeKind::Buffer)};
}

void update_buffer(renderer::DeviceBufferHandle, const void *,
                   std::ptrdiff_t) noexcept {}

void destroy_buffer(renderer::DeviceBufferHandle buffer) noexcept {
  fake_destroy(FakeKind::Buffer, buffer.value);
}

renderer::DeviceTextureHandle
create_texture(const renderer::TextureDesc &) noexcept {
  return renderer::DeviceTextureHandle{fake_create(FakeKind::Texture)};
}

void destroy_texture(renderer::DeviceTextureHandle texture) noexcept {
  fake_destroy(FakeKind::Texture, texture.value);
}

void bind_texture_slot(std::uint32_t, renderer::DeviceTextureHandle) noexcept {}

renderer::DeviceProgramHandle create_program_binary(const void *,
                                                    std::ptrdiff_t,
                                                    const void *,
                                                    std::ptrdiff_t) noexcept {
  return renderer::DeviceProgramHandle{fake_create(FakeKind::Program)};
}

void destroy_program(renderer::DeviceProgramHandle program) noexcept {
  fake_destroy(FakeKind::Program, program.value);
}

void bind_program(renderer::DeviceProgramHandle) noexcept {}

renderer::DeviceGeometryHandle
create_geometry(const renderer::GeometryDesc &) noexcept {
  return renderer::DeviceGeometryHandle{fake_create(FakeKind::Geometry)};
}

void destroy_geometry(renderer::DeviceGeometryHandle geometry) noexcept {
  fake_destroy(FakeKind::Geometry, geometry.value);
}

renderer::RenderTargetHandle
create_render_target(const renderer::RenderTargetDesc &) noexcept {
  return renderer::RenderTargetHandle{fake_create(FakeKind::RenderTarget)};
}

void destroy_render_target(renderer::RenderTargetHandle target) noexcept {
  fake_destroy(FakeKind::RenderTarget, target.value);
}

void bind_render_target(renderer::RenderTargetHandle target) noexcept {
  g_log.boundRenderTarget = target.value;
}

void copy_depth(renderer::RenderTargetHandle, renderer::RenderTargetHandle,
                std::int32_t, std::int32_t) noexcept {}

void set_param_mat4(renderer::ShaderParam, const float *) noexcept {}
void set_param_mat3(renderer::ShaderParam, const float *) noexcept {}
void set_param_f32(renderer::ShaderParam, float) noexcept {}
void set_param_i32(renderer::ShaderParam, std::int32_t) noexcept {}
void set_param_vec2(renderer::ShaderParam, const float *) noexcept {}
void set_param_vec3(renderer::ShaderParam, const float *) noexcept {}
void set_param_vec4(renderer::ShaderParam, const float *) noexcept {}
void set_param_vec4_array(renderer::ShaderParam, const float *,
                          std::int32_t) noexcept {}
void set_param_mat4_array(renderer::ShaderParam, const float *,
                          std::int32_t) noexcept {}

void draw(renderer::DeviceGeometryHandle, renderer::PrimitiveTopology,
          std::int32_t, std::int32_t) noexcept {
  ++g_log.draws;
  if (g_log.boundRenderTarget == 0U) {
    ++g_log.drawsToBackBuffer;
  }
}

void apply_render_state(const renderer::RenderState &) noexcept {}
void set_viewport(std::int32_t, std::int32_t, std::int32_t,
                  std::int32_t) noexcept {}
void clear(renderer::ClearFlags, float, float, float, float) noexcept {}

} // namespace fake

} // namespace engine::tests

namespace engine::renderer {

// The seams the renderer TUs under test call in place of the real backend.

const RenderDevice *render_device() noexcept {
  return tests::fake_log().present ? &tests::fake_device() : nullptr;
}

bool initialize_render_device() noexcept {
  return tests::fake_log().initializeSucceeds;
}

void shutdown_render_device() noexcept {}

} // namespace engine::renderer
