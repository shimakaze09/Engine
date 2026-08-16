// Implements the Inspector's per-component custom drawers and the
// registry-generated Add Component menu declared in
// editor_panels_inspector_custom.h.

#include "editor_panels_inspector_custom.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "editor_commands.h"
#include "editor_inspector_metadata.h"
#include "editor_reference_pickers.h"
#include "editor_session.h"
#include "engine/renderer/asset_database.h"

namespace engine::editor {

namespace {

void mark_modified(bool *modified, bool changed) noexcept {
  if ((modified != nullptr) && changed) {
    *modified = true;
  }
}

/// Predicate restricting the mesh's scene-capture-source entity picker to
/// entities that actually carry a SceneCaptureComponent.
bool entity_has_scene_capture(const runtime::World &world,
                              runtime::Entity entity) noexcept {
  runtime::SceneCaptureComponent capture{};
  return world.get_scene_capture_component(entity, &capture);
}

} // namespace

bool draw_mesh_component_fields(runtime::Entity entity,
                                runtime::MeshComponent &mesh,
                                bool editable) noexcept {
  bool modifiedValue = false;
  bool *modified = &modifiedValue;
  if (!editable) {
    ImGui::BeginDisabled();
  }

  mark_modified(modified,
               draw_asset_reference_picker("Mesh", renderer::AssetTypeTag::Mesh,
                                          &mesh.meshAssetId));
  mark_modified(modified, draw_asset_reference_picker(
                              "Material", renderer::AssetTypeTag::Material,
                              &mesh.materialAssetId));
  mark_modified(modified, ImGui::ColorEdit3("Albedo", &mesh.albedo.x));
  mark_modified(modified, ImGui::SliderFloat("Roughness", &mesh.roughness,
                                             0.0F, 1.0F, "%.2f"));
  mark_modified(modified,
                ImGui::SliderFloat("Metallic", &mesh.metallic, 0.0F, 1.0F,
                                   "%.2f"));
  mark_modified(modified,
                ImGui::SliderFloat("Opacity", &mesh.opacity, 0.0F, 1.0F,
                                   "%.2f"));
  mark_modified(modified, draw_entity_reference_picker(
                              "Capture Source", &mesh.sceneCaptureSourceId,
                              entity_has_scene_capture));

  if (editable) {
    // Blockout material presets: named albedo/roughness/metallic/opacity
    // sets applied as one undoable edit through the history (the caller's
    // direct apply is suppressed for that frame).
    static const struct {
      const char *name;
      math::Vec3 albedo;
      float roughness;
      float metallic;
      float opacity;
    } kMaterialPresets[] = {
        {"Grass", math::Vec3(0.22F, 0.55F, 0.21F), 0.90F, 0.0F, 1.0F},
        {"Stone", math::Vec3(0.45F, 0.45F, 0.47F), 0.85F, 0.0F, 1.0F},
        {"Sand", math::Vec3(0.83F, 0.75F, 0.55F), 0.95F, 0.0F, 1.0F},
        {"Wood", math::Vec3(0.48F, 0.33F, 0.19F), 0.80F, 0.0F, 1.0F},
        {"Snow", math::Vec3(0.92F, 0.94F, 0.97F), 0.70F, 0.0F, 1.0F},
        {"Metal", math::Vec3(0.75F, 0.77F, 0.80F), 0.30F, 1.0F, 1.0F},
        {"Water", math::Vec3(0.12F, 0.35F, 0.55F), 0.15F, 0.0F, 0.75F},
    };
    if (ImGui::BeginCombo("Preset", "Apply preset...")) {
      for (const auto &preset : kMaterialPresets) {
        if (ImGui::Selectable(preset.name)) {
          ComponentEditSnapshot after{};
          after.mesh = mesh;
          after.mesh.albedo = preset.albedo;
          after.mesh.roughness = preset.roughness;
          after.mesh.metallic = preset.metallic;
          after.mesh.opacity = preset.opacity;
          execute_component_add(entity, ComponentEditType::Mesh,
                                after);
          modifiedValue = false;
        }
      }
      ImGui::EndCombo();
    }
  }

  if (!editable) {
    ImGui::EndDisabled();
  }
  return modifiedValue;
}

bool draw_light_type_combo(runtime::LightComponent &light) noexcept {
  constexpr const char *kLightTypeNames[] = {"Directional", "Point"};
  int currentType = static_cast<int>(light.type);
  if (ImGui::Combo("Type", &currentType, kLightTypeNames, 2)) {
    light.type = static_cast<runtime::LightType>(currentType);
    return true;
  }
  return false;
}

bool draw_script_component_fields(runtime::ScriptComponent &script) noexcept {
  return draw_path_reference_picker("Script Path", script.scriptPath,
                                    sizeof(script.scriptPath), ".lua");
}

bool draw_animation_component_fields(
    runtime::AnimationComponent &animation) noexcept {
  bool modified = draw_path_reference_picker(
      "Controller Path", animation.controllerPath,
      sizeof(animation.controllerPath), ".json");
  mark_modified(&modified, ImGui::Checkbox("Playing", &animation.playing));
  mark_modified(&modified, ImGui::DragFloat("Playback Speed",
                                            &animation.playbackSpeed, 0.01F,
                                            0.0F, 8.0F));
  return modified;
}

void draw_foliage_patch_fields(runtime::Entity entity,
                               runtime::FoliagePatchComponent &foliage,
                               bool editable, bool *modified) noexcept {
  if (!editable) {
    ImGui::BeginDisabled();
  }

  for (std::size_t lod = 0U; lod < runtime::FoliagePatchComponent::kMaxLods;
      ++lod) {
    char label[32] = {};
    std::snprintf(label, sizeof(label), "LOD %zu Mesh", lod);
    mark_modified(modified,
                  draw_asset_reference_picker(label,
                                             renderer::AssetTypeTag::Mesh,
                                             &foliage.meshAssetIds[lod]));
  }

  int instanceCount = static_cast<int>(foliage.instanceCount);
  if (ImGui::SliderInt(
          "Instance Count", &instanceCount, 0,
          static_cast<int>(runtime::FoliagePatchComponent::kMaxInstances))) {
    if (instanceCount < 0) {
      instanceCount = 0;
    }
    foliage.instanceCount = static_cast<std::uint32_t>(instanceCount);
    mark_modified(modified, true);
  }

  mark_modified(modified, ImGui::DragFloat("Density", &foliage.density, 0.05F,
                                           0.0F, 100.0F, "%.2f"));
  mark_modified(modified, ImGui::ColorEdit3("Albedo", &foliage.albedo.x));
  mark_modified(modified, ImGui::SliderFloat("Roughness", &foliage.roughness,
                                             0.0F, 1.0F, "%.2f"));
  mark_modified(modified, ImGui::SliderFloat("Metallic", &foliage.metallic,
                                             0.0F, 1.0F, "%.2f"));
  mark_modified(modified, ImGui::SliderFloat("Opacity", &foliage.opacity, 0.0F,
                                             1.0F, "%.2f"));
  mark_modified(modified,
                ImGui::DragFloat("Wind Strength", &foliage.windStrength, 0.01F,
                                 0.0F, 5.0F, "%.2f"));
  mark_modified(modified,
                ImGui::DragFloat("Wind Frequency", &foliage.windFrequency,
                                 0.05F, 0.0F, 20.0F, "%.2f"));

  if (ImGui::TreeNode("Instances")) {
    std::uint32_t visibleCount = foliage.instanceCount;
    if (visibleCount > static_cast<std::uint32_t>(
                           runtime::FoliagePatchComponent::kMaxInstances)) {
      visibleCount = static_cast<std::uint32_t>(
          runtime::FoliagePatchComponent::kMaxInstances);
    }

    std::uint32_t removeIndex = visibleCount;
    for (std::uint32_t i = 0U; i < visibleCount; ++i) {
      runtime::FoliageInstance &instance = foliage.instances[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::Separator();
      ImGui::Text("Instance %u", i);
      if (editable) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
          removeIndex = i;
        }
      }
      mark_modified(modified,
                    ImGui::DragFloat3("Offset", &instance.offset.x, 0.05F));
      mark_modified(modified, ImGui::DragFloat("Scale", &instance.scale, 0.01F,
                                               0.05F, 10.0F, "%.2f"));
      mark_modified(modified, ImGui::DragFloat("Phase", &instance.phase, 0.05F,
                                               -100.0F, 100.0F, "%.2f"));
      int lodIndex = static_cast<int>(instance.lodIndex);
      if (ImGui::SliderInt(
              "LOD Index", &lodIndex, 0,
              static_cast<int>(runtime::FoliagePatchComponent::kMaxLods -
                               1U))) {
        instance.lodIndex = static_cast<std::uint32_t>(lodIndex);
        mark_modified(modified, true);
      }
      ImGui::PopID();
    }

    if (editable && (removeIndex < visibleCount)) {
      // The structural edit routes through the command history from the
      // local copy (which carries any same-frame scrubs), so the caller's
      // direct apply is suppressed to keep the recorded edit authoritative.
      ComponentEditSnapshot after{};
      after.foliagePatch = foliage;
      runtime::FoliagePatchComponent &patch = after.foliagePatch;
      for (std::uint32_t i = removeIndex + 1U; i < visibleCount; ++i) {
        patch.instances[i - 1U] = patch.instances[i];
      }
      patch.instanceCount = visibleCount - 1U;
      patch.instances[patch.instanceCount] = runtime::FoliageInstance{};
      execute_component_add(entity, ComponentEditType::FoliagePatch, after);
      *modified = false;
    }

    const bool atCapacity =
        foliage.instanceCount >=
        static_cast<std::uint32_t>(
            runtime::FoliagePatchComponent::kMaxInstances);
    if (editable && !atCapacity && ImGui::Button("Add Instance")) {
      ComponentEditSnapshot after{};
      after.foliagePatch = foliage;
      runtime::FoliagePatchComponent &patch = after.foliagePatch;
      patch.instances[visibleCount] = runtime::FoliageInstance{};
      patch.instanceCount = visibleCount + 1U;
      execute_component_add(entity, ComponentEditType::FoliagePatch, after);
      *modified = false;
    }
    ImGui::TreePop();
  }

