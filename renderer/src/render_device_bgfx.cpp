// Implements the bgfx render device backend (#138 Phase B): the engine
// RenderDevice table over bgfx with the same generational slot tables,
// stale-handle detection, and dropped-operation diagnostics as the GL
// backend. bgfx runs single-threaded on the Noop renderer until Phase D
// ports the platform window and presentation path; program creation
// fails by contract until the Phase C shaderc cook supplies compiled
// shaders (bgfx has no runtime GLSL compile), so draw submission drops
// visibly on the missing program. Vertex buffers are realized as
// resizable byte-layout dynamic buffers with per-geometry layout
// overrides at draw; buffer/texture uploads copy through bgfx transient
// memory (bgfx's required ownership model — the per-frame allocation is
// bgfx-internal and unavoidable at this boundary).

#include "render_device_bgfx.h"

#include "device_slot_table.h"
#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/renderer/render_device.h"
#include "render_device_bgfx_internal.h"
#include "render_device_null.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 3)
#endif
#include <bx/math.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace engine::renderer {

using bgfx_detail::bgfx_clear_flags;
using bgfx_detail::bgfx_clear_rgba;
using bgfx_detail::bgfx_sampler_flags;
using bgfx_detail::bgfx_state_bits;
using bgfx_detail::bgfx_stride_layout;
using bgfx_detail::bgfx_texel_upload;
using bgfx_detail::bgfx_vertex_layout;
using bgfx_detail::BgfxTexelUpload;
using bgfx_detail::TexelStagingOp;

namespace {

// Capacities mirror the GL backend so engine registries above the
// device see identical headroom on both backends.
constexpr std::size_t kMaxDeviceBuffers = 8704U;
constexpr std::size_t kMaxDeviceTextures = 1024U;
constexpr std::size_t kMaxDevicePrograms = 128U;
constexpr std::size_t kMaxDeviceGeometries = 4352U;
constexpr std::size_t kMaxDeviceTargets = 256U;
constexpr std::size_t kMaxTextureSlots = 16U;

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
  device_slot_detail::DeviceSlotTable<BgfxGeometryRecord, kMaxDeviceGeometries>
      geometries{};
  device_slot_detail::DeviceSlotTable<BgfxTargetRecord, kMaxDeviceTargets>
      targets{};
  DeviceDebugStats stats{};
  RenderState currentState{};
  std::uint16_t currentView = 0U;
  std::uint16_t viewsUsed = 0U;
  std::uint32_t boundTextures[kMaxTextureSlots] = {};
};

/// Returns the bgfx render device context.
BgfxDeviceContext &device_context() noexcept {
  static BgfxDeviceContext context{};
  return context;
}

/// Records one dropped operation; the first few log so the violation is
/// visible without flooding per-frame paths.
void drop_operation(const char *what) noexcept {
  DeviceDebugStats &stats = device_context().stats;
  ++stats.droppedOperations;
  if (stats.droppedOperations <= 8U) {
    char msg[160] = {};
    std::snprintf(msg, sizeof(msg),
                  "dropped device operation (invalid or stale argument): %s",
                  what);
    core::log_message(core::LogLevel::Error, "render_device", msg);
  }
}

/// Routes bgfx fatal errors and (cvar-gated) trace output into the
/// engine log; cache/screenshot/capture callbacks stay inert. Virtual
/// dispatch is bgfx's callback contract and only runs on cold paths.
class BgfxCallback final : public bgfx::CallbackI {
public:
  ~BgfxCallback() override = default;

  void fatal(const char *filePath, std::uint16_t line, bgfx::Fatal::Enum code,
             const char *str) override {
    char msg[256] = {};
    std::snprintf(msg, sizeof(msg), "bgfx fatal %d at %s:%u: %s",
                  static_cast<int>(code),
                  (filePath != nullptr) ? filePath : "<unknown>",
                  static_cast<unsigned>(line), (str != nullptr) ? str : "");
    core::log_message(core::LogLevel::Error, "bgfx", msg);
    // bgfx's CallbackI contract: returning from a non-DebugCheck fatal
    // continues into undefined state, so the process must stop here.
    if (code != bgfx::Fatal::DebugCheck) {
      std::abort();
    }
  }

  void traceVargs(const char *filePath, std::uint16_t line, const char *format,
                  va_list argList) override {
    if (!core::cvar_get_bool("r_bgfx_trace", false)) {
      return;
    }
    static_cast<void>(filePath);
    static_cast<void>(line);
    char msg[256] = {};
    std::vsnprintf(msg, sizeof(msg), format, argList);
    const std::size_t len = std::strlen(msg);
    if ((len > 0U) && (msg[len - 1U] == '\n')) {
      msg[len - 1U] = '\0';
    }
    core::log_message(core::LogLevel::Info, "bgfx", msg);
  }

  void profilerBegin(const char *, std::uint32_t, const char *,
                     std::uint16_t) override {}
  void profilerBeginLiteral(const char *, std::uint32_t, const char *,
                            std::uint16_t) override {}
  void profilerEnd() override {}
  std::uint32_t cacheReadSize(std::uint64_t) override { return 0U; }
  bool cacheRead(std::uint64_t, void *, std::uint32_t) override {
    return false;
  }
  void cacheWrite(std::uint64_t, const void *, std::uint32_t) override {}
  void screenShot(const char *, std::uint32_t, std::uint32_t, std::uint32_t,
                  bgfx::TextureFormat::Enum, const void *, std::uint32_t,
                  bool) override {}
  void captureBegin(std::uint32_t, std::uint32_t, std::uint32_t,
                    bgfx::TextureFormat::Enum, bool) override {}
  void captureEnd() override {}
  void captureFrame(const void *, std::uint32_t) override {}
};

