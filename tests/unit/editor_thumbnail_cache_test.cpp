// Verifies the content browser's thumbnail cache on the null render
// device: a folder of 200 assets shows every thumbnail at once (the cache
// once stopped at 128 and left the rest blank), a folder larger than the
// cache's slots still shows each thumbnail as it scrolls into view by
// releasing the least recently drawn, and a thumbnail drawn this frame is
// never released to make room for another.

#include "editor_session.h"

#include "../test_harness.h"

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/renderer/render_device.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

using engine::editor::editor_session;
using engine::editor::kMaxThumbnails;
using engine::editor::load_thumbnail_texture;
using engine::tests::TestContext;

/// A 1x1 RGBA PNG: every thumbnail the test writes decodes to one texel.
constexpr unsigned char kPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
    0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
    0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

/// Writes the thumbnail for asset `i` and returns the asset's path.
std::string write_thumbnail(const std::filesystem::path &dir, std::size_t i) {
  const std::string name = "asset_" + std::to_string(i) + ".mesh";
  const std::filesystem::path png = dir / ".thumbnails" / (name + ".png");
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, png.string().c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(png.string().c_str(), "wb");
#endif
  if (file != nullptr) {
    std::fwrite(kPng, 1U, sizeof(kPng), file);
    std::fclose(file);
  }
  return (dir / name).string();
}

} // namespace

int main() {
  TestContext t;
  t.check(engine::core::initialize_logging(), "initialize logging");
  t.check(engine::core::initialize_cvars(), "initialize cvars");
  t.check(engine::core::cvar_register_bool("r_null_device", true, "test"),
          "select the null device");
  engine::editor::register_thumbnail_cache_cvars();
  t.check(engine::renderer::initialize_render_device(),
          "initialize the null device");

  std::error_code ec{};
  const std::filesystem::path dir =
      std::filesystem::current_path(ec) / "editor_thumbnail_cache_test";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir / ".thumbnails", ec);
  t.check(!ec, "create the fixture folder");
  constexpr std::size_t kAssets = kMaxThumbnails + 100U;
  std::string paths[kAssets];
  for (std::size_t i = 0U; i < kAssets; ++i) {
    paths[i] = write_thumbnail(dir, i);
  }

  // --- A 200-asset folder, all on screen in one frame.
  engine::editor::clear_thumbnail_cache();
  engine::editor::advance_thumbnail_frame();
  std::size_t shown = 0U;
  for (std::size_t i = 0U; i < 200U; ++i) {
    shown += (load_thumbnail_texture(paths[i].c_str()) !=
              engine::renderer::kInvalidDeviceTexture)
                 ? 1U
                 : 0U;
  }
  t.check(shown == 200U, "a 200-asset folder shows every thumbnail");

  // --- More assets than slots, scrolled through a frame at a time: each
  // still shows, and the cache never holds more than its slots.
  engine::editor::clear_thumbnail_cache();
  shown = 0U;
  for (std::size_t i = 0U; i < kAssets; ++i) {
    engine::editor::advance_thumbnail_frame();
    shown += (load_thumbnail_texture(paths[i].c_str()) !=
              engine::renderer::kInvalidDeviceTexture)
                 ? 1U
                 : 0U;
  }
  t.check(shown == kAssets,
          "scrolling past the cache's slots shows every thumbnail");
  t.check(editor_session().thumbnailCount == kMaxThumbnails,
          "the least recently drawn thumbnails made room");

  // --- Everything cached is on screen this frame: nothing is released,
  // and the one more that does not fit is refused.
  engine::editor::clear_thumbnail_cache();
  engine::editor::advance_thumbnail_frame();
  for (std::size_t i = 0U; i < kMaxThumbnails; ++i) {
    static_cast<void>(load_thumbnail_texture(paths[i].c_str()));
  }
  const engine::renderer::DeviceTextureHandle first =
      load_thumbnail_texture(paths[0].c_str());
  t.check(load_thumbnail_texture(paths[kMaxThumbnails].c_str()) ==
              engine::renderer::kInvalidDeviceTexture,
          "a thumbnail that cannot fit beside the ones on screen is refused");
  t.check(load_thumbnail_texture(paths[0].c_str()) == first,
          "and no thumbnail drawn this frame was released for it");

  // --- The byte budget binds too: a 1 KB budget holds 256 one-texel
  // (4-byte) thumbnails, far fewer than the slots, and scrolling 300 still
  // shows each by releasing the least recently drawn.
  engine::editor::clear_thumbnail_cache();
  t.check(engine::core::cvar_set_int("editor.thumbnail_cache_kb", 1),
          "shrink the budget to 1 KB");
  shown = 0U;
  for (std::size_t i = 0U; i < 300U; ++i) {
    engine::editor::advance_thumbnail_frame();
    shown += (load_thumbnail_texture(paths[i].c_str()) !=
              engine::renderer::kInvalidDeviceTexture)
                 ? 1U
                 : 0U;
  }
  t.check(shown == 300U, "every thumbnail shows under a small budget");
  t.check((editor_session().thumbnailBytes <= 1024U) &&
              (editor_session().thumbnailCount == 256U),
          "and the cache holds no more bytes than the budget");

  engine::editor::clear_thumbnail_cache();
  engine::renderer::shutdown_render_device();
  std::filesystem::remove_all(dir, ec);
  engine::core::shutdown_cvars();
  engine::core::shutdown_logging();
  return t.finish("editor_thumbnail_cache");
}
