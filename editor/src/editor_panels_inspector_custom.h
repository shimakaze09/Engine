// Declares the Inspector's per-component custom drawers: the
// components whose authoring-relevant fields are not representable by the
// generic reflected-field loop (64-bit asset ids, VFS paths, fixed arrays)
// keep a dedicated, typed drawer instead of forcing those fields through
// core::TypeField. Also declares the registry-generated Add Component menu.

#pragma once

#include "editor_component_registry.h"
#include "engine/runtime/world.h"

namespace engine::editor {

/// Draws MeshComponent's asset-id pickers (mesh, material), PBR sliders,
/// scene-capture-source entity picker, and material presets; returns true
/// if any field changed (a preset application applies directly through the
/// command history and reports unmodified so the caller does not double
/// record it).
bool draw_mesh_component_fields(runtime::Entity entity,
                                runtime::MeshComponent &mesh,
                                bool editable) noexcept;

/// Draws SkyLightComponent's environment picker, listing Environment
/// (.hdr) assets; returns true if the environment changed.
bool draw_sky_light_component_fields(runtime::SkyLightComponent &skyLight,
                                     bool editable) noexcept;

/// Draws LightComponent's Type combo ahead of its generic reflected fields
/// (color/direction/intensity); returns true if the type changed.
bool draw_light_type_combo(runtime::LightComponent &light) noexcept;

/// Draws ScriptComponent's script-path field as a searchable path picker.
bool draw_script_component_fields(runtime::ScriptComponent &script) noexcept;

/// Draws AnimationComponent's controller-path picker plus playback fields.
bool draw_animation_component_fields(
    runtime::AnimationComponent &animation) noexcept;

/// Draws FoliagePatchComponent's full custom editor: LOD mesh pickers,
/// density/material sliders, and the per-instance array editor. Structural
/// edits (add/remove instance) route directly through the command history
/// (matching the pre-existing gesture contract) and suppress the caller's
/// direct-apply for that frame via *modified = false.
void draw_foliage_patch_fields(runtime::Entity entity,
                               runtime::FoliagePatchComponent &foliage,
                               bool editable, bool *modified) noexcept;

/// Draws SceneCaptureComponent's live preview image beneath its generic
/// fields.
void draw_scene_capture_preview(
    runtime::Entity entity,
    const runtime::SceneCaptureComponent &capture) noexcept;

/// Draws the searchable, categorized Add Component menu, generated from the
/// persistent-component registry plus ComponentMetadata display/category
/// hints -- a new registry row appears here automatically.
void draw_add_component_menu(runtime::Entity entity, bool editable) noexcept;

/// Writes the component types the Add Component menu offers for `entity`
/// into `out`, in registry order, and returns how many: every registry
/// type the entity does not carry, less those whose Inspector metadata
/// marks them not offered.
std::size_t add_component_menu_candidates(runtime::Entity entity,
                                          ComponentEditType *out,
                                          std::size_t capacity) noexcept;

} // namespace engine::editor
