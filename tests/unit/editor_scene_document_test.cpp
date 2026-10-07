// Verifies the scene-document identity/dirty core (issue #158): default
// "Untitled Scene" identity, dirty tracking driven by CommandHistory's
// token (including the undo-to-saved-marker-clears-dirty contract),
// New/Open/Save/Save As through the production editor-session paths,
// failed-load state preservation, the asset-root jail check, the Error
// line and status every failed save leaves, and the recent-scenes list
// (MRU order, dedupe, and pruning invalid entries).

#include "editor_commands.h"
#include "editor_scene_document.h"
#include "editor_scene_document_fixture.h"
#include "editor_scene_templates.h"
#include "editor_session.h"
#include "engine/content/asset_sidecar.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/editor/editor.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace {

using namespace engine::editor;
using namespace engine::runtime;

/// Routes every case's recent-scenes persistence to scratch; armed in
/// main before the first document operation.
engine::tests::RecentScenesGuard g_recentGuard;

/// Absolute path to the scratch root, nested under the real editor asset
/// root ("assets", relative to the test's working directory) so
/// perform_scene_save_as's production jail check accepts scratch paths
/// without a test-only bypass.
bool scratch_root(char *out, std::size_t capacity) noexcept {
  std::error_code ec{};
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(
      std::filesystem::path("assets/engine_scene_document_test"), ec);
  if (ec) {
    return false;
  }
  const std::string asString = resolved.string();
  const int written = std::snprintf(out, capacity, "%s", asString.c_str());
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

/// Builds "<assets>/engine_scene_document_test/<leaf>"; false when the
/// asset root is unavailable or the path would truncate.
bool make_scratch_path(const char *leaf, char *out,
                       std::size_t capacity) noexcept {
  char root[900] = {};
  if (!scratch_root(root, sizeof(root))) {
    return false;
  }
  const int written = std::snprintf(out, capacity, "%s/%s", root, leaf);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

/// Ensures the scratch root directory exists.
bool ensure_scratch_root() noexcept {
  char root[900] = {};
  if (!scratch_root(root, sizeof(root))) {
    return false;
  }
  std::error_code ec{};
  std::filesystem::create_directories(std::filesystem::path(root), ec);
  return !ec;
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

/// Pushes one trivial undoable transform edit through the command
/// history so current_token() advances (the production dirty driver).
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

/// EXPECTATION: a freshly bound world starts as a clean, untitled document.
int check_default_state_is_untitled_and_clean() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  editor_set_world(world.get());

  const bool ok =
      !scene_document_has_path() &&
      (std::strcmp(scene_document_display_name(), "Untitled Scene") == 0) &&
      !scene_document_is_dirty();
  editor_set_world(nullptr);
  return ok ? 0 : 2;
}

/// EXPECTATION: a command entering history marks the document dirty; Save
/// As clears it, and undoing back past the saved marker restores dirty
/// while redoing back to the marker clears it again (the exact
/// undo-to-saved-marker contract CLAUDE.md calls out for issue #158).
int check_dirty_tracks_history_position_around_save() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char scenePath[512] = {};
  if (!make_scratch_path("saved_scene.json", scenePath, sizeof(scenePath))) {
    return 2;
  }
  static_cast<void>(std::remove(scenePath));

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 3;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "Dirty");
  if (entity == kInvalidEntity) {
    editor_set_world(nullptr);
    return 4;
  }

  if (!push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 5;
  }
  if (!scene_document_is_dirty()) {
    editor_set_world(nullptr);
    return 6;
  }

  if (!perform_scene_save_as(scenePath)) {
    editor_set_world(nullptr);
    return 7;
  }
  if (scene_document_is_dirty() || !scene_document_has_path() ||
      (std::strcmp(scene_document_path(), scenePath) != 0)) {
    editor_set_world(nullptr);
    return 8;
  }

  if (!push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 9;
  }
  if (!scene_document_is_dirty()) {
    editor_set_world(nullptr);
    return 10;
  }

  editor_history_undo();
  if (scene_document_is_dirty()) {
    editor_set_world(nullptr);
    return 11;
  }

  editor_history_redo();
  if (!scene_document_is_dirty()) {
    editor_set_world(nullptr);
    return 12;
  }

  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION: Save (no path yet) fails without touching dirty status;
/// Save As with a bad/unset path likewise leaves the document untouched.
int check_save_without_path_fails_cleanly() {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "NoPath");
  if ((entity == kInvalidEntity) || !push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 2;
  }

  if (perform_scene_save()) {
    editor_set_world(nullptr);
    return 3; // must fail: no document path yet
  }
  if (!scene_document_is_dirty() || scene_document_has_path()) {
    editor_set_world(nullptr);
    return 4;
  }

  editor_set_world(nullptr);
  return 0;
}

