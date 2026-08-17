// Verifies the material editor's business logic (issue #160): opening
// loads the resolved state, a field-edit gesture applies live immediately
// and pushes exactly one undo step once the gesture ends, undo/redo round
// trips through the live database record, Save persists to disk, Reload
// discards unsaved edits and reflects the file, and closing the panel does
// not discard already-applied live edits.

#include "editor_commands.h"
#include "editor_material_edit.h"
#include "editor_session.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/service_registry.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

using namespace engine::editor;

constexpr const char *kMountPrefix = "edmatpanel";
constexpr const char *kOsPath = "editor_material_edit_test.json";
constexpr const char *kVirtualPath = "edmatpanel/editor_material_edit_test.json";

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

void remove_file(const char *path) noexcept {
  static_cast<void>(std::remove(path));
}

/// Binds a fresh asset database as the published editor asset service, and
/// resets the material editor + command history on destruction so tests
/// never leak process-wide state into each other.
struct MaterialEditScope final {
  std::unique_ptr<engine::renderer::AssetDatabase> database;
  engine::runtime::EngineAssetDatabaseService service{};

  MaterialEditScope() noexcept
      : database(new (std::nothrow) engine::renderer::AssetDatabase()) {
    service.database = database.get();
    engine::runtime::set_editor_asset_service(&service);
  }

  ~MaterialEditScope() noexcept {
    close_material_editor();
    editor_session().commandHistory.clear();
    engine::runtime::set_editor_asset_service(nullptr);
  }

  bool valid() const noexcept { return database != nullptr; }
};

/// EXPECTATION: opening a material loads its resolved state into the
/// panel buffer.
int check_open_loads_state() noexcept {
  if (!write_file(kOsPath, "{\"version\":2,\"roughness\":0.3}")) {
    return 1;
  }
  MaterialEditScope scope;
  const auto finish = [&](int result) noexcept {
    remove_file(kOsPath);
    return result;
  };
  if (!scope.valid()) {
    return finish(2);
  }

  open_material_editor(kVirtualPath);
  const MaterialEditorState &state = material_editor_state();
  if (!state.open || !state.found ||
      !exactly_equal(state.buffer.roughness, 0.3F)) {
    return finish(3);
  }

  return finish(0);
}

/// EXPECTATION: a live edit applies to the database record immediately
/// (visible through editor_load_material without a save), and ending the
/// gesture pushes exactly one undoable command whose undo restores the
/// prior value.
int check_live_edit_and_undo() noexcept {
  if (!write_file(kOsPath, "{\"version\":2,\"roughness\":0.3,"
                          "\"metallic\":0.1}")) {
    return 10;
  }
  MaterialEditScope scope;
  const auto finish = [&](int result) noexcept {
    remove_file(kOsPath);
    return result;
  };
  if (!scope.valid()) {
    return finish(11);
  }

  open_material_editor(kVirtualPath);
  MaterialEditorState &state = material_editor_state();
  if (!state.found) {
    return finish(12);
  }

  // Simulate one drag gesture spanning three frames: item active while
  // dragging, released on the third.
  const engine::renderer::Material beforeGesture = state.buffer;
  const engine::renderer::MaterialTextureSlots beforeSlots = state.textureSlots;

  state.buffer.roughness = 0.5F;
  material_editor_apply_frame(beforeGesture, beforeSlots, true, true);
  // Live value must already be visible without ending the gesture.
  if (!exactly_equal(
          engine::runtime::editor_load_material(kVirtualPath).params.roughness,
          0.5F)) {
    return finish(13);
  }

  state.buffer.roughness = 0.9F;
  material_editor_apply_frame(beforeGesture, beforeSlots, true, true);

  // Gesture ends: item no longer active.
  material_editor_apply_frame(beforeGesture, beforeSlots, false, false);
  if (!exactly_equal(state.buffer.roughness, 0.9F)) {
    return finish(14);
  }
  if (!editor_session().commandHistory.can_undo()) {
    return finish(15); // exactly one command should now sit on the stack
  }

  if (!editor_session().commandHistory.undo()) {
    return finish(16);
  }
  if (!exactly_equal(
          engine::runtime::editor_load_material(kVirtualPath).params.roughness,
          0.3F)) {
    return finish(17); // undo restored the pre-gesture value
  }

  if (!editor_session().commandHistory.redo()) {
    return finish(18);
  }
  if (!exactly_equal(
          engine::runtime::editor_load_material(kVirtualPath).params.roughness,
          0.9F)) {
    return finish(19); // redo re-applied the gesture's final value
  }

  return finish(0);
}

