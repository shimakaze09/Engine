// Verifies the bgfx render device backend (#138 Phases B/C) on the Noop
// renderer: initialization/shutdown/re-initialization lifecycle, honest
// capability flags, buffer/texture/geometry/render-target creation with
// stale-handle and dropped-operation behavior, the program-less draw
// contract, cooked-binary program linking with parameter/sampler
// resolution (when the build cooked the proving shaders), and the pure
// engine-to-bgfx translation (state bits, formats, sampler flags,
// vertex layouts) pinned exactly.

#include "engine/renderer/render_device.h"
#include "render_device_bgfx.h"
#include "render_device_bgfx_internal.h"

#include "../test_harness.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/core/vfs.h"
#include "engine/renderer/shader_system.h"

namespace {

using namespace engine::renderer;
using engine::tests::TestContext;

/// Current dropped-operation count from the active device.
std::uint64_t dropped(const RenderDevice *dev) noexcept {
  return dev->debug_stats().droppedOperations;
}

/// Lifecycle: init succeeds headlessly, caps are the bgfx contract,
/// shutdown invalidates, re-init starts clean.
void test_lifecycle(TestContext &t) {
  t.check(render_backend_owns_swapchain(),
          "bgfx backend owns its swapchain");
  present_render_device(); // defined no-op before initialization
  t.check(initialize_render_device(), "initialize succeeds on Noop");
  present_render_device(); // presentation entry drives the frame hook
  t.check(initialize_render_device(), "initialize is idempotent");
  const RenderDevice *dev = render_device();
  t.check(dev != nullptr, "device table published");
  t.check(dev->caps.instancing, "caps: instancing available");
  t.check(!dev->caps.uniformBlocks, "caps: uniform blocks unavailable");
  t.check(!dev->caps.timestampQueries, "caps: timestamp queries unavailable");

  shutdown_render_device();
  t.check(render_device() == nullptr, "shutdown unpublishes the table");
  t.check(initialize_render_device(), "re-initialize after shutdown");
  t.check(render_device() != nullptr, "table republished");
  t.check(dropped(render_device()) == 0U, "re-init starts with zero drops");
}

/// Buffers: create/update/destroy, range overflow, stale handles,
/// uniform-buffer refusal, idempotent destroy.
void test_buffers(TestContext &t) {
  const RenderDevice *dev = render_device();
  const float vertices[12] = {};
  BufferDesc vertexDesc{};
  vertexDesc.usage = BufferUsage::Vertex;
  vertexDesc.sizeBytes = sizeof(vertices);
  vertexDesc.data = vertices;
  const DeviceBufferHandle vertex = dev->create_buffer(vertexDesc);
  t.check(vertex.value != 0U, "vertex buffer created");

  const std::uint32_t indices[3] = {0U, 1U, 2U};
  BufferDesc indexDesc{};
  indexDesc.usage = BufferUsage::Index;
  indexDesc.sizeBytes = sizeof(indices);
  indexDesc.data = indices;
  const DeviceBufferHandle index = dev->create_buffer(indexDesc);
  t.check(index.value != 0U, "index buffer created");

  BufferDesc uniformDesc{};
  uniformDesc.usage = BufferUsage::Uniform;
  uniformDesc.sizeBytes = 64;
  t.check(dev->create_buffer(uniformDesc).value == 0U,
          "uniform buffer creation reports failure (caps false)");

  const std::uint64_t before = dropped(dev);
  dev->update_buffer(vertex, vertices, sizeof(vertices));
  t.check(dropped(dev) == before, "full update accepted");
  dev->update_buffer_range(vertex, vertices, sizeof(vertices) * 2);
  t.check(dropped(dev) == before + 1U,
          "range update beyond allocation dropped");

  dev->destroy_buffer(vertex);
  dev->destroy_buffer(vertex); // idempotent
  const std::uint64_t afterDestroy = dropped(dev);
  dev->update_buffer(vertex, vertices, sizeof(vertices));
  t.check(dropped(dev) == afterDestroy + 1U, "stale buffer update dropped");
  dev->destroy_buffer(index);
}

/// Textures: creation with texels, the RGB16F widening path, partial
/// updates, encoding mismatches, depth rules, the empty-creation
/// render-target signal, and stale binds.
void test_textures(TestContext &t) {
  const RenderDevice *dev = render_device();
  const std::uint8_t pixels[4 * 4 * 4] = {};
  TextureDesc desc{};
  desc.format = TextureFormat::RGBA8;
  desc.width = 4;
  desc.height = 4;
  desc.pixels = pixels;
  const DeviceTextureHandle texture = dev->create_texture(desc);
  t.check(texture.value != 0U, "RGBA8 texture created");

  const std::uint64_t before = dropped(dev);
  dev->update_texture(texture, pixels, 4, 2);
  t.check(dropped(dev) == before, "partial row update accepted");
  dev->update_texture(texture, pixels, 8, 8);
  t.check(dropped(dev) == before + 1U, "oversized update dropped");

  const float hdr[2 * 2 * 3] = {1.0f, 0.5f, 0.25f, 0.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f};
  TextureDesc hdrDesc{};
  hdrDesc.format = TextureFormat::RGB16F;
  hdrDesc.pixelData = TexelData::F32;
  hdrDesc.width = 2;
  hdrDesc.height = 2;
  hdrDesc.pixels = hdr;
  t.check(dev->create_texture(hdrDesc).value != 0U,
          "RGB16F texture widens to RGBA16F");

  TextureDesc mismatch{};
  mismatch.format = TextureFormat::R16F;
  mismatch.pixelData = TexelData::U8;
  mismatch.width = 2;
  mismatch.height = 2;
  mismatch.pixels = pixels;
  t.check(dev->create_texture(mismatch).value == 0U,
          "format/texel encoding mismatch rejected on upload");
  mismatch.pixels = nullptr;
  t.check(dev->create_texture(mismatch).value != 0U,
          "empty creation ignores the texel encoding");

  TextureDesc depthWithPixels{};
  depthWithPixels.format = TextureFormat::Depth24;
  depthWithPixels.width = 4;
  depthWithPixels.height = 4;
  depthWithPixels.pixels = pixels;
  t.check(dev->create_texture(depthWithPixels).value == 0U,
          "depth texture rejects client texels");

  dev->destroy_texture(texture);
  dev->destroy_texture(texture); // idempotent
  const std::uint64_t afterDestroy = dropped(dev);
  dev->bind_texture_slot(0U, texture);
  t.check(dropped(dev) == afterDestroy + 1U, "stale texture bind dropped");
}

/// Render targets: empty-created color+depth attachments compose, a
/// sampled (texel-created) texture is rejected as an attachment, and
/// copy_depth between two depth-carrying targets is accepted.
void test_render_targets(TestContext &t) {
  const RenderDevice *dev = render_device();
  TextureDesc colorDesc{};
  colorDesc.format = TextureFormat::RGBA16F;
  colorDesc.pixelData = TexelData::F32;
  colorDesc.width = 8;
  colorDesc.height = 8;
  const DeviceTextureHandle color = dev->create_texture(colorDesc);
  TextureDesc depthDesc{};
  depthDesc.format = TextureFormat::Depth24;
  depthDesc.width = 8;
  depthDesc.height = 8;
  const DeviceTextureHandle depth = dev->create_texture(depthDesc);
  t.check((color.value != 0U) && (depth.value != 0U),
          "attachment textures created empty");

  RenderTargetDesc targetDesc{};
  targetDesc.colorCount = 1U;
  targetDesc.colors[0].texture = color;
  targetDesc.depth.texture = depth;
  const RenderTargetHandle target = dev->create_render_target(targetDesc);
  t.check(target.value != 0U, "render target created");

  TextureDesc depth2Desc = depthDesc;
  const DeviceTextureHandle depth2 = dev->create_texture(depth2Desc);
  RenderTargetDesc target2Desc{};
  target2Desc.depth.texture = depth2;
  const RenderTargetHandle target2 = dev->create_render_target(target2Desc);
  t.check(target2.value != 0U, "depth-only target created");

  const std::uint64_t before = dropped(dev);
  dev->bind_render_target(target);
  dev->set_viewport(0, 0, 8, 8);
  dev->clear(ClearFlags::ColorDepth, 0.0f, 0.0f, 0.0f, 1.0f);
  dev->copy_depth(target, target2, 8, 8);
  t.check(dropped(dev) == before, "bind/viewport/clear/copy_depth accepted");

  const std::uint8_t texels[4] = {};
  TextureDesc sampledDesc{};
  sampledDesc.format = TextureFormat::RGBA8;
  sampledDesc.width = 1;
  sampledDesc.height = 1;
  sampledDesc.pixels = texels;
  const DeviceTextureHandle sampled = dev->create_texture(sampledDesc);
  RenderTargetDesc badDesc{};
  badDesc.colorCount = 1U;
  badDesc.colors[0].texture = sampled;
  t.check(dev->create_render_target(badDesc).value == 0U,
          "sampled texture rejected as attachment");

  dev->destroy_render_target(target);
  dev->destroy_render_target(target2);
  dev->destroy_texture(color);
  dev->destroy_texture(depth);
  dev->destroy_texture(depth2);
  dev->destroy_texture(sampled);
  render_device_bgfx_frame();
}

/// Programs and draws: creation fails until the Phase C cook, parameter
/// tokens are defined no-ops, and program-less draws drop visibly.
void test_programs_and_draws(TestContext &t) {
  const RenderDevice *dev = render_device();
  t.check(dev->create_program("void main(){}", "void main(){}").value == 0U,
          "program creation fails before the shader cook");
  const ShaderParam param = dev->shader_param(kInvalidDeviceProgram, "u_x");
  t.check(!param.valid(), "shader params resolve invalid");
  dev->set_param_f32(param, 1.0f); // defined no-op
  t.check(!dev->bind_program_uniform_block(kInvalidDeviceProgram, "B", 0U),
          "uniform block binding reports failure");
  t.check(dev->create_timestamp_query().value == 0U,
          "timestamp queries unavailable");

  const float vertices[9] = {};
  BufferDesc vertexDesc{};
  vertexDesc.usage = BufferUsage::Vertex;
  vertexDesc.sizeBytes = sizeof(vertices);
  vertexDesc.data = vertices;
  const DeviceBufferHandle vertex = dev->create_buffer(vertexDesc);
  GeometryDesc geometryDesc{};
  geometryDesc.vertexBuffer = vertex;
  geometryDesc.layout.strideBytes = 12;
  geometryDesc.layout.attributeCount = 1U;
  geometryDesc.layout.attributes[0] = {VertexSemantic::Position, 3, 0};
  const DeviceGeometryHandle geometry = dev->create_geometry(geometryDesc);
  t.check(geometry.value != 0U, "geometry created");

  const std::uint64_t before = dropped(dev);
  dev->draw(geometry, PrimitiveTopology::Triangles, 0, 3);
  t.check(dropped(dev) == before + 1U,
          "program-less draw drops (pending Phase C)");

  dev->destroy_buffer(vertex);
  const std::uint64_t afterDestroy = dropped(dev);
  dev->draw(geometry, PrimitiveTopology::Triangles, 0, 3);
  t.check(dropped(dev) == afterDestroy + 1U, "stale vertex buffer dropped");
  dev->destroy_geometry(geometry);
  render_device_bgfx_frame();
}

#ifdef ENGINE_TEST_COOKED_SHADER_DIR
/// Whole-file byte read; empty on failure.
std::vector<char> read_cooked(const char *name) {
  const std::string path =
      std::string(ENGINE_TEST_COOKED_SHADER_DIR) + "/" + name;
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  std::vector<char> bytes(static_cast<std::size_t>(size > 0 ? size : 0));
  const std::size_t read =
      bytes.empty() ? 0U : std::fread(bytes.data(), 1U, bytes.size(), file);
  std::fclose(file);
  if (read != bytes.size()) {
    bytes.clear();
  }
  return bytes;
}

/// Cooked programs (#138 Phase C): binary linking, parameter and
/// sampler resolution, submitting draws, and malformed-input rejection.
void test_cooked_programs(TestContext &t) {
  const RenderDevice *dev = render_device();
  t.check(dev->caps.cookedPrograms, "caps: cooked programs available");
  t.check(dev->cooked_program_profile != nullptr &&
              (dev->cooked_program_profile()[0] != '\0'),
          "cooked profile tag published");

  const std::vector<char> vsBin =
      read_cooked("debug_line.vert.default.spirv.bin");
  const std::vector<char> fsBin =
      read_cooked("debug_line.frag.default.spirv.bin");
  t.check(!vsBin.empty() && !fsBin.empty(), "cooked proving binaries read");

  const char garbage[16] = "not-a-shader";
  t.check(dev->create_program_binary(
                 garbage, sizeof(garbage), garbage, sizeof(garbage))
                  .value == 0U,
          "malformed binaries rejected");

  const DeviceProgramHandle program = dev->create_program_binary(
      vsBin.data(), static_cast<std::ptrdiff_t>(vsBin.size()), fsBin.data(),
      static_cast<std::ptrdiff_t>(fsBin.size()));
  t.check(program.value != 0U, "cooked program linked");

  const ShaderParam viewProj = dev->shader_param(program, "uViewProjection");
  t.check(viewProj.valid(), "uniform resolved from cooked program");
  t.check(!dev->shader_param(program, "u_absent").valid(),
          "absent uniform resolves invalid");

  dev->bind_program(program);
  const float identity[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                              0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                              0.0f, 0.0f, 0.0f, 1.0f};
  dev->set_param_mat4(viewProj, identity);

  // A draw with a bound cooked program submits instead of dropping.
  const float vertices[21] = {};
  BufferDesc vertexDesc{};
  vertexDesc.usage = BufferUsage::Vertex;
  vertexDesc.sizeBytes = sizeof(vertices);
  vertexDesc.data = vertices;
  const DeviceBufferHandle vertex = dev->create_buffer(vertexDesc);
  GeometryDesc geometryDesc{};
  geometryDesc.vertexBuffer = vertex;
  geometryDesc.layout.strideBytes = 28;
  geometryDesc.layout.attributeCount = 2U;
  geometryDesc.layout.attributes[0] = {VertexSemantic::Position, 3, 0};
  geometryDesc.layout.attributes[1] = {VertexSemantic::Color, 4, 12};
  const DeviceGeometryHandle geometry = dev->create_geometry(geometryDesc);
  const std::uint64_t before = dropped(dev);
  dev->draw(geometry, PrimitiveTopology::Lines, 0, 3);
  t.check(dropped(dev) == before, "draw with cooked program submits");
  render_device_bgfx_frame();

  // Sampler resolution on the tonemap program.
  const std::vector<char> fsVs =
      read_cooked("fullscreen.vert.default.spirv.bin");
  const std::vector<char> toneFs =
      read_cooked("tonemap.frag.default.spirv.bin");
  t.check(!fsVs.empty() && !toneFs.empty(), "tonemap binaries read");
  const DeviceProgramHandle tonemap = dev->create_program_binary(
      fsVs.data(), static_cast<std::ptrdiff_t>(fsVs.size()), toneFs.data(),
      static_cast<std::ptrdiff_t>(toneFs.size()));
  t.check(tonemap.value != 0U, "tonemap program linked");
  const ShaderParam scene = dev->shader_param(tonemap, "u_sceneColor");
  const ShaderParam exposure = dev->shader_param(tonemap, "u_exposure");
  t.check(scene.valid() && exposure.valid(),
          "sampler and vec4 uniforms resolved");
  dev->bind_program(tonemap);
  dev->set_param_i32(scene, 0);
  dev->set_param_f32(exposure, 1.5f);

  dev->destroy_program(tonemap);
  dev->destroy_program(tonemap); // idempotent
  const std::uint64_t afterDestroy = dropped(dev);
  dev->bind_program(tonemap);
  t.check(dropped(dev) == afterDestroy + 1U, "stale program bind dropped");

  dev->bind_program(kInvalidDeviceProgram);
  dev->destroy_program(program);
  dev->destroy_buffer(vertex);
  dev->destroy_geometry(geometry);
  render_device_bgfx_frame();
}

/// Variant stage fallback: SKINNED only recooks the vertex stage, so the
/// production loader must pair the flagged vertex binary with the
/// default fragment cook instead of failing the program (a fresh cook
/// exposed this: stale dev-era fragment-variant files had been masking
/// the miss).
void test_cooked_variant_stage_fallback(TestContext &t) {
  namespace fs = std::filesystem;
  const fs::path tree =
      fs::temp_directory_path() / "engine_bgfx_cooked_vfs";
  std::error_code ec;
  fs::remove_all(tree, ec);
  fs::create_directories(tree / "shaders" / "bgfx" / "cooked", ec);
  t.check(!ec, "cooked VFS tree created");
  const fs::path src{ENGINE_TEST_COOKED_SHADER_DIR};
  bool copied = true;
  for (const char *name :
       {"gbuffer.vert.SKINNED.spirv.bin", "gbuffer.frag.default.spirv.bin"}) {
    fs::copy_file(src / name, tree / "shaders" / "bgfx" / "cooked" / name,
                  fs::copy_options::overwrite_existing, ec);
    copied = copied && !ec;
  }
  t.check(copied, "variant vertex and default fragment binaries staged");

  t.check(engine::core::mount("assets", tree.string().c_str()),
          "cooked tree mounted");
  t.check(initialize_shader_system(), "shader system initialized");

  const ShaderDefine skinned{"SKINNED", "1"};
  ShaderVariantDesc desc{};
  desc.vertPath = "assets/shaders/gbuffer.vert";
  desc.fragPath = "assets/shaders/gbuffer.frag";
  desc.defines = &skinned;
  desc.defineCount = 1U;
  const ShaderProgramHandle variant = load_shader_variant(desc);
  t.check(variant.id != 0U,
          "vertex-only variant links against the default fragment cook");

  shutdown_shader_system();
  static_cast<void>(engine::core::unmount("assets"));
  fs::remove_all(tree, ec);
}

/// PBR_FULL variant: the forward shadow/IBL sampling program must cook,
/// load through the production variant loader, and expose its shadow
/// and IBL uniforms — on a cook without the variant, the stage fallback
/// silently loads the reduced fragment and these names do not resolve.
void test_cooked_pbr_full_variant(TestContext &t) {
  namespace fs = std::filesystem;
  const fs::path tree =
      fs::temp_directory_path() / "engine_bgfx_pbr_full_vfs";
  std::error_code ec;
  fs::remove_all(tree, ec);
  fs::create_directories(tree / "shaders" / "bgfx" / "cooked", ec);
  const fs::path src{ENGINE_TEST_COOKED_SHADER_DIR};
  bool copied = true;
  for (const char *name :
       {"pbr.vert.default.spirv.bin", "pbr.frag.PBR_FULL.spirv.bin"}) {
    fs::copy_file(src / name, tree / "shaders" / "bgfx" / "cooked" / name,
                  fs::copy_options::overwrite_existing, ec);
    copied = copied && !ec;
  }
  t.check(copied, "PBR_FULL cooked binaries staged");

  t.check(engine::core::mount("assets", tree.string().c_str()),
          "PBR_FULL cooked tree mounted");
  t.check(initialize_shader_system(), "shader system initialized (PBR_FULL)");

  const ShaderDefine fullDefine{"PBR_FULL", "1"};
  ShaderVariantDesc desc{};
  desc.vertPath = "assets/shaders/pbr.vert";
  desc.fragPath = "assets/shaders/pbr.frag";
  desc.defines = &fullDefine;
  desc.defineCount = 1U;
  const ShaderProgramHandle full = load_shader_variant(desc);
  t.check(full.id != 0U, "PBR_FULL variant links (default vertex cook)");

  const RenderDevice *dev = render_device();
  const DeviceProgramHandle prog = shader_device_program(full);
  t.check(prog != kInvalidDeviceProgram, "PBR_FULL device program published");
  t.check(dev->shader_param(prog, "uShadowMap0").valid(),
          "cascade shadow sampler resolves on the full variant");
  t.check(dev->shader_param(prog, "uIrradianceMap").valid(),
          "IBL irradiance sampler resolves on the full variant");
  t.check(dev->shader_param(prog, "uShadowMatrix").valid(),
          "cascade matrix array resolves on the full variant");
  t.check(dev->shader_param(prog, "u_viewMatrix").valid(),
          "view matrix resolves on the full variant");

  shutdown_shader_system();
  static_cast<void>(engine::core::unmount("assets"));
  fs::remove_all(tree, ec);
}
#endif // ENGINE_TEST_COOKED_SHADER_DIR

/// Pure translation: exact state bits, format table, sampler flags,
/// clear packing, and vertex layout building incl. gap/overlap rules.
void test_translation(TestContext &t) {
  using namespace engine::renderer::bgfx_detail;

  RenderState state{};
  state.depthTest = DepthTest::Less;
  state.depthWrite = true;
  state.blend = BlendMode::Disabled;
  state.cull = CullMode::Back;
  t.check(bgfx_state_bits(state, PrimitiveTopology::Triangles) ==
              (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
               BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
               BGFX_STATE_CULL_CW),
          "opaque depth-tested state bits exact");
  state.depthTest = DepthTest::LessEqual;
  state.depthWrite = false;
  state.blend = BlendMode::Alpha;
  state.cull = CullMode::None;
  t.check(bgfx_state_bits(state, PrimitiveTopology::Lines) ==
              (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
               BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_ALPHA |
               BGFX_STATE_PT_LINES),
          "transparent line state bits exact");

  const BgfxTexelUpload rgb16f =
      bgfx_texel_upload(TextureFormat::RGB16F, TexelData::F32);
  t.check(rgb16f.valid && (rgb16f.format == bgfx::TextureFormat::RGBA16F) &&
              (rgb16f.op == TexelStagingOp::WidenPackHalf) &&
              (rgb16f.dstBytesPerPixel == 8),
          "RGB16F maps to widened RGBA16F halves");
  const BgfxTexelUpload r32f =
      bgfx_texel_upload(TextureFormat::R32F, TexelData::F32);
  t.check(r32f.valid && (r32f.format == bgfx::TextureFormat::R32F) &&
              (r32f.op == TexelStagingOp::Copy),
          "R32F copies byte-identical");
  t.check(!bgfx_texel_upload(TextureFormat::RGBA8, TexelData::F32).valid,
          "U8 format rejects F32 texels");

  t.check(bgfx_sampler_flags(TextureFilter::Nearest, TextureWrap::Repeat) ==
              (BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
               BGFX_SAMPLER_MIP_POINT),
          "nearest/repeat sampler flags exact");
  t.check(bgfx_sampler_flags(TextureFilter::Linear, TextureWrap::ClampEdge) ==
              (BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
               BGFX_SAMPLER_W_CLAMP),
          "linear/clamp sampler flags exact");

  t.check(bgfx_clear_rgba(1.0f, 0.0f, 0.0f, 1.0f) == 0xFF0000FFU,
          "clear color packs 0xRRGGBBAA");
  t.check(bgfx_clear_flags(ClearFlags::ColorDepth) ==
              (BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH),
          "clear flags translate");

  VertexLayout layout{};
  layout.strideBytes = 36;
  layout.attributeCount = 3U;
  layout.attributes[0] = {VertexSemantic::Position, 3, 0};
  layout.attributes[1] = {VertexSemantic::Normal, 3, 12};
  layout.attributes[2] = {VertexSemantic::TexCoord0, 2, 28}; // 4-byte gap
  bgfx::VertexLayout translated{};
  t.check(bgfx_vertex_layout(layout, &translated), "gapped layout accepted");
  t.check(translated.m_stride == 36U, "translated stride matches");

  VertexLayout overlapping = layout;
  overlapping.attributes[1].offsetBytes = 8;
  t.check(!bgfx_vertex_layout(overlapping, &translated),
          "overlapping layout rejected");
  VertexLayout instanced = layout;
  instanced.attributes[0].semantic = VertexSemantic::InstanceModel0;
  t.check(!bgfx_vertex_layout(instanced, &translated),
          "instance semantics rejected in mesh layouts");
}

} // namespace

int main() {
  TestContext t{};
  test_lifecycle(t);
  test_buffers(t);
  test_textures(t);
  test_render_targets(t);
  test_programs_and_draws(t);
#ifdef ENGINE_TEST_COOKED_SHADER_DIR
  test_cooked_programs(t);
  test_cooked_variant_stage_fallback(t);
  test_cooked_pbr_full_variant(t);
#endif
  test_translation(t);
  shutdown_render_device();
  return t.finish("render_device_bgfx");
}
