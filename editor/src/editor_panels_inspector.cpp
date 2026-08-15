// Implements the editor inspector panel and its reflected field editors.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_panels_inspector.h"

#include "editor_commands.h"
#include "editor_session.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/profiler.h"
#include "engine/core/reflect.h"
#include "engine/editor/editor_camera.h"
#include "engine/engine.h"
#include "engine/math/transform.h"
#include "engine/math/vec2.h"
#include "engine/math/vec4.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "ImGuizmo.h"

#include "engine/editor/command_history.h"
#include "engine/editor/debug_camera.h"

#include <stb_image.h>

namespace engine::editor {

namespace {

void mark_modified(bool *modified, bool changed) noexcept {
  if ((modified != nullptr) && changed) {
    *modified = true;
  }
}

void draw_vec2_field(const char *label, math::Vec2 &value,
                     bool *modified) noexcept {
  constexpr ImGuiInputTextFlags kCommitFlags = ImGuiInputTextFlags_None;

  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(modified, ImGui::InputFloat("##x", &value.x, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(modified, ImGui::InputFloat("##y", &value.y, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::PopID();
}

void draw_vec3_field(const char *label, math::Vec3 &value,
                     bool *modified) noexcept {
  constexpr ImGuiInputTextFlags kCommitFlags = ImGuiInputTextFlags_None;

  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(modified, ImGui::InputFloat("##x", &value.x, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(modified, ImGui::InputFloat("##y", &value.y, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  mark_modified(modified, ImGui::InputFloat("##z", &value.z, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::PopID();
}

void draw_vec4_field(const char *label, math::Vec4 &value,
                     bool *modified) noexcept {
  constexpr ImGuiInputTextFlags kCommitFlags = ImGuiInputTextFlags_None;

  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##x", &value.x, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##y", &value.y, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##z", &value.z, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##w", &value.w, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::PopID();
}

void draw_quat_field(const char *label, math::Quat &value,
                     bool *modified) noexcept {
  constexpr ImGuiInputTextFlags kCommitFlags = ImGuiInputTextFlags_None;

  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##x", &value.x, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##y", &value.y, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##z", &value.z, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  mark_modified(modified, ImGui::InputFloat("##w", &value.w, 0.0F, 0.0F, "%.3f",
                                            kCommitFlags));
  ImGui::PopID();
}

void draw_field(const core::TypeDescriptor &desc, void *instance,
                const core::TypeField &field, bool *modified) noexcept {
  if ((instance == nullptr) || (field.name == nullptr)) {
    return;
  }

  constexpr ImGuiInputTextFlags kCommitFlags = ImGuiInputTextFlags_None;

  switch (field.kind) {
  case core::TypeField::Kind::Float: {
    float *value = desc.field_ptr<float>(instance, field);
    if (value != nullptr) {
      mark_modified(modified, ImGui::InputFloat(field.name, value, 0.0F, 0.0F,
                                                "%.3f", kCommitFlags));
    }
    break;
  }
  case core::TypeField::Kind::Int32: {
    std::int32_t *value = desc.field_ptr<std::int32_t>(instance, field);
    if (value != nullptr) {
      mark_modified(modified,
                    ImGui::InputScalar(field.name, ImGuiDataType_S32, value,
                                       nullptr, nullptr, "%d", kCommitFlags));
    }
    break;
  }
  case core::TypeField::Kind::Uint32: {
    std::uint32_t *value = desc.field_ptr<std::uint32_t>(instance, field);
    if (value != nullptr) {
      mark_modified(modified,
                    ImGui::InputScalar(field.name, ImGuiDataType_U32, value,
                                       nullptr, nullptr, "%u", kCommitFlags));
    }
    break;
  }
  case core::TypeField::Kind::Bool: {
    bool *value = desc.field_ptr<bool>(instance, field);
    if (value != nullptr) {
      mark_modified(modified, ImGui::Checkbox(field.name, value));
    }
    break;
  }
  case core::TypeField::Kind::Vec2: {
    math::Vec2 *value = desc.field_ptr<math::Vec2>(instance, field);
    if (value != nullptr) {
      draw_vec2_field(field.name, *value, modified);
    }
    break;
  }
  case core::TypeField::Kind::Vec3: {
    math::Vec3 *value = desc.field_ptr<math::Vec3>(instance, field);
    if (value != nullptr) {
      draw_vec3_field(field.name, *value, modified);
    }
    break;
  }
  case core::TypeField::Kind::Vec4: {
    math::Vec4 *value = desc.field_ptr<math::Vec4>(instance, field);
    if (value != nullptr) {
      draw_vec4_field(field.name, *value, modified);
    }
    break;
  }
  case core::TypeField::Kind::Quat: {
    math::Quat *value = desc.field_ptr<math::Quat>(instance, field);
    if (value != nullptr) {
      draw_quat_field(field.name, *value, modified);
    }
    break;
  }
  }
}

bool draw_reflected_component(const char *typeName, void *instance) noexcept {
  if ((typeName == nullptr) || (instance == nullptr)) {
    return false;
  }

  const core::TypeDescriptor *desc =
      core::global_type_registry().find_type(typeName);
  if (desc == nullptr) {
    return false;
  }

  bool modified = false;

  for (std::size_t i = 0U; i < desc->fieldCount; ++i) {
    draw_field(*desc, instance, desc->fields[i], &modified);
  }

  return modified;
}

bool draw_remove_component_button(const char *id, bool editable) noexcept {
  if (!editable || (id == nullptr)) {
    return false;
  }

  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 20.0F);
  ImGui::PushID(id);
  const bool removePressed = ImGui::SmallButton("X");
  ImGui::PopID();
  return removePressed;
}

void draw_add_component_combo(runtime::Entity entity, bool editable) noexcept {
  if (!editable || (editor_session().world == nullptr)) {
    return;
  }

  ImGui::Separator();
  if (!ImGui::BeginCombo("##addcomp", "Add Component...")) {
    return;
  }

  const core::TypeRegistry &registry = core::global_type_registry();
  for (std::size_t i = 0U; i < registry.type_count(); ++i) {
    const core::TypeDescriptor *desc = registry.type_at(i);
    if ((desc == nullptr) || (desc->name == nullptr)) {
      continue;
    }

    if (std::strcmp(desc->name, kNameTypeName) == 0) {
      // NameComponent is intentionally managed via the dedicated + Name/- UI.
      continue;
    }

    if ((std::strcmp(desc->name, kTransformTypeName) == 0) &&
        (editor_session().world->get_transform_read_ptr(entity) == nullptr)) {
      if (ImGui::Selectable(kTransformSectionLabel)) {
        execute_component_add(
            entity, ComponentEditType::Transform,
            default_component_snapshot(entity, ComponentEditType::Transform));
      }
      continue;
    }

    if ((std::strcmp(desc->name, kRigidBodyTypeName) == 0) &&
        (editor_session().world->get_rigid_body_ptr(entity) == nullptr)) {
      if (ImGui::Selectable(kRigidBodySectionLabel)) {
        execute_component_add(
            entity, ComponentEditType::RigidBody,
            default_component_snapshot(entity, ComponentEditType::RigidBody));
      }
      continue;
    }

    if ((std::strcmp(desc->name, kColliderTypeName) == 0) &&
        (editor_session().world->get_collider_ptr(entity) == nullptr)) {
      if (ImGui::Selectable(kColliderSectionLabel)) {
        execute_component_add(
            entity, ComponentEditType::Collider,
            default_component_snapshot(entity, ComponentEditType::Collider));
      }
      continue;
    }

    if ((std::strcmp(desc->name, kReflectionProbeTypeName) == 0) &&
        (editor_session().world->get_reflection_probe_component_ptr(entity) ==
         nullptr)) {
      if (ImGui::Selectable(kReflectionProbeSectionLabel)) {
        execute_component_add(entity, ComponentEditType::ReflectionProbe,
                              default_component_snapshot(
                                  entity, ComponentEditType::ReflectionProbe));
      }
      continue;
    }

    if ((std::strcmp(desc->name, kFoliagePatchTypeName) == 0) &&
        !editor_session().world->has_foliage_patch_component(entity)) {
      if (ImGui::Selectable(kFoliagePatchSectionLabel)) {
        execute_component_add(entity, ComponentEditType::FoliagePatch,
                              default_component_snapshot(
                                  entity, ComponentEditType::FoliagePatch));
      }
      continue;
    }

    if ((std::strcmp(desc->name, kPointLightTypeName) == 0) &&
        !editor_session().world->has_point_light_component(entity)) {
      if (ImGui::Selectable(kPointLightSectionLabel)) {
        execute_component_add(
            entity, ComponentEditType::PointLight,
            default_component_snapshot(entity, ComponentEditType::PointLight));
      }
      continue;
    }

    if ((std::strcmp(desc->name, kSpotLightTypeName) == 0) &&
        !editor_session().world->has_spot_light_component(entity)) {
      if (ImGui::Selectable(kSpotLightSectionLabel)) {
        execute_component_add(
            entity, ComponentEditType::SpotLight,
            default_component_snapshot(entity, ComponentEditType::SpotLight));
      }
      continue;
    }

    if ((std::strcmp(desc->name, kSpringArmTypeName) == 0) &&
        !editor_session().world->has_spring_arm(entity)) {
      if (ImGui::Selectable(kSpringArmSectionLabel)) {
        execute_component_add(
            entity, ComponentEditType::SpringArm,
            default_component_snapshot(entity, ComponentEditType::SpringArm));
      }
      continue;
    }

    if ((std::strcmp(desc->name, kSceneCaptureTypeName) == 0) &&
        !editor_session().world->has_scene_capture_component(entity)) {
      if (ImGui::Selectable(kSceneCaptureSectionLabel)) {
        execute_component_add(entity, ComponentEditType::SceneCapture,
                              default_component_snapshot(
                                  entity, ComponentEditType::SceneCapture));
      }
      continue;
    }
  }

  if (editor_session().world->get_mesh_component_ptr(entity) == nullptr) {
    if (ImGui::Selectable(kMeshSectionLabel)) {
      execute_component_add(
          entity, ComponentEditType::Mesh,
          default_component_snapshot(entity, ComponentEditType::Mesh));
    }
  }

  {
    runtime::LightComponent tmpLight{};
    if (!editor_session().world->get_light_component(entity, &tmpLight)) {
      if (ImGui::Selectable(kLightSectionLabel)) {
        execute_component_add(
            entity, ComponentEditType::Light,
            default_component_snapshot(entity, ComponentEditType::Light));
      }
    }
  }

  {
    runtime::ScriptComponent tmpScript{};
    if (!editor_session().world->get_script_component(entity, &tmpScript)) {
      if (ImGui::Selectable(kScriptSectionLabel)) {
        execute_component_add(
            entity, ComponentEditType::Script,
            default_component_snapshot(entity, ComponentEditType::Script));
      }
    }
  }

  {
    runtime::AnimationComponent tmpAnimation{};
    if (!editor_session().world->get_animation_component(entity,
                                                         &tmpAnimation)) {
      if (ImGui::Selectable(kAnimationSectionLabel)) {
        execute_component_add(
            entity, ComponentEditType::Animation,
            default_component_snapshot(entity, ComponentEditType::Animation));
      }
    }
  }

  ImGui::EndCombo();
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
    std::snprintf(label, sizeof(label), "LOD %zu Mesh ID", lod);
    mark_modified(modified, ImGui::InputScalar(label, ImGuiDataType_U64,
                                               &foliage.meshAssetIds[lod],
                                               nullptr, nullptr, "%llu",
                                               ImGuiInputTextFlags_None));
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
      draw_vec3_field("Offset", instance.offset, modified);
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

} // namespace

void draw_inspector_panel() noexcept {
  if (!ImGui::Begin("Inspector")) {
    ImGui::End();
    return;
  }

  if (editor_session().worldRestoreFailed) {
    ImGui::TextColored(ImVec4(1.0F, 0.5F, 0.2F, 1.0F),
                       "Scene restore failed on Stop.");
    ImGui::TextUnformatted("Use File -> Load Scene to recover.");
    ImGui::Separator();
  }

  const runtime::Entity entity = selected_entity();
  if ((editor_session().world == nullptr) ||
      (entity == runtime::kInvalidEntity)) {
    inspector_commit_pending_edit();
    ImGui::TextUnformatted("No entity selected");
    ImGui::End();
    return;
  }

  const bool editable = world_is_editable();
  runtime::NameComponent nameComponent{};
  const bool hasNameComponent =
      editor_session().world->get_name_component(entity, &nameComponent);
  if (hasNameComponent) {
    // The name is entity identity, not a removable behavior — it can be
    // edited but never deleted from the inspector.
    const runtime::NameComponent nameBefore = nameComponent;
    bool nameChanged = false;
    if (!editable) {
      ImGui::BeginDisabled();
    }

    nameChanged = ImGui::InputText("Name", nameComponent.name,
                                   sizeof(nameComponent.name));

    if (!editable) {
      ImGui::EndDisabled();
    }

    if (editable && nameChanged) {
      ComponentEditSnapshot before{};
      before.name = nameBefore;
      ComponentEditSnapshot after{};
      after.name = nameComponent;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::Name, before, after));
    }
  } else {
    if (!editable) {
      ImGui::BeginDisabled();
    }

    if (ImGui::SmallButton("+ Name") && editable) {
      execute_component_add(
          entity, ComponentEditType::Name,
          default_component_snapshot(entity, ComponentEditType::Name));
    }

    if (!editable) {
      ImGui::EndDisabled();
    }
  }

  ImGui::Separator();

  if (!editable) {
    ImGui::BeginDisabled();
  }

  const bool deletePressed = ImGui::Button("Delete Entity");

  if (!editable) {
    ImGui::EndDisabled();
  }

  if (editable && deletePressed) {
    static_cast<void>(execute_entity_delete(entity));
    clear_entity_selection();
    ImGui::End();
    return;
  }

  ImGui::SameLine();
  ImGui::Text("Entity [%u] gen=%u", entity.index, entity.generation);
  ImGui::Separator();

  runtime::Transform transform{};
  if (editor_session().world->get_transform(entity, &transform)) {
    // The transform is entity identity (position/axis) — editable in place
    // but never removable, matching the empty-object mental model.
    const runtime::Transform transformBefore = transform;
    ImGui::PushID("TransformSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kTransformSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);

    bool transformModified = false;
    if (sectionOpen) {
      if (editable) {
        transformModified =
            draw_reflected_component(kTransformTypeName, &transform);
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(
            draw_reflected_component(kTransformTypeName, &transform));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && transformModified) {
      ComponentEditSnapshot before{};
      before.transform = transformBefore;
      ComponentEditSnapshot after{};
      after.transform = transform;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::Transform, before, after));
    }
  } else {
    ImGui::TextUnformatted("Transform: <none>");
  }

  runtime::RigidBody rigidBody{};
  if (editor_session().world->get_rigid_body(entity, &rigidBody)) {
    const runtime::RigidBody rigidBodyBefore = rigidBody;
    ImGui::PushID("RigidBodySection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kRigidBodySectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool rigidBodyModified = false;
    if (sectionOpen) {
      if (editable) {
        rigidBodyModified =
            draw_reflected_component(kRigidBodyTypeName, &rigidBody);
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(
            draw_reflected_component(kRigidBodyTypeName, &rigidBody));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::RigidBody);
    } else if (editable && rigidBodyModified) {
      ComponentEditSnapshot before{};
      before.rigidBody = rigidBodyBefore;
      ComponentEditSnapshot after{};
      after.rigidBody = rigidBody;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::RigidBody, before, after));
    }
  } else {
    ImGui::TextUnformatted("RigidBody: <none>");
  }

  runtime::Collider collider{};
  if (editor_session().world->get_collider(entity, &collider)) {
    const runtime::Collider colliderBefore = collider;
    ImGui::PushID("ColliderSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kColliderSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool colliderModified = false;
    if (sectionOpen) {
      if (!editable) {
        ImGui::BeginDisabled();
      }

      // Only the analytically authorable shapes are selectable: a convex
      // hull needs a provenance payload only primitive spawns carry, and
      // heightfield payloads are not editor-authorable at all, so
      // switching into either would create a payload-less collider.
      constexpr const char *kColliderShapeNames[] = {
          "Box", "Sphere", "Capsule", "Convex Hull", "Heightfield"};
      constexpr int kAuthorableShapeCount = 3;
      int shapeIndex = static_cast<int>(collider.shape);
      if ((shapeIndex < 0) || (shapeIndex >= 5)) {
        shapeIndex = 0;
      }
      if (ImGui::BeginCombo("Shape", kColliderShapeNames[shapeIndex])) {
        for (int i = 0; i < kAuthorableShapeCount; ++i) {
          if (ImGui::Selectable(kColliderShapeNames[i], shapeIndex == i) &&
              (shapeIndex != i)) {
            collider.shape = static_cast<runtime::ColliderShape>(i);
            colliderModified = true;
          }
        }
        ImGui::EndCombo();
      }
      colliderModified =
          draw_reflected_component(kColliderTypeName, &collider) ||
          colliderModified;

      if (!editable) {
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::Collider);
    } else if (editable && colliderModified) {
      ComponentEditSnapshot before{};
      before.collider = colliderBefore;
      ComponentEditSnapshot after{};
      after.collider = collider;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::Collider, before, after));
    }
  } else {
    ImGui::TextUnformatted("Collider: <none>");
  }

  runtime::LightComponent light{};
  if (editor_session().world->get_light_component(entity, &light)) {
    const runtime::LightComponent lightBefore = light;
    ImGui::PushID("LightComponentSection");
    const bool lightOpen = ImGui::CollapsingHeader(
        kLightSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removeLightPressed =
        draw_remove_component_button("remove", editable);

    bool lightModified = false;
    if (lightOpen) {
      if (!editable) {
        ImGui::BeginDisabled();
      }

      constexpr const char *kLightTypeNames[] = {"Directional", "Point"};
      int currentType = static_cast<int>(light.type);
      if (ImGui::Combo("Type", &currentType, kLightTypeNames, 2)) {
        light.type = static_cast<runtime::LightType>(currentType);
        lightModified = true;
      }

      lightModified |= ImGui::ColorEdit3("Color", &light.color.x);
      lightModified |= ImGui::DragFloat("Intensity", &light.intensity, 0.05F,
                                        0.0F, 100.0F, "%.2f");
      draw_vec3_field("Direction", light.direction, &lightModified);

      if (!editable) {
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removeLightPressed) {
      execute_component_remove(entity, ComponentEditType::Light);
    } else if (editable && lightModified) {
      ComponentEditSnapshot before{};
      before.light = lightBefore;
      ComponentEditSnapshot after{};
      after.light = light;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::Light, before, after));
    }
  } else {
    ImGui::TextUnformatted("LightComponent: <none>");
  }

  runtime::PointLightComponent pointLight{};
  if (editor_session().world->get_point_light_component(entity, &pointLight)) {
    const runtime::PointLightComponent pointLightBefore = pointLight;
    ImGui::PushID("PointLightComponentSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kPointLightSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool pointLightModified = false;
    if (sectionOpen) {
      if (editable) {
        pointLightModified =
            draw_reflected_component(kPointLightTypeName, &pointLight);
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(
            draw_reflected_component(kPointLightTypeName, &pointLight));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::PointLight);
    } else if (editable && pointLightModified) {
      ComponentEditSnapshot before{};
      before.pointLight = pointLightBefore;
      ComponentEditSnapshot after{};
      after.pointLight = pointLight;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::PointLight, before, after));
    }
  } else {
    ImGui::TextUnformatted("PointLightComponent: <none>");
  }

  runtime::SpotLightComponent spotLight{};
  if (editor_session().world->get_spot_light_component(entity, &spotLight)) {
    const runtime::SpotLightComponent spotLightBefore = spotLight;
    ImGui::PushID("SpotLightComponentSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kSpotLightSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool spotLightModified = false;
    if (sectionOpen) {
      if (editable) {
        spotLightModified =
            draw_reflected_component(kSpotLightTypeName, &spotLight);
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(
            draw_reflected_component(kSpotLightTypeName, &spotLight));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::SpotLight);
    } else if (editable && spotLightModified) {
      ComponentEditSnapshot before{};
      before.spotLight = spotLightBefore;
      ComponentEditSnapshot after{};
      after.spotLight = spotLight;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::SpotLight, before, after));
    }
  } else {
    ImGui::TextUnformatted("SpotLightComponent: <none>");
  }

  runtime::MeshComponent mesh{};
  if (editor_session().world->get_mesh_component(entity, &mesh)) {
    const runtime::MeshComponent meshBefore = mesh;
    ImGui::PushID("MeshComponentSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kMeshSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool meshModified = false;
    if (sectionOpen) {
      ImGui::Text("Mesh Asset ID: %llu",
                  static_cast<unsigned long long>(mesh.meshAssetId));
      ImGui::Text("Material Asset ID: %llu",
                  static_cast<unsigned long long>(mesh.materialAssetId));
      // Persistent id of an enabled SceneCaptureComponent entity whose
      // rendered output feeds this mesh's albedo texture (0 = none).
      int captureSourceId = static_cast<int>(mesh.sceneCaptureSourceId);
      // Material path entry converts to the path-derived id on Enter
      // (e.g. assets/materials/rock.json); empty clears the reference. The
      // buffer is keyed to the inspected entity so text typed for one
      // selection can never be committed against another.
      static char materialPathBuffer[196] = {};
      static runtime::PersistentId materialPathOwner =
          runtime::kInvalidPersistentId;
      const runtime::PersistentId inspectedId =
          editor_session().world->persistent_id(entity);
      if (materialPathOwner != inspectedId) {
        materialPathOwner = inspectedId;
        materialPathBuffer[0] = '\0';
      }
      if (editable) {
        if (ImGui::InputText("Material Path", materialPathBuffer,
                             sizeof(materialPathBuffer),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
          mesh.materialAssetId =
              (materialPathBuffer[0] != '\0')
                  ? renderer::make_asset_id_from_path(materialPathBuffer)
                  : 0ULL;
          meshModified = true;
        }
        meshModified |= ImGui::ColorEdit3("Albedo", &mesh.albedo.x);
        meshModified |= ImGui::SliderFloat("Roughness", &mesh.roughness, 0.0F,
                                           1.0F, "%.2f");
        meshModified |=
            ImGui::SliderFloat("Metallic", &mesh.metallic, 0.0F, 1.0F, "%.2f");
        meshModified |=
            ImGui::SliderFloat("Opacity", &mesh.opacity, 0.0F, 1.0F, "%.2f");
        if (ImGui::InputInt("Capture Source ID", &captureSourceId)) {
          mesh.sceneCaptureSourceId =
              (captureSourceId > 0)
                  ? static_cast<std::uint32_t>(captureSourceId)
                  : 0U;
          meshModified = true;
        }
        // Blockout material presets: named albedo/roughness/metallic/
        // opacity sets applied as one undoable edit through the history
        // (the caller's direct apply is suppressed for that frame).
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
              execute_component_add(entity, ComponentEditType::Mesh, after);
              meshModified = false;
            }
          }
          ImGui::EndCombo();
        }
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(ImGui::ColorEdit3("Albedo", &mesh.albedo.x));
        static_cast<void>(ImGui::SliderFloat("Roughness", &mesh.roughness, 0.0F,
                                             1.0F, "%.2f"));
        static_cast<void>(
            ImGui::SliderFloat("Metallic", &mesh.metallic, 0.0F, 1.0F, "%.2f"));
        static_cast<void>(
            ImGui::SliderFloat("Opacity", &mesh.opacity, 0.0F, 1.0F, "%.2f"));
        static_cast<void>(
            ImGui::InputInt("Capture Source ID", &captureSourceId));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::Mesh);
    } else if (editable && meshModified) {
      ComponentEditSnapshot before{};
      before.mesh = meshBefore;
      ComponentEditSnapshot after{};
      after.mesh = mesh;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::Mesh, before, after));
    }
  } else {
    ImGui::TextUnformatted("MeshComponent: <none>");
  }