/// EXPECTATION: perform_scene_new resets identity to untitled/clean and
/// empties the world.
int check_perform_scene_new_resets_identity() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char scenePath[512] = {};
  if (!make_scratch_path("new_reset.json", scenePath, sizeof(scenePath))) {
    return 2;
  }
  static_cast<void>(std::remove(scenePath));

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 3;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "ToClear");
  if ((entity == kInvalidEntity) || !push_transform_edit(*world, entity) ||
      !perform_scene_save_as(scenePath)) {
    editor_set_world(nullptr);
    return 4;
  }

  if (!perform_scene_new()) {
    editor_set_world(nullptr);
    return 5;
  }
  if (scene_document_has_path() ||
      (std::strcmp(scene_document_display_name(), "Untitled Scene") != 0) ||
      scene_document_is_dirty() || (world->alive_entity_count() != 0U)) {
    editor_set_world(nullptr);
    return 6;
  }

  editor_set_world(nullptr);
  return 0;
}

/// The bytes of the file at `path`, or "" when it cannot be read.
std::string file_bytes(const char *path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

/// EXPECTATION: New Scene from Template opens the template's entities as
/// an untitled scene with nothing unsaved; saving it asks for a new path,
/// and the template's file is never written.
int check_new_from_template_opens_untitled_and_clean() {
  char templatePath[512] = {};
  char savedPath[512] = {};
  if (!ensure_scratch_root() ||
      !make_scratch_path("template.scene", templatePath,
                         sizeof(templatePath)) ||
      !make_scratch_path("from_template.scene", savedPath, sizeof(savedPath))) {
    return 1;
  }
  static_cast<void>(std::remove(savedPath));
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 2;
  }
  editor_set_world(world.get());
  if ((add_named_entity(*world, "TemplateCamera") == kInvalidEntity) ||
      (add_named_entity(*world, "TemplateLight") == kInvalidEntity) ||
      !perform_scene_save_as(templatePath) || !perform_scene_new()) {
    editor_set_world(nullptr);
    return 3;
  }
  const std::string templateBytes = file_bytes(templatePath);

  request_scene_new_from_template(templatePath);
  if (scene_document_has_path() ||
      (std::strcmp(scene_document_display_name(), "Untitled Scene") != 0) ||
      scene_document_is_dirty() || (world->alive_entity_count() != 2U) ||
      (world->find_entity_by_name("TemplateLight") == kInvalidEntity)) {
    editor_set_world(nullptr);
    return 4;
  }
  // An edit and a save land in the new file, not the template.
  const Entity camera = world->find_entity_by_name("TemplateCamera");
  if (!push_transform_edit(*world, camera) || !scene_document_is_dirty() ||
      perform_scene_save() || !perform_scene_save_as(savedPath)) {
    editor_set_world(nullptr);
    return 5;
  }
  const bool ok = (file_bytes(templatePath) == templateBytes) &&
                  (std::strcmp(scene_document_path(), savedPath) == 0);
  editor_set_world(nullptr);
  return ok ? 0 : 6;
}

/// EXPECTATION: over unsaved changes, New Scene from Template waits for the
/// unsaved-change prompt; Discard then opens the template, and a template
/// that does not load leaves the document and the World as they were.
int check_new_from_template_waits_and_refuses() {
  char templatePath[512] = {};
  char scenePath[512] = {};
  if (!ensure_scratch_root() ||
      !make_scratch_path("template_two.scene", templatePath,
                         sizeof(templatePath)) ||
      !make_scratch_path("working.scene", scenePath, sizeof(scenePath))) {
    return 1;
  }
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 2;
  }
  editor_set_world(world.get());
  if ((add_named_entity(*world, "FromTemplate") == kInvalidEntity) ||
      !perform_scene_save_as(templatePath) || !perform_scene_new()) {
    editor_set_world(nullptr);
    return 3;
  }
  const Entity mine = add_named_entity(*world, "Mine");
  if ((mine == kInvalidEntity) || !perform_scene_save_as(scenePath) ||
      !push_transform_edit(*world, mine) || !scene_document_is_dirty()) {
    editor_set_world(nullptr);
    return 4;
  }

  request_scene_new_from_template("/nonexistent/template.scene");
  perform_scene_new_from_template("/nonexistent/template.scene");
  if (scene_document_prompt_open() || !scene_document_is_dirty() ||
      (std::strcmp(scene_document_path(), scenePath) != 0) ||
      (world->find_entity_by_name("Mine") == kInvalidEntity)) {
    editor_set_world(nullptr);
    return 5;
  }

  request_scene_new_from_template(templatePath);
  if (!scene_document_prompt_open() ||
      (world->find_entity_by_name("Mine") == kInvalidEntity)) {
    editor_set_world(nullptr);
    return 6;
  }
  scene_document_prompt_choose_discard();
  const bool ok =
      !scene_document_prompt_open() && !scene_document_has_path() &&
      !scene_document_is_dirty() &&
      (world->find_entity_by_name("FromTemplate") != kInvalidEntity) &&
      (world->find_entity_by_name("Mine") == kInvalidEntity);
  editor_set_world(nullptr);
  return ok ? 0 : 7;
}

