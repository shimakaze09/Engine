// Implements the material editor panel declared in editor_panels_material.h.

#include "editor_panels_material.h"

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include "imgui.h"

#include "editor_material_edit.h"
#include "editor_reference_pickers.h"
#include "engine/renderer/asset_metadata.h"
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
      "Albedo Texture", renderer::AssetTypeTag::Texture, &slots.albedo);
  changed |= draw_asset_reference_picker("Metallic/Roughness Texture",
                                         renderer::AssetTypeTag::Texture,
                                         &slots.metallicRoughness);
  changed |= draw_asset_reference_picker(
      "Emissive Texture", renderer::AssetTypeTag::Texture, &slots.emissive);
  changed |= draw_asset_reference_picker(
      "Occlusion Texture", renderer::AssetTypeTag::Texture, &slots.occlusion);
  changed |= draw_asset_reference_picker(
      "Opacity Texture", renderer::AssetTypeTag::Texture, &slots.opacity);
  return changed;
}

} // namespace

void draw_material_editor_panel() noexcept {
  MaterialEditorState &state = material_editor_state();
  if (!state.open) {
    return;
  }

  bool stillOpen = true;
  ImGui::SetNextWindowSize(ImVec2(420.0F, 520.0F), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Material Editor", &stillOpen)) {
    ImGui::End();
    if (!stillOpen) {
      close_material_editor();
    }
    return;
  }

  if (!state.found) {
    ImGui::TextColored(ImVec4(0.9F, 0.4F, 0.3F, 1.0F),
                       "Failed to load material: %s", state.virtualPath);
    ImGui::End();
    if (!stillOpen) {
      close_material_editor();
    }
    return;
  }

  ImGui::TextDisabled("%s", state.virtualPath);
  if (state.hasParent) {
    ImGui::TextDisabled("Parent: %s", state.parentVirtualPath);
  }
  if (state.dirty) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9F, 0.75F, 0.2F, 1.0F), "(unsaved)");
  }
  ImGui::TextDisabled(
      "Edits apply live to the viewport immediately; Save writes to disk.");
  ImGui::Separator();

  // The gesture's "before" snapshot: taken before any widget below can
  // mutate the buffer, so material_editor_apply_frame always records the
  // true pre-edit value on the first changed frame of a drag/interaction.
  const renderer::Material beforeFrameParams = state.buffer;
  const renderer::MaterialTextureSlots beforeFrameSlots = state.textureSlots;

  bool changed = false;
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

  const bool anyItemActive = ImGui::IsAnyItemActive();
  ImGui::End();

  material_editor_apply_frame(beforeFrameParams, beforeFrameSlots, changed,
                              anyItemActive);

  if (!stillOpen) {
    close_material_editor();
  }
}

} // namespace engine::editor
