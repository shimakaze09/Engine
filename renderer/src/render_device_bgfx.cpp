// Implements the bgfx render device backend (#138): resources, views,
// render state, and draws over the engine RenderDevice table, with the
// same generational slot tables, stale-handle detection, and
// dropped-operation diagnostics as the GL backend (the program and
// shader-parameter path lives in render_device_bgfx_programs.cpp; shared
// records in render_device_bgfx_context.h). bgfx runs single-threaded —
// windowed against the platform's native handles with the renderer from
// r_bgfx_renderer, headless on Noop. Vertex data stages CPU-side until
// its geometry/instance attachment realizes the dynamic buffer at the
// real stride (bgfx binds layouts at buffer creation); buffer/texture
// uploads copy through bgfx transient memory (bgfx's required ownership
// model — that allocation is bgfx-internal and unavoidable here).

#include "render_device_bgfx.h"

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "render_device_bgfx_context.h"
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

namespace bgfx_backend {

BgfxDeviceContext &device_context() noexcept {
  static BgfxDeviceContext context{};
  return context;
}

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

} // namespace bgfx_backend

using namespace bgfx_backend;

namespace {

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
  /// Writes bgfx's readback as an uncompressed TGA: the diagnostic
  /// screenshot path (ENGINE_BGFX_SCREENSHOT) needs zero image-codec
  /// dependencies, and TGA holds BGRA rows natively.
  void screenShot(const char *filePath, std::uint32_t width,
                  std::uint32_t height, std::uint32_t pitch,
                  bgfx::TextureFormat::Enum, const void *data,
                  std::uint32_t, bool yflip) override {
    FILE *file = nullptr;
#ifdef _WIN32
    if (fopen_s(&file, filePath, "wb") != 0) {
      file = nullptr;
    }
#else
    file = std::fopen(filePath, "wb");
#endif
    if (file == nullptr) {
      return;
    }
    std::uint8_t header[18] = {};
    header[2] = 2; // uncompressed true-color
    header[12] = static_cast<std::uint8_t>(width & 0xFFU);
    header[13] = static_cast<std::uint8_t>((width >> 8U) & 0xFFU);
    header[14] = static_cast<std::uint8_t>(height & 0xFFU);
    header[15] = static_cast<std::uint8_t>((height >> 8U) & 0xFFU);
    header[16] = 32;   // BGRA
    header[17] = 0x20; // top-left origin
    std::fwrite(header, 1U, sizeof(header), file);
    const auto *rows = static_cast<const std::uint8_t *>(data);
    for (std::uint32_t y = 0U; y < height; ++y) {
      const std::uint32_t row = yflip ? (height - 1U - y) : y;
      std::fwrite(rows + (static_cast<std::size_t>(row) * pitch), 1U,
                  static_cast<std::size_t>(width) * 4U, file);
    }
    std::fclose(file);
  }
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
  bgfx::setViewClear(0U, BGFX_CLEAR_NONE, 0U, 1.0F, 0U);
  ctx.currentView = 0U;
  ctx.viewsUsed = 1U;
}

// --- Texel staging ---

/// Row stride handed to bgfx for staged texels. stage_texels packs rows
/// tightly, and this sentinel tells every backend to derive the stride
/// from the rect's own width in 32-bit arithmetic. A stride computed here
/// would be a 16-bit value that cannot represent a 65536-byte row (RGBA8
/// at 16384 or RGBA16F at 8192 wide): it wraps to zero and the backend
/// reads every row from the same offset.
constexpr std::uint16_t kTightlyPackedPitch = UINT16_MAX;

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
    // named parameters instead).
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
  // buffers cannot do (a never-updated static fast path is a Phase G
  // measurement). Vertex data stages CPU-side until a geometry or
  // instance-stream attachment supplies the layout bgfx requires at
  // buffer creation.
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
  } else if ((desc.data != nullptr) && (sizeBytes > 0U)) {
    record.staging = std::malloc(sizeBytes);
    if (record.staging == nullptr) {
      core::log_message(core::LogLevel::Error, "render_device",
                        "bgfx backend: vertex staging allocation failed");
      return kInvalidDeviceBuffer;
    }
    std::memcpy(record.staging, desc.data, sizeBytes);
  }
  const std::uint32_t value = device_context().buffers.allocate(record);
  if (value == 0U) {
    if (bgfx::isValid(record.index)) {
      bgfx::destroy(record.index);
    }
    std::free(record.staging);
    drop_operation("create_buffer: table full");
    return kInvalidDeviceBuffer;
  }
  return DeviceBufferHandle{value};
}

