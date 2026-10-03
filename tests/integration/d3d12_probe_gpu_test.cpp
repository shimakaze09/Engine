// Diagnostic probe for the Direct3D 12 BRDF lookup reading back as zeros.
// It prints what each variant reads and always passes; it exists only to
// choose between causes, and leaves with the fix.
//
// The renderer's own lookup read twice straight after pipeline frames,
// to tell a wrong first readback from a wrong bake; the bake program drawn
// straight into the back buffer; and test bakes read after pipeline
// frames with their target destroyed or kept.

#include "../gpu_scene_fixture.h"

#include "command_buffer_context.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace {

namespace r = engine::renderer;
using engine::tests::CapturedFrame;

/// Copies `texture` onto the back buffer's top-left, one texel to a pixel,
/// and reads the frame back.
bool read_back(r::DeviceTextureHandle texture, int size, const char *path,
               CapturedFrame *out) noexcept {
  const r::BackendState &backend = r::backend_state();
  const r::RenderDevice *dev = r::render_device();
  std::error_code ec{};
  std::filesystem::remove(path, ec);
  dev->bind_render_target(r::kBackBufferTarget);
  dev->set_viewport(0, 0, size, size);
  // A marker clear: where the copy draws nothing it reads 128/64.
  dev->clear(r::ClearFlags::ColorDepth, 0.5F, 0.25F, 0.75F, 1.0F);
  dev->apply_render_state(r::RenderState{
      r::DepthTest::Disabled, true, r::BlendMode::Disabled, r::CullMode::None});
  dev->bind_program(backend.presentBlitProgram);
  dev->bind_texture_slot(0U, texture);
  if (backend.presentBlitInputLoc.valid()) {
    dev->set_param_i32(backend.presentBlitInputLoc, 0);
  }
  dev->draw(backend.emptyGeometry, r::PrimitiveTopology::Triangles, 0, 3);
  dev->bind_texture_slot(0U, r::kInvalidDeviceTexture);
  dev->bind_program(r::kInvalidDeviceProgram);
  if (!r::render_device_bgfx_request_screenshot(path)) {
    return false;
  }
  for (int frame = 0; frame < 16; ++frame) {
    r::present_render_device();
    if (std::filesystem::exists(path, ec) &&
        engine::tests::load_captured_tga(path, out)) {
      return true;
    }
  }
  return false;
}

/// Prints scale and bias at a grid of texels: columns across NdotV, rows
/// across roughness.
void print_grid(const char *label, const CapturedFrame &frame,
                int size) noexcept {
  const std::uint32_t s = static_cast<std::uint32_t>(size);
  const std::uint32_t points[] = {s / 50U, s / 4U, s / 2U, s - 1U};
  std::printf("probe %s:", label);
  for (const std::uint32_t column : points) {
    for (const std::uint32_t row : points) {
      std::printf(" [%u,%u]=%d/%d", column, row, frame.channel(column, row, 2U),
                  frame.channel(column, row, 1U));
    }
  }
  // One texel in full and one pixel right of the copy, which holds what
  // the back buffer held before the readback's view.
  std::printf(" | texel(1,1) bgra %d %d %d %d | outside %d %d %d\n",
              frame.channel(1U, 1U, 0U), frame.channel(1U, 1U, 1U),
              frame.channel(1U, 1U, 2U), frame.channel(1U, 1U, 3U),
              frame.channel(s + 2U, 2U, 0U), frame.channel(s + 2U, 2U, 1U),
              frame.channel(s + 2U, 2U, 2U));
}

/// Draws the bake program into a fresh target of `format`; the target is
/// destroyed at once when `destroyNow`, otherwise kept until after the
/// readback. With `switchAway` another target is bound and cleared after
/// the draw, so the device leaves the bake target before it goes.
/// The pipeline the probes run under, for a probe that lets frames pass.
engine::EnginePipeline *g_pipeline = nullptr;

