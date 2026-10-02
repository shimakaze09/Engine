// Diagnostic probe for the Direct3D 12 BRDF lookup reading back as zeros.
// It prints what each variant reads and always passes; it exists only to
// choose between causes, and leaves with the fix.
//
// The renderer's own lookup; the bake program drawn by this test straight
// into the back buffer and into fresh targets of three formats and two
// sizes, the target destroyed in the frame of its draw (as the renderer
// does) or kept; and the renderer baking its lookup again long after its
// first frame.

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
  dev->clear(r::ClearFlags::ColorDepth, 0.0F, 0.0F, 0.0F, 1.0F);
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
  std::printf("\n");
}

/// Draws the bake program into a fresh target of `format`; the target is
/// destroyed at once when `destroyNow`, otherwise kept until after the
/// readback. With `switchAway` another target is bound and cleared after
/// the draw, so the device leaves the bake target before it goes.
void probe_bake(const char *label, int kSize, r::TextureFormat format,
                bool destroyNow, bool switchAway) noexcept {
  const r::BackendState &backend = r::backend_state();
  const r::RenderDevice *dev = r::render_device();
  r::TextureDesc desc{};
  desc.kind = r::TextureKind::Tex2D;
  desc.format = format;
  desc.width = kSize;
  desc.height = kSize;
  desc.filter = r::TextureFilter::Nearest;
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
  r::present_render_device();
  r::present_render_device();
  CapturedFrame frame{};
  char path[64] = {};
  std::snprintf(path, sizeof(path), "d3d12_probe_%s.tga", label);
  if (read_back(texture, kSize, path, &frame)) {
    print_grid(label, frame, kSize);
  } else {
    std::printf("probe %s: no readback\n", label);
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
  using engine::tests::checked;
  if (!engine::tests::settle_frames(pipeline, 2)) {
    return 10;
  }
  const r::BackendState &backend = r::backend_state();
  CapturedFrame shipped{};
  if (read_back(backend.brdfLutTexture, backend.brdfLutSize,
                "d3d12_probe_shipped.tga", &shipped)) {
    print_grid("shipped", shipped, backend.brdfLutSize);
  }
  probe_bake_direct();
  probe_bake("rg16f_destroyed", 128, r::TextureFormat::RG16F, true, false);
  probe_bake("rg16f_kept", 128, r::TextureFormat::RG16F, false, false);
  probe_bake("rg16f_switched_destroyed", 128, r::TextureFormat::RG16F, true,
             true);
  probe_bake("rgba16f_kept", 128, r::TextureFormat::RGBA16F, false, false);
  probe_bake("rgba8_kept", 128, r::TextureFormat::RGBA8, false, false);

  probe_bake("rg16f_512_destroyed", 512, r::TextureFormat::RG16F, true, false);

  // The renderer's own bake again, in a frame long after the first: a
  // new size makes the next flush bake a new lookup.
  for (const int size : {256, 512}) {
    checked(engine::core::cvar_set_int("r_env_brdf_lut_size", size),
            "r_env_brdf_lut_size");
    CapturedFrame rebaked{};
    char label[32] = {};
    std::snprintf(label, sizeof(label), "rebaked_%d", size);
    char path[64] = {};
    std::snprintf(path, sizeof(path), "d3d12_probe_%s.tga", label);
    if (engine::tests::settle_frames(pipeline, 3) &&
        (backend.brdfLutSize == size) &&
        read_back(backend.brdfLutTexture, size, path, &rebaked)) {
      print_grid(label, rebaked, size);
    } else {
      std::printf("probe %s: no rebake or readback (size %d)\n", label,
                  backend.brdfLutSize);
    }
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("d3d12_probe_gpu_test", &run);
}
