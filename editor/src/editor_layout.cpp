// Implements the editor's dock/window layout persistence (issue #313).
// ImGui's default ini writer truncates its destination in place and
// resolves it against the launch working directory, so a crash during
// its flush loses the whole layout and launching from elsewhere silently
// forks it. Layout is authored editor-settings data, which the hard rule
// on authored files requires to be staged and atomically replaced, so
// the engine disables that writer and routes the layout through
// core::atomic_write_file into the per-user save directory instead.

#include "editor_layout.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include "engine/core/atomic_file.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"

namespace engine::editor {

namespace {

constexpr const char *kLogChannel = "editor.layout";
constexpr const char *kLayoutFileName = "editor_layout.ini";

/// Upper bound on a stored layout. ImGui's ini text runs a few hundred
/// bytes per window and per dock node, so this leaves room for far more
/// panels than the editor has while keeping both directions of the
/// transfer free of heap allocation.
constexpr std::size_t kMaxLayoutBytes = 64U * 1024U;

/// Test-only override directory; empty means "use the real platform save
/// directory". Single-threaded like the rest of editor session state.
char g_directoryOverride[900] = {};

/// Read staging for the loader. File-scope rather than stack because the
/// editor is single-threaded here and a 64 KiB frame would be the
/// largest in the module by an order of magnitude.
char g_readBuffer[kMaxLayoutBytes] = {};

/// Portably opens a file for reading across CRTs.
std::FILE *open_file_for_read(const char *path) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  return file;
}

/// Distinguishes the ordinary fresh-profile case from a real fault, so
/// the loader can stay silent for the former and report the latter
/// without a filesystem probe (which would allocate on a noexcept path).
/// TooLarge is kept apart from Unreadable because the two say different
/// things to a user: the stored layout is damaged, versus it is intact
/// but outgrew the buffer this build can hold.
enum class ReadResult : std::uint8_t { Ok, Absent, Unreadable, TooLarge };

/// Latched when a stored layout exists but could not be read. Saving is
/// refused for the rest of the session while it is set, because the
/// alternative is that the default layout ImGui builds moments later is
/// atomically committed over a file whose contents we failed to read —
/// the very loss this module exists to prevent, and a rule violation:
/// a failed load must preserve the previous valid state, not preserve it
/// until the first settle. An absent or empty file is NOT a failed load;
/// those are the genuine fresh-profile paths and keep saving normally.
bool g_loadFailed = false;

/// True once the refusal above has been logged, so a session that keeps
/// settling logs the reason once rather than once per settle.
bool g_refusalLogged = false;

/// Reads the whole file into `out`, NUL-terminated. A read that fails
/// part-way is rejected rather than reported as a shorter layout, so a
/// damaged read can never be re-saved over the good file as a
/// truncation; so is one larger than capacity - 1, which could not be
/// written back whole either.
ReadResult read_whole_file(const char *path, char *out, std::size_t capacity,
                           std::size_t *outSize) noexcept {
  std::FILE *file = open_file_for_read(path);
  if (file == nullptr) {
    return ReadResult::Absent;
  }
  const std::size_t readCount = std::fread(out, 1U, capacity - 1U, file);
  const bool overflow = std::fgetc(file) != EOF;
  const bool hitError = std::ferror(file) != 0;
  static_cast<void>(std::fclose(file));
  if (hitError) {
    return ReadResult::Unreadable;
  }
  if (overflow) {
    return ReadResult::TooLarge;
  }
  out[readCount] = '\0';
  if (outSize != nullptr) {
    *outSize = readCount;
  }
  return ReadResult::Ok;
}

/// Resolves the layout directory: the test override when set, otherwise
/// the real per-user platform save directory.
bool resolve_layout_directory(char *out, std::size_t capacity) noexcept {
  if (g_directoryOverride[0] != '\0') {
    const int written =
        std::snprintf(out, capacity, "%s", g_directoryOverride);
    return (written > 0) && (static_cast<std::size_t>(written) < capacity);
  }
  return core::platform_get_save_dir(out, capacity);
}

} // namespace

bool editor_layout_path(char *out, std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return false;
  }
  char directory[900] = {};
  if (!resolve_layout_directory(directory, sizeof(directory))) {
    return false;
  }
  const int written =
      std::snprintf(out, capacity, "%s/%s", directory, kLayoutFileName);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

bool editor_layout_initialize() noexcept {
  if (ImGui::GetCurrentContext() == nullptr) {
    return false;
  }
  // Hands layout persistence to the engine: with no filename ImGui stops
  // writing (and stops looking for) an imgui.ini beside the launch
  // directory and instead raises WantSaveIniSettings for us to service.
  ImGui::GetIO().IniFilename = nullptr;
  // A new session re-reads the stored layout, so any latch from a previous
  // one is stale; the load below re-arms it if the file is still unreadable.
  g_loadFailed = false;
  g_refusalLogged = false;
  static_cast<void>(editor_layout_load());
  return true;
}

