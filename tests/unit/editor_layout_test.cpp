// Verifies the editor's dock/window layout persistence (issue #313): the
// layout is owned by the engine rather than ImGui's truncating ini
// writer, resolves under the per-user save directory independently of
// the launch working directory, round-trips through the atomic writer,
// services ImGui's dirty flag in place of its periodic write, and — on a
// failed commit, an empty document, or a stored layout that could not be
// read — leaves the stored file byte-for-byte intact instead of
// replacing it.

#include "../test_harness.h"
#include "editor_layout.h"
#include "engine/core/platform.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

using engine::tests::TestContext;
using namespace engine::editor;

/// A layout document naming one window the editor never creates, so a
/// round trip is unambiguous evidence that these bytes made the trip.
constexpr const char *kProbeLayout = "[Window][EngineLayoutProbe]\n"
                                     "Pos=123,45\n"
                                     "Size=678,90\n"
                                     "Collapsed=0\n\n";

/// Absolute scratch directory for one named case, recreated empty so a
/// previous run cannot mask a missing write.
bool make_scratch_dir(const char *leaf, std::string *out) noexcept {
  std::error_code ec{};
  const std::filesystem::path base =
      std::filesystem::temp_directory_path(ec) / "engine_editor_layout_test";
  if (ec) {
    return false;
  }
  const std::filesystem::path dir = base / leaf;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return false;
  }
  *out = dir.string();
  return true;
}

/// Reads a whole file; false when it is absent or unreadable. Opens
/// portably across CRTs — MSVC's UCRT deprecates fopen, and the lane
/// builds with /WX.
bool read_file(const std::filesystem::path &path, std::string *out) noexcept {
  const std::string asString = path.string();
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, asString.c_str(), "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(asString.c_str(), "rb");
#endif
  if (file == nullptr) {
    return false;
  }
  // Reads to EOF rather than into one fixed buffer: the oversized-layout
  // case compares a file deliberately larger than the loader's cap, and a
  // truncating reader would make that comparison lie in both directions.
  out->clear();
  char buffer[8192] = {};
  for (;;) {
    const std::size_t count = std::fread(buffer, 1U, sizeof(buffer), file);
    out->append(buffer, count);
    if (count < sizeof(buffer)) {
      break;
    }
  }
  const bool failed = std::ferror(file) != 0;
  static_cast<void>(std::fclose(file));
  if (failed) {
    out->clear();
    return false;
  }
  return true;
}

/// Writes `bytes` to `path`, creating or truncating it. Opens portably
/// for the same reason read_file does.
bool write_file(const std::filesystem::path &path,
                const std::string &bytes) noexcept {
  const std::string asString = path.string();
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, asString.c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(asString.c_str(), "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const bool wrote =
      bytes.empty() ||
      (std::fwrite(bytes.data(), 1U, bytes.size(), file) == bytes.size());
  return (std::fclose(file) == 0) && wrote;
}

/// Creates a bare ImGui context; docking is opt-in because ImGui's dock
/// settings handler always emits a "[Docking][Data]" header, which would
/// make an otherwise empty document non-empty.
void begin_context(bool enableDocking) noexcept {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  if (enableDocking) {
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  }
}

void end_context() noexcept { ImGui::DestroyContext(); }

/// The layout ImGui currently holds, as a string ("" when it holds none).
std::string current_settings() noexcept {
  std::size_t size = 0U;
  const char *data = ImGui::SaveIniSettingsToMemory(&size);
  if ((data == nullptr) || (size == 0U)) {
    return std::string{};
  }
  return std::string(data, size);
}

/// The layout file path for the directory currently in force.
bool layout_path(std::string *out) noexcept {
  char path[1024] = {};
  if (!editor_layout_path(path, sizeof(path))) {
    return false;
  }
  out->assign(path);
  return true;
}

/// Counts staged-temporary siblings left beside the destination; the
/// atomic writer must leave none behind, successful commit or not.
std::size_t count_staged_temporaries(const std::filesystem::path &dir,
                                     const char *stem) noexcept {
  std::size_t found = 0U;
  std::error_code ec{};
  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if ((name.rfind(stem, 0U) == 0U) && (name != stem)) {
      ++found;
    }
  }
  return found;
}

