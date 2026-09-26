// Verifies that a material's catalog edges follow its live record and that
// a change reaches every material below it (issue #681): an editor edit
// that re-points a texture slot moves the catalog edge from the old
// texture to the new one, an undo that hands the slot back to the parent
// drops the edge, an edit that changes no reference leaves the catalog's
// generation alone, and a grandchild re-inherits when the grandparent is
// edited live or reloaded from disk.

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>

#include "engine/content/asset_catalog.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_inheritance.h"
#include "engine/renderer/material_loader.h"

#include "../material_ref_fixture.h"

namespace {

using engine::content::AssetId;

engine::content::AssetCatalog *g_catalog = nullptr;

constexpr const char *kParentOs = "material_live_edges_parent.json";
constexpr const char *kChildOs = "material_live_edges_child.json";
constexpr const char *kGrandchildOs = "material_live_edges_grandchild.json";
constexpr const char *kParentPath = "mat/material_live_edges_parent.json";
constexpr const char *kChildPath = "mat/material_live_edges_child.json";
constexpr const char *kGrandchildPath =
    "mat/material_live_edges_grandchild.json";

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
  const std::size_t size = std::strlen(text);
  const std::size_t written = std::fwrite(text, 1U, size, file);
  std::fclose(file);
  return written == size;
}

void remove_files() noexcept {
  for (const char *path : {kParentOs, kChildOs, kGrandchildOs}) {
    static_cast<void>(std::remove(path));
  }
}

/// Whether the catalog's record for `id` depends on exactly `expected`, in
/// order.
bool edges_are(AssetId id, std::initializer_list<AssetId> expected) noexcept {
  AssetId deps[engine::content::AssetMetadata::kMaxDependencies] = {};
  const std::size_t count = engine::content::get_dependencies(
      g_catalog, id, deps, engine::content::AssetMetadata::kMaxDependencies);
  if (count != expected.size()) {
    return false;
  }
  std::size_t i = 0U;
  for (const AssetId dep : expected) {
    if (deps[i++] != dep) {
      return false;
    }
  }
  return true;
}

std::size_t dependents_of(AssetId id) noexcept {
  return engine::content::find_asset_dependents(g_catalog, id, nullptr, 0U);
}

/// Loads parent <- child <- grandchild; the child authors its own albedo.
/// Returns 0 on success.
int load_chain(engine::renderer::AssetDatabase *database, AssetId *parent,
               AssetId *child, AssetId *grandchild) noexcept {
  const auto parentRef =
      engine::tests::catalog_material(g_catalog, kParentPath);
  const auto childRef = engine::tests::catalog_material(g_catalog, kChildPath);
  const auto parentAlbedo = engine::tests::catalog_texture(
      g_catalog, "mat/material_live_edges_parent_albedo.png");
  const auto childAlbedo = engine::tests::catalog_texture(
      g_catalog, "mat/material_live_edges_child_albedo.png");
  char parentJson[256] = {};
  char childJson[256] = {};
  char grandchildJson[256] = {};
  std::snprintf(parentJson, sizeof(parentJson),
                "{\"version\":4,\"roughness\":0.3,\"metallic\":0.1,"
                "\"textures\":{\"albedo\":\"%s\"}}",
                parentAlbedo.text);
  std::snprintf(childJson, sizeof(childJson),
                "{\"version\":4,\"parent\":\"%s\",\"opacity\":0.5,"
                "\"textures\":{\"albedo\":\"%s\"}}",
                parentRef.text, childAlbedo.text);
  std::snprintf(grandchildJson, sizeof(grandchildJson),
                "{\"version\":4,\"parent\":\"%s\",\"alphaCutoff\":0.25}",
                childRef.text);
  if (!write_file(kParentOs, parentJson) || !write_file(kChildOs, childJson) ||
      !write_file(kGrandchildOs, grandchildJson)) {
    return 1;
  }
  const auto parentId =
      engine::renderer::load_material_asset(database, g_catalog, kParentPath);
  const auto childId =
      engine::renderer::load_material_asset(database, g_catalog, kChildPath);
  const auto grandchildId = engine::renderer::load_material_asset(
      database, g_catalog, kGrandchildPath);
  if (!parentId.has_value() || !childId.has_value() ||
      !grandchildId.has_value()) {
    return 2;
  }
  *parent = *parentId;
  *child = *childId;
  *grandchild = *grandchildId;
  return 0;
}