  if (!editable) {
    ImGui::EndDisabled();
  }
}

void draw_scene_capture_preview(
    runtime::Entity entity,
    const runtime::SceneCaptureComponent &capture) noexcept {
  const runtime::PersistentId capturePersistentId =
      editor_session().world->persistent_id(entity);
  ImGui::Text("Capture Source Id: %u",
              static_cast<unsigned>(capturePersistentId));

  // Live preview of the capture output, fitted to the panel width (flipped
  // V to match the GL framebuffer origin, like the viewport).
  const std::int32_t captureSlot =
      editor_session().world->scene_capture_slot_for_entity(entity);
  const std::uint32_t captureTexture =
      (captureSlot >= 0)
          ? renderer::get_scene_capture_texture(
                static_cast<std::size_t>(captureSlot))
          : 0U;
  if ((captureTexture != 0U) && (capture.width > 0U)) {
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float aspect =
        static_cast<float>(capture.height) / static_cast<float>(capture.width);
    const ImVec2 previewSize(availWidth, availWidth * aspect);
    ImGui::Image(static_cast<ImTextureID>(captureTexture), previewSize,
                ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));
  } else {
    ImGui::TextUnformatted(capture.enabled ? "Preview: waiting for first frame"
                                           : "Preview: capture disabled");
  }
}

