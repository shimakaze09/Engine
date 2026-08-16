// Implements multi-entity Inspector support declared in editor_multi_edit.h.

#include "editor_multi_edit.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <new>

#include "editor_commands.h"
#include "editor_inspector_metadata.h"
#include "editor_panels_inspector_generic.h"
#include "editor_session.h"
#include "engine/core/reflect.h"
#include "engine/runtime/world.h"

namespace engine::editor {

void *component_member_ptr(ComponentEditType type,
                           ComponentEditSnapshot *snapshot) noexcept {
  if (snapshot == nullptr) {
    return nullptr;
  }
  switch (type) {
#define ENGINE_ICR_PTR_ROW(Type, Key, GetFn, AddFn, RemoveFn)                 \
  case ComponentEditType::ENGINE_ICR_ALIAS(Type):                             \
    return &snapshot->ENGINE_ICR_MEMBER(Type);
    ENGINE_PERSISTENT_COMPONENT_TABLE(ENGINE_ICR_PTR_ROW)
#undef ENGINE_ICR_PTR_ROW
  }
  return nullptr;
}

const void *
component_member_ptr(ComponentEditType type,
                     const ComponentEditSnapshot &snapshot) noexcept {
  return component_member_ptr(type, const_cast<ComponentEditSnapshot *>(&snapshot));
}

void compute_selection_common_components(
    std::array<bool, kComponentEditTypeCount> *outCommon) noexcept {
  if (outCommon == nullptr) {
    return;
  }
  outCommon->fill(false);
  const EditorSession &session = editor_session();
  if ((session.world == nullptr) || (session.selectedEntityCount == 0U)) {
    return;
  }
  for (std::size_t t = 0U; t < kComponentEditTypeCount; ++t) {
    const auto type = static_cast<ComponentEditType>(t);
    bool all = true;
    for (std::size_t i = 0U; all && (i < session.selectedEntityCount); ++i) {
      all = has_component_of_type(type, session.selectedEntities[i]);
    }
    (*outCommon)[t] = all;
  }
}

bool selection_field_is_mixed(ComponentEditType type, std::size_t fieldOffset,
                              std::size_t fieldSize) noexcept {
  const EditorSession &session = editor_session();
  if ((session.world == nullptr) || (session.selectedEntityCount < 2U)) {
    return false;
  }
  ComponentEditSnapshot first{};
  if (!capture_component_snapshot(type, session.selectedEntities[0], &first)) {
    return false;
  }
  const void *firstMember = component_member_ptr(type, first);
  if (firstMember == nullptr) {
    return false;
  }
  const auto *firstBytes = static_cast<const std::byte *>(firstMember);

  for (std::size_t i = 1U; i < session.selectedEntityCount; ++i) {
    ComponentEditSnapshot other{};
    if (!capture_component_snapshot(type, session.selectedEntities[i],
                                    &other)) {
      // Presence is guaranteed by compute_selection_common_components
      // before this is called for a drawn section; treat an unexpected
      // miss as mixed rather than silently reporting "common".
      return true;
    }
    const void *otherMember = component_member_ptr(type, other);
    if (otherMember == nullptr) {
      return true;
    }
    if (std::memcmp(static_cast<const void *>(firstBytes + fieldOffset),
                    static_cast<const std::byte *>(otherMember) + fieldOffset,
                    fieldSize) != 0) {
      return true;
    }
  }
  return false;
}

bool selection_representative_component(
    ComponentEditType type, ComponentEditSnapshot *outRepresentative) noexcept {
  const EditorSession &session = editor_session();
  if ((session.world == nullptr) || (session.selectedEntityCount == 0U) ||
      (outRepresentative == nullptr)) {
    return false;
  }
  return capture_component_snapshot(type, session.selectedEntities[0],
                                    outRepresentative);
}

namespace {

/// One entity's before/after component value inside a multi-edit gesture.
/// beforeExists/afterExists default true (a value-only field edit never
/// changes presence); apply_multi_component_remove sets afterExists false
/// so the same command shape and atomic rollback cover batch removal too.
struct MultiEditEntry final {
  runtime::Entity entity{};
  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  ComponentEditSnapshot before{};
  ComponentEditSnapshot after{};
  bool beforeExists = true;
  bool afterExists = true;
};

/// Undoable batch edit: applies one component type's before/after snapshot
/// to every captured entity as a single history entry (one gesture, one
/// undo step). execute()/undo() are atomic -- a mid-batch failure rolls
/// back everything already touched this call and reports failure instead
/// of leaving a partially-applied selection.
struct MultiComponentEditCommand final : EditorCommand {
  ComponentEditType type = ComponentEditType::Transform;
  std::array<MultiEditEntry, EditorSession::kMaxSelectedEntities> entries{};
  std::size_t entryCount = 0U;

