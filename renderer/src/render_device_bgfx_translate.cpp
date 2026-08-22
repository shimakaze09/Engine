// Implements the pure engine-to-bgfx descriptor translation declared in
// render_device_bgfx_internal.h (#138 Phase B): render-state bits,
// texture format/staging shapes, sampler and clear flags, and vertex
// semantic/layout mapping. Kept free of device state so the unit suite
// pins the mapping without initializing bgfx.

#include "render_device_bgfx_internal.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace engine::renderer::bgfx_detail {

BgfxTexelUpload bgfx_texel_upload(TextureFormat format,
                                  TexelData texel) noexcept {
  BgfxTexelUpload out{};
  const bool isU8 = (texel == TexelData::U8);
  switch (format) {
  case TextureFormat::R8:
    out = {isU8, bgfx::TextureFormat::R8, 1, 1, 1, TexelStagingOp::Copy};
    break;
  case TextureFormat::RG8:
    out = {isU8, bgfx::TextureFormat::RG8, 2, 2, 2, TexelStagingOp::Copy};
    break;
  case TextureFormat::RGB8:
    out = {isU8, bgfx::TextureFormat::RGB8, 3, 3, 3, TexelStagingOp::Copy};
    break;
  case TextureFormat::RGBA8:
    out = {isU8, bgfx::TextureFormat::RGBA8, 4, 4, 4, TexelStagingOp::Copy};
    break;
  case TextureFormat::R16F:
    out = {!isU8, bgfx::TextureFormat::R16F, 4, 2, 1,
           TexelStagingOp::PackHalf};
    break;
  case TextureFormat::RG16F:
    out = {!isU8, bgfx::TextureFormat::RG16F, 8, 4, 2,
           TexelStagingOp::PackHalf};
    break;
  case TextureFormat::RGB16F:
    // bgfx has no RGB16F; texels widen to RGBA16F with alpha 1.0.
    out = {!isU8, bgfx::TextureFormat::RGBA16F, 12, 8, 3,
           TexelStagingOp::WidenPackHalf};
    break;
  case TextureFormat::RGBA16F:
    out = {!isU8, bgfx::TextureFormat::RGBA16F, 16, 8, 4,
           TexelStagingOp::PackHalf};
    break;
  case TextureFormat::R32F:
    out = {!isU8, bgfx::TextureFormat::R32F, 4, 4, 1, TexelStagingOp::Copy};
    break;
  case TextureFormat::Depth24:
    out = {true, bgfx::TextureFormat::D24, 0, 0, 0, TexelStagingOp::Copy};
    break;
  }
  return out;
}

std::uint64_t bgfx_state_bits(const RenderState &state,
                              PrimitiveTopology topology) noexcept {
  std::uint64_t bits = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
  if (state.depthWrite) {
    bits |= BGFX_STATE_WRITE_Z;
  }
  switch (state.depthTest) {
  case DepthTest::Disabled:
    break;
  case DepthTest::Less:
    bits |= BGFX_STATE_DEPTH_TEST_LESS;
    break;
  case DepthTest::LessEqual:
    bits |= BGFX_STATE_DEPTH_TEST_LEQUAL;
    break;
  }
  if (state.blend == BlendMode::Alpha) {
    bits |= BGFX_STATE_BLEND_ALPHA;
  }
  if (state.cull == CullMode::Back) {
    // Engine meshes wind counter-clockwise front faces (GL default), so
    // culling back faces means culling clockwise triangles.
    bits |= BGFX_STATE_CULL_CW;
  }
  if (topology == PrimitiveTopology::Lines) {
    bits |= BGFX_STATE_PT_LINES;
  }
  return bits;
}

std::uint64_t bgfx_sampler_flags(TextureFilter filter,
                                 TextureWrap wrap) noexcept {
  std::uint64_t flags = 0U;
  if (filter == TextureFilter::Nearest) {
    flags |= BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
             BGFX_SAMPLER_MIP_POINT;
  }
  if (wrap == TextureWrap::ClampEdge) {
    flags |= BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
             BGFX_SAMPLER_W_CLAMP;
  }
  return flags;
}

std::uint16_t bgfx_clear_flags(ClearFlags flags) noexcept {
  std::uint16_t out = BGFX_CLEAR_NONE;
  const std::uint8_t bits = static_cast<std::uint8_t>(flags);
  if ((bits & static_cast<std::uint8_t>(ClearFlags::Color)) != 0U) {
    out |= BGFX_CLEAR_COLOR;
  }
  if ((bits & static_cast<std::uint8_t>(ClearFlags::Depth)) != 0U) {
    out |= BGFX_CLEAR_DEPTH;
  }
  return out;
}