/// A stored layout survives a shutdown/restart: saved from one context
/// through the production writer, loaded into a fresh one, and still
/// describing the same window.
void check_layout_round_trips_through_the_save_directory(
    TestContext &ctx) noexcept {
  std::string dir{};
  if (!make_scratch_dir("round_trip", &dir)) {
    ctx.fail("round trip: scratch directory");
    return;
  }
  editor_layout_set_directory_override_for_tests(dir.c_str());

  begin_context(true);
  ImGui::LoadIniSettingsFromMemory(kProbeLayout, std::strlen(kProbeLayout));
  // Captured before the context dies: save writes precisely these bytes,
  // so the stored file is exactly determined and asserted as such.
  const std::string saved = current_settings();
  ctx.check(editor_layout_save(), "round trip: save reports success");
  end_context();

  std::string path{};
  ctx.check(layout_path(&path), "round trip: layout path resolves");
  ctx.check(std::filesystem::exists(std::filesystem::path(path)),
            "round trip: layout file exists in the save directory");

  std::string stored{};
  ctx.check(read_file(std::filesystem::path(path), &stored),
            "round trip: stored layout is readable");
  ctx.check(stored == saved,
            "round trip: stored bytes are exactly the saved document");
  ctx.check(stored.find("EngineLayoutProbe") != std::string::npos,
            "round trip: stored layout names the probe window");

  begin_context(true);
  ctx.check(editor_layout_load(), "round trip: load reports success");
  const std::string restored = current_settings();
  end_context();

  ctx.check(restored.find("EngineLayoutProbe") != std::string::npos,
            "round trip: restored layout names the probe window");
  ctx.check(restored.find("Pos=123,45") != std::string::npos,
            "round trip: restored layout keeps the window position");

  editor_layout_set_directory_override_for_tests("");
}

/// A fresh profile — no stored layout — is not an error, and leaves the
/// context on ImGui's defaults for the dockspace builder to fill in.
void check_fresh_profile_starts_on_the_default_layout(
    TestContext &ctx) noexcept {
  std::string dir{};
  if (!make_scratch_dir("fresh_profile", &dir)) {
    ctx.fail("fresh profile: scratch directory");
    return;
  }
  editor_layout_set_directory_override_for_tests(dir.c_str());

  std::string path{};
  ctx.check(layout_path(&path), "fresh profile: layout path resolves");
  ctx.check(!std::filesystem::exists(std::filesystem::path(path)),
            "fresh profile: no layout file is present");

  begin_context(true);
  ctx.check(!editor_layout_load(),
            "fresh profile: load reports that nothing was restored");
  ctx.check(editor_layout_initialize(),
            "fresh profile: initialize succeeds with no stored layout");
  ctx.check(ImGui::GetIO().IniFilename == nullptr,
            "fresh profile: ImGui's own ini writer is disabled");
  const std::string settings = current_settings();
  end_context();

  ctx.check(settings.find("EngineLayoutProbe") == std::string::npos,
            "fresh profile: no layout was invented");
  ctx.check(!std::filesystem::exists(std::filesystem::path(path)),
            "fresh profile: loading did not create a layout file");

  editor_layout_set_directory_override_for_tests("");
}

/// A failed commit leaves the destination untouched and stages nothing
/// behind. The fault is injected at the production write boundary: a
/// non-empty directory occupies the destination name, so the staged
/// temporary is written but the atomic rename over it fails — the same
/// stream state a rename failure produces on a full or read-only volume,
/// and one no privilege level can bypass.
void check_failed_commit_leaves_the_destination_untouched(
    TestContext &ctx) noexcept {
  std::string dir{};
  if (!make_scratch_dir("failed_commit", &dir)) {
    ctx.fail("failed commit: scratch directory");
    return;
  }
  editor_layout_set_directory_override_for_tests(dir.c_str());

  std::string path{};
  if (!layout_path(&path)) {
    ctx.fail("failed commit: layout path resolves");
    editor_layout_set_directory_override_for_tests("");
    return;
  }

  const std::filesystem::path destination(path);
  std::error_code ec{};
  std::filesystem::create_directories(destination / "occupied", ec);
  if (ec) {
    ctx.fail("failed commit: destination could not be occupied");
    editor_layout_set_directory_override_for_tests("");
    return;
  }

  begin_context(true);
  ImGui::LoadIniSettingsFromMemory(kProbeLayout, std::strlen(kProbeLayout));
  ctx.check(!editor_layout_save(),
            "failed commit: save reports the failure instead of succeeding");
  end_context();

  ctx.check(std::filesystem::is_directory(destination),
            "failed commit: the previous destination is untouched");
  ctx.check(std::filesystem::exists(destination / "occupied"),
            "failed commit: the destination's contents are intact");
  ctx.check(count_staged_temporaries(std::filesystem::path(dir),
                                     destination.filename().string().c_str()) ==
                0U,
            "failed commit: no staged temporary is left behind");

  editor_layout_set_directory_override_for_tests("");
}