bool editor_layout_load() noexcept {
  if (ImGui::GetCurrentContext() == nullptr) {
    return false;
  }
  char path[1024] = {};
  if (!editor_layout_path(path, sizeof(path))) {
    core::log_message(core::LogLevel::Warning, kLogChannel,
                      "no layout directory available; using defaults");
    return false;
  }

  std::size_t size = 0U;
  const ReadResult result =
      read_whole_file(path, g_readBuffer, sizeof(g_readBuffer), &size);
  if (result == ReadResult::Unreadable) {
    // Latched, not merely reported: see g_loadFailed. The file stays where
    // it is so a later run — or the user — can still recover it.
    g_loadFailed = true;
    core::log_message(
        core::LogLevel::Error, kLogChannel,
        "stored editor layout could not be read; using defaults and leaving "
        "the stored file untouched for this session");
    return false;
  }
  if (result == ReadResult::TooLarge) {
    g_loadFailed = true;
    core::log_message(
        core::LogLevel::Error, kLogChannel,
        "stored editor layout is larger than this build can load; using "
        "defaults and leaving the stored file untouched for this session");
    return false;
  }
  // A missing file is the ordinary fresh-profile case, and so is an empty
  // one; neither is a fault, and both leave ImGui on its defaults — and
  // both keep saving enabled, so a first run still stores its layout.
  if ((result != ReadResult::Ok) || (size == 0U)) {
    return false;
  }

  ImGui::LoadIniSettingsFromMemory(g_readBuffer, size);
  return true;
}

bool editor_layout_save() noexcept {
  if (ImGui::GetCurrentContext() == nullptr) {
    return false;
  }
  if (g_loadFailed) {
    if (!g_refusalLogged) {
      g_refusalLogged = true;
      core::log_message(
          core::LogLevel::Warning, kLogChannel,
          "not saving the editor layout: the stored one could not be read "
          "this session, and overwriting it would discard it");
    }
    return false;
  }

  std::size_t size = 0U;
  const char *settings = ImGui::SaveIniSettingsToMemory(&size);
  if ((settings == nullptr) || (size == 0U)) {
    // ImGui yields an empty document when it has no window or dock state
    // to describe — before the first frame, or after a context reset.
    // Writing it would replace a good layout with nothing, which is the
    // truncation this path exists to prevent.
    return false;
  }
  if (size > kMaxLayoutBytes - 1U) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "editor layout exceeds the storable size; not saved");
    return false;
  }

  // platform_get_save_dir composes a path without creating it, so the
  // first save of a fresh profile has to. Mirrors recent_scenes_persist,
  // including its allocating path construction on a noexcept path, which
  // #316 tracks for the persist helpers as a group.
  char directory[900] = {};
  if (resolve_layout_directory(directory, sizeof(directory))) {
    std::error_code ec{};
    std::filesystem::create_directories(std::filesystem::path(directory), ec);
  }

  char path[1024] = {};
  if (!editor_layout_path(path, sizeof(path))) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "no layout directory available; layout not saved");
    return false;
  }

  if (!core::atomic_write_file(path, settings, size)) {
    core::log_message(core::LogLevel::Error, kLogChannel,
                      "failed to write the editor layout");
    return false;
  }
  return true;
}

void editor_layout_save_if_dirty() noexcept {
  if (ImGui::GetCurrentContext() == nullptr) {
    return;
  }
  ImGuiIO &io = ImGui::GetIO();
  if (!io.WantSaveIniSettings) {
    return;
  }
  // Cleared whether or not the write succeeds: a failing destination
  // would otherwise re-arm every frame and turn one logged failure into
  // an unbounded log flood. The next layout change re-raises the flag.
  io.WantSaveIniSettings = false;
  static_cast<void>(editor_layout_save());
}

void editor_layout_set_directory_override_for_tests(
    const char *directory) noexcept {
  // Pointing at a different directory is pointing at a different profile,
  // so the failed-load latch from the previous one must not carry over and
  // silently disable saving for the next case.
  g_loadFailed = false;
  g_refusalLogged = false;
  if ((directory == nullptr) || (directory[0] == '\0')) {
    g_directoryOverride[0] = '\0';
    return;
  }
  const int written = std::snprintf(g_directoryOverride,
                                    sizeof(g_directoryOverride), "%s",
                                    directory);
  if ((written <= 0) ||
      (static_cast<std::size_t>(written) >= sizeof(g_directoryOverride))) {
    g_directoryOverride[0] = '\0';
  }
}

} // namespace engine::editor
