// Implements the editor inspector panel: entity identity (Name/Transform),
// a metadata-driven section per persistent component (generic reflected
// fields or a component's custom drawer), and the registry-generated Add
// Component menu. Split out of editor.cpp (REVIEW_FINDINGS A3); the
// per-component field/dispatch code moved out to
// editor_panels_inspector_generic.cpp/editor_panels_inspector_custom.cpp
// (issue #156) to keep this TU an orchestrator, not a growing branch list.

#include "editor_panels_inspector.h"

#include "editor_commands.h"
#include "editor_inspector_metadata.h"
#include "editor_panels_inspector_custom.h"
#include "editor_panels_inspector_generic.h"
#include "editor_session.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include <cstddef>

namespace engine::editor {

namespace {

/// True while the Inspector's progressive-disclosure Advanced/Debug toggle
/// is on: raw ids/paths and metadata-marked advanced fields become visible
/// (issue #156 acceptance: "raw IDs/paths remain inspectable in an
/// Advanced/Debug view"). Process-wide like the rest of the panel's static
/// UI state (g_pendingInspectorEdit in editor_commands.cpp).
bool g_showAdvanced = false;

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

/// Shared capture -> draw -> stage/remove boilerplate for one component
/// section; `drawFn` is `bool(Component&)`, called with the field editor
/// disabled while the world is not editable so the section still renders
/// (greyed out) instead of disappearing.
template <typename Component, typename DrawFn>
void draw_component_section(runtime::Entity entity, ComponentEditType type,
                            const char *sectionLabel,
                            Component ComponentEditSnapshot::*member,
                            bool editable, bool removable,
                            DrawFn &&drawFn) noexcept {
  ComponentEditSnapshot snapshot{};
  if (!capture_component_snapshot(type, entity, &snapshot)) {
    ImGui::Text("%s: <none>", sectionLabel);
    return;
  }
  const Component before = snapshot.*member;

  ImGui::PushID(sectionLabel);
  const bool open = ImGui::CollapsingHeader(sectionLabel,
                                            ImGuiTreeNodeFlags_DefaultOpen);
  const bool removePressed =
      removable && draw_remove_component_button("remove", editable);

  bool modified = false;
  if (open) {
    if (!editable) {
      ImGui::BeginDisabled();
    }
    modified = drawFn(snapshot.*member);
    if (!editable) {
      ImGui::EndDisabled();
    }
  }
  ImGui::PopID();

  if (editable && removePressed) {
    execute_component_remove(entity, type);
  } else if (editable && modified) {
    ComponentEditSnapshot beforeSnapshot{};
    beforeSnapshot.*member = before;
    ComponentEditSnapshot afterSnapshot{};
    afterSnapshot.*member = snapshot.*member;
    static_cast<void>(inspector_stage_component_edit(
        entity, type, beforeSnapshot, afterSnapshot));
  }
}

/// Renders every non-identity persistent-component section. One row per
/// registry entry except Name and Transform (drawn separately above as
/// entity identity); the count assert below fails the build if a new
/// registry row is not also added here, matching the ledger the ADD
/// COMPONENT menu already keeps generically.
void draw_component_sections(runtime::Entity entity, bool editable) noexcept {
  draw_component_section(
      entity, ComponentEditType::RigidBody, "Rigid Body",
      &ComponentEditSnapshot::rigidBody, editable, true,
      [](runtime::RigidBody &c) {
        return draw_reflected_component_fields("engine::runtime::RigidBody",
                                               &c, g_showAdvanced);
      });

  draw_component_section(
      entity, ComponentEditType::Collider, "Collider",
      &ComponentEditSnapshot::collider, editable, true,
      [](runtime::Collider &c) {
        // Only the analytically authorable shapes are selectable: a convex
        // hull needs a provenance payload only primitive spawns carry, and
        // heightfield payloads are not editor-authorable at all, so
        // switching into either would create a payload-less collider.
        constexpr const char *kColliderShapeNames[] = {
            "Box", "Sphere", "Capsule", "Convex Hull", "Heightfield"};
        constexpr int kAuthorableShapeCount = 3;
        int shapeIndex = static_cast<int>(c.shape);
        bool modified = false;
        if ((shapeIndex < 0) || (shapeIndex >= 5)) {
          shapeIndex = 0;
        }
        if (ImGui::BeginCombo("Shape", kColliderShapeNames[shapeIndex])) {
          for (int i = 0; i < kAuthorableShapeCount; ++i) {
            if (ImGui::Selectable(kColliderShapeNames[i], shapeIndex == i) &&
                (shapeIndex != i)) {
              c.shape = static_cast<runtime::ColliderShape>(i);
              modified = true;
            }
          }
          ImGui::EndCombo();
        }
        return draw_reflected_component_fields("engine::runtime::Collider",
                                               &c, g_showAdvanced) ||
              modified;
      });

  draw_component_section(
      entity, ComponentEditType::Light, "Directional/Point Light",
      &ComponentEditSnapshot::light, editable, true,
      [](runtime::LightComponent &c) {
        bool modified = draw_light_type_combo(c);
        return draw_reflected_component_fields("engine::runtime::LightComponent",
                                               &c, g_showAdvanced) ||
              modified;
      });

  draw_component_section(entity, ComponentEditType::Mesh, "Mesh",
                         &ComponentEditSnapshot::mesh, editable, true,
                         [entity, editable](runtime::MeshComponent &c) {
                           return draw_mesh_component_fields(entity, c,
                                                             editable);
                         });

  draw_component_section(
      entity, ComponentEditType::FoliagePatch, "Foliage Patch",
      &ComponentEditSnapshot::foliagePatch, editable, true,
      [entity, editable](runtime::FoliagePatchComponent &c) {
        bool modified = false;
        draw_foliage_patch_fields(entity, c, editable, &modified);
        return modified;
      });

  draw_component_section(entity, ComponentEditType::PointLight, "Point Light",
                         &ComponentEditSnapshot::pointLight, editable, true,
                         [](runtime::PointLightComponent &c) {
                           return draw_reflected_component_fields(
                               "engine::runtime::PointLightComponent", &c,
                               g_showAdvanced);
                         });

  draw_component_section(entity, ComponentEditType::SpotLight, "Spot Light",
                         &ComponentEditSnapshot::spotLight, editable, true,
                         [](runtime::SpotLightComponent &c) {
                           return draw_reflected_component_fields(
                               "engine::runtime::SpotLightComponent", &c,
                               g_showAdvanced);
                         });

  draw_component_section(
      entity, ComponentEditType::ReflectionProbe, "Reflection Probe",
      &ComponentEditSnapshot::reflectionProbe, editable, true,
      [](runtime::ReflectionProbeComponent &c) {
        return draw_reflected_component_fields(
            "engine::runtime::ReflectionProbeComponent", &c, g_showAdvanced);
      });

  draw_component_section(
      entity, ComponentEditType::SceneCapture, "Scene Capture",
      &ComponentEditSnapshot::sceneCapture, editable, true,
      [entity](runtime::SceneCaptureComponent &c) {
        const bool modified = draw_reflected_component_fields(
            "engine::runtime::SceneCaptureComponent", &c, g_showAdvanced);
        draw_scene_capture_preview(entity, c);
        return modified;
      });

  draw_component_section(entity, ComponentEditType::Script,
                         "Script", &ComponentEditSnapshot::script, editable,
                         true, [](runtime::ScriptComponent &c) {
                           return draw_script_component_fields(c);
                         });

  draw_component_section(entity, ComponentEditType::Animation,
                         "Animation", &ComponentEditSnapshot::animation,
                         editable, true, [](runtime::AnimationComponent &c) {
                           return draw_animation_component_fields(c);
                         });

  draw_component_section(
      entity, ComponentEditType::SpringArm, "Spring Arm",
      &ComponentEditSnapshot::springArm, editable, true,
      [](runtime::SpringArmComponent &c) {
        return draw_reflected_component_fields(
            "engine::runtime::SpringArmComponent", &c, g_showAdvanced);
      });
}
// 12 sections above cover every registry row except Name and Transform.
static_assert(kComponentEditTypeCount == 14U,
             "a new persistent-component registry row needs both a section "
             "in draw_component_sections and an entry in "
             "editor_inspector_metadata's ComponentMetadata table");

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
  ImGui::Checkbox("Advanced", &g_showAdvanced);
  ImGui::SameLine();
  ImGui::TextDisabled("(raw ids/paths and simulation state)");

