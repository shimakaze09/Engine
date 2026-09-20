// The recent-scenes guard every scene-document suite runs inside. Opening
// or saving a scene adds it to the persisted Recent Scenes list, which
// lives in the developer's real per-user save directory unless the
// test override points elsewhere; a suite that forgets the override fills
// the developer's editor menu with paths to deleted test files. The guard
// routes persistence to a scratch directory before any document
// operation and proves the real file's bytes are what they were when the
// suite ends.

#pragma once

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "editor_scene_document.h"
#include "engine/core/platform.h"

namespace engine::tests {

/// Arms the recent-scenes override for one suite and checks the real
/// file on disarm. Cases that reset the override for their own purposes
/// call rearm() before they return.
class RecentScenesGuard final {
public:
  /// Remembers the real recent-scenes file's bytes and routes persistence
  /// to `scratchDirectory`, creating it. False when the scratch directory
  /// cannot be created or the real save directory is unknown.
  bool arm(const char *scratchDirectory) noexcept {
    char saveDir[900] = {};
    if (!engine::core::platform_get_save_dir(saveDir, sizeof(saveDir))) {
      return false;
    }
    m_realPath = std::string(saveDir) + "/editor_recent_scenes.json";
    m_realExisted = read_bytes(m_realPath, &m_realBytes);
    std::error_code ec{};
    std::filesystem::create_directories(
        std::filesystem::path(scratchDirectory), ec);
    if (ec) {
      return false;
    }
    m_scratch = scratchDirectory;
    rearm();
    return true;
  }

  /// Points persistence back at the scratch directory (drops the cached
  /// list, as the production override does).
  void rearm() const noexcept {
    engine::editor::recent_scenes_set_directory_override_for_tests(
        m_scratch.c_str());
  }

  /// Clears the override. True when the real file exists exactly as it did
  /// at arm (or still does not exist); false, with a line on stderr, when
  /// the suite wrote it.
  bool disarm() noexcept {
    engine::editor::recent_scenes_set_directory_override_for_tests("");
    std::string bytesNow;
    const bool existsNow = read_bytes(m_realPath, &bytesNow);
    const bool untouched =
        (existsNow == m_realExisted) && (bytesNow == m_realBytes);
    if (!untouched) {
      std::fprintf(stderr,
                   "FAIL: the suite changed the real recent-scenes file %s\n",
                   m_realPath.c_str());
    }
    return untouched;
  }

private:
  static bool read_bytes(const std::string &path, std::string *out) noexcept {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
      out->clear();
      return false;
    }
    out->assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    return true;
  }

  std::string m_scratch;
  std::string m_realPath;
  std::string m_realBytes;
  bool m_realExisted = false;
};

} // namespace engine::tests
