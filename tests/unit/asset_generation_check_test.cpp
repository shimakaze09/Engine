// Verifies the cooked-asset generation gate (audit #211) through the
// production mesh loader: a .cookstamp whose output manifest contradicts
// the files on disk (hash mismatch, missing essential output, malformed
// manifest line) rejects the load, presentation-only thumbnail drift and
// never-certified assets (no stamp, pre-manifest stamp) stay loadable,
// and verdicts cache per session until the test-only reset.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/content/asset_staleness.h"
#include "engine/core/hash.h"
#include "engine/core/mesh_asset.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/render_device.h"

namespace {

bool open_file_for_write(const char *path, FILE **outFile) noexcept {
  *outFile = nullptr;
#ifdef _WIN32
  return fopen_s(outFile, path, "wb") == 0;
#else
  *outFile = std::fopen(path, "wb");
  return *outFile != nullptr;
#endif
}

/// Writes raw bytes to a file (fixture side; the production path under
/// test is the loader, not this writer).
bool write_bytes(const char *path, const void *data,
                 std::size_t size) noexcept {
  FILE *file = nullptr;
  if (!open_file_for_write(path, &file) || (file == nullptr)) {
    return false;
  }
  const bool ok = (size == 0U) || (std::fwrite(data, 1U, size, file) == size);
  return (std::fclose(file) == 0) && ok;
}

/// FNV-1a of a file's bytes, matching the packer's output-manifest hash.
bool hash_file(const char *path, std::uint64_t *outHash) noexcept {
  FILE *file = nullptr;
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
  std::uint64_t hash = engine::core::kFnv1a64Offset;
  unsigned char buffer[512] = {};
  std::size_t bytesRead = 0U;
  while ((bytesRead = std::fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
    for (std::size_t i = 0U; i < bytesRead; ++i) {
      hash = engine::core::fnv1a_64_append(hash, buffer[i]);
    }
  }
  std::fclose(file);
  *outHash = hash;
  return true;
}

/// Writes a minimal valid cooked mesh (1 position/normal vertex).
bool write_valid_mesh(const char *path) noexcept {
  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 1U;
  header.indexCount = 0U;

  const std::array<float, 6U> vertexData = {0.0F, 0.0F, 0.0F,
                                            0.0F, 1.0F, 0.0F};
  FILE *file = nullptr;
  if (!open_file_for_write(path, &file) || (file == nullptr)) {
    return false;
  }
  bool ok = std::fwrite(&header, sizeof(header), 1U, file) == 1U;
  ok = ok && (std::fwrite(vertexData.data(), sizeof(float), vertexData.size(),
                          file) == vertexData.size());
  return (std::fclose(file) == 0) && ok;
}

/// Writes a SCHEMA-3 stamp for `meshPath` from printf-style OUTPUT lines.
bool write_stamp(const char *meshPath, const char *outputLines) noexcept {
  char stampPath[512] = {};
  std::snprintf(stampPath, sizeof(stampPath), "%s.cookstamp", meshPath);
  char text[2048] = {};
  const int written = std::snprintf(
      text, sizeof(text),
      "SCHEMA 3\nTOOL_VERSION 3\nSOURCE_HASH 0000000000000001\n"
      "IMPORT_HASH 0000000000000002\nPLATFORM TestPlat\n%s",
      outputLines);
  if ((written <= 0) || (written >= static_cast<int>(sizeof(text)))) {
    return false;
  }
  return write_bytes(stampPath, text, static_cast<std::size_t>(written));
}

void remove_with_stamp(const char *meshPath) noexcept {
  char stampPath[512] = {};
  std::snprintf(stampPath, sizeof(stampPath), "%s.cookstamp", meshPath);
  static_cast<void>(std::remove(meshPath));
  static_cast<void>(std::remove(stampPath));
}

/// Loads through the production loader; true on accepted load.
bool load_mesh(const char *path) noexcept {
  engine::renderer::CpuMeshData data{};
  return engine::renderer::load_mesh_data_from_file(path, &data);
}

/// An intact certified generation (mesh + meta listed with true hashes)
/// loads; the same stamp with the mesh bytes changed afterwards rejects
/// both the validator and the production load, and the rejection verdict
/// caches until the test-only reset revalidates the repaired state.
int check_certified_and_mixed_generation() {
  constexpr const char *kMesh = "gen_check_certified.mesh";
  constexpr const char *kMeta = "gen_check_certified.mesh.meta.json";
  remove_with_stamp(kMesh);
  static_cast<void>(std::remove(kMeta));

  const char metaText[] = "{}";
  if (!write_valid_mesh(kMesh) ||
      !write_bytes(kMeta, metaText, sizeof(metaText) - 1U)) {
    return 500;
  }
  std::uint64_t meshHash = 0ULL;
  std::uint64_t metaHash = 0ULL;
  if (!hash_file(kMesh, &meshHash) || !hash_file(kMeta, &metaHash)) {
    return 501;
  }
  char outputs[512] = {};
  std::snprintf(outputs, sizeof(outputs),
                "OUTPUT %016llx %s\nOUTPUT %016llx %s\n",
                static_cast<unsigned long long>(meshHash), kMesh,
                static_cast<unsigned long long>(metaHash), kMeta);
  if (!write_stamp(kMesh, outputs)) {
    return 502;
  }
  if (!engine::content::cooked_asset_generation_ok(kMesh) ||
      !load_mesh(kMesh)) {
    return 503; // an intact certified generation must load
  }

  // Simulate an interrupted recook: a different but individually valid
  // mesh beside the old stamp — only the generation check can tell the
  // mix apart, the decoder alone would accept it.
  engine::content::reset_cooked_asset_stale_warnings();
  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 1U;
  header.indexCount = 0U;
  const std::array<float, 6U> recookedVertex = {9.0F, 9.0F, 9.0F,
                                                0.0F, 1.0F, 0.0F};
  FILE *recookFile = nullptr;
  if (!open_file_for_write(kMesh, &recookFile) || (recookFile == nullptr)) {
    return 504;
  }
  bool recookOk = std::fwrite(&header, sizeof(header), 1U, recookFile) == 1U;
  recookOk = recookOk && (std::fwrite(recookedVertex.data(), sizeof(float),
                                      recookedVertex.size(), recookFile) ==
                          recookedVertex.size());
  if ((std::fclose(recookFile) != 0) || !recookOk) {
    return 504;
  }
  if (engine::content::cooked_asset_generation_ok(kMesh)) {
    return 505; // hash-contradicted essential output must reject
  }
  if (load_mesh(kMesh)) {
    return 506; // the production load must refuse the mixed generation
  }

  // The verdict is cached: repairing the disk state alone changes nothing
  // until the session cache resets.
  if (!write_valid_mesh(kMesh)) {
    return 507;
  }
  if (engine::content::cooked_asset_generation_ok(kMesh)) {
    return 508;
  }
  engine::content::reset_cooked_asset_stale_warnings();
  if (!engine::content::cooked_asset_generation_ok(kMesh) ||
      !load_mesh(kMesh)) {
    return 509; // repaired generation revalidates after reset
  }

  remove_with_stamp(kMesh);
  static_cast<void>(std::remove(kMeta));
  return 0;
}

/// A stamp listing an essential output that no longer exists rejects.
int check_missing_essential_output_rejects() {
  constexpr const char *kMesh = "gen_check_missing.mesh";
  remove_with_stamp(kMesh);
  if (!write_valid_mesh(kMesh)) {
    return 520;
  }
  std::uint64_t meshHash = 0ULL;
  if (!hash_file(kMesh, &meshHash)) {
    return 521;
  }
  char outputs[512] = {};
  std::snprintf(outputs, sizeof(outputs),
                "OUTPUT %016llx %s\nOUTPUT %016llx %s\n",
                static_cast<unsigned long long>(meshHash), kMesh,
                static_cast<unsigned long long>(meshHash),
                "gen_check_missing.mesh.hull");
  if (!write_stamp(kMesh, outputs)) {
    return 522;
  }
  const int result = (!engine::content::cooked_asset_generation_ok(kMesh) &&
                      !load_mesh(kMesh))
                         ? 0
                         : 523;
  remove_with_stamp(kMesh);
  return result;
}

/// A malformed OUTPUT manifest line is a torn commit marker and rejects.
int check_malformed_manifest_rejects() {
  constexpr const char *kMesh = "gen_check_malformed.mesh";
  remove_with_stamp(kMesh);
  if (!write_valid_mesh(kMesh) || !write_stamp(kMesh, "OUTPUT nonsense\n")) {
    return 530;
  }
  const int result = (!engine::content::cooked_asset_generation_ok(kMesh) &&
                      !load_mesh(kMesh))
                         ? 0
                         : 531;
  remove_with_stamp(kMesh);
  return result;
}

/// Missing and drifted thumbnails only warn: presentation outputs under
/// .thumbnails/ never brick the asset.
int check_thumbnail_drift_stays_loadable() {
  constexpr const char *kMesh = "gen_check_thumb.mesh";
  remove_with_stamp(kMesh);
  if (!write_valid_mesh(kMesh)) {
    return 540;
  }
  std::uint64_t meshHash = 0ULL;
  if (!hash_file(kMesh, &meshHash)) {
    return 541;
  }
  char outputs[512] = {};
  std::snprintf(outputs, sizeof(outputs),
                "OUTPUT %016llx %s\n"
                "OUTPUT 00000000000000ff gen_thumbs/.thumbnails/absent.png\n",
                static_cast<unsigned long long>(meshHash), kMesh);
  if (!write_stamp(kMesh, outputs)) {
    return 542;
  }
  const int result = (engine::content::cooked_asset_generation_ok(kMesh) &&
                      load_mesh(kMesh))
                         ? 0
                         : 543;
  remove_with_stamp(kMesh);
  return result;
}

/// Never-certified content stays loadable: no stamp at all, and a
/// pre-manifest stamp with no OUTPUT lines.
int check_uncertified_assets_stay_loadable() {
  constexpr const char *kNoStamp = "gen_check_nostamp.mesh";
  constexpr const char *kLegacy = "gen_check_legacy.mesh";
  remove_with_stamp(kNoStamp);
  remove_with_stamp(kLegacy);
  if (!write_valid_mesh(kNoStamp) || !write_valid_mesh(kLegacy) ||
      !write_stamp(kLegacy, "")) {
    return 550;
  }
  int result = 0;
  if (!engine::content::cooked_asset_generation_ok(kNoStamp) ||
      !load_mesh(kNoStamp)) {
    result = 551;
  } else if (!engine::content::cooked_asset_generation_ok(kLegacy) ||
             !load_mesh(kLegacy)) {
    result = 552;
  }
  remove_with_stamp(kNoStamp);
  remove_with_stamp(kLegacy);
  return result;
}

} // namespace

// mesh_loader.cpp compiles standalone into this suite (same recipe as
// mesh_loader_test.cpp); its GPU upload entry points are never called on
// the CPU decode path, so the device hooks are inert stubs.
namespace engine::renderer {

bool initialize_render_device() noexcept { return false; }

void shutdown_render_device() noexcept {}

const RenderDevice *render_device() noexcept { return nullptr; }

} // namespace engine::renderer

/// Runs this executable or test program.
int main() {
  engine::content::reset_cooked_asset_stale_warnings();

  int result = check_certified_and_mixed_generation();
  if (result != 0) {
    return result;
  }
  result = check_missing_essential_output_rejects();
  if (result != 0) {
    return result;
  }
  result = check_malformed_manifest_rejects();
  if (result != 0) {
    return result;
  }
  result = check_thumbnail_drift_stays_loadable();
  if (result != 0) {
    return result;
  }
  return check_uncertified_assets_stay_loadable();
}