  runtime::NameComponent nameComponent{};
  const bool hasNameComponent =
      editor_session().world->get_name_component(entity, &nameComponent);
  if (hasNameComponent) {
    // The name is entity identity, not a removable behavior -- it can be
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
      execute_component_add(entity, ComponentEditType::Name,
                            default_component_snapshot(
                                entity, ComponentEditType::Name));
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
  if (g_showAdvanced) {
    ImGui::Text("Persistent Id: %u",
               static_cast<unsigned>(editor_session().world->persistent_id(entity)));
  }
  ImGui::Separator();

  // Transform is entity identity (position/axis): editable in place but
  // never removable from the inspector, matching the empty-object mental
  // model -- routed through the generic drawer so Rotation gets the
  // EulerDegrees widget like every other reflected Quat field.
  draw_component_section(
      entity, ComponentEditType::Transform, "Transform",
      &ComponentEditSnapshot::transform, editable, false,
      [](runtime::Transform &c) {
        return draw_reflected_component_fields("engine::runtime::Transform",
                                               &c, g_showAdvanced);
      });

  draw_component_sections(entity, editable);

  draw_add_component_menu(entity, editable);

  if (inspector_has_pending_edit() && !ImGui::IsAnyItemActive()) {
    inspector_commit_pending_edit();
  }

  ImGui::End();
}

} // namespace engine::editor
