// Declares an entity's script behaviours and the per-instance values of
// their properties. An entity lists up to kMaxScriptBehaviours Lua
// behaviours, each a script file it runs and an enabled flag, as a Unity
// GameObject carries several MonoBehaviours. A script declares its
// properties and their defaults in its own text (the scripting layer scans
// them); an entity stores only the values that differ from those defaults,
// per behaviour, as Godot stores a node's changed properties and Unity a
// prefab instance's modifications, so a default changed in the script
// reaches every instance that did not override it.

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

/// Most behaviours one entity lists.
inline constexpr std::size_t kMaxScriptBehaviours = 8U;

/// One behaviour an entity runs: a Lua script returning a module table
/// with optional on_begin_play(self), on_tick(self, dt),
/// on_fixed_tick(self, dt) and on_end_play(self) functions. A disabled
/// behaviour is neither begun nor ticked; one disabled after it began still
/// ends play.
struct ScriptBehaviour final {
  static constexpr std::size_t kMaxPathLength = 127U; // +1 for null terminator
  char scriptPath[kMaxPathLength + 1U] = {};
  bool enabled = true;
};

/// The behaviours an entity runs, in the order they are called. The list
/// is compact: the used entries come first, each with a non-empty path,
/// and the first empty path ends it. A script appears at most once, so
/// its path names the behaviour. Many entities may share one script file.
struct ScriptComponent final {
  static constexpr std::size_t kMaxPathLength = ScriptBehaviour::kMaxPathLength;
  ScriptBehaviour behaviours[kMaxScriptBehaviours] = {};
};

/// The number of behaviours `component` lists.
[[nodiscard]] inline std::size_t
script_behaviour_count(const ScriptComponent &component) noexcept {
  std::size_t count = 0U;
  while ((count < kMaxScriptBehaviours) &&
         (component.behaviours[count].scriptPath[0] != '\0')) {
    ++count;
  }
  return count;
}

/// The index of the behaviour running `path`, or kMaxScriptBehaviours.
[[nodiscard]] inline std::size_t
find_script_behaviour(const ScriptComponent &component,
                      const char *path) noexcept {
  if ((path == nullptr) || (path[0] == '\0')) {
    return kMaxScriptBehaviours;
  }
  const std::size_t count = script_behaviour_count(component);
  for (std::size_t i = 0U; i < count; ++i) {
    if (std::strcmp(component.behaviours[i].scriptPath, path) == 0) {
      return i;
    }
  }
  return kMaxScriptBehaviours;
}

/// True when every path is terminated, the list is compact, no script
/// repeats, and every entry past the list is empty and enabled: the only
/// lists a World stores.
[[nodiscard]] inline bool
script_component_is_valid(const ScriptComponent &component) noexcept {
  const std::size_t count = script_behaviour_count(component);
  for (std::size_t i = 0U; i < kMaxScriptBehaviours; ++i) {
    const ScriptBehaviour &entry = component.behaviours[i];
    if (std::memchr(entry.scriptPath, '\0', sizeof(entry.scriptPath)) ==
        nullptr) {
      return false;
    }
    if (i >= count) {
      if ((entry.scriptPath[0] != '\0') || !entry.enabled) {
        return false;
      }
      continue;
    }
    for (std::size_t j = 0U; j < i; ++j) {
      if (std::strcmp(component.behaviours[j].scriptPath, entry.scriptPath) ==
          0) {
        return false;
      }
    }
  }
  return true;
}

/// An entity's overrides of its behaviours' property defaults: at most
/// kMaxOverrides values, each naming the behaviour (its index in the
/// entity's ScriptComponent) and the property it sets, in the order they
/// were set. A behaviour/name pair appears once. A property with no
/// override takes the script's default.
struct ScriptPropertiesComponent final {
  static constexpr std::size_t kMaxOverrides = 16U;
  /// One property's per-instance value.
  struct Override final {
    std::uint8_t behaviour = 0U;
    char name[kMaxScriptPropertyNameLength + 1U] = {};
    ScriptPropertyValue value{};
  };
  std::uint32_t count = 0U;
  Override overrides[kMaxOverrides] = {};
};