/// Resets per-frame view allocation: view 0 targets the back buffer in
/// submission order so state calls before the first bind have a home.
void reset_views() noexcept {
  BgfxDeviceContext &ctx = device_context();
  bgfx::setViewFrameBuffer(0U, BGFX_INVALID_HANDLE);
  bgfx::setViewMode(0U, bgfx::ViewMode::Sequential);
  ctx.currentView = 0U;
  ctx.viewsUsed = 1U;
}

// --- Texel staging ---

/// Stages one rectangle of client texels into bgfx-owned memory,
/// applying the format's staging operation (half packing / RGBA
/// widening). Returns nullptr when the size overflows bgfx's 32-bit
/// allocation limit.
const bgfx::Memory *stage_texels(const BgfxTexelUpload &shape,
                                 std::int32_t width, std::int32_t height,
                                 const void *pixels) noexcept {
  const std::uint64_t pixelCount = static_cast<std::uint64_t>(width) *
                                   static_cast<std::uint64_t>(height);
  const std::uint64_t dstBytes =
      pixelCount * static_cast<std::uint64_t>(shape.dstBytesPerPixel);
  if ((dstBytes == 0U) || (dstBytes > 0x7FFFFFFFU)) {
    return nullptr;
  }
  if (shape.op == TexelStagingOp::Copy) {
    return bgfx::copy(pixels, static_cast<std::uint32_t>(dstBytes));
  }
  const bgfx::Memory *mem =
      bgfx::alloc(static_cast<std::uint32_t>(dstBytes));
  const float *src = static_cast<const float *>(pixels);
  std::uint16_t *dst = reinterpret_cast<std::uint16_t *>(mem->data);
  constexpr std::uint16_t kOneHalf = 0x3C00U;
  for (std::uint64_t i = 0U; i < pixelCount; ++i) {
    for (std::int32_t c = 0; c < shape.components; ++c) {
      *dst++ = bx::halfFromFloat(*src++);
    }
    if (shape.op == TexelStagingOp::WidenPackHalf) {
      *dst++ = kOneHalf;
    }
  }
  return mem;
}

// --- Buffers ---

DeviceBufferHandle bgfx_create_buffer(const BufferDesc &desc) noexcept {
  if (desc.sizeBytes < 0) {
    drop_operation("create_buffer: negative size");
    return kInvalidDeviceBuffer;
  }
  if (desc.usage == BufferUsage::Uniform) {
    // bgfx exposes no arbitrary uniform-buffer binding; the caps flag is
    // false and callers gate on it (uniform data reaches shaders through
    // named parameters once the Phase C cook lands).
    static bool logged = false;
    if (!logged) {
      logged = true;
      core::log_message(core::LogLevel::Info, "render_device",
                        "bgfx backend: uniform buffers unavailable "
                        "(caps.uniformBlocks false)");
    }
    return kInvalidDeviceBuffer;
  }

  BgfxBufferRecord record{};
  record.usage = desc.usage;
  record.access = desc.access;
  record.sizeBytes = static_cast<std::int32_t>(desc.sizeBytes);
  const std::uint32_t sizeBytes =
      static_cast<std::uint32_t>(desc.sizeBytes);
  // Both access modes realize as resizable dynamic storage: the engine
  // contract lets "static" buffers be re-specified, which bgfx static
  // buffers cannot do. A never-updated static fast path is a Phase G
  // measurement, not a semantic requirement.
  if (desc.usage == BufferUsage::Index) {
    record.index = bgfx::createDynamicIndexBuffer(
        (sizeBytes > 4U) ? (sizeBytes / 4U) : 1U,
        BGFX_BUFFER_INDEX32 | BGFX_BUFFER_ALLOW_RESIZE);
    if (!bgfx::isValid(record.index)) {
      core::log_message(core::LogLevel::Error, "render_device",
                        "bgfx backend: index buffer creation failed");
      return kInvalidDeviceBuffer;
    }
    if ((desc.data != nullptr) && (sizeBytes > 0U)) {
      bgfx::update(record.index, 0U, bgfx::copy(desc.data, sizeBytes));
    }
  } else {
    bgfx::VertexLayout byteLayout{};
    bgfx_stride_layout(1, &byteLayout);
    record.vertex = bgfx::createDynamicVertexBuffer(
        (sizeBytes > 0U) ? sizeBytes : 1U, byteLayout,
        BGFX_BUFFER_ALLOW_RESIZE);
    if (!bgfx::isValid(record.vertex)) {
      core::log_message(core::LogLevel::Error, "render_device",
                        "bgfx backend: vertex buffer creation failed");
      return kInvalidDeviceBuffer;
    }
    if ((desc.data != nullptr) && (sizeBytes > 0U)) {
      bgfx::update(record.vertex, 0U, bgfx::copy(desc.data, sizeBytes));
    }
  }
  const std::uint32_t value = device_context().buffers.allocate(record);
  if (value == 0U) {
    if (bgfx::isValid(record.vertex)) {
      bgfx::destroy(record.vertex);
    }
    if (bgfx::isValid(record.index)) {
      bgfx::destroy(record.index);
    }
    drop_operation("create_buffer: table full");
    return kInvalidDeviceBuffer;
  }
  return DeviceBufferHandle{value};
}