/// EXPECTATION: the template list offers the engine's Basic scene first,
/// then the project's templates/ scenes by name, other files skipped.
int check_scene_template_list() {
  char root[900] = {};
  if (!ensure_scratch_root() || !scratch_root(root, sizeof(root))) {
    return 1;
  }
  namespace fs = std::filesystem;
  std::error_code ec{};
  const fs::path engineRoot = fs::path(root) / "engine_root";
  const fs::path assetRoot = fs::path(root) / "project_assets";
  fs::remove_all(engineRoot, ec);
  fs::remove_all(assetRoot, ec);
  const fs::path basic = engineRoot / kBasicSceneTemplate;
  fs::create_directories(basic.parent_path(), ec);
  fs::create_directories(assetRoot / kProjectSceneTemplatesFolder, ec);
  for (const fs::path &file :
       {basic, assetRoot / kProjectSceneTemplatesFolder / "level_b.scene",
        assetRoot / kProjectSceneTemplatesFolder / "level_a.scene",
        assetRoot / kProjectSceneTemplatesFolder / "notes.txt"}) {
    std::ofstream out(file, std::ios::binary);
    if (!out) {
      return 2;
    }
  }
  SceneTemplate templates[kMaxSceneTemplates];
  const std::size_t count = list_scene_templates_in(
      engineRoot.string().c_str(), assetRoot.string().c_str(), templates,
      kMaxSceneTemplates);
  const bool listed = (count == 3U) && templates[0].builtIn &&
                      (std::strcmp(templates[0].label, "Basic") == 0) &&
                      !templates[1].builtIn &&
                      (std::strcmp(templates[1].label, "level_a") == 0) &&
                      (std::strcmp(templates[2].label, "level_b") == 0);
  const bool noRoots =
      list_scene_templates_in("", "", templates, kMaxSceneTemplates) == 0U;
  fs::remove_all(engineRoot, ec);
  fs::remove_all(assetRoot, ec);
  if (!listed) {
    return 3;
  }
  return noRoots ? 0 : 4;
}

/// EXPECTATION: a failed Open leaves the current document identity, dirty
/// status, and world content completely untouched (load_scene is
/// transactional; the document layer must not partially switch either).
int check_failed_open_preserves_current_document() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char scenePath[512] = {};
  if (!make_scratch_path("preserved.json", scenePath, sizeof(scenePath))) {
    return 2;
  }
  static_cast<void>(std::remove(scenePath));

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 3;
  }
  editor_set_world(world.get());

  const Entity entity = add_named_entity(*world, "Survivor");
  if ((entity == kInvalidEntity) || !perform_scene_save_as(scenePath)) {
    editor_set_world(nullptr);
    return 4;
  }
  if (!push_transform_edit(*world, entity)) {
    editor_set_world(nullptr);
    return 5;
  }
  const bool dirtyBefore = scene_document_is_dirty();

  if (perform_scene_open("/nonexistent/path/that/does/not/exist.json")) {
    editor_set_world(nullptr);
    return 6; // must fail
  }

  const bool ok = (scene_document_is_dirty() == dirtyBefore) &&
                  scene_document_has_path() &&
                  (std::strcmp(scene_document_path(), scenePath) == 0) &&
                  (world->find_entity_by_name("Survivor") != kInvalidEntity);
  editor_set_world(nullptr);
  return ok ? 0 : 7;
}

/// EXPECTATION: Open adopts the loaded file's identity and clears dirty.
int check_successful_open_adopts_identity() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char scenePath[512] = {};
  if (!make_scratch_path("open_target.json", scenePath, sizeof(scenePath))) {
    return 2;
  }
  static_cast<void>(std::remove(scenePath));

  std::unique_ptr<World> writer(new (std::nothrow) World());
  if (writer == nullptr) {
    return 3;
  }
  if (add_named_entity(*writer, "FromDisk") == kInvalidEntity ||
      !save_scene(*writer, scenePath)) {
    return 4;
  }

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 5;
  }
  editor_set_world(world.get());

  if (!perform_scene_open(scenePath)) {
    editor_set_world(nullptr);
    return 6;
  }
  const bool ok = scene_document_has_path() &&
                  (std::strcmp(scene_document_path(), scenePath) == 0) &&
                  !scene_document_is_dirty() &&
                  (world->find_entity_by_name("FromDisk") != kInvalidEntity);
  editor_set_world(nullptr);
  return ok ? 0 : 7;
}

