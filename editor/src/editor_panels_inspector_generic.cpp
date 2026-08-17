// Implements the Inspector's generic, metadata-driven reflected-field
// drawer declared in editor_panels_inspector_generic.h.

#include "editor_panels_inspector_generic.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include <cstdio>
#include <cstring>

#include "editor_inspector_metadata.h"
#include "engine/core/reflect.h"
#include "engine/math/vec2.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"

namespace engine::editor {

namespace {

constexpr float kRadToDeg = 180.0F / 3.14159265358979323846F;
constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;

void mark_modified(bool *modified, bool changed) noexcept {
  if ((modified != nullptr) && changed) {
    *modified = true;
  }
}

/// Resolves the label the widget should show: the field name unless
/// metadata supplies a friendlier display name.
const char *display_label(const core::TypeField &field,
                          const FieldMetadata *meta) noexcept {
  if ((meta != nullptr) && (meta->displayName != nullptr)) {
    return meta->displayName;
  }
  return field.name;
}

/// Appends " (units)" to a label into a caller-owned scratch buffer when
/// metadata supplies units; returns the label unchanged otherwise.
const char *label_with_units(const char *label, const FieldMetadata *meta,
                             char *scratch, std::size_t scratchSize) noexcept {
  if ((meta == nullptr) || (meta->units == nullptr)) {
    return label;
  }
  std::snprintf(scratch, scratchSize, "%s (%s)", label, meta->units);
  return scratch;
}

/// Appends an arbitrary caller-supplied suffix (the multi Inspector's
/// " (mixed)") to a label into a caller-owned scratch buffer; returns the
/// label unchanged when there is no suffix.
const char *label_with_suffix(const char *label, const char *suffix,
                              char *scratch, std::size_t scratchSize) noexcept {
  if (suffix == nullptr) {
    return label;
  }
  std::snprintf(scratch, scratchSize, "%s%s", label, suffix);
  return scratch;
}

void draw_tooltip(const FieldMetadata *meta) noexcept {
  if ((meta != nullptr) && (meta->tooltip != nullptr) &&
      ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", meta->tooltip);
  }
}

void draw_vec2_field(const char *label, math::Vec2 &value,
                     bool *modified) noexcept {
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(modified, ImGui::InputFloat("##x", &value.x));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(modified, ImGui::InputFloat("##y", &value.y));
  ImGui::PopID();
}

void draw_vec3_field(const char *label, math::Vec3 &value,
                     bool *modified) noexcept {
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(modified, ImGui::InputFloat("##x", &value.x));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(modified, ImGui::InputFloat("##y", &value.y));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(modified, ImGui::InputFloat("##z", &value.z));
  ImGui::PopID();
}

void draw_vec4_field(const char *label, math::Vec4 &value,
                     bool *modified) noexcept {
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##x", &value.x));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##y", &value.y));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##z", &value.z));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##w", &value.w));
  ImGui::PopID();
}

void draw_quat_raw_field(const char *label, math::Quat &value,
                         bool *modified) noexcept {
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##x", &value.x));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##y", &value.y));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##z", &value.z));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##w", &value.w));
  ImGui::PopID();
}

/// Draws a Vec3 as three Drag/Slider floats (metadata-opted widgets) rather
/// than the raw x/y/z InputFloat trio.
void draw_vec3_ranged_field(const char *label, math::Vec3 &value,
                            const FieldMetadata &meta,
                            bool *modified) noexcept {
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  const float speed = (meta.speed > 0.0F) ? meta.speed : 0.1F;
  const bool ranged = meta.max > meta.min;
  ImGui::SetNextItemWidth(180.0F);
  bool changed = false;
  if (meta.widget == InspectorWidget::Slider && ranged) {
    changed = ImGui::SliderFloat3("##v", &value.x, meta.min, meta.max);
  } else {
    changed = ImGui::DragFloat3("##v", &value.x, speed, meta.min, meta.max);
  }
  mark_modified(modified, changed);
  ImGui::PopID();
}

/// Draws one radian-valued float field in degrees (SpotLight cone angles,
/// SceneCapture field of view); converts on both read and write.
void draw_angle_degrees_field(const char *label, float &radians,
                              const FieldMetadata &meta,
                              bool *modified) noexcept {
  float degrees = radians * kRadToDeg;
  const bool ranged = meta.max > meta.min;
  bool changed = false;
  if (ranged) {
    changed = ImGui::SliderFloat(label, &degrees, meta.min, meta.max, "%.1f");
  } else {
    const float speed = (meta.speed > 0.0F) ? meta.speed : 1.0F;
    changed = ImGui::DragFloat(label, &degrees, speed, 0.0F, 0.0F, "%.1f");
  }
  if (changed) {
    radians = degrees * kDegToRad;
  }
  mark_modified(modified, changed);
}

/// Draws a Quat field as pitch/yaw/roll degrees (Transform.rotation).
/// Storage stays the quaternion; see euler_degrees_from_quat's round-trip
/// policy comment for the pole (gimbal) behavior this widget inherits.
void draw_euler_degrees_field(const char *label, math::Quat &value,
                              bool *modified) noexcept {
  math::Vec3 degrees = euler_degrees_from_quat(value);
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(180.0F);
  const bool changed = ImGui::DragFloat3("##euler", &degrees.x, 1.0F, 0.0F,
                                         0.0F, "%.1f");
  if (changed) {
    value = quat_from_euler_degrees(degrees);
  }
  mark_modified(modified, changed);
  ImGui::PopID();
}

/// Draws a Uint32 enum-backed field as a named combo from metadata
/// labels; a stored value past the label range keeps a numeric preview so
/// real data is shown rather than silently clamped to the first entry.
void draw_enum_combo_field(const char *label, std::uint32_t &value,
                           const FieldMetadata &meta,
                           bool *modified) noexcept {
  char numericScratch[16] = {};
  const char *preview = nullptr;
  if (value < meta.enumLabelCount) {
    preview = meta.enumLabels[value];
  } else {
    std::snprintf(numericScratch, sizeof(numericScratch), "%u", value);
    preview = numericScratch;
  }
  if (ImGui::BeginCombo(label, preview)) {
    for (std::size_t i = 0U; i < meta.enumLabelCount; ++i) {
      const bool selected = (value == i);
      if (ImGui::Selectable(meta.enumLabels[i], selected) && !selected) {
        value = static_cast<std::uint32_t>(i);
        mark_modified(modified, true);
      }
    }
    ImGui::EndCombo();
  }
}

/// Draws a Uint32 bitmask field as per-bit named checkboxes.
void draw_layer_mask_field(const char *label, std::uint32_t &mask,
                           bool *modified) noexcept {
  ImGui::PushID(label);
  if (ImGui::TreeNode(label)) {
    for (std::uint32_t bit = 0U; bit < kInspectorLayerCount; ++bit) {
      if ((bit % 4U) != 0U) {
        ImGui::SameLine();
      }
      bool set = (mask & (1U << bit)) != 0U;
      ImGui::SetNextItemWidth(90.0F);
      if (ImGui::Checkbox(inspector_layer_name(bit), &set)) {
        if (set) {
          mask |= (1U << bit);
        } else {
          mask &= ~(1U << bit);
        }
        mark_modified(modified, true);
      }
    }
    ImGui::TreePop();
  }
  ImGui::PopID();
}

void draw_field(const core::TypeDescriptor &desc, void *instance,
                const core::TypeField &field, bool showAdvanced,
                bool *modified, const char *labelSuffix = nullptr) noexcept {
  if ((instance == nullptr) || (field.name == nullptr)) {
    return;
  }

  const FieldMetadata *meta = find_field_metadata(desc.name, field.name);
  if ((meta != nullptr) && meta->advanced && !showAdvanced) {
    return;
  }
  const bool readOnly = (meta != nullptr) && meta->readOnly;
  if (readOnly) {
    ImGui::BeginDisabled();
  }

  char labelScratch[96] = {};
  const char *labelWithUnits =
      label_with_units(display_label(field, meta), meta, labelScratch,
                       sizeof(labelScratch));
  char labelScratch2[112] = {};
  const char *label = label_with_suffix(labelWithUnits, labelSuffix,
                                        labelScratch2, sizeof(labelScratch2));

  switch (field.kind) {
  case core::TypeField::Kind::Float: {
    float *value = desc.field_ptr<float>(instance, field);
    if (value == nullptr) {
      break;
    }
    if ((meta != nullptr) && (meta->widget == InspectorWidget::AngleDegrees)) {
      draw_angle_degrees_field(label, *value, *meta, modified);
    } else if ((meta != nullptr) &&
              ((meta->widget == InspectorWidget::Drag) ||
               (meta->widget == InspectorWidget::Slider))) {
      const float speed = (meta->speed > 0.0F) ? meta->speed : 0.1F;
      const bool ranged = meta->max > meta->min;
      bool changed = false;
      if ((meta->widget == InspectorWidget::Slider) && ranged) {
        changed = ImGui::SliderFloat(label, value, meta->min, meta->max);
      } else {
        changed = ImGui::DragFloat(label, value, speed, meta->min, meta->max);
      }
      mark_modified(modified, changed);
    } else {
      mark_modified(modified, ImGui::InputFloat(label, value));
    }
    break;
  }
  case core::TypeField::Kind::Int32: {
    std::int32_t *value = desc.field_ptr<std::int32_t>(instance, field);
    if (value != nullptr) {
      mark_modified(modified, ImGui::InputScalar(label, ImGuiDataType_S32,
                                                 value));
    }
    break;
  }
  case core::TypeField::Kind::Uint32: {
    std::uint32_t *value = desc.field_ptr<std::uint32_t>(instance, field);
    if (value == nullptr) {
      break;
    }
    if ((meta != nullptr) && (meta->widget == InspectorWidget::LayerMask)) {
      draw_layer_mask_field(label, *value, modified);
    } else if ((meta != nullptr) && (meta->widget == InspectorWidget::Enum) &&
               (meta->enumLabels != nullptr) && (meta->enumLabelCount > 0U)) {
      draw_enum_combo_field(label, *value, *meta, modified);
    } else {
      mark_modified(modified,
                    ImGui::InputScalar(label, ImGuiDataType_U32, value));
    }
    break;
  }
  case core::TypeField::Kind::Bool: {
    bool *value = desc.field_ptr<bool>(instance, field);
    if (value != nullptr) {
      mark_modified(modified, ImGui::Checkbox(label, value));
    }
    break;
  }
  case core::TypeField::Kind::Vec2: {
    math::Vec2 *value = desc.field_ptr<math::Vec2>(instance, field);
    if (value != nullptr) {
      draw_vec2_field(label, *value, modified);
    }
    break;
  }
  case core::TypeField::Kind::Vec3: {
    math::Vec3 *value = desc.field_ptr<math::Vec3>(instance, field);
    if (value == nullptr) {
      break;
    }
    if ((meta != nullptr) && (meta->widget == InspectorWidget::Color)) {
      mark_modified(modified, ImGui::ColorEdit3(label, &value->x));
    } else if ((meta != nullptr) &&
              ((meta->widget == InspectorWidget::Drag) ||
               (meta->widget == InspectorWidget::Slider))) {
      draw_vec3_ranged_field(label, *value, *meta, modified);
    } else {
      draw_vec3_field(label, *value, modified);
    }
    break;
  }
  case core::TypeField::Kind::Vec4: {
    math::Vec4 *value = desc.field_ptr<math::Vec4>(instance, field);
    if (value != nullptr) {
      draw_vec4_field(label, *value, modified);
    }
    break;
  }
  case core::TypeField::Kind::Quat: {
    math::Quat *value = desc.field_ptr<math::Quat>(instance, field);
    if (value == nullptr) {
      break;
    }
    if ((meta != nullptr) && (meta->widget == InspectorWidget::EulerDegrees)) {
      draw_euler_degrees_field(label, *value, modified);
    } else {
      draw_quat_raw_field(label, *value, modified);
    }
    break;
  }
  }

  draw_tooltip(meta);
  if (readOnly) {
    ImGui::EndDisabled();
  }
}

} // namespace

bool draw_reflected_component_fields(const char *typeName, void *instance,
                                     bool showAdvanced) noexcept {
  if ((typeName == nullptr) || (instance == nullptr)) {
    return false;
  }

  const core::TypeDescriptor *desc =
      core::global_type_registry().find_type(typeName);
  if (desc == nullptr) {
    return false;
  }

  bool modified = false;
  const char *lastCategory = nullptr;
  for (std::size_t i = 0U; i < desc->fieldCount; ++i) {
    const core::TypeField &field = desc->fields[i];
    const FieldMetadata *meta = find_field_metadata(desc->name, field.name);
    const char *category = (meta != nullptr) ? meta->category : "General";
    if ((lastCategory == nullptr) || (std::strcmp(lastCategory, category) != 0)) {
      if (std::strcmp(category, "General") != 0) {
        ImGui::SeparatorText(category);
      }
      lastCategory = category;
    }
    draw_field(*desc, instance, field, showAdvanced, &modified);
  }

  return modified;
}

bool draw_reflected_field(const char *typeName, const char *fieldName,
                         void *instance, const char *labelSuffix,
                         bool showAdvanced) noexcept {
  if ((typeName == nullptr) || (fieldName == nullptr) ||
      (instance == nullptr)) {
    return false;
  }

  const core::TypeDescriptor *desc =
      core::global_type_registry().find_type(typeName);
  if (desc == nullptr) {
    return false;
  }
  const core::TypeField *field = desc->find_field(fieldName);
  if (field == nullptr) {
    return false;
  }

  bool modified = false;
  draw_field(*desc, instance, *field, showAdvanced, &modified, labelSuffix);
  return modified;
}

} // namespace engine::editor