void probe_bake(const char *label, int kSize, r::TextureFormat format,
                bool destroyNow, bool switchAway,
                r::TextureFilter filter = r::TextureFilter::Nearest,
                int pipelineFrames = 0) noexcept {
  const r::BackendState &backend = r::backend_state();
  const r::RenderDevice *dev = r::render_device();
  r::TextureDesc desc{};
  desc.kind = r::TextureKind::Tex2D;
  desc.format = format;
  desc.width = kSize;
  desc.height = kSize;
  desc.filter = filter;
  desc.wrap = r::TextureWrap::ClampEdge;
  const r::DeviceTextureHandle texture = dev->create_texture(desc);
  r::RenderTargetDesc targetDesc{};
  targetDesc.colorCount = 1U;
  targetDesc.colors[0].texture = texture;
  const r::RenderTargetHandle target = dev->create_render_target(targetDesc);
  if ((texture == r::kInvalidDeviceTexture) || (target.value == 0U)) {
    std::printf("probe %s: could not create the target\n", label);
    return;
  }
  dev->bind_render_target(target);
  dev->set_viewport(0, 0, kSize, kSize);
  dev->apply_render_state(r::RenderState{
      r::DepthTest::Disabled, true, r::BlendMode::Disabled, r::CullMode::None});
  dev->bind_program(backend.environmentBrdfLutProgram);
  dev->draw(backend.emptyGeometry, r::PrimitiveTopology::Triangles, 0, 3);
  dev->bind_program(r::kInvalidDeviceProgram);
  r::RenderTargetHandle other{};
  r::DeviceTextureHandle otherTexture = r::kInvalidDeviceTexture;
  if (switchAway) {
    r::TextureDesc otherDesc = desc;
    otherDesc.format = r::TextureFormat::RGBA8;
    otherTexture = dev->create_texture(otherDesc);
    r::RenderTargetDesc otherTargetDesc{};
    otherTargetDesc.colorCount = 1U;
    otherTargetDesc.colors[0].texture = otherTexture;
    other = dev->create_render_target(otherTargetDesc);
    dev->bind_render_target(other);
    dev->set_viewport(0, 0, kSize, kSize);
    dev->clear(r::ClearFlags::ColorDepth, 0.0F, 0.0F, 0.0F, 1.0F);
  }
  if (destroyNow) {
    dev->destroy_render_target(target);
  }
  if ((pipelineFrames > 0) && (g_pipeline != nullptr)) {
    static_cast<void>(
        engine::tests::settle_frames(*g_pipeline, pipelineFrames));
  } else {
    r::present_render_device();
    r::present_render_device();
  }
  CapturedFrame frame{};
  char path[128] = {};
  std::snprintf(path, sizeof(path), "d3d12_probe_%s.tga", label);
  if (read_back(texture, kSize, path, &frame)) {
    print_grid(label, frame, kSize);
  } else {
    std::printf("probe %s: no readback\n", label);
  }
  // A second readback in a frame of its own: whether the first one after
  // pipeline frames is what reads wrong.
  if (pipelineFrames > 0) {
    char again[64] = {};
    std::snprintf(again, sizeof(again), "%s_again", label);
    std::snprintf(path, sizeof(path), "d3d12_probe_%s.tga", again);
    if (read_back(texture, kSize, path, &frame)) {
      print_grid(again, frame, kSize);
    }
  }
  if (!destroyNow) {
    dev->destroy_render_target(target);
  }
  if (other.value != 0U) {
    dev->destroy_render_target(other);
  }
  if (otherTexture != r::kInvalidDeviceTexture) {
    dev->destroy_texture(otherTexture);
  }
  dev->destroy_texture(texture);
}

/// Draws the bake program straight into the back buffer's top-left and
/// reads it back, so no render target or copy stands between the shader
/// and the readback.
void probe_bake_direct() noexcept {
  const r::BackendState &backend = r::backend_state();
  const r::RenderDevice *dev = r::render_device();
  constexpr int kSize = 128;
  const char *path = "d3d12_probe_direct.tga";
  std::error_code ec{};
  std::filesystem::remove(path, ec);
  dev->bind_render_target(r::kBackBufferTarget);
  dev->set_viewport(0, 0, kSize, kSize);
  dev->clear(r::ClearFlags::ColorDepth, 0.0F, 0.0F, 0.0F, 1.0F);
  dev->apply_render_state(r::RenderState{
      r::DepthTest::Disabled, true, r::BlendMode::Disabled, r::CullMode::None});
  dev->bind_program(backend.environmentBrdfLutProgram);
  dev->draw(backend.emptyGeometry, r::PrimitiveTopology::Triangles, 0, 3);
  dev->bind_program(r::kInvalidDeviceProgram);
  CapturedFrame frame{};
  bool captured = false;
  if (r::render_device_bgfx_request_screenshot(path)) {
    for (int i = 0; (i < 16) && !captured; ++i) {
      r::present_render_device();
      captured = std::filesystem::exists(path, ec) &&
                 engine::tests::load_captured_tga(path, &frame);
    }
  }
  if (captured) {
    print_grid("direct", frame, kSize);
  } else {
    std::printf("probe direct: no readback\n");
  }
}

int run(engine::EnginePipeline &pipeline, engine::runtime::World &) noexcept {
  if (!engine::tests::settle_frames(pipeline, 2)) {
    return 10;
  }
  g_pipeline = &pipeline;
  const r::BackendState &backend = r::backend_state();
  // The renderer's lookup read four times: straight after pipeline frames,
  // again with only a readback frame between, and the same pair after one
  // more pipeline frame.
  const auto read_shipped = [&backend](const char *label) noexcept {
    CapturedFrame frame{};
    char path[64] = {};
    std::snprintf(path, sizeof(path), "d3d12_probe_%s.tga", label);
    if (read_back(backend.brdfLutTexture, backend.brdfLutSize, path, &frame)) {
      print_grid(label, frame, backend.brdfLutSize);
    } else {
      std::printf("probe %s: no readback\n", label);
    }
  };
  read_shipped("shipped_first");
  read_shipped("shipped_second");
  if (!engine::tests::settle_frames(pipeline, 1)) {
    return 11;
  }
  read_shipped("shipped_after_frame_first");
  read_shipped("shipped_after_frame_second");
  probe_bake_direct();
  probe_bake("rg16f_512_destroyed", 512, r::TextureFormat::RG16F, true, false);
  probe_bake("rg16f_512_after_frames", 512, r::TextureFormat::RG16F, true,
             false, r::TextureFilter::Nearest, 3);
  probe_bake("rg16f_512_kept_after_frames", 512, r::TextureFormat::RG16F, false,
             false, r::TextureFilter::Nearest, 3);
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("d3d12_probe_gpu_test", &run);
}
