// Implements the Inspector's component Reset and one-slot component
// clipboard on top of execute_component_batch, so every change is one
// undoable, atomic command however many entities it touches.

#include "editor_component_ops.h"

#include "editor_commands.h"
#include "editor_multi_edit.h"
#include "editor_session.h"

#include "imgui.h"

#include "engine/math/quat.h"

namespace engine::editor {

namespace {

/// The clipboard: one component, the document it was copied in, as in
/// Unity's single component clipboard.
struct ComponentClipboard final {
  bool full = false;
  ComponentEditType type = ComponentEditType::Transform;
  ComponentEditSnapshot value{};
  std::uint64_t documentGeneration = 0U;
};

ComponentClipboard g_clipboard{};

/// Adapts the clipboard's value to `target` (see
/// execute_component_paste_values): what only the source could own stays
/// the target's, or starts fresh.
void adapt_for_target(ComponentEditType type, bool targetHad,
                      const ComponentEditSnapshot &targetCurrent,
                      ComponentEditSnapshot *value) noexcept {
  if (type == ComponentEditType::Transform) {
    value->transform.parentId = targetHad ? targetCurrent.transform.parentId
                                          : runtime::kInvalidPersistentId;
    const math::Quat &q = value->transform.rotation;
    if (((q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w)) <= 1.0e-6F) {
      value->transform.rotation = math::Quat{};
    }
  } else if (type == ComponentEditType::Animation) {
    // The state machine's position is runtime state of the source.
    value->animation.controllerSlot = runtime::kInvalidAnimSlot;
    value->animation.currentState = 0U;
    value->animation.previousState = 0U;
    value->animation.stateTime = 0.0F;
    value->animation.previousStateTime = 0.0F;
    value->animation.blendRemaining = 0.0F;
    value->animation.blendDuration = 0.0F;
    value->animation.paletteSlot = runtime::kInvalidAnimSlot;
  } else if ((type == ComponentEditType::Mesh) &&
             (g_clipboard.documentGeneration !=
              editor_session().documentGeneration)) {
    // A persistent id names an entity only within its document.
    value->mesh.sceneCaptureSourceId = 0U;
  }
}

/// Which component a Reset batch resets.
struct ResetContext final {
  ComponentEditType type = ComponentEditType::Transform;
};

bool fill_reset(void *context, runtime::Entity target, bool exists,
                const ComponentEditSnapshot &current, bool *,
                ComponentEditSnapshot *outAfter) noexcept {
  if (!exists) {
    return false;
  }
  const ComponentEditType type = static_cast<ResetContext *>(context)->type;
  *outAfter = default_component_snapshot(target, type);
  if (type == ComponentEditType::Transform) {
    outAfter->transform.parentId = current.transform.parentId;
  }
  return true;
}

bool fill_paste_values(void *, runtime::Entity, bool exists,
                       const ComponentEditSnapshot &current, bool *,
                       ComponentEditSnapshot *outAfter) noexcept {
  if (!exists) {
    return false;
  }
  *outAfter = g_clipboard.value;
  adapt_for_target(g_clipboard.type, true, current, outAfter);
  return true;
}

bool fill_paste_as_new(void *, runtime::Entity, bool exists,
                       const ComponentEditSnapshot &current,
                       bool *outAfterExists,
                       ComponentEditSnapshot *outAfter) noexcept {
  if (exists) {
    return false;
  }
  *outAfter = g_clipboard.value;
  *outAfterExists = true;
  adapt_for_target(g_clipboard.type, false, current, outAfter);
  return true;
}

} // namespace

bool component_reset_available(ComponentEditType type) noexcept {
  return (type != ComponentEditType::Name) &&
         (type != ComponentEditType::Script);
}

bool execute_component_reset(const runtime::Entity *targets, std::size_t count,
                             ComponentEditType type) noexcept {
  if (!world_is_editable() || !component_reset_available(type)) {
    return false;
  }
  ResetContext context{};
  context.type = type;
  return execute_component_batch(type, targets, count, &fill_reset, &context);
}

bool component_clipboard_copy(runtime::Entity source,
                              ComponentEditType type) noexcept {
  ComponentEditSnapshot value{};
  if ((editor_session().world == nullptr) ||
      !capture_component_snapshot(type, source, &value)) {
    return false;
  }
  g_clipboard.full = true;
  g_clipboard.type = type;
  g_clipboard.value = value;
  g_clipboard.documentGeneration = editor_session().documentGeneration;
  return true;
}

bool component_clipboard_type(ComponentEditType *outType) noexcept {
  if (!g_clipboard.full) {
    return false;
  }
  if (outType != nullptr) {
    *outType = g_clipboard.type;
  }
  return true;
}

void component_clipboard_clear() noexcept {
  g_clipboard = ComponentClipboard{};
}

bool execute_component_paste_values(const runtime::Entity *targets,
                                    std::size_t count) noexcept {
  if (!g_clipboard.full || !world_is_editable()) {
    return false;
  }
  return execute_component_batch(g_clipboard.type, targets, count,
                                 &fill_paste_values, nullptr);
}

bool execute_component_paste_as_new(const runtime::Entity *targets,
                                    std::size_t count) noexcept {
  if (!g_clipboard.full || !world_is_editable()) {
    return false;
  }
  return execute_component_batch(g_clipboard.type, targets, count,
                                 &fill_paste_as_new, nullptr);
}

void draw_component_menu(const runtime::Entity *targets, std::size_t count,
                         ComponentEditType type, bool editable) noexcept {
  if ((targets == nullptr) || (count == 0U) ||
      !ImGui::BeginPopupContextItem("component_menu")) {
    return;
  }
  if (ImGui::MenuItem("Reset", nullptr, false,
                      editable && component_reset_available(type))) {
    static_cast<void>(execute_component_reset(targets, count, type));
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Copy Component")) {
    static_cast<void>(component_clipboard_copy(targets[0], type));
  }
  ComponentEditType held = ComponentEditType::Transform;
  const bool full = component_clipboard_type(&held);
  if (ImGui::MenuItem("Paste Component Values", nullptr, false,
                      editable && full && (held == type))) {
    static_cast<void>(execute_component_paste_values(targets, count));
  }
  bool someoneLacks = false;
  for (std::size_t i = 0U; full && !someoneLacks && (i < count); ++i) {
    ComponentEditSnapshot probe{};
    someoneLacks = !capture_component_snapshot(held, targets[i], &probe);
  }
  if (ImGui::MenuItem("Paste Component As New", nullptr, false,
                      editable && someoneLacks)) {
    static_cast<void>(execute_component_paste_as_new(targets, count));
  }
  ImGui::EndPopup();
}

} // namespace engine::editor