/// EXPECTATION: Save persists the live buffer to disk; Reload discards an
/// unsaved edit and reflects whatever is on disk (or leaves the buffer
/// untouched on a malformed file).
int check_save_and_reload() noexcept {
  if (!write_file(kOsPath, "{\"version\":2,\"roughness\":0.2}")) {
    return 20;
  }
  MaterialEditScope scope;
  const auto finish = [&](int result) noexcept {
    remove_file(kOsPath);
    return result;
  };
  if (!scope.valid()) {
    return finish(21);
  }

  open_material_editor(kVirtualPath);
  MaterialEditorState &state = material_editor_state();
  if (!state.found) {
    return finish(22);
  }

  const engine::renderer::Material before = state.buffer;
  const engine::renderer::MaterialTextureSlots beforeSlots = state.textureSlots;
  state.buffer.roughness = 0.77F;
  material_editor_apply_frame(before, beforeSlots, true, false); // ends immediately

  if (!save_material_editor() || state.dirty) {
    return finish(23);
  }

  // A further unsaved edit...
  const engine::renderer::Material beforeSecond = state.buffer;
  const engine::renderer::MaterialTextureSlots beforeSecondSlots =
      state.textureSlots;
  state.buffer.roughness = 0.11F;
  material_editor_apply_frame(beforeSecond, beforeSecondSlots, true, false);
  if (!exactly_equal(state.buffer.roughness, 0.11F) || !state.dirty) {
    return finish(24);
  }

  // ...Reload discards it and reflects the saved (0.77) value.
  if (!reload_material_editor_from_disk()) {
    return finish(25);
  }
  if (!exactly_equal(state.buffer.roughness, 0.77F) || state.dirty) {
    return finish(26);
  }

  return finish(0);
}

/// EXPECTATION: closing the panel does not revert an already-applied live
/// edit (only the panel's own visibility changes).
int check_close_keeps_live_edit() noexcept {
  if (!write_file(kOsPath, "{\"version\":2,\"roughness\":0.4}")) {
    return 30;
  }
  MaterialEditScope scope;
  const auto finish = [&](int result) noexcept {
    remove_file(kOsPath);
    return result;
  };
  if (!scope.valid()) {
    return finish(31);
  }

  open_material_editor(kVirtualPath);
  MaterialEditorState &state = material_editor_state();
  const engine::renderer::Material before = state.buffer;
  const engine::renderer::MaterialTextureSlots beforeSlots = state.textureSlots;
  state.buffer.roughness = 0.66F;
  material_editor_apply_frame(before, beforeSlots, true, false);

  close_material_editor();
  if (material_editor_state().open) {
    return finish(32);
  }
  if (!exactly_equal(
          engine::runtime::editor_load_material(kVirtualPath).params.roughness,
          0.66F)) {
    return finish(33);
  }

  return finish(0);
}

} // namespace

int main() {
  if (!engine::core::initialize_vfs()) {
    return 1;
  }
  if (!engine::core::mount(kMountPrefix, ".")) {
    engine::core::shutdown_vfs();
    return 2;
  }

  int result = check_open_loads_state();
  if (result == 0) {
    result = check_live_edit_and_undo();
  }
  if (result == 0) {
    result = check_save_and_reload();
  }
  if (result == 0) {
    result = check_close_keeps_live_edit();
  }

  engine::core::shutdown_vfs();
  if (result != 0) {
    std::fprintf(stderr, "editor_material_edit_test failed: %d\n", result);
    return result;
  }
  std::printf("editor_material_edit_test: all tests passed\n");
  return 0;
}
