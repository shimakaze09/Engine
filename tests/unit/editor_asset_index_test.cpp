// Verifies the content-browser asset index (issue #157): cold rebuild
// classifies every file kind correctly (including content-sniffed
// ambiguous ".json" scene/material/animation-controller documents) and
// skips sidecar/internal files; the filter cache only recomputes on an
// actual filter or generation change and handles the empty-query and
// no-match boundaries; typed-action kind routing is pure and correct; and
// execute_asset_open dispatches through real production entry points —
// scene Open routes through the #158 unsaved-change gate and a mesh Open
// spawns through execute_asset_spawn, not a copied model of either.

#include "editor_asset_index.h"
#include "editor_commands.h"
#include "editor_scene_document.h"
#include "editor_session.h"
#include "engine/core/vfs.h"
#include "engine/editor/editor.h"
#include "engine/renderer/asset_database.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace {

using namespace engine::editor;
using namespace engine::runtime;

/// Scratch root nested under the real editor asset root (default
/// "assets"), left in the same plain relative form rebuild_asset_index's
/// walk uses (root = active_config().editorAssetRoot, uncanonicalized) so
/// AssetIndexEntry::folder comparisons in the filter-cache checks match
/// byte-for-byte; perform_scene_save_as's jail check canonicalizes
/// internally, so the relative form works there too.
bool scratch_root(char *out, std::size_t capacity) noexcept {
  const int written =
      std::snprintf(out, capacity, "%s", "assets/engine_asset_index_test");
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

bool make_scratch_path(const char *leaf, char *out,
                       std::size_t capacity) noexcept {
  char root[900] = {};
  if (!scratch_root(root, sizeof(root))) {
    return false;
  }
  const int written = std::snprintf(out, capacity, "%s/%s", root, leaf);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

bool write_text_file(const char *path, const char *text) noexcept {
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
  const std::size_t length = std::strlen(text);
  const std::size_t written = std::fwrite(text, 1U, length, file);
  std::fclose(file);
  return written == length;
}

/// Recreates a clean scratch tree with one file of each ambiguous/
/// unambiguous kind used by the classification and filter checks below.
bool rebuild_scratch_tree() noexcept {
  char root[900] = {};
  if (!scratch_root(root, sizeof(root))) {
    return false;
  }
  std::error_code ec{};
  std::filesystem::remove_all(std::filesystem::path(root), ec);
  std::filesystem::create_directories(std::filesystem::path(root), ec);
  std::filesystem::create_directories(
      std::filesystem::path(root) / "sub", ec);
  if (ec) {
    return false;
  }

  char meshPath[1024] = {};
  char texPath[1024] = {};
  char scriptPath[1024] = {};
  char scenePath[1024] = {};
  char materialPath[1024] = {};
  char controllerPath[1024] = {};
  char metaPath[1024] = {};
  char subMeshPath[1024] = {};
  if (!make_scratch_path("thing.mesh", meshPath, sizeof(meshPath)) ||
      !make_scratch_path("thing.png", texPath, sizeof(texPath)) ||
      !make_scratch_path("thing.lua", scriptPath, sizeof(scriptPath)) ||
      !make_scratch_path("thing_scene.json", scenePath, sizeof(scenePath)) ||
      !make_scratch_path("thing_material.json", materialPath,
                         sizeof(materialPath)) ||
      !make_scratch_path("thing.animctrl.json", controllerPath,
                         sizeof(controllerPath)) ||
      !make_scratch_path("thing.mesh.meta.json", metaPath,
                         sizeof(metaPath)) ||
      !make_scratch_path("sub/nested.mesh", subMeshPath,
                         sizeof(subMeshPath))) {
    return false;
  }

  return write_text_file(meshPath, "not a real mesh, kind is by extension") &&
         write_text_file(texPath, "not a real png, kind is by extension") &&
         write_text_file(scriptPath, "-- lua\n") &&
         write_text_file(scenePath, "{\"entities\":[],\"version\":1}") &&
         write_text_file(materialPath,
                         "{\"version\":1,\"albedo\":[1,1,1]}") &&
         write_text_file(controllerPath,
                         "{\"states\":{},\"clips\":{},\"initial\":\"idle\"}") &&
         write_text_file(metaPath, "{\"importSettings\":{}}") &&
         write_text_file(subMeshPath, "nested mesh");
}

/// Finds the index entry whose osPath ends with `leaf`; nullptr if absent.
const AssetIndexEntry *find_entry_by_leaf(const char *leaf) noexcept {
  const std::size_t count = asset_index_count();
  for (std::size_t i = 0U; i < count; ++i) {
    const AssetIndexEntry *entry = asset_index_entry(i);
    if (entry == nullptr) {
      continue;
    }
    std::string osPath(entry->osPath);
    for (char &c : osPath) {
      if (c == '\\') {
        c = '/';
      }
    }
    const std::string suffix(leaf);
    if ((osPath.size() >= suffix.size()) &&
        (osPath.compare(osPath.size() - suffix.size(), suffix.size(),
                        suffix) == 0)) {
      return entry;
    }
  }
  return nullptr;
}

/// EXPECTATION: rebuild_asset_index classifies every scratch file kind
/// correctly (including content-sniffed ambiguous .json files), hides the
/// .meta.json sidecar from the index, and bumps the generation counter.
int check_rebuild_classifies_and_hides_sidecars() {
  if (!rebuild_scratch_tree()) {
    return 1;
  }

  const std::uint64_t genBefore = asset_index_generation();
  if (!rebuild_asset_index()) {
    return 2; // the real "assets" root must exist for this repo's tests
  }
  if (asset_index_generation() == genBefore) {
    return 3; // generation must bump on every rebuild
  }
  if (!asset_index_built()) {
    return 4;
  }

  const AssetIndexEntry *mesh = find_entry_by_leaf("thing.mesh");
  const AssetIndexEntry *tex = find_entry_by_leaf("thing.png");
  const AssetIndexEntry *script = find_entry_by_leaf("thing.lua");
  const AssetIndexEntry *scene = find_entry_by_leaf("thing_scene.json");
  const AssetIndexEntry *material = find_entry_by_leaf("thing_material.json");
  const AssetIndexEntry *controller = find_entry_by_leaf("thing.animctrl.json");
  const AssetIndexEntry *nested = find_entry_by_leaf("sub/nested.mesh");
  const AssetIndexEntry *meta = find_entry_by_leaf("thing.mesh.meta.json");

  if ((mesh == nullptr) || (mesh->kind != AssetKind::Mesh)) {
    return 5;
  }
  if ((tex == nullptr) || (tex->kind != AssetKind::Texture)) {
    return 6;
  }
  if ((script == nullptr) || (script->kind != AssetKind::Script)) {
    return 7;
  }
  if ((scene == nullptr) || (scene->kind != AssetKind::Scene)) {
    return 8;
  }
  if ((material == nullptr) || (material->kind != AssetKind::Material)) {
    return 9;
  }
  if ((controller == nullptr) ||
      (controller->kind != AssetKind::AnimationController)) {
    return 10;
  }
  if ((nested == nullptr) || (nested->kind != AssetKind::Mesh)) {
    return 11;
  }
  if (meta != nullptr) {
    return 12; // .meta.json sidecars must never appear in the index
  }
  if (mesh->virtualPath[0] == '\0') {
    return 13; // must resolve a VFS virtual path under the mount root
  }
  return 0;
}

/// EXPECTATION: classify_asset_kind is consistent standalone (exposed for
/// direct testing, not only through a full rebuild).
int check_classify_asset_kind_direct() {
  if (classify_asset_kind("x.mesh") != AssetKind::Mesh) {
    return 1;
  }
  if (classify_asset_kind("x.WAV") != AssetKind::Sound) {
    return 2; // extension match is case-insensitive
  }
  if (classify_asset_kind("x.skel") != AssetKind::Animation) {
    return 3;
  }
  if (classify_asset_kind("x.unknownext") != AssetKind::Other) {
    return 4;
  }
  return 0;
}

/// EXPECTATION: refresh_asset_filter_cache only recomputes when the filter
/// or the index generation actually changed, and the empty-query /
/// no-match boundaries behave correctly.
int check_filter_cache_change_driven_and_boundaries() {
  if (!rebuild_scratch_tree() || !rebuild_asset_index()) {
    return 1;
  }

  char scratchFolder[kMaxAssetIndexPath] = {};
  if (!scratch_root(scratchFolder, sizeof(scratchFolder))) {
    return 2;
  }

  AssetFilterState filter{};
  std::snprintf(filter.folder, sizeof(filter.folder), "%s", scratchFolder);
  filter.flatSearch = false;
  filter.query[0] = '\0'; // empty query: every type-matching entry in scope

  AssetFilterCache cache{};
  if (!refresh_asset_filter_cache(filter, &cache)) {
    return 3; // first apply must always recompute
  }
  const std::size_t emptyQueryMatches = cache.matches.size();
  if (emptyQueryMatches == 0U) {
    return 4; // the scratch root's direct children must be included
  }

  // Re-applying the identical filter against an unchanged index must not
  // recompute (the change-driven cache contract).
  if (refresh_asset_filter_cache(filter, &cache)) {
    return 5;
  }
  if (cache.matches.size() != emptyQueryMatches) {
    return 6;
  }

  // A query with no possible match narrows to zero without erroring.
  std::snprintf(filter.query, sizeof(filter.query), "%s",
               "zzz_definitely_not_present_zzz");
  if (!refresh_asset_filter_cache(filter, &cache)) {
    return 7; // filter changed: must recompute
  }
  if (!cache.matches.empty()) {
    return 8;
  }

  // A query matching exactly one scratch file narrows correctly.
  std::snprintf(filter.query, sizeof(filter.query), "%s", "thing.mesh");
  if (!refresh_asset_filter_cache(filter, &cache)) {
    return 9;
  }
  if (cache.matches.size() != 1U) {
    return 10;
  }
  const AssetIndexEntry *matched = asset_index_entry(cache.matches[0]);
  if ((matched == nullptr) || (matched->kind != AssetKind::Mesh)) {
    return 11;
  }

  // A rebuild bumps the generation; the identical filter must recompute
  // even though nothing about the filter itself changed.
  if (!rebuild_asset_index()) {
    return 12;
  }
  if (!refresh_asset_filter_cache(filter, &cache)) {
    return 13;
  }
  return 0;
}

/// EXPECTATION: resolve_asset_open_action's kind->action mapping is exact.
int check_resolve_asset_open_action_mapping() {
  if (resolve_asset_open_action(AssetKind::Mesh) !=
      AssetOpenAction::SpawnMesh) {
    return 1;
  }
  if (resolve_asset_open_action(AssetKind::Scene) !=
      AssetOpenAction::OpenScene) {
    return 2;
  }
  // Issue #160: Material's typed Open now routes to the material editor
  // panel instead of merely selecting the asset.
  if (resolve_asset_open_action(AssetKind::Material) !=
      AssetOpenAction::EditMaterial) {
    return 4;
  }
  const AssetKind selectOnlyKinds[] = {
      AssetKind::Texture,   AssetKind::Script,
      AssetKind::Animation, AssetKind::AnimationController,
      AssetKind::Sound,     AssetKind::Other,
  };
  for (const AssetKind kind : selectOnlyKinds) {
    if (resolve_asset_open_action(kind) != AssetOpenAction::SelectOnly) {
      return 3;
    }
  }
  return 0;
}

Entity add_named_entity(World &world, const char *name) noexcept {
  const Entity entity = world.create_scene_object();
  if (entity == kInvalidEntity) {
    return kInvalidEntity;
  }
  NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  if (!world.add_name_component(entity, nameComponent)) {
    return kInvalidEntity;
  }
  return entity;
}

bool push_transform_edit(World &world, Entity entity) noexcept {
  auto *command = new (std::nothrow) TransformEditCommand();
  if (command == nullptr) {
    return false;
  }
  command->entity = entity;
  command->persistentId = world.persistent_id(entity);
  command->oldTransform = Transform{};
  command->newTransform.position = engine::math::Vec3(1.0F, 0.0F, 0.0F);
  return editor_session().commandHistory.execute(command);
}

/// EXPECTATION: execute_asset_open for a Scene-kind entry routes through
/// the real #158 unsaved-change gate — a dirty document defers (arms the
/// Save/Discard/Cancel prompt) instead of switching immediately, and
/// choosing Discard then completes the deferred open through the same
/// production request_scene_open/perform_scene_open path the File menu
/// uses.
int check_scene_open_routes_through_unsaved_gate() {
  if (!rebuild_scratch_tree()) {
    return 1;
  }
  char currentPath[512] = {};
  char targetPath[512] = {};
  if (!make_scratch_path("gate_current.json", currentPath,
                         sizeof(currentPath)) ||
      !make_scratch_path("gate_target.json", targetPath,
                         sizeof(targetPath))) {
    return 2;
  }
  static_cast<void>(std::remove(currentPath));
  static_cast<void>(std::remove(targetPath));

  std::unique_ptr<World> targetWriter(new (std::nothrow) World());
  if ((targetWriter == nullptr) ||
      (add_named_entity(*targetWriter, "TargetEntity") == kInvalidEntity) ||
      !save_scene(*targetWriter, targetPath)) {
    return 3;
  }

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 4;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "CurrentEntity");
  if ((entity == kInvalidEntity) || !perform_scene_save_as(currentPath) ||
      !push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 5;
  }
  if (!scene_document_is_dirty()) {
    editor_set_world(nullptr);
    return 6;
  }

  AssetIndexEntry targetEntry{};
  targetEntry.kind = AssetKind::Scene;
  std::snprintf(targetEntry.osPath, sizeof(targetEntry.osPath), "%s",
               targetPath);
  std::snprintf(targetEntry.name, sizeof(targetEntry.name), "gate_target.json");

  if (!execute_asset_open(targetEntry)) {
    editor_set_world(nullptr);
    return 7;
  }
  // Dirty document: the open must have deferred behind the prompt rather
  // than switching immediately.
  if (!scene_document_prompt_open() ||
      (std::strcmp(scene_document_path(), currentPath) != 0)) {
    editor_set_world(nullptr);
    return 8;
  }
  if (std::strcmp(editor_session().selectedAssetPath, targetPath) != 0) {
    editor_set_world(nullptr);
    return 9; // selection updates immediately even while the open defers
  }

  scene_document_prompt_choose_discard();
  const bool ok = !scene_document_prompt_open() &&
                  scene_document_has_path() &&
                  (std::strcmp(scene_document_path(), targetPath) == 0) &&
                  (world->find_entity_by_name("TargetEntity") !=
                   kInvalidEntity);
  editor_set_world(nullptr);
  return ok ? 0 : 10;
}

/// EXPECTATION: execute_asset_open for a Mesh-kind entry dispatches
/// through the real execute_asset_spawn production entry point (not a
/// copy), creating a mesh entity and updating the selection.
int check_mesh_open_spawns_through_production_path() {
  constexpr const char *kMountPrefix = "asset_index_test_mount";

  if (!engine::core::initialize_vfs()) {
    return 1;
  }
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<World> world(new (std::nothrow) World());
  if ((database == nullptr) || (world == nullptr)) {
    engine::core::shutdown_vfs();
    return 2;
  }

  const auto finish = [](int result) noexcept {
    editor_set_world(nullptr);
    set_editor_asset_service(nullptr);
    engine::core::shutdown_vfs();
    return result;
  };

  if (!engine::core::mount(kMountPrefix, ".")) {
    engine::core::shutdown_vfs();
    return 3;
  }
  EngineAssetDatabaseService service{};
  service.database = database.get();
  set_editor_asset_service(&service);
  editor_set_world(world.get());

  AssetIndexEntry entry{};
  entry.kind = AssetKind::Mesh;
  std::snprintf(entry.virtualPath, sizeof(entry.virtualPath),
               "%s/thing.mesh", kMountPrefix);
  std::snprintf(entry.osPath, sizeof(entry.osPath), "thing.mesh");
  std::snprintf(entry.name, sizeof(entry.name), "thing.mesh");

  const std::size_t before = world->alive_entity_count();
  if (!execute_asset_open(entry)) {
    return finish(4);
  }
  if (world->alive_entity_count() <= before) {
    return finish(5);
  }
  if (std::strcmp(editor_session().selectedAssetPath, entry.osPath) != 0) {
    return finish(6);
  }
  return finish(0);
}

} // namespace

/// Runs this executable or test program.
int main() {
  struct NamedCheck {
    const char *name;
    int (*fn)();
  };
  const NamedCheck checks[] = {
      {"check_rebuild_classifies_and_hides_sidecars",
       &check_rebuild_classifies_and_hides_sidecars},
      {"check_classify_asset_kind_direct", &check_classify_asset_kind_direct},
      {"check_filter_cache_change_driven_and_boundaries",
       &check_filter_cache_change_driven_and_boundaries},
      {"check_resolve_asset_open_action_mapping",
       &check_resolve_asset_open_action_mapping},
      {"check_scene_open_routes_through_unsaved_gate",
       &check_scene_open_routes_through_unsaved_gate},
      {"check_mesh_open_spawns_through_production_path",
       &check_mesh_open_spawns_through_production_path},
  };

  for (const auto &check : checks) {
    const int result = check.fn();
    if (result != 0) {
      std::fprintf(stderr, "editor_asset_index_test: %s failed: %d\n",
                   check.name, result);
      return result;
    }
  }

  std::printf("editor_asset_index_test: all tests passed\n");
  return 0;
}
