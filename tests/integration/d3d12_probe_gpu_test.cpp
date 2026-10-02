// Diagnostic probe for the two Direct3D 12 defects the WARP lane pins:
// the BRDF lookup reading back as zeros and the deferred sky ignoring
// its uniforms. It prints what each variant reads and always passes; it
// exists only to choose between causes, and leaves with the fix.
//
// The lookup: the renderer's own bake, then the bake program drawn by
// this test into fresh targets of three formats, with the target either
// destroyed in the frame of its draw (as the renderer does) or kept.
// The sky: the deferred sky at two turbidities under r_debug_probe 0
// (as shipped), 1 (the depth seed in a view of its own) and 2 (no seed).

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
void probe_bake(const char *label, r::TextureFormat format, bool destroyNow,
                bool switchAway) noexcept {
  const r::BackendState &backend = r::backend_state();
  const r::RenderDevice *dev = r::render_device();
  constexpr int kSize = 128;
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

/// Mean RGB over the frame's upper middle.
void print_sky(const char *label, const CapturedFrame &frame) noexcept {
  double sum[3] = {};
  std::uint32_t count = 0U;
  for (std::uint32_t y = frame.height / 8U; y < frame.height / 2U; ++y) {
    for (std::uint32_t x = frame.width / 4U; x < (frame.width * 3U) / 4U; ++x) {
      sum[0] += frame.channel(x, y, 2U);
      sum[1] += frame.channel(x, y, 1U);
      sum[2] += frame.channel(x, y, 0U);
      ++count;
    }
  }
  std::printf("probe %s: sky %.1f %.1f %.1f\n", label, sum[0] / count,
              sum[1] / count, sum[2] / count);
}

int run(engine::EnginePipeline &pipeline,
        engine::runtime::World &world) noexcept {
  using engine::tests::checked;
  static_cast<void>(engine::core::cvar_register_int(
      "r_debug_probe", 0, "Direct3D 12 diagnostic probe"));
  if (!engine::tests::settle_frames(pipeline, 2)) {
    return 10;
  }
  const r::BackendState &backend = r::backend_state();
  CapturedFrame shipped{};
  if (read_back(backend.brdfLutTexture, backend.brdfLutSize,
                "d3d12_probe_shipped.tga", &shipped)) {
    print_grid("shipped", shipped, backend.brdfLutSize);
  }
  probe_bake("rg16f_destroyed", r::TextureFormat::RG16F, true, false);
  probe_bake("rg16f_kept", r::TextureFormat::RG16F, false, false);
  probe_bake("rg16f_switched_destroyed", r::TextureFormat::RG16F, true, true);
  probe_bake("rgba16f_kept", r::TextureFormat::RGBA16F, false, false);
  probe_bake("rgba8_kept", r::TextureFormat::RGBA8, false, false);

  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");
  checked(engine::core::cvar_set_string("r_sky_model", "hosek"), "r_sky_model");
  if (!engine::tests::look_from(world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
                                engine::math::Vec3(0.0F, 10.0F, -4.0F))) {
    return 11;
  }
  for (int probe = 0; probe < 3; ++probe) {
    checked(engine::core::cvar_set_int("r_debug_probe", probe),
            "r_debug_probe");
    for (const float turbidity : {2.0F, 8.0F}) {
      checked(engine::core::cvar_set_float("r_sky_turbidity", turbidity),
              "r_sky_turbidity");
      CapturedFrame frame{};
      char path[64] = {};
      std::snprintf(path, sizeof(path), "d3d12_probe_sky_%d_%d.tga", probe,
                    static_cast<int>(turbidity));
      if (engine::tests::settle_frames(pipeline, 6) &&
          engine::tests::capture_presented_frame(pipeline, path, &frame)) {
        char label[48] = {};
        std::snprintf(label, sizeof(label), "deferred probe %d turbidity %d",
                      probe, static_cast<int>(turbidity));
        print_sky(label, frame);
      }
    }
  }
  checked(engine::core::cvar_set_int("r_debug_probe", 0), "r_debug_probe");
  checked(engine::core::cvar_set_float("r_sky_turbidity", 3.0F),
          "r_sky_turbidity");
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("d3d12_probe_gpu_test", &run);
}
