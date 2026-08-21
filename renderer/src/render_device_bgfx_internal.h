// Pure engine-to-bgfx descriptor translation for the bgfx render device
// backend (#138 Phase B): render-state bits, texture formats and texel
// staging shapes, sampler and clear flags, and vertex semantic/layout
// mapping. Everything here is side-effect free so unit tests can pin the
// mapping without initializing bgfx; the stateful backend lives in
// render_device_bgfx.cpp.

#pragma once

#include "engine/renderer/render_device.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 3)
#endif
#include <bgfx/bgfx.h>
#include <bgfx/defines.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cstdint>

namespace engine::renderer::bgfx_detail {

/// How a CPU upload reaches a bgfx texture: byte-identical copy, F32
/// components packed to half floats, or RGB-to-RGBA widening (alpha 1.0)
/// combined with half packing for the one engine format bgfx lacks.
enum class TexelStagingOp : std::uint8_t {
  Copy = 0,
  PackHalf = 1,
  WidenPackHalf = 2,
};

/// Resolved upload shape for one engine texture format: destination bgfx
/// format, per-pixel byte sizes on both sides, component count, and the
/// staging operation. valid is false when the format/texel-data pairing
/// violates the TextureDesc encoding contract.
struct BgfxTexelUpload final {
  bool valid = false;
  bgfx::TextureFormat::Enum format = bgfx::TextureFormat::Count;
  std::int32_t srcBytesPerPixel = 0;
  std::int32_t dstBytesPerPixel = 0;
  std::int32_t components = 0;
  TexelStagingOp op = TexelStagingOp::Copy;
};

/// Maps an engine texture format + texel encoding to its bgfx upload
/// shape. RGB16F widens to RGBA16F (bgfx has no RGB16F); float formats
/// pack the engine's F32 client data to the half layout bgfx stores.
/// Depth24 maps to D24 and never accepts CPU texels.
BgfxTexelUpload bgfx_texel_upload(TextureFormat format,
                                  TexelData texel) noexcept;

/// Translates whole-state application plus draw topology to BGFX_STATE
/// bits. Color writes stay enabled (GL colorMask default); cull Back maps
/// to CULL_CW because engine meshes wind counter-clockwise front faces.
std::uint64_t bgfx_state_bits(const RenderState &state,
                              PrimitiveTopology topology) noexcept;

/// Translates filter/wrap intent to BGFX_SAMPLER flags (0 is bgfx's
/// linear/repeat default).
std::uint64_t bgfx_sampler_flags(TextureFilter filter,
                                 TextureWrap wrap) noexcept;

/// Translates engine clear flags to BGFX_CLEAR bits.
std::uint16_t bgfx_clear_flags(ClearFlags flags) noexcept;

/// Packs a float clear color to bgfx's 0xRRGGBBAA byte encoding.
std::uint32_t bgfx_clear_rgba(float r, float g, float b, float a) noexcept;

/// Maps a mesh vertex semantic to its bgfx attribute; false for the
/// instance semantics, which bind through the instance-data stride
/// rather than named attributes.
bool bgfx_vertex_attrib(VertexSemantic semantic,
                        bgfx::Attrib::Enum *out) noexcept;

/// Builds a bgfx vertex layout from an engine layout: attributes are
/// emitted in offset order with skip() padding for gaps and a trailing
/// skip up to strideBytes. False when the layout is empty, overlapping,
/// exceeds its stride, or names an instance semantic.
bool bgfx_vertex_layout(const VertexLayout &layout,
                        bgfx::VertexLayout *out) noexcept;

/// Builds an attribute-less layout of exactly strideBytes, used to
/// realize raw byte buffers and instance-data streams.
void bgfx_stride_layout(std::int32_t strideBytes,
                        bgfx::VertexLayout *out) noexcept;

} // namespace engine::renderer::bgfx_detail
