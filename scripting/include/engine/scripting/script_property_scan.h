// Declares the static scan that reads a script's property declarations
// from its text, without running Lua, so the editor can show a script's
// properties as typed fields before anything plays. Like Defold's editor,
// which reads go.property declarations from the source, it accepts
// literals only and reports anything else as a diagnostic with its line
// rather than leaving a property out silently.
//
// A script declares its properties in one table literal at its top level,
// either assigned to a field of the module table or as a field of a
// top-level table constructor:
//
//   M.properties = {
//       speed = 0.25,                                 -- float
//       lives = 3,                                    -- integer
//       enabled = true,                               -- bool
//       target = "Player",                            -- string
//       amplitude = { type = "float", default = 2.0, min = 0.0, max = 10.0,
//                     tooltip = "How far the platform travels" },
//   }
//
// A plain literal declares the property with that default and its type:
// a number with a '.' or an exponent is a float, any other number an
// integer, as Lua 5.4 reads them. The table form names the type and adds a
// range and a tooltip; an integer default for a float property is allowed.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/math/script_behaviours.h"

namespace engine::scripting {

/// One declared property: its name, default and editor metadata.
struct ScriptPropertyDecl final {
  static constexpr std::size_t kMaxTooltipLength = 127U;
  char name[math::kMaxScriptPropertyNameLength + 1U] = {};
  math::ScriptPropertyValue defaultValue{};
  bool hasMin = false;
  bool hasMax = false;
  double minValue = 0.0;
  double maxValue = 0.0;
  char tooltip[kMaxTooltipLength + 1U] = {};
  /// The 1-based line of the declaration.
  std::uint32_t line = 0U;
};

/// Something the scan could not read, with the 1-based line it is on.
struct ScriptPropertyDiagnostic final {
  static constexpr std::size_t kMaxMessageLength = 159U;
  std::uint32_t line = 0U;
  char message[kMaxMessageLength + 1U] = {};
};

/// Everything one scan found: the declared properties, in declaration
/// order, and what it could not read.
struct ScriptPropertySchema final {
  static constexpr std::size_t kMaxProperties =
      math::ScriptPropertiesComponent::kMaxOverrides;
  static constexpr std::size_t kMaxDiagnostics = 16U;
  /// True when the script declares a properties table at all.
  bool declared = false;
  std::size_t count = 0U;
  ScriptPropertyDecl properties[kMaxProperties] = {};
  std::size_t diagnosticCount = 0U;
  ScriptPropertyDiagnostic diagnostics[kMaxDiagnostics] = {};
  /// Diagnostics past kMaxDiagnostics, counted but not kept.
  std::size_t droppedDiagnostics = 0U;
};

/// Scans `length` bytes of Lua source for its property declarations into
/// `out`, which is reset first. Never runs the script and never fails: what
/// it cannot read becomes a diagnostic.
void scan_script_properties(const char *source, std::size_t length,
                            ScriptPropertySchema *out) noexcept;

/// The declaration named `name`, or nullptr.
[[nodiscard]] const ScriptPropertyDecl *
find_script_property(const ScriptPropertySchema &schema,
                     const char *name) noexcept;

/// The name a property type is declared by ("bool", "integer", "float",
/// "string").
[[nodiscard]] const char *
script_property_type_name(math::ScriptPropertyType type) noexcept;

} // namespace engine::scripting
