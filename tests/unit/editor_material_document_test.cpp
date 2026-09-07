// Verifies the material editor as a document of its own (regression for
// #449): its edits live on its own history and never read as scene
// dirtiness; a scene save leaves an unsaved material unsaved; the quit
// gate covers both documents and Save from the quit prompt persists both,
// while a material save failure keeps the prompt armed; closing or
// switching a dirty material is gated by Save/Discard/Cancel where Discard
// reverts the live record to the file; dirtiness follows undo/redo across
// the saved position; a scene switch leaves the material document alone;
// and undo/redo route to the panel while it is the undo target. Every
// check drives the production functions the panels call, not a copy.

#include "editor_commands.h"
#include "editor_material_edit.h"
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
#include <system_error>

namespace {

using namespace engine::editor;
using namespace engine::runtime;

constexpr const char *kMountPrefix = "edmatdoc";
constexpr const char *kOsPathA = "editor_material_document_a.json";
constexpr const char *kOsPathB = "editor_material_document_b.json";
constexpr const char *kVirtualPathA = "edmatdoc/editor_material_document_a.json";
constexpr const char *kVirtualPathB = "edmatdoc/editor_material_document_b.json";
constexpr const char *kScenePath = "editor_material_document_scene.json";
constexpr const char *kMaterialA = "{\"version\":2,\"roughness\":0.3}";
constexpr const char *kMaterialB = "{\"version\":2,\"roughness\":0.6}";

bool exactly_equal(float lhs, float rhs) noexcept { return lhs == rhs; }

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

/// Roughness the file on disk holds, read through the production reload
/// (which also makes the live record match the file again).
float roughness_on_disk(const char *virtualPath) noexcept {
  return editor_reload_material(virtualPath).params.roughness;
}

/// Roughness the live database record holds, without touching disk.
float roughness_live(const char *virtualPath) noexcept {
  return editor_load_material(virtualPath).params.roughness;
}

/// One material-edit gesture that ends immediately: the value lands in
/// the live record and one command lands on the material history.
void edit_roughness(float value) noexcept {
  MaterialEditorState &state = material_editor_state();
  const engine::renderer::Material before = state.buffer;
  const engine::renderer::MaterialTextureSlots beforeSlots = state.textureSlots;
  state.buffer.roughness = value;
  material_editor_apply_frame(before, beforeSlots, true, false);
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

/// Fresh fixture per check: both material files rewritten, a fresh asset
/// database published as the editor asset service, a fresh World bound as
/// the (untitled, clean) scene document. Everything is torn down on
/// destruction so no check leaks process-wide state into the next.
struct DocumentScope final {
  std::unique_ptr<engine::renderer::AssetDatabase> database;
  EngineAssetDatabaseService service{};
  std::unique_ptr<World> world;

  DocumentScope() noexcept
      : database(new (std::nothrow) engine::renderer::AssetDatabase()),
        world(new (std::nothrow) World()) {
    static_cast<void>(write_file(kOsPathA, kMaterialA));
    static_cast<void>(write_file(kOsPathB, kMaterialB));
    service.database = database.get();
    set_editor_asset_service(&service);
    editor_set_world(world.get());
  }

  ~DocumentScope() noexcept {
    scene_document_prompt_choose_cancel();
    material_editor_prompt_choose_cancel();
    reset_material_editor();
    editor_session().commandHistory.clear();
    editor_set_world(nullptr);
    set_editor_asset_service(nullptr);
    std::error_code ec{};
    std::filesystem::remove_all(kOsPathA, ec);
    std::filesystem::remove_all(kOsPathB, ec);
    static_cast<void>(std::remove(kScenePath));
  }

  bool valid() const noexcept {
    return (database != nullptr) && (world != nullptr);
  }

  /// Gives the scene document a path and a clean saved state on disk.
  bool title_scene() const noexcept {
    SceneDocumentState &doc = editor_session().document;
    std::snprintf(doc.path, sizeof(doc.path), "%s", kScenePath);
    doc.hasPath = true;
    return perform_scene_save();
  }
};

/// EXPECTATION: a material edit is a material-document change only: the
/// scene document stays clean and its history empty, while the material
/// document is dirty with the command on its own history. Red on base:
/// the command entered the shared history and dirtied the scene.
int check_material_edit_is_not_scene_dirtiness() noexcept {
  DocumentScope scope;
  if (!scope.valid() || !scope.title_scene()) {
    return 1;
  }
  open_material_editor(kVirtualPathA);
  if (!material_editor_state().found || material_editor_is_dirty()) {
    return 2;
  }
  edit_roughness(0.77F);
  if (!material_editor_is_dirty() || !material_editor_history().can_undo()) {
    return 3;
  }
  if (scene_document_is_dirty() || editor_session().commandHistory.can_undo()) {
    return 4;
  }
  return 0;
}

/// EXPECTATION: the quit gate defers while only the material is unsaved,
/// a scene save does not clear that gate, and Save from the quit prompt
/// persists the material and lets the quit continue. Red on base: the
/// scene save advanced the shared saved token, the gate read clean, and
/// the material file kept its old value.
int check_scene_save_never_clears_material_dirtiness() noexcept {
  DocumentScope scope;
  if (!scope.valid() || !scope.title_scene()) {
    return 1;
  }
  open_material_editor(kVirtualPathA);
  edit_roughness(0.77F);
  if (request_scene_quit() || !scene_document_prompt_open() ||
      !scene_document_prompt_covers_material()) {
    return 2;
  }
  scene_document_prompt_choose_cancel();
  if (scene_document_prompt_open()) {
    return 3;
  }

  if (!perform_scene_save()) {
    return 4;
  }
  if (request_scene_quit()) {
    return 5; // the scene save must not have cleared the material's gate
  }
  if (!material_editor_is_dirty()) {
    return 6;
  }

  scene_document_prompt_choose_save();
  if (scene_document_prompt_open() || material_editor_is_dirty()) {
    return 7;
  }
  if (!exactly_equal(roughness_on_disk(kVirtualPathA), 0.77F)) {
    return 8;
  }
  return 0;
}

/// EXPECTATION: with both documents dirty, Save from the quit prompt
/// persists both (scene entity on disk, material value on disk) and the
/// quit continues; Cancel beforehand leaves both dirty and untouched.
int check_quit_save_persists_both_documents() noexcept {
  DocumentScope scope;
  if (!scope.valid() || !scope.title_scene()) {
    return 1;
  }
  const Entity entity = add_named_entity(*scope.world, "Kept");
  if ((entity == kInvalidEntity) ||
      !push_transform_edit(*scope.world, entity)) {
    return 2;
  }
  open_material_editor(kVirtualPathA);
  edit_roughness(0.77F);
  if (!scene_document_is_dirty() || !material_editor_is_dirty()) {
    return 3;
  }

  if (request_scene_quit() || !scene_document_prompt_covers_material()) {
    return 4;
  }
  scene_document_prompt_choose_cancel();
  if (scene_document_prompt_open() || !scene_document_is_dirty() ||
      !material_editor_is_dirty() ||
      !exactly_equal(roughness_live(kVirtualPathA), 0.77F)) {
    return 5;
  }

  if (request_scene_quit()) {
    return 6;
  }
  scene_document_prompt_choose_save();
  if (scene_document_prompt_open() || scene_document_is_dirty() ||
      material_editor_is_dirty()) {
    return 7;
  }
  if (!exactly_equal(roughness_on_disk(kVirtualPathA), 0.77F)) {
    return 8;
  }
  std::unique_ptr<World> verify(new (std::nothrow) World());
  if ((verify == nullptr) || !load_scene(*verify, kScenePath) ||
      (verify->find_entity_by_name("Kept") == kInvalidEntity)) {
    return 9;
  }
  return 0;
}

/// EXPECTATION: Discard from the quit prompt continues the quit without
/// writing the material: the file keeps its authored value.
int check_quit_discard_is_explicit() noexcept {
  DocumentScope scope;
  if (!scope.valid() || !scope.title_scene()) {
    return 1;
  }
  open_material_editor(kVirtualPathA);
  edit_roughness(0.77F);
  if (request_scene_quit() || !scene_document_prompt_open()) {
    return 2;
  }
  scene_document_prompt_choose_discard();
  if (scene_document_prompt_open()) {
    return 3;
  }
  if (!exactly_equal(roughness_on_disk(kVirtualPathA), 0.3F)) {
    return 4; // Discard never writes
  }
  return 0;
}

/// EXPECTATION: a material save failure keeps the quit prompt armed with
/// the material's error, the material stays dirty, and nothing quits;
/// once the destination is writable again Save succeeds. The failure is
/// injected by replacing the material file with a directory, which no
/// atomic replacement can rename over.
int check_material_save_failure_blocks_quit() noexcept {
  DocumentScope scope;
  if (!scope.valid() || !scope.title_scene()) {
    return 1;
  }
  open_material_editor(kVirtualPathA);
  edit_roughness(0.77F);

  std::error_code ec{};
  std::filesystem::remove(kOsPathA, ec);
  std::filesystem::create_directory(kOsPathA, ec);
  if (ec) {
    return 2;
  }

  if (request_scene_quit()) {
    return 3;
  }
  scene_document_prompt_choose_save();
  if (!scene_document_prompt_open() || !material_editor_is_dirty() ||
      (scene_document_last_error()[0] == '\0') ||
      (material_editor_state().lastSaveError[0] == '\0')) {
    return 4;
  }

  std::filesystem::remove(kOsPathA, ec);
  scene_document_prompt_choose_save();
  if (scene_document_prompt_open() || material_editor_is_dirty() ||
      (scene_document_last_error()[0] != '\0')) {
    return 5;
  }
  if (!exactly_equal(roughness_on_disk(kVirtualPathA), 0.77F)) {
    return 6;
  }
  return 0;
}

/// EXPECTATION: opening another material while the open one is dirty
/// arms the material prompt and keeps the dirty one open. Cancel leaves
/// it dirty and open; Discard reverts its live record to the file and
/// then opens the other, clean, with an empty history; Save persists it
/// and then switches.
int check_switching_dirty_material_is_gated() noexcept {
  DocumentScope scope;
  if (!scope.valid()) {
    return 1;
  }
  MaterialEditorState &state = material_editor_state();
  open_material_editor(kVirtualPathA);
  edit_roughness(0.77F);

  open_material_editor(kVirtualPathB);
  if (!material_editor_prompt_open() ||
      (std::strcmp(state.virtualPath, kVirtualPathA) != 0) ||
      !material_editor_is_dirty()) {
    return 2;
  }
  // A second request while the prompt is up must not replace the pending
  // action or the open material.
  open_material_editor(kVirtualPathB);
  material_editor_prompt_choose_cancel();
  if (material_editor_prompt_open() ||
      (std::strcmp(state.virtualPath, kVirtualPathA) != 0) ||
      !material_editor_is_dirty() ||
      !exactly_equal(roughness_live(kVirtualPathA), 0.77F)) {
    return 3;
  }

  open_material_editor(kVirtualPathB);
  material_editor_prompt_choose_discard();
  if (material_editor_prompt_open() ||
      (std::strcmp(state.virtualPath, kVirtualPathB) != 0) ||
      material_editor_is_dirty() || material_editor_history().can_undo()) {
    return 4;
  }
  if (!exactly_equal(roughness_live(kVirtualPathA), 0.3F) ||
      !exactly_equal(state.buffer.roughness, 0.6F)) {
    return 5; // the discarded edit was reverted, not left live
  }

  edit_roughness(0.9F);
  open_material_editor(kVirtualPathA);
  if (!material_editor_prompt_open()) {
    return 6;
  }
  material_editor_prompt_choose_save();
  if (material_editor_prompt_open() ||
      (std::strcmp(state.virtualPath, kVirtualPathA) != 0) ||
      material_editor_is_dirty()) {
    return 7;
  }
  if (!exactly_equal(roughness_on_disk(kVirtualPathB), 0.9F)) {
    return 8;
  }
  return 0;
}

/// EXPECTATION: closing a dirty material is gated the same way: Cancel
/// keeps it open and dirty, Discard reverts the record and closes, Save
/// persists and closes; a clean material closes at once.
int check_closing_dirty_material_is_gated() noexcept {
  DocumentScope scope;
  if (!scope.valid()) {
    return 1;
  }
  MaterialEditorState &state = material_editor_state();
  open_material_editor(kVirtualPathA);
  request_close_material_editor();
  if (state.open || material_editor_prompt_open()) {
    return 2; // clean: closed immediately
  }

  open_material_editor(kVirtualPathA);
  edit_roughness(0.77F);
  request_close_material_editor();
  if (!state.open || !material_editor_prompt_open()) {
    return 3;
  }
  material_editor_prompt_choose_cancel();
  if (!state.open || material_editor_prompt_open() ||
      !material_editor_is_dirty()) {
    return 4;
  }

  request_close_material_editor();
  material_editor_prompt_choose_discard();
  if (state.open || material_editor_prompt_open() ||
      material_editor_history().can_undo() ||
      !exactly_equal(roughness_live(kVirtualPathA), 0.3F)) {
    return 5;
  }

  open_material_editor(kVirtualPathA);
  edit_roughness(0.55F);
  request_close_material_editor();
  material_editor_prompt_choose_save();
  if (state.open || material_editor_prompt_open() ||
      !exactly_equal(roughness_on_disk(kVirtualPathA), 0.55F)) {
    return 6;
  }
  return 0;
}

/// EXPECTATION: dirtiness is the distance from the saved history
/// position: undo past a save re-dirties, redo back to it re-cleans, and
/// a save after further edits moves the position.
int check_dirtiness_follows_undo_across_saved_position() noexcept {
  DocumentScope scope;
  if (!scope.valid()) {
    return 1;
  }
  open_material_editor(kVirtualPathA);
  edit_roughness(0.4F);
  if (!save_material_editor() || material_editor_is_dirty()) {
    return 2;
  }
  if (!material_editor_history().undo() || !material_editor_is_dirty() ||
      !exactly_equal(roughness_live(kVirtualPathA), 0.3F)) {
    return 3;
  }
  if (!material_editor_history().redo() || material_editor_is_dirty() ||
      !exactly_equal(roughness_live(kVirtualPathA), 0.4F)) {
    return 4;
  }

  edit_roughness(0.5F);
  edit_roughness(0.6F);
  if (!material_editor_is_dirty() || !save_material_editor() ||
      material_editor_is_dirty()) {
    return 5;
  }
  if (!material_editor_history().undo() || !material_editor_history().undo() ||
      !material_editor_is_dirty()) {
    return 6;
  }
  if (!material_editor_history().redo() || !material_editor_is_dirty()) {
    return 7; // one step short of the saved position
  }
  if (!material_editor_history().redo() || material_editor_is_dirty()) {
    return 8;
  }
  // Reload marks the current position saved without touching the stack.
  if (!material_editor_history().undo() || !reload_material_editor_from_disk() ||
      material_editor_is_dirty() || !material_editor_history().can_redo()) {
    return 9;
  }
  return 0;
}

/// EXPECTATION: a scene switch (New) replaces the scene document only:
/// the material stays open, dirty, with its history intact, and the quit
/// gate still covers it afterwards.
int check_scene_switch_leaves_material_document_alone() noexcept {
  DocumentScope scope;
  if (!scope.valid() || !scope.title_scene()) {
    return 1;
  }
  open_material_editor(kVirtualPathA);
  edit_roughness(0.77F);
  if (!perform_scene_new()) {
    return 2;
  }
  if (!material_editor_state().open || !material_editor_is_dirty() ||
      !material_editor_history().can_undo() ||
      !exactly_equal(roughness_live(kVirtualPathA), 0.77F)) {
    return 3;
  }
  if (request_scene_quit() || !scene_document_prompt_covers_material()) {
    return 4;
  }
  return 0;
}

/// EXPECTATION: undo/redo address the material history while the panel is
/// the undo target and the scene history otherwise, and each document's
/// stack is left alone by the other's undo.
int check_undo_routes_to_the_target_document() noexcept {
  DocumentScope scope;
  if (!scope.valid()) {
    return 1;
  }
  const Entity entity = add_named_entity(*scope.world, "Routed");
  if ((entity == kInvalidEntity) ||
      !push_transform_edit(*scope.world, entity)) {
    return 2;
  }
  open_material_editor(kVirtualPathA);
  edit_roughness(0.77F);
  MaterialEditorState &state = material_editor_state();

  state.undoTarget = true;
  if (!editor_history_can_undo()) {
    return 3;
  }
  editor_history_undo();
  if (material_editor_history().can_undo() ||
      !editor_session().commandHistory.can_undo() ||
      !exactly_equal(roughness_live(kVirtualPathA), 0.3F)) {
    return 4;
  }
  editor_history_redo();
  if (!material_editor_history().can_undo() ||
      !exactly_equal(roughness_live(kVirtualPathA), 0.77F)) {
    return 5;
  }

  state.undoTarget = false;
  editor_history_undo();
  if (editor_session().commandHistory.can_undo() ||
      !material_editor_history().can_undo()) {
    return 6;
  }
  if (!editor_history_can_redo()) {
    return 7;
  }
  editor_history_redo();
  if (!editor_session().commandHistory.can_undo()) {
    return 8;
  }

  // A closed panel is never the target, whatever the latch says.
  state.undoTarget = true;
  close_material_editor();
  if (editor_history_can_undo() != editor_session().commandHistory.can_undo()) {
    return 9;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_vfs()) {
    return 1;
  }
  if (!engine::core::mount(kMountPrefix, ".")) {
    engine::core::shutdown_vfs();
    return 2;
  }

  struct NamedCheck {
    const char *name;
    int (*fn)() noexcept;
  };
  const NamedCheck checks[] = {
      {"check_material_edit_is_not_scene_dirtiness",
       &check_material_edit_is_not_scene_dirtiness},
      {"check_scene_save_never_clears_material_dirtiness",
       &check_scene_save_never_clears_material_dirtiness},
      {"check_quit_save_persists_both_documents",
       &check_quit_save_persists_both_documents},
      {"check_quit_discard_is_explicit", &check_quit_discard_is_explicit},
      {"check_material_save_failure_blocks_quit",
       &check_material_save_failure_blocks_quit},
      {"check_switching_dirty_material_is_gated",
       &check_switching_dirty_material_is_gated},
      {"check_closing_dirty_material_is_gated",
       &check_closing_dirty_material_is_gated},
      {"check_dirtiness_follows_undo_across_saved_position",
       &check_dirtiness_follows_undo_across_saved_position},
      {"check_scene_switch_leaves_material_document_alone",
       &check_scene_switch_leaves_material_document_alone},
      {"check_undo_routes_to_the_target_document",
       &check_undo_routes_to_the_target_document},
  };

  int result = 0;
  for (const auto &check : checks) {
    result = check.fn();
    if (result != 0) {
      std::fprintf(stderr, "editor_material_document_test: %s failed: %d\n",
                   check.name, result);
      break;
    }
  }

  engine::core::shutdown_vfs();
  if (result == 0) {
    std::printf("editor_material_document_test: all tests passed\n");
  }
  return result;
}