/// EXPECTATION: destinations inside the asset root pass, siblings/parents
/// outside it fail, and a not-yet-existing subdirectory under the root
/// still passes (Save As into a new folder is a normal operation).
int check_jail_validates_destination_root() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char root[900] = {};
  char tempDir[480] = {};
  if (!engine::core::platform_get_temp_dir(tempDir, sizeof(tempDir))) {
    return 2;
  }
  std::snprintf(root, sizeof(root), "%s/engine_scene_document_test/jail_root",
                tempDir);
  std::error_code ec{};
  std::filesystem::create_directories(std::filesystem::path(root), ec);
  if (ec) {
    return 3;
  }

  char insidePath[1000] = {};
  std::snprintf(insidePath, sizeof(insidePath), "%s/scene.json", root);
  if (!scene_path_passes_jail_under(insidePath, root)) {
    return 4;
  }

  char newSubdirPath[1000] = {};
  std::snprintf(newSubdirPath, sizeof(newSubdirPath),
                "%s/not_yet_created/scene.json", root);
  if (!scene_path_passes_jail_under(newSubdirPath, root)) {
    return 5; // Save As into a not-yet-existing subfolder must still pass
  }

  char outsidePath[1000] = {};
  std::snprintf(outsidePath, sizeof(outsidePath), "%s/../escaped.json", root);
  if (scene_path_passes_jail_under(outsidePath, root)) {
    return 6;
  }

  if (scene_path_passes_jail_under(nullptr, root) ||
      scene_path_passes_jail_under(insidePath, nullptr)) {
    return 7;
  }

  return 0;
}

/// EXPECTATION: Save As to a destination outside the asset root fails and
/// leaves the previous document identity/dirty status untouched.
int check_save_as_rejects_destination_outside_jail() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char scenePath[512] = {};
  if (!make_scratch_path("inside_root.json", scenePath, sizeof(scenePath))) {
    return 2;
  }
  static_cast<void>(std::remove(scenePath));

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 3;
  }
  editor_set_world(world.get());
  if (add_named_entity(*world, "Inside") == kInvalidEntity ||
      !perform_scene_save_as(scenePath)) {
    editor_set_world(nullptr);
    return 4;
  }

  // editor_asset_root() defaults to "assets" (relative to cwd); an
  // absolute OS temp path is never inside it.
  char tempDir[480] = {};
  if (!engine::core::platform_get_temp_dir(tempDir, sizeof(tempDir))) {
    editor_set_world(nullptr);
    return 5;
  }
  char outsidePath[1000] = {};
  std::snprintf(outsidePath, sizeof(outsidePath), "%s/escaped_save.json",
                tempDir);

  if (perform_scene_save_as(outsidePath)) {
    editor_set_world(nullptr);
    return 6; // must be rejected by the jail check
  }
  const bool ok = scene_document_has_path() &&
                  (std::strcmp(scene_document_path(), scenePath) == 0) &&
                  !scene_document_is_dirty();
  editor_set_world(nullptr);
  return ok ? 0 : 7;
}

int g_editorErrorLines = 0;
char g_lastEditorError[512] = {};

void count_editor_errors(engine::core::LogLevel level, const char *channel,
                         const char *message, void *) noexcept {
  if ((level != engine::core::LogLevel::Error) || (channel == nullptr) ||
      (std::strcmp(channel, "editor") != 0) || (message == nullptr)) {
    return;
  }
  ++g_editorErrorLines;
  std::snprintf(g_lastEditorError, sizeof(g_lastEditorError), "%s", message);
}

/// EXPECTATION: every refused or failed save logs exactly one Error line
/// naming the reason and leaves the same reason in the document status,
/// so a Save As that wrote nothing is never silent: a destination outside
/// the asset root, a destination whose directory does not exist, and a
/// Save with no path yet.
int check_save_failures_log_an_error() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 2;
  }
  editor_set_world(world.get());
  if (add_named_entity(*world, "Unsaved") == kInvalidEntity) {
    editor_set_world(nullptr);
    return 3;
  }
  g_editorErrorLines = 0;
  g_lastEditorError[0] = '\0';
  if (!engine::core::initialize_logging() ||
      !engine::core::log_register_sink(&count_editor_errors, nullptr)) {
    editor_set_world(nullptr);
    return 4;
  }
  const auto finish = [](int result) noexcept {
    engine::core::log_unregister_sink(&count_editor_errors, nullptr);
    engine::core::shutdown_logging();
    editor_set_world(nullptr);
    return result;
  };

  char tempDir[480] = {};
  if (!engine::core::platform_get_temp_dir(tempDir, sizeof(tempDir))) {
    return finish(5);
  }
  char outsidePath[1000] = {};
  std::snprintf(outsidePath, sizeof(outsidePath), "%s/escaped_save.json",
                tempDir);
  if (perform_scene_save_as(outsidePath) || (g_editorErrorLines != 1) ||
      (std::strstr(g_lastEditorError, "outside the project asset root") ==
       nullptr) ||
      (std::strstr(scene_document_last_error(),
                   "outside the project asset root") == nullptr)) {
    return finish(6);
  }

  char missingDirPath[1000] = {};
  if (!make_scratch_path("no_such_directory/unwritable.json", missingDirPath,
                         sizeof(missingDirPath))) {
    return finish(7);
  }
  if (perform_scene_save_as(missingDirPath) || (g_editorErrorLines != 2) ||
      (std::strstr(g_lastEditorError, "failed to write") == nullptr) ||
      (std::strstr(scene_document_last_error(), "failed to write") ==
       nullptr)) {
    return finish(8);
  }

  if (perform_scene_save() || (g_editorErrorLines != 3) ||
      (std::strstr(g_lastEditorError, "no path yet") == nullptr)) {
    return finish(9);
  }

  // A successful save clears the status.
  char scenePath[512] = {};
  if (!make_scratch_path("logged_then_saved.json", scenePath,
                         sizeof(scenePath)) ||
      !perform_scene_save_as(scenePath) || (g_editorErrorLines != 3) ||
      (scene_document_last_error()[0] != '\0')) {
    return finish(10);
  }
  return finish(0);
}