namespace {

bool contains_ci_local(const char *haystack, const char *needle) noexcept {
  if ((haystack == nullptr) || (needle == nullptr) || (needle[0] == '\0')) {
    return true;
  }
  const std::size_t haystackLen = std::strlen(haystack);
  const std::size_t needleLen = std::strlen(needle);
  if (needleLen > haystackLen) {
    return false;
  }
  for (std::size_t start = 0U; start <= (haystackLen - needleLen); ++start) {
    std::size_t i = 0U;
    for (; i < needleLen; ++i) {
      if (std::tolower(static_cast<unsigned char>(haystack[start + i])) !=
          std::tolower(static_cast<unsigned char>(needle[i]))) {
        break;
      }
    }
    if (i == needleLen) {
      return true;
    }
  }
  return false;
}

/// One Add Component menu candidate, gathered from the persistent-component
/// registry so a new registry row appears here without a matching manual
/// branch (issue #156).
struct AddMenuEntry final {
  ComponentEditType type = ComponentEditType::Transform;
  const char *displayName = nullptr;
  const char *category = "General";
  const char *tooltip = nullptr;
};

} // namespace

void draw_add_component_menu(runtime::Entity entity, bool editable) noexcept {
  if (!editable || (editor_session().world == nullptr)) {
    return;
  }

  AddMenuEntry candidates[kComponentEditTypeCount];
  std::size_t candidateCount = 0U;
#define ENGINE_ICR_ADDMENU_GATHER(Type, Key, GetFn, AddFn, RemoveFn)          \
  {                                                                            \
    const ComponentEditType editType = ComponentEditType::ENGINE_ICR_ALIAS(Type); \
    if (!has_component_of_type(editType, entity)) {                           \
      const ComponentMetadata *meta =                                        \
          find_component_metadata("engine::runtime::" #Type);                \
      AddMenuEntry &entry = candidates[candidateCount++];                    \
      entry.type = editType;                                                 \
      entry.displayName = (meta != nullptr) ? meta->displayName : #Type;     \
      entry.category = (meta != nullptr) ? meta->category : "General";       \
      entry.tooltip = (meta != nullptr) ? meta->tooltip : nullptr;           \
    }                                                                         \
  }
  ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_ICR_ADDMENU_GATHER)
#undef ENGINE_ICR_ADDMENU_GATHER

  // Stable sort by category so same-category entries render contiguously
  // under one header, without reordering the authoritative registry table
  // itself (its row order is cross-checked against World's type list).
  for (std::size_t i = 1U; i < candidateCount; ++i) {
    AddMenuEntry key = candidates[i];
    std::size_t j = i;
    while ((j > 0U) &&
          (std::strcmp(candidates[j - 1U].category, key.category) > 0)) {
      candidates[j] = candidates[j - 1U];
      --j;
    }
    candidates[j] = key;
  }

  ImGui::Separator();
  if (!ImGui::BeginCombo("##addcomp", "Add Component...")) {
    return;
  }
  static char filter[64] = {};
  ImGui::SetNextItemWidth(-1.0F);
  ImGui::InputTextWithHint("##addcompfilter", "Search...", filter,
                           sizeof(filter));

  const char *lastCategory = nullptr;
  for (std::size_t i = 0U; i < candidateCount; ++i) {
    const AddMenuEntry &entry = candidates[i];
    if (!contains_ci_local(entry.displayName, filter)) {
      continue;
    }
    if ((lastCategory == nullptr) ||
        (std::strcmp(lastCategory, entry.category) != 0)) {
      ImGui::SeparatorText(entry.category);
      lastCategory = entry.category;
    }
    if (ImGui::Selectable(entry.displayName)) {
      execute_component_add(entity, entry.type,
                            default_component_snapshot(entity, entry.type));
    }
    if ((entry.tooltip != nullptr) && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", entry.tooltip);
    }
  }

  ImGui::EndCombo();
}

} // namespace engine::editor
