// Regression for #321's fault-behavior change in editor_session.cpp: the
// content-browser state file is editor-settings data, and a stored file
// that exists but cannot round-trip through the loader (unreadable, or
// larger than the read buffer) must not be treated as a fresh profile —
// before this change, the next persist atomically committed defaults over
// the file the session had just failed to read. Absent stays the ordinary
// fresh-profile case and keeps persisting enabled. A stored key this build
// does not read is named in the log. The state belongs to the open project:
// its folder is stored relative to the asset root and restored only while
// it is still a folder inside it, and a second project opened after the
// first starts at its own root rather than at the first one's folder.

#include "editor_asset_index.h"
#include "editor_session.h"

#include "engine/core/logging.h"
#include "engine/core/project_data.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
bool write_file(const std::filesystem::path &path, const std::string &content) {
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

int g_unreadKeyWarnings = 0;

/// Counts the Warnings that name the stored state's unread `futureKey`.
void note_unread_key(engine::core::LogLevel level, const char * /*channel*/,
                     const char *message, void * /*userData*/) noexcept {
  if ((level == engine::core::LogLevel::Warning) && (message != nullptr) &&
      (std::strstr(message, "'futureKey' is not read by this build") !=
       nullptr)) {
    ++g_unreadKeyWarnings;
  }
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

/// The folder the browser lists, as the asset index spells it.
std::string listed_folder() {
  return engine::editor::editor_session().contentBrowser.filter.folder;
}

/// Loads the state stored in `directory` into a fresh browser and returns
/// the folder it opens at.
std::string restore_from(const std::filesystem::path &directory,
                         const std::string &content) {
  if (!write_file(directory / kStateFileName, content)) {
    return "<unwritten>";
  }
  rebind_state_directory(directory);
  engine::editor::editor_session().contentBrowser.filter = {};
  engine::editor::content_browser_state_load_once();
  return listed_folder();
}

/// Project A browses a subfolder; project B, opened next in the same
/// editor, starts at its own root. Each keeps its state in its own
/// per-user project directory, through the production path (no override).
void check_projects_keep_their_own_folder(const std::filesystem::path &root) {
  namespace fs = std::filesystem;
  using namespace engine::editor;
  std::error_code ec{};
  const fs::path projectA = root / "project_a";
  const fs::path projectB = root / "project_b";
  check(fs::create_directories(projectA, ec) && !ec &&
            fs::create_directories(projectB, ec) && !ec,
        "create the two project roots");
  content_browser_state_set_directory_override_for_tests("");

  check(engine::core::set_project_data_root(projectA.string().c_str()),
        "project A is open");
  content_browser_state_reset();
  content_browser_state_load_once();
  content_browser_navigate("assets/props");
  char fileA[1024] = {};
  check(engine::core::project_data_dir(fileA, sizeof(fileA)),
        "project A has a data directory");

  check(engine::core::set_project_data_root(projectB.string().c_str()),
        "project B is open");
  content_browser_state_reset();
  content_browser_state_load_once();
  check(listed_folder().empty(),
        "project B starts at its root, not at project A's folder");

  check(engine::core::set_project_data_root(projectA.string().c_str()),
        "project A is open again");
  content_browser_state_reset();
  content_browser_state_load_once();
  check(listed_folder() == "assets/props", "project A reopens its folder");

  content_browser_state_reset();
  fs::remove(fs::path(fileA) / kStateFileName, ec);
  check(engine::core::set_project_data_root(projectB.string().c_str()),
        "project B is open to clean up");
  char fileB[1024] = {};
  if (engine::core::project_data_dir(fileB, sizeof(fileB))) {
    fs::remove(fs::path(fileB) / kStateFileName, ec);
  }
  engine::core::clear_project_data_root();
}

/// Runs this executable or test program.
int main() {
  namespace fs = std::filesystem;
  using namespace engine::editor;

  const fs::path root = fs::temp_directory_path() / "engine_cb_state_test";
  std::error_code ec{};
  fs::remove_all(root, ec);
  if (!fs::create_directories(root, ec) || ec) {
    std::fprintf(stderr, "FAIL: could not create test directory\n");
    return 1;
  }
  // The browser's asset root is the relative "assets" the editor defaults
  // to, so the test works inside its own directory.
  const fs::path previousDirectory = fs::current_path(ec);
  fs::current_path(root, ec);
  check(!ec && fs::create_directories("assets/props", ec) && !ec &&
            fs::create_directories("assets/sounds", ec) && !ec,
        "create the asset folders");

  // Fresh profile: no stored file, so load adopts defaults and persisting
  // stays enabled — a first session must still be able to store its state.
  const fs::path freshDir = root / "fresh";
  check(fs::create_directories(freshDir, ec) && !ec, "create fresh dir");
  rebind_state_directory(freshDir);
  content_browser_state_load_once();
  std::snprintf(editor_session().contentBrowser.filter.folder,
                sizeof(editor_session().contentBrowser.filter.folder), "%s",
                "assets/props");
  content_browser_state_persist();
  const fs::path freshFile = freshDir / kStateFileName;
  check(fs::exists(freshFile, ec) && !ec,
        "fresh profile persists a state file");

  // Round trip: a later session in the same directory restores the folder.
  rebind_state_directory(freshDir);
  editor_session().contentBrowser.filter = {};
  content_browser_state_load_once();
  check(listed_folder() == "assets/props", "stored folder restored");

  // The folder is stored below the asset root, never as an OS path.
  {
    std::ifstream in(freshFile);
    std::string stored((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    check(stored.find("\"folder\": \"props\"") != std::string::npos ||
              stored.find("\"folder\":\"props\"") != std::string::npos,
          "the folder is stored relative to the asset root");
  }

  // A stored folder is restored only while it is a folder inside the
  // asset root; otherwise the browser starts at the root.
  const fs::path checkedDir = root / "checked";
  check(fs::create_directories(checkedDir, ec) && !ec, "create checked dir");
  check(restore_from(checkedDir, "{\"folder\":\"sounds\"}") == "assets/sounds",
        "a folder inside the root is restored");
  check(restore_from(checkedDir, "{\"folder\":\"gone\"}").empty(),
        "a folder that no longer exists starts at the root");
  check(restore_from(checkedDir, "{\"folder\":\"../props\"}").empty(),
        "a folder outside the root starts at the root");
  check(restore_from(checkedDir, "{\"folder\":\"/tmp\"}").empty(),
        "an absolute folder starts at the root");

  // A mask written before the type count was recorded covered nine types;
  // the types added since must come back visible, and a hidden legacy bit
  // must stay hidden.
  const fs::path legacyDir = root / "legacy";
  check(fs::create_directories(legacyDir, ec) && !ec, "create legacy dir");
  check(write_file(legacyDir / kStateFileName,
                   "{\"folder\":\"props\",\"typeMask\":510}"),
        "write legacy state fixture");
  rebind_state_directory(legacyDir);
  editor_session().contentBrowser.filter = {};
  content_browser_state_load_once();
  check(editor_session().contentBrowser.filter.typeMask ==
            (kAssetKindMaskAll & ~1U),
        "a legacy mask keeps its hidden type and shows every newer type");

  // A key this build does not read is named, since the next save drops it.
  const fs::path newerDir = root / "newer";
  check(fs::create_directories(newerDir, ec) && !ec, "create newer dir");
  check(write_file(newerDir / kStateFileName,
                   "{\"folder\":\"props\",\"futureKey\":1}"),
        "write a state with a key this build does not read");
  const bool sinkRegistered =
      engine::core::initialize_logging() &&
      engine::core::log_register_sink(&note_unread_key, nullptr);
  rebind_state_directory(newerDir);
  editor_session().contentBrowser.filter = {};
  content_browser_state_load_once();
  check(sinkRegistered && (listed_folder() == "assets/props") &&
            (g_unreadKeyWarnings == 1),
        "the state loads, and the unread key is named once");
  engine::core::log_unregister_sink(&note_unread_key, nullptr);
  engine::core::shutdown_logging();

  // A mask written with the current width is taken as it is.
  const fs::path currentDir = root / "current";
  check(fs::create_directories(currentDir, ec) && !ec, "create current dir");
  {
    char stored[128] = {};
    std::snprintf(stored, sizeof(stored),
                  "{\"folder\":\"\",\"typeMask\":%u,\"typeMaskKinds\":%u}",
                  static_cast<unsigned>(kAssetKindMaskAll & ~2U),
                  static_cast<unsigned>(engine::content::kAssetTypeCount));
    check(write_file(currentDir / kStateFileName, stored),
          "write current-width state fixture");
  }
  rebind_state_directory(currentDir);
  editor_session().contentBrowser.filter = {};
  content_browser_state_load_once();
  check(editor_session().contentBrowser.filter.typeMask ==
            (kAssetKindMaskAll & ~2U),
        "a current-width mask is restored exactly");

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

  check_projects_keep_their_own_folder(root);

  content_browser_state_set_directory_override_for_tests(nullptr);
  fs::current_path(previousDirectory, ec);
  fs::remove_all(root, ec);
  return g_tests.finish("editor content browser state tests");
}