/// A context holding no layout must never replace a stored one. This is
/// the truncation ImGui's own writer performs: it opens the destination
/// in "wt" mode and writes whatever it has, so a save issued before the
/// layout is populated empties the file.
void check_empty_settings_never_replace_a_stored_layout(
    TestContext &ctx) noexcept {
  std::string dir{};
  if (!make_scratch_dir("empty_settings", &dir)) {
    ctx.fail("empty settings: scratch directory");
    return;
  }
  editor_layout_set_directory_override_for_tests(dir.c_str());

  begin_context(true);
  ImGui::LoadIniSettingsFromMemory(kProbeLayout, std::strlen(kProbeLayout));
  const bool stored = editor_layout_save();
  end_context();
  if (!stored) {
    ctx.fail("empty settings: the layout to protect was stored");
    editor_layout_set_directory_override_for_tests("");
    return;
  }

  std::string path{};
  std::string before{};
  if (!layout_path(&path) ||
      !read_file(std::filesystem::path(path), &before)) {
    ctx.fail("empty settings: the stored layout is readable");
    editor_layout_set_directory_override_for_tests("");
    return;
  }

  begin_context(false);
  ctx.check(current_settings().empty(),
            "empty settings: the context holds no layout");
  ctx.check(!editor_layout_save(),
            "empty settings: save refuses an empty document");
  end_context();

  std::string after{};
  ctx.check(read_file(std::filesystem::path(path), &after),
            "empty settings: the stored layout is still readable");
  ctx.check(after == before,
            "empty settings: the stored layout is byte-for-byte intact");

  editor_layout_set_directory_override_for_tests("");
}

/// The layout path is the per-user save directory's, not the launch
/// directory's, so the same profile is read back whatever the editor was
/// started from.
void check_layout_path_is_independent_of_the_working_directory(
    TestContext &ctx) noexcept {
  editor_layout_set_directory_override_for_tests("");

  char saveDir[900] = {};
  if (!engine::core::platform_get_save_dir(saveDir, sizeof(saveDir))) {
    ctx.fail("cwd independence: platform save directory is available");
    return;
  }

  std::error_code ec{};
  const std::filesystem::path originalCwd =
      std::filesystem::current_path(ec);
  if (ec) {
    ctx.fail("cwd independence: current directory is readable");
    return;
  }

  std::string first{};
  ctx.check(layout_path(&first), "cwd independence: layout path resolves");
  ctx.check(first.rfind(saveDir, 0U) == 0U,
            "cwd independence: layout lives under the per-user save "
            "directory");
  ctx.check(std::filesystem::path(first).is_absolute(),
            "cwd independence: layout path is absolute");

  const std::filesystem::path elsewhere =
      std::filesystem::temp_directory_path(ec);
  if (ec) {
    ctx.fail("cwd independence: an alternate directory is available");
    return;
  }
  std::filesystem::current_path(elsewhere, ec);
  if (ec) {
    ctx.fail("cwd independence: the working directory could be changed");
    return;
  }

  std::string second{};
  const bool resolvedAgain = layout_path(&second);

  std::filesystem::current_path(originalCwd, ec);
  ctx.check(!ec, "cwd independence: the working directory was restored");

  ctx.check(resolvedAgain,
            "cwd independence: layout path resolves from elsewhere");
  ctx.check(first == second,
            "cwd independence: the path is identical from another working "
            "directory");
}