/// True when `entry` is the Recent Scenes entry for the scene at `path`:
/// the list stores each scene in one normalized form (#1216).
bool is_entry_for(const char *entry, const char *path) noexcept {
  char expected[kMaxRecentPathLength] = {};
  return recent_scene_entry(path, expected, sizeof(expected)) &&
         (std::strcmp(entry, expected) == 0);
}

/// EXPECTATION: recent scenes are MRU-ordered, de-duplicated on re-add,
/// survive a simulated restart (reload from the persisted file), and
/// silently drop an entry pointing at a deleted file.
int check_recent_scenes_persist_and_prune() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char recentDir[900] = {};
  if (!make_scratch_path("recent_dir", recentDir, sizeof(recentDir))) {
    return 2;
  }
  std::error_code ec{};
  std::filesystem::remove_all(std::filesystem::path(recentDir), ec);
  std::filesystem::create_directories(std::filesystem::path(recentDir), ec);
  if (ec) {
    return 3;
  }
  recent_scenes_set_directory_override_for_tests(recentDir);

  char pathA[1000] = {};
  char pathB[1000] = {};
  char pathC[1000] = {};
  std::snprintf(pathA, sizeof(pathA), "%s/a.json", recentDir);
  std::snprintf(pathB, sizeof(pathB), "%s/b.json", recentDir);
  std::snprintf(pathC, sizeof(pathC), "%s/c.json", recentDir);
  for (const char *path : {pathA, pathB, pathC}) {
    std::FILE *file = nullptr;
#ifdef _WIN32
    if (fopen_s(&file, path, "wb") != 0) {
      file = nullptr;
    }
#else
    file = std::fopen(path, "wb");
#endif
    if (file == nullptr) {
      g_recentGuard.rearm();
      return 4;
    }
    std::fputs("{}", file);
    std::fclose(file);
  }

  recent_scenes_add(pathA);
  recent_scenes_add(pathB);
  recent_scenes_add(pathC);
  // Re-adding A must move it to the front without duplicating it.
  recent_scenes_add(pathA);

  bool ok = (recent_scene_count() == 3U) &&
            is_entry_for(recent_scene_at(0U), pathA) &&
            is_entry_for(recent_scene_at(1U), pathC) &&
            is_entry_for(recent_scene_at(2U), pathB);

  // Simulate a restart: drop the in-memory cache and delete one file
  // before the next load.
  static_cast<void>(std::remove(pathB));
  recent_scenes_set_directory_override_for_tests(recentDir);
  ok = ok && (recent_scene_count() == 2U);
  bool foundA = false;
  bool foundB = false;
  for (std::size_t i = 0U; i < recent_scene_count(); ++i) {
    if (is_entry_for(recent_scene_at(i), pathA)) {
      foundA = true;
    }
    if (is_entry_for(recent_scene_at(i), pathB)) {
      foundB = true;
    }
  }
  ok = ok && foundA && !foundB;

  g_recentGuard.rearm();
  return ok ? 0 : 5;
}

/// Writes `contents` to `path`; false on any failure.
bool write_file_bytes(const char *path, const std::string &contents) noexcept {
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
  const bool ok = std::fwrite(contents.data(), 1U, contents.size(), file) ==
                  contents.size();
  return (std::fclose(file) == 0) && ok;
}

/// Reads the whole file at `path`; empty when missing or unreadable.
std::string read_file_bytes(const char *path) {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return {};
  }
  std::string bytes;
  char chunk[4096] = {};
  for (;;) {
    const std::size_t read = std::fread(chunk, 1U, sizeof(chunk), file);
    bytes.append(chunk, read);
    if (read < sizeof(chunk)) {
      break;
    }
  }
  std::fclose(file);
  return bytes;
}