/// Shared upload for update_buffer / update_buffer_range; allowGrow
/// distinguishes full re-specification from prefix overwrite.
void bgfx_buffer_upload(DeviceBufferHandle buffer, const void *data,
                        std::ptrdiff_t sizeBytes, bool allowGrow) noexcept {
  BgfxBufferRecord *record =
      device_context().buffers.resolve(buffer.value);
  if ((record == nullptr) || (data == nullptr) || (sizeBytes <= 0)) {
    drop_operation("update_buffer: stale handle or invalid data");
    return;
  }
  if (!allowGrow &&
      (sizeBytes > static_cast<std::ptrdiff_t>(record->sizeBytes))) {
    drop_operation("update_buffer_range: exceeds allocated size");
    return;
  }
  const std::uint32_t bytes = static_cast<std::uint32_t>(sizeBytes);
  if (bgfx::isValid(record->index)) {
    bgfx::update(record->index, 0U, bgfx::copy(data, bytes));
  } else if (bgfx::isValid(record->vertex)) {
    bgfx::update(record->vertex, 0U, bgfx::copy(data, bytes));
  }
  if (allowGrow &&
      (sizeBytes > static_cast<std::ptrdiff_t>(record->sizeBytes))) {
    record->sizeBytes = static_cast<std::int32_t>(sizeBytes);
  }
}

void bgfx_update_buffer(DeviceBufferHandle buffer, const void *data,
                        std::ptrdiff_t sizeBytes) noexcept {
  bgfx_buffer_upload(buffer, data, sizeBytes, true);
}

void bgfx_update_buffer_range(DeviceBufferHandle buffer, const void *data,
                              std::ptrdiff_t sizeBytes) noexcept {
  bgfx_buffer_upload(buffer, data, sizeBytes, false);
}

void bgfx_destroy_buffer(DeviceBufferHandle buffer) noexcept {
  if (buffer.value == 0U) {
    return;
  }
  BgfxBufferRecord *record =
      device_context().buffers.resolve(buffer.value);
  if (record == nullptr) {
    return; // idempotent destroy
  }
  if (bgfx::isValid(record->vertex)) {
    bgfx::destroy(record->vertex);
  }
  if (bgfx::isValid(record->index)) {
    bgfx::destroy(record->index);
  }
  device_context().buffers.release(buffer.value);
}

void bgfx_bind_uniform_buffer_slot(std::uint32_t,
                                   DeviceBufferHandle buffer) noexcept {
  if (buffer.value == 0U) {
    return;
  }
  drop_operation("bind_uniform_buffer_slot: caps.uniformBlocks is false");
}

// --- Textures ---

DeviceTextureHandle bgfx_create_texture(const TextureDesc &desc) noexcept {
  const BgfxTexelUpload shape =
      bgfx_texel_upload(desc.format, desc.pixelData);
  if (!shape.valid || (desc.width <= 0) ||
      ((desc.kind == TextureKind::Tex2D) && (desc.height <= 0)) ||
      (desc.mipLevels < 0)) {
    drop_operation("create_texture: invalid descriptor");
    return kInvalidDeviceTexture;
  }
  const bool hasPixels = (desc.kind == TextureKind::Tex2D)
                             ? (desc.pixels != nullptr)
                             : (desc.facePixels != nullptr);
  if ((desc.format == TextureFormat::Depth24) && hasPixels) {
    drop_operation("create_texture: depth formats take no client texels");
    return kInvalidDeviceTexture;
  }

  BgfxTextureRecord record{};
  record.kind = desc.kind;
  record.format = desc.format;
  record.texel = desc.pixelData;
  record.width = desc.width;
  record.height =
      (desc.kind == TextureKind::Cube) ? desc.width : desc.height;
  // Empty creation ("null for an empty target" per the contract) is the
  // render-target signal: bgfx requires RT/blit intent at creation while
  // the engine descriptor carries none. Every current empty-creation
  // site is an attachment; a texture created empty for CPU streaming
  // would surface as dropped updates below and needs an explicit usage
  // field on TextureDesc (Phase D revisit).
  record.renderTarget = !hasPixels;
  std::uint64_t flags = bgfx_sampler_flags(desc.filter, desc.wrap);
  if (record.renderTarget) {
    flags |= BGFX_TEXTURE_RT;
    if (desc.format == TextureFormat::Depth24) {
      flags |= BGFX_TEXTURE_BLIT_DST;
    }
  }
  // mipLevels 0 asks for a runtime-generated chain, which bgfx cannot
  // do; the chain is allocated and generation moves to the Phase C cook.
  const bool hasMips = (desc.mipLevels != 1);
  if (desc.mipLevels == 0) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      core::log_message(core::LogLevel::Info, "render_device",
                        "bgfx backend: runtime mip generation "
                        "unavailable; levels beyond 0 stay empty until "
                        "the cook supplies them (#138 Phase C)");
    }
  }

  if (desc.kind == TextureKind::Tex2D) {
    record.handle = bgfx::createTexture2D(
        static_cast<std::uint16_t>(desc.width),
        static_cast<std::uint16_t>(desc.height), hasMips, 1U, shape.format,
        flags);
  } else {
    record.handle = bgfx::createTextureCube(
        static_cast<std::uint16_t>(desc.width), hasMips, 1U, shape.format,
        flags);
  }
  if (!bgfx::isValid(record.handle)) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "bgfx backend: texture creation failed");
    return kInvalidDeviceTexture;
  }

  const std::uint16_t pitch = static_cast<std::uint16_t>(
      desc.width * shape.dstBytesPerPixel);
  if ((desc.kind == TextureKind::Tex2D) && (desc.pixels != nullptr)) {
    const bgfx::Memory *mem =
        stage_texels(shape, desc.width, desc.height, desc.pixels);
    if (mem != nullptr) {
      bgfx::updateTexture2D(record.handle, 0U, 0U, 0U, 0U,
                            static_cast<std::uint16_t>(desc.width),
                            static_cast<std::uint16_t>(desc.height), mem,
                            pitch);
    }
  } else if ((desc.kind == TextureKind::Cube) &&
             (desc.facePixels != nullptr)) {
    for (std::uint8_t face = 0U; face < 6U; ++face) {
      if (desc.facePixels[face] == nullptr) {
        continue;
      }
      const bgfx::Memory *mem =
          stage_texels(shape, desc.width, desc.width, desc.facePixels[face]);
      if (mem != nullptr) {
        bgfx::updateTextureCube(record.handle, 0U, face, 0U, 0U, 0U,
                                static_cast<std::uint16_t>(desc.width),
                                static_cast<std::uint16_t>(desc.width), mem,
                                pitch);
      }
    }
  }

  const std::uint32_t value = device_context().textures.allocate(record);
  if (value == 0U) {
    bgfx::destroy(record.handle);
    drop_operation("create_texture: table full");
    return kInvalidDeviceTexture;
  }
  return DeviceTextureHandle{value};
}

