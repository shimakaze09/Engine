// Verifies packer thumbnail path construction and publication (audit
// #212): an overlong destination is refused instead of silently
// redirected into the working directory, truncated path assembly is
// rejected before any write, the PNG/checksum pair publishes atomically
// as a unit with an unwritable destination failing cleanly, and a
// successful publication is skip-gated on the second cook.

// stb implementations, warning-suppressed exactly as the packer driver
// does (third-party code compiled under the engine's strict flags).
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#pragma warning(disable : 4244)
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "packer_shared.h"

/// Test double for the packer driver's directory helper (real definition
/// lives in main.cpp, which owns main()); faithful: false when the path
/// exists as a file.
bool ensure_directory_exists(const char *dirPath) {
  std::error_code ec{};
  std::filesystem::create_directories(std::filesystem::path(dirPath), ec);
  return !ec && std::filesystem::is_directory(dirPath, ec);
}

namespace {

constexpr const char *kWorkDir = "thumbnail_paths_work";

void remove_all(const char *path) noexcept {
  std::error_code ignored{};
  std::filesystem::remove_all(std::filesystem::path(path), ignored);
}

bool write_file(const char *path, const char *text) noexcept {
  FILE *file = nullptr;
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
  const std::size_t length = std::strlen(text);
  const bool ok = std::fwrite(text, 1U, length, file) == length;
  return (std::fclose(file) == 0) && ok;
}

/// One triangle, position+normal only (stride 6, non-indexed).
PrimitiveData make_triangle() {
  PrimitiveData data{};
  data.hasUVs = false;
  data.hasSkin = false;
  const float vertices[18] = {
      -1.0F, -1.0F, 0.0F, 0.0F, 0.0F, 1.0F, // v0
      1.0F,  -1.0F, 0.0F, 0.0F, 0.0F, 1.0F, // v1
      0.0F,  1.0F,  0.0F, 0.0F, 0.0F, 1.0F, // v2
  };
  data.interleavedVertices.assign(vertices, vertices + 18);
  return data;
}

/// Overlong destinations are refused with a cleared buffer — never
/// redirected into the working directory where assets could collide.
int check_overlong_destination_refused() {
  const std::string longDir(600U, 'd');
  const std::string outputPath = longDir + "/asset.mesh";

  char thumbPath[512] = {};
  std::memset(thumbPath, 'x', sizeof(thumbPath) - 1U);
  if (build_thumbnail_path(outputPath.c_str(), thumbPath, sizeof(thumbPath))) {
    return 101;
  }
  if (thumbPath[0] != '\0') {
    return 102; // failure must clear the buffer
  }

  // The historical fallback wrote ./.thumbnails/<name>.png — a refused
  // build must leave the working directory untouched.
  remove_all(".thumbnails");
  const PrimitiveData triangle = make_triangle();
  if (generate_mesh_thumbnail(nullptr, outputPath.c_str(), triangle, 0U)) {
    return 103;
  }
  std::error_code ec{};
  if (std::filesystem::exists(".thumbnails/asset.mesh.png", ec)) {
    return 104; // the cwd redirect is the audit's failure scenario
  }
  return 0;
}

/// A destination that fits the directory but truncates the final path is
/// rejected before any write, as is a truncating checksum path.
int check_truncated_paths_rejected() {
  char tinyThumb[24] = {};
  if (build_thumbnail_path("some_dir/asset_with_a_long_name.mesh", tinyThumb,
                           sizeof(tinyThumb))) {
    return 111;
  }
  if (tinyThumb[0] != '\0') {
    return 112;
  }

  char tinyChecksum[8] = {};
  if (build_thumbnail_checksum_path(".thumbnails/asset.mesh.png", tinyChecksum,
                                    sizeof(tinyChecksum))) {
    return 113;
  }
  if (tinyChecksum[0] != '\0') {
    return 114;
  }
  return 0;
}

/// A valid cook publishes the PNG/checksum pair, and the second cook
/// skips through the checksum gate; both paths land beside the asset.
int check_publication_and_skip_gate() {
  remove_all(kWorkDir);
  std::error_code ec{};
  std::filesystem::create_directories(kWorkDir, ec);
  if (ec) {
    return 121;
  }

  const std::string sourcePath = std::string(kWorkDir) + "/asset.gltf";
  const std::string outputPath = std::string(kWorkDir) + "/asset.mesh";
  if (!write_file(sourcePath.c_str(), "source-bytes")) {
    return 122;
  }

  const PrimitiveData triangle = make_triangle();
  if (!generate_mesh_thumbnail(sourcePath.c_str(), outputPath.c_str(),
                               triangle, 7U)) {
    return 123;
  }

  char thumbPath[512] = {};
  char checksumPath[512] = {};
  if (!build_thumbnail_path(outputPath.c_str(), thumbPath,
                            sizeof(thumbPath)) ||
      !build_thumbnail_checksum_path(thumbPath, checksumPath,
                                     sizeof(checksumPath))) {
    return 124;
  }
  if (!std::filesystem::exists(thumbPath, ec) ||
      !std::filesystem::exists(checksumPath, ec)) {
    return 125; // the pair publishes as a unit
  }
  const std::uintmax_t pngSize = std::filesystem::file_size(thumbPath, ec);
  if (ec || (pngSize == 0U)) {
    return 126;
  }

  // Second cook with unchanged source + settings must skip via the gate
  // and leave the published pair in place.
  if (!generate_mesh_thumbnail(sourcePath.c_str(), outputPath.c_str(),
                               triangle, 7U)) {
    return 127;
  }
  if (std::filesystem::file_size(thumbPath, ec) != pngSize) {
    return 128;
  }

  remove_all(kWorkDir);
  return 0;
}

/// An unwritable destination (the .thumbnails slot occupied by a file)
/// fails the publication cleanly: no torn PNG, no checksum.
int check_unwritable_destination_fails_cleanly() {
  remove_all(kWorkDir);
  std::error_code ec{};
  std::filesystem::create_directories(kWorkDir, ec);
  if (ec) {
    return 131;
  }

  const std::string blocker = std::string(kWorkDir) + "/.thumbnails";
  if (!write_file(blocker.c_str(), "not-a-directory")) {
    return 132;
  }

  const std::string sourcePath = std::string(kWorkDir) + "/asset.gltf";
  const std::string outputPath = std::string(kWorkDir) + "/asset.mesh";
  if (!write_file(sourcePath.c_str(), "source-bytes")) {
    return 133;
  }

  const PrimitiveData triangle = make_triangle();
  if (generate_mesh_thumbnail(sourcePath.c_str(), outputPath.c_str(), triangle,
                              7U)) {
    return 134; // publication into a blocked destination must fail
  }
  if (!std::filesystem::is_regular_file(blocker, ec)) {
    return 135; // the blocking file itself is untouched
  }

  remove_all(kWorkDir);
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_overlong_destination_refused();
  if (result != 0) {
    return result;
  }
  result = check_truncated_paths_rejected();
  if (result != 0) {
    return result;
  }
  result = check_publication_and_skip_gate();
  if (result != 0) {
    return result;
  }
  return check_unwritable_destination_fails_cleanly();
}
