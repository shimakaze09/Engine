// Private shared state of the bgfx render device backend (#138): the
// resource records, generational slot tables, and device context used by
// the backend's translation units (render_device_bgfx.cpp for resources,
// views, and draws; render_device_bgfx_programs.cpp for cooked program
// linking, shader parameters, and sampler application). Engine code
// above the backend never includes this header.

#pragma once

#include "device_slot_table.h"
#include "engine/renderer/render_device.h"
#include "render_device_bgfx_internal.h"

#include <cstdint>

namespace engine::renderer::bgfx_backend {

// Capacities mirror the GL backend so engine registries above the
// device see identical headroom on both backends.
inline constexpr std::size_t kMaxDeviceBuffers = 8704U;
inline constexpr std::size_t kMaxDeviceTextures = 1024U;
inline constexpr std::size_t kMaxDevicePrograms = 128U;
inline constexpr std::size_t kMaxDeviceGeometries = 4352U;
inline constexpr std::size_t kMaxDeviceTargets = 256U;
inline constexpr std::size_t kMaxTextureSlots = 16U;
inline constexpr std::size_t kMaxProgramParams = 64U;
inline constexpr std::size_t kMaxParamNameLength = 44U;

/// Realized bgfx storage behind one engine buffer handle. Vertex and
/// instance data live in a resizable dynamic vertex buffer (byte layout
/// until an instance stride re-realizes it); indices in a resizable
/// 32-bit dynamic index buffer.
struct BgfxBufferRecord final {
  BufferUsage usage = BufferUsage::Vertex;
  BufferAccess access = BufferAccess::Static;
  std::int32_t sizeBytes = 0;
  std::int32_t instanceStride = 0;
  bgfx::DynamicVertexBufferHandle vertex = BGFX_INVALID_HANDLE;
  bgfx::DynamicIndexBufferHandle index = BGFX_INVALID_HANDLE;
};

/// bgfx texture behind one engine texture handle, with the creation
/// shape needed to validate updates and attachments.
struct BgfxTextureRecord final {
  bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
  TextureKind kind = TextureKind::Tex2D;
  TextureFormat format = TextureFormat::RGBA8;
  TexelData texel = TexelData::U8;
  std::int32_t width = 0;
  std::int32_t height = 0;
  bool renderTarget = false;
};

/// Geometry: referenced engine buffer handles (resolved per draw for
/// staleness) plus the translated layout override applied at draw.
struct BgfxGeometryRecord final {
  std::uint32_t vertexBuffer = 0U;
  std::uint32_t indexBuffer = 0U;
  std::uint32_t instanceBuffer = 0U;
  std::int32_t vertexStride = 0;
  bgfx::VertexLayoutHandle layout = BGFX_INVALID_HANDLE;
};

/// Frame buffer behind one engine render-target handle; the depth
/// attachment's engine handle backs copy_depth blits.
struct BgfxTargetRecord final {
  bgfx::FrameBufferHandle handle = BGFX_INVALID_HANDLE;
  std::uint32_t depthTexture = 0U;
};

/// One resolvable shader input of a linked program: the bgfx uniform
/// handle (owned by the program's shaders, valid for the program's
/// lifetime), its type, and — for samplers — the texture slot assigned
/// through set_param_i32 (the GL texture-unit convention).
struct BgfxParamRecord final {
  char name[kMaxParamNameLength] = {};
  bgfx::UniformHandle handle = BGFX_INVALID_HANDLE;
  bgfx::UniformType::Enum type = bgfx::UniformType::Count;
  std::int8_t samplerStage = -1;
};

/// Linked program from cooked shader binaries with its introspected,
/// name-addressable parameter table.
struct BgfxProgramRecord final {
  bgfx::ProgramHandle handle = BGFX_INVALID_HANDLE;
  std::uint16_t paramCount = 0U;
  BgfxParamRecord params[kMaxProgramParams] = {};
};

/// Which backend fill initialize_render_device selected.
enum class BgfxBackendMode : std::uint8_t {
  None = 0,
  Null = 1,
  Bgfx = 2,
};

/// Owns the bgfx-backed render device state.
struct BgfxDeviceContext final {
  bool initialized = false;
  BgfxBackendMode mode = BgfxBackendMode::None;
  RenderDevice device{};
  device_slot_detail::DeviceSlotTable<BgfxBufferRecord, kMaxDeviceBuffers>
      buffers{};
  device_slot_detail::DeviceSlotTable<BgfxTextureRecord, kMaxDeviceTextures>
      textures{};
  device_slot_detail::DeviceSlotTable<BgfxProgramRecord, kMaxDevicePrograms>
      programs{};
  device_slot_detail::DeviceSlotTable<BgfxGeometryRecord, kMaxDeviceGeometries>
      geometries{};
  device_slot_detail::DeviceSlotTable<BgfxTargetRecord, kMaxDeviceTargets>
      targets{};
  DeviceDebugStats stats{};
  RenderState currentState{};
  std::uint32_t currentProgram = 0U;
  std::uint16_t currentView = 0U;
  std::uint16_t viewsUsed = 0U;
  std::uint32_t boundTextures[kMaxTextureSlots] = {};
};

/// Returns the bgfx render device context.
BgfxDeviceContext &device_context() noexcept;

/// Records one dropped operation; the first few log so the violation is
/// visible without flooding per-frame paths.
void drop_operation(const char *what) noexcept;

/// Live record of the currently bound program; nullptr when none is
/// bound or the bound handle went stale.
BgfxProgramRecord *current_program_record() noexcept;

/// Applies the program's sampler parameters for the next submit from the
/// context's bound-texture slots (stale or unbound slots are skipped).
void apply_program_samplers(const BgfxProgramRecord &program) noexcept;

// Program/parameter device entries implemented in
// render_device_bgfx_programs.cpp and wired by fill_bgfx_render_device.
DeviceProgramHandle bgfx_create_program(const char *vertexSource,
                                        const char *fragmentSource) noexcept;
DeviceProgramHandle bgfx_create_program_binary(
    const void *vertexData, std::ptrdiff_t vertexSize,
    const void *fragmentData, std::ptrdiff_t fragmentSize) noexcept;
const char *bgfx_cooked_program_profile() noexcept;
void bgfx_destroy_program(DeviceProgramHandle program) noexcept;
void bgfx_bind_program(DeviceProgramHandle program) noexcept;
ShaderParam bgfx_shader_param(DeviceProgramHandle program,
                              const char *name) noexcept;
void bgfx_set_param_mat4(ShaderParam param, const float *value) noexcept;
void bgfx_set_param_mat3(ShaderParam param, const float *value) noexcept;
void bgfx_set_param_f32(ShaderParam param, float value) noexcept;
void bgfx_set_param_i32(ShaderParam param, std::int32_t value) noexcept;
void bgfx_set_param_vec2(ShaderParam param, const float *value) noexcept;
void bgfx_set_param_vec3(ShaderParam param, const float *value) noexcept;
void bgfx_set_param_vec4(ShaderParam param, const float *value) noexcept;
bool bgfx_bind_program_uniform_block(DeviceProgramHandle program,
                                     const char *blockName,
                                     std::uint32_t slot) noexcept;

} // namespace engine::renderer::bgfx_backend