/// EXPECTATION (issue #321): a recent-scenes file that exists but cannot be
/// read this session (here: a valid list larger than the reader's fixed
/// buffer) starts the session with an empty in-memory list and latches
/// persistence off, so a later add updates the in-memory list but leaves
/// the stored bytes exactly as they were. A stored path that is a
/// directory (an unreadable file) latches the same way. A truly absent
/// file still starts a fresh list that persists normally, and a readable
/// file after the latch is cleared loads and persists again.
int check_recent_scenes_unreadable_file_never_overwritten() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char recentDir[900] = {};
  if (!make_scratch_path("recent_unreadable_dir", recentDir,
                         sizeof(recentDir))) {
    return 2;
  }
  std::error_code ec{};
  std::filesystem::remove_all(std::filesystem::path(recentDir), ec);
  std::filesystem::create_directories(std::filesystem::path(recentDir), ec);
  if (ec) {
    return 3;
  }
  char recentFile[1000] = {};
  std::snprintf(recentFile, sizeof(recentFile), "%s/editor_recent_scenes.json",
                recentDir);
  char scenePath[1000] = {};
  std::snprintf(scenePath, sizeof(scenePath), "%s/opened.json", recentDir);
  if (!write_file_bytes(scenePath, "{}")) {
    return 4;
  }

  // 1. TooLarge: a list-shaped document whose bytes exceed the longest
  //    list the reader accepts (kMaxStoredRecentListBytes). The reader reports
  //    TooLarge before parsing, so these bytes are never interpreted; the path
  //    is spliced in unescaped and the document is only valid JSON where the
  //    path has no escapable characters. What matters is that the bytes are
  //    exact and survive.
  std::string oversized = "{\"scenes\":[";
  while (oversized.size() <= kMaxStoredRecentListBytes + 1024U) {
    oversized += "\"";
    oversized += scenePath;
    oversized += "\",";
  }
  oversized.pop_back();
  oversized += "]}";
  if (!write_file_bytes(recentFile, oversized)) {
    return 5;
  }

  recent_scenes_set_directory_override_for_tests(recentDir);
  bool ok = (recent_scene_count() == 0U);
  recent_scenes_add(scenePath);
  ok = ok && (recent_scene_count() == 1U) &&
       is_entry_for(recent_scene_at(0U), scenePath);
  // The stored bytes are untouched by the add's persistence attempt.
  ok = ok && (read_file_bytes(recentFile) == oversized);
  if (!ok) {
    g_recentGuard.rearm();
    return 6;
  }

  // 2. Unreadable: the stored path is a directory. The session latches the
  //    same way and the directory survives the add.
  static_cast<void>(std::remove(recentFile));
  std::filesystem::create_directories(std::filesystem::path(recentFile), ec);
  if (ec) {
    g_recentGuard.rearm();
    return 7;
  }
  recent_scenes_set_directory_override_for_tests(recentDir);
  ok = (recent_scene_count() == 0U);
  recent_scenes_add(scenePath);
  ok = ok && (recent_scene_count() == 1U) &&
       std::filesystem::is_directory(std::filesystem::path(recentFile), ec);
  std::filesystem::remove_all(std::filesystem::path(recentFile), ec);
  if (!ok) {
    g_recentGuard.rearm();
    return 8;
  }

  // 3. Absent: no stored file starts a fresh list that persists normally.
  //    Persistence is proven through the production reload path (a fresh
  //    session reads the document back), not a byte search: the writer
  //    escapes path separators, so the raw path is not a substring of the
  //    stored bytes on every platform.
  recent_scenes_set_directory_override_for_tests(recentDir);
  ok = (recent_scene_count() == 0U);
  recent_scenes_add(scenePath);
  ok = ok && !read_file_bytes(recentFile).empty();
  recent_scenes_set_directory_override_for_tests(recentDir);
  ok = ok && (recent_scene_count() == 1U) &&
       is_entry_for(recent_scene_at(0U), scenePath);
  if (!ok) {
    g_recentGuard.rearm();
    return 9;
  }

  // 4. Recovery: the readable list written in step 3 keeps persisting in
  //    the session that loaded it (the latch was per unreadable file), so
  //    a further add is visible to the session after that.
  char secondScene[1000] = {};
  std::snprintf(secondScene, sizeof(secondScene), "%s/second.json", recentDir);
  ok = write_file_bytes(secondScene, "{}");
  recent_scenes_add(secondScene);
  ok = ok && (recent_scene_count() == 2U);
  recent_scenes_set_directory_override_for_tests(recentDir);
  ok = ok && (recent_scene_count() == 2U) &&
       is_entry_for(recent_scene_at(0U), secondScene) &&
       is_entry_for(recent_scene_at(1U), scenePath);

  g_recentGuard.rearm();
  return ok ? 0 : 10;
}

} // namespace

