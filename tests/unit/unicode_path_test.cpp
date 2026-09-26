// Verifies that every engine file entry point opens a path holding
// characters outside ASCII: a CJK and accented directory and file, as a
// Windows profile, project folder or asset name commonly carries (#692).
// The files are created, and checked for, through std::filesystem with
// char8_t paths, which reach the operating system in the right encoding on
// every platform; the engine's char paths are the same text as UTF-8. On
// Windows those paths used to reach the ANSI file APIs in the active code
// page, so the engine named a different file than the one on disk.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "engine/core/atomic_file.h"
#include "engine/core/file_read.h"
#include "engine/core/vfs.h"
#include "engine/runtime/save_data.h"

#include "../test_harness.h"

namespace {

engine::tests::TestContext g_tests{};

/// The root the case works in, under the current directory, with a name
/// no code page other than UTF-8 spells: Chinese, Japanese and a Latin
/// accent in one component.
constexpr const char8_t *kRootName = u8"unicode_path_测试_ひらがな_ü";
constexpr const char8_t *kFileName = u8"地图_é.txt";
constexpr const char *kContent = "unicode path round trip";

std::string utf8(const std::u8string &text) {
  return std::string(text.begin(), text.end());
}

std::filesystem::path root_path() {
  return std::filesystem::current_path() / std::filesystem::path(kRootName);
}

std::string root_utf8() { return utf8(root_path().u8string()); }

bool write_with_filesystem(const std::filesystem::path &path) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << kContent;
  return static_cast<bool>(stream);
}

std::string read_with_filesystem(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
}

/// A file the operating system holds under its Unicode name reads back
/// through the engine's path-taking reader.
void check_read_whole_file() {
  const std::filesystem::path file = root_path() / kFileName;
  g_tests.check(write_with_filesystem(file), "plant a Unicode-named file");
  char buffer[128] = {};
  std::size_t size = 0U;
  const std::string path = utf8(file.u8string());
  const engine::core::FileReadResult result = engine::core::read_whole_file(
      path.c_str(), buffer, sizeof(buffer), &size);
  g_tests.check(result == engine::core::FileReadResult::Ok,
                "read_whole_file opens the Unicode-named file");
  g_tests.check((size == std::strlen(kContent)) &&
                    (std::strcmp(buffer, kContent) == 0),
                "read_whole_file returns its bytes");
}

/// A Unicode-named directory mounts, and its file is found and read.
void check_vfs() {
  g_tests.check(engine::core::initialize_vfs(), "initialize the VFS");
  g_tests.check(engine::core::mount("uni", root_utf8().c_str()),
                "mount a Unicode-named directory");
  const std::string virtualPath = std::string("uni/") + utf8(kFileName);
  g_tests.check(engine::core::vfs_file_exists(virtualPath.c_str()),
                "the VFS finds the Unicode-named file");
  char *text = nullptr;
  std::size_t size = 0U;
  const engine::core::Status read =
      engine::core::vfs_read_text(virtualPath.c_str(), &text, &size);
  g_tests.check(read.succeeded() && (text != nullptr) &&
                    (std::strcmp(text, kContent) == 0),
                "the VFS reads the Unicode-named file");
  engine::core::vfs_free(text);
  engine::core::shutdown_vfs();
}

/// What the engine writes lands under the Unicode name, not a mangled one.
void check_atomic_write_and_directories() {
  const std::filesystem::path directory =
      root_path() / std::filesystem::path(u8"子目录");
  g_tests.check(engine::core::create_directories_durably(
                    utf8(directory.u8string()).c_str()),
                "create a Unicode-named directory");
  std::error_code ec{};
  g_tests.check(std::filesystem::is_directory(directory, ec),
                "the directory exists under its Unicode name");

  const std::filesystem::path file =
      directory / std::filesystem::path(u8"保存.bin");
  g_tests.check(engine::core::atomic_write_file(utf8(file.u8string()).c_str(),
                                                kContent,
                                                std::strlen(kContent)),
                "atomic_write_file writes a Unicode-named file");
  g_tests.check(read_with_filesystem(file) == kContent,
                "the written file exists under its Unicode name");
}

/// A save directory under a Unicode path saves and loads.
void check_save_data() {
  const std::filesystem::path directory =
      root_path() / std::filesystem::path(u8"存档");
  const std::string directoryUtf8 = utf8(directory.u8string());
  const char *json = "{\"coins\":8}";
  g_tests.check(engine::runtime::save_game_data_to(directoryUtf8.c_str(), json,
                                                   std::strlen(json)),
                "save into a Unicode-named directory");
  char buffer[64] = {};
  std::size_t length = 0U;
  g_tests.check(engine::runtime::load_game_data_from(
                    directoryUtf8.c_str(), buffer, sizeof(buffer), &length) &&
                    (std::strcmp(buffer, json) == 0),
                "load it back");
  g_tests.check(read_with_filesystem(directory / "save.json") == json,
                "the save exists under its Unicode name");
}

} // namespace

int main() {
  std::error_code ec{};
  std::filesystem::remove_all(root_path(), ec);
  if (!std::filesystem::create_directories(root_path(), ec)) {
    g_tests.fail("create the Unicode-named root");
    return g_tests.finish("unicode path tests");
  }

  check_read_whole_file();
  check_vfs();
  check_atomic_write_and_directories();
  check_save_data();

  std::filesystem::remove_all(root_path(), ec);
  return g_tests.finish("unicode path tests");
}
