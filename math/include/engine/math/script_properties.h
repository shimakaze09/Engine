// Declares the values a script's properties hold and the component that
// carries an entity's per-instance overrides of them. A script declares
// its properties and their defaults in its own text (the scripting layer
// scans them); an entity stores only the values that differ from those
// defaults, as Godot stores a node's changed properties and Unity a prefab
// instance's modifications, so a default changed in the script reaches
// every instance that did not override it.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace engine::math {

/// The kinds of value a script property holds.
enum class ScriptPropertyType : std::uint8_t {
  Bool = 0,
  Integer = 1,
  Float = 2,
  String = 3,
};

/// Number of ScriptPropertyType values; a stored type at or past it is one
/// this build does not know and is refused.
inline constexpr std::uint32_t kScriptPropertyTypeCount = 4U;

/// One property value. Only the member its type names is meaningful; the
/// others stay zero.
struct ScriptPropertyValue final {
  /// Longest string value; a longer one is refused, never cut.
  static constexpr std::size_t kMaxTextLength = 47U;
  ScriptPropertyType type = ScriptPropertyType::Float;
  bool boolValue = false;
  std::int64_t integerValue = 0;
  float floatValue = 0.0F;
  char text[kMaxTextLength + 1U] = {};
};

/// Longest property name: a Lua identifier of at most this many bytes.
inline constexpr std::size_t kMaxScriptPropertyNameLength = 31U;

/// True when `name` is a Lua identifier ([A-Za-z_][A-Za-z0-9_]*) of 1 to
/// kMaxScriptPropertyNameLength bytes.
[[nodiscard]] inline bool
script_property_name_is_valid(const char *name) noexcept {
  if ((name == nullptr) || (name[0] == '\0') ||
      ((name[0] >= '0') && (name[0] <= '9'))) {
    return false;
  }
  std::size_t length = 0U;
  for (; name[length] != '\0'; ++length) {
    const char c = name[length];
    const bool letter =
        ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'));
    const bool digit = (c >= '0') && (c <= '9');
    if ((!letter && !digit && (c != '_')) ||
        (length >= kMaxScriptPropertyNameLength)) {
      return false;
    }
  }
  return true;
}

/// True when `value` holds a known type, a finite float, a terminated
/// string, and zero in every member its type does not use.
[[nodiscard]] inline bool
script_property_value_is_valid(const ScriptPropertyValue &value) noexcept {
  if (static_cast<std::uint32_t>(value.type) >= kScriptPropertyTypeCount) {
    return false;
  }
  if ((std::memchr(value.text, '\0', sizeof(value.text)) == nullptr) ||
      !std::isfinite(value.floatValue)) {
    return false;
  }
  const bool usesText = value.type == ScriptPropertyType::String;
  return ((value.type == ScriptPropertyType::Bool) || !value.boolValue) &&
         ((value.type == ScriptPropertyType::Integer) ||
          (value.integerValue == 0)) &&
         ((value.type == ScriptPropertyType::Float) ||
          (value.floatValue == 0.0F)) &&
         (usesText || (value.text[0] == '\0'));
}

/// True when two values have the same type and the same value.
[[nodiscard]] inline bool
script_property_values_equal(const ScriptPropertyValue &a,
                             const ScriptPropertyValue &b) noexcept {
  if (a.type != b.type) {
    return false;
  }
  switch (a.type) {
  case ScriptPropertyType::Bool:
    return a.boolValue == b.boolValue;
  case ScriptPropertyType::Integer:
    return a.integerValue == b.integerValue;
  case ScriptPropertyType::Float:
    return a.floatValue == b.floatValue;
  case ScriptPropertyType::String:
    return std::strcmp(a.text, b.text) == 0;
  }
  return false;
}

/// An entity's overrides of its script's property defaults: at most
/// kMaxOverrides named values, names distinct, in the order they were set.
/// A property with no override takes the script's default.
struct ScriptPropertiesComponent final {
  static constexpr std::size_t kMaxOverrides = 16U;
  /// One property's per-instance value.
  struct Override final {
    char name[kMaxScriptPropertyNameLength + 1U] = {};
    ScriptPropertyValue value{};
  };
  std::uint32_t count = 0U;
  Override overrides[kMaxOverrides] = {};
};

/// The override named `name`, or nullptr.
[[nodiscard]] inline const ScriptPropertiesComponent::Override *
script_property_override(const ScriptPropertiesComponent &component,
                         const char *name) noexcept {
  if (name == nullptr) {
    return nullptr;
  }
  const std::size_t count =
      (component.count < ScriptPropertiesComponent::kMaxOverrides)
          ? component.count
          : ScriptPropertiesComponent::kMaxOverrides;
  for (std::size_t i = 0U; i < count; ++i) {
    if (std::strcmp(component.overrides[i].name, name) == 0) {
      return &component.overrides[i];
    }
  }
  return nullptr;
}

/// True when every override has a valid name and value, the count fits and
/// no name repeats: the only override sets a World stores.
[[nodiscard]] inline bool script_properties_are_valid(
    const ScriptPropertiesComponent &component) noexcept {
  if (component.count > ScriptPropertiesComponent::kMaxOverrides) {
    return false;
  }
  for (std::size_t i = 0U; i < component.count; ++i) {
    const ScriptPropertiesComponent::Override &entry = component.overrides[i];
    if ((std::memchr(entry.name, '\0', sizeof(entry.name)) == nullptr) ||
        !script_property_name_is_valid(entry.name) ||
        !script_property_value_is_valid(entry.value)) {
      return false;
    }
    for (std::size_t j = 0U; j < i; ++j) {
      if (std::strcmp(component.overrides[j].name, entry.name) == 0) {
        return false;
      }
    }
  }
  return true;
}

/// Sets the override named `name` to `value`, replacing one already there
/// or appending a new one. False, with the component unchanged, for an
/// invalid name or value or when a new one would pass kMaxOverrides.
inline bool script_properties_set(ScriptPropertiesComponent *component,
                                  const char *name,
                                  const ScriptPropertyValue &value) noexcept {
  if ((component == nullptr) || !script_property_name_is_valid(name) ||
      !script_property_value_is_valid(value) ||
      (component->count > ScriptPropertiesComponent::kMaxOverrides)) {
    return false;
  }
  for (std::size_t i = 0U; i < component->count; ++i) {
    if (std::strcmp(component->overrides[i].name, name) == 0) {
      component->overrides[i].value = value;
      return true;
    }
  }
  if (component->count == ScriptPropertiesComponent::kMaxOverrides) {
    return false;
  }
  ScriptPropertiesComponent::Override &slot =
      component->overrides[component->count];
  slot = ScriptPropertiesComponent::Override{};
  std::memcpy(slot.name, name, std::strlen(name));
  slot.value = value;
  ++component->count;
  return true;
}

/// Removes the override named `name`, keeping the others in order; false
/// when there is none.
inline bool script_properties_clear(ScriptPropertiesComponent *component,
                                    const char *name) noexcept {
  if ((component == nullptr) || (name == nullptr)) {
    return false;
  }
  for (std::size_t i = 0U; i < component->count; ++i) {
    if (std::strcmp(component->overrides[i].name, name) == 0) {
      for (std::size_t j = i + 1U; j < component->count; ++j) {
        component->overrides[j - 1U] = component->overrides[j];
      }
      --component->count;
      component->overrides[component->count] =
          ScriptPropertiesComponent::Override{};
      return true;
    }
  }
  return false;
}

} // namespace engine::math
