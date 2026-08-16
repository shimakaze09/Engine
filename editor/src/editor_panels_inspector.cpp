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
#include "editor_live_edit.h"
#include "editor_multi_edit.h"
#include "editor_panels_inspector_custom.h"
#include "editor_panels_inspector_generic.h"
#include "editor_session.h"

#include "engine/runtime/camera_component_update.h"

#include <cstdint>

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

/// Draws the play-mode live-edit affordance row for one component section:
/// a badge once the field has an uncommitted transient tweak this session,
/// plus "Apply to authored value" (queues the current runtime value for
/// Stop to replay as an ordinary undoable edit) and "Revert runtime edit"
/// (writes the play-session baseline straight back). No-op when the
/// component was never live-edited this session.
void draw_live_edit_row(runtime::Entity entity, ComponentEditType type) noexcept {
  if (!has_live_component_edit(entity, type)) {
    return;
  }
  ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F), "Live edit (transient)");
  ImGui::SameLine();
  if (ImGui::SmallButton("Apply to authored value")) {
    static_cast<void>(queue_apply_to_authored(entity, type));
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Revert runtime edit")) {
    static_cast<void>(revert_live_component_edit(entity, type));
  }
  if (has_pending_apply_to_authored(entity, type)) {
    ImGui::TextDisabled("Queued: will apply to the authored scene on Stop.");
  }
}

/// Shared capture -> draw -> stage/remove boilerplate for one component
/// section; `drawFn` is `bool(Component&)`. Fields render disabled unless
/// `authoredEditable` (Stopped, routes edits through command history) or
/// `liveEditable` (Playing/Paused with the opt-in live-edit toggle on,
/// routes edits straight to the running world and never touches undo --
/// issue #159); the remove-component action stays authored-only since
/// removing a component is a structural edit, not a value tweak.
template <typename Component, typename DrawFn>
void draw_component_section(runtime::Entity entity, ComponentEditType type,
                            const char *sectionLabel,
                            Component ComponentEditSnapshot::*member,
                            bool authoredEditable, bool liveEditable,
                            bool removable, DrawFn &&drawFn) noexcept {
  ComponentEditSnapshot snapshot{};
  if (!capture_component_snapshot(type, entity, &snapshot)) {
    ImGui::Text("%s: <none>", sectionLabel);
    return;
  }
  const Component before = snapshot.*member;
  const bool fieldsEditable = authoredEditable || liveEditable;

  ImGui::PushID(sectionLabel);
  const bool open = ImGui::CollapsingHeader(sectionLabel,
                                            ImGuiTreeNodeFlags_DefaultOpen);
  const bool removePressed =
      removable && draw_remove_component_button("remove", authoredEditable);

  bool modified = false;
  if (open) {
    if (!fieldsEditable) {
      ImGui::BeginDisabled();
    }
    modified = drawFn(snapshot.*member);
    if (!fieldsEditable) {
      ImGui::EndDisabled();
    }
    if (liveEditable) {
      draw_live_edit_row(entity, type);
    } else if (!authoredEditable && live_edit_available()) {
      // The author opted in to live edit, but this specific component's
      // caller passed liveEditable=false (a custom-drawer type out of
      // per-field batch/live scope) -- say so instead of leaving the
      // disabled fields unexplained (issue #159 acceptance: unsupported
      // fields stay read-only with a reason).
      ImGui::TextDisabled(
          "Read-only: live edit not yet supported for this component "
          "(Stop to edit).");
    }
  }
  ImGui::PopID();

  if (authoredEditable && removePressed) {
    execute_component_remove(entity, type);
  } else if (authoredEditable && modified) {
    ComponentEditSnapshot beforeSnapshot{};
    beforeSnapshot.*member = before;
    ComponentEditSnapshot afterSnapshot{};
    afterSnapshot.*member = snapshot.*member;
    static_cast<void>(inspector_stage_component_edit(
        entity, type, beforeSnapshot, afterSnapshot));
  } else if (liveEditable && modified) {
    ComponentEditSnapshot afterSnapshot{};
    afterSnapshot.*member = snapshot.*member;
    static_cast<void>(apply_live_component_edit(entity, type, afterSnapshot));
  }
}

