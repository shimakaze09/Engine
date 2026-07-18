// Implements the editor inspector panel and its reflected field editors.
// Split out of editor.cpp (REVIEW_FINDINGS A3).

#include "editor_panels_inspector.h"

#include "editor_commands.h"
#include "editor_session.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#if __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL2 headers not found"
#endif

#if __has_include(<SDL_opengl.h>)
#include <SDL_opengl.h>
#elif __has_include(<SDL2/SDL_opengl.h>)
#include <SDL2/SDL_opengl.h>
#else
#error "SDL OpenGL headers not found"
#endif

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
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
#include "engine/engine.h"
#include "engine/editor/editor_camera.h"
#include "engine/math/transform.h"
#include "engine/math/vec2.h"
#include "engine/math/vec4.h"
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
        (editor_session().world->get_reflection_probe_component_ptr(entity) == nullptr)) {
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
        execute_component_add(
            entity, ComponentEditType::FoliagePatch,
            default_component_snapshot(entity, ComponentEditType::FoliagePatch));
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

  ImGui::EndCombo();
}

void draw_foliage_patch_fields(runtime::FoliagePatchComponent &foliage,
                               bool editable, bool *modified) noexcept {
  if (!editable) {
    ImGui::BeginDisabled();
  }

  for (std::size_t lod = 0U; lod < runtime::FoliagePatchComponent::kMaxLods;
       ++lod) {
    char label[32] = {};
    std::snprintf(label, sizeof(label), "LOD %zu Mesh ID", lod);
    mark_modified(
        modified,
        ImGui::InputScalar(label, ImGuiDataType_U64,
                           &foliage.meshAssetIds[lod], nullptr, nullptr,
                           "%llu", ImGuiInputTextFlags_None));
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
                ImGui::DragFloat("Wind Strength", &foliage.windStrength,
                                  0.01F, 0.0F, 5.0F, "%.2f"));
  mark_modified(modified,
                ImGui::DragFloat("Wind Frequency", &foliage.windFrequency,
                                  0.05F, 0.0F, 20.0F, "%.2f"));

  if (ImGui::TreeNode("Instances")) {
    std::uint32_t visibleCount = foliage.instanceCount;
    if (visibleCount >
        static_cast<std::uint32_t>(runtime::FoliagePatchComponent::kMaxInstances)) {
      visibleCount =
          static_cast<std::uint32_t>(runtime::FoliagePatchComponent::kMaxInstances);
    }
    if (visibleCount > 16U) {
      visibleCount = 16U;
    }

    for (std::uint32_t i = 0U; i < visibleCount; ++i) {
      runtime::FoliageInstance &instance = foliage.instances[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::Separator();
      ImGui::Text("Instance %u", i);
      draw_vec3_field("Offset", instance.offset, modified);
      mark_modified(modified, ImGui::DragFloat("Scale", &instance.scale,
                                               0.01F, 0.05F, 10.0F, "%.2f"));
      mark_modified(modified, ImGui::DragFloat("Phase", &instance.phase,
                                               0.05F, -100.0F, 100.0F,
                                               "%.2f"));
      int lodIndex = static_cast<int>(instance.lodIndex);
      if (ImGui::SliderInt(
              "LOD Index", &lodIndex, 0,
              static_cast<int>(runtime::FoliagePatchComponent::kMaxLods - 1U))) {
        instance.lodIndex = static_cast<std::uint32_t>(lodIndex);
        mark_modified(modified, true);
      }
      ImGui::PopID();
    }

    if (foliage.instanceCount > visibleCount) {
      ImGui::Text("%u more instances stored",
                  foliage.instanceCount - visibleCount);
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

  if ((editor_session().world == nullptr) || (editor_session().selectedEntityIndex == 0U)) {
    ImGui::TextUnformatted("No entity selected");
    ImGui::End();
    return;
  }

  const runtime::Entity entity =
      editor_session().world->find_entity_by_index(editor_session().selectedEntityIndex);
  if (entity == runtime::kInvalidEntity) {
    ImGui::TextUnformatted("Selected entity is no longer alive");
    editor_session().selectedEntityIndex = 0U;
    ImGui::End();
    return;
  }

  const bool editable = world_is_editable();
  runtime::NameComponent nameComponent{};
  const bool hasNameComponent =
      editor_session().world->get_name_component(entity, &nameComponent);
  if (hasNameComponent) {
    bool nameChanged = false;
    bool removeNamePressed = false;
    if (!editable) {
      ImGui::BeginDisabled();
    }

    nameChanged = ImGui::InputText("Name", nameComponent.name,
                                   sizeof(nameComponent.name));
    ImGui::SameLine();
    removeNamePressed = ImGui::SmallButton("-");

    if (!editable) {
      ImGui::EndDisabled();
    }

    if (editable && removeNamePressed) {
      execute_component_remove(entity, ComponentEditType::Name);
    } else if (editable && nameChanged) {
      static_cast<void>(editor_session().world->add_name_component(entity, nameComponent));
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
    static_cast<void>(editor_session().world->destroy_entity(entity));
    editor_session().selectedEntityIndex = 0U;
    ImGui::End();
    return;
  }

  ImGui::SameLine();
  ImGui::Text("Entity [%u] gen=%u", entity.index, entity.generation);
  ImGui::Separator();

  runtime::Transform transform{};
  if (editor_session().world->get_transform(entity, &transform)) {
    ImGui::PushID("TransformSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kTransformSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

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

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::Transform);
    } else if (editable && transformModified) {
      static_cast<void>(editor_session().world->add_transform(entity, transform));
    }
  } else {
    ImGui::TextUnformatted("Transform: <none>");
  }

  runtime::RigidBody rigidBody{};
  if (editor_session().world->get_rigid_body(entity, &rigidBody)) {
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
      static_cast<void>(editor_session().world->add_rigid_body(entity, rigidBody));
    }
  } else {
    ImGui::TextUnformatted("RigidBody: <none>");
  }

  runtime::Collider collider{};
  if (editor_session().world->get_collider(entity, &collider)) {
    ImGui::PushID("ColliderSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kColliderSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool colliderModified = false;
    if (sectionOpen) {
      if (editable) {
        colliderModified =
            draw_reflected_component(kColliderTypeName, &collider);
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(
            draw_reflected_component(kColliderTypeName, &collider));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::Collider);
    } else if (editable && colliderModified) {
      static_cast<void>(editor_session().world->add_collider(entity, collider));
    }
  } else {
    ImGui::TextUnformatted("Collider: <none>");
  }

  runtime::LightComponent light{};
  if (editor_session().world->get_light_component(entity, &light)) {
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
      static_cast<void>(editor_session().world->add_light_component(entity, light));
    }
  } else {
    ImGui::TextUnformatted("LightComponent: <none>");
  }

  runtime::PointLightComponent pointLight{};
  if (editor_session().world->get_point_light_component(entity, &pointLight)) {
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
      static_cast<void>(
          editor_session().world->add_point_light_component(entity, pointLight));
    }
  } else {
    ImGui::TextUnformatted("PointLightComponent: <none>");
  }

  runtime::SpotLightComponent spotLight{};
  if (editor_session().world->get_spot_light_component(entity, &spotLight)) {
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
      static_cast<void>(editor_session().world->add_spot_light_component(entity, spotLight));
    }
  } else {
    ImGui::TextUnformatted("SpotLightComponent: <none>");
  }

  runtime::MeshComponent mesh{};
  if (editor_session().world->get_mesh_component(entity, &mesh)) {
    ImGui::PushID("MeshComponentSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kMeshSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool meshModified = false;
    if (sectionOpen) {
      ImGui::Text("Mesh Asset ID: %llu",
                  static_cast<unsigned long long>(mesh.meshAssetId));
      if (editable) {
        meshModified |= ImGui::ColorEdit3("Albedo", &mesh.albedo.x);
        meshModified |= ImGui::SliderFloat("Roughness", &mesh.roughness, 0.0F,
                                           1.0F, "%.2f");
        meshModified |=
            ImGui::SliderFloat("Metallic", &mesh.metallic, 0.0F, 1.0F, "%.2f");
        meshModified |=
            ImGui::SliderFloat("Opacity", &mesh.opacity, 0.0F, 1.0F, "%.2f");
      } else {
        ImGui::BeginDisabled();
        static_cast<void>(ImGui::ColorEdit3("Albedo", &mesh.albedo.x));
        static_cast<void>(ImGui::SliderFloat("Roughness", &mesh.roughness, 0.0F,
                                             1.0F, "%.2f"));
        static_cast<void>(
            ImGui::SliderFloat("Metallic", &mesh.metallic, 0.0F, 1.0F, "%.2f"));
        static_cast<void>(
            ImGui::SliderFloat("Opacity", &mesh.opacity, 0.0F, 1.0F, "%.2f"));
        ImGui::EndDisabled();
      }
    }
    ImGui::PopID();

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::Mesh);
    } else if (editable && meshModified) {
      static_cast<void>(editor_session().world->add_mesh_component(entity, mesh));
    }
  } else {
    ImGui::TextUnformatted("MeshComponent: <none>");
  }

  runtime::FoliagePatchComponent foliagePatch{};
  if (editor_session().world->get_foliage_patch_component(entity, &foliagePatch)) {
    ImGui::PushID("FoliagePatchComponentSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kFoliagePatchSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool foliageModified = false;
    if (sectionOpen) {
      draw_foliage_patch_fields(foliagePatch, editable, &foliageModified);
    }
    ImGui::PopID();

    if (editable && removePressed) {
      execute_component_remove(entity, ComponentEditType::FoliagePatch);
    } else if (editable && foliageModified) {
      static_cast<void>(
          editor_session().world->add_foliage_patch_component(entity, foliagePatch));
    }
  } else {
    ImGui::TextUnformatted("FoliagePatchComponent: <none>");
  }

  runtime::ReflectionProbeComponent reflectionProbe{};
  if (editor_session().world->get_reflection_probe_component(entity, &reflectionProbe)) {
    ImGui::PushID("ReflectionProbeComponentSection");
    const bool sectionOpen = ImGui::CollapsingHeader(
        kReflectionProbeSectionLabel, ImGuiTreeNodeFlags_DefaultOpen);
    const bool removePressed = draw_remove_component_button("remove", editable);

    bool probeModified = false;
    if (sectionOpen) {
      if (editable) {
        probeModified =
            draw_reflected_component(kReflectionProbeTypeName,
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
      static_cast<void>(
          editor_session().world->add_reflection_probe_component(entity, reflectionProbe));
    }
  } else {
    ImGui::TextUnformatted("ReflectionProbeComponent: <none>");
  }

  runtime::ScriptComponent script{};
  if (editor_session().world->get_script_component(entity, &script)) {
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

      // InputText needs a mutable buffer — copy into one.
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
      static_cast<void>(editor_session().world->add_script_component(entity, script));
    }
  } else {
    ImGui::TextUnformatted("ScriptComponent: <none>");
  }

  runtime::SpringArmComponent springArm{};
  if (editor_session().world->get_spring_arm(entity, &springArm)) {
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
      static_cast<void>(editor_session().world->add_spring_arm(entity, springArm));
    }
  } else {
    ImGui::TextUnformatted("SpringArmComponent: <none>");
  }

  draw_add_component_combo(entity, editable);

  ImGui::End();
}


} // namespace engine::editor
