// Pins the material loader's schema-version refusal (issue #369): the
// version gate in parse_material_text is what stops a material written by a
// newer engine — or one whose version field is malformed — from loading as
// the subset this build recognizes and later being resaved over the author's
// file as that reduction. The upper bound already has one pinned case in
// engine_unit_material_asset; this suite pins the rest of the gate through
// both production entry points: every refused version shape fails
// load_material_asset with Parse, a refused reload_material_asset leaves the
// previously registered record serving its exact prior values, and the
// accepted revisions (1, 2, absent-as-v1) still commit.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_loader.h"

namespace {

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

/// Exact float comparison: every tested value is exactly representable and
/// never crosses lossy text formatting wider than round-trip precision.
bool exactly_equal(float lhs, float rhs) noexcept { return lhs == rhs; }

/// Writes a material JSON file into the mounted test directory.
bool write_material_file(const char *path, const char *text) noexcept {
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
  const std::size_t size = std::strlen(text);
  const std::size_t written = std::fwrite(text, 1U, size, file);
  std::fclose(file);
  return written == size;
}

void remove_file(const char *path) noexcept {
  static_cast<void>(std::remove(path));
}

struct VersionCase final {
  const char *json;
  bool accepted;
};

// Revisions 1 and 2 are the supported range; an absent key is documented v1
// semantics. Everything else — zero, future, negative, fractional,
// past-uint32, or a non-integer JSON type — must be refused. Mirrors the
// scene loader's table (engine_unit_scene_version_gate) with the material
// schema's own accepted set.
constexpr VersionCase kCases[] = {
    {"{\"version\":1,\"roughness\":0.25}", true},
    {"{\"version\":2,\"roughness\":0.25}", true},
    {"{\"roughness\":0.25}", true},
    {"{\"version\":3,\"roughness\":0.25}", false},
    {"{\"version\":999,\"roughness\":0.25}", false},
    {"{\"version\":0,\"roughness\":0.25}", false},
    {"{\"version\":-1,\"roughness\":0.25}", false},
    {"{\"version\":1.5,\"roughness\":0.25}", false},
    {"{\"version\":4294967296,\"roughness\":0.25}", false},
    {"{\"version\":\"2\",\"roughness\":0.25}", false},
    {"{\"version\":true,\"roughness\":0.25}", false},
    {"{\"version\":null,\"roughness\":0.25}", false},
    {"{\"version\":[2],\"roughness\":0.25}", false},
};

// Every case runs against its own file so an accepted case's registration
// (AssetIds are path-derived) can never mask a later case's outcome.
void run_load_case(engine::renderer::AssetDatabase *database,
                   const VersionCase &testCase, std::size_t index) noexcept {
  char path[64] = {};
  std::snprintf(path, sizeof(path), "material_version_case_%zu.json", index);
  char virtualPath[80] = {};
  std::snprintf(virtualPath, sizeof(virtualPath), "mat/%s", path);

  if (!write_material_file(path, testCase.json)) {
    check(false, "write case file");
    return;
  }
  const auto result =
      engine::renderer::load_material_asset(database, virtualPath);
  remove_file(path);

  char label[128] = {};
  std::snprintf(label, sizeof(label), "%s: %s",
                testCase.accepted ? "accepted" : "refused", testCase.json);
  check(result.has_value() == testCase.accepted, label);

  if (testCase.accepted) {
    if (!result.has_value()) {
      return;
    }
    // An accepted load registers the record with its authored value intact.
    const engine::renderer::Material *params =
        engine::renderer::find_material_params(database, *result);
    check((params != nullptr) && exactly_equal(params->roughness, 0.25F),
          "accepted load registered exact params");
  } else {
    // Short-circuit before error(): if the gate regressed and the load
    // succeeded, reading error() from an engaged expected would be UB on
    // exactly the path that must report the regression.
    check(!result.has_value() &&
              (result.error() == engine::renderer::MaterialLoadError::Parse),
          "refusal classified as Parse");
    // A refused fresh load must not have registered anything for the id the
    // path would have produced.
    const engine::renderer::AssetId wouldBeId =
        engine::renderer::make_asset_id_from_path(virtualPath);
    check(engine::renderer::find_material_params(database, wouldBeId) ==
              nullptr,
          "refused load registered nothing");
  }
}

// The reload direction is where the data-loss guard earns its keep: a live
// record must keep serving its previous valid state when the on-disk file
// comes back with a version this build cannot read.
void run_reload_cases(engine::renderer::AssetDatabase *database) noexcept {
  constexpr const char *kPath = "material_version_reload.json";
  constexpr const char *kVirtualPath = "mat/material_version_reload.json";

  if (!write_material_file(kPath, "{\"version\":1,\"roughness\":0.25}")) {
    check(false, "write reload baseline");
    return;
  }
  const auto baseline =
      engine::renderer::load_material_asset(database, kVirtualPath);
  if (!baseline.has_value()) {
    remove_file(kPath);
    check(false, "load reload baseline");
    return;
  }
  const engine::renderer::AssetId id = *baseline;

  for (const VersionCase &testCase : kCases) {
    if (testCase.accepted) {
      continue;
    }
    if (!write_material_file(kPath, testCase.json)) {
      check(false, "rewrite reload file");
      break;
    }
    const auto reloaded =
        engine::renderer::reload_material_asset(database, kVirtualPath);

    char label[128] = {};
    std::snprintf(label, sizeof(label), "reload refused: %s", testCase.json);
    check(!reloaded.has_value() &&
              (reloaded.error() ==
               engine::renderer::MaterialLoadError::Parse),
          label);

    const engine::renderer::Material *params =
        engine::renderer::find_material_params(database, id);
    check((params != nullptr) && exactly_equal(params->roughness, 0.25F),
          "refused reload kept the prior record's exact value");
  }
  remove_file(kPath);
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_vfs()) {
    return 1;
  }
  // Test material files are written to the working directory, mounted
  // under the "mat" virtual prefix.
  if (!engine::core::mount("mat", ".")) {
    engine::core::shutdown_vfs();
    return 2;
  }

  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (database == nullptr) {
    engine::core::shutdown_vfs();
    return 3;
  }

  std::size_t index = 0U;
  for (const VersionCase &testCase : kCases) {
    run_load_case(database.get(), testCase, index);
    ++index;
  }
  run_reload_cases(database.get());

  database.reset();
  engine::core::shutdown_vfs();
  return g_tests.finish("material version gate tests");
}