/// Renders every non-identity persistent-component section. One row per
/// registry entry except Name and Transform (drawn separately above as
/// entity identity); the count assert below fails the build if a new
/// registry row is not also added here, matching the ledger the ADD
/// COMPONENT menu already keeps generically. `liveEditable` (Playing/Paused
/// with the live-edit toggle on) only reaches the reflected-field sections
/// (RigidBody, Collider, Light, PointLight, SpotLight, ReflectionProbe,
/// SpringArm) -- the custom-drawer sections below (Mesh, FoliagePatch,
/// Script, Animation, SceneCapture) manage their own sub-widget structural
/// edits (asset picks, foliage instance add/remove) that are out of scope
/// for issue #159's live-edit path and stay Stop-only, always read-only
/// during Play regardless of the toggle.
void draw_component_sections(runtime::Entity entity, bool authoredEditable,
                             bool liveEditable) noexcept {
  draw_component_section(
      entity, ComponentEditType::RigidBody, "Rigid Body",
      &ComponentEditSnapshot::rigidBody, authoredEditable, liveEditable, true,
      [](runtime::RigidBody &c) {
        return draw_reflected_component_fields("engine::runtime::RigidBody",
                                               &c, g_showAdvanced);
      });

  draw_component_section(
      entity, ComponentEditType::Collider, "Collider",
      &ComponentEditSnapshot::collider, authoredEditable, liveEditable, true,
      [](runtime::Collider &c) {
        // The combo's selectable options come from metadata (the Enum
        // widget kind); the display-only names cover the two shapes a
        // primitive spawn or import can produce but this combo cannot
        // select into (a convex hull needs provenance only primitive
        // spawns carry, and heightfields are not editor-authorable at all
        // -- see the field's tooltip in editor_inspector_metadata), so an
        // inspected cylinder/pyramid/imported collider still shows its
        // real shape name instead of clamping to the first entry.
        constexpr const char *kDisplayOnlyNames[] = {"Convex Hull",
                                                      "Heightfield"};
        constexpr const char *kFallbackSelectable[] = {"Box", "Sphere",
                                                        "Capsule"};
        const FieldMetadata *meta =
            find_field_metadata("engine::runtime::Collider", "shape");
        const char *const *selectable =
            (meta != nullptr) ? meta->enumLabels : kFallbackSelectable;
        const int selectableCount =
            (meta != nullptr) ? static_cast<int>(meta->enumLabelCount) : 3;

        int shapeIndex = static_cast<int>(c.shape);
        if ((shapeIndex < 0) || (shapeIndex >= 5)) {
          shapeIndex = 0;
        }
        const char *currentLabel =
            (shapeIndex < selectableCount)
                ? selectable[shapeIndex]
                : kDisplayOnlyNames[shapeIndex - selectableCount];
        bool modified = false;
        if (ImGui::BeginCombo("Shape", currentLabel)) {
          for (int i = 0; i < selectableCount; ++i) {
            if (ImGui::Selectable(selectable[i], shapeIndex == i) &&
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
      &ComponentEditSnapshot::light, authoredEditable, liveEditable, true,
      [](runtime::LightComponent &c) {
        bool modified = draw_light_type_combo(c);
        return draw_reflected_component_fields("engine::runtime::LightComponent",
                                               &c, g_showAdvanced) ||
              modified;
      });

  draw_component_section(entity, ComponentEditType::Mesh, "Mesh",
                         &ComponentEditSnapshot::mesh, authoredEditable, false,
                         true,
                         [entity, authoredEditable](runtime::MeshComponent &c) {
                           return draw_mesh_component_fields(entity, c,
                                                             authoredEditable);
                         });

  draw_component_section(
      entity, ComponentEditType::FoliagePatch, "Foliage Patch",
      &ComponentEditSnapshot::foliagePatch, authoredEditable, false, true,
      [entity, authoredEditable](runtime::FoliagePatchComponent &c) {
        bool modified = false;
        draw_foliage_patch_fields(entity, c, authoredEditable, &modified);
        return modified;
      });

  draw_component_section(entity, ComponentEditType::PointLight, "Point Light",
                         &ComponentEditSnapshot::pointLight, authoredEditable,
                         liveEditable, true,
                         [](runtime::PointLightComponent &c) {
                           return draw_reflected_component_fields(
                               "engine::runtime::PointLightComponent", &c,
                               g_showAdvanced);
                         });

  draw_component_section(entity, ComponentEditType::SpotLight, "Spot Light",
                         &ComponentEditSnapshot::spotLight, authoredEditable,
                         liveEditable, true,
                         [](runtime::SpotLightComponent &c) {
                           return draw_reflected_component_fields(
                               "engine::runtime::SpotLightComponent", &c,
                               g_showAdvanced);
                         });

  draw_component_section(
      entity, ComponentEditType::ReflectionProbe, "Reflection Probe",
      &ComponentEditSnapshot::reflectionProbe, authoredEditable, liveEditable,
      true,
      [](runtime::ReflectionProbeComponent &c) {
        return draw_reflected_component_fields(
            "engine::runtime::ReflectionProbeComponent", &c, g_showAdvanced);
      });

  draw_component_section(
      entity, ComponentEditType::SceneCapture, "Scene Capture",
      &ComponentEditSnapshot::sceneCapture, authoredEditable, false, true,
      [entity](runtime::SceneCaptureComponent &c) {
        const bool modified = draw_reflected_component_fields(
            "engine::runtime::SceneCaptureComponent", &c, g_showAdvanced);
        draw_scene_capture_preview(entity, c);
        return modified;
      });

  draw_component_section(entity, ComponentEditType::Script,
                         "Script", &ComponentEditSnapshot::script,
                         authoredEditable, false,
                         true, [](runtime::ScriptComponent &c) {
                           return draw_script_component_fields(c);
                         });

  draw_component_section(entity, ComponentEditType::Animation,
                         "Animation", &ComponentEditSnapshot::animation,
                         authoredEditable, false, true,
                         [](runtime::AnimationComponent &c) {
                           return draw_animation_component_fields(c);
                         });

  draw_component_section(
      entity, ComponentEditType::SpringArm, "Spring Arm",
      &ComponentEditSnapshot::springArm, authoredEditable, liveEditable, true,
      [](runtime::SpringArmComponent &c) {
        return draw_reflected_component_fields(
            "engine::runtime::SpringArmComponent", &c, g_showAdvanced);
      });

  draw_component_section(
      entity, ComponentEditType::Camera, "Camera",
      &ComponentEditSnapshot::camera, editable, true,
      [entity](runtime::CameraComponent &c) {
        // Projection is a plain uint32 (not a reflected C++ enum, matching
        // Collider.shape/LightComponent.type) so it stays in the generic
        // codec; the combo below is the same "hand-drawn selector ahead of
        // the generic loop" pattern those two use.
        constexpr const char *kProjectionFallback[] = {"Perspective",
                                                        "Orthographic"};
        const FieldMetadata *meta = find_field_metadata(
            "engine::runtime::CameraComponent", "projection");
        const char *const *labels =
            (meta != nullptr) ? meta->enumLabels : kProjectionFallback;
        const int labelCount =
            (meta != nullptr) ? static_cast<int>(meta->enumLabelCount) : 2;
        int projIndex = static_cast<int>(c.projection);
        if ((projIndex < 0) || (projIndex >= labelCount)) {
          projIndex = 0;
        }
        bool modified = false;
        if (ImGui::BeginCombo("Projection", labels[projIndex])) {
          for (int i = 0; i < labelCount; ++i) {
            if (ImGui::Selectable(labels[i], projIndex == i) &&
                (projIndex != i)) {
              c.projection = static_cast<std::uint32_t>(i);
              modified = true;
            }
          }
          ImGui::EndCombo();
        }
        modified = draw_reflected_component_fields(
                       "engine::runtime::CameraComponent", &c,
                       g_showAdvanced) ||
                   modified;

        // Selection/conflict status (acceptance: "reports conflicts/
        // no-camera states clearly"). Computed from authored components
        // directly so it reads correctly in Edit mode too, not only while
        // CameraManager is populated during Play.
        if (editor_session().world != nullptr) {
          std::uint32_t tieCount = 0U;
          const runtime::Entity activeCam = runtime::find_authored_active_camera(
              *editor_session().world, &tieCount);
          if (!c.active) {
            ImGui::TextDisabled("Disabled");
          } else if (activeCam == entity) {
            if (tieCount > 0U) {
              ImGui::TextColored(
                  ImVec4(1.0F, 0.8F, 0.2F, 1.0F),
                  "Active game camera (priority tied with %u other%s)",
                  tieCount, (tieCount == 1U) ? "" : "s");
            } else {
              ImGui::TextColored(ImVec4(0.3F, 0.9F, 0.3F, 1.0F),
                                 "Active game camera");
            }
          } else {
            ImGui::TextDisabled(
                "Not selected -- another camera has higher priority");
          }
        }

        // Optional live preview (acceptance: "optional live camera
        // preview"): reuses the existing SceneCapture render-to-texture
        // pipeline when the author also attaches a Scene Capture component
        // to this entity, rather than duplicating a second capture path.
        runtime::SceneCaptureComponent preview{};
        if ((editor_session().world != nullptr) &&
            editor_session().world->get_scene_capture_component(entity,
                                                                 &preview)) {
          ImGui::Separator();
          ImGui::TextUnformatted("Live Preview (via Scene Capture)");
          draw_scene_capture_preview(entity, preview);
        } else {
          ImGui::TextDisabled(
              "Live preview: add a Scene Capture component for a rendered "
              "preview");
        }

        return modified;
      });
}
// 13 sections above cover every registry row except Name and Transform.
static_assert(kComponentEditTypeCount == 15U,
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

  // Multi-selection (issue #159): common components across every selected
  // entity, mixed-value fields flagged, batch edits as one undoable
  // command. A pending single-entity gesture from before the selection
  // grew must still commit instead of being silently dropped.
  if (editor_session().selectedEntityCount > 1U) {
    inspector_commit_pending_edit();
    draw_multi_select_inspector_panel();
    ImGui::End();
    return;
  }

  const runtime::Entity entity = selected_entity();
  if ((editor_session().world == nullptr) ||
      (entity == runtime::kInvalidEntity)) {
    inspector_commit_pending_edit();
    ImGui::TextUnformatted("No entity selected");
    ImGui::End();
    return;
  }

  // authoredEditable: Stopped, edits route through command history as
  // usual. liveEditable: Playing/Paused with the opt-in live-edit toggle
  // on (issue #159) -- edits write straight to the running world and
  // never touch undo/dirty state. The two are mutually exclusive (Stopped
  // implies liveEditable is false) so callers never need to pick between
  // them for a single field.
  const bool authoredEditable = world_is_editable();
  const bool liveEditable = !authoredEditable && live_edit_available();
  if (editor_session().playState != PlayState::Stopped) {
    const bool paused = editor_session().playState == PlayState::Paused;
    ImGui::TextColored(ImVec4(0.4F, 0.8F, 1.0F, 1.0F), "%s -- runtime values",
                       paused ? "PAUSED" : "PLAYING");
    ImGui::SameLine();
    ImGui::Checkbox("Live Edit", &editor_session().liveEditEnabled);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Opt in to edit the running world directly. Live edits are "
          "transient, never enter Undo, and are discarded on Stop unless "
          "explicitly applied to the authored value.");
    }
    if (!editor_session().liveEditEnabled) {
      ImGui::TextDisabled("Read-only: values shown are the live simulation "
                          "state, not the authored scene.");
    }
    ImGui::Separator();
  }

  ImGui::Checkbox("Advanced", &g_showAdvanced);
  ImGui::SameLine();
  ImGui::TextDisabled("(raw ids/paths and simulation state)");

  runtime::NameComponent nameComponent{};
  const bool hasNameComponent =
      editor_session().world->get_name_component(entity, &nameComponent);
  if (hasNameComponent) {
    // The name is entity identity, not a removable behavior -- it can be
    // edited but never deleted from the inspector. Renaming stays
    // authored-only: it is not part of issue #159's live-edit scope.
    const runtime::NameComponent nameBefore = nameComponent;
    bool nameChanged = false;
    if (!authoredEditable) {
      ImGui::BeginDisabled();
    }

    nameChanged = ImGui::InputText("Name", nameComponent.name,
                                   sizeof(nameComponent.name));

    if (!authoredEditable) {
      ImGui::EndDisabled();
    }

    if (authoredEditable && nameChanged) {
      ComponentEditSnapshot before{};
      before.name = nameBefore;
      ComponentEditSnapshot after{};
      after.name = nameComponent;
      static_cast<void>(inspector_stage_component_edit(
          entity, ComponentEditType::Name, before, after));
    }
  } else {
    if (!authoredEditable) {
      ImGui::BeginDisabled();
    }

    if (ImGui::SmallButton("+ Name") && authoredEditable) {
      execute_component_add(entity, ComponentEditType::Name,
                            default_component_snapshot(
                                entity, ComponentEditType::Name));
    }

    if (!authoredEditable) {
      ImGui::EndDisabled();
    }
  }

  ImGui::Separator();

  if (!authoredEditable) {
    ImGui::BeginDisabled();
  }

  const bool deletePressed = ImGui::Button("Delete Entity");

  if (!authoredEditable) {
    ImGui::EndDisabled();
  }

  if (authoredEditable && deletePressed) {
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
  // EulerDegrees widget like every other reflected Quat field. Transform
  // is in live-edit scope (tuning a playing actor's pose is the canonical
  // use case).
  draw_component_section(
      entity, ComponentEditType::Transform, "Transform",
      &ComponentEditSnapshot::transform, authoredEditable, liveEditable,
      false,
      [](runtime::Transform &c) {
        return draw_reflected_component_fields("engine::runtime::Transform",
                                               &c, g_showAdvanced);
      });

  draw_component_sections(entity, authoredEditable, liveEditable);

  draw_add_component_menu(entity, authoredEditable);

  if (inspector_has_pending_edit() && !ImGui::IsAnyItemActive()) {
    inspector_commit_pending_edit();
  }

  ImGui::End();
}

} // namespace engine::editor
