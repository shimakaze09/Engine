// Every scene, prefab and material committed to the repository is in the
// form the engine writes: loaded and saved again through the production
// readers and writers, each file comes back byte for byte. So an author who
// opens a sample and saves it sees a diff of exactly what they changed, and
// a merge of two authors' edits resolves per field (the writers put one
// field per line). Files a script or a hand wrote in another layout fail
// here until they are re-saved.
//
// With the argument --rewrite it saves each document over its source file
// instead of comparing, which is how a generated or hand-edited document is
// brought into the engine's form.

#include "engine/content/asset_catalog.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_loader.h"
#include "engine/renderer/material_writer.h"
#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr const char *kSourceMount = "canonical_src";
constexpr const char *kOutputMount = "canonical_out";
constexpr const char *kOutputDir = "canonical_documents_out";

bool g_rewrite = false;
int g_failures = 0;
int g_checked = 0;

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

/// Compares the re-saved `saved` with the committed `source`, naming the
/// first line that differs.
void compare(const fs::path &source, const fs::path &saved) {
  const std::string expected = read_file(source);
  const std::string actual = read_file(saved);
  ++g_checked;
  if (!actual.empty() && (expected == actual)) {
    return;
  }
  std::size_t line = 1U;
  std::size_t at = 0U;
  while ((at < expected.size()) && (at < actual.size()) &&
         (expected[at] == actual[at])) {
    line += (expected[at] == '\n') ? 1U : 0U;
    ++at;
  }
  std::fprintf(stderr,
               "FAIL: %s is not in the engine's form (first difference at "
               "line %zu); re-save it, or run this test with --rewrite\n",
               source.generic_string().c_str(), line);
  ++g_failures;
}

void check_scene(const fs::path &source) {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if ((world == nullptr) ||
      !engine::runtime::load_scene(*world, source.string().c_str())) {
    std::fprintf(stderr, "FAIL: %s does not load\n",
                 source.generic_string().c_str());
    ++g_failures;
    return;
  }
  const fs::path saved =
      g_rewrite ? source : fs::path(kOutputDir) / source.filename();
  if (!engine::runtime::save_scene(*world, saved.string().c_str())) {
    std::fprintf(stderr, "FAIL: %s does not save\n",
                 source.generic_string().c_str());
    ++g_failures;
    return;
  }
  if (!g_rewrite) {
    compare(source, saved);
  }
}

void check_prefab(const fs::path &source) {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    ++g_failures;
    return;
  }
  const engine::runtime::Entity entity =
      engine::runtime::instantiate_prefab(*world, source.string().c_str());
  const fs::path saved =
      g_rewrite ? source : fs::path(kOutputDir) / source.filename();
  if ((entity == engine::runtime::kInvalidEntity) ||
      !engine::runtime::save_prefab(*world, entity, saved.string().c_str())) {
    std::fprintf(stderr, "FAIL: %s does not load and save\n",
                 source.generic_string().c_str());
    ++g_failures;
    return;
  }
  if (!g_rewrite) {
    compare(source, saved);
  }
}

/// A material is read and written through a mount, as the editor does:
/// its folder is mounted for reading, the output folder for writing.
void check_material(const fs::path &source) {
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::content::AssetCatalog> catalog(
      new (std::nothrow) engine::content::AssetCatalog());
  if ((database == nullptr) || (catalog == nullptr)) {
    ++g_failures;
    return;
  }
  const std::string folder = source.parent_path().string();
  const fs::path outputFolder =
      g_rewrite ? source.parent_path() : fs::path(kOutputDir);
  engine::core::unmount(kSourceMount);
  engine::core::unmount(kOutputMount);
  if (!engine::core::mount(kSourceMount, folder.c_str()) ||
      !engine::core::mount(kOutputMount, outputFolder.string().c_str())) {
    ++g_failures;
    return;
  }
  // The folder's sidecars give its textures and parents their identity.
  static_cast<void>(engine::content::register_mounted_assets(
      catalog.get(), kSourceMount, folder.c_str()));

  const std::string name = source.filename().generic_string();
  const std::string sourcePath = std::string(kSourceMount) + "/" + name;
  const std::string outputPath = std::string(kOutputMount) + "/" + name;
  const auto loaded = engine::renderer::load_material_asset(
      database.get(), catalog.get(), sourcePath.c_str());
  if (!loaded.has_value()) {
    std::fprintf(stderr, "FAIL: %s does not load\n",
                 source.generic_string().c_str());
    ++g_failures;
    return;
  }
  const engine::content::AssetId id = *loaded;
  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database.get(), id);
  const engine::renderer::MaterialTextureSlots *slots =
      engine::renderer::find_material_texture_slots(database.get(), id);
  char parent[256] = {};
  const bool hasParent = engine::renderer::find_material_parent_virtual_path(
      catalog.get(), id, parent, sizeof(parent));
  const engine::renderer::MaterialTextureSlots noSlots{};
  if ((params == nullptr) ||
      !engine::renderer::save_material_asset(
          catalog.get(), outputPath.c_str(), *params,
          (slots != nullptr) ? *slots : noSlots, hasParent ? parent : nullptr,
          engine::renderer::material_overrides(database.get(), id))) {
    std::fprintf(stderr, "FAIL: %s does not save\n",
                 source.generic_string().c_str());
    ++g_failures;
    return;
  }
  if (!g_rewrite) {
    compare(source, outputFolder / source.filename());
  }
}

/// The committed documents under `root`, in a stable order.
void collect(const fs::path &root, std::vector<fs::path> *out) {
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && (it != end);
       it.increment(ec)) {
    const fs::path extension = it->path().extension();
    if (it->is_regular_file(ec) &&
        ((extension == ".scene") || (extension == ".prefab") ||
         (extension == ".mat"))) {
      out->push_back(it->path());
    }
  }
}

} // namespace

/// Runs this executable or test program.
int main(int argc, char **argv) {
  g_rewrite = (argc > 1) && (std::strcmp(argv[1], "--rewrite") == 0);
  const fs::path sourceRoot(ENGINE_SOURCE_ROOT);
  std::vector<fs::path> documents;
  collect(sourceRoot / "samples", &documents);
  collect(sourceRoot / "engine_assets", &documents);
  std::sort(documents.begin(), documents.end());

  std::error_code ec;
  fs::create_directories(kOutputDir, ec);
  if (!engine::core::initialize_vfs()) {
    std::fprintf(stderr, "FAIL: vfs\n");
    return 1;
  }
  for (const fs::path &document : documents) {
    const fs::path extension = document.extension();
    if (extension == ".scene") {
      check_scene(document);
    } else if (extension == ".prefab") {
      check_prefab(document);
    } else {
      check_material(document);
    }
  }
  engine::core::shutdown_vfs();
  fs::remove_all(kOutputDir, ec);

  if (g_rewrite) {
    std::printf("rewrote %zu documents (%d failed)\n", documents.size(),
                g_failures);
    return (g_failures == 0) ? 0 : 1;
  }
  if (documents.empty()) {
    std::fprintf(stderr, "FAIL: no committed documents found under %s\n",
                 sourceRoot.generic_string().c_str());
    return 1;
  }
  if (g_failures != 0) {
    return 1;
  }
  std::printf("PASS: %d committed documents are in the engine's form\n",
              g_checked);
  return 0;
}
