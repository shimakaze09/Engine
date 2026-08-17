// Implements play-mode live-edit support declared in editor_live_edit.h.

#include "editor_live_edit.h"

#include <array>
#include <cstddef>

#include "editor_commands.h"
#include "engine/core/logging.h"
#include "engine/runtime/world.h"

namespace engine::editor {

namespace {

/// One tracked live-edit baseline: whether this play session has touched
/// (entity, type) via a live edit since Play started (or since the last
/// Revert), so the Inspector's runtime-edit badge and Revert have
/// something to key off without re-deriving it from the authored JSON
/// snapshot -- Stop always discards the whole running world anyway, so the
/// baseline only needs to survive for the current session, not persist.
struct LiveEditBaseline final {
  bool active = false;
  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  ComponentEditType type = ComponentEditType::Transform;
  ComponentEditSnapshot baseline{};
};

/// One queued "Apply to authored value" entry: the exact value captured at
/// click time (frozen, not re-read at Stop) so a tweak made after clicking
/// Apply and never re-applied cannot silently leak into the authored scene.
struct PendingAuthoredApply final {
  bool active = false;
  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  ComponentEditType type = ComponentEditType::Transform;
  ComponentEditSnapshot snapshot{};
};

// Both tables are keyed by (PersistentId, ComponentEditType) and persist
// across selection changes until Revert/cancel/Stop, so their budget is
// independent of the simultaneous-selection capacity (audit #224): one
// entity consumes one slot per live-editable component type it touches,
// and sequentially edited entities accumulate. 128 pairs covers e.g. 16
// entities times 8 component types in one session at ~313 KB of static
// storage per table (sizeof(ComponentEditSnapshot) is ~2.4 KB).
constexpr std::size_t kMaxLiveEditBaselines = 128U;
constexpr std::size_t kMaxPendingAuthoredApplies = 128U;

std::array<LiveEditBaseline, kMaxLiveEditBaselines> g_baselines{};
std::array<PendingAuthoredApply, kMaxPendingAuthoredApplies> g_pending{};

LiveEditBaseline *find_baseline(runtime::PersistentId id,
                                ComponentEditType type) noexcept {
  for (auto &entry : g_baselines) {
    if (entry.active && (entry.persistentId == id) && (entry.type == type)) {
      return &entry;
    }
  }
  return nullptr;
}

LiveEditBaseline *allocate_baseline() noexcept {
  for (auto &entry : g_baselines) {
    if (!entry.active) {
      return &entry;
    }
  }
  return nullptr;
}

PendingAuthoredApply *find_pending(runtime::PersistentId id,
                                   ComponentEditType type) noexcept {
  for (auto &entry : g_pending) {
    if (entry.active && (entry.persistentId == id) && (entry.type == type)) {
      return &entry;
    }
  }
  return nullptr;
}

PendingAuthoredApply *allocate_pending() noexcept {
  for (auto &entry : g_pending) {
    if (!entry.active) {
      return &entry;
    }
  }
  return nullptr;
}

} // namespace

bool live_edit_available() noexcept {
  const EditorSession &session = editor_session();
  return session.liveEditEnabled && (session.world != nullptr) &&
         !session.worldRestoreFailed &&
         (session.playState != PlayState::Stopped) &&
         (session.world->current_phase() == runtime::WorldPhase::Input);
}

bool apply_live_component_edit(runtime::Entity entity, ComponentEditType type,
                               const ComponentEditSnapshot &after) noexcept {
  if (!live_edit_available()) {
    return false;
  }
  runtime::World *const world = editor_session().world;
  if (!world->is_alive(entity)) {
    return false;
  }
  const runtime::PersistentId id = world->persistent_id(entity);
  if (id == runtime::kInvalidPersistentId) {
    return false;
  }

  LiveEditBaseline *created = nullptr;
  if (find_baseline(id, type) == nullptr) {
    // The baseline is acquired BEFORE the mutation: a live edit whose
    // advertised Revert cannot be provided must be refused up front, not
    // applied with the affordance silently missing (audit #224).
    ComponentEditSnapshot current{};
    if (!capture_component_snapshot(type, entity, &current)) {
      return false;
    }
    created = allocate_baseline();
    if (created == nullptr) {
      core::log_message(core::LogLevel::Warning, "editor",
                        "live-edit budget exhausted; edit blocked so Revert "
                        "stays available — revert an edit or Stop to free "
                        "entries");
      return false;
    }
    created->active = true;
    created->persistentId = id;
    created->type = type;
    created->baseline = current;
  }

  if (!apply_component_snapshot(type, entity, true, after)) {
    // Drop a baseline created for a rejected write so an untouched pair
    // does not wear the "edited live" badge or hold a budget slot.
    if (created != nullptr) {
      *created = LiveEditBaseline{};
    }
    return false;
  }
  return true;
}

bool has_live_component_edit(runtime::Entity entity,
                             ComponentEditType type) noexcept {
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return false;
  }
  return find_baseline(world->persistent_id(entity), type) != nullptr;
}

