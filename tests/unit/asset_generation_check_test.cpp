// Verifies the cooked-asset generation gate (audit #211) through the
// production mesh loader: a .cookstamp whose output manifest contradicts
// the files on disk (hash mismatch, missing essential output, malformed
// manifest line) rejects the load, presentation-only thumbnail drift and
// never-certified assets (no stamp, pre-manifest stamp) stay loadable,
// verdicts cache per session until the test-only reset, a stamp from
// another tool version or a newer stamp schema rejects even with intact
// outputs (#424), a schema-4 manifest resolves relative to its stamp from
// any working directory and never outside it, and a non-regular file is
// refused without being opened (#527).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "../cook_fixture.h"
#include "engine/content/asset_staleness.h"
#include "engine/content/cook_contract.h"
#include "engine/core/hash.h"
#include "engine/core/mesh_asset.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/render_device.h"

namespace {

using engine::tests::hash_file;
using engine::tests::open_file_for_write;
using engine::tests::remove_with_stamp;
using engine::tests::write_bytes;
using engine::tests::write_stamp;
using engine::tests::write_stamp_text;
using engine::tests::write_valid_mesh;

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

/// A current-schema stamp that declares no TOOL_VERSION certifies nothing
/// (#571): only a pre-manifest schema predates the line.
int check_stamp_without_tool_version_rejects() {
  constexpr const char *kMesh = "gen_check_no_tool_version.mesh";
  remove_with_stamp(kMesh);
  if (!write_valid_mesh(kMesh)) {
    return 580;
  }
  std::uint64_t meshHash = 0ULL;
  if (!hash_file(kMesh, &meshHash)) {
    return 581;
  }
  char text[1024] = {};
  std::snprintf(text, sizeof(text),
                "SCHEMA %u\nSOURCE_HASH 0000000000000001\n"
                "IMPORT_HASH 0000000000000002\nPLATFORM TestPlat\n"
                "OUTPUT %016llx %s\n",
                static_cast<unsigned int>(engine::content::kCookStampSchema),
                static_cast<unsigned long long>(meshHash), kMesh);
  if (!write_stamp_text(kMesh, text)) {
    return 582;
  }
  const int result =
      (engine::content::cooked_asset_generation_ok(kMesh) || load_mesh(kMesh))
          ? 583
          : 0;
  remove_with_stamp(kMesh);
  engine::content::reset_cooked_asset_stale_warnings();
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

/// A stamp whose contract lines disagree with this build certifies
/// nothing (#424): a TOOL_VERSION other than the one the build was cooked
/// against, or a SCHEMA newer than the reader, rejects the load even when
/// every listed output hash is intact; a pre-manifest SCHEMA 2 stamp
/// without a TOOL_VERSION line stays on the legacy accept path.
int check_foreign_contract_rejects() {
  constexpr const char *kMesh = "gen_check_contract.mesh";
  remove_with_stamp(kMesh);
  if (!write_valid_mesh(kMesh)) {
    return 560;
  }
  std::uint64_t meshHash = 0ULL;
  if (!hash_file(kMesh, &meshHash)) {
    return 561;
  }

  char text[1024] = {};
  std::snprintf(text, sizeof(text),
                "SCHEMA %u\nTOOL_VERSION %u\nSOURCE_HASH 0000000000000001\n"
                "IMPORT_HASH 0000000000000002\nPLATFORM TestPlat\n"
                "OUTPUT %016llx %s\n",
                static_cast<unsigned int>(engine::content::kCookStampSchema),
                static_cast<unsigned int>(engine::content::kCookToolVersion -
                                          1U),
                static_cast<unsigned long long>(meshHash), kMesh);
  if (!write_stamp_text(kMesh, text)) {
    return 562;
  }
  if (engine::content::cooked_asset_generation_ok(kMesh) || load_mesh(kMesh)) {
    remove_with_stamp(kMesh);
    return 563; // another tool version's cook must be refused
  }

  engine::content::reset_cooked_asset_stale_warnings();
  std::snprintf(text, sizeof(text),
                "SCHEMA %u\nTOOL_VERSION %u\nSOURCE_HASH 0000000000000001\n"
                "IMPORT_HASH 0000000000000002\nPLATFORM TestPlat\n"
                "OUTPUT %016llx %s\n",
                static_cast<unsigned int>(engine::content::kCookStampSchema +
                                          1U),
                static_cast<unsigned int>(engine::content::kCookToolVersion),
                static_cast<unsigned long long>(meshHash), kMesh);
  if (!write_stamp_text(kMesh, text)) {
    return 564;
  }
  if (engine::content::cooked_asset_generation_ok(kMesh) || load_mesh(kMesh)) {
    remove_with_stamp(kMesh);
    return 565; // a newer stamp schema must be refused
  }

  engine::content::reset_cooked_asset_stale_warnings();
  if (!write_stamp_text(kMesh, "SCHEMA 2\nSOURCE_HASH 0000000000000001\n"
                               "IMPORT_HASH 0000000000000002\n")) {
    return 566;
  }
  if (!engine::content::cooked_asset_generation_ok(kMesh) ||
      !load_mesh(kMesh)) {
    remove_with_stamp(kMesh);
    return 567; // pre-manifest legacy stamps stay loadable
  }

  engine::content::reset_cooked_asset_stale_warnings();
  std::snprintf(text, sizeof(text),
                "SCHEMA %u\nTOOL_VERSION %u\nSOURCE_HASH 0000000000000001\n"
                "IMPORT_HASH 0000000000000002\nPLATFORM TestPlat\n"
                "OUTPUT %016llx %s\n",
                static_cast<unsigned int>(engine::content::kCookStampSchema),
                static_cast<unsigned int>(engine::content::kCookToolVersion),
                static_cast<unsigned long long>(meshHash), kMesh);
  if (!write_stamp_text(kMesh, text)) {
    return 568;
  }
  if (!engine::content::cooked_asset_generation_ok(kMesh) ||
      !load_mesh(kMesh)) {
    remove_with_stamp(kMesh);
    return 569; // the current contract still loads
  }

  remove_with_stamp(kMesh);
  return 0;
}


/// EXPECTATION (#527): a schema-4 manifest names its outputs relative to
/// the stamp, so the certified asset loads from any working directory
/// through its absolute path; a manifest naming a path that leaves the
/// stamp's directory is corrupt and rejects even when the file it points
/// at is intact. On base the recorded path was opened relative to the
/// process working directory and the stamp certified nothing elsewhere.
int check_relative_manifest_loads_from_any_cwd() {
  constexpr const char *kDir = "gen_check_cwd";
  constexpr const char *kElsewhere = "gen_check_elsewhere";
  std::error_code ec{};
  std::filesystem::remove_all(kDir, ec);
  std::filesystem::remove_all(kElsewhere, ec);
  std::filesystem::create_directories(kDir, ec);
  std::filesystem::create_directories(kElsewhere, ec);
  const std::string mesh = std::string(kDir) + "/rel.mesh";
  const std::string victim = "gen_check_victim.mesh";
  if (ec || !write_valid_mesh(mesh.c_str()) ||
      !write_valid_mesh(victim.c_str())) {
    return 570;
  }
  std::uint64_t meshHash = 0ULL;
  if (!hash_file(mesh.c_str(), &meshHash)) {
    return 571;
  }
  char outputs[512] = {};
  std::snprintf(outputs, sizeof(outputs), "OUTPUT %016llx rel.mesh\n",
                static_cast<unsigned long long>(meshHash));
  if (!write_stamp(mesh.c_str(), outputs)) {
    return 572;
  }
  const std::filesystem::path home = std::filesystem::current_path(ec);
  const std::string absMesh = std::filesystem::absolute(mesh, ec).string();
  std::filesystem::current_path(std::filesystem::path(kElsewhere), ec);
  if (ec) {
    return 573;
  }
  engine::content::reset_cooked_asset_stale_warnings();
  const bool okElsewhere =
      engine::content::cooked_asset_generation_ok(absMesh.c_str()) &&
      load_mesh(absMesh.c_str());
  std::filesystem::current_path(home, ec);
  int result = 0;
  if (!okElsewhere) {
    std::fprintf(stderr, "certified asset did not load from another "
                         "working directory\n");
    result = 574;
  }

  // An escaping entry rejects even though the victim it names is intact.
  if (result == 0) {
    engine::content::reset_cooked_asset_stale_warnings();
    std::snprintf(outputs, sizeof(outputs),
                  "OUTPUT %016llx rel.mesh\nOUTPUT %016llx ../%s\n",
                  static_cast<unsigned long long>(meshHash),
                  static_cast<unsigned long long>(meshHash), victim.c_str());
    if (!write_stamp(mesh.c_str(), outputs)) {
      result = 575;
    } else if (engine::content::cooked_asset_generation_ok(mesh.c_str()) ||
               load_mesh(mesh.c_str())) {
      std::fprintf(stderr, "a manifest leaving the stamp directory was "
                           "accepted\n");
      result = 576;
    }
  }
  remove_with_stamp(mesh.c_str());
  static_cast<void>(std::remove(victim.c_str()));
  std::filesystem::remove_all(kDir, ec);
  std::filesystem::remove_all(kElsewhere, ec);
  return result;
}

/// EXPECTATION (#527): a stamp or sidecar naming something that is not
/// a regular file is refused without opening it, so the load path can
/// never block on a device or FIFO. On base hash_file_bytes opened the
/// FIFO and waited for a writer forever.
int check_non_regular_files_are_refused_promptly() {
#ifdef _WIN32
  return 0;
#else
  constexpr const char *kMesh = "gen_check_fifo.mesh";
  constexpr const char *kFifo = "gen_check_fifo.mesh.hull";
  constexpr const char *kMeta = "gen_check_fifo.mesh.meta.json";
  remove_with_stamp(kMesh);
  static_cast<void>(std::remove(kFifo));
  static_cast<void>(std::remove(kMeta));
  if (!write_valid_mesh(kMesh) || (mkfifo(kFifo, 0600) != 0)) {
    return 580;
  }
  std::uint64_t meshHash = 0ULL;
  if (!hash_file(kMesh, &meshHash)) {
    return 581;
  }
  char outputs[512] = {};
  std::snprintf(outputs, sizeof(outputs),
                "OUTPUT %016llx %s\nOUTPUT 0000000000000001 %s\n",
                static_cast<unsigned long long>(meshHash), kMesh, kFifo);
  int result = 0;
  if (!write_stamp(kMesh, outputs)) {
    result = 582;
  } else if (engine::content::cooked_asset_generation_ok(kMesh) ||
             load_mesh(kMesh)) {
    result = 583; // a FIFO is not a cooked output
  }
  // The staleness sidecar naming a device returns without reading it.
  const char metaText[] =
      "{\"source\":\"/dev/zero\",\"sourceContentHash\":\"0000000000000001\"}";
  if ((result == 0) &&
      !write_bytes(kMeta, metaText, sizeof(metaText) - 1U)) {
    result = 584;
  }
  if (result == 0) {
    engine::content::reset_cooked_asset_stale_warnings();
    engine::content::warn_if_cooked_asset_stale(kMesh);
  }
  remove_with_stamp(kMesh);
  static_cast<void>(std::remove(kFifo));
  static_cast<void>(std::remove(kMeta));
  return result;
#endif
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
  result = check_uncertified_assets_stay_loadable();
  if (result != 0) {
    return result;
  }
  engine::content::reset_cooked_asset_stale_warnings();
  result = check_stamp_without_tool_version_rejects();
  if (result != 0) {
    return result;
  }

  result = check_foreign_contract_rejects();
  if (result != 0) {
    return result;
  }
  engine::content::reset_cooked_asset_stale_warnings();
  result = check_relative_manifest_loads_from_any_cwd();
  if (result != 0) {
    return result;
  }
  engine::content::reset_cooked_asset_stale_warnings();
  return check_non_regular_files_are_refused_promptly();
}