std::uint32_t bgfx_clear_rgba(float r, float g, float b, float a) noexcept {
  const auto channel = [](float value) noexcept -> std::uint32_t {
    const float clamped = (value < 0.0f) ? 0.0f : (value > 1.0f ? 1.0f : value);
    return static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
  };
  return (channel(r) << 24U) | (channel(g) << 16U) | (channel(b) << 8U) |
         channel(a);
}

bool bgfx_vertex_attrib(VertexSemantic semantic,
                        bgfx::Attrib::Enum *out) noexcept {
  switch (semantic) {
  case VertexSemantic::Position:
    *out = bgfx::Attrib::Position;
    return true;
  case VertexSemantic::Normal:
    *out = bgfx::Attrib::Normal;
    return true;
  case VertexSemantic::TexCoord0:
    *out = bgfx::Attrib::TexCoord0;
    return true;
  case VertexSemantic::Joints:
    *out = bgfx::Attrib::Indices;
    return true;
  case VertexSemantic::Weights:
    *out = bgfx::Attrib::Weight;
    return true;
  case VertexSemantic::Color:
    *out = bgfx::Attrib::Color0;
    return true;
  case VertexSemantic::InstanceModel0:
  case VertexSemantic::InstanceModel1:
  case VertexSemantic::InstanceModel2:
  case VertexSemantic::InstanceModel3:
  case VertexSemantic::InstanceParams:
    return false;
  }
  return false;
}

/// Emits skip() padding in uint8-sized steps (bgfx's skip argument).
void skip_bytes(bgfx::VertexLayout *out, std::int32_t bytes) noexcept {
  while (bytes > 0) {
    const std::int32_t step = (bytes > 255) ? 255 : bytes;
    out->skip(static_cast<std::uint8_t>(step));
    bytes -= step;
  }
}

bool bgfx_vertex_layout(const VertexLayout &layout,
                        bgfx::VertexLayout *out) noexcept {
  if ((layout.attributeCount == 0U) ||
      (layout.attributeCount > kMaxVertexAttributes) ||
      (layout.strideBytes <= 0)) {
    return false;
  }
  // Locally clamped copy of the (already validated) count, sorted by an
  // insertion sort: at most kMaxVertexAttributes entries, and GCC 16's
  // -Werror=array-bounds false-positives on std::sort's inlined
  // introsort over the fixed-size array regardless of the range check
  // above (the shader-define sort in shader_system uses the same
  // pattern for the same reason).
  const std::size_t attributeCount = std::min<std::size_t>(
      layout.attributeCount, kMaxVertexAttributes);
  std::array<std::size_t, kMaxVertexAttributes> order{};
  for (std::size_t i = 0U; i < attributeCount; ++i) {
    order[i] = i;
  }
  for (std::size_t i = 1U; i < attributeCount; ++i) {
    const std::size_t current = order[i];
    std::size_t j = i;
    while ((j > 0U) && (layout.attributes[current].offsetBytes <
                        layout.attributes[order[j - 1U]].offsetBytes)) {
      order[j] = order[j - 1U];
      --j;
    }
    order[j] = current;
  }

  out->begin(bgfx::RendererType::Noop);
  std::int32_t cursor = 0;
  for (std::size_t i = 0U; i < attributeCount; ++i) {
    const VertexAttribute &attribute = layout.attributes[order[i]];
    bgfx::Attrib::Enum attrib = bgfx::Attrib::Count;
    if (!bgfx_vertex_attrib(attribute.semantic, &attrib) ||
        (attribute.componentCount < 1) || (attribute.componentCount > 4) ||
        (attribute.offsetBytes < cursor)) {
      return false;
    }
    if (attribute.offsetBytes > cursor) {
      skip_bytes(out, attribute.offsetBytes - cursor);
      cursor = attribute.offsetBytes;
    }
    out->add(attrib, static_cast<std::uint8_t>(attribute.componentCount),
             bgfx::AttribType::Float);
    cursor += attribute.componentCount * 4;
  }
  if (cursor > layout.strideBytes) {
    return false;
  }
  if (cursor < layout.strideBytes) {
    skip_bytes(out, layout.strideBytes - cursor);
  }
  out->end();
  return true;
}

void bgfx_stride_layout(std::int32_t strideBytes,
                        bgfx::VertexLayout *out) noexcept {
  out->begin(bgfx::RendererType::Noop);
  skip_bytes(out, strideBytes);
  out->end();
}

} // namespace engine::renderer::bgfx_detail