/// Without a context there is nothing to read or write; every entry
/// point must decline rather than reach into a null ImGui context.
void check_entry_points_decline_without_a_context(TestContext &ctx) noexcept {
  ctx.check(!editor_layout_initialize(),
            "no context: initialize declines");
  ctx.check(!editor_layout_load(), "no context: load declines");
  ctx.check(!editor_layout_save(), "no context: save declines");
  // Must not reach into the null context; reaching one would fault here
  // rather than return, so surviving the call is the assertion.
  editor_layout_save_if_dirty();
  ctx.check(ImGui::GetCurrentContext() == nullptr,
            "no context: the dirty-flag service created no context");
}

/// The substitute for ImGui's periodic writer: a raised dirty flag saves
/// the layout and clears the flag; a clear flag writes nothing at all.
void check_dirty_flag_service_saves_and_clears(TestContext &ctx) noexcept {
  std::string dir{};
  if (!make_scratch_dir("dirty_flag", &dir)) {
    ctx.fail("dirty flag: scratch directory");
    return;
  }
  editor_layout_set_directory_override_for_tests(dir.c_str());

  std::string path{};
  if (!layout_path(&path)) {
    ctx.fail("dirty flag: layout path resolves");
    editor_layout_set_directory_override_for_tests("");
    return;
  }
  const std::filesystem::path destination(path);

  // A clear flag must not write: a sentinel already at the destination
  // survives untouched.
  const std::string sentinel = "sentinel-not-a-layout\n";
  if (!write_file(destination, sentinel)) {
    ctx.fail("dirty flag: sentinel could be planted");
    editor_layout_set_directory_override_for_tests("");
    return;
  }

  begin_context(true);
  ImGui::LoadIniSettingsFromMemory(kProbeLayout, std::strlen(kProbeLayout));
  ImGui::GetIO().WantSaveIniSettings = false;
  editor_layout_save_if_dirty();
  end_context();

  std::string afterClear{};
  ctx.check(read_file(destination, &afterClear),
            "dirty flag: destination still readable after a clear flag");
  ctx.check(afterClear == sentinel,
            "dirty flag: a clear flag writes nothing");

  // A raised flag must save and clear the flag.
  begin_context(true);
  ImGui::LoadIniSettingsFromMemory(kProbeLayout, std::strlen(kProbeLayout));
  ImGui::GetIO().WantSaveIniSettings = true;
  const std::string expected = current_settings();
  ImGui::GetIO().WantSaveIniSettings = true;
  editor_layout_save_if_dirty();
  const bool flagCleared = !ImGui::GetIO().WantSaveIniSettings;
  end_context();

  ctx.check(flagCleared, "dirty flag: a raised flag is cleared once serviced");

  std::string afterDirty{};
  ctx.check(read_file(destination, &afterDirty),
            "dirty flag: the serviced layout is readable");
  ctx.check(afterDirty == expected,
            "dirty flag: a raised flag stores exactly the current layout");
  ctx.check(afterDirty != sentinel,
            "dirty flag: the sentinel was replaced by the real layout");

  editor_layout_set_directory_override_for_tests("");
}

/// An empty stored layout is the fresh-profile path, not a fault: it
/// loads as "nothing restored" and must leave saving enabled, so a first
/// run still stores its layout.
void check_empty_stored_file_still_permits_saving(TestContext &ctx) noexcept {
  std::string dir{};
  if (!make_scratch_dir("empty_file", &dir)) {
    ctx.fail("empty file: scratch directory");
    return;
  }
  editor_layout_set_directory_override_for_tests(dir.c_str());

  std::string path{};
  if (!layout_path(&path) ||
      !write_file(std::filesystem::path(path), std::string{})) {
    ctx.fail("empty file: an empty layout could be planted");
    editor_layout_set_directory_override_for_tests("");
    return;
  }

  begin_context(true);
  ctx.check(!editor_layout_load(),
            "empty file: load reports that nothing was restored");
  ImGui::LoadIniSettingsFromMemory(kProbeLayout, std::strlen(kProbeLayout));
  ctx.check(editor_layout_save(),
            "empty file: saving stays enabled after an empty stored layout");
  end_context();

  std::string stored{};
  ctx.check(read_file(std::filesystem::path(path), &stored),
            "empty file: the newly saved layout is readable");
  ctx.check(stored.find("EngineLayoutProbe") != std::string::npos,
            "empty file: the fresh layout was stored");

  editor_layout_set_directory_override_for_tests("");
}