/// The override of behaviour `behaviour`'s property `name`, or nullptr.
[[nodiscard]] inline const ScriptPropertiesComponent::Override *
script_property_override(const ScriptPropertiesComponent &component,
                         std::size_t behaviour, const char *name) noexcept {
  if (name == nullptr) {
    return nullptr;
  }
  const std::size_t count =
      (component.count < ScriptPropertiesComponent::kMaxOverrides)
          ? component.count
          : ScriptPropertiesComponent::kMaxOverrides;
  for (std::size_t i = 0U; i < count; ++i) {
    if ((component.overrides[i].behaviour == behaviour) &&
        (std::strcmp(component.overrides[i].name, name) == 0)) {
      return &component.overrides[i];
    }
  }
  return nullptr;
}

/// True when every override names a behaviour index below
/// kMaxScriptBehaviours and has a valid name and value, the count fits and
/// no behaviour/name pair repeats: the only override sets a World stores.
[[nodiscard]] inline bool script_properties_are_valid(
    const ScriptPropertiesComponent &component) noexcept {
  if (component.count > ScriptPropertiesComponent::kMaxOverrides) {
    return false;
  }
  for (std::size_t i = 0U; i < component.count; ++i) {
    const ScriptPropertiesComponent::Override &entry = component.overrides[i];
    if ((entry.behaviour >= kMaxScriptBehaviours) ||
        (std::memchr(entry.name, '\0', sizeof(entry.name)) == nullptr) ||
        !script_property_name_is_valid(entry.name) ||
        !script_property_value_is_valid(entry.value)) {
      return false;
    }
    for (std::size_t j = 0U; j < i; ++j) {
      if ((component.overrides[j].behaviour == entry.behaviour) &&
          (std::strcmp(component.overrides[j].name, entry.name) == 0)) {
        return false;
      }
    }
  }
  return true;
}

