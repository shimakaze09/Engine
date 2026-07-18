// Implements the private editor session state and session lifecycle
// (play snapshot/start/pause/stop, thumbnail cache, editability checks).
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_session.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#if __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL2 headers not found"
#endif

#if __has_include(<SDL_opengl.h>)
#include <SDL_opengl.h>
#elif __has_include(<SDL2/SDL_opengl.h>)
#include <SDL2/SDL_opengl.h>
#else
#error "SDL OpenGL headers not found"
#endif

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
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

#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/profiler.h"
#include "engine/core/reflect.h"
#include "engine/engine.h"
#include "engine/editor/editor_camera.h"
#include "engine/math/transform.h"
#include "engine/math/vec2.h"
#include "engine/math/vec4.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "ImGuizmo.h"

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
GLuint load_thumbnail_texture(const char *assetPath) noexcept {
  if (assetPath == nullptr) {
    return 0U;
  }

  // Check cache.
  for (std::size_t i = 0U; i < editor_session().thumbnailCount; ++i) {
    if (std::strcmp(editor_session().thumbnailCache[i].path, assetPath) == 0) {
      return editor_session().thumbnailCache[i].textureId;
    }
  }
  if (editor_session().thumbnailCount >= kMaxThumbnails) {
    return 0U;
  }

  // Build the .thumbnails/<basename>.png path.
  // Asset path example: "assets/triangle.mesh" →
  // "assets/.thumbnails/triangle.mesh.png"
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

  GLuint tex = 0U;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               pixels);
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

/// Releases cached thumbnail GL textures owned by the editor.
void clear_thumbnail_cache() noexcept {
  for (std::size_t i = 0U; i < editor_session().thumbnailCount; ++i) {
    if (editor_session().thumbnailCache[i].textureId != 0U) {
      const GLuint tex = editor_session().thumbnailCache[i].textureId;
      glDeleteTextures(1, &tex);
    }
    editor_session().thumbnailCache[i] = ThumbnailEntry{};
  }
  editor_session().thumbnailCount = 0U;
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
  }

  editor_session().playState = PlayState::Playing;
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
  } else if (!runtime::load_scene(*editor_session().world, editor_session().playSnapshotBuffer.get(),
                                  editor_session().playSnapshotSize)) {
    core::log_message(core::LogLevel::Error, "editor",
                      "failed to restore pre-play scene snapshot");
    runtime::reset_world(*editor_session().world);
    core::log_message(core::LogLevel::Warning, "editor",
                      "world reset to empty after restore failure");
    editor_session().selectedEntityIndex = 0U;
    // restored stays true: world is clean and usable, just empty
  } else {
    editor_session().selectedEntityIndex = 0U;
  }

  editor_session().playState = PlayState::Stopped;
  editor_session().worldRestoreFailed = !restored;

  core::log_message(core::LogLevel::Info, "editor", "stop");
}


} // namespace engine::editor