/// Realizes an unrealized vertex/instance buffer as a resizable dynamic
/// buffer at the given stride, uploading and releasing any staged data.
/// False when the buffer is already realized at a different stride (one
/// layout per buffer — matching the engine's 1:1 mesh/buffer usage).
/// Realizes a staged vertex buffer as a bgfx dynamic buffer. The engine
/// layout must be passed for shader-consumed streams: bgfx binds vertex
/// attributes from the buffer's creation layout on every backend (the
/// GL family binds nothing for a skip-only layout, and Vulkan's
/// missing-attribute fallback reads position data for every input).
/// Instance streams pass nullptr — bgfx only reads their stride.
bool bgfx_realize_vertex_buffer(BgfxBufferRecord *record,
                                std::int32_t strideBytes,
                                const VertexLayout *engineLayout) noexcept {
  if (bgfx::isValid(record->vertex)) {
    return record->strideBytes == strideBytes;
  }
  if (strideBytes <= 0) {
    return false;
  }
  bgfx::VertexLayout strideLayout{};
  if (engineLayout != nullptr) {
    if (!bgfx_vertex_layout(*engineLayout, &strideLayout)) {
      return false;
    }
  } else {
    bgfx_stride_layout(strideBytes, &strideLayout);
  }
  const std::uint32_t count = static_cast<std::uint32_t>(
      (record->sizeBytes > strideBytes) ? (record->sizeBytes / strideBytes)
                                        : 1);
  record->vertex = bgfx::createDynamicVertexBuffer(
      count, strideLayout, BGFX_BUFFER_ALLOW_RESIZE);
  if (!bgfx::isValid(record->vertex)) {
    return false;
  }
  record->strideBytes = strideBytes;
  if (record->staging != nullptr) {
    bgfx::update(record->vertex, 0U,
                 bgfx::copy(record->staging,
                            static_cast<std::uint32_t>(record->sizeBytes)));
    std::free(record->staging);
    record->staging = nullptr;
  }
  return true;
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
  } else if (record->usage == BufferUsage::Vertex) {
    // Not yet realized: replace the CPU staging copy.
    if ((record->staging == nullptr) ||
        (sizeBytes > static_cast<std::ptrdiff_t>(record->sizeBytes))) {
      std::free(record->staging);
      record->staging = std::malloc(bytes);
    }
    if (record->staging == nullptr) {
      drop_operation("update_buffer: staging allocation failed");
      return;
    }
    std::memcpy(record->staging, data, bytes);
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
  std::free(record->staging);
  record->staging = nullptr;
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
  const bool hasPixels = (desc.kind == TextureKind::Cube)
                             ? (desc.facePixels != nullptr)
                             : (desc.pixels != nullptr);
  const BgfxTexelUpload shape =
      bgfx_texel_upload(desc.format, desc.pixelData);
  // The texel-encoding pairing only constrains actual uploads; empty
  // (render-target) creation just needs the format mapping.
  if ((hasPixels && !shape.valid) ||
      (shape.format == bgfx::TextureFormat::Count) || (desc.width <= 0) ||
      ((desc.kind != TextureKind::Cube) && (desc.height <= 0)) ||
      (desc.mipLevels < 0)) {
    drop_operation("create_texture: invalid descriptor");
    return kInvalidDeviceTexture;
  }
  if (desc.kind == TextureKind::Tex2DArray) {
    // Arrays are layered render targets (the shadow arrays); a client
    // upload has no per-layer shape in the descriptor, so it is
    // rejected rather than silently landing in layer 0.
    if ((desc.layers <= 0) || (desc.layers > 0xFFFF) || hasPixels) {
      drop_operation("create_texture: invalid array descriptor");
      return kInvalidDeviceTexture;
    }
  }
  if (desc.cpuUpdatable && ((desc.kind != TextureKind::Tex2D) || hasPixels)) {
    // cpuUpdatable means "created empty, filled through update_texture":
    // initial pixels would make the bgfx texture immutable and every
    // later update a silent no-op.
    drop_operation("create_texture: invalid cpu-updatable descriptor");
    return kInvalidDeviceTexture;
  }
  // Device dimension cap: an oversized request must soft-fail here (the
  // callers all have a fallback) instead of reaching the backend API —
  // D3D rejects >16384 with E_INVALIDARG and bgfx's own guard lets a
  // single oversized axis through (found on a 4K fullscreen drawable
  // by the one-row-per-tile culling texture, #301 hardware runs).
  {
    const auto maxDim =
        static_cast<std::int32_t>(bgfx::getCaps()->limits.maxTextureSize);
    if ((maxDim > 0) && ((desc.width > maxDim) || (desc.height > maxDim))) {
      drop_operation("create_texture: dimensions exceed device limit");
      return kInvalidDeviceTexture;
    }
  }
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
  record.layers = (desc.kind == TextureKind::Tex2DArray) ? desc.layers : 1;
  // Empty creation ("null for an empty target" per the contract) is the
  // render-target signal — bgfx requires RT/blit intent at creation
  // while the engine descriptor carries none — unless the descriptor
  // opts into the cpu-updatable usage, whose textures are created empty
  // precisely so bgfx keeps them mutable for update_texture.
  record.renderTarget = !hasPixels && !desc.cpuUpdatable;
  record.immutable = hasPixels;
  std::uint64_t flags = bgfx_sampler_flags(desc.filter, desc.wrap);
  if (record.renderTarget) {
    flags |= BGFX_TEXTURE_RT;
    if (desc.format == TextureFormat::Depth24) {
      flags |= BGFX_TEXTURE_BLIT_DST;
    }
  }
  // mipLevels 0 asks for a runtime-generated chain, which bgfx cannot
  // do; the chain is allocated and generation moves to the cook.
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

  if (desc.kind == TextureKind::Cube) {
    record.handle = bgfx::createTextureCube(
        static_cast<std::uint16_t>(desc.width), hasMips, 1U, shape.format,
        flags);
  } else {
    // Tex2D and Tex2DArray share the creation entry point; the layer
    // count is the only difference.
    record.handle = bgfx::createTexture2D(
        static_cast<std::uint16_t>(desc.width),
        static_cast<std::uint16_t>(desc.height), hasMips,
        (desc.kind == TextureKind::Tex2DArray)
            ? static_cast<std::uint16_t>(desc.layers)
            : 1U,
        shape.format, flags);
  }
  if (!bgfx::isValid(record.handle)) {
    core::log_message(core::LogLevel::Error, "render_device",
                      "bgfx backend: texture creation failed");
    return kInvalidDeviceTexture;
  }

  if ((desc.kind == TextureKind::Tex2D) && (desc.pixels != nullptr)) {
    const bgfx::Memory *mem =
        stage_texels(shape, desc.width, desc.height, desc.pixels);
    if (mem != nullptr) {
      bgfx::updateTexture2D(record.handle, 0U, 0U, 0U, 0U,
                            static_cast<std::uint16_t>(desc.width),
                            static_cast<std::uint16_t>(desc.height), mem,
                            kTightlyPackedPitch);
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
                                kTightlyPackedPitch);
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
  if (record->immutable) {
    // bgfx marks mem-created textures immutable and silently discards
    // their updates; dropping here makes the misuse visible — per-frame
    // data must be created cpuUpdatable instead.
    drop_operation("update_texture: immutable (created with pixels)");
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
      static_cast<std::uint16_t>(height), mem, kTightlyPackedPitch);
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
  // bgfx attaches textures per submit through the bound program's
  // sampler uniforms (apply_program_samplers); the slot contents are
  // recorded here.
  ctx.boundTextures[slot] = texture.value;
}

// --- Geometry and draws ---

DeviceGeometryHandle bgfx_create_geometry(const GeometryDesc &desc) noexcept {
  BgfxDeviceContext &ctx = device_context();
  BgfxGeometryRecord record{};
  if (desc.vertexBuffer.value != 0U) {
    BgfxBufferRecord *vertex = ctx.buffers.resolve(desc.vertexBuffer.value);
    if ((vertex == nullptr) || (vertex->usage != BufferUsage::Vertex)) {
      drop_operation("create_geometry: stale or non-vertex buffer");
      return kInvalidDeviceGeometry;
    }
    // Attaching a geometry realizes the buffer at this layout (bgfx
    // binds the layout at buffer creation; per-draw stride overrides
    // are rejected).
    if (!bgfx_realize_vertex_buffer(vertex, desc.layout.strideBytes,
                                    &desc.layout)) {
      drop_operation("create_geometry: vertex buffer realization failed "
                     "(one layout per buffer)");
      return kInvalidDeviceGeometry;
    }
    record.vertexBuffer = desc.vertexBuffer.value;
    record.vertexStride = desc.layout.strideBytes;
  }
  if (desc.indexBuffer.value != 0U) {
    BgfxBufferRecord *index = ctx.buffers.resolve(desc.indexBuffer.value);
    if ((index == nullptr) || !bgfx::isValid(index->index)) {
      drop_operation("create_geometry: stale or non-index buffer");
      return kInvalidDeviceGeometry;
    }
    record.indexBuffer = desc.indexBuffer.value;
  }
  const std::uint32_t value = ctx.geometries.allocate(record);
  if (value == 0U) {
    drop_operation("create_geometry: table full");
    return kInvalidDeviceGeometry;
  }
  return DeviceGeometryHandle{value};
}

void bgfx_destroy_geometry(DeviceGeometryHandle geometry) noexcept {
  if (geometry.value == 0U) {
    return;
  }
  if (device_context().geometries.resolve(geometry.value) == nullptr) {
    return; // idempotent destroy
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
  if ((stream == nullptr) || (stream->usage != BufferUsage::Vertex) ||
      (layout.strideBytes <= 0)) {
    drop_operation("set_geometry_instance_stream: stale buffer or layout");
    return false;
  }
  // Attaching the stream realizes the buffer at the instance stride
  // (bgfx derives per-instance advance from the creation layout).
  if (!bgfx_realize_vertex_buffer(stream, layout.strideBytes, nullptr)) {
    drop_operation("set_geometry_instance_stream: realization failed "
                   "(one layout per buffer)");
    return false;
  }
  record->instanceBuffer = buffer.value;
  return true;
}

/// Applies the geometry's streams for one draw; false (with the drop
/// recorded) when a referenced buffer went stale. Attribute-less
/// geometry binds the backend-owned fullscreen triangle stream.
bool bgfx_apply_geometry(const BgfxGeometryRecord &record,
                         std::int32_t firstVertex,
                         std::int32_t vertexCount) noexcept {
  BgfxDeviceContext &ctx = device_context();
  if (record.vertexBuffer == 0U) {
    if (bgfx::isValid(ctx.fullscreenVertex)) {
      bgfx::setVertexBuffer(0U, ctx.fullscreenVertex, 0U, 3U);
    }
    return true;
  }
  BgfxBufferRecord *vertex = ctx.buffers.resolve(record.vertexBuffer);
  if ((vertex == nullptr) || !bgfx::isValid(vertex->vertex)) {
    drop_operation("draw: stale or unrealized vertex buffer");
    return false;
  }
  std::uint32_t count = static_cast<std::uint32_t>(vertexCount);
  if ((vertexCount <= 0) && (record.vertexStride > 0)) {
    count = static_cast<std::uint32_t>(vertex->sizeBytes /
                                       record.vertexStride);
  }
  bgfx::setVertexBuffer(0U, vertex->vertex,
                        static_cast<std::uint32_t>(firstVertex), count);
  return true;
}

/// Submits the prepared draw with the bound program's samplers applied;
/// no bound program records the drop (programs exist only from the
/// shaderc cook, caps.cookedPrograms).
void bgfx_submit_draw(std::uint64_t stateBits) noexcept {
  BgfxDeviceContext &ctx = device_context();
  BgfxProgramRecord *program = current_program_record();
  if (program == nullptr) {
    drop_operation("draw: no program bound (cooked programs only)");
    bgfx::discard();
    return;
  }
  bgfx::setState(stateBits);
  apply_staged_uniforms();
  apply_program_samplers(*program);
  bgfx::submit(ctx.currentView, program->handle);
}

void bgfx_draw(DeviceGeometryHandle geometry, PrimitiveTopology topology,
               std::int32_t firstVertex, std::int32_t vertexCount) noexcept {
  BgfxDeviceContext &ctx = device_context();
  BgfxGeometryRecord *record = ctx.geometries.resolve(geometry.value);
  if (record == nullptr) {
    drop_operation("draw: stale geometry");
    bgfx::discard();
    return;
  }
  if (!bgfx_apply_geometry(*record, firstVertex, vertexCount)) {
    bgfx::discard();
    return;
  }
  bgfx_submit_draw(bgfx_state_bits(ctx.currentState, topology));
}

void bgfx_draw_indexed(DeviceGeometryHandle geometry,
                       std::int32_t indexCount) noexcept {
  BgfxDeviceContext &ctx = device_context();
  BgfxGeometryRecord *record = ctx.geometries.resolve(geometry.value);
  if ((record == nullptr) || (record->indexBuffer == 0U)) {
    drop_operation("draw_indexed: stale or index-less geometry");
    bgfx::discard();
    return;
  }
  BgfxBufferRecord *index = ctx.buffers.resolve(record->indexBuffer);
  if ((index == nullptr) || !bgfx::isValid(index->index)) {
    drop_operation("draw_indexed: stale index buffer");
    bgfx::discard();
    return;
  }
  if (!bgfx_apply_geometry(*record, 0, 0)) {
    bgfx::discard();
    return;
  }
  bgfx::setIndexBuffer(index->index, 0U,
                       static_cast<std::uint32_t>(indexCount));
  bgfx_submit_draw(
      bgfx_state_bits(ctx.currentState, PrimitiveTopology::Triangles));
}

void bgfx_draw_indexed_instanced(DeviceGeometryHandle geometry,
                                 std::int32_t indexCount,
                                 std::int32_t instanceCount) noexcept {
  BgfxDeviceContext &ctx = device_context();
  BgfxGeometryRecord *record = ctx.geometries.resolve(geometry.value);
  if ((record == nullptr) || (record->indexBuffer == 0U) ||
      (record->instanceBuffer == 0U)) {
    drop_operation("draw_indexed_instanced: geometry missing streams");
    bgfx::discard();
    return;
  }
  BgfxBufferRecord *index = ctx.buffers.resolve(record->indexBuffer);
  BgfxBufferRecord *stream = ctx.buffers.resolve(record->instanceBuffer);
  if ((index == nullptr) || !bgfx::isValid(index->index) ||
      (stream == nullptr) || !bgfx::isValid(stream->vertex)) {
    drop_operation("draw_indexed_instanced: stale buffer");
    bgfx::discard();
    return;
  }
  if (!bgfx_apply_geometry(*record, 0, 0)) {
    bgfx::discard();
    return;
  }
  bgfx::setIndexBuffer(index->index, 0U,
                       static_cast<std::uint32_t>(indexCount));
  bgfx::setInstanceDataBuffer(stream->vertex, 0U,
                              static_cast<std::uint32_t>(instanceCount));
  bgfx_submit_draw(
      bgfx_state_bits(ctx.currentState, PrimitiveTopology::Triangles));
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
    // Layer selects a Tex2DArray slice; cubes address by face and every
    // other kind must leave it 0 so a typo cannot silently alias.
    if ((attachment.layer != 0) &&
        (texture->kind != TextureKind::Tex2DArray)) {
      drop_operation(what);
      return false;
    }
    if ((texture->kind == TextureKind::Tex2DArray) &&
        ((attachment.layer < 0) || (attachment.layer >= texture->layers))) {
      drop_operation(what);
      return false;
    }
    const std::uint16_t layer =
        wantsFace ? static_cast<std::uint16_t>(attachment.face)
                  : static_cast<std::uint16_t>(attachment.layer);
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
  // View state persists per id across frames in bgfx; a pass that never
  // clears would otherwise inherit whatever clear an earlier frame (or
  // startup IBL cook) configured on this id and wipe its own input.
  bgfx::setViewClear(view, BGFX_CLEAR_NONE, 0U, 1.0F, 0U);
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
/// capability set: instancing and cooked programs work; uniform blocks
/// and timestamp queries do not exist in bgfx's model.
void fill_bgfx_render_device(RenderDevice *device) noexcept {
  *device = RenderDevice{};
  device->caps.instancing = true;
  device->caps.uniformBlocks = false;
  device->caps.timestampQueries = false;
  device->caps.cookedPrograms = true;
  // Valid here: this fill runs after bgfx::init. WebGL2 reports its
  // 16-unit floor through this, gating the deferred pass off on web.
  device->caps.maxTextureSamplers = static_cast<std::uint16_t>(
      bgfx::getCaps()->limits.maxTextureSamplers);
  // bgfx reports the live API's conventions: homogeneousDepth means the
  // GL [-1, 1] clip range; the engine's projection builders key off
  // these instead of assuming GL.
  device->caps.depthZeroToOne = !bgfx::getCaps()->homogeneousDepth;
  // Not bgfx's originBottomLeft: that reports the normalized clip
  // convention (true even on Vulkan). Engine render-target row order
  // follows the API family — bottom-up on GL, top-down elsewhere —
  // which is the same split as the depth convention.
  device->caps.textureOriginBottomLeft = bgfx::getCaps()->homogeneousDepth;

  device->create_buffer = &bgfx_create_buffer;
  device->update_buffer = &bgfx_update_buffer;
  device->update_buffer_range = &bgfx_update_buffer_range;
  device->destroy_buffer = &bgfx_destroy_buffer;
  device->bind_uniform_buffer_slot = &bgfx_bind_uniform_buffer_slot;
  device->create_texture = &bgfx_create_texture;
  device->update_texture = &bgfx_update_texture;
  device->destroy_texture = &bgfx_destroy_texture;
  device->bind_texture_slot = &bgfx_bind_texture_slot;
  device->create_program_binary = &bgfx_backend::bgfx_create_program_binary;
  device->create_program_binary_introspected =
      &bgfx_backend::bgfx_create_program_binary_introspected;
  device->cooked_program_profile = &bgfx_backend::bgfx_cooked_program_profile;
  device->destroy_program = &bgfx_backend::bgfx_destroy_program;
  device->bind_program = &bgfx_backend::bgfx_bind_program;
  device->shader_param = &bgfx_backend::bgfx_shader_param;
  device->set_param_mat4 = &bgfx_backend::bgfx_set_param_mat4;
  device->set_param_mat3 = &bgfx_backend::bgfx_set_param_mat3;
  device->set_param_f32 = &bgfx_backend::bgfx_set_param_f32;
  device->set_param_i32 = &bgfx_backend::bgfx_set_param_i32;
  device->set_param_vec2 = &bgfx_backend::bgfx_set_param_vec2;
  device->set_param_vec3 = &bgfx_backend::bgfx_set_param_vec3;
  device->set_param_vec4 = &bgfx_backend::bgfx_set_param_vec4;
  device->set_param_vec4_array = &bgfx_backend::bgfx_set_param_vec4_array;
  device->set_param_mat4_array = &bgfx_backend::bgfx_set_param_mat4_array;
  device->bind_program_uniform_block =
      &bgfx_backend::bgfx_bind_program_uniform_block;
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
  // the engine's main-thread flush model. Emscripten builds compile bgfx
  // single-threaded already and assert on the call.
#if !defined(__EMSCRIPTEN__)
  bgfx::renderFrame();
#endif
  static BgfxCallback callback{};
  bgfx::Init init{};
  init.callback = &callback;
  init.debug = core::cvar_get_bool("r_bgfx_debug", false);

  // Renderer selection: r_bgfx_renderer names the API; without a native
  // window (headless/dummy driver) only Noop is reachable. "auto" lets
  // bgfx pick the platform's best backend.
  void *nativeWindow = core::platform_native_window_handle();
  const char *requested = core::cvar_get_string("r_bgfx_renderer", "auto");
  if (nativeWindow == nullptr) {
    init.type = bgfx::RendererType::Noop;
  } else if (std::strcmp(requested, "noop") == 0) {
    init.type = bgfx::RendererType::Noop;
  } else if (std::strcmp(requested, "vulkan") == 0) {
    init.type = bgfx::RendererType::Vulkan;
  } else if (std::strcmp(requested, "opengl") == 0) {
    init.type = bgfx::RendererType::OpenGL;
  } else if (std::strcmp(requested, "gles") == 0) {
    // The web export's API family, runnable natively for diagnosis.
    init.type = bgfx::RendererType::OpenGLES;
  } else if (std::strcmp(requested, "metal") == 0) {
    init.type = bgfx::RendererType::Metal;
  } else if (std::strcmp(requested, "d3d11") == 0) {
    init.type = bgfx::RendererType::Direct3D11;
  } else if (std::strcmp(requested, "d3d12") == 0) {
    init.type = bgfx::RendererType::Direct3D12;
  } else {
#ifdef _WIN32
    // "auto" picks Vulkan explicitly on Windows: it is the proven
    // backend on the canonical spirv cook. The #301 shadow-array unit
    // map fits DXBC and Windows builds cook the dx11 profile, so the
    // D3D backends are runnable — but they stay explicit d3d11/d3d12
    // opt-ins until verified, an owner call to flip.
    init.type = bgfx::RendererType::Vulkan;
#else
    init.type = bgfx::RendererType::Count; // auto
#endif
  }
  if (nativeWindow != nullptr) {
    init.platformData.nwh = nativeWindow;
    init.platformData.ndt = core::platform_native_display_handle();
    init.platformData.type = core::platform_window_is_wayland()
                                 ? bgfx::NativeWindowHandleType::Wayland
                                 : bgfx::NativeWindowHandleType::Default;
  }

  int width = 0;
  int height = 0;
  core::render_drawable_size(&width, &height);
  ctx.backBufferWidth = (width > 0) ? width : 1;
  ctx.backBufferHeight = (height > 0) ? height : 1;
  ctx.backBufferVsync = (core::cvar_get_int("r_vsync", 1) != 0);
  init.resolution.width = static_cast<std::uint32_t>(ctx.backBufferWidth);
  init.resolution.height = static_cast<std::uint32_t>(ctx.backBufferHeight);
  init.resolution.reset =
      ctx.backBufferVsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
  // One frame of CPU run-ahead: without a bound, Vulkan's FIFO queue
  // lets several short CPU frames pile up before one long block at
  // acquire, which reads as unstable pacing (and >refresh FPS spikes)
  // even when presentation is locked to vsync.
  init.resolution.maxFrameLatency = 1U;
  if (!bgfx::init(init)) {
    core::log_message(core::LogLevel::Error, "renderer",
                      "bgfx initialization failed");
    return false;
  }
  // Backend-owned fullscreen triangle for attribute-less engine draws
  // (the GL path synthesizes it from gl_VertexID; bgfx needs a stream).
  {
    bgfx::VertexLayout layout{};
    layout.begin(bgfx::RendererType::Noop)
        .add(bgfx::Attrib::Position, 3U, bgfx::AttribType::Float)
        .end();
    ctx.fullscreenLayout = bgfx::createVertexLayout(layout);
    ctx.fullscreenVertex =
        bgfx::createDynamicVertexBuffer(3U, layout, BGFX_BUFFER_NONE);
    const float triangle[9] = {-1.0f, -1.0f, 0.0f, 3.0f, -1.0f,
                               0.0f,  -1.0f, 3.0f, 0.0f};
    bgfx::update(ctx.fullscreenVertex, 0U,
                 bgfx::copy(triangle, sizeof(triangle)));
  }
  fill_bgfx_render_device(&ctx.device);
  reset_views();
  ctx.mode = BgfxBackendMode::Bgfx;
  ctx.initialized = true;
  char msg[128] = {};
  std::snprintf(msg, sizeof(msg), "render device: bgfx backend (%s, #138)",
                bgfx::getRendererName(bgfx::getRendererType()));
  core::log_message(core::LogLevel::Info, "renderer", msg);
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
  ctx.programs.clear();
  ctx.geometries.clear();
  ctx.targets.clear();
  ctx.stats = DeviceDebugStats{};
  ctx.currentState = RenderState{};
  ctx.currentProgram = 0U;
  ctx.currentView = 0U;
  ctx.viewsUsed = 0U;
  for (std::uint32_t &slot : ctx.boundTextures) {
    slot = 0U;
  }
  ctx.device = RenderDevice{};
  if (ctx.mode == BgfxBackendMode::Bgfx) {
    // Global uniform handles die with the device, before bgfx::shutdown.
    reset_global_uniforms();
    if (bgfx::isValid(ctx.fullscreenVertex)) {
      bgfx::destroy(ctx.fullscreenVertex);
    }
    if (bgfx::isValid(ctx.fullscreenLayout)) {
      bgfx::destroy(ctx.fullscreenLayout);
    }
    bgfx::shutdown();
  }
  ctx.fullscreenVertex = BGFX_INVALID_HANDLE;
  ctx.fullscreenLayout = BGFX_INVALID_HANDLE;
  // Null-mode runs never create registry entries; this only clears
  // counters (the Bgfx branch above already destroyed live handles).
  ctx.uniformCount = 0U;
  ctx.dirtyUniformCount = 0U;
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
  // Diagnostic capture: with ENGINE_BGFX_SCREENSHOT=<path.tga> in the
  // environment, the presented back buffer is written there every ~2
  // seconds through bgfx's readback — visual verification on hosts
  // whose compositors block external capture, and in CI.
  {
    static const char *screenshotPath =
        core::non_empty_env("ENGINE_BGFX_SCREENSHOT");
    static std::uint32_t frameCounter = 0U;
    if ((screenshotPath != nullptr) && ((frameCounter++ % 120U) == 60U)) {
      bgfx::requestScreenShot(BGFX_INVALID_HANDLE, screenshotPath);
    }
  }
  // Re-reset the swapchain when the drawable or vsync intent changed
  // (r_vsync applies live, matching the GL path's swap-interval cvar).
  int width = 0;
  int height = 0;
  core::render_drawable_size(&width, &height);
  const bool vsync = (core::cvar_get_int("r_vsync", 1) != 0);
  if ((width > 0) && (height > 0) &&
      ((width != ctx.backBufferWidth) || (height != ctx.backBufferHeight) ||
       (vsync != ctx.backBufferVsync))) {
    ctx.backBufferWidth = width;
    ctx.backBufferHeight = height;
    ctx.backBufferVsync = vsync;
    bgfx::reset(static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height),
                vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE);
  }
  bgfx::frame();
  reset_views();
}

bool render_backend_owns_swapchain() noexcept { return true; }

void present_render_device() noexcept { render_device_bgfx_frame(); }

} // namespace engine::renderer
