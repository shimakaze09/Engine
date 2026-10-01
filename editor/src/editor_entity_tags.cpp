// Implements the Inspector's Tags row: the typed-tag rule and the row that
// edits the selected entity's tags through the command history.

#include "editor_entity_tags.h"

#include "editor_commands.h"
#include "editor_session.h"

#include "imgui.h"

#include <cstdio>

namespace engine::editor {

namespace {

/// The tag being typed, and why the last add was refused; both belong to
/// the entity they were typed for and are cleared when it changes.
struct TagEditState final {
  runtime::Entity entity{};
  char typed[runtime::TagSetComponent::kMaxTagLength + 8U] = {};
  const char *problem = nullptr;
};
TagEditState g_edit{};

} // namespace

void commit_entity_tags(runtime::Entity entity,
                        const runtime::TagSetComponent *before,
                        const runtime::TagSetComponent &after) noexcept {
  if (before == nullptr) {
    if (after.count > 0U) {
      ComponentEditSnapshot added{};
      added.tagSet = after;
      execute_component_add(entity, ComponentEditType::Tags, added);
    }
    return;
  }
  if (after.count == 0U) {
    execute_component_remove(entity, ComponentEditType::Tags);
    return;
  }
  ComponentEditSnapshot from{};
  from.tagSet = *before;
  ComponentEditSnapshot to{};
  to.tagSet = after;
  if (inspector_stage_component_edit(entity, ComponentEditType::Tags, from,
                                     to)) {
    inspector_commit_pending_edit();
  }
}

const char *apply_typed_tag(const runtime::TagSetComponent &current,
                            const char *typed,
                            runtime::TagSetComponent *out) noexcept {
  *out = current;
  switch (runtime::tag_set_add(out, typed)) {
  case runtime::TagSetAdd::Added:
  case runtime::TagSetAdd::AlreadyPresent:
    return nullptr;
  case runtime::TagSetAdd::InvalidTag:
    *out = current;
    return "A tag is 1 to 31 letters, digits, '_', '-' or '.'.";
  case runtime::TagSetAdd::Full:
    *out = current;
    return "An entity carries at most 8 tags.";
  }
  *out = current;
  return nullptr;
}

void draw_entity_tags_row(runtime::Entity entity, bool editable) noexcept {
  runtime::World *world = editor_session().world;
  if (world == nullptr) {
    return;
  }
  if (!(g_edit.entity == entity)) {
    g_edit = TagEditState{};
    g_edit.entity = entity;
  }
  runtime::TagSetComponent tags{};
  const bool hasTags = world->get_tag_set_component_ptr(entity) != nullptr;
  if (hasTags) {
    static_cast<void>(world->get_tag_set_component(entity, &tags));
  }

  ImGui::PushID("EntityTags");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Tags:");
  for (std::size_t i = 0U; i < tags.count; ++i) {
    ImGui::SameLine();
    if (!editable) {
      ImGui::TextUnformatted(tags.tags[i]);
      continue;
    }
    char button[runtime::TagSetComponent::kMaxTagLength + 8U] = {};
    std::snprintf(button, sizeof(button), "%s  x", tags.tags[i]);
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::SmallButton(button)) {
      runtime::TagSetComponent edited = tags;
      static_cast<void>(runtime::tag_set_remove(&edited, tags.tags[i]));
      commit_entity_tags(entity, &tags, edited);
      ImGui::PopID();
      break; // the set just changed; the next frame draws it
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
      ImGui::SetTooltip("Remove the tag; scripts find tagged entities with "
                        "engine.find_entities_by_tag(\"%s\")",
                        tags.tags[i]);
    }
    ImGui::PopID();
  }
  if (!editable) {
    if (tags.count == 0U) {
      ImGui::SameLine();
      ImGui::TextDisabled("none");
    }
    ImGui::PopID();
    return;
  }

  ImGui::SameLine();
  ImGui::SetNextItemWidth(editor_px(140.0F));
  if (ImGui::InputTextWithHint("##AddTag", "Add tag", g_edit.typed,
                               sizeof(g_edit.typed),
                               ImGuiInputTextFlags_EnterReturnsTrue)) {
    runtime::TagSetComponent edited{};
    g_edit.problem = apply_typed_tag(tags, g_edit.typed, &edited);
    if (g_edit.problem == nullptr) {
      if (edited.count != tags.count) {
        commit_entity_tags(entity, hasTags ? &tags : nullptr, edited);
      }
      g_edit.typed[0] = '\0';
    }
    ImGui::SetKeyboardFocusHere(-1);
  }
  if (g_edit.problem != nullptr) {
    ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.35F, 1.0F), "%s", g_edit.problem);
  }
  ImGui::PopID();
}

} // namespace engine::editor
