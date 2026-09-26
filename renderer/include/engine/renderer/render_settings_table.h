// Declares the renderer settings an author adjusts from the editor: each
// entry names the cvar holding the setting, how to present it (a toggle, a
// slider over a range, or a choice of named values) and what it does. The
// renderer owns the list because it owns the cvars; the editor's Rendering
// panel draws it and adds nothing of its own.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

/// How a setting is presented and which cvar type backs it.
enum class RenderSettingKind : std::uint8_t {
  Toggle, ///< a bool cvar
  Slider, ///< a float cvar within [minValue, maxValue]
  Choice, ///< a string cvar holding one of `choices`
};

/// One author-facing renderer setting.
struct RenderSettingEntry final {
  const char *section = "";
  const char *label = "";
  const char *cvar = "";
  RenderSettingKind kind = RenderSettingKind::Toggle;
  float minValue = 0.0F;
  float maxValue = 1.0F;
  const char *const *choices = nullptr;
  std::size_t choiceCount = 0U;
  const char *tooltip = "";
};

/// The settings in presentation order, grouped by section; `count`
/// receives their number.
const RenderSettingEntry *render_setting_entries(std::size_t *count) noexcept;

} // namespace engine::renderer
