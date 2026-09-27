// Implements the editor's entity clipboard on the duplicate command: Copy
// keeps a forest capture's records and its roots' world poses, and each
// Paste builds a fresh command from them, so a paste is undone and redone
// like any duplicate.

#include "editor_entity_clipboard.h"

#include "editor_commands.h"
#include "editor_session.h"
#include "editor_transform_util.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/math/mat4.h"

namespace engine::editor {

namespace {

/// What Copy captured: the forest's records, each root's world pose at
/// copy time, and the document they came from.
struct EntityClipboard final {
  std::unique_ptr<EntityDuplicateRecord[]> records;
  std::size_t recordCount = 0U;
  std::unique_ptr<std::size_t[]> rootRecords;
  std::unique_ptr<math::Mat4[]> rootWorld;
  std::unique_ptr<bool[]> rootHasWorld;
  std::size_t rootCount = 0U;
  std::uint64_t documentGeneration = 0U;
};

EntityClipboard g_clipboard{};

/// True when `id` is the source of one of the copied records.
bool copied(const EntityClipboard &clip, runtime::PersistentId id) noexcept {
  for (std::size_t i = 0U; i < clip.recordCount; ++i) {
    if (clip.records[i].sourcePersistentId == id) {
      return true;
    }
  }
  return false;
}

/// Places pasted root `record` (see execute_entity_paste): under `parent`
/// keeping its world pose, beside its original when that parent still
/// exists here, or at the root with its world pose.
void place_root(const runtime::World &world, runtime::Entity parent,
                bool sameDocument, bool hasWorld, const math::Mat4 &rootWorld,
                EntityDuplicateRecord *record) noexcept {
  runtime::Transform &local = record->components.transform;
  if (parent != runtime::kInvalidEntity) {
    const runtime::WorldTransform *parentWorld =
        world.get_world_transform_read_ptr(parent);
    runtime::Transform placed = local;
    if (hasWorld && (parentWorld != nullptr) &&
        world_matrix_to_local_transform(rootWorld, &parentWorld->matrix, local,
                                        &placed)) {
      local = placed;
    }
    local.parentId = world.persistent_id(parent);
    return;
  }
  const bool originalParentHere =
      sameDocument && (local.parentId != runtime::kInvalidPersistentId) &&
      (world.find_entity_by_persistent_id(local.parentId) !=
       runtime::kInvalidEntity);
  if (originalParentHere) {
    return;
  }
  runtime::Transform placed = local;
  if (hasWorld &&
      world_matrix_to_local_transform(rootWorld, nullptr, local, &placed)) {
    local = placed;
  }
  local.parentId = runtime::kInvalidPersistentId;
}

} // namespace

bool entity_clipboard_copy() noexcept {
  EditorSession &session = editor_session();
  runtime::World *const world = session.world;
  if (world == nullptr) {
    return false;
  }
  prune_entity_selection();
  const runtime::Entity primary = selected_entity();
  const runtime::Entity *entities = session.selectedEntities.data();
  std::size_t count = session.selectedEntityCount;
  if (count == 0U) {
    if (primary == runtime::kInvalidEntity) {
      return false;
    }
    entities = &primary;
    count = 1U;
  }
  std::unique_ptr<EntityDuplicateCommand> capture(
      build_entity_duplicate_command(entities, count));
  if (capture == nullptr) {
    core::log_message(core::LogLevel::Error, "editor",
                      "copy refused: the selection could not be captured "
                      "(out of memory or nothing alive)");
    return false;
  }
  EntityClipboard next{};
  next.rootWorld.reset(new (std::nothrow) math::Mat4[capture->rootCount]);
  next.rootHasWorld.reset(new (std::nothrow) bool[capture->rootCount]());
  if ((next.rootWorld == nullptr) || (next.rootHasWorld == nullptr)) {
    return false;
  }
  for (std::size_t k = 0U; k < capture->rootCount; ++k) {
    const runtime::Entity root = world->find_entity_by_persistent_id(
        capture->records[capture->rootRecords[k]].sourcePersistentId);
    const runtime::WorldTransform *rootWorld =
        world->get_world_transform_read_ptr(root);
    if (rootWorld != nullptr) {
      next.rootWorld[k] = rootWorld->matrix;
      next.rootHasWorld[k] = true;
    }
  }
  next.records = std::move(capture->records);
  next.recordCount = capture->recordCount;
  next.rootRecords = std::move(capture->rootRecords);
  next.rootCount = capture->rootCount;
  next.documentGeneration = session.documentGeneration;
  g_clipboard = std::move(next);
  return true;
}

bool entity_clipboard_has() noexcept { return g_clipboard.recordCount > 0U; }

void entity_clipboard_clear() noexcept { g_clipboard = EntityClipboard{}; }

bool execute_entity_paste(runtime::Entity parent) noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || !world_is_editable() || !entity_clipboard_has() ||
      ((parent != runtime::kInvalidEntity) && !world->is_alive(parent))) {
    return false;
  }
  inspector_commit_pending_edit();
  gizmo_commit_gesture();
  const EntityClipboard &clip = g_clipboard;
  const bool sameDocument =
      clip.documentGeneration == editor_session().documentGeneration;

  auto *command = allocate_command<EntityDuplicateCommand>();
  if (command == nullptr) {
    return false;
  }
  command->records.reset(new (std::nothrow)
                             EntityDuplicateRecord[clip.recordCount]);
  command->rootRecords.reset(new (std::nothrow) std::size_t[clip.rootCount]);
  if ((command->records == nullptr) || (command->rootRecords == nullptr)) {
    delete command;
    return false;
  }
  constexpr std::size_t meshSlot =
      static_cast<std::size_t>(ComponentEditType::Mesh);
  for (std::size_t i = 0U; i < clip.recordCount; ++i) {
    EntityDuplicateRecord &record = command->records[i];
    record = clip.records[i];
    // Every paste makes new entities, with ids of their own.
    record.persistentId = runtime::kInvalidPersistentId;
    // A persistent id names an entity only within its document; one out
    // of the pasted set, copied elsewhere, would name a stranger here.
    if (!sameDocument && record.present[meshSlot] &&
        !copied(clip, record.components.mesh.sceneCaptureSourceId)) {
      record.components.mesh.sceneCaptureSourceId = 0U;
    }
  }
  for (std::size_t k = 0U; k < clip.rootCount; ++k) {
    command->rootRecords[k] = clip.rootRecords[k];
    place_root(*world, parent, sameDocument, clip.rootHasWorld[k],
               clip.rootWorld[k], &command->records[clip.rootRecords[k]]);
  }
  command->recordCount = clip.recordCount;
  command->rootCount = clip.rootCount;
  return execute_duplicate_and_select(command);
}

} // namespace engine::editor