/// A stored layout too large for this build to read must not then be
/// replaced by the default layout ImGui builds moments later. This is
/// issue #313's own failure mode reappearing at the new layer: the write
/// is atomic and therefore durable, which is exactly what makes it
/// destructive. Drives the production entry points in the order the
/// editor does — load, then the first settle.
void check_oversized_stored_layout_is_never_overwritten(
    TestContext &ctx) noexcept {
  std::string dir{};
  if (!make_scratch_dir("oversized", &dir)) {
    ctx.fail("oversized: scratch directory");
    return;
  }
  editor_layout_set_directory_override_for_tests(dir.c_str());

  std::string path{};
  if (!layout_path(&path)) {
    ctx.fail("oversized: layout path resolves");
    editor_layout_set_directory_override_for_tests("");
    return;
  }
  const std::filesystem::path destination(path);

  // Larger than the loader's buffer, so the read is refused. A real user
  // reaches this by outgrowing the cap, not by corruption — the file is
  // perfectly good, and losing it would be the worst version of this bug.
  const std::string oversized(200U * 1024U, 'x');
  if (!write_file(destination, oversized)) {
    ctx.fail("oversized: the oversized layout could be planted");
    editor_layout_set_directory_override_for_tests("");
    return;
  }

  begin_context(true);
  ctx.check(!editor_layout_load(),
            "oversized: load reports that nothing was restored");
  // What the editor does next: builds a default layout and settles.
  ImGui::LoadIniSettingsFromMemory(kProbeLayout, std::strlen(kProbeLayout));
  ImGui::GetIO().WantSaveIniSettings = true;
  editor_layout_save_if_dirty();
  ctx.check(!editor_layout_save(),
            "oversized: an explicit save is refused too");
  end_context();

  std::string after{};
  ctx.check(read_file(destination, &after),
            "oversized: the stored layout is still readable");
  ctx.check(after.size() == oversized.size(),
            "oversized: the stored layout kept its full length");
  ctx.check(after == oversized,
            "oversized: the stored layout is byte-for-byte intact");

  // The latch is per-profile, not permanent: a different profile saves.
  editor_layout_set_directory_override_for_tests("");
  std::string other{};
  if (make_scratch_dir("oversized_other", &other)) {
    editor_layout_set_directory_override_for_tests(other.c_str());
    begin_context(true);
    ImGui::LoadIniSettingsFromMemory(kProbeLayout, std::strlen(kProbeLayout));
    ctx.check(editor_layout_save(),
              "oversized: the refusal does not leak into another profile");
    end_context();
    editor_layout_set_directory_override_for_tests("");
  } else {
    ctx.fail("oversized: second scratch directory");
  }
}