void bgfx_update_texture(DeviceTextureHandle texture, const void *pixels,
                         std::int32_t width, std::int32_t height) noexcept {
  BgfxTextureRecord *record =
      device_context().textures.resolve(texture.value);
  if ((record == nullptr) || (pixels == nullptr) || (width <= 0) ||
      (height <= 0) || (width > record->width) ||
      (height > record->height)) {
    drop_operation("update_texture: stale handle or invalid rectangle");
    return;
  }
  if (record->renderTarget || (record->kind != TextureKind::Tex2D)) {
    drop_operation("update_texture: target or cube texture");
    return;
  }
  const BgfxTexelUpload shape =
      bgfx_texel_upload(record->format, record->texel);
  const bgfx::Memory *mem = stage_texels(shape, width, height, pixels);
  if (mem == nullptr) {
    drop_operation("update_texture: staging overflow");
    return;
  }
  bgfx::updateTexture2D(
      record->handle, 0U, 0U, 0U, 0U, static_cast<std::uint16_t>(width),
      static_cast<std::uint16_t>(height), mem,
      static_cast<std::uint16_t>(width * shape.dstBytesPerPixel));
}

void bgfx_destroy_texture(DeviceTextureHandle texture) noexcept {
  if (texture.value == 0U) {
    return;
  }
  BgfxTextureRecord *record =
      device_context().textures.resolve(texture.value);
  if (record == nullptr) {
    return; // idempotent destroy
  }
  bgfx::destroy(record->handle);
  device_context().textures.release(texture.value);
}

void bgfx_bind_texture_slot(std::uint32_t slot,
                            DeviceTextureHandle texture) noexcept {
  BgfxDeviceContext &ctx = device_context();
  if (slot >= kMaxTextureSlots) {
    drop_operation("bind_texture_slot: slot out of range");
    return;
  }
  if (texture.value == 0U) {
    ctx.boundTextures[slot] = 0U;
    return;
  }
  if (ctx.textures.resolve(texture.value) == nullptr) {
    drop_operation("bind_texture_slot: stale texture");
    return;
  }
  // bgfx attaches textures per submit through sampler uniforms, which
  // exist only once programs do; the binding is recorded now so draws
  // can apply it when the Phase C cook lands.
  ctx.boundTextures[slot] = texture.value;
}

// --- Programs and shader parameters (pending the Phase C shader cook) ---

DeviceProgramHandle bgfx_create_program(const char *, const char *) noexcept {
  static bool logged = false;
  if (!logged) {
    logged = true;
    core::log_message(core::LogLevel::Info, "render_device",
                      "bgfx backend: runtime GLSL compilation "
                      "unavailable; programs arrive with the shaderc "
                      "cook (#138 Phase C)");
  }
  return kInvalidDeviceProgram;
}

void bgfx_destroy_program(DeviceProgramHandle) noexcept {}

void bgfx_bind_program(DeviceProgramHandle program) noexcept {
  if (program.value != 0U) {
    drop_operation("bind_program: no programs exist before the shader cook");
  }
}

ShaderParam bgfx_shader_param(DeviceProgramHandle, const char *) noexcept {
  return kInvalidShaderParam;
}

void bgfx_set_param_mat4(ShaderParam, const float *) noexcept {}
void bgfx_set_param_mat3(ShaderParam, const float *) noexcept {}
void bgfx_set_param_f32(ShaderParam, float) noexcept {}
void bgfx_set_param_i32(ShaderParam, std::int32_t) noexcept {}
void bgfx_set_param_vec2(ShaderParam, const float *) noexcept {}
void bgfx_set_param_vec3(ShaderParam, const float *) noexcept {}
void bgfx_set_param_vec4(ShaderParam, const float *) noexcept {}

bool bgfx_bind_program_uniform_block(DeviceProgramHandle, const char *,
                                     std::uint32_t) noexcept {
  return false;
}

// --- Geometry and draws ---

