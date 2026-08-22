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
// Matches BGFX_CONFIG_MAX_TEXTURE_SAMPLERS=32 set by the build (#138):
// the deferred pass binds engine texture units up to 21.
inline constexpr std::size_t kMaxTextureSlots = 32U;
// Sized for the largest engine program: the PBR_FULL forward variant
// carries ~50 material/light/fog uniforms plus 15 shadow/IBL samplers
// and their matrices, overflowing the previous 64-entry table.
inline constexpr std::size_t kMaxProgramParams = 96U;
inline constexpr std::size_t kMaxParamNameLength = 44U;

/// bgfx storage behind one engine buffer handle. bgfx requires the
/// vertex layout at buffer creation (stride overrides at draw are
/// rejected), so vertex data stages on the CPU until the first
/// geometry/instance-stream attachment realizes the dynamic buffer at
/// its real stride; indices realize eagerly as resizable 32-bit dynamic
/// index buffers. `staging` is owned by the record and freed at
/// realization or destroy.
struct BgfxBufferRecord final {
  BufferUsage usage = BufferUsage::Vertex;
  BufferAccess access = BufferAccess::Static;
  std::int32_t sizeBytes = 0;
  std::int32_t strideBytes = 0; // realized vertex/instance stride
  void *staging = nullptr;
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

/// Geometry: referenced engine buffer handles, resolved per draw for
/// staleness (the vertex layout lives on the realized buffer).
struct BgfxGeometryRecord final {
  std::uint32_t vertexBuffer = 0U;
  std::uint32_t indexBuffer = 0U;
  std::uint32_t instanceBuffer = 0U;
  std::int32_t vertexStride = 0;
};

/// Frame buffer behind one engine render-target handle; the depth
/// attachment's engine handle backs copy_depth blits.
struct BgfxTargetRecord final {
  bgfx::FrameBufferHandle handle = BGFX_INVALID_HANDLE;
  std::uint32_t depthTexture = 0U;
};

// The global uniform registry can hold every distinct uniform name the
// engine's programs declare; ~60 exist today, the cap leaves headroom.
inline constexpr std::size_t kMaxGlobalUniforms = 512U;

/// One uniform in the device-lifetime global registry. bgfx uniforms
/// are global by name (one value shared by every program), so staging
/// lives here rather than per program: ShaderParam tokens are indices
/// into this table, which makes setting a parameter correct regardless
/// of which program is bound — the class of defect where a token from
/// program A silently indexed program B's table cannot occur. Value
/// sets stage into `pending` and apply once per submit through the
/// context's dirty list (bgfx allows one setUniform per uniform per
/// draw; GL semantics are last-write-wins), sized for the largest
/// staged payload (vec4[32] = 128 floats). The handle is created by
/// the registry (bgfx refcounts by name) and destroyed at device
/// shutdown, so it never dangles when a program that also referenced
/// the name is destroyed first.
struct BgfxGlobalUniform final {
  char name[kMaxParamNameLength] = {};
  bgfx::UniformHandle handle = BGFX_INVALID_HANDLE;
  bgfx::UniformType::Enum type = bgfx::UniformType::Count;
  // Largest array element count any program declared for this name;
  // the handle must be created at least this size or bgfx clamps
  // array uploads.
  std::uint16_t declaredNum = 1U;
  std::int8_t samplerStage = -1;
  bool dirty = false;
  std::uint16_t pendingNum = 0U;
  float pending[128] = {};
};

/// Linked program from cooked shader binaries. The program only lists
/// which global uniforms its shaders declare — shader_param answers
/// from this list; values and sampler stages live in the global
/// registry.
struct BgfxProgramRecord final {
  bgfx::ProgramHandle handle = BGFX_INVALID_HANDLE;
  std::uint16_t paramCount = 0U;
  std::uint16_t paramIndices[kMaxProgramParams] = {};
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
  // Global uniform registry (see BgfxGlobalUniform) with the dirty list
  // applied once per submit; both reset with the device.
  BgfxGlobalUniform uniforms[kMaxGlobalUniforms] = {};
  std::uint16_t uniformCount = 0U;
  std::uint16_t dirtyUniforms[kMaxGlobalUniforms] = {};
  std::uint16_t dirtyUniformCount = 0U;
  DeviceDebugStats stats{};
  RenderState currentState{};
  std::uint32_t currentProgram = 0U;
  std::uint16_t currentView = 0U;
  std::uint16_t viewsUsed = 0U;
  std::uint32_t boundTextures[kMaxTextureSlots] = {};
  // Swapchain reset tracking (#138 platform bring-up): the frame hook
  // re-resets bgfx when the drawable size or vsync intent changes.
  std::int32_t backBufferWidth = 0;
  std::int32_t backBufferHeight = 0;
  bool backBufferVsync = false;
  // Backend-owned fullscreen triangle (#138 forward path): bgfx submits
  // require a vertex stream, so attribute-less engine draws bind this
  // three-vertex position stream instead (fullscreen.vs.sc reads it).
  bgfx::DynamicVertexBufferHandle fullscreenVertex = BGFX_INVALID_HANDLE;
  bgfx::VertexLayoutHandle fullscreenLayout = BGFX_INVALID_HANDLE;
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

/// Flushes every staged (dirty) global uniform for the next submit —
/// one setUniform per staged entry; values persist globally in bgfx
/// across draws and programs, so clean entries are skipped.
void apply_staged_uniforms() noexcept;

/// Destroys the global uniform registry's bgfx handles and resets it;
/// called from device shutdown.
void reset_global_uniforms() noexcept;

// Program/parameter device entries implemented in
// render_device_bgfx_programs.cpp and wired by fill_bgfx_render_device.
DeviceProgramHandle bgfx_create_program_binary(
    const void *vertexData, std::ptrdiff_t vertexSize,
    const void *fragmentData, std::ptrdiff_t fragmentSize) noexcept;
const char *bgfx_cooked_program_profile() noexcept;
DeviceProgramHandle bgfx_create_program_binary_introspected(
    const void *vertexData, std::ptrdiff_t vertexSize,
    const void *fragmentData, std::ptrdiff_t fragmentSize,
    const void *vertexMeta, std::ptrdiff_t vertexMetaSize,
    const void *fragmentMeta, std::ptrdiff_t fragmentMetaSize) noexcept;
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
void bgfx_set_param_vec4_array(ShaderParam param, const float *values,
                               std::int32_t count) noexcept;
void bgfx_set_param_mat4_array(ShaderParam param, const float *values,
                               std::int32_t count) noexcept;
bool bgfx_bind_program_uniform_block(DeviceProgramHandle program,
                                     const char *blockName,
                                     std::uint32_t slot) noexcept;

} // namespace engine::renderer::bgfx_backend