  runtime::FoliagePatchComponent foliagePatch{};
  if (editor_session().world->get_foliage_patch_component(entity,
                                                          &foliagePatch)) {
    const runtime::FoliagePatchComponent foliagePatchBefore = foliagePatch;
    ImGui::PushID("FoliagePatchComponentSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kFoliagePatchSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool foliageModified = false;
    if (sectionOpen) {
      draw_foliage_patch_fields(entity, foliagePatch, editable,
                                &foliageModified);
    }
    ImGui::PopID();

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::FoliagePatch);
    } else if (editable && foliageModified) {
      ComponentEditSnapshot before{};
      before.foliagePatch = foliagePatchBefore;
      ComponentEditSnapshot after{};
      after.foliagePatch = foliagePatch;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::FoliagePatch, before, after));
    }
  } else {
    ImGui::TextUnformatted("FoliagePatchComponent: <none>");
  }

  runtime::ReflectionProbeComponent reflectionProbe{};
  if (editor_session().world->get_reflection_probe_component(
          entity, &reflectionProbe)) {
    const runtime::ReflectionProbeComponent reflectionProbeBefore =
        reflectionProbe;
    ImGui::PushID("ReflectionProbeComponentSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kReflectionProbeSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool probeModified = false;
    if (sectionOpen) {
      if (editable) {
        probeModified = draw_reflected_component(kReflectionProbeTypeName,
                                                 &reflectionProbe);
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(draw_reflected_component(kReflectionProbeTypeName,
                                                   &reflectionProbe));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::ReflectionProbe);
    } else if (editable && probeModified) {
      ComponentEditSnapshot before{};
      before.reflectionProbe = reflectionProbeBefore;
      ComponentEditSnapshot after{};
      after.reflectionProbe = reflectionProbe;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::ReflectionProbe, before, after));
    }
  } else {
    ImGui::TextUnformatted("ReflectionProbeComponent: <none>");
  }

  runtime::SceneCaptureComponent sceneCapture{};
  if (editor_session().world->get_scene_capture_component(entity,
                                                          &sceneCapture)) {
    const runtime::SceneCaptureComponent sceneCaptureBefore = sceneCapture;
    ImGui::PushID("SceneCaptureComponentSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kSceneCaptureSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool captureModified = false;
    if (sectionOpen) {
      if (editable) {
        captureModified =
            draw_reflected_component(kSceneCaptureTypeName, &sceneCapture);
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(
            draw_reflected_component(kSceneCaptureTypeName, &sceneCapture));
        ImGui::EndDisabled();
      }

      const runtime::PersistentId capturePersistentId =
          editor_session().world->persistent_id(entity);
      ImGui::Text("Capture Source ID: %u",
                  static_cast<unsigned>(capturePersistentId));

      // Live preview of the capture output, fitted to the panel width
      // (flipped V to match the GL framebuffer origin, like the viewport).
      const std::int32_t captureSlot =
          editor_session().world->scene_capture_slot_for_entity(entity);
      const std::uint32_t captureTexture =
          (captureSlot >= 0) ? renderer::get_scene_capture_texture(
                                   static_cast<std::size_t>(captureSlot))
                             : 0U;
      if ((captureTexture != 0U) && (sceneCapture.width > 0U)) {
        const float availWidth = ImGui::GetContentRegionAvail().x;
        const float aspect = static_cast<float>(sceneCapture.height) /
                             static_cast<float>(sceneCapture.width);
        const ImVec2 previewSize(availWidth, availWidth * aspect);
        ImGui::Image(static_cast<ImTextureID>(captureTexture), previewSize,
                     ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));
      } else {
        ImGui::TextUnformatted(sceneCapture.enabled
                                   ? "Preview: waiting for first frame"
                                   : "Preview: capture disabled");
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::SceneCapture);
    } else if (editable && captureModified) {
      ComponentEditSnapshot before{};
      before.sceneCapture = sceneCaptureBefore;
      ComponentEditSnapshot after{};
      after.sceneCapture = sceneCapture;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::SceneCapture, before, after));
    }
  } else {
    ImGui::TextUnformatted("SceneCaptureComponent: <none>");
  }

  runtime::ScriptComponent script{};
  if (editor_session().world->get_script_component(entity, &script)) {
    const runtime::ScriptComponent scriptBefore = script;
    ImGui::PushID("ScriptComponentSection");
    const bool scriptOpen = ImGui::CollapsingHeader(
        kScriptSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removeScriptPressed =
        draw_remove_component_button("remove", editable);

    bool scriptModified = false;
    if (scriptOpen) {
      if (!editable) {
        ImGui::BeginDisabled();
      }

      char pathBuf[sizeof(script.scriptPath)] = {};
      std::memcpy(pathBuf, script.scriptPath, sizeof(pathBuf));
      if (ImGui::InputText("Script Path", pathBuf, sizeof(pathBuf))) {
        std::memcpy(script.scriptPath, pathBuf, sizeof(pathBuf));
        scriptModified = true;
      }

      if (!editable) {
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removeScriptPressed) {
      execute_component_remove(entity, ComponentEditType::Script);
    } else if (editable && scriptModified) {
      ComponentEditSnapshot before{};
      before.script = scriptBefore;
      ComponentEditSnapshot after{};
      after.script = script;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::Script, before, after));
    }
  } else {
    ImGui::TextUnformatted("ScriptComponent: <none>");
  }

  runtime::AnimationComponent animation{};
  if (editor_session().world->get_animation_component(entity, &animation)) {
    const runtime::AnimationComponent animationBefore = animation;
    ImGui::PushID("AnimationComponentSection");
    const bool animationOpen = ImGui::CollapsingHeader(
        kAnimationSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removeAnimationPressed =
        draw_remove_component_button("remove", editable);

    bool animationModified = false;
    if (animationOpen) {
      if (!editable) {
        ImGui::BeginDisabled();
      }

      char controllerBuf[sizeof(animation.controllerPath)] = {};
      std::memcpy(controllerBuf, animation.controllerPath,
                  sizeof(controllerBuf));
      if (ImGui::InputText("Controller Path", controllerBuf,
                           sizeof(controllerBuf))) {
        std::memcpy(animation.controllerPath, controllerBuf,
                    sizeof(controllerBuf));
        animationModified = true;
      }
      if (ImGui::Checkbox("Playing", &animation.playing)) {
        animationModified = true;
      }
      if (ImGui::DragFloat("Playback Speed", &animation.playbackSpeed, 0.01F,
                           0.0F, 8.0F)) {
        animationModified = true;
      }

      if (!editable) {
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removeAnimationPressed) {
      execute_component_remove(entity, ComponentEditType::Animation);
    } else if (editable && animationModified) {
      ComponentEditSnapshot before{};
      before.animation = animationBefore;
      ComponentEditSnapshot after{};
      after.animation = animation;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::Animation, before, after));
    }
  } else {
    ImGui::TextUnformatted("AnimationComponent: <none>");
  }

  runtime::SpringArmComponent springArm{};
  if (editor_session().world->get_spring_arm(entity, &springArm)) {
    const runtime::SpringArmComponent springArmBefore = springArm;
    ImGui::PushID("SpringArmComponentSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kSpringArmSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool springArmModified = false;
    if (sectionOpen) {
      if (editable) {
        springArmModified =
            draw_reflected_component(kSpringArmTypeName, &springArm);
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(
            draw_reflected_component(kSpringArmTypeName, &springArm));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::SpringArm);
    } else if (editable && springArmModified) {
      ComponentEditSnapshot before{};
      before.springArm = springArmBefore;
      ComponentEditSnapshot after{};
      after.springArm = springArm;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::SpringArm, before, after));
    }
  } else {
    ImGui::TextUnformatted("SpringArmComponent: <none>");
  }

  draw_add_component_combo(entity, editable);

  if (inspector_has_pending_edit() && !ImGui::IsAnyItemActive()) {
    inspector_commit_pending_edit();
  }

  ImGui::End();
}

} // namespace engine::editor