bool revert_live_component_edit(runtime::Entity entity,
                                ComponentEditType type) noexcept {
  runtime::World *const world = editor_session().world;
  if ((world == nullptr) || !world->is_alive(entity)) {
    return false;
  }
  const runtime::PersistentId id = world->persistent_id(entity);
  LiveEditBaseline *slot = find_baseline(id, type);
  if (slot == nullptr) {
    return false;
  }
  if (!apply_component_snapshot(type, entity, true, slot->baseline)) {
    return false;
  }
  *slot = LiveEditBaseline{};
  return true;
}

bool queue_apply_to_authored(runtime::Entity entity,
                             ComponentEditType type) noexcept {
  if (!live_edit_available()) {
    return false;
  }
  runtime::World *const world = editor_session().world;
  if (!world->is_alive(entity)) {
    return false;
  }
  const runtime::PersistentId id = world->persistent_id(entity);
  if (id == runtime::kInvalidPersistentId) {
    return false;
  }
  ComponentEditSnapshot current{};
  if (!capture_component_snapshot(type, entity, &current)) {
    return false;
  }

  PendingAuthoredApply *slot = find_pending(id, type);
  if (slot == nullptr) {
    slot = allocate_pending();
  }
  if (slot == nullptr) {
    core::log_message(core::LogLevel::Warning, "editor",
                      "apply-to-authored queue full; edit not queued");
    return false;
  }
  slot->active = true;
  slot->persistentId = id;
  slot->type = type;
  slot->snapshot = current;
  return true;
}

bool has_pending_apply_to_authored(runtime::Entity entity,
                                   ComponentEditType type) noexcept {
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return false;
  }
  return find_pending(world->persistent_id(entity), type) != nullptr;
}

void cancel_apply_to_authored(runtime::Entity entity,
                              ComponentEditType type) noexcept {
  runtime::World *const world = editor_session().world;
  if (world == nullptr) {
    return;
  }
  PendingAuthoredApply *slot =
      find_pending(world->persistent_id(entity), type);
  if (slot != nullptr) {
    *slot = PendingAuthoredApply{};
  }
}

void reset_live_edit_state() noexcept {
  g_baselines.fill(LiveEditBaseline{});
  g_pending.fill(PendingAuthoredApply{});
}

std::size_t live_edit_baseline_count() noexcept {
  std::size_t count = 0U;
  for (const auto &entry : g_baselines) {
    if (entry.active) {
      ++count;
    }
  }
  return count;
}

std::size_t live_edit_baseline_capacity() noexcept {
  return kMaxLiveEditBaselines;
}

std::size_t pending_apply_to_authored_capacity() noexcept {
  return kMaxPendingAuthoredApplies;
}

std::size_t pending_apply_to_authored_count() noexcept {
  std::size_t count = 0U;
  for (const auto &entry : g_pending) {
    if (entry.active) {
      ++count;
    }
  }
  return count;
}

std::size_t replay_pending_authored_applies() noexcept {
  runtime::World *const world = editor_session().world;
  std::size_t applied = 0U;
  if (world == nullptr) {
    reset_live_edit_state();
    return 0U;
  }

  for (auto &entry : g_pending) {
    if (!entry.active) {
      continue;
    }
    const runtime::Entity target =
        world->find_entity_by_persistent_id(entry.persistentId);
    if (target == runtime::kInvalidEntity) {
      core::log_message(
          core::LogLevel::Warning, "editor",
          "apply-to-authored target no longer exists in the restored "
          "scene; edit dropped");
      continue;
    }
    ComponentEditSnapshot before{};
    const bool beforeExists =
        capture_component_snapshot(entry.type, target, &before);

    auto *cmd = new (std::nothrow) ComponentEditCommand();
    if (cmd == nullptr) {
      core::log_message(core::LogLevel::Error, "editor",
                        "apply-to-authored command allocation failed");
      continue;
    }
    cmd->entity = target;
    cmd->persistentId = entry.persistentId;
    cmd->type = entry.type;
    cmd->beforeExists = beforeExists;
    cmd->before = before;
    cmd->afterExists = true;
    cmd->after = entry.snapshot;
    if (editor_session().commandHistory.execute(cmd)) {
      ++applied;
    }
  }

  reset_live_edit_state();
  return applied;
}

} // namespace engine::editor