DeviceGeometryHandle bgfx_create_geometry(const GeometryDesc &desc) noexcept {
  BgfxDeviceContext &ctx = device_context();
  BgfxGeometryRecord record{};
  if (desc.vertexBuffer.value != 0U) {
    BgfxBufferRecord *vertex = ctx.buffers.resolve(desc.vertexBuffer.value);
    if ((vertex == nullptr) || !bgfx::isValid(vertex->vertex)) {
      drop_operation("create_geometry: stale or non-vertex buffer");
      return kInvalidDeviceGeometry;
    }
    bgfx::VertexLayout layout{};
    if (!bgfx_vertex_layout(desc.layout, &layout)) {
      drop_operation("create_geometry: invalid vertex layout");
      return kInvalidDeviceGeometry;
    }
    record.layout = bgfx::createVertexLayout(layout);
    if (!bgfx::isValid(record.layout)) {
      drop_operation("create_geometry: layout table full");
      return kInvalidDeviceGeometry;
    }
    record.vertexBuffer = desc.vertexBuffer.value;
    record.vertexStride = desc.layout.strideBytes;
  }
  if (desc.indexBuffer.value != 0U) {
    BgfxBufferRecord *index = ctx.buffers.resolve(desc.indexBuffer.value);
    if ((index == nullptr) || !bgfx::isValid(index->index)) {
      if (bgfx::isValid(record.layout)) {
        bgfx::destroy(record.layout);
      }
      drop_operation("create_geometry: stale or non-index buffer");
      return kInvalidDeviceGeometry;
    }
    record.indexBuffer = desc.indexBuffer.value;
  }
  const std::uint32_t value = ctx.geometries.allocate(record);
  if (value == 0U) {
    if (bgfx::isValid(record.layout)) {
      bgfx::destroy(record.layout);
    }
    drop_operation("create_geometry: table full");
    return kInvalidDeviceGeometry;
  }
  return DeviceGeometryHandle{value};
}

void bgfx_destroy_geometry(DeviceGeometryHandle geometry) noexcept {
  if (geometry.value == 0U) {
    return;
  }
  BgfxGeometryRecord *record =
      device_context().geometries.resolve(geometry.value);
  if (record == nullptr) {
    return; // idempotent destroy
  }
  if (bgfx::isValid(record->layout)) {
    bgfx::destroy(record->layout);
  }
  device_context().geometries.release(geometry.value);
}

bool bgfx_set_geometry_instance_stream(DeviceGeometryHandle geometry,
                                       DeviceBufferHandle buffer,
                                       const VertexLayout &layout) noexcept {
  BgfxDeviceContext &ctx = device_context();
  BgfxGeometryRecord *record = ctx.geometries.resolve(geometry.value);
  if (record == nullptr) {
    drop_operation("set_geometry_instance_stream: stale geometry");
    return false;
  }
  if (buffer.value == 0U) {
    record->instanceBuffer = 0U;
    return true;
  }
  BgfxBufferRecord *stream = ctx.buffers.resolve(buffer.value);
  if ((stream == nullptr) || !bgfx::isValid(stream->vertex) ||
      (layout.strideBytes <= 0)) {
    drop_operation("set_geometry_instance_stream: stale buffer or layout");
    return false;
  }
  // bgfx derives the instance stride from the buffer's creation layout,
  // so the byte-layout buffer is re-realized at the stream stride here.
  // Data uploaded before this call is not carried over; instance streams
  // are re-uploaded per frame by contract of their Stream access.
  if (stream->instanceStride != layout.strideBytes) {
    bgfx::destroy(stream->vertex);
    bgfx::VertexLayout strideLayout{};
    bgfx_stride_layout(layout.strideBytes, &strideLayout);
    const std::uint32_t count = static_cast<std::uint32_t>(
        (stream->sizeBytes > layout.strideBytes)
            ? (stream->sizeBytes / layout.strideBytes)
            : 1);
    stream->vertex = bgfx::createDynamicVertexBuffer(
        count, strideLayout, BGFX_BUFFER_ALLOW_RESIZE);
    if (!bgfx::isValid(stream->vertex)) {
      drop_operation("set_geometry_instance_stream: re-realization failed");
      return false;
    }
    stream->instanceStride = layout.strideBytes;
  }
  record->instanceBuffer = buffer.value;
  return true;
}

/// Applies the geometry's streams for one draw; false (with the drop
/// recorded) when a referenced buffer went stale. Draws cannot submit
/// until programs exist (#138 Phase C), so every draw currently ends in
/// a recorded drop on the missing program rather than a bgfx submit.
bool bgfx_apply_geometry(const BgfxGeometryRecord &record,
                         std::int32_t firstVertex,
                         std::int32_t vertexCount) noexcept {
  BgfxDeviceContext &ctx = device_context();
  if (record.vertexBuffer != 0U) {
    BgfxBufferRecord *vertex = ctx.buffers.resolve(record.vertexBuffer);
    if ((vertex == nullptr) || !bgfx::isValid(vertex->vertex)) {
      drop_operation("draw: stale vertex buffer");
      return false;
    }
    std::uint32_t count = static_cast<std::uint32_t>(vertexCount);
    if ((vertexCount <= 0) && (record.vertexStride > 0)) {
      count = static_cast<std::uint32_t>(vertex->sizeBytes /
                                         record.vertexStride);
    }
    bgfx::setVertexBuffer(0U, vertex->vertex,
                          static_cast<std::uint32_t>(firstVertex), count,
                          record.layout);
  }
  if (record.indexBuffer != 0U) {
    BgfxBufferRecord *index = ctx.buffers.resolve(record.indexBuffer);
    if ((index == nullptr) || !bgfx::isValid(index->index)) {
      drop_operation("draw: stale index buffer");
      return false;
    }
  }
  return true;
}

