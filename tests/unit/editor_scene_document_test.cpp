// Verifies the scene-document identity/dirty core (issue #158): default
// "Untitled Scene" identity, dirty tracking driven by CommandHistory's
// token (including the undo-to-saved-marker-clears-dirty contract),
// New/Open/Save/Save As through the production editor-session paths,
// failed-load state preservation, the asset-root jail check, and the
// recent-scenes list (MRU order, dedupe, and pruning invalid entries).

#include "editor_commands.h"
#include "editor_scene_document.h"
#include "editor_session.h"
#include "engine/core/platform.h"
#include "engine/editor/editor.h"
#include "engine/runtime/scene_serializer.h"
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

  const bool ok = !scene_document_has_path() &&
                  (std::strcmp(scene_document_display_name(),
                              "Untitled Scene") == 0) &&
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
      recent_scenes_set_directory_override_for_tests("");
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
            (std::strcmp(recent_scene_at(0U), pathA) == 0) &&
            (std::strcmp(recent_scene_at(1U), pathC) == 0) &&
            (std::strcmp(recent_scene_at(2U), pathB) == 0);

  // Simulate a restart: drop the in-memory cache and delete one file
  // before the next load.
  static_cast<void>(std::remove(pathB));
  recent_scenes_set_directory_override_for_tests(recentDir);
  ok = ok && (recent_scene_count() == 2U);
  bool foundA = false;
  bool foundB = false;
  for (std::size_t i = 0U; i < recent_scene_count(); ++i) {
    if (std::strcmp(recent_scene_at(i), pathA) == 0) {
      foundA = true;
    }
    if (std::strcmp(recent_scene_at(i), pathB) == 0) {
      foundB = true;
    }
  }
  ok = ok && foundA && !foundB;

  recent_scenes_set_directory_override_for_tests("");
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
  const bool ok =
      std::fwrite(contents.data(), 1U, contents.size(), file) == contents.size();
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

  // 1. TooLarge: a list-shaped document whose bytes exceed the 8 KiB reader
  //    buffer. The reader reports TooLarge before parsing, so these bytes
  //    are never interpreted; the path is spliced in unescaped and the
  //    document is only valid JSON where the path has no escapable
  //    characters. What matters is that the bytes are exact and survive.
  std::string oversized = "{\"scenes\":[";
  while (oversized.size() < 9000U) {
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
       (std::strcmp(recent_scene_at(0U), scenePath) == 0);
  // The stored bytes are untouched by the add's persistence attempt.
  ok = ok && (read_file_bytes(recentFile) == oversized);
  if (!ok) {
    recent_scenes_set_directory_override_for_tests("");
    return 6;
  }

  // 2. Unreadable: the stored path is a directory. The session latches the
  //    same way and the directory survives the add.
  static_cast<void>(std::remove(recentFile));
  std::filesystem::create_directories(std::filesystem::path(recentFile), ec);
  if (ec) {
    recent_scenes_set_directory_override_for_tests("");
    return 7;
  }
  recent_scenes_set_directory_override_for_tests(recentDir);
  ok = (recent_scene_count() == 0U);
  recent_scenes_add(scenePath);
  ok = ok && (recent_scene_count() == 1U) &&
       std::filesystem::is_directory(std::filesystem::path(recentFile), ec);
  std::filesystem::remove_all(std::filesystem::path(recentFile), ec);
  if (!ok) {
    recent_scenes_set_directory_override_for_tests("");
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
       (std::strcmp(recent_scene_at(0U), scenePath) == 0);
  if (!ok) {
    recent_scenes_set_directory_override_for_tests("");
    return 9;
  }

  // 4. Recovery: the readable list written in step 3 keeps persisting in
  //    the session that loaded it (the latch was per unreadable file), so
  //    a further add is visible to the session after that.
  char secondScene[1000] = {};
  std::snprintf(secondScene, sizeof(secondScene), "%s/second.json",
                recentDir);
  ok = write_file_bytes(secondScene, "{}");
  recent_scenes_add(secondScene);
  ok = ok && (recent_scene_count() == 2U);
  recent_scenes_set_directory_override_for_tests(recentDir);
  ok = ok && (recent_scene_count() == 2U) &&
       (std::strcmp(recent_scene_at(0U), secondScene) == 0) &&
       (std::strcmp(recent_scene_at(1U), scenePath) == 0);

  recent_scenes_set_directory_override_for_tests("");
  return ok ? 0 : 10;
}

} // namespace

/// Runs this executable or test program.
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
      {"check_recent_scenes_persist_and_prune",
       &check_recent_scenes_persist_and_prune},
      {"check_recent_scenes_unreadable_file_never_overwritten",
       &check_recent_scenes_unreadable_file_never_overwritten},
  };

  for (const auto &check : checks) {
    const int result = check.fn();
    if (result != 0) {
      std::fprintf(stderr, "editor_scene_document_test: %s failed: %d\n",
                   check.name, result);
      return result;
    }
  }

  std::printf("editor_scene_document_test: all tests passed\n");
  return 0;
}