/// EXPECTATION: the catalog edges a material records are the references
/// its live record holds. An editor edit changed the record and left the
/// edges as the file had them, so a notify for the new texture missed the
/// material and one for the old texture reached it.
int check_live_slot_edit_moves_edges(
    engine::renderer::AssetDatabase *database) noexcept {
  AssetId parent = 0U;
  AssetId child = 0U;
  AssetId grandchild = 0U;
  if (load_chain(database, &parent, &child, &grandchild) != 0) {
    return 10;
  }
  const AssetId oldAlbedo = engine::content::make_asset_id_from_path(
      "mat/material_live_edges_child_albedo.png");
  constexpr const char *kNewAlbedoPath =
      "mat/material_live_edges_new_albedo.png";
  if (engine::tests::catalog_texture(g_catalog, kNewAlbedoPath).text[0] ==
      '\0') {
    return 18;
  }
  const AssetId newAlbedo =
      engine::content::make_asset_id_from_path(kNewAlbedoPath);
  const AssetId parentAlbedo = engine::content::make_asset_id_from_path(
      "mat/material_live_edges_parent_albedo.png");
  if (!edges_are(child, {parent, oldAlbedo})) {
    return 11;
  }

  // An edit that changes no reference writes no catalog record.
  const std::uint64_t before = g_catalog->generation;
  engine::renderer::Material params =
      *engine::renderer::find_material_params(database, child);
  engine::renderer::MaterialTextureSlots slots =
      *engine::renderer::find_material_texture_slots(database, child);
  params.roughness = 0.9F;
  if (!engine::renderer::edit_material_asset(database, g_catalog, child, params,
                                             slots) ||
      (g_catalog->generation != before) ||
      !edges_are(child, {parent, oldAlbedo})) {
    return 12;
  }

  // Re-pointing the slot moves the edge.
  slots.albedo = newAlbedo;
  if (!engine::renderer::edit_material_asset(database, g_catalog, child, params,
                                             slots)) {
    return 13;
  }
  if (!edges_are(child, {parent, newAlbedo})) {
    std::printf("child edges after re-pointing albedo are stale\n");
    return 14;
  }
  if ((dependents_of(oldAlbedo) != 0U) || (dependents_of(newAlbedo) != 1U)) {
    return 15;
  }

  // An undo that hands the slot back to the parent drops the edge: the
  // inherited texture is the parent's edge to carry.
  const std::uint16_t overrides = static_cast<std::uint16_t>(
      engine::renderer::material_overrides(database, child) &
      ~engine::renderer::material_field::kAlbedoTexture);
  slots.albedo = parentAlbedo;
  if (!engine::renderer::restore_material_asset(database, g_catalog, child,
                                                params, slots, overrides)) {
    return 16;
  }
  if (!edges_are(child, {parent}) || (dependents_of(newAlbedo) != 0U) ||
      (dependents_of(parentAlbedo) != 1U)) {
    std::printf("child edges after returning albedo to the parent\n");
    return 17;
  }
  return 0;
}

/// EXPECTATION: a change to a material reaches every material below it,
/// not only its children: a grandchild that authors nothing of a field
/// shows the grandparent's value after a live edit and after a reload.
int check_grandchild_follows(
    engine::renderer::AssetDatabase *database) noexcept {
  AssetId parent = 0U;
  AssetId child = 0U;
  AssetId grandchild = 0U;
  if (load_chain(database, &parent, &child, &grandchild) != 0) {
    return 20;
  }

  engine::renderer::Material params =
      *engine::renderer::find_material_params(database, parent);
  const engine::renderer::MaterialTextureSlots slots =
      *engine::renderer::find_material_texture_slots(database, parent);
  params.roughness = 0.75F;
  if (!engine::renderer::edit_material_asset(database, g_catalog, parent,
                                             params, slots)) {
    return 21;
  }
  const engine::renderer::Material *grand =
      engine::renderer::find_material_params(database, grandchild);
  if ((grand == nullptr) || (grand->roughness != 0.75F) ||
      (grand->alphaCutoff != 0.25F) || (grand->opacity != 0.5F)) {
    std::printf("grandchild after a live grandparent edit\n");
    return 22;
  }

  if (!write_file(kParentOs, "{\"version\":4,\"roughness\":0.3,"
                             "\"metallic\":0.6}")) {
    return 23;
  }
  if (!engine::renderer::reload_material_asset(database, g_catalog, kParentPath)
           .has_value()) {
    return 24;
  }
  grand = engine::renderer::find_material_params(database, grandchild);
  if ((grand == nullptr) || (grand->metallic != 0.6F) ||
      (grand->roughness != 0.3F) || (grand->alphaCutoff != 0.25F)) {
    std::printf("grandchild after a grandparent reload\n");
    return 25;
  }
  return 0;
}

} // namespace

int main() {
  std::unique_ptr<engine::content::AssetCatalog> catalogOwner(
      new (std::nothrow) engine::content::AssetCatalog());
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if ((catalogOwner == nullptr) || (database == nullptr)) {
    return 1;
  }
  g_catalog = catalogOwner.get();
  if (!engine::core::initialize_vfs()) {
    return 2;
  }
  if (!engine::core::mount("mat", ".")) {
    engine::core::shutdown_vfs();
    return 3;
  }

  engine::content::clear_asset_catalog(g_catalog);
  int result = check_live_slot_edit_moves_edges(database.get());
  if (result == 0) {
    database.reset(new (std::nothrow) engine::renderer::AssetDatabase());
    engine::content::clear_asset_catalog(g_catalog);
    result =
        (database == nullptr) ? 4 : check_grandchild_follows(database.get());
  }

  remove_files();
  engine::core::shutdown_vfs();
  if (result != 0) {
    std::fprintf(stderr, "material_live_edges_test failed: %d\n", result);
    return result;
  }
  std::printf("material_live_edges_test: all tests passed\n");
  return 0;
}