void bgfx_draw(DeviceGeometryHandle geometry, PrimitiveTopology topology,
               std::int32_t firstVertex, std::int32_t vertexCount) noexcept {
  BgfxDeviceContext &ctx = device_context();
  BgfxGeometryRecord *record = ctx.geometries.resolve(geometry.value);
  if (record == nullptr) {
    drop_operation("draw: stale geometry");
    return;
  }
  if (!bgfx_apply_geometry(*record, firstVertex, vertexCount)) {
    return;
  }
  bgfx::setState(bgfx_state_bits(ctx.currentState, topology));
  drop_operation("draw: no program bound (pending #138 Phase C)");
  bgfx::discard();
}

void bgfx_draw_indexed(DeviceGeometryHandle geometry,
                       std::int32_t indexCount) noexcept {
  BgfxDeviceContext &ctx = device_context();
  BgfxGeometryRecord *record = ctx.geometries.resolve(geometry.value);
  if ((record == nullptr) || (record->indexBuffer == 0U)) {
    drop_operation("draw_indexed: stale or index-less geometry");
    return;
  }
  if (!bgfx_apply_geometry(*record, 0, 0)) {
    return;
  }
  BgfxBufferRecord *index = ctx.buffers.resolve(record->indexBuffer);
  bgfx::setIndexBuffer(index->index, 0U,
                       static_cast<std::uint32_t>(indexCount));
  bgfx::setState(
      bgfx_state_bits(ctx.currentState, PrimitiveTopology::Triangles));
  drop_operation("draw_indexed: no program bound (pending #138 Phase C)");
  bgfx::discard();
}

void bgfx_draw_indexed_instanced(DeviceGeometryHandle geometry,
                                 std::int32_t indexCount,
                                 std::int32_t instanceCount) noexcept {
  BgfxDeviceContext &ctx = device_context();
  BgfxGeometryRecord *record = ctx.geometries.resolve(geometry.value);
  if ((record == nullptr) || (record->indexBuffer == 0U) ||
      (record->instanceBuffer == 0U)) {
    drop_operation("draw_indexed_instanced: geometry missing streams");
    return;
  }
  if (!bgfx_apply_geometry(*record, 0, 0)) {
    return;
  }
  BgfxBufferRecord *stream = ctx.buffers.resolve(record->instanceBuffer);
  if ((stream == nullptr) || !bgfx::isValid(stream->vertex)) {
    drop_operation("draw_indexed_instanced: stale instance buffer");
    return;
  }
  BgfxBufferRecord *index = ctx.buffers.resolve(record->indexBuffer);
  bgfx::setIndexBuffer(index->index, 0U,
                       static_cast<std::uint32_t>(indexCount));
  bgfx::setInstanceDataBuffer(stream->vertex, 0U,
                              static_cast<std::uint32_t>(instanceCount));
  bgfx::setState(
      bgfx_state_bits(ctx.currentState, PrimitiveTopology::Triangles));
  drop_operation(
      "draw_indexed_instanced: no program bound (pending #138 Phase C)");
  bgfx::discard();
}

// --- Render targets, state, views ---

RenderTargetHandle
bgfx_create_render_target(const RenderTargetDesc &desc) noexcept {
  BgfxDeviceContext &ctx = device_context();
  if ((desc.colorCount > kMaxColorAttachments) ||
      ((desc.colorCount == 0U) && (desc.depth.texture.value == 0U))) {
    drop_operation("create_render_target: invalid attachment set");
    return RenderTargetHandle{};
  }
  bgfx::Attachment attachments[kMaxColorAttachments + 1U] = {};
  std::uint8_t count = 0U;
  BgfxTargetRecord record{};

  const auto append = [&](const RenderTargetAttachment &attachment,
                          const char *what) noexcept -> bool {
    BgfxTextureRecord *texture =
        ctx.textures.resolve(attachment.texture.value);
    if ((texture == nullptr) || !texture->renderTarget) {
      drop_operation(what);
      return false;
    }
    const bool wantsFace = (texture->kind == TextureKind::Cube);
    if (wantsFace == (attachment.face == CubeFace::None)) {
      drop_operation(what);
      return false;
    }
    const std::uint16_t layer =
        wantsFace ? static_cast<std::uint16_t>(attachment.face) : 0U;
    attachments[count].init(texture->handle, bgfx::Access::Write, layer, 1U,
                            static_cast<std::uint16_t>(attachment.mipLevel),
                            BGFX_RESOLVE_NONE);
    ++count;
    return true;
  };

  for (std::size_t i = 0U; i < desc.colorCount; ++i) {
    if (!append(desc.colors[i],
                "create_render_target: invalid color attachment")) {
      return RenderTargetHandle{};
    }
  }
  if (desc.depth.texture.value != 0U) {
    if (!append(desc.depth,
                "create_render_target: invalid depth attachment")) {
      return RenderTargetHandle{};
    }
    record.depthTexture = desc.depth.texture.value;
  }

  record.handle = bgfx::createFrameBuffer(count, attachments, false);
  if (!bgfx::isValid(record.handle)) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "bgfx backend: frame buffer creation failed");
    return RenderTargetHandle{};
  }
  const std::uint32_t value = ctx.targets.allocate(record);
  if (value == 0U) {
    bgfx::destroy(record.handle);
    drop_operation("create_render_target: table full");
    return RenderTargetHandle{};
  }
  return RenderTargetHandle{value};
}

