// Implements the Inspector's numeric widgets declared in
// editor_inspector_widgets.h. One draft is enough: ImGui has one active
// text field at a time, and the draft is keyed by that field's id so it
// never follows a different widget. The three-component variants draw
// three single widgets under one id scope, so each component keeps its
// own text-input session, as ImGui's own DragFloat3 does.

#include "editor_inspector_widgets.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>

namespace engine::editor {

namespace {

/// The value typed so far into the one field being edited by text.
struct TextEntryDraft final {
  ImGuiID id = 0U;
  double value = 0.0;
  bool active = false;
};

TextEntryDraft g_draft{};

/// The draft for the widget about to be drawn, or the stored value when
/// no text edit is in progress on it.
template <typename T> T entry_start(ImGuiID id, const T *value) noexcept {
  if (g_draft.active && (g_draft.id == id)) {
    return static_cast<T>(g_draft.value);
  }
  return *value;
}

/// Finishes one widget drawn this frame with `draft` as its value.
/// `typing` says the widget is in a text-input session right now: the
/// draft is kept and nothing is reported. On the frame a session ends
/// after an edit, the draft is written to `value` and reported once,
/// unless Escape ended it. Outside a session a change is a scrub and
/// applies at once.
template <typename T>
bool entry_finish(ImGuiID id, bool typing, bool changed, T draft,
                  T *value) noexcept {
  const bool held = g_draft.active && (g_draft.id == id);
  if (typing) {
    if (changed || held) {
      g_draft.active = true;
      g_draft.id = id;
      g_draft.value = static_cast<double>(draft);
    }
    return false;
  }
  if (held) {
    g_draft.active = false;
    if (!ImGui::IsItemDeactivatedAfterEdit() ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      return false;
    }
    *value = draft;
    return true;
  }
  if (changed) {
    *value = draft;
  }
  return changed;
}

/// Draws three of one widget across the current item width, each under
/// its own id so its text session and draft are its own.
template <typename DrawFn>
bool draw_three(const char *label, float *values, DrawFn draw) noexcept {
  ImGui::BeginGroup();
  ImGui::PushID(label);
  const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
  const float width = std::floor((ImGui::CalcItemWidth() - (2.0F * spacing)) /
                                 3.0F);
  bool changed = false;
  for (int i = 0; i < 3; ++i) {
    ImGui::PushID(i);
    if (i > 0) {
      ImGui::SameLine(0.0F, spacing);
    }
    ImGui::SetNextItemWidth(width);
    changed = draw("##component", &values[i]) || changed;
    ImGui::PopID();
  }
  ImGui::PopID();
  ImGui::EndGroup();
  return changed;
}

} // namespace

bool inspector_input_float(const char *label, float *value) noexcept {
  const ImGuiID id = ImGui::GetID(label);
  float draft = entry_start(id, value);
  const bool changed = ImGui::InputFloat(label, &draft);
  return entry_finish(id, ImGui::IsItemActive(), changed, draft, value);
}

bool inspector_input_int(const char *label, std::int32_t *value) noexcept {
  const ImGuiID id = ImGui::GetID(label);
  std::int32_t draft = entry_start(id, value);
  const bool changed = ImGui::InputScalar(label, ImGuiDataType_S32, &draft);
  return entry_finish(id, ImGui::IsItemActive(), changed, draft, value);
}

bool inspector_input_uint(const char *label, std::uint32_t *value) noexcept {
  const ImGuiID id = ImGui::GetID(label);
  std::uint32_t draft = entry_start(id, value);
  const bool changed = ImGui::InputScalar(label, ImGuiDataType_U32, &draft);
  return entry_finish(id, ImGui::IsItemActive(), changed, draft, value);
}

bool inspector_drag_float(const char *label, float *value, float speed,
                          float min, float max, const char *format) noexcept {
  const ImGuiID id = ImGui::GetID(label);
  float draft = entry_start(id, value);
  const bool changed = ImGui::DragFloat(label, &draft, speed, min, max, format);
  return entry_finish(id, ImGui::TempInputIsActive(id), changed, draft,
                      value);
}

bool inspector_slider_float(const char *label, float *value, float min,
                            float max, const char *format) noexcept {
  const ImGuiID id = ImGui::GetID(label);
  float draft = entry_start(id, value);
  const bool changed = ImGui::SliderFloat(label, &draft, min, max, format);
  return entry_finish(id, ImGui::TempInputIsActive(id), changed, draft,
                      value);
}

bool inspector_drag_float3(const char *label, float *values, float speed,
                           float min, float max, const char *format) noexcept {
  return draw_three(label, values, [&](const char *l, float *v) noexcept {
    return inspector_drag_float(l, v, speed, min, max, format);
  });
}

bool inspector_slider_float3(const char *label, float *values, float min,
                             float max, const char *format) noexcept {
  return draw_three(label, values, [&](const char *l, float *v) noexcept {
    return inspector_slider_float(l, v, min, max, format);
  });
}

} // namespace engine::editor