  bool execute() noexcept override { return apply_all(true); }
  bool undo() noexcept override { return apply_all(false); }

private:
  bool apply_all(bool useAfter) noexcept {
    std::size_t applied = 0U;
    bool ok = true;
    for (; applied < entryCount; ++applied) {
      MultiEditEntry &entry = entries[applied];
      const runtime::Entity target =
          resolve_command_target(entry.entity, entry.persistentId);
      const ComponentEditSnapshot &snapshot =
          useAfter ? entry.after : entry.before;
      const bool exists = useAfter ? entry.afterExists : entry.beforeExists;
      if (!apply_component_snapshot(type, target, exists, snapshot)) {
        ok = false;
        break;
      }
    }
    if (ok) {
      return true;
    }
    // Roll every already-applied entry back to the endpoint this call
    // started from, so a mid-batch failure leaves the world exactly as it
    // found it rather than half-migrated.
    for (std::size_t i = 0U; i < applied; ++i) {
      MultiEditEntry &entry = entries[i];
      const runtime::Entity target =
          resolve_command_target(entry.entity, entry.persistentId);
      const ComponentEditSnapshot &rollback =
          useAfter ? entry.before : entry.after;
      const bool rollbackExists =
          useAfter ? entry.beforeExists : entry.afterExists;
      static_cast<void>(
          apply_component_snapshot(type, target, rollbackExists, rollback));
    }
    return false;
  }
};

} // namespace

bool apply_multi_field_edit(ComponentEditType type, std::size_t fieldOffset,
                            std::size_t fieldSize,
                            const ComponentEditSnapshot &fieldSource) noexcept {
  EditorSession &session = editor_session();
  runtime::World *const world = session.world;
  if ((world == nullptr) || (session.selectedEntityCount == 0U)) {
    return false;
  }
  const void *sourceMember = component_member_ptr(type, fieldSource);
  if (sourceMember == nullptr) {
    return false;
  }
  const auto *sourceBytes =
      static_cast<const std::byte *>(sourceMember) + fieldOffset;

  auto *cmd = new (std::nothrow) MultiComponentEditCommand();
  if (cmd == nullptr) {
    return false;
  }
  cmd->type = type;

  for (std::size_t i = 0U; i < session.selectedEntityCount; ++i) {
    const runtime::Entity entity = session.selectedEntities[i];
    if (!world->is_alive(entity)) {
      delete cmd;
      return false;
    }
    ComponentEditSnapshot before{};
    if (!capture_component_snapshot(type, entity, &before)) {
      delete cmd;
      return false;
    }
    ComponentEditSnapshot after = before;
    void *afterMember = component_member_ptr(type, &after);
    if (afterMember == nullptr) {
      delete cmd;
      return false;
    }
    std::memcpy(static_cast<std::byte *>(afterMember) + fieldOffset,
               sourceBytes, fieldSize);

    MultiEditEntry &entry = cmd->entries[cmd->entryCount++];
    entry.entity = entity;
    entry.persistentId = world->persistent_id(entity);
    entry.before = before;
    entry.after = after;
  }

  return session.commandHistory.execute(cmd);
}

bool apply_multi_component_remove(ComponentEditType type) noexcept {
  EditorSession &session = editor_session();
  runtime::World *const world = session.world;
  if ((world == nullptr) || (session.selectedEntityCount == 0U)) {
    return false;
  }

  auto *cmd = new (std::nothrow) MultiComponentEditCommand();
  if (cmd == nullptr) {
    return false;
  }
  cmd->type = type;

  for (std::size_t i = 0U; i < session.selectedEntityCount; ++i) {
    const runtime::Entity entity = session.selectedEntities[i];
    if (!world->is_alive(entity)) {
      delete cmd;
      return false;
    }
    ComponentEditSnapshot before{};
    if (!capture_component_snapshot(type, entity, &before)) {
      delete cmd;
      return false; // removal targets only entities that currently have it
    }

    MultiEditEntry &entry = cmd->entries[cmd->entryCount++];
    entry.entity = entity;
    entry.persistentId = world->persistent_id(entity);
    entry.before = before;
    entry.beforeExists = true;
    entry.after = before;
    entry.afterExists = false;
  }

  return session.commandHistory.execute(cmd);
}

namespace {

/// Reflected type names the multi Inspector's per-field editor covers (the
/// same set editor_panels_inspector.cpp routes through
/// draw_reflected_component_fields for a single entity); components with a
/// custom drawer (Name, Mesh, FoliagePatch, Script, Animation, SceneCapture)
/// are out of scope for per-field multi-edit and are skipped here --
/// deferred scope, tracked in the PR description rather than silently
/// mis-edited.
struct MultiSectionDesc final {
  ComponentEditType type;
  const char *typeName;
  const char *label;
  bool removable;
};

constexpr MultiSectionDesc kMultiSections[] = {
    {ComponentEditType::Transform, kTransformTypeName, "Transform", false},
    {ComponentEditType::RigidBody, kRigidBodyTypeName, "Rigid Body", true},
    {ComponentEditType::Collider, kColliderTypeName, "Collider", true},
    {ComponentEditType::Light, kLightTypeName, "Directional/Point Light",
     true},
    {ComponentEditType::PointLight, kPointLightTypeName, "Point Light", true},
    {ComponentEditType::SpotLight, kSpotLightTypeName, "Spot Light", true},
    {ComponentEditType::ReflectionProbe, kReflectionProbeTypeName,
     "Reflection Probe", true},
    {ComponentEditType::SpringArm, kSpringArmTypeName, "Spring Arm", true},
};

/// Draws every reflected field of one common component across the
/// selection: mixed fields get a "(mixed)" label suffix and a shared edit
/// still overwrites every selected entity uniformly (the standard
/// Unity/Unreal multi-edit convention -- editing a mixed field commits one
/// new common value rather than exposing N independent widgets). A
/// removable section also gets a "Remove from all" button, atomic across
/// the selection like the field edit above.
void draw_multi_component_section(const MultiSectionDesc &desc) noexcept {
  const core::TypeDescriptor *typeDesc =
      core::global_type_registry().find_type(desc.typeName);
  if (typeDesc == nullptr) {
    return;
  }
  ComponentEditSnapshot representative{};
  if (!selection_representative_component(desc.type, &representative)) {
    return;
  }
  void *component = component_member_ptr(desc.type, &representative);
  if (component == nullptr) {
    return;
  }

  ImGui::PushID(desc.label);
  const bool open =
      ImGui::CollapsingHeader(desc.label, ImGuiTreeNodeFlags_DefaultOpen);
  bool removePressed = false;
  if (desc.removable) {
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 110.0F);
    removePressed = ImGui::SmallButton("Remove from all");
  }
  if (removePressed) {
    static_cast<void>(apply_multi_component_remove(desc.type));
  }
  if (open) {
    for (std::size_t i = 0U; i < typeDesc->fieldCount; ++i) {
      const core::TypeField &field = typeDesc->fields[i];
      const bool mixed =
          selection_field_is_mixed(desc.type, field.offset, field.size);
      if (draw_reflected_field(desc.typeName, field.name, component,
                               mixed ? " (mixed)" : nullptr, false)) {
        static_cast<void>(apply_multi_field_edit(
            desc.type, field.offset, field.size, representative));
      }
    }
  }
  ImGui::PopID();
}

} // namespace

void draw_multi_select_inspector_panel() noexcept {
  const EditorSession &session = editor_session();
  if (session.selectedEntityCount < 2U) {
    return;
  }

  ImGui::Text("%zu entities selected", session.selectedEntityCount);
  ImGui::Separator();

  const bool editable = world_is_editable();
  if (!editable) {
    ImGui::TextDisabled("Multi-edit is available only while editing "
                        "(Stopped).");
    return;
  }

  std::array<bool, kComponentEditTypeCount> common{};
  compute_selection_common_components(&common);

  bool anyCommon = false;
  for (const MultiSectionDesc &desc : kMultiSections) {
    if (common[static_cast<std::size_t>(desc.type)]) {
      anyCommon = true;
      draw_multi_component_section(desc);
    }
  }
  if (!anyCommon) {
    ImGui::TextDisabled("Selected entities share no editable component.");
  }
}

} // namespace engine::editor
