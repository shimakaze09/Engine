// Regression for #321's fault-behavior change in editor_session.cpp: the
// content-browser state file is editor-settings data, and a stored file
// that exists but cannot round-trip through the loader (unreadable, or
// larger than the read buffer) must not be treated as a fresh profile —
// before this change, the next persist atomically committed defaults over
// the file the session had just failed to read. Absent stays the ordinary
// fresh-profile case and keeps persisting enabled.

#include "editor_session.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "../test_harness.h"

namespace {

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

constexpr const char *kStateFileName = "editor_content_browser_state.json";

/// Writes `content` to the path; false on any short write. The open is
/// guarded per CRT: the Windows lanes build with /W4 /WX, where a bare
/// fopen is a deprecation error.
bool write_file(const std::filesystem::path &path,
                const std::string &content) {
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

/// Reads the file's byte size; SIZE_MAX when absent/unreadable.
std::size_t stored_size(const std::filesystem::path &path) {
  std::error_code ec{};
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  return ec ? static_cast<std::size_t>(-1) : static_cast<std::size_t>(size);
}

/// Points persistence at `directory` and re-arms the load-once guard so the
/// next content_browser_state_load_once call re-reads from it.
void rebind_state_directory(const std::filesystem::path &directory) {
  engine::editor::content_browser_state_set_directory_override_for_tests(
      directory.string().c_str());
  engine::editor::editor_session().contentBrowser.persistedStateLoaded = false;
}

} // namespace

/// Runs this executable or test program.
int main() {
  namespace fs = std::filesystem;
  using namespace engine::editor;

  const fs::path root =
      fs::temp_directory_path() / "engine_cb_state_test";
  std::error_code ec{};
  fs::remove_all(root, ec);
  if (!fs::create_directories(root, ec) || ec) {
    std::fprintf(stderr, "FAIL: could not create test directory\n");
    return 1;
  }

  // Fresh profile: no stored file, so load adopts defaults and persisting
  // stays enabled — a first session must still be able to store its state.
  const fs::path freshDir = root / "fresh";
  check(fs::create_directories(freshDir, ec) && !ec, "create fresh dir");
  rebind_state_directory(freshDir);
  content_browser_state_load_once();
  std::snprintf(editor_session().contentBrowser.filter.folder,
                sizeof(editor_session().contentBrowser.filter.folder), "%s",
                "props");
  content_browser_state_persist();
  const fs::path freshFile = freshDir / kStateFileName;
  check(fs::exists(freshFile, ec) && !ec,
        "fresh profile persists a state file");

  // Round trip: a later session in the same directory restores the folder.
  rebind_state_directory(freshDir);
  editor_session().contentBrowser.filter = {};
  content_browser_state_load_once();
  check(std::strcmp(editor_session().contentBrowser.filter.folder,
                    "props") == 0,
        "stored folder restored");

  // The fault case: a stored state file larger than the loader's fixed
  // buffer cannot round-trip, so the session must adopt defaults WITHOUT
  // ever committing them over the stored file. The oversized file stands in
  // for every fault class the reader reports (Unreadable takes the same
  // latch path) because it needs no permission tricks and holds under any
  // uid.
  const fs::path faultDir = root / "fault";
  check(fs::create_directories(faultDir, ec) && !ec, "create fault dir");
  const fs::path faultFile = faultDir / kStateFileName;
  const std::string oversized(4096U, 'x');
  check(write_file(faultFile, oversized), "write oversized state fixture");
  rebind_state_directory(faultDir);
  editor_session().contentBrowser.filter = {};
  content_browser_state_load_once();

  std::snprintf(editor_session().contentBrowser.filter.folder,
                sizeof(editor_session().contentBrowser.filter.folder), "%s",
                "sounds");
  content_browser_state_persist();
  check(stored_size(faultFile) == oversized.size(),
        "persist after a failed load leaves the stored bytes untouched");

  // The refusal holds for the whole session, not just the first persist.
  content_browser_state_persist();
  check(stored_size(faultFile) == oversized.size(),
        "repeat persist still leaves the stored bytes untouched");

  // A rebind to a healthy location clears the latch: the fault belonged to
  // the unreadable file, not to the session.
  const fs::path recoveryDir = root / "recovery";
  check(fs::create_directories(recoveryDir, ec) && !ec, "create recovery dir");
  rebind_state_directory(recoveryDir);
  editor_session().contentBrowser.filter = {};
  content_browser_state_load_once();
  content_browser_state_persist();
  check(fs::exists(recoveryDir / kStateFileName, ec) && !ec,
        "persisting resumes at a healthy location");

  content_browser_state_set_directory_override_for_tests(nullptr);
  fs::remove_all(root, ec);
  return g_tests.finish("editor content browser state tests");
}
