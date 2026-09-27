// Implements the table of author-facing renderer settings the editor's
// Rendering panel draws.

#include "engine/renderer/render_settings_table.h"

namespace engine::renderer {

namespace {

constexpr const char *kFogModes[] = {"off", "linear", "exp", "exp2"};

constexpr RenderSettingEntry kEntries[] = {
    {"Fog", "Height fog", "r_height_fog", RenderSettingKind::Toggle, 0.0F, 1.0F,
     nullptr, 0U,
     "Ground-hugging fog that thins with height. Off by default: at ground "
     "level it greys every material."},
    {"Fog", "Height fog density", "r_height_fog_density",
     RenderSettingKind::Slider, 0.0F, 0.2F, nullptr, 0U,
     "Fog density at the base height."},
    {"Fog", "Height fog base", "r_height_fog_base", RenderSettingKind::Slider,
     -50.0F, 50.0F, nullptr, 0U,
     "World height where the height fog is densest."},
    {"Fog", "Height fog falloff", "r_height_fog_falloff",
     RenderSettingKind::Slider, 0.0F, 1.0F, nullptr, 0U,
     "How fast the height fog thins above its base."},
    {"Fog", "Distance fog", "r_fog_mode", RenderSettingKind::Choice, 0.0F, 1.0F,
     kFogModes, sizeof(kFogModes) / sizeof(kFogModes[0]),
     "Fog that thickens with distance from the camera."},
    {"Fog", "Distance fog density", "r_fog_density", RenderSettingKind::Slider,
     0.0F, 0.1F, nullptr, 0U, "Density of the exponential distance fog modes."},
    {"Exposure", "Auto exposure", "r_auto_exposure", RenderSettingKind::Toggle,
     0.0F, 1.0F, nullptr, 0U,
     "Adapt exposure to the scene's average brightness."},
    {"Exposure", "Exposure", "r_exposure", RenderSettingKind::Slider, 0.1F,
     8.0F, nullptr, 0U,
     "The exposure; with auto exposure on, the compensation applied on top "
     "of the adapted one."},
    {"Post", "Bloom", "r_bloom", RenderSettingKind::Toggle, 0.0F, 1.0F, nullptr,
     0U, "Glow around bright areas."},
    {"Post", "Ambient occlusion", "r_ssao", RenderSettingKind::Toggle, 0.0F,
     1.0F, nullptr, 0U, "Screen-space darkening of contact creases."},
    {"Post", "Anti-aliasing", "r_fxaa", RenderSettingKind::Toggle, 0.0F, 1.0F,
     nullptr, 0U, "FXAA edge smoothing."},
    {"Lighting", "Shadows", "r_shadows", RenderSettingKind::Toggle, 0.0F, 1.0F,
     nullptr, 0U, "Cascaded shadows from the directional light."},
};

} // namespace

const RenderSettingEntry *render_setting_entries(std::size_t *count) noexcept {
  if (count != nullptr) {
    *count = sizeof(kEntries) / sizeof(kEntries[0]);
  }
  return kEntries;
}

} // namespace engine::renderer
