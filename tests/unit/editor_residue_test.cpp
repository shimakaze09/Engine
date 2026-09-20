// Pins that reset_editor_session_residue, which shutdown_editor runs,
// returns every piece of editor state that outlives a world to its
// defaults: the content-browser asset index is dropped, the layout's
// refusal latch clears so the next session judges its own load, and the
// picker, console and inspector view state start over. Headless: none of
// it needs a window or an ImGui backend.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <imgui.h>

#include "../test_harness.h"
#include "editor_asset_index.h"
#include "editor_layout.h"
#include "editor_session.h"

namespace {

using namespace engine::editor;

/// Walks upward until the bundled assets are found, so the asset index
/// has a root to walk.
bool set_working_directory_with_assets() noexcept {
  const std::filesystem::path original = std::filesystem::current_path();
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};
  for (const std::filesystem::path &candidate : candidates) {
    std::error_code ec{};
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      continue;
    }
    if (std::filesystem::exists(normalized / "assets/main.lua", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

bool write_file(const std::filesystem::path &path,
                const std::string &content) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path.string().c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.string().c_str(), "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t written =
      std::fwrite(content.data(), 1U, content.size(), file);
  std::fclose(file);
  return written == content.size();
}

constexpr const char *kProbeLayout = "[Window][Probe]\nPos=1,2\nSize=3,4\n\n";

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  if (!set_working_directory_with_assets()) {
    std::printf("FAIL: assets not found\n");
    return 1;
  }

  // --- Asset index ---
  ctx.check(rebuild_asset_index(), "the asset index walks the bundled root");
  ctx.check(asset_index_built() && (asset_index_count() > 0U),
            "the index holds the bundled assets");
  const std::uint64_t generationBefore = asset_index_generation();

  // --- Panel view state ---
  std::snprintf(editor_session().pickers.assetQuery,
                sizeof(editor_session().pickers.assetQuery), "%s", "barrel");
  editor_session().console.paused = true;
  editor_session().console.pausedEntryCount = 9U;
  editor_session().console.filter.showTrace = false;
  std::snprintf(editor_session().inspector.addComponentFilter,
                sizeof(editor_session().inspector.addComponentFilter), "%s",
                "light");

  // --- Layout refusal latch ---
  std::error_code ec{};
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() / "engine_editor_residue_test";
  std::filesystem::remove_all(scratch, ec);
  ctx.check(std::filesystem::create_directories(scratch, ec) && !ec,
            "scratch directory");
  editor_layout_set_directory_override_for_tests(scratch.string().c_str());
  char layoutPath[1024] = {};
  ctx.check(editor_layout_path(layoutPath, sizeof(layoutPath)),
            "layout path resolves");
  const std::string oversized(200U * 1024U, 'x');
  ctx.check(write_file(layoutPath, oversized), "plant an unreadable layout");

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  ctx.check(!editor_layout_load(), "the oversized layout is not restored");
  ImGui::LoadIniSettingsFromMemory(kProbeLayout, std::strlen(kProbeLayout));
  ctx.check(!editor_layout_save(), "saving is refused while latched");
  static_cast<void>(std::remove(layoutPath));
  ctx.check(!editor_layout_save(),
            "the refusal is the session's latch, not the stored file");

  // --- The reset ---
  reset_editor_session_residue();

  ctx.check(!asset_index_built() && (asset_index_count() == 0U),
            "the asset index is dropped");
  ctx.check(asset_index_generation() != generationBefore,
            "dependent caches see a new generation");
  ctx.check(editor_session().pickers.assetQuery[0] == '\0',
            "picker search text starts over");
  ctx.check(!editor_session().console.paused &&
                (editor_session().console.pausedEntryCount == 0U) &&
                editor_session().console.filter.showTrace,
            "console view state starts over");
  ctx.check(editor_session().inspector.addComponentFilter[0] == '\0',
            "inspector search text starts over");
  ctx.check(editor_layout_save(), "the next session's save is judged afresh");
  ctx.check(std::filesystem::exists(layoutPath, ec) && !ec,
            "the fresh save reached the profile");

  ImGui::DestroyContext();
  editor_layout_set_directory_override_for_tests("");
  std::filesystem::remove_all(scratch, ec);
  return ctx.finish("editor_residue");
}