void bgfx_destroy_render_target(RenderTargetHandle target) noexcept {
  if (target.value == 0U) {
    return;
  }
  BgfxTargetRecord *record = device_context().targets.resolve(target.value);
  if (record == nullptr) {
    return; // idempotent destroy
  }
  bgfx::destroy(record->handle);
  device_context().targets.release(target.value);
}

void bgfx_bind_render_target(RenderTargetHandle target) noexcept {
  BgfxDeviceContext &ctx = device_context();
  bgfx::FrameBufferHandle frameBuffer = BGFX_INVALID_HANDLE;
  if (target.value != 0U) {
    BgfxTargetRecord *record = ctx.targets.resolve(target.value);
    if (record == nullptr) {
      drop_operation("bind_render_target: stale target");
      return;
    }
    frameBuffer = record->handle;
  }
  // Each bind claims a fresh sequential view so pass order matches
  // submission order (the command buffer flushes passes sequentially).
  if (ctx.viewsUsed >= 255U) {
    drop_operation("bind_render_target: per-frame view budget exhausted");
    return;
  }
  const std::uint16_t view = ctx.viewsUsed;
  ++ctx.viewsUsed;
  bgfx::setViewFrameBuffer(view, frameBuffer);
  bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
  ctx.currentView = view;
}

void bgfx_copy_depth(RenderTargetHandle source, RenderTargetHandle destination,
                     std::int32_t width, std::int32_t height) noexcept {
  BgfxDeviceContext &ctx = device_context();
  BgfxTargetRecord *src = ctx.targets.resolve(source.value);
  BgfxTargetRecord *dst = ctx.targets.resolve(destination.value);
  if ((src == nullptr) || (dst == nullptr) || (src->depthTexture == 0U) ||
      (dst->depthTexture == 0U) || (width <= 0) || (height <= 0)) {
    drop_operation("copy_depth: stale target or missing depth attachment");
    return;
  }
  BgfxTextureRecord *srcTexture = ctx.textures.resolve(src->depthTexture);
  BgfxTextureRecord *dstTexture = ctx.textures.resolve(dst->depthTexture);
  if ((srcTexture == nullptr) || (dstTexture == nullptr)) {
    drop_operation("copy_depth: stale depth texture");
    return;
  }
  bgfx::blit(ctx.currentView, dstTexture->handle, 0U, 0U, srcTexture->handle,
             0U, 0U, static_cast<std::uint16_t>(width),
             static_cast<std::uint16_t>(height));
}

void bgfx_apply_render_state(const RenderState &state) noexcept {
  // Whole state is captured and applied per submit (bgfx's model); the
  // per-pass reset the contract promises falls out of that.
  device_context().currentState = state;
}

void bgfx_set_viewport(std::int32_t x, std::int32_t y, std::int32_t w,
                       std::int32_t h) noexcept {
  BgfxDeviceContext &ctx = device_context();
  if ((w < 0) || (h < 0)) {
    drop_operation("set_viewport: negative extent");
    return;
  }
  // GL's viewport origin is bottom-left, bgfx's is top-left; the flip
  // becomes observable (and is resolved) when Phase D ports the passes.
  bgfx::setViewRect(ctx.currentView, static_cast<std::uint16_t>(x),
                    static_cast<std::uint16_t>(y),
                    static_cast<std::uint16_t>(w),
                    static_cast<std::uint16_t>(h));
}

void bgfx_clear(ClearFlags flags, float r, float g, float b,
                float a) noexcept {
  BgfxDeviceContext &ctx = device_context();
  bgfx::setViewClear(ctx.currentView, bgfx_clear_flags(flags),
                     bgfx_clear_rgba(r, g, b, a), 1.0f, 0U);
  bgfx::touch(ctx.currentView);
}

// --- Timestamp queries (caps.timestampQueries is false) ---

DeviceQueryHandle bgfx_create_timestamp_query() noexcept {
  return kInvalidDeviceQuery;
}
void bgfx_destroy_timestamp_query(DeviceQueryHandle) noexcept {}
void bgfx_write_timestamp(DeviceQueryHandle) noexcept {}
bool bgfx_timestamp_ready(DeviceQueryHandle) noexcept { return false; }
std::uint64_t bgfx_timestamp_value(DeviceQueryHandle) noexcept { return 0U; }

// --- Diagnostics ---

std::uint64_t bgfx_native_texture_id(DeviceTextureHandle texture) noexcept {
  BgfxTextureRecord *record =
      device_context().textures.resolve(texture.value);
  // The bgfx handle index is what a bgfx-backed UI renderer consumes
  // (the ImGui bgfx backend arrives with Phase D).
  return (record != nullptr) ? static_cast<std::uint64_t>(record->handle.idx)
                             : 0U;
}

DeviceDebugStats bgfx_debug_stats() noexcept {
  return device_context().stats;
}

