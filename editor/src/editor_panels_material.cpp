// Implements the material editor panel declared in editor_panels_material.h:
// the field widgets, the panel's undo-target focus tracking, and the
// unsaved-change prompt that gates closing or switching a dirty material.
// The decisions themselves are production logic in editor_material_edit.cpp;
// this file only presents them.

#include "editor_panels_material.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include "editor_material_edit.h"
#include "editor_reference_pickers.h"
#include "engine/content/asset_metadata.h"
#include "engine/renderer/material.h"

namespace engine::editor {

namespace {

constexpr const char *kAlphaModeNames[] = {"Opaque", "Mask", "Blend"};

/// Draws every scalar/vector/enum field; returns true if any changed.
bool draw_scalar_fields(renderer::Material &params) noexcept {
  bool changed = false;
  changed |= ImGui::ColorEdit3("Albedo", &params.albedo.x);
  changed |= ImGui::ColorEdit3("Emissive", &params.emissive.x);
  changed |= ImGui::SliderFloat("Roughness", &params.roughness, 0.0F, 1.0F,
                                "%.2f");
  changed |=
      ImGui::SliderFloat("Metallic", &params.metallic, 0.0F, 1.0F, "%.2f");
  changed |=
      ImGui::SliderFloat("Opacity", &params.opacity, 0.0F, 1.0F, "%.2f");

  int alphaMode = static_cast<int>(params.alphaMode);
  if (ImGui::Combo("Alpha Mode", &alphaMode, kAlphaModeNames, 3)) {
    params.alphaMode = static_cast<renderer::AlphaMode>(alphaMode);
    changed = true;
  }
  if (params.alphaMode == renderer::AlphaMode::Mask) {
    changed |= ImGui::SliderFloat("Alpha Cutoff", &params.alphaCutoff, 0.0F,
                                  1.0F, "%.2f");
  }

  changed |= ImGui::DragFloat2("UV Tiling", &params.uvTiling.x, 0.01F);
  changed |= ImGui::DragFloat2("UV Offset", &params.uvOffset.x, 0.01F);
  return changed;
}

/// Draws every texture-slot picker; returns true if any changed. Pickers
/// share the #157/#218 searchable asset-reference widget (issue #160
/// acceptance: texture slots use the same picker as every other asset
/// reference, not a raw path field).
bool draw_texture_slot_fields(renderer::MaterialTextureSlots &slots) noexcept {
  bool changed = false;
  changed |= draw_asset_reference_picker(
      "Albedo Texture", content::AssetTypeTag::Texture, &slots.albedo);
  changed |= draw_asset_reference_picker("Metallic/Roughness Texture",
                                         content::AssetTypeTag::Texture,
                                         &slots.metallicRoughness);
  changed |= draw_asset_reference_picker(
      "Emissive Texture", content::AssetTypeTag::Texture, &slots.emissive);
  changed |= draw_asset_reference_picker(
      "Occlusion Texture", content::AssetTypeTag::Texture, &slots.occlusion);
  changed |= draw_asset_reference_picker(
      "Opacity Texture", content::AssetTypeTag::Texture, &slots.opacity);
  return changed;
}

/// Tracks whether this panel is the undo target; runs inside the panel's
/// Begin/End. Focus on the panel (or a child of it) takes the target;
/// focus moving to another regular window releases it. Menus and popups
/// leave it as it was, so Edit > Undo still reaches the material the
/// user was editing when they opened the menu.
void update_undo_target(MaterialEditorState &state) noexcept {
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    state.undoTarget = true;
  } else if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) &&
             !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) {
    state.undoTarget = false;
  }
}

/// Draws the Save/Discard/Cancel modal that gates closing the panel or
/// switching it to another material while the open one is dirty.
void draw_unsaved_material_prompt(const MaterialEditorState &state) noexcept {
  if (!material_editor_prompt_open()) {
    return;
  }

  constexpr const char *kPopupId =
      "Unsaved Material Changes###material_unsaved_prompt";
  if (!ImGui::IsPopupOpen(kPopupId)) {
    ImGui::OpenPopup(kPopupId);
  }

  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  if (viewport != nullptr) {
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5F, 0.5F));
  }

  if (ImGui::BeginPopupModal(kPopupId, nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Save changes to material \"%s\" before continuing?",
                state.virtualPath);
    if (state.lastSaveError[0] != '\0') {
      ImGui::TextColored(ImVec4(0.9F, 0.35F, 0.35F, 1.0F), "%s",
                         state.lastSaveError);
    }

    if (ImGui::Button("Save")) {
      material_editor_prompt_choose_save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
      ImGui::CloseCurrentPopup();
      material_editor_prompt_choose_discard();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
      material_editor_prompt_choose_cancel();
    }

    if (!material_editor_prompt_open()) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

} // namespace

void draw_material_editor_panel() noexcept {
  MaterialEditorState &state = material_editor_state();
  if (!state.open) {
    return;
  }

  bool stillOpen = true;
  ImGui::SetNextWindowSize(ImVec2(420.0F, 520.0F), ImGuiCond_FirstUseEver);
  const bool visible = ImGui::Begin("Material Editor", &stillOpen);
  update_undo_target(state);

  if (visible && !state.found) {
    ImGui::TextColored(ImVec4(0.9F, 0.4F, 0.3F, 1.0F),
                       "Failed to load material: %s", state.virtualPath);
  }

  bool changed = false;
  bool anyItemActive = false;
  // The gesture's "before" snapshot: taken before any widget below can
  // mutate the buffer, so material_editor_apply_frame always records the
  // true pre-edit value on the first changed frame of a drag/interaction.
  const renderer::Material beforeFrameParams = state.buffer;
  const renderer::MaterialTextureSlots beforeFrameSlots = state.textureSlots;
  const bool drawFields = visible && state.found;
  if (drawFields) {
    ImGui::TextDisabled("%s", state.virtualPath);
    if (state.hasParent) {
      ImGui::TextDisabled("Parent: %s", state.parentVirtualPath);
    }
    if (material_editor_is_dirty()) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.9F, 0.75F, 0.2F, 1.0F), "(unsaved)");
    }
    ImGui::TextDisabled(
        "Edits apply live to the viewport immediately; Save writes to disk.");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
      changed |= draw_scalar_fields(state.buffer);
    }
    if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen)) {
      changed |= draw_texture_slot_fields(state.textureSlots);
    }

    ImGui::Separator();
    if (ImGui::Button("Save")) {
      static_cast<void>(save_material_editor());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload from Disk")) {
      static_cast<void>(reload_material_editor_from_disk());
    }
    if (state.lastSaveError[0] != '\0') {
      ImGui::TextColored(ImVec4(0.9F, 0.35F, 0.35F, 1.0F), "%s",
                         state.lastSaveError);
    }

    anyItemActive = ImGui::IsAnyItemActive();
  }
  ImGui::End();

  if (drawFields) {
    material_editor_apply_frame(beforeFrameParams, beforeFrameSlots, changed,
                                anyItemActive);
  }

  draw_unsaved_material_prompt(state);

  if (!stillOpen) {
    request_close_material_editor();
  }
}

} // namespace engine::editor