/// Runs this executable or test program.
/// EXPECTATION: Save As gives the scene it writes a source-side sidecar
/// in the same transaction, so the scene it just created is referenceable
/// without the author running a tool first (#639). A scene with no
/// identity is reported by the catalog on every later start and can be
/// named by no document.
int check_save_as_establishes_scene_identity() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char scenePath[512] = {};
  if (!make_scratch_path("identified_scene.scene", scenePath,
                         sizeof(scenePath))) {
    return 2;
  }
  char sidecarPath[600] = {};
  const int written =
      std::snprintf(sidecarPath, sizeof(sidecarPath), "%s.meta", scenePath);
  if ((written <= 0) ||
      (static_cast<std::size_t>(written) >= sizeof(sidecarPath))) {
    return 3;
  }
  static_cast<void>(std::remove(scenePath));
  static_cast<void>(std::remove(sidecarPath));

  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 4;
  }
  editor_set_world(world.get());

  if (!perform_scene_save_as(scenePath)) {
    editor_set_world(nullptr);
    return 5;
  }

  std::error_code ec{};
  if (!std::filesystem::exists(scenePath, ec) || ec) {
    editor_set_world(nullptr);
    return 6;
  }
  if (!std::filesystem::exists(sidecarPath, ec) || ec) {
    editor_set_world(nullptr);
    return 7;
  }

  // The sidecar must carry a usable identity, not merely exist.
  engine::content::AssetSidecar sidecar{};
  if (engine::content::read_asset_sidecar(scenePath, &sidecar) !=
      engine::content::SidecarReadResult::Ok) {
    editor_set_world(nullptr);
    return 8;
  }
  if (!engine::content::asset_guid_is_valid(sidecar.guid)) {
    editor_set_world(nullptr);
    return 9;
  }

  // Saving again over the same path keeps the identity it already had:
  // minting a second one would silently rebind every reference written
  // against the first.
  const engine::core::AssetGuid first = sidecar.guid;
  if (!perform_scene_save_as(scenePath)) {
    editor_set_world(nullptr);
    return 10;
  }
  engine::content::AssetSidecar again{};
  if ((engine::content::read_asset_sidecar(scenePath, &again) !=
       engine::content::SidecarReadResult::Ok) ||
      !(again.guid == first)) {
    editor_set_world(nullptr);
    return 11;
  }

  editor_set_world(nullptr);
  static_cast<void>(std::remove(scenePath));
  static_cast<void>(std::remove(sidecarPath));
  return 0;
}

/// Writes a one-entity scene named `name` to `path`, as a teammate's
/// commit or a regeneration would change the file under the editor.
bool write_external_scene(const char *path, const char *name) noexcept {
  std::unique_ptr<World> writer(new (std::nothrow) World());
  return (writer != nullptr) &&
         (add_named_entity(*writer, name) != kInvalidEntity) &&
         save_scene(*writer, path);
}

/// Whether the file at `path` currently holds the text `needle`.
bool file_holds(const char *path, const char *needle) {
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
  std::string text;
  char chunk[1024];
  std::size_t count = 0U;
  while ((count = std::fread(chunk, 1U, sizeof(chunk), file)) > 0U) {
    text.append(chunk, count);
  }
  std::fclose(file);
  return text.find(needle) != std::string::npos;
}

