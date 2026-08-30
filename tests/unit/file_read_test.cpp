// Verifies core::read_whole_file's outcome contract (issue #321): Ok versus
// the ordinary Absent case versus the two fault classes (Unreadable,
// TooLarge) that persisting callers must never mistake for "no stored file".

#include "engine/core/file_read.h"
#include "../test_harness.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

/// Writes `size` bytes of 'a' to the path; false on any short write.
bool write_bytes(const std::filesystem::path &path, std::size_t size) {
  std::FILE *file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  bool ok = true;
  for (std::size_t i = 0U; i < size; ++i) {
    if (std::fputc('a', file) == EOF) {
      ok = false;
      break;
    }
  }
  std::fclose(file);
  return ok;
}

} // namespace

/// Runs this executable or test program.
int main() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "engine_file_read_test";
  std::error_code ec{};
  fs::remove_all(root, ec);
  if (!fs::create_directories(root, ec) || ec) {
    std::fprintf(stderr, "FAIL: could not create test directory\n");
    return 1;
  }

  using engine::core::FileReadResult;
  using engine::core::read_whole_file;

  char buffer[16] = {};
  std::size_t size = 0U;

  // Ok: content, reported size, and NUL termination.
  const fs::path okPath = root / "ok.txt";
  check(write_bytes(okPath, 5U), "write ok fixture");
  check(read_whole_file(okPath.string().c_str(), buffer, sizeof(buffer),
                        &size) == FileReadResult::Ok,
        "short file reads Ok");
  check(size == 5U, "reported size matches content");
  check(std::strcmp(buffer, "aaaaa") == 0, "content NUL-terminated");
  check(read_whole_file(okPath.string().c_str(), buffer, sizeof(buffer),
                        nullptr) == FileReadResult::Ok,
        "null outSize tolerated");

  // Ok boundary: exactly capacity - 1 bytes still fits...
  const fs::path fitPath = root / "fit.txt";
  check(write_bytes(fitPath, sizeof(buffer) - 1U), "write fit fixture");
  check(read_whole_file(fitPath.string().c_str(), buffer, sizeof(buffer),
                        &size) == FileReadResult::Ok,
        "capacity-1 bytes read Ok");
  check(size == sizeof(buffer) - 1U, "boundary size reported");

  // ...and one more byte is TooLarge, with the buffer's stored bytes not
  // reported as a shorter successful read.
  const fs::path bigPath = root / "big.txt";
  check(write_bytes(bigPath, sizeof(buffer)), "write big fixture");
  check(read_whole_file(bigPath.string().c_str(), buffer, sizeof(buffer),
                        &size) == FileReadResult::TooLarge,
        "capacity bytes read TooLarge");

  // Empty file is Ok with size zero — the fresh-but-touched profile case.
  const fs::path emptyPath = root / "empty.txt";
  check(write_bytes(emptyPath, 0U), "write empty fixture");
  size = 99U;
  check(read_whole_file(emptyPath.string().c_str(), buffer, sizeof(buffer),
                        &size) == FileReadResult::Ok,
        "empty file reads Ok");
  check(size == 0U, "empty file reports size zero");

  // Absent: ENOENT and nothing else.
  const fs::path missingPath = root / "missing.txt";
  check(read_whole_file(missingPath.string().c_str(), buffer, sizeof(buffer),
                        &size) == FileReadResult::Absent,
        "missing file reads Absent");

  // A directory at the path is a fault, not an absence: POSIX opens it and
  // fails the read (EISDIR), Windows refuses the open with a non-ENOENT
  // error — Unreadable either way, and unlike permission bits this
  // injection holds under any uid.
  const fs::path dirPath = root / "actually_a_directory";
  check(fs::create_directory(dirPath, ec) && !ec, "create directory fixture");
  check(read_whole_file(dirPath.string().c_str(), buffer, sizeof(buffer),
                        &size) == FileReadResult::Unreadable,
        "directory at the path reads Unreadable");

  // Invalid arguments are faults, never absences.
  check(read_whole_file(nullptr, buffer, sizeof(buffer), &size) ==
            FileReadResult::Unreadable,
        "null path is Unreadable");
  check(read_whole_file(okPath.string().c_str(), nullptr, sizeof(buffer),
                        &size) == FileReadResult::Unreadable,
        "null buffer is Unreadable");
  check(read_whole_file(okPath.string().c_str(), buffer, 0U, &size) ==
            FileReadResult::Unreadable,
        "zero capacity is Unreadable");

  // capacity == 1 holds only a terminator: an empty file fits, one byte
  // does not.
  check(read_whole_file(emptyPath.string().c_str(), buffer, 1U, &size) ==
            FileReadResult::Ok,
        "empty file fits capacity one");
  check(read_whole_file(okPath.string().c_str(), buffer, 1U, &size) ==
            FileReadResult::TooLarge,
        "nonempty file overflows capacity one");

  fs::remove_all(root, ec);
  return g_tests.finish("core file_read tests");
}
