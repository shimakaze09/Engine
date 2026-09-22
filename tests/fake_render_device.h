// The one fake render device the renderer unit tests link against in place
// of the real backend, and the render_device() seam that returns it.
//
// RenderDevice is a table of function pointers, and production code takes
// different paths depending on which entries are null. So the fake starts
// empty and a test assigns exactly the entries it needs -- from the shared
// implementations below, or from a recorder of its own when it asserts on
// something they do not keep. Every test used to carry its own copy of the
// seam, the handle counter, the alive counts and a failure switch, each
// with its own idea of which create fails.
//
// Link fake_render_device.cpp into a test instead of the real device TU.

#pragma once

#include "engine/renderer/render_device.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::tests {

/// The resource kinds the fake counts and can refuse to create.
enum class FakeKind : std::uint8_t {
  Buffer,
  Texture,
  Program,
  Geometry,
  RenderTarget,
  Query,
  Count,
};

inline constexpr std::size_t kFakeKindCount =
    static_cast<std::size_t>(FakeKind::Count);

/// The bit for `kind` in FakeDeviceLog::failKinds.
constexpr std::uint32_t fake_kind_bit(FakeKind kind) noexcept {
  return 1U << static_cast<std::uint32_t>(kind);
}

/// What the shared implementations record, and the switches that script
/// their failures.
struct FakeDeviceLog final {
  /// The next handle value handed out, shared by every kind so no two live
  /// resources carry the same value.
  std::uint32_t nextHandle = 1U;
  /// Create calls per kind, refused ones included.
  std::array<int, kFakeKindCount> creates{};
  /// Resources created and not yet destroyed, per kind.
  std::array<int, kFakeKindCount> alive{};
  /// Destroy calls per kind, null handles included.
  std::array<int, kFakeKindCount> destroys{};
  /// Create calls of every kind, in order: failCreateCall counts these.
  std::uint32_t createCalls = 0U;

  /// Every create of a kind whose bit is set here fails.
  std::uint32_t failKinds = 0U;
  /// The 1-based create call, of any kind, that fails; 0 for none.
  std::uint32_t failCreateCall = 0U;

  /// The render target last bound; 0 is the back buffer.
  std::uint32_t boundRenderTarget = 0U;
  /// Draw calls of every form, and those that landed on the back buffer.
  int draws = 0;
  int drawsToBackBuffer = 0;

  /// When false, render_device() answers as it does with no device live.
  bool present = true;
  /// What initialize_render_device() returns.
  bool initializeSucceeds = true;
};

/// The table render_device() returns.
renderer::RenderDevice &fake_device() noexcept;
/// The shared record.
FakeDeviceLog &fake_log() noexcept;
/// Empties the table and the record: every entry null, every count zero,
/// the device present.
void reset_fake_device() noexcept;

/// The shared create: counts the call, applies the failure switches, and
/// returns a fresh handle value, or 0 when the create is refused. A test's
/// own recorder calls this to keep the shared counts right.
std::uint32_t fake_create(FakeKind kind) noexcept;
/// The shared destroy: counts the call and, for a non-null handle, one
/// fewer alive.
void fake_destroy(FakeKind kind, std::uint32_t value) noexcept;
/// Alive resources of a kind.
int fake_alive(FakeKind kind) noexcept;
/// Create calls of a kind.
int fake_creates(FakeKind kind) noexcept;
/// Destroy calls of a kind.
int fake_destroys(FakeKind kind) noexcept;

// Shared entry implementations, one per RenderDevice entry the tests use.
// Creates and destroys go through fake_create and fake_destroy; binds and
// draws record into the log; everything else does nothing.
namespace fake {

renderer::DeviceBufferHandle
create_buffer(const renderer::BufferDesc &) noexcept;
void update_buffer(renderer::DeviceBufferHandle, const void *,
                   std::ptrdiff_t) noexcept;
void destroy_buffer(renderer::DeviceBufferHandle buffer) noexcept;

renderer::DeviceTextureHandle
create_texture(const renderer::TextureDesc &) noexcept;
void destroy_texture(renderer::DeviceTextureHandle texture) noexcept;
void bind_texture_slot(std::uint32_t, renderer::DeviceTextureHandle) noexcept;

renderer::DeviceProgramHandle create_program_binary(const void *,
                                                    std::ptrdiff_t,
                                                    const void *,
                                                    std::ptrdiff_t) noexcept;
void destroy_program(renderer::DeviceProgramHandle program) noexcept;
void bind_program(renderer::DeviceProgramHandle) noexcept;

renderer::DeviceGeometryHandle
create_geometry(const renderer::GeometryDesc &) noexcept;
void destroy_geometry(renderer::DeviceGeometryHandle geometry) noexcept;

renderer::RenderTargetHandle
create_render_target(const renderer::RenderTargetDesc &) noexcept;
void destroy_render_target(renderer::RenderTargetHandle target) noexcept;
void bind_render_target(renderer::RenderTargetHandle target) noexcept;
void copy_depth(renderer::RenderTargetHandle, renderer::RenderTargetHandle,
                std::int32_t, std::int32_t) noexcept;

void set_param_mat4(renderer::ShaderParam, const float *) noexcept;
void set_param_mat3(renderer::ShaderParam, const float *) noexcept;
void set_param_f32(renderer::ShaderParam, float) noexcept;
void set_param_i32(renderer::ShaderParam, std::int32_t) noexcept;
void set_param_vec2(renderer::ShaderParam, const float *) noexcept;
void set_param_vec3(renderer::ShaderParam, const float *) noexcept;
void set_param_vec4(renderer::ShaderParam, const float *) noexcept;
void set_param_vec4_array(renderer::ShaderParam, const float *,
                          std::int32_t) noexcept;
void set_param_mat4_array(renderer::ShaderParam, const float *,
                          std::int32_t) noexcept;

void draw(renderer::DeviceGeometryHandle, renderer::PrimitiveTopology,
          std::int32_t, std::int32_t) noexcept;

void apply_render_state(const renderer::RenderState &) noexcept;
void set_viewport(std::int32_t, std::int32_t, std::int32_t,
                  std::int32_t) noexcept;
void clear(renderer::ClearFlags, float, float, float, float) noexcept;

} // namespace fake

} // namespace engine::tests