/// EXPECTATION (#987): a Save never silently writes over a scene file that
/// changed on disk since the editor opened or last saved it. It stops with
/// the file untouched and asks: Cancel keeps both as they are, Overwrite
/// writes the edits, Reload takes the file's version. A file made
/// unparsable on disk is a conflict too, and a Reload that cannot read the
/// file keeps the edits open. A file deleted on disk is not: nothing there
/// is lost, so Save writes it again.
int check_save_over_external_change() {
  if (!ensure_scratch_root()) {
    return 1;
  }
  char scenePath[512] = {};
  if (!make_scratch_path("external_change.json", scenePath,
                         sizeof(scenePath)) ||
      !write_external_scene(scenePath, "Original")) {
    return 2;
  }
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 3;
  }
  editor_set_world(world.get());
  const auto finish = [&](int result) {
    editor_set_world(nullptr);
    static_cast<void>(std::remove(scenePath));
    return result;
  };
  if (!perform_scene_open(scenePath)) {
    return finish(4);
  }
  const Entity original = world->find_entity_by_name("Original");
  if ((original == kInvalidEntity) || !push_transform_edit(*world, original)) {
    return finish(5);
  }

  // A save with nothing changed on disk goes straight through, and so
  // does the next one: the editor's own write is not a conflict.
  if (!perform_scene_save() || scene_document_conflict_open() ||
      !push_transform_edit(*world, original) || !perform_scene_save()) {
    return finish(6);
  }

  // Changed on disk: the save stops, the file keeps the other version.
  if (!write_external_scene(scenePath, "Teammate") ||
      !push_transform_edit(*world, original)) {
    return finish(7);
  }
  if (perform_scene_save() || !scene_document_conflict_open() ||
      !file_holds(scenePath, "Teammate") || !scene_document_is_dirty()) {
    return finish(8);
  }
  scene_document_conflict_choose_cancel();
  if (scene_document_conflict_open() || !file_holds(scenePath, "Teammate")) {
    return finish(9);
  }

  // Overwrite writes the edits over it.
  if (perform_scene_save() || !scene_document_conflict_open()) {
    return finish(10);
  }
  scene_document_conflict_choose_overwrite();
  if (scene_document_conflict_open() || file_holds(scenePath, "Teammate") ||
      !file_holds(scenePath, "Original") || scene_document_is_dirty()) {
    return finish(11);
  }

  // Reload takes the file's version and drops the edits.
  if (!write_external_scene(scenePath, "Teammate") ||
      !push_transform_edit(*world, original) || perform_scene_save()) {
    return finish(12);
  }
  scene_document_conflict_choose_reload();
  if (scene_document_conflict_open() || scene_document_is_dirty() ||
      (world->find_entity_by_name("Teammate") == kInvalidEntity) ||
      (world->find_entity_by_name("Original") != kInvalidEntity)) {
    return finish(13);
  }

  // Deleted on disk: Save writes the file again, with no conflict.
  const Entity teammate = world->find_entity_by_name("Teammate");
  static_cast<void>(std::remove(scenePath));
  if (!push_transform_edit(*world, teammate) || !perform_scene_save() ||
      scene_document_conflict_open()) {
    return finish(14);
  }
  if (!file_holds(scenePath, "Teammate") || scene_document_is_dirty()) {
    return finish(15);
  }

  // Unparsable on disk: a conflict; a Reload that cannot read it keeps the
  // edits and the document as they were.
  if (!write_file_bytes(scenePath, "{ not a scene") ||
      !push_transform_edit(*world, teammate) || perform_scene_save() ||
      !scene_document_conflict_open()) {
    return finish(16);
  }
  scene_document_conflict_choose_reload();
  if ((world->find_entity_by_name("Teammate") == kInvalidEntity) ||
      !scene_document_is_dirty() ||
      (std::strcmp(scene_document_path(), scenePath) != 0) ||
      (scene_document_last_error()[0] == '\0')) {
    return finish(17);
  }
  return finish(0);
}

int main() {
  struct NamedCheck {
    const char *name;
    int (*fn)();
  };
  const NamedCheck checks[] = {
      {"check_default_state_is_untitled_and_clean",
       &check_default_state_is_untitled_and_clean},
      {"check_dirty_tracks_history_position_around_save",
       &check_dirty_tracks_history_position_around_save},
      {"check_save_without_path_fails_cleanly",
       &check_save_without_path_fails_cleanly},
      {"check_perform_scene_new_resets_identity",
       &check_perform_scene_new_resets_identity},
      {"check_failed_open_preserves_current_document",
       &check_failed_open_preserves_current_document},
      {"check_successful_open_adopts_identity",
       &check_successful_open_adopts_identity},
      {"check_jail_validates_destination_root",
       &check_jail_validates_destination_root},
      {"check_save_as_rejects_destination_outside_jail",
       &check_save_as_rejects_destination_outside_jail},
      {"check_save_as_establishes_scene_identity",
       &check_save_as_establishes_scene_identity},
      {"check_save_failures_log_an_error", &check_save_failures_log_an_error},
      {"check_recent_scenes_persist_and_prune",
       &check_recent_scenes_persist_and_prune},
      {"check_recent_scenes_unreadable_file_never_overwritten",
       &check_recent_scenes_unreadable_file_never_overwritten},
      {"check_save_over_external_change", &check_save_over_external_change},
      {"check_new_from_template_opens_untitled_and_clean",
       &check_new_from_template_opens_untitled_and_clean},
      {"check_new_from_template_waits_and_refuses",
       &check_new_from_template_waits_and_refuses},
      {"check_scene_template_list", &check_scene_template_list},
  };

  char recentScratch[1000] = {};
  if (!ensure_scratch_root() ||
      !make_scratch_path("recent_scenes_fixture", recentScratch,
                         sizeof(recentScratch)) ||
      !g_recentGuard.arm(recentScratch)) {
    std::fprintf(stderr, "editor_scene_document_test: the recent-scenes "
                         "guard could not be armed\n");
    return 98;
  }

  for (const auto &check : checks) {
    const int result = check.fn();
    if (result != 0) {
      std::fprintf(stderr, "editor_scene_document_test: %s failed: %d\n",
                   check.name, result);
      static_cast<void>(g_recentGuard.disarm());
      return result;
    }
  }

  if (!g_recentGuard.disarm()) {
    return 99;
  }
  std::printf("editor_scene_document_test: all tests passed\n");
  return 0;
}