/// The Unreadable branch: a stored layout that exists but cannot be read
/// must latch saving off, exactly as the oversized one does, rather than
/// being replaced by the default layout at the first settle.
///
/// A directory planted at the destination reaches that branch on both
/// platforms, by different routes: a POSIX CRT opens it and fails the
/// read with the error flag set, while a Windows CRT refuses the open
/// with something other than ENOENT. The second route is only Unreadable
/// because the opener distinguishes absence from other open failures —
/// before that it was Absent, and this case passed on Windows for a
/// reason unrelated to the contract it names.
///
/// Permission damage is the other way in, and the more likely one in the
/// field, but it cannot be injected here: the test suite runs as root in
/// CI containers, and root bypasses the DAC check that would deny the
/// open. The planted directory needs no privilege to work.
void check_unopenable_stored_layout_latches_and_survives(
    TestContext &ctx) noexcept {
  std::string dir{};
  if (!make_scratch_dir("read_fault", &dir)) {
    ctx.fail("read fault: scratch directory");
    return;
  }
  editor_layout_set_directory_override_for_tests(dir.c_str());

  std::string path{};
  if (!layout_path(&path)) {
    ctx.fail("read fault: layout path resolves");
    editor_layout_set_directory_override_for_tests("");
    return;
  }
  const std::filesystem::path destination(path);

  std::error_code ec{};
  std::filesystem::create_directories(destination / "occupied", ec);
  if (ec) {
    ctx.fail("read fault: the destination could be occupied");
    editor_layout_set_directory_override_for_tests("");
    return;
  }

  begin_context(true);
  ctx.check(!editor_layout_load(),
            "read fault: load reports that nothing was restored");
  ImGui::LoadIniSettingsFromMemory(kProbeLayout, std::strlen(kProbeLayout));
  ImGui::GetIO().WantSaveIniSettings = true;
  editor_layout_save_if_dirty();
  // The latch, not the destination's happening to be unwritable, is what
  // must stop this: assert the refusal itself, so the case cannot pass
  // for the incidental reason that atomic_write_file fails on a directory.
  ctx.check(!editor_layout_save(),
            "read fault: saving is latched off for the session");
  end_context();

  ctx.check(std::filesystem::is_directory(destination),
            "read fault: the destination is untouched");
  ctx.check(std::filesystem::exists(destination / "occupied"),
            "read fault: the destination's contents are intact");

  editor_layout_set_directory_override_for_tests("");
}

/// The same Unreadable contract reached through a failing *open* rather
/// than a failing read, which is the branch that decides whether "the
/// open failed" is mistaken for "there is no file".
///
/// A self-referential symlink is the injection: opening it fails with
/// ELOOP, which no privilege level bypasses — unlike the permission
/// damage this stands in for, which root would sail straight through.
/// Skipped where symlinks cannot be created (Windows without the
/// privilege), rather than silently asserting nothing.
void check_unopenable_path_is_not_mistaken_for_absent(
    TestContext &ctx) noexcept {
  std::string dir{};
  if (!make_scratch_dir("open_fault", &dir)) {
    ctx.fail("open fault: scratch directory");
    return;
  }
  editor_layout_set_directory_override_for_tests(dir.c_str());

  std::string path{};
  if (!layout_path(&path)) {
    ctx.fail("open fault: layout path resolves");
    editor_layout_set_directory_override_for_tests("");
    return;
  }
  const std::filesystem::path destination(path);

  std::error_code ec{};
  std::filesystem::create_symlink(destination.filename(), destination, ec);
  if (ec) {
    std::fprintf(stdout,
                 "open fault: symlinks unavailable here; case skipped\n");
    editor_layout_set_directory_override_for_tests("");
    return;
  }

  begin_context(true);
  ctx.check(!editor_layout_load(),
            "open fault: load reports that nothing was restored");
  ImGui::LoadIniSettingsFromMemory(kProbeLayout, std::strlen(kProbeLayout));
  ImGui::GetIO().WantSaveIniSettings = true;
  editor_layout_save_if_dirty();
  ctx.check(!editor_layout_save(),
            "open fault: a failed open latches saving off, not a fresh "
            "profile");
  end_context();

  ctx.check(std::filesystem::is_symlink(std::filesystem::symlink_status(
                destination, ec)),
            "open fault: the unopenable path was not replaced");

  editor_layout_set_directory_override_for_tests("");
}

} // namespace

int main() {
  TestContext ctx{};

  check_entry_points_decline_without_a_context(ctx);
  check_layout_round_trips_through_the_save_directory(ctx);
  check_fresh_profile_starts_on_the_default_layout(ctx);
  check_failed_commit_leaves_the_destination_untouched(ctx);
  check_empty_settings_never_replace_a_stored_layout(ctx);
  check_layout_path_is_independent_of_the_working_directory(ctx);
  check_dirty_flag_service_saves_and_clears(ctx);
  check_empty_stored_file_still_permits_saving(ctx);
  check_oversized_stored_layout_is_never_overwritten(ctx);
  check_unopenable_stored_layout_latches_and_survives(ctx);
  check_unopenable_path_is_not_mistaken_for_absent(ctx);

  return ctx.finish("editor_layout");
}
