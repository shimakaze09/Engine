// Verifies the single-slot game save over explicit directories: byte-exact
// roundtrip, recursive directory creation, missing-file and oversized
// rejections, and the capacity-overflow guard on load.

#include <cstdio>
#include <cstring>
#include <string>

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

/// Removes the scratch save file (directories may remain; they are empty).
void cleanup(const char *directory) {
  char path[640] = {};
  std::snprintf(path, sizeof(path), "%s/save.json", directory);
  static_cast<void>(std::remove(path));
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

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_roundtrip_with_directory_creation();
  if (result != 0) {
    return result;
  }
  return check_failure_paths();
}
