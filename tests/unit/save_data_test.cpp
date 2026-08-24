// Verifies the single-slot game save over explicit directories: byte-exact
// roundtrip, recursive directory creation, missing-file and oversized
// rejections, the capacity-overflow guard on load, the read-capacity
// boundaries, and that a failed read is reported as a load failure rather
// than as a successful load of truncated data.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "engine/core/platform.h"
#include "engine/runtime/save_data.h"

namespace {

/// Builds a scratch directory path under the OS temp dir.
bool make_scratch_dir(char *out, std::size_t capacity) {
  char tempDir[512] = {};
  if (!engine::core::platform_get_temp_dir(tempDir, sizeof(tempDir))) {
    return false;
  }
  const int written = std::snprintf(
      out, capacity, "%s/engine_save_test/nested", tempDir);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

/// Builds "<directory>/save.json", the slot path the production loader
/// derives, so tests can plant a fault at that exact location.
void make_slot_path(const char *directory, char *out, std::size_t capacity) {
  std::snprintf(out, capacity, "%s/save.json", directory);
}

/// Writes `bytes` straight into the slot file, creating the directory
/// first; used for on-disk states the production writer cannot produce.
bool write_slot_bytes(const char *directory, const char *bytes,
                      std::size_t length) {
  std::error_code ec{};
  std::filesystem::create_directories(std::filesystem::path(directory), ec);
  if (ec) {
    return false;
  }
  char path[640] = {};
  make_slot_path(directory, path, sizeof(path));
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const bool written =
      (length == 0U) || (std::fwrite(bytes, 1U, length, file) == length);
  return (std::fclose(file) == 0) && written;
}

/// Removes the scratch save file, whether it is the regular slot file or
/// a directory planted in its place (directories may remain; empty).
void cleanup(const char *directory) {
  char path[640] = {};
  make_slot_path(directory, path, sizeof(path));
  static_cast<void>(std::remove(path));
  std::error_code ec{};
  static_cast<void>(std::filesystem::remove_all(std::filesystem::path(path),
                                                ec));
}

/// EXPECTATION: a save into a not-yet-existing nested directory succeeds
/// (recursive creation) and loads back byte-exact with the exact length.
int check_roundtrip_with_directory_creation() {
  char directory[576] = {};
  if (!make_scratch_dir(directory, sizeof(directory))) {
    std::puts("temp dir unavailable");
    return 1;
  }
  cleanup(directory);

  const char *json = "{\"entries\":[{\"k\":\"best_time\",\"v\":12.5}]}";
  const std::size_t length = std::strlen(json);
  if (!engine::runtime::save_game_data_to(directory, json, length)) {
    std::puts("save failed");
    return 1;
  }

  char loaded[512] = {};
  std::size_t loadedLength = 0U;
  if (!engine::runtime::load_game_data_from(directory, loaded, sizeof(loaded),
                                            &loadedLength)) {
    std::puts("load failed");
    cleanup(directory);
    return 1;
  }
  if ((loadedLength != length) || (std::strcmp(loaded, json) != 0)) {
    std::puts("roundtrip mismatch");
    cleanup(directory);
    return 1;
  }

  // A rewrite replaces the slot (single-slot semantics).
  const char *second = "{\"entries\":[]}";
  if (!engine::runtime::save_game_data_to(directory, second,
                                          std::strlen(second)) ||
      !engine::runtime::load_game_data_from(directory, loaded, sizeof(loaded),
                                            &loadedLength) ||
      (std::strcmp(loaded, second) != 0)) {
    std::puts("rewrite mismatch");
    cleanup(directory);
    return 1;
  }

  cleanup(directory);
  return 0;
}

/// EXPECTATION: loading a missing slot fails; oversized saves are
/// rejected; a load buffer smaller than the file fails instead of
/// truncating silently.
int check_failure_paths() {
  char directory[576] = {};
  if (!make_scratch_dir(directory, sizeof(directory))) {
    return 1;
  }
  cleanup(directory);

  char loaded[64] = {};
  std::size_t loadedLength = 0U;
  if (engine::runtime::load_game_data_from(directory, loaded, sizeof(loaded),
                                           &loadedLength)) {
    std::puts("missing slot loaded");
    return 1;
  }

  const std::string oversized(engine::runtime::kMaxSaveDataBytes + 1U, 'x');
  if (engine::runtime::save_game_data_to(directory, oversized.c_str(),
                                         oversized.size())) {
    std::puts("oversized save accepted");
    return 1;
  }

  const char *json = "{\"entries\":[{\"k\":\"a\",\"v\":1}]}";
  if (!engine::runtime::save_game_data_to(directory, json,
                                          std::strlen(json))) {
    std::puts("small save failed");
    return 1;
  }
  char tiny[8] = {};
  if (engine::runtime::load_game_data_from(directory, tiny, sizeof(tiny),
                                           &loadedLength)) {
    std::puts("overflowing load succeeded");
    cleanup(directory);
    return 1;
  }

  cleanup(directory);
  return 0;
}

/// EXPECTATION: the read-capacity boundaries are exact — an empty slot
/// loads as a zero-length document, a file of exactly capacity - 1 bytes
/// loads whole, and one byte more is rejected instead of truncated.
int check_read_capacity_boundaries() {
  char directory[576] = {};
  if (!make_scratch_dir(directory, sizeof(directory))) {
    std::puts("temp dir unavailable");
    return 1;
  }
  cleanup(directory);

  char loaded[32] = {};
  std::size_t loadedLength = 1U;
  // core::atomic_write_file refuses a zero-byte payload, so the empty
  // slot is planted directly: it is the on-disk state the loader must
  // keep reporting as an empty document rather than as a read failure.
  if (!write_slot_bytes(directory, nullptr, 0U)) {
    std::puts("empty slot plant failed");
    return 1;
  }
  if (!engine::runtime::load_game_data_from(directory, loaded, sizeof(loaded),
                                            &loadedLength)) {
    std::puts("empty slot rejected");
    cleanup(directory);
    return 1;
  }
  if ((loadedLength != 0U) || (loaded[0] != '\0')) {
    std::puts("empty slot mislength");
    cleanup(directory);
    return 1;
  }

  const std::string exact(sizeof(loaded) - 1U, 'a');
  if (!engine::runtime::save_game_data_to(directory, exact.c_str(),
                                          exact.size())) {
    std::puts("exact-capacity save failed");
    cleanup(directory);
    return 1;
  }
  if (!engine::runtime::load_game_data_from(directory, loaded, sizeof(loaded),
                                            &loadedLength)) {
    std::puts("exact-capacity load rejected");
    cleanup(directory);
    return 1;
  }
  if ((loadedLength != exact.size()) ||
      (std::strcmp(loaded, exact.c_str()) != 0)) {
    std::puts("exact-capacity mismatch");
    cleanup(directory);
    return 1;
  }

  const std::string oneOver(sizeof(loaded), 'a');
  if (!engine::runtime::save_game_data_to(directory, oneOver.c_str(),
                                          oneOver.size())) {
    std::puts("over-capacity save failed");
    cleanup(directory);
    return 1;
  }
  loadedLength = 1U;
  if (engine::runtime::load_game_data_from(directory, loaded, sizeof(loaded),
                                           &loadedLength)) {
    std::puts("over-capacity load succeeded");
    cleanup(directory);
    return 1;
  }
  if (loadedLength != 0U) {
    std::puts("over-capacity load reported a length");
    cleanup(directory);
    return 1;
  }

  cleanup(directory);
  return 0;
}

/// Reports whether reading `path` fails with the stream error flag set,
/// which is what decides whether this platform actually exercises the
/// loader's read-error branch.
bool read_error_is_injectable(const char *path) {
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
  char probe[4] = {};
  const std::size_t probed = std::fread(probe, 1U, sizeof(probe), file);
  const bool errored = (probed == 0U) && (std::ferror(file) != 0);
  std::fclose(file);
  return errored;
}

/// EXPECTATION: an I/O error at the read boundary is reported as a load
/// failure, never as a successful load of the bytes read so far. The
/// fault is injected into the production path by planting a directory
/// where the slot file belongs: a POSIX CRT opens it and fails the read
/// with EISDIR, setting the stream's error flag while leaving its EOF
/// flag clear — the same stream state a mid-read device error produces,
/// and the state an fgetc-only overflow check cannot distinguish from a
/// complete short file.
int check_read_error_is_not_a_successful_load() {
  char directory[576] = {};
  if (!make_scratch_dir(directory, sizeof(directory))) {
    std::puts("temp dir unavailable");
    return 1;
  }
  cleanup(directory);

  char slotPath[640] = {};
  make_slot_path(directory, slotPath, sizeof(slotPath));
  std::error_code ec{};
  std::filesystem::create_directories(std::filesystem::path(slotPath), ec);
  if (ec) {
    std::puts("read-fault injection unavailable: cannot plant the slot");
    return 1;
  }
  if (!read_error_is_injectable(slotPath)) {
    // Windows CRTs refuse the open outright, so the loader fails at the
    // open instead of the read; the contract asserted below still holds,
    // but the read-error branch itself is covered only where the open
    // succeeds. The log line keeps that distinction visible in CI.
    std::puts("note: this platform rejects the open; read branch not "
              "exercised");
  }

  char loaded[64] = {};
  std::memset(loaded, 'Z', sizeof(loaded));
  std::size_t loadedLength = 1U;
  const bool ok = engine::runtime::load_game_data_from(
      directory, loaded, sizeof(loaded), &loadedLength);
  cleanup(directory);
  if (ok) {
    std::puts("failed read reported as a successful load");
    return 1;
  }
  if (loadedLength != 0U) {
    std::puts("failed read reported a length");
    return 1;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_roundtrip_with_directory_creation();
  if (result != 0) {
    return result;
  }
  result = check_failure_paths();
  if (result != 0) {
    return result;
  }
  result = check_read_capacity_boundaries();
  if (result != 0) {
    return result;
  }
  return check_read_error_is_not_a_successful_load();
}