/// Fills the device table with the bgfx backend entries and its honest
/// capability set: instancing works, uniform blocks and timestamp
/// queries do not exist in bgfx's model.
void fill_bgfx_render_device(RenderDevice *device) noexcept {
  *device = RenderDevice{};
  device->caps.instancing = true;
  device->caps.uniformBlocks = false;
  device->caps.timestampQueries = false;

  device->create_buffer = &bgfx_create_buffer;
  device->update_buffer = &bgfx_update_buffer;
  device->update_buffer_range = &bgfx_update_buffer_range;
  device->destroy_buffer = &bgfx_destroy_buffer;
  device->bind_uniform_buffer_slot = &bgfx_bind_uniform_buffer_slot;
  device->create_texture = &bgfx_create_texture;
  device->update_texture = &bgfx_update_texture;
  device->destroy_texture = &bgfx_destroy_texture;
  device->bind_texture_slot = &bgfx_bind_texture_slot;
  device->create_program = &bgfx_create_program;
  device->destroy_program = &bgfx_destroy_program;
  device->bind_program = &bgfx_bind_program;
  device->shader_param = &bgfx_shader_param;
  device->set_param_mat4 = &bgfx_set_param_mat4;
  device->set_param_mat3 = &bgfx_set_param_mat3;
  device->set_param_f32 = &bgfx_set_param_f32;
  device->set_param_i32 = &bgfx_set_param_i32;
  device->set_param_vec2 = &bgfx_set_param_vec2;
  device->set_param_vec3 = &bgfx_set_param_vec3;
  device->set_param_vec4 = &bgfx_set_param_vec4;
  device->bind_program_uniform_block = &bgfx_bind_program_uniform_block;
  device->create_geometry = &bgfx_create_geometry;
  device->destroy_geometry = &bgfx_destroy_geometry;
  device->set_geometry_instance_stream = &bgfx_set_geometry_instance_stream;
  device->draw = &bgfx_draw;
  device->draw_indexed = &bgfx_draw_indexed;
  device->draw_indexed_instanced = &bgfx_draw_indexed_instanced;
  device->create_render_target = &bgfx_create_render_target;
  device->destroy_render_target = &bgfx_destroy_render_target;
  device->bind_render_target = &bgfx_bind_render_target;
  device->copy_depth = &bgfx_copy_depth;
  device->apply_render_state = &bgfx_apply_render_state;
  device->set_viewport = &bgfx_set_viewport;
  device->clear = &bgfx_clear;
  device->create_timestamp_query = &bgfx_create_timestamp_query;
  device->destroy_timestamp_query = &bgfx_destroy_timestamp_query;
  device->write_timestamp = &bgfx_write_timestamp;
  device->timestamp_ready = &bgfx_timestamp_ready;
  device->timestamp_value = &bgfx_timestamp_value;
  device->native_texture_id = &bgfx_native_texture_id;
  device->debug_stats = &bgfx_debug_stats;
}

} // namespace

/// Initializes the owning system for render device.
bool initialize_render_device() noexcept {
  BgfxDeviceContext &ctx = device_context();
  if (ctx.initialized) {
    return true;
  }

  // #196 parity: the null backend stays selectable so headless pipeline
  // tests behave identically on either compiled backend.
  if (core::cvar_get_bool("r_null_device", false)) {
    fill_null_render_device(&ctx.device);
    ctx.mode = BgfxBackendMode::Null;
    ctx.initialized = true;
    core::log_message(core::LogLevel::Info, "renderer",
                      "render device: null backend (r_null_device)");
    return true;
  }

  // Calling renderFrame before init keeps bgfx single-threaded, matching
  // the engine's main-thread flush model. The Noop renderer is the only
  // one reachable before Phase D wires the platform window into
  // bgfx::PlatformData; real renderer selection lands there.
  bgfx::renderFrame();
  static BgfxCallback callback{};
  bgfx::Init init{};
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 1U;
  init.resolution.height = 1U;
  init.resolution.reset = BGFX_RESET_NONE;
  init.callback = &callback;
  if (!bgfx::init(init)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "bgfx initialization failed");
    return false;
  }
  fill_bgfx_render_device(&ctx.device);
  reset_views();
  ctx.mode = BgfxBackendMode::Bgfx;
  ctx.initialized = true;
  core::log_message(core::LogLevel::Info, "renderer",
                    "render device: bgfx backend (Noop renderer, #138 "
                    "Phase B)");
  return true;
}

/// Shuts down the owning system for render device.
void shutdown_render_device() noexcept {
  BgfxDeviceContext &ctx = device_context();
  // Invalidate every outstanding handle; owning systems destroy their
  // device resources before this point (shutdown_renderer ordering) and
  // bgfx::shutdown reclaims anything that slipped through.
  ctx.buffers.clear();
  ctx.textures.clear();
  ctx.geometries.clear();
  ctx.targets.clear();
  ctx.stats = DeviceDebugStats{};
  ctx.currentState = RenderState{};
  ctx.currentView = 0U;
  ctx.viewsUsed = 0U;
  for (std::uint32_t &slot : ctx.boundTextures) {
    slot = 0U;
  }
  ctx.device = RenderDevice{};
  if (ctx.mode == BgfxBackendMode::Bgfx) {
    bgfx::shutdown();
  }
  ctx.mode = BgfxBackendMode::None;
  ctx.initialized = false;
}

const RenderDevice *render_device() noexcept {
  if (!device_context().initialized) {
    return nullptr;
  }
  return &device_context().device;
}

void render_device_bgfx_frame() noexcept {
  BgfxDeviceContext &ctx = device_context();
  if (!ctx.initialized || (ctx.mode != BgfxBackendMode::Bgfx)) {
    return;
  }
  bgfx::frame();
  reset_views();
}

} // namespace engine::renderer
