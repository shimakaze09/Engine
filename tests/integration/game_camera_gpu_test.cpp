// GPU regression for #796: only a Camera renders the game. A scene with a
// lit cube and no Camera must present nothing but the cleared back
// buffer — before the fix the Game view drew the scene from a default
// pose no camera held, (0, 2, 5) looking at the origin, which frames this
// very cube. Once a Camera entity exists the same scene is drawn, and the
// Game view has an image the editor can show; once the Camera goes the
// view is black again and has no image, so the editor never shows the
// last frame a camera drew.

#include "../gpu_scene_fixture.h"
#include "engine/renderer/command_buffer.h"

#include <cstdint>
#include <cstdio>

namespace {

using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

/// The brightest byte of any colour channel in the frame.
std::uint8_t brightest(const CapturedFrame &frame) noexcept {
  std::uint8_t peak = 0U;
  for (std::uint32_t y = 0U; y < frame.height; ++y) {
    for (std::uint32_t x = 0U; x < frame.width; ++x) {
      for (std::uint32_t c = 0U; c < 3U; ++c) {
        const std::uint8_t value = frame.channel(x, y, c);
        peak = (value > peak) ? value : peak;
      }
    }
  }
  return peak;
}

int body(engine::EnginePipeline &pipeline, World &world) {
  engine::runtime::Transform cubeAt{};
  if (engine::tests::add_builtin_mesh(world, "builtin://cube", cubeAt,
                                      engine::math::Vec3(0.9F, 0.4F, 0.2F)) ==
      kInvalidEntity) {
    std::fprintf(stderr, "FAIL: the cube could not be added\n");
    return 10;
  }

  CapturedFrame none{};
  if (!engine::tests::settle_frames(pipeline) ||
      !engine::tests::capture_presented_frame(pipeline, "game_camera_none.tga",
                                              &none)) {
    std::fprintf(stderr, "FAIL: no frame was captured without a camera\n");
    return 11;
  }
  const engine::renderer::RenderViewId game =
      engine::renderer::RenderViewId::Game;
  if (engine::renderer::get_render_view_texture(game) !=
      engine::renderer::kInvalidDeviceTexture) {
    std::fprintf(stderr, "FAIL: with no Camera the Game view has an image\n");
    return 16;
  }
  if (brightest(none) != 0U) {
    std::fprintf(stderr,
                 "FAIL: with no Camera the game drew something (brightest "
                 "byte %u); it must present only the cleared back buffer\n",
                 static_cast<unsigned>(brightest(none)));
    return 12;
  }

  if (!engine::tests::look_from(world, engine::math::Vec3(0.0F, 2.0F, 5.0F),
                                engine::math::Vec3(0.0F, 0.0F, 0.0F))) {
    std::fprintf(stderr, "FAIL: the Camera could not be added\n");
    return 13;
  }
  CapturedFrame seen{};
  if (!engine::tests::settle_frames(pipeline) ||
      !engine::tests::capture_presented_frame(pipeline, "game_camera_seen.tga",
                                              &seen)) {
    std::fprintf(stderr, "FAIL: no frame was captured with a camera\n");
    return 14;
  }
  // The sky and the cube are tens of levels above black; the cleared
  // buffer is exactly zero, so any lit pixel tells the two apart.
  if (brightest(seen) < 16U) {
    std::fprintf(stderr,
                 "FAIL: with a Camera the scene was not drawn (brightest "
                 "byte %u)\n",
                 static_cast<unsigned>(brightest(seen)));
    return 15;
  }
  if (engine::renderer::get_render_view_texture(game) ==
      engine::renderer::kInvalidDeviceTexture) {
    std::fprintf(stderr, "FAIL: with a Camera the Game view has no image\n");
    return 17;
  }

  // The Camera goes: nothing renders again, and the frame the camera drew
  // last is not offered as the view's image.
  engine::runtime::Entity cameraEntity = kInvalidEntity;
  world.for_each_alive([&](engine::runtime::Entity entity) {
    if (world.has_camera_component(entity)) {
      cameraEntity = entity;
    }
  });
  CapturedFrame gone{};
  if ((cameraEntity == kInvalidEntity) ||
      !world.remove_camera_component(cameraEntity) ||
      !engine::tests::settle_frames(pipeline) ||
      !engine::tests::capture_presented_frame(pipeline, "game_camera_gone.tga",
                                              &gone)) {
    std::fprintf(stderr, "FAIL: no frame was captured after the Camera went\n");
    return 18;
  }
  if ((brightest(gone) != 0U) ||
      (engine::renderer::get_render_view_texture(game) !=
       engine::renderer::kInvalidDeviceTexture)) {
    std::fprintf(stderr,
                 "FAIL: after the Camera went the game still drew (brightest "
                 "byte %u) or kept an image\n",
                 static_cast<unsigned>(brightest(gone)));
    return 19;
  }
  std::printf("game camera gpu: nothing without a Camera, the scene with "
              "one (brightest %u), nothing once it goes\n",
              static_cast<unsigned>(brightest(seen)));
  return 0;
}

} // namespace

int main() {
  return engine::tests::run_gpu_scene_test("game_camera_gpu", &body);
}