/// Sets behaviour `behaviour`'s override of `name` to `value`, replacing
/// one already there or appending a new one. False, with the component
/// unchanged, for an invalid behaviour index, name or value, or when a new
/// one would pass kMaxOverrides.
inline bool script_properties_set(ScriptPropertiesComponent *component,
                                  std::size_t behaviour, const char *name,
                                  const ScriptPropertyValue &value) noexcept {
  if ((component == nullptr) || (behaviour >= kMaxScriptBehaviours) ||
      !script_property_name_is_valid(name) ||
      !script_property_value_is_valid(value) ||
      (component->count > ScriptPropertiesComponent::kMaxOverrides)) {
    return false;
  }
  for (std::size_t i = 0U; i < component->count; ++i) {
    if ((component->overrides[i].behaviour == behaviour) &&
        (std::strcmp(component->overrides[i].name, name) == 0)) {
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
  slot.behaviour = static_cast<std::uint8_t>(behaviour);
  std::memcpy(slot.name, name, std::strlen(name));
  slot.value = value;
  ++component->count;
  return true;
}

/// Removes override `index`, keeping the others in order.
inline void script_properties_erase(ScriptPropertiesComponent *component,
                                    std::size_t index) noexcept {
  for (std::size_t j = index + 1U; j < component->count; ++j) {
    component->overrides[j - 1U] = component->overrides[j];
  }
  --component->count;
  component->overrides[component->count] =
      ScriptPropertiesComponent::Override{};
}

/// Removes behaviour `behaviour`'s override of `name`, keeping the others
/// in order; false when there is none.
inline bool script_properties_clear(ScriptPropertiesComponent *component,
                                    std::size_t behaviour,
                                    const char *name) noexcept {
  if ((component == nullptr) || (name == nullptr) ||
      (component->count > ScriptPropertiesComponent::kMaxOverrides)) {
    return false;
  }
  for (std::size_t i = 0U; i < component->count; ++i) {
    if ((component->overrides[i].behaviour == behaviour) &&
        (std::strcmp(component->overrides[i].name, name) == 0)) {
      script_properties_erase(component, i);
      return true;
    }
  }
  return false;
}

/// Follows the removal of behaviour `behaviour` from the entity's list:
/// drops its overrides and renumbers those of the behaviours after it, so
/// each override keeps naming the script it was set for.
inline void
script_properties_remove_behaviour(ScriptPropertiesComponent *component,
                                   std::size_t behaviour) noexcept {
  if ((component == nullptr) ||
      (component->count > ScriptPropertiesComponent::kMaxOverrides)) {
    return;
  }
  std::size_t i = 0U;
  while (i < component->count) {
    ScriptPropertiesComponent::Override &entry = component->overrides[i];
    if (entry.behaviour == behaviour) {
      script_properties_erase(component, i);
      continue;
    }
    if (entry.behaviour > behaviour) {
      --entry.behaviour;
    }
    ++i;
  }
}

/// Follows behaviours `a` and `b` trading places in the entity's list, so
/// each override keeps naming the script it was set for.
inline void
script_properties_swap_behaviours(ScriptPropertiesComponent *component,
                                  std::size_t a, std::size_t b) noexcept {
  if ((component == nullptr) ||
      (component->count > ScriptPropertiesComponent::kMaxOverrides)) {
    return;
  }
  for (std::size_t i = 0U; i < component->count; ++i) {
    std::uint8_t &behaviour = component->overrides[i].behaviour;
    if (behaviour == a) {
      behaviour = static_cast<std::uint8_t>(b);
    } else if (behaviour == b) {
      behaviour = static_cast<std::uint8_t>(a);
    }
  }
}

/// Appends an enabled behaviour running `path`. False, with the list
/// unchanged, for an empty or over-long path, a script the list already
/// holds, or a full list.
inline bool script_behaviour_append(ScriptComponent *component,
                                    const char *path) noexcept {
  if ((component == nullptr) || (path == nullptr) || (path[0] == '\0')) {
    return false;
  }
  const std::size_t length = std::strlen(path);
  const std::size_t count = script_behaviour_count(*component);
  if ((length > ScriptBehaviour::kMaxPathLength) ||
      (count >= kMaxScriptBehaviours) ||
      (find_script_behaviour(*component, path) < kMaxScriptBehaviours)) {
    return false;
  }
  ScriptBehaviour &added = component->behaviours[count];
  added = ScriptBehaviour{};
  std::memcpy(added.scriptPath, path, length);
  return true;
}

/// Removes behaviour `index`, closing the gap, and drops its overrides from
/// `properties` (which may be null) while renumbering the rest. False when
/// there is no such behaviour.
inline bool script_behaviour_remove(ScriptComponent *component,
                                    ScriptPropertiesComponent *properties,
                                    std::size_t index) noexcept {
  if (component == nullptr) {
    return false;
  }
  const std::size_t count = script_behaviour_count(*component);
  if (index >= count) {
    return false;
  }
  for (std::size_t i = index + 1U; i < count; ++i) {
    component->behaviours[i - 1U] = component->behaviours[i];
  }
  component->behaviours[count - 1U] = ScriptBehaviour{};
  script_properties_remove_behaviour(properties, index);
  return true;
}

/// Swaps behaviours `a` and `b`, with their overrides in `properties`
/// (which may be null). False unless both are in the list.
inline bool script_behaviour_swap(ScriptComponent *component,
                                  ScriptPropertiesComponent *properties,
                                  std::size_t a, std::size_t b) noexcept {
  if (component == nullptr) {
    return false;
  }
  const std::size_t count = script_behaviour_count(*component);
  if ((a >= count) || (b >= count)) {
    return false;
  }
  const ScriptBehaviour held = component->behaviours[a];
  component->behaviours[a] = component->behaviours[b];
  component->behaviours[b] = held;
  script_properties_swap_behaviours(properties, a, b);
  return true;
}

} // namespace engine::math
