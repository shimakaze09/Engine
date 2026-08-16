// Implements the private editor session state and session lifecycle
// (play snapshot/start/pause/stop, thumbnail cache, editability checks).
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_session.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

#include "engine/core/atomic_file.h"
#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/platform.h"
#include "engine/core/profiler.h"
#include "engine/core/reflect.h"
#include "engine/engine.h"
#include "engine/editor/editor_camera.h"
#include "engine/math/transform.h"
#include "engine/math/vec2.h"
#include "engine/math/vec4.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "ImGuizmo.h"

#include "editor_console_capture.h"
#include "editor_live_edit.h"
#include "engine/editor/command_history.h"
#include "engine/editor/debug_camera.h"

#include <stb_image.h>

namespace engine::editor {

namespace {

/// Process-wide editor session storage behind editor_session().
EditorSession g_session{};

} // namespace

/// Returns the process-wide editor session state.
EditorSession &editor_session() noexcept { return g_session; }

/// Returns the configured editor scene path.
const char *editor_scene_path() noexcept {
  const char *path = active_config().editorScenePath;
  return (path != nullptr) ? path : "";
}

/// Returns the configured editor asset browser root.
const char *editor_asset_root() noexcept {
  const char *path = active_config().editorAssetRoot;
  return (path != nullptr) ? path : "";
}

/// Loads the requested resource for thumbnail texture.
std::uint32_t load_thumbnail_texture(const char *assetPath) noexcept {
  if (assetPath == nullptr) {
    return 0U;
  }

  for (std::size_t i = 0U; i < editor_session().thumbnailCount; ++i) {
    if (std::strcmp(editor_session().thumbnailCache[i].path, assetPath) == 0) {
      return editor_session().thumbnailCache[i].textureId;
    }
  }
  if (editor_session().thumbnailCount >= kMaxThumbnails) {
    return 0U;
  }

  std::string assetStr(assetPath);
  std::size_t lastSlash = assetStr.find_last_of("/\\");
  std::string thumbPath;
  if (lastSlash != std::string::npos) {
    thumbPath = assetStr.substr(0, lastSlash) + "/.thumbnails/" +
                assetStr.substr(lastSlash + 1U) + ".png";
  } else {
    thumbPath = ".thumbnails/" + assetStr + ".png";
  }

  // Load file into memory (stbi_load not available: renderer uses
  // STBI_NO_STDIO).
  FILE *fp = nullptr;
#ifdef _WIN32
  if (fopen_s(&fp, thumbPath.c_str(), "rb") != 0) {
    fp = nullptr;
  }
#else
  fp = std::fopen(thumbPath.c_str(), "rb");
#endif
  if (fp == nullptr) {
    return 0U;
  }
  std::fseek(fp, 0, SEEK_END);
  const long fileLen = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  if ((fileLen <= 0) ||
      (static_cast<unsigned long>(fileLen) >
       static_cast<unsigned long>(std::numeric_limits<int>::max()))) {
    std::fclose(fp);
    return 0U;
  }
  std::vector<unsigned char> fileData(static_cast<std::size_t>(fileLen));
  const std::size_t bytesRead =
      std::fread(fileData.data(), 1U, fileData.size(), fp);
  std::fclose(fp);
  if (bytesRead != fileData.size()) {
    return 0U;
  }

  int w = 0;
  int h = 0;
  int channels = 0;
  const int stbSize = static_cast<int>(fileData.size());
  unsigned char *pixels = stbi_load_from_memory(
      fileData.data(), stbSize, &w, &h, &channels, 4);
  if (pixels == nullptr) {
    return 0U;
  }

  // Routed through the renderer's RenderDevice (audit #206) instead of
  // calling glGenTextures/glTexImage2D directly: GL stays inside the
  // renderer implementation, and the editor only ever sees the opaque
  // texture id RenderDevice::create_texture_2d returns.
  std::uint32_t tex = 0U;
  const renderer::RenderDevice *device = renderer::render_device();
  if ((device != nullptr) && (device->create_texture_2d != nullptr)) {
    tex = device->create_texture_2d(w, h, 4, pixels);
  }
  stbi_image_free(pixels);

  if (tex != 0U) {
    auto &entry = editor_session().thumbnailCache[editor_session().thumbnailCount];
    std::snprintf(entry.path, sizeof(entry.path), "%s", assetPath);
    entry.textureId = tex;
    entry.width = w;
    entry.height = h;
    ++editor_session().thumbnailCount;
  }

  return tex;
}

/// Releases cached thumbnail textures owned by the editor through the
/// renderer's RenderDevice (audit #206).
void clear_thumbnail_cache() noexcept {
  const renderer::RenderDevice *device = renderer::render_device();
  for (std::size_t i = 0U; i < editor_session().thumbnailCount; ++i) {
    const std::uint32_t tex = editor_session().thumbnailCache[i].textureId;
    if ((tex != 0U) && (device != nullptr) &&
        (device->destroy_texture != nullptr)) {
      device->destroy_texture(tex);
    }
    editor_session().thumbnailCache[i] = ThumbnailEntry{};
  }
  editor_session().thumbnailCount = 0U;
}

namespace {

constexpr const char *kContentBrowserLogChannel = "editor.content_browser";
constexpr const char *kContentBrowserStateFileName =
    "editor_content_browser_state.json";
char g_contentBrowserDirectoryOverride[512] = {};

/// Resolves the content-browser state persistence directory: the test
/// override when set, otherwise the real per-user platform save directory.
bool resolve_content_browser_directory(char *out,
                                       std::size_t capacity) noexcept {
  if (g_contentBrowserDirectoryOverride[0] != '\0') {
    const int written =
        std::snprintf(out, capacity, "%s", g_contentBrowserDirectoryOverride);
    return (written > 0) && (static_cast<std::size_t>(written) < capacity);
  }
  return core::platform_get_save_dir(out, capacity);
}

bool build_content_browser_state_path(char *out,
                                      std::size_t capacity) noexcept {
  char directory[900] = {};
  if (!resolve_content_browser_directory(directory, sizeof(directory))) {
    return false;
  }
  const int written = std::snprintf(out, capacity, "%s/%s", directory,
                                    kContentBrowserStateFileName);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

/// Reads a whole small file into a fixed buffer; false on overflow or I/O
/// failure (mirrors editor_scene_document.cpp's recent-scenes reader).
bool read_whole_small_file(const char *path, char *out, std::size_t capacity,
                           std::size_t *outSize) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t readCount = std::fread(out, 1U, capacity - 1U, file);
  const bool overflow = std::fgetc(file) != EOF;
  const bool hitError = std::ferror(file) != 0;
  std::fclose(file);
  if (overflow || hitError) {
    return false;
  }
  out[readCount] = '\0';
  if (outSize != nullptr) {
    *outSize = readCount;
  }
  return true;
}

} // namespace

void content_browser_state_set_directory_override_for_tests(
    const char *directory) noexcept {
  std::snprintf(g_contentBrowserDirectoryOverride,
               sizeof(g_contentBrowserDirectoryOverride), "%s",
               (directory != nullptr) ? directory : "");
}

void content_browser_state_persist() noexcept {
  const ContentBrowserState &cb = editor_session().contentBrowser;

  char directory[900] = {};
  if (resolve_content_browser_directory(directory, sizeof(directory))) {
    std::error_code ec{};
    std::filesystem::create_directories(std::filesystem::path(directory), ec);
  }

  char path[1024] = {};
  if (!build_content_browser_state_path(path, sizeof(path))) {
    return;
  }

  core::JsonWriter writer{};
  writer.begin_object();
  writer.write_string("folder", cb.filter.folder);
  writer.write_uint("typeMask", cb.filter.typeMask);
  writer.end_object();
  if (writer.failed()) {
    core::log_message(core::LogLevel::Error, kContentBrowserLogChannel,
                      "failed to serialize content browser state");
    return;
  }

  if (!core::atomic_write_file(path, writer.result(), writer.result_size())) {
    core::log_message(core::LogLevel::Error, kContentBrowserLogChannel,
                      "failed to write content browser state");
  }
}

void content_browser_state_load_once() noexcept {
  ContentBrowserState &cb = editor_session().contentBrowser;
  if (cb.persistedStateLoaded) {
    return;
  }
  cb.persistedStateLoaded = true;

  char path[1024] = {};
  if (!build_content_browser_state_path(path, sizeof(path))) {
    return;
  }

  char buffer[2048] = {};
  std::size_t size = 0U;
  if (!read_whole_small_file(path, buffer, sizeof(buffer), &size)) {
    return;
  }

  core::JsonParser parser{};
  if (!parser.parse(buffer, size)) {
    return;
  }
  const core::JsonValue *root = parser.root();
  if ((root == nullptr) || (root->type != core::JsonValue::Type::Object)) {
    return;
  }

  char folder[kMaxAssetIndexPath] = {};
  const core::JsonValue *folderValue = parser.get_object_field(*root, "folder");
  if ((folderValue != nullptr) &&
      parser.copy_string(*folderValue, folder, sizeof(folder))) {
    std::snprintf(cb.filter.folder, sizeof(cb.filter.folder), "%s", folder);
  }

  const core::JsonValue *maskValue =
      parser.get_object_field(*root, "typeMask");
  if (maskValue != nullptr) {
    std::uint32_t mask = kAssetKindMaskAll;
    if (parser.as_uint(*maskValue, &mask) && (mask != 0U)) {
      cb.filter.typeMask = mask & kAssetKindMaskAll;
    }
  }
}

void content_browser_navigate(const char *folder) noexcept {
  EditorSession &session = editor_session();
  ContentBrowserState &cb = session.contentBrowser;
  ContentBrowserNavHistory &hist = cb.navHistory;
  if (hist.count == 0U) {
    hist.entries[0][0] = '\0';
    hist.count = 1U;
    hist.position = 0U;
  }

  const char *target = (folder != nullptr) ? folder : "";
  if (std::strcmp(hist.entries[hist.position], target) == 0) {
    return;
  }

  std::size_t newPosition = hist.position + 1U;
  if (newPosition >= ContentBrowserNavHistory::kMaxEntries) {
    // History is full: drop the oldest entry to make room for the new one.
    std::memmove(hist.entries[0], hist.entries[1],
                (ContentBrowserNavHistory::kMaxEntries - 1U) *
                    kMaxAssetIndexPath);
    newPosition = ContentBrowserNavHistory::kMaxEntries - 1U;
  }
  std::snprintf(hist.entries[newPosition], kMaxAssetIndexPath, "%s", target);
  hist.count = newPosition + 1U;
  hist.position = newPosition;

  std::snprintf(cb.filter.folder, sizeof(cb.filter.folder), "%s", target);
  cb.filter.flatSearch = false;
  content_browser_state_persist();
}

bool content_browser_can_go_back() noexcept {
  return editor_session().contentBrowser.navHistory.position > 0U;
}

bool content_browser_can_go_forward() noexcept {
  const ContentBrowserNavHistory &hist =
      editor_session().contentBrowser.navHistory;
  return (hist.position + 1U) < hist.count;
}

void content_browser_go_back() noexcept {
  if (!content_browser_can_go_back()) {
    return;
  }
  ContentBrowserState &cb = editor_session().contentBrowser;
  --cb.navHistory.position;
  std::snprintf(cb.filter.folder, sizeof(cb.filter.folder), "%s",
               cb.navHistory.entries[cb.navHistory.position]);
  cb.filter.flatSearch = false;
  content_browser_state_persist();
}

void content_browser_go_forward() noexcept {
  if (!content_browser_can_go_forward()) {
    return;
  }
  ContentBrowserState &cb = editor_session().contentBrowser;
  ++cb.navHistory.position;
  std::snprintf(cb.filter.folder, sizeof(cb.filter.folder), "%s",
               cb.navHistory.entries[cb.navHistory.position]);
  cb.filter.flatSearch = false;
  content_browser_state_persist();
}

bool world_is_editable() noexcept {
  return (editor_session().world != nullptr) && !editor_session().worldRestoreFailed &&
         (editor_session().playState == PlayState::Stopped) &&
         (editor_session().world->current_phase() == runtime::WorldPhase::Input);
}

bool world_can_load_scene() noexcept {
  return (editor_session().world != nullptr) &&
         (editor_session().world->current_phase() == runtime::WorldPhase::Input);
}

/// Returns whether the default scene file is available on disk.
bool default_scene_file_exists() noexcept {
  std::error_code ec{};
  return std::filesystem::is_regular_file(editor_scene_path(), ec) && !ec;
}

bool capture_play_snapshot() noexcept {
  if (editor_session().world == nullptr) {
    return false;
  }

  std::size_t capacity = editor_session().playSnapshotCapacity;
  if (capacity < core::JsonWriter::kBufferBytes) {
    capacity = core::JsonWriter::kBufferBytes;
  }

  const std::size_t estimatedCapacity =
      (editor_session().world->alive_entity_count() * 256U) + 4096U;
  if (capacity < estimatedCapacity) {
    capacity = estimatedCapacity;
  }

  for (std::size_t attempt = 0U; attempt < 6U; ++attempt) {
    std::unique_ptr<char[]> candidate(new (std::nothrow) char[capacity]);
    if (candidate == nullptr) {
      return false;
    }

    std::size_t snapshotSize = 0U;
    if (runtime::save_scene(*editor_session().world, candidate.get(), capacity,
                            &snapshotSize)) {
      editor_session().playSnapshotBuffer.swap(candidate);
      editor_session().playSnapshotCapacity = capacity;
      editor_session().playSnapshotSize = snapshotSize;
      editor_session().hasPlaySnapshot = true;
      editor_session().playSnapshotWorld = editor_session().world;
      return true;
    }

    if (capacity >= core::JsonWriter::kMaxBufferBytes) {
      break;
    }

    const std::size_t doubledCapacity = capacity * 2U;
    if ((doubledCapacity <= capacity) ||
        (doubledCapacity > core::JsonWriter::kMaxBufferBytes)) {
      capacity = core::JsonWriter::kMaxBufferBytes;
    } else {
      capacity = doubledCapacity;
    }
  }

  return false;
}

/// True when retained selection handles may still be dereferenced: the
/// world is attached and its content epoch matches the capture epoch.
static bool selection_epoch_valid() noexcept {
  const EditorSession &session = editor_session();
  return (session.world != nullptr) &&
         (session.selectionEpoch == session.world->content_epoch());
}

bool is_entity_selected(runtime::Entity entity) noexcept {
  if (!selection_epoch_valid()) {
    return false;
  }
  const EditorSession &session = editor_session();
  if (!session.world->is_alive(entity)) {
    return false;
  }
  for (std::size_t i = 0U; i < session.selectedEntityCount; ++i) {
    if (session.selectedEntities[i] == entity) {
      return true;
    }
  }
  return false;
}

void select_entity(runtime::Entity entity, bool additive) noexcept {
  EditorSession &session = editor_session();
  if (!selection_epoch_valid()) {
    clear_entity_selection();
  }
  if (session.world != nullptr) {
    session.selectionEpoch = session.world->content_epoch();
  }
  if (!additive) {
    session.selectedEntityCount = 0U;
  }
  if (additive && is_entity_selected(entity)) {
    std::size_t write = 0U;
    for (std::size_t i = 0U; i < session.selectedEntityCount; ++i) {
      if (session.selectedEntities[i] != entity) {
        session.selectedEntities[write++] = session.selectedEntities[i];
      }
    }
    session.selectedEntityCount = write;
    session.selectedEntity =
        (write > 0U) ? session.selectedEntities[write - 1U]
                     : runtime::kInvalidEntity;
    return;
  }
  if (session.selectedEntityCount < EditorSession::kMaxSelectedEntities) {
    session.selectedEntities[session.selectedEntityCount++] = entity;
  }
  session.selectedEntity = entity;
}

void clear_entity_selection() noexcept {
  editor_session().selectedEntityCount = 0U;
  editor_session().selectedEntity = runtime::kInvalidEntity;
}

runtime::Entity selected_entity() noexcept {
  EditorSession &session = editor_session();
  if (!selection_epoch_valid()) {
    clear_entity_selection();
    return runtime::kInvalidEntity;
  }
  if (session.selectedEntity == runtime::kInvalidEntity) {
    return runtime::kInvalidEntity;
  }
  if (!session.world->is_alive(session.selectedEntity)) {
    prune_entity_selection();
    return runtime::kInvalidEntity;
  }
  return session.selectedEntity;
}

void prune_entity_selection() noexcept {
  EditorSession &session = editor_session();
  if (!selection_epoch_valid()) {
    clear_entity_selection();
    return;
  }
  std::size_t write = 0U;
  for (std::size_t i = 0U; i < session.selectedEntityCount; ++i) {
    if (session.world->is_alive(session.selectedEntities[i])) {
      session.selectedEntities[write++] = session.selectedEntities[i];
    }
  }
  session.selectedEntityCount = write;
  if ((session.selectedEntity != runtime::kInvalidEntity) &&
      !session.world->is_alive(session.selectedEntity)) {
    session.selectedEntity = (write > 0U) ? session.selectedEntities[write - 1U]
                                          : runtime::kInvalidEntity;
  }
}

void editor_history_undo() noexcept {
  if (world_is_editable()) {
    editor_session().commandHistory.undo();
  }
}

void editor_history_redo() noexcept {
  if (world_is_editable()) {
    editor_session().commandHistory.redo();
  }
}

void start_play_mode() noexcept {
  if (editor_session().world == nullptr) {
    return;
  }

  if (editor_session().worldRestoreFailed) {
    core::log_message(core::LogLevel::Warning, "editor",
                      "play blocked: load scene to recover from restore error");
    return;
  }

  if (editor_session().playState == PlayState::Playing) {
    return;
  }

  if (editor_session().playState == PlayState::Stopped) {
    if (!capture_play_snapshot()) {
      core::log_message(core::LogLevel::Error, "editor",
                        "failed to capture pre-play scene snapshot");
      return;
    }
    // Fresh play session: a prior session's live-edit baselines/queued
    // apply-to-authored entries must never leak into this one.
    reset_live_edit_state();
  }

  editor_session().playState = PlayState::Playing;
  editor_session().stepRequested = false;
  // "Current session" in the Console's filter reads as "since I hit Play."
  console_capture_begin_session();
  core::log_message(core::LogLevel::Info, "editor", "play");
}

void pause_play_mode() noexcept {
  if ((editor_session().world == nullptr) || (editor_session().playState != PlayState::Playing)) {
    return;
  }

  editor_session().playState = PlayState::Paused;
  core::log_message(core::LogLevel::Info, "editor", "pause");
}

void stop_play_mode() noexcept {
  if ((editor_session().world == nullptr) || (editor_session().playState == PlayState::Stopped)) {
    return;
  }

  bool restored = true;

  if (!editor_session().hasPlaySnapshot || (editor_session().playSnapshotSize == 0U)) {
    core::log_message(core::LogLevel::Warning, "editor",
                      "stop requested without pre-play snapshot");
    restored = false;
  } else if (editor_session().playSnapshotWorld != editor_session().world) {
    core::log_message(core::LogLevel::Warning, "editor",
                      "play snapshot belongs to a different world; discarded");
    editor_session().hasPlaySnapshot = false;
    editor_session().playSnapshotSize = 0U;
    editor_session().playSnapshotWorld = nullptr;
    restored = false;
  } else if (!runtime::load_scene(*editor_session().world, editor_session().playSnapshotBuffer.get(),
                                  editor_session().playSnapshotSize)) {
    // Scene loading is transactional, so the live world is intact after a
    // failed restore; keep it and the snapshot for recovery/diagnostics
    // instead of destroying the only remaining copy of the user's work.
    core::log_message(core::LogLevel::Error, "editor",
                      "failed to restore pre-play scene snapshot; the play "
                      "world is preserved");
    clear_entity_selection();
    restored = false;
  } else {
    clear_entity_selection();
  }

  editor_session().playState = PlayState::Stopped;
  editor_session().stepRequested = false;
  editor_session().worldRestoreFailed = !restored;

  if (restored) {
    // Authored state is back; any "Apply to authored value" queued during
    // Play now replays as ordinary undoable edits against it (issue #159)
    // instead of the transient live-edit values that just got discarded
    // by the restore above. Also drops every live-edit baseline -- a new
    // Play session starts tracking fresh regardless.
    const std::size_t appliedCount = replay_pending_authored_applies();
    if (appliedCount > 0U) {
      char message[96] = {};
      std::snprintf(message, sizeof(message),
                    "applied %zu live edit(s) to the authored scene",
                    appliedCount);
      core::log_message(core::LogLevel::Info, "editor", message);
    }
  }

  core::log_message(core::LogLevel::Info, "editor", "stop");
}


} // namespace engine::editor
